# HackiOS Dossier: Porting Apple iOS 16.5 (Darwin XNU 22.5.0) to General Mobile GM 5 Plus (MSM8952 / shamrock)

> **Document Type:** Mission Handoff & Deep Architectural Engineering Dossier  
> **Date:** October 27, 2026  
> **Target Audience:** Incoming Autonomous AI Agent in Mission / Goal Mode (with Full Codebase & Web Search Tools)  
> **Host Machine:** Ubuntu Linux 22.04 LTS | AMD Ryzen 7 7840HS (8C/16T, 16 Threads) | NVIDIA RTX 4070 (8GB VRAM)  
> **Target Hardware:** General Mobile GM 5 Plus (`shamrock` / `seed_mt`) | Qualcomm Snapdragon 617 (MSM8952)  
> **Current Status:** **HISTORIC MILESTONE ACHIEVED** — Darwin Kernel 22.5.0 boots on hardware, renders 1080x1920 video console, ChipIdea USB CDC-ACM enumerates as `HackiOS XNU Console`.  
> **Current Blocker:** In-kernel USB CDC-ACM log streaming produces 0 bytes after enumeration; screen output freezes after `iBoot Stage 2 version: shamrock-shim`; automated two-way log stream needed to eliminate manual screen photos.

---

## 1. Executive Summary & Project Mission

The objective of **HackiOS** on the General Mobile GM 5 Plus (`shamrock`) is to boot the Apple iOS 16.5 Darwin Kernel (`xnu-8796.121.2`) on the bare-metal Qualcomm Snapdragon 617 (MSM8952) SoC and execute the iOS userland (from static Mach-O Process 1 `hello` to `launchd`, `backboardd`, and `SpringBoard`).

We are combining architectural insights from:
1. **`dipa-studio/ios-sweet`:** Bare-metal Android `boot.img` shim (LK/lk2nd bootloader -> `fastboot boot` -> XNU Mach-O + ADT), non-PAC ARM64 patches, `SweetFramebuffer` (`AppleCLCD` software rendering), `0001-0069` XNU patch series.
2. **`oosexgang/hackios-xnu-kernel`:** Linux-native XNU build system (`xnu-deps`, `xnu-build.sh`), `DEVELOPMENT_ARM64_BCM2837` target adapted for Qualcomm GICv2 + PSCI, custom `hakios.*` drivers.
3. **Corellium Sandcastle / Checkm8 / t8030:** Emulating or replacing Apple-proprietary blocks (AIC -> GICv2, Apple UART -> ChipIdea CDC-ACM, Apple PMIC -> PMI8950).

---

## 2. Hardware & Environment Specifications

| Component | Specification | Memory / Address / IRQ Details |
| :--- | :--- | :--- |
| **SoC** | Qualcomm Snapdragon 617 (MSM8952) | 28nm LP process, 64-bit octa-core |
| **CPU Architecture** | ARMv8.0-A (8x ARM Cortex-A53) | **NO Pointer Authentication (PAC)**, 4KB page size |
| **CPU Clusters & MPIDR** | Cluster 0: 4x A53 @ 1.2 GHz<br>Cluster 1: 4x A53 @ 1.5 GHz | Cluster 0 MPIDR: `0x000, 0x001, 0x002, 0x003`<br>Cluster 1 MPIDR: `0x100, 0x101, 0x102, 0x103`<br>**Boot CPU in LK is CPU 4 (`MPIDR = 0x100`)** |
| **RAM** | 3GB LPDDR3 @ 933 MHz | Physical Range: `0x80000000 - 0x140000000` (3 GiB) |
| **Interrupt Controller** | ARM GICv2 (Qualcomm implementation) | GICD (Distributor): `0x0b000000` (4KB)<br>GICC (CPU Interface): `0x0b002000` (4KB)<br>Arch Timer: PPI 27 (Virtual Timer) |
| **Framebuffer** | 1080 x 1920 (FHD) IPS LCD | Base: `0x83200000`, Format: RGB888 (24 BPP), Stride: 3240 bytes |
| **USB Controller** | ChipIdea High-Speed USB 2.0 OTG | Core MMIO Base: `0x078db000` (4KB)<br>Cap Base: `0x078db140`<br>Synopsys 28nm Femto PHY CSR: `0x0006c000` |
| **PMIC & Charger** | Qualcomm PMI8950 + SMB1351 | SPMI USID 2 (PMI8950), I2C QUP2 `@ 0x5d` (SMB1351)<br>Fast Charge: `FAST_CHG_ACTIVE` (1.5A via `CMD_IL = 0x03`) |
| **Storage** | 32GB eMMC 5.1 | `sdhc_1` @ `0x07824900` (eMMC), `sdhc_2` @ `0x07864900` (SD Card) |

---

## 3. Verified Breakthroughs (What Is Proven & Working)

1. **Two-Stage Chainloader via Fastboot:**
   - Stage 1: `fastboot boot tools/lk2nd-msm8952.img` (re-initializes LK with 64-bit EL1, PSCI 0.2/1.0 SMC support).
   - Stage 2: `fastboot boot out/shamrock-shim-boot.img` (loads `shim-shamrock` bare-metal loader into RAM).
2. **Interactive Bare-Metal Loader (`shamrock-shim`):**
   - Presents interactive shell (`shamrock>`) over `/dev/ttyACM0`.
   - Commands: `chg` (PMI8950 battery telemetry), `boot`/`xnu` (boots kernel), `reboot` (reboots to fastboot), `peek`/`poke` (MMIO testing).
   - **Battery Life Preservation:** PMI8950 and SMB1351 chargers are active and forced to 1.5A fast charge (`FAST_CHG_ACTIVE`). The device does not deplete battery while docked in the loader shell.
3. **Darwin Kernel 22.5.0 Boot on GM 5 Plus Hardware:**
   - Kernel boots, sets up TTBR0/TTBR1 4KB page tables, initializes caches, and maps 3GB RAM (`vm_page_bootstrap: 177198 free pages`).
   - Mach IPC, Clutch Scheduler, Zones, kalloc, IOKit, and Qualcomm GICv2 interrupt handler initialize.
4. **Real Display Text Console (1080x1920 Screen Output):**
   - Video console (`initialize_screen` / `vcputc`) successfully renders Darwin boot log onto the physical phone screen.
   - Display shows verbose messages: L1/L2 cache, SMP boot info, machine startup, Darwin Kernel version.
5. **CPU Topology Matching (`machine_routines.c`):**
   - Solved panic `machine_routines.c:1325 Assertion failed: cpu_id <= ml_get_max_cpu_number()`.
   - Boot CPU is matched against hardware `MPIDR_EL1` (Cluster 1, `0x100`). Single-core fallback returns CPU 0 when `topology_info.num_cpus == 1`.
6. **In-Kernel ChipIdea USB CDC-ACM Driver (`ci_usb_xnu.c`):**
   - Successfully ported bare-metal ChipIdea driver from the shim into XNU (`osfmk/arm64/ci_usb_xnu.c`).
   - MMIO mapped via `ml_io_map(0x078db000, 0x1000)` and `ml_io_map(0x0006c000, 0x1000)`.
   - Address translation for DMA uses `ci_va_to_pa(va)` via `gVirtBase` / `gPhysBase`.
   - Hardware counter reading uses `__builtin_arm_rsr64("CNTVCT_EL0")`.
   - **Enumeration PROVEN in Linux dmesg:**
     ```text
     [34061.009029] usb 1-3: new high-speed USB device number 26 using xhci_hcd
     [34061.137597] usb 1-3: New USB device found, idVendor=1209, idProduct=0001, bcdDevice= 1.00
     [34061.137613] usb 1-3: Product: HackiOS XNU Console
     [34061.137617] usb 1-3: Manufacturer: General Mobile
     [34061.137620] usb 1-3: SerialNumber: SHAMROCK001
     [34061.180130] cdc_acm 1-3:1.0: ttyACM0: USB ACM device
     ```

---

## 4. Detailed Breakdown of Current Blockers

### Blocker A: In-Kernel USB CDC-ACM Data Streaming (Reads return 0 bytes)
- **Symptom:** The host Linux kernel detects `HackiOS XNU Console`, creates `/dev/ttyACM0`. However, attempting to `read()` from `/dev/ttyACM0` produces 0 bytes.
- **Root Cause Hypotheses:**
  1. **Interrupts Masked during Boot:** Between `arm_init()` line 688 and userland spawn, the kernel executes with interrupts disabled (`DAIF.I = 1`). `rtclock_intr()` does not fire. `machine_idle()` does not run. Therefore, `ci_usb_xnu_poll()` is only called when `loader_log_putc(c)` is invoked.
  2. **Transfer Descriptor (dTD) Not Serviced / IOC Handling:** When `ci_log_pump()` primes EP1 IN via `prime_ep(1, 1, &dtd_ep1_in)`, `ep1_tx_busy` is set to `true`. `ep1_tx_busy` only clears when `ENDPTCOMPLETE & (1U << 17)` is detected. If the USB host issues IN tokens, hardware completes the dTD, but if `ci_usb_poll_internal()` is not invoked frequently enough, `ep1_tx_busy` stays `true`, blocking subsequent log pumps.
  3. **DMA Memory Attribute / Cache Slicing:** The dQH and dTD structs and buffers are declared as static variables in `__DATA`. While `cache_clean` (`dc cvac`) and `cache_inval` (`dc civac`) are performed, does Cortex-A53 out-of-order write buffering or Qualcomm BIMC system cache require `dsb sy` or memory mapped as non-cacheable (`ml_io_map_wcomb` or uncached DMA pages)?
  4. **Host Terminal Line Control:** Does the host `cdc_acm` driver require `SET_CONTROL_LINE_STATE` (DTR/RTS assertion) before accepting BULK IN packets?

### Blocker B: Video Console Pausing after `iBoot Stage 2 version: shamrock-shim`
- **Symptom:** As seen in user photos (`media_1793054173642_5e86968b.jpg` and `media_1793055279047_ca48966e.jpg`), the screen boots up to:
  ```text
  Darwin Kernel Version 22.5.0: ...; root:xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837
  iBoot Stage 2 version: shamrock-shim
  ```
  and then stops outputting new text to the screen.
- **Root Cause Hypotheses:**
  1. **`switch_to_serial_console()`:** Previously, `serial=3` in `--cmdline` caused line 753 in `arm_init.c` to switch console ops from `VC_CONS_OPS` to `SERIAL_CONS_OPS`, redirecting all subsequent printfs to unpopulated UART TX hardware. Removing `serial=3` preserves `VC_CONS_OPS`.
  2. **`vcattach()` buffer replay vs live text:** Inside `PE_init_iokit()`, `vcattach()` acquires the screen and replays `msgbufp->msg_bufc`. Did `disableConsoleOutput` get set to `TRUE` by `PE_create_console()` or `bsd_init()`?
  3. **Silent Kernel Panic or Stall:** Does the kernel stall in `vfs_mountroot()` searching for `rd=md0`?

### Blocker C: Ramdisk & Userland (Process 1 `hello`)
- **Current Setup:**
  - Ramdisk: `/media/atakan/Yeni Birim/GM5PlusPort/hackios/prebuilt/userland/hello` (Mach-O 64-bit arm64 static executable).
  - Packed by `mkpayload.py` with `--ramdisk` and `--cmdline "-v debug=0xc cpus=1 keepsyms=1 rd=md0"`.
  - `RAMDisk` node `<phys, size>` added to `/chosen/memory-map` in `shamrock.adt`.
  - In XNU, `IOFindBSDRoot()` locates `RAMDisk` and hands it to `mockfs_mountroot()` to create `md0`.
  - Target: XNU spawns Process 1 (`/hello`), which writes:
    `"Hello from user space on Darwin!\nI am process 1, uid 0, console fd 3."`
    to `/dev/console` (FD 1).
  - In `bsd/dev/arm/km.c`, userland `/dev/console` write calls `loader_log_putc((char)*cp)`.

---

## 5. Architectural Directory & File Map

```text
/media/atakan/Yeni Birim/GM5PlusPort/
├── AGENTS.md                               <- Project Constitution & Hardware Rules
├── HACKIOS_PORTING_DOSSIER.md              <- This Master Technical Dossier
├── hackios/
│   ├── xnu-build.sh                        <- XNU build script (MUST use -j16)
│   ├── prebuilt/userland/hello             <- Static arm64 Mach-O Process 1 binary
│   └── devices/                            <- Device definitions
├── shim-shamrock/                          <- Bare-metal C loader & fastboot shim
│   ├── build.sh                            <- Builds out/shamrock-shim-boot.img
│   ├── shim.c                              <- Interactive command loop & reset logic
│   ├── loader.c                            <- SWEETXNU payload unpacker & XNU jump
│   ├── ci_usb.c & ci_usb.h                 <- Bare-metal ChipIdea USB CDC-ACM driver
│   ├── shamrock_charger.c                  <- PMI8950 / SMB1351 fast charge driver (1.5A)
│   └── mmu.c, console.c, start.S
├── tools/
│   ├── mkadt-shamrock.py                   <- ADT compiler for shamrock (iPhone10,3 / D22AP)
│   ├── auto-boot-and-log.py                <- Automated fastboot & boot sequence
│   └── launch-and-stream.py                <- Boot XNU and stream live USB CDC log
├── out/
│   ├── shamrock.adt                        <- Compiled Apple DeviceTree (12848 bytes)
│   ├── shamrock-payload.bin                <- SWEETXNU kernel + ADT + ramdisk payload
│   ├── shamrock-shim-boot.img              <- Final bootable Android boot.img
│   └── xnu-live-boot.log                   <- Output capture file
└── /home/atakan/hackios-work/xnu-src/xnu-8796.121.2/
    ├── osfmk/arm64/ci_usb_xnu.c            <- In-kernel ChipIdea USB CDC-ACM driver
    ├── osfmk/arm64/ci_usb_xnu.h            <- In-kernel driver headers
    ├── osfmk/arm64/ci_hw.h                 <- MSM8952 USB controller & PHY registers
    ├── osfmk/arm64/loader_return.c         <- loader_log_putc() & panic jump stub
    ├── osfmk/arm64/machine_routines.c      <- ml_is_boot_cpu (MPIDR 0x100) & machine_idle()
    ├── osfmk/arm/arm_init.c                <- ci_usb_xnu_init() hook & serial_init()
    ├── osfmk/arm/rtclock.c                 <- ci_usb_xnu_poll() hook in rtclock_intr()
    ├── osfmk/console/video_console.c       <- vcputc_options() & video console text mode
    ├── osfmk/console/serial_console.c      <- _serial_getc() USB input hook
    └── bsd/dev/arm/km.c                    <- /dev/console userland stdout hook
```

---

## 6. Exact Build & Run Commands

### 1. Compile XNU Kernel (Host Linux)
```bash
export DARWIN_REDWOOD=/home/atakan/hackios-work
cd /home/atakan/hackios-work/xnu-src/xnu-8796.121.2
/media/atakan/Yeni\ Birim/GM5PlusPort/hackios/xnu-build.sh -j16 -k
```
*Note: The `-k` flag is intentional because BCM2837 omits unused symbolsets; the kernel binary `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837` links cleanly in seconds.*

### 2. Generate Apple Device Tree (ADT)
```bash
cd /media/atakan/Yeni\ Birim/GM5PlusPort
python3 tools/mkadt-shamrock.py out/shamrock.adt
```

### 3. Pack SWEETXNU Payload (Kernel + ADT + Ramdisk)
```bash
cd /media/atakan/Yeni\ Birim/GM5PlusPort
python3 ios-sweet/tools/mkpayload.py \
  --ramdisk hackios/prebuilt/userland/hello \
  --cmdline "-v debug=0xc cpus=1 keepsyms=1 rd=md0" \
  /home/atakan/hackios-work/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  out/shamrock.adt \
  out/shamrock-payload.bin
```

### 4. Build Bare-Metal Shim Boot Image
```bash
cd /media/atakan/Yeni\ Birim/GM5PlusPort/shim-shamrock
./build.sh ../out/shamrock-payload.bin
cp -v out/shamrock-shim-boot.img ../out/shamrock-shim-boot.img
```

### 5. Flash / Boot via Fastboot (Two-Stage Chainload)
```bash
fastboot boot tools/lk2nd-msm8952.img
sleep 3
fastboot boot out/shamrock-shim-boot.img
```

---

## 7. Actionable Tasks & Next Steps for the Incoming Agent

### Priority 1: Solve USB CDC-ACM Data Pumping in `ci_usb_xnu.c`
- **Investigation:** Why did `read(/dev/ttyACM0)` return 0 bytes even though `Product: HackiOS XNU Console` enumerated?
- **Action Items:**
  1. In `ci_log_pump()`, check if `dtd_ep1_in` is being retired. Check `rd32(op_reg(OP_ENDPTSTAT))` and `rd32(op_reg(OP_ENDPTCOMPLETE))`.
  2. Implement an active poll loop or interrupt service for ChipIdea USB in XNU. On MSM8952, ChipIdea USB IRQ is SPI 134 (GIC interrupt ID `134 + 32 = 166`). Connecting a simple GICv2 interrupt handler for ID 166 to call `ci_usb_poll_internal()` will make USB handling 100% interrupt-driven without relying on polling!
  3. Ensure physical address conversion `ci_va_to_pa(&ep1_tx_buf)` and `ci_va_to_pa(&dtd_ep1_in)` is accessible by the 32-bit ChipIdea DMA engine. Check if uncached memory attributes (`ml_io_map_wcomb`) or cache line alignment (64 bytes) is strictly enforced.

### Priority 2: Diagnose Video Console Output Freezing
- **Investigation:** Why did the text console stop printing after `iBoot Stage 2 version`?
- **Action Items:**
  1. Inspect `bsd_init` in `bsd/kern/bsd_init.c`: Does it call `PE_init_printf(FALSE)`?
  2. Inspect `video_console.c`: Ensure `disableConsoleOutput` is `FALSE` throughout `bsd_init` and `vfs_mountroot`.
  3. Add distinct framebuffer color bands (or UART writes) across `vfs_mountroot`, `mockfs_mountroot`, and `load_init_program` to see exactly which C function XNU is executing.

### Priority 3: Verify Userland PID 1 Spawn (`/hello`)
- **Investigation:** Does `mockfs` mount the ramdisk (`rd=md0`)? Does XNU attempt to launch `/hello` or fallback to `/sbin/launchd`?
- **Action Items:**
  1. Check boot-args: Add `init=/hello` to `--cmdline` in `mkpayload.py` to ensure XNU directly executes the static Mach-O `/hello` instead of trying `/sbin/launchd`.
  2. Verify that `/hello`'s console output (via file descriptors 1 and 2) correctly reaches `/dev/console` -> `km.c:loader_log_putc` -> `ci_usb_xnu_putc`.

### Priority 4: Safe Hardware Handling
- **Rule:** Never discharge the battery. Always verify PMI8950 status (`chg` command in shim shows `FAST_CHG_ACTIVE`).
- **Rule:** Never advise `fastboot flash` of stock iOS IPSWs directly to eMMC partitions. All boots must remain RAM-booted via `fastboot boot`.

---
*Dossier prepared and verified with live hardware evidence on General Mobile GM 5 Plus (`shamrock`).*
