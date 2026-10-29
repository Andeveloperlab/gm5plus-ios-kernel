# HackiOS (GM 5 Plus / MSM8952 -> iOS 16.5) Proje Anayasası ve Sistem Kuralları

## 1. Donanım ve Ortam Bağlamı
- **Host Sistem:** Ubuntu Linux | AMD Ryzen 7 7840HS (8C/16T) | NVIDIA RTX 4070 (8GB VRAM). Derlemelerde her zaman `-j16` parametresini kullan.
- **Hedef Cihaz (Target):** General Mobile GM 5 Plus (Kod adı: `shamrock` / `seed_mt`).
- **SoC & Mimari:** Qualcomm Snapdragon 617 (MSM8952) | 8x ARM Cortex-A53 (ARMv8.0-A, 64-bit) | Adreno 405 GPU | 3GB LPDDR3 RAM | eMMC 5.1.
- **Hedef Sistem:** iOS 16.5 (Darwin / XNU Kernel + iOS 16.5 Userland).

## 2. Kritik Mimari Kısıtlamalar (Halüsinasyon Önleme)
- **Komut Seti & PAC:** iOS 16.5 binary'leri Apple A11+ / ARMv8.3-A+ (`arm64e`) hedeflidir. GM 5 Plus donanımsal PAC desteklemez. PAC komutları (`pacibsp`, `autibsp` vb.) emüle edilmeli veya NOP'lanmalıdır.
- **Sayfa Boyutu (16KB vs 4KB):** iOS 16.5 userland (dyld, launchd) katı şekilde **16KB page size** bekler. Cortex-A53 MMU donanımı ise yalnızca 4KB ve 64KB destekler. Bare-metalde sayfa boyutu shim/çevirisi veya QEMU TCG emülasyonu şarttır.
- **Güvenlik Alt Sistemi:** AMFI ve kod imzalama bypass edilmeden userland ayağa kalkamaz (`amfi_get_out_of_my_way=1`, `cs_enforcement_disable=1`). FairPlay korumalı ikililer için `inject_fairplay.c` yaklaşımı esastır.
- **Interrupt Controller:** Apple AIC yerine Qualcomm GICv2 köprülenecektir.
- **Yasaklı Yaklaşım:** Asla stok iOS 16.5 IPSW dosyasını doğrudan `fastboot flash` ile GM 5 Plus'a yazmayı önerme!
- **Zorunlu Mühendislik Rotaları:**
  1. **Rota A (QEMU-KVM / t8030 Yaklaşımı):** GM 5 Plus üzerinde Linux + KVM/QEMU katmanı (`qemu-t8030` / custom board) ile XNU'yu sanallaştırmak, Framebuffer/DRM ile ekrana basmak.
  2. **Rota B (Açık Kaynak XNU + Shim/Device Tree Portu):** XNU'yu ARMv8.0-A için derlemek, `msm8952-shamrock.dtb`'yi Apple DeviceTree (`chosen`, `arm-io`) formatına dönüştürmek, erken UART (`ci_hw.h`) ve framebuffer sürücülerini bağlamak.

## 3. Token ve Context Ekonomisi (Katı Kurallar)
- **Surgical Diffs:** Asla mevcut bir dosyanın tamamını baştan yazdırma. Yanıtlarında sadece `diff -u` formatında veya etkilenen fonksiyon bloğunu içeren minimal patch'ler sun.
- **Skeleton First:** Bir sürücü veya fonksiyon incelerken tüm dosyayı okuma; önce `ctags` veya `grep` ile sadece struct ve fonksiyon prototiplerini talep et.
- **Log Sınırı:** Serial/panic loglarından (`out_qemu_test*`) 50 satırdan fazlasını ham olarak context'e alma. Yalnızca `panic()`, `ESR_EL1`, `FAR_EL1`, `PC`, `LR` ve backtrace satırlarını analiz et.

## 4. Codebase Navigation (Graphify & Bellek)
- Kod tabanında kaybolmamak ve grep spam'i yapmamak için önce `GRAPH_REPORT.md` dosyasını oku.
- Sembol bağımlılıkları, sürücü hiyerarşisi ve fonksiyon çağrı yolları için `graphify-out/graph.json` grafını referans al.
- Donanım adresleri (UART, SPMI, GIC) için varsayımda bulunma, yerel DTS/DTSI ve `shim/` tanımlarını temel gerçek (ground truth) kabul et.
