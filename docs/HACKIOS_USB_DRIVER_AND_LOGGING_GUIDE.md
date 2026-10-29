# HackiOS USB Sürücüsü ve Canlı Log Çekme Kılavuzu

Bu doküman, General Mobile GM 5 Plus (`shamrock` / Snapdragon 617 MSM8952) üzerinde koşan Darwin XNU 16.5 çekirdeğinden USB üzerinden nasıl canlı log alınacağını, sürücü mimarisini ve bağlantı kurallarını açıklar. Yeni gelecek AI modelleri bu talimatları doğrudan uygulamalıdır.

---

## 1. Donanım ve Sürücü Mimarisi

GM 5 Plus'ta UART seri port hatları harici olarak dışarı verilmemiştir. Bu sebeple tüm çekirdek konsol çıktısı doğrudan **Qualcomm ChipIdea High-Speed USB 2.0 denetleyicisi** üzerinden USB CDC-ACM (Seri Port Emülasyonu) olarak host makineye iletilir.

- **USB Controller MMIO:** `0x078db000` (Qualcomm CI / EHCI tabanlı USB IP)
- **PHY Base MMIO:** `0x07900000` (Synopsys Femto PHY)
- **DMA Buffer:** `0x831F0000` (Qualcomm `cont_splash` hemen öncesinde ayrılan tampon bölge)
- **USB Vendor / Product ID:**
  - **Shim Aşaması (Bootloader):** `18d1:d00d` (Google Fastboot) veya `1209:0001` CDC
  - **XNU Kernel Aşaması:** `1209:0001 Generic pid.codes Test PID`
    - `iManufacturer: General Mobile`
    - `iProduct: HackiOS XNU Console`
- **Host Portu:** Linux host üzerinde `/dev/ttyACM0` olarak numaralandırılır (115200 baud, 8N1).

---

## 2. İki Aşamalı Boot Zinciri ve Portun Açılması

Cihaz Fastboot modundayken boot süreci şu sırayla gerçekleşir:

```
[Fastboot Modu] (dfe7d673)
       │
       ▼  fastboot boot tools/lk2nd-msm8952.img
[Stage 1: lk2nd]
       │
       ▼  fastboot boot out/shamrock-shim-boot.img
[Stage 2: shamrock-shim] (Açılışta /dev/ttyACM0 verir, 'shamrock> ' promptu)
       │
       ▼  'boot' komutu gönderilir
[XNU Kernel Jump] (USB yeniden numaralandırılır -> HackiOS XNU Console)
       │
       ▼  /dev/ttyACM0 dinlenir
[Canlı XNU Log Akışı]
```

---

## 3. Otomatik Log Çekme Scripti

Repo kökünde hazır bulunan `tools/auto-boot-and-log.py` betiği tüm bu aşamaları otomatik olarak yönetir:

```bash
python3 tools/auto-boot-and-log.py
```

### Script Neler Yapar?
1. Fastboot cihazını kontrol eder (`dfe7d673`).
2. `tools/lk2nd-msm8952.img` dosyasını `fastboot boot` eder.
3. 3 saniye sonra `out/shamrock-shim-boot.img` dosyasını `fastboot boot` eder.
4. `shamrock-shim` konsolunun (`/dev/ttyACM0`) gelmesini bekler ve `boot` komutunu gönderir.
5. Cihaz XNU'ya atladığında USB portu düşüp tekrar `HackiOS XNU Console` olarak gelir.
6. Port açılır açılmaz tüm kernel loglarını hem terminale yazar hem de `out/xnu-live-boot.log` dosyasına kaydeder.

---

## 4. Manuel Log Çekme ve Python Kodu

Eğer script yerine doğrudan Python ile log çekmek istersen:

```python
import serial
import time
import os

PORT = "/dev/ttyACM0"
LOG_FILE = "out/xnu-live-boot.log"

print(f"[*] {PORT} bekleniyor...")
while not os.path.exists(PORT):
    time.sleep(0.1)

# Port açma ve yetki retry döngüsü
s = None
for _ in range(30):
    try:
        s = serial.Serial(PORT, 115200, timeout=0.1)
        break
    except Exception:
        time.sleep(0.1)

if not s:
    raise RuntimeError("Port açılamadı!")

s.dtr = True
s.rts = True
print("[+] Bağlandı! Loglar akıyor:")

with open(LOG_FILE, "wb") as f:
    idle_count = 0
    while True:
        data = s.read(1024)
        if data:
            f.write(data)
            f.flush()
            print(data.decode("utf-8", errors="replace"), end="", flush=True)
            idle_count = 0
        else:
            time.sleep(0.1)
            idle_count += 1
            if idle_count > 150: # 15 sn sessizlik
                break

s.close()
print(f"\n[+] Log kaydedildi: {LOG_FILE}")
```

---

## 5. Kritik Hatalar ve Dikkat Edilmesi Gerekenler

1. **Udev Yetki Yarış Durumu (Permission Denied):**
   - Kernel USB'yi ayağa kaldırdığında Linux udev kuralları (`/etc/udev/rules.d/99-shamrock-usb.rules`) uygulanana kadar yaklaşık 100-200ms `[Errno 13] Permission denied` verebilir.
   - **Kural:** Portu doğrudan Linux `open()` ile açmaya çalışma (char device seekable değildir). Her zaman `serial.Serial` ile en az 10-20 denemelik `try-except` retry döngüsü kullan.
2. **Cihazı Fastboot'a Geri Döndürme:**
   - Cihaz XNU konsolundayken `/dev/ttyACM0` üzerinden `\r\nreboot\r\n` gönderilirse `loader_return` tetiklenir veya cihaz bootloader'a döner.
   - Cihaz kilitlenmişse kullanıcıdan `Ses Kısma + Güç` tuşlarıyla Fastboot'a alması istenir.
3. **USB CDC Buffer Taşması (Buffer Flooding):**
   - XNU içerisindeki `osfmk/arm64/ci_usb_xnu.c` sürücüsünde döngüsel tampon (`g_persist_log`) kullanılır. Loglar dökülürken recursive (özyinelemeli) kprintf çağrısı yapılmamalıdır.
