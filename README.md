# GM 5 Plus (MSM8952) iOS 16.5 (XNU xnu-8796) Bare-Metal Port

This repository contains the bare-metal porting files, kernel patches, custom device drivers, and userland hooks required to boot **iOS 16.5 (Darwin 22.5.0 / XNU xnu-8796.121.2)** on the **General Mobile GM 5 Plus (Qualcomm Snapdragon 617 / MSM8952)**.

---

## 📱 Hardware & Target Specifications

| Parameter | Specification |
| :--- | :--- |
| **Device** | General Mobile GM 5 Plus (`shamrock` / `seed_mt`) |
| **SoC** | Qualcomm Snapdragon 617 (MSM8952) |
| **CPU Architecture** | 8x ARM Cortex-A53 (ARMv8.0-A, 64-bit, Little-Endian) |
| **GPU** | Adreno 405 (Software CPU rendering via QuartzCore) |
| **RAM** | 3GB LPDDR3 |
| **Display Panel** | 1080x1920, 24bpp (RGB888, stride: 3240, Base: `0x83200000`) |
| **Storage** | eMMC 5.1 (Direct block & APFS container support) |
| **Target OS** | iOS 16.5 (iPhone10,3 / D22AP, Build 20F66) |

---

## 🚀 Current Milestone & Status

- [x] **XNU Kernel Boot:** Darwin 22.5.0 boots bare-metal via custom `shamrock-shim`.
- [x] **ARMv8.0-A Compatibility:** ARMv8.3 PAC instructions NOP'd/emulated, 16KB/4KB page alignment resolved.
- [x] **Device Drivers:**
  - `MiniIOMobileFramebuffer`: TimingElements, ColorElements, `IOMFBMaxSrcPixels` registered.
  - `MiniIOSurface`: Fast shared-memory surface creation and management.
  - `GenericARMPE`: GICv2 interrupt handling and MSM8952 clock routing.
  - Qualcomm USB CDC-ACM console & interactive shell (/dev/ttyACM*).
  - Qualcomm PMIC Battery & Haptic controllers.
- [x] **Userland Bringup:**
  - `launchd` (PID 1) spawns all core daemons.
  - `backboardd` (PID 12) reaches **"startup complete"**.
  - `SpringBoard` (PID 7) connects as primary system shell (`BKSystemShellClient`).
  - `CAWindowServer` (`QuartzCore`) successfully initialized.
  - `PUIProgressWindow` boot UI and Apple Logo overlay active.
  - `SwapBegin` (Selector 4) and `SwapEnd` (Selector 5) paths validated.

---

## 📂 Repository Structure

- `xnu/`:
  - `hakios.patch`: Complete patch set against Apple's open-source `xnu-8796.121.2`.
  - `files/`: Custom in-tree drivers (`MiniIOMobileFramebuffer.cpp`, `MiniIOSurface.cpp`, `GenericARMPE.cpp`, PMIC/battery/haptics/UART).
  - `apply.sh` / `export.sh`: Scripts to apply or export changes to/from the XNU tree.
- `userland/`:
  - `src/inject.c`: Dynamic library injection (`libhakios.dylib`) for userland syscall translation and QuartzCore display hooking.
  - `src/init.c`: Custom PID 1 bootstrap orchestrator.
  - `build.sh`: Toolchain builder for userland hooks.
- `devices/shamrock/`:
  - `shim/`: Bare-metal assembly/C bootloader payload (`start.S`, `shim.c`, `ci_usb.c`).
  - `tools/`: DeviceTree generator (`mkadt-shamrock.py`), automated USB logging, and payload packing.
- `docs/`: Comprehensive hardware dossiers and bringup notes.

---

## ⚖️ Legal Disclaimer

> **IMPORTANT:**
> This repository is strictly an independent, non-commercial educational and security research project. 
> 
> - **NO** proprietary Apple binaries, copyrighted code, encrypted firmware files, or rootfs images are hosted or distributed in this repository.
> - **NO** proprietary device calibration data (e.g., IMEI, MAC addresses, RF calibration, `persist.img`, `modemst*`) are included.
> - Users must obtain the open-source XNU source code from Apple's official open-source releases (`xnu-8796.121.2`) and extract legitimate iOS filesystem components from Apple's publicly signed IPSW packages.

---

## 📜 Licenses

- Kernel modifications and drivers based on XNU are licensed under the **Apple Public Source License 2.0 (APSL 2.0)** (`LICENSE.APSL`).
- Custom bootloader tools, shims, userland hooks, and scripts authored for this project are licensed under the **MIT License** (`LICENSE`).
