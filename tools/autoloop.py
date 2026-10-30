#!/usr/bin/env python3
"""One unattended iteration: boot what out/ holds, capture it, and talk the board back to fastboot.

The phone cannot be reached through anything but its serial console while XNU runs, and XNU's
halt path is a wfe spin -- so the userland library carries the way out: with /redwood/etc/rain in
the image (see userland/src/inject.c, rain_later) the window server kills launchd after five
minutes, XNU panics, the patched panic path calls loader_return(), and the shim's USB shell is
back on the same CDC-ACM node.  This tool sends that shell "reboot" and waits for fastboot, so
the next boot-capture run starts from a reachable device with nobody at the desk.

  python3 tools/autoloop.py --tag fix54 [--seconds 660] [--idle 240] [--no-fastboot]
"""
import argparse
import importlib.util
import os
import subprocess
import sys
import time

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def boot_capture_module():
    spec = importlib.util.spec_from_file_location(
        "bootcapture", os.path.join(REPO_DIR, "tools", "boot-capture.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def shell_and_reboot(bc, seconds=30):
    """If the shim shell is listening, say 'reboot' and wait for fastboot."""
    port, fd = bc.wait_port(seconds)
    if fd is None:
        print("[autoloop] no serial node appeared; device state unknown")
        return False
    print(f"[autoloop] serial node: {port}")
    try:
        bc.kick(fd)
    except OSError as why:
        # the kick only asks XNU to replay its RAM log; the shim's shell answers without it
        print(f"[autoloop] kick failed ({why}); asking anyway")
    time.sleep(0.5)
    seen = b""
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            data = os.read(fd, 4096)
        except (BlockingIOError, OSError):
            time.sleep(0.05)
            continue
        if not data:
            break
        seen += data
    print(f"[autoloop] console says: {seen[:200].decode(errors='replace')!r}")
    if b"shamrock>" not in seen:
        print("[autoloop] not the shim's shell; leaving it alone")
        return False
    os.write(fd, b"\r\nreboot\r\n")
    print("[autoloop] told the shim to reboot; waiting for fastboot")
    if bc.wait_fastboot(90):
        print("[autoloop] the board is in fastboot again")
        return True
    print("[autoloop] fastboot did not appear")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True, help="name for the capture log, e.g. fix54")
    ap.add_argument("--seconds", type=int, default=660)
    ap.add_argument("--idle", type=int, default=240)
    ap.add_argument("--skip-lk2nd", action="store_true")
    ap.add_argument("--wait", type=int, default=120, help="seconds to wait for the board in fastboot")
    ap.add_argument("--no-fastboot", action="store_true", help="capture only, do not talk back")
    ap.add_argument("--reboot-only", action="store_true", help="skip the boot, just ask the serial who is home")
    args = ap.parse_args()

    bc = boot_capture_module()
    if args.reboot_only:
        return 0 if shell_and_reboot(bc, seconds=8) else 1

    log_path = os.path.join(REPO_DIR, "out", f"capture-windowserver-{args.tag}.log")
    cmd = [sys.executable, os.path.join(REPO_DIR, "tools", "boot-capture.py"),
           "--seconds", str(args.seconds), "--idle", str(args.idle), "--wait", str(args.wait),
           "--log", log_path]
    if args.skip_lk2nd:
        cmd.append("--skip-lk2nd")
    print(f"[autoloop] booting, capture -> {log_path}")
    rc = subprocess.run(cmd).returncode
    print(f"[autoloop] boot-capture exited {rc}")

    bc = boot_capture_module()
    reached_fastboot = False
    if not args.no_fastboot:
        reached_fastboot = shell_and_reboot(bc)

    print(f"[autoloop] --- what the log shows ({os.path.getsize(log_path)} bytes) ---")
    for pattern in ("freeze flag", "the flag too", "no boolean", "would not take", "mprotect",
                    "export trie", "QuartzCore + 0x", "UX_EXCEPTION", "signal 1",
                    "StartWindowServer", "MiniIOSurface", "swap ", "registerNotificationPort",
                    "startup complete", "XNU KERNEL PANIC", "vsync #"):
        result = subprocess.run(["grep", "-ac", pattern, log_path], capture_output=True, text=True)
        count = result.stdout.strip() or "0"
        print(f"  {pattern!r}: {count}")
    return 0 if reached_fastboot or args.no_fastboot else 1


if __name__ == "__main__":
    sys.exit(main())
