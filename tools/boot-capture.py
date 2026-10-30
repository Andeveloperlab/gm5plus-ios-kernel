#!/usr/bin/env python3
"""
Bare-metal boot + serial capture for HackiOS on GM 5 Plus (shamrock / MSM8952).

Two hard-won rules are built into this tool:

 1. The shim's CDC-ACM port and the in-kernel XNU CDC-ACM port are the same
    USB product (1209:0001).  "A /dev/ttyACM* exists" proves nothing: the
    shim's node stays around for a moment, the kernel resets the controller
    (D+ drop) and the node we hold reports hang-up (read() == 0).  We therefore
    keep reading, leak the dead fd (closing a hung-up cdc_acm blocks in the
    kernel) and re-open the node when it comes back.

 2. The kernel only transmits on EP1 IN once the host has configured it, and
    it replays its persistent RAM log when the host asserts DTR
    (SET_CONTROL_LINE_STATE).  A plain os.open() never asserts DTR, which is
    why an earlier capture produced 0 bytes.  We assert DTR/RTS explicitly.

Before booting XNU the shim's own `log` command is used to dump the persistent
kernel log of the *previous* run (0x831F4000, 'SHMR'), which is the only way to
see a run that panicked before its USB console was usable.

Usage:
  tools/boot-capture.py                 # full chain lk2nd -> shim -> xnu
  tools/boot-capture.py --skip-lk2nd    # device already in lk2nd fastboot
  tools/boot-capture.py --no-boot       # only dump the previous run's log
"""
import argparse
import glob
import os
import struct
import subprocess
import sys
import termios
import time
import fcntl

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LK2ND_IMG = os.path.join(REPO_DIR, "tools/lk2nd-msm8952.img")
SHIM_BOOT_IMG = os.path.join(REPO_DIR, "out/shamrock-shim-boot.img")

TIOCMBIS = 0x5416
TIOCMBIC = 0x5417
TIOCM_DTR = 0x002
TIOCM_RTS = 0x004


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def find_port():
    ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    return ports[0] if ports else None


def fastboot_devices():
    try:
        r = subprocess.run(["fastboot", "devices"], capture_output=True, text=True, timeout=10)
        return r.stdout.strip()
    except Exception:
        return ""


def adb_devices():
    try:
        r = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=5)
        for line in r.stdout.strip().splitlines()[1:]:
            parts = line.split()
            if len(parts) >= 2 and parts[1] == "device":
                return parts[0]
        return ""
    except Exception:
        return ""


def wait_fastboot(timeout):
    t0 = time.time()
    last_adb = 0
    while time.time() - t0 < timeout:
        d = fastboot_devices()
        if d:
            log(f"fastboot: {d}")
            return True
        if time.time() - last_adb > 3.0:
            last_adb = time.time()
            adb_dev = adb_devices()
            if adb_dev:
                log(f"phone found in Android ({adb_dev}): sending 'adb reboot bootloader'...")
                try:
                    subprocess.run(["adb", "reboot", "bootloader"], capture_output=True, timeout=5)
                except Exception:
                    pass
        time.sleep(0.3)
    return False


def open_port(path):
    """Raw 115200 8N1, DTR+RTS asserted (the kernel replays its log on DTR)."""
    fd = -1
    try:
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        time.sleep(0.05)
        a = termios.tcgetattr(fd)
        a[0] = 0
        a[1] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[3] = 0
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, a)
        fcntl.ioctl(fd, TIOCMBIS, struct.pack("i", TIOCM_DTR | TIOCM_RTS))
        return fd
    except Exception as e:
        if fd >= 0:
            try:
                os.close(fd)
            except Exception:
                pass
        return None


def kick(fd):
    """A fresh DTR edge.

    The kernel's console arms itself on the line-state change and replays its log
    once; if the node was opened a moment too early that edge is lost and the port
    stays silent forever. Toggling it again is cheap and harmless.
    """
    try:
        fcntl.ioctl(fd, TIOCMBIC, struct.pack("i", TIOCM_DTR | TIOCM_RTS))
        time.sleep(0.2)
        fcntl.ioctl(fd, TIOCMBIS, struct.pack("i", TIOCM_DTR | TIOCM_RTS))
        os.write(fd, b"\r\n")
    except OSError as e:
        log(f"kick failed: {e}")


def wait_port(timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        p = find_port()
        if p:
            fd = open_port(p)
            if fd is not None:
                return p, fd
        time.sleep(0.1)
    return None, None


def pump(fd, sink, echo, deadline, idle_limit):
    """Read until `deadline` or `idle_limit` seconds without data.

    Returns True when the node hung up (kernel took the controller over).
    """
    last = time.time()
    while time.time() < deadline:
        try:
            data = os.read(fd, 4096)
        except BlockingIOError:
            if time.time() - last > idle_limit:
                return False
            time.sleep(0.01)
            continue
        except OSError as e:
            log(f"read error: {e}")
            return True
        if not data:
            return True
        last = time.time()
        sink.write(data)
        sink.flush()
        if echo:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-lk2nd", action="store_true")
    ap.add_argument("--no-boot", action="store_true", help="dump the previous run's log only")
    ap.add_argument("--wait", type=int, default=120, help="seconds to wait for fastboot")
    ap.add_argument("--seconds", type=int, default=800, help="capture window after 'boot'")
    ap.add_argument("--idle", type=int, default=240)
    ap.add_argument("--log", default=os.path.join(REPO_DIR, "out/xnu-live-boot.log"))
    ap.add_argument("--prev", default=os.path.join(REPO_DIR, "out/xnu-prev-run-log.txt"))
    args = ap.parse_args()

    if not args.skip_lk2nd:
        log("waiting for the phone in fastboot ...")
        if not wait_fastboot(args.wait):
            log("no fastboot device")
            return 1
        log(f"booting stage 1 (lk2nd): {LK2ND_IMG}")
        r = subprocess.run(["fastboot", "boot", LK2ND_IMG], capture_output=True, text=True)
        print(r.stdout, r.stderr)
        if r.returncode != 0:
            return 1
        time.sleep(3)

    log("waiting for lk2nd fastboot ...")
    if not wait_fastboot(20):
        log("lk2nd fastboot did not appear")
        return 1

    log(f"booting stage 2 (shim): {SHIM_BOOT_IMG}")
    r = subprocess.run(["fastboot", "boot", SHIM_BOOT_IMG], capture_output=True, text=True)
    print(r.stdout, r.stderr)
    if r.returncode != 0:
        return 1

    port, fd = wait_port(25)
    if fd is None:
        log("shim console never appeared")
        return 1
    log(f"shim console: {port}")

    # 1) the shim's own log command: persistent kernel RAM log of the last run
    with open(args.prev, "wb", buffering=0) as prev:
        os.write(fd, b"\r\nlog\r\n")
        pump(fd, prev, True, time.time() + 12, 3)
    log(f"previous run's log -> {args.prev} ({os.path.getsize(args.prev)} bytes)")

    if args.no_boot:
        return 0

    # 2) hand over to XNU and follow the console through the hand-off
    os.write(fd, b"\r\nboot\r\n")
    log("sent 'boot'; following the console")

    with open(args.log, "wb", buffering=0) as out:
        deadline = time.time() + args.seconds
        total = 0
        while time.time() < deadline:
            hung = pump(fd, out, True, deadline, args.idle)
            total = os.path.getsize(args.log)
            if not hung:
                break
            if total == 0:
                log("port hung up before any data")
            log(f"node hung up after {total} bytes; waiting for the kernel's node")
            fd = None
            time.sleep(0.05)
            port2, fd = wait_port(80)
            if fd is None:
                log("no node came back")
                adb_dev = adb_devices()
                if adb_dev:
                    log(f"device rebooted into Android ({adb_dev})!")
                break
            log(f"kernel console: {port2} (DTR asserted)")
            prev_total = os.path.getsize(args.log)
            # Give the console a few fresh line-state edges until it answers.
            for _ in range(10):
                if os.path.getsize(args.log) > prev_total:
                    break
                kick(fd)
                time.sleep(0.5)
                try:
                    data = os.read(fd, 4096)
                except (BlockingIOError, OSError):
                    data = b""
                if data:
                    out.write(data)
                    out.flush()
                    sys.stdout.write(data.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                    break
            time.sleep(0.05)
        log(f"captured {os.path.getsize(args.log)} bytes -> {args.log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
