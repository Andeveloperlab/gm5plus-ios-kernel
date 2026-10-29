# HackiOS (GM 5 Plus / MSM8952 -> iOS 16.5) Proje Anayasası ve Sistem Kuralları

## 1. Donanım ve Ortam Bağlamı
- **Host Sistem:** Ubuntu Linux | AMD Ryzen 7 7840HS (8C/16T) | NVIDIA RTX 4070 (8GB VRAM). Derlemelerde her zaman `-j16` parametresini kullan.
- **Hedef Cihaz (Target):** General Mobile GM 5 Plus (Kod adı: `shamrock` / `seed_mt`).
- **SoC & Mimari:** Qualcomm Snapdragon 617 (MSM8952) | 8x ARM Cortex-A53 (ARMv8.0-A, 64-bit) | Adreno 405 GPU | 3GB LPDDR3 RAM | eMMC 5.1.
- **Hedef Sistem:** iOS 16.5 (Darwin / XNU Kernel).

## 2. Kritik Mimari Kısıtlamalar (Halüsinasyon Önleme)
- **Komut Seti Farkı (ARM64e vs ARMv8.0-A):** iOS 16.5 çekirdeği ve sistem binary'leri Apple A11+ / ARMv8.3-A+ (`arm64e` - Pointer Authentication / PAC, 16KB page size, Apple AIC interrupt controller) hedeflidir. GM 5 Plus ise standart `ARMv8.0-A` (Cortex-A53) kullanır, donanımsal PAC desteklemez ve Qualcomm GICv2 interrupt controller kullanır.
- **Yasaklı Yaklaşım:** Asla stok iOS 16.5 IPSW dosyasını doğrudan `fastboot flash` ile GM 5 Plus'a yazmayı önerme!
- **Zorunlu Mühendislik Rotaları:**
  1. **Rota A (QEMU-KVM / ChefKiss / Sandcastle / t8030 yaklaşımı):** GM 5 Plus üzerinde mainline/custom Linux + KVM veya TCG/QEMU katmanı (`qemu-t8030` / custom ARM64 board modeli) üzerinden iOS 16.5 XNU kernelini önyüklemek, ARMv8.3 PAC komutlarını (`pacibsp`, `autibsp` vb.) TCG/yamalarla NOP'lamak veya emüle etmek, Framebuffer/DRM üzerinden ekran çıktısı almak.
  2. **Rota B (Açık Kaynak XNU + LittleKernel/U-Boot + Device Tree Portu):** Açık kaynak XNU çekirdeğini ARMv8.0-A için derlemek, GM 5 Plus (`msm8952-shamrock.dtb`) Device Tree yapısını Apple DeviceTree (`chosen`, `arm-io`) formatına köprülemek ve temel UART/Framebuffer sürücülerini bağlamak.

## 3. Çalışma Prensipleri
- Her zaman kanıt odaklı ilerle: Bir register adresi, bellek haritası (MMIO) veya kernel parametresi yazmadan önce yerel `.dts`/`.dtsi` dosyalarını veya `ipsw` çıktılarını kontrol et.
- Büyük dosyaları tek seferde okumaya çalışarak context penceresini doldurma; `grep`, `ripgrep (rg)`, `ctags` veya `ipsw` filtrelerini kullan.
