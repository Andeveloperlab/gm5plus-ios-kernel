# General Mobile GM 5 Plus (MSM8952) iOS 16.5 Bare-Metal Bringup: `backboardd` HID Kilitlenme Analizi ve Çözüm Kılavuzu

## 1. Sistem Bağlamı, Mevcut Kilometre Taşları ve Tersine Mühendislik Bulguları

**General Mobile GM 5 Plus (Qualcomm Snapdragon 617 / MSM8952 — 8x ARM Cortex-A53, ARMv8.0-A)** donanımı üzerinde **iOS 16.5 (Darwin 22.5.0 / XNU `xnu-8796.121.2` / `iPhone10,3` - `D22AP`)** işletim sisteminin bare-metal önyükleme sürecinde çekirdek ve kullanıcı alanı (userspace) başlatma zinciri aşağıdaki aşamaları tamamlamıştır:

| Katman / Bileşen | Durum | Teknik Detaylar (`capture-stable-hid4.log` Verileri) |
| :--- | :--- | :--- |
| **XNU Çekirdek & Bellek** | Çalışıyor | Physical Base `0x90000000`, Virtual Base `0xFFFFFFF006000000`, 2815 MB RAM, 16 KB sayfa boyutu (`page size 16384`), 1 aktif CPU çekirdeği (`cpu 0`). |
| **Donanım Sürücüleri (Bringup)** | Çalışıyor | `MiniIOMobileFramebuffer` (`1125x2436` mantıksal / `1080x1920` panel), `HakiosBattery` (PM8950/PMI8950 QPNP VADC & Charger), `backlight` (`/dev/hakios-backlight`, PMI8950 WLED), `haptic` (`/dev/hakios-haptic`), SDHCI eMMC 5.1 (`/dev/disk0`, `/dev/disk1`), ChipIdea USB CDC-ACM, `Hakios WDT` (`IOWatchdog` sahte check-in). |
| **Kök Dosya Sistemi & `launchd`** | Çalışıyor | `/dev/disk1` APFS `System` volume read-only mount edildi, `/private/var` bellek dosya sistemi ve `/redwood` overlay hazırlandı. `/sbin/launchd.ios` (PID 1) `libignition` (`level 0x5`) ile devraldı. |
| **Kullanıcı Alanı Servisleri** | Çalışıyor | `powerd` (PID 8), `SpringBoard` (PID 7), `backboardd` (PID 12), `datamigrator` (PID 30) başarıyla spawn edildi. `com.apple.backboardd.plist` içindeki `ResetAtClose: true` bayrağı devre dışı bırakıldı. |
| **Dylib Enjeksiyonu (`inject.conf`)** | Çalışıyor | `/redwood/lib/libhakios.dylib` (`backboardd`, `containermanagerd`), `/redwood/lib/libhakios.sb.dylib` (`SpringBoard`), `/redwood/lib/libhakios.media.dylib` (`mediaserverd`). |

### `/usr/libexec/backboardd` Başlatma Akışı ve Disassembly Analizi

iOS 16.5 `/usr/libexec/backboardd` ikili dosyasında (binary) başlatma kontrol listesi (checklist) aşağıdaki sırayı takip etmektedir:

1. **`prewarm oscard where available`**: `11.5065s` içinde tamamlandı (`com.apple.oscard` bulunamadı hatası güvenli şekilde geçildi).
2. **`CoreAccessories`**: `19.0408s` içinde tamamlandı (toplam süre `30.6606s`).
3. **`HID system`**:
   - **3.1. `HID system open` (`pc = 0x10000a79c`)**:
     `+[BKHIDSystemInterface sharedInstance]` çağrılır. Bu çağrı içsel olarak `-[BKHIDSystemInterface init]` metodunu çalıştırır, `25.6016s` içinde tamamlanır ve `0x100e16650` nesne adresini döner (toplam süre `84.1919s`).
   - **3.2. `HID system start` (`pc = 0x10000a058` -> `0x10000a7d8`)**:
     `0x10000a7d8` adresindeki assembly bloğu çalışır:
     ```assembly
     10000a7d8: ldr x8, [x0, #0x20]
     10000a7dc: ldr x8, [x8, #0x8]
     10000a7e0: ldr x0, [x8, #0x28] ; x0 = BKHIDSystemInterface instance (0x100e16650)
     10000a7e4: b   0x10009f3a0    ; objc_msgSend stub -> Selector: "startHIDSystem"
     ```
     `-[BKHIDSystemInterface startHIDSystem]` çağrıldığı anda `backboardd` tamamen kilitlenir.
4. **`GS` (GraphicsServices)** *(Ulaşılamadı)*
5. **`window server` (CoreAnimation `CA::WindowServer`)** *(Ulaşılamadı)*
6. **`HID connection manager`** *(Ulaşılamadı)*

### Canlı Konsol Çıktısı (`/dev/console` Kilitlenme Anı)

```text
1970-01-01 02:05:27.625344+0200 backboardd[12:211] [Common] start HID system...
1970-01-01 02:05:27.736294+0200 backboardd[12:211] [Common] start HID system open...
hakios: +[BKHIDSystemInterface sharedInstance] START
hakios: -[BKHIDSystemInterface init] START (0x100e16650)
1970-01-01 02:05:37.825133+0200 backboardd[12:211] [PearlEventFilter] AssertMacros: service (value = 0x0), file: /Library/Caches/com.apple.xbs/Sources/Pearl_UI/PearlEventFilter/PearlEventFilter.mm, line: 302
1970-01-01 02:05:38.036988+0200 backboardd[12:211] [PearlEventFilter] PearlEventFilter::pearlConnect -> [-536870212: Unknown error: -536870212]
1970-01-01 02:05:51.470099+0200 backboardd[12:250] AssertMacros: 0 (value = 0x0), file: /Library/Caches/com.apple.xbs/Sources/BiometricKit/BiometricKit/BiometricKitXPCClient.m, line: 89
1970-01-01 02:05:51.703565+0200 backboardd[12:250] AssertMacros: 0 (value = 0x0), file: /Library/Caches/com.apple.xbs/Sources/BiometricKit/BiometricKit/BiometricKitXPCClient.m, line: 535
1970-01-01 02:05:51.937149+0200 backboardd[12:250] [Framework-Internal-Legacy] AssertMacros: _xpcClient (value = 0x0), file: /Library/Caches/com.apple.xbs/Sources/BiometricKit/BiometricKit/BiometricKit.m, line: 133
1970-01-01 02:05:52.244971+0200 backboardd[12:211] [Framework-API] BKDeviceTouchID::deviceAvailableWithFailure -> 0 (0)
1970-01-01 02:05:52.366336+0200 backboardd[12:211] [Framework-API] BKDevicePearl::deviceAvailableWithFailure -> 0
hakios: -[BKHIDSystemInterface init] DONE -> 0x100e16650
hakios: +[BKHIDSystemInterface sharedInstance] DONE -> 0x100e16650
1970-01-01 02:05:53.337902+0200 backboardd[12:211] [Common]       HID system open ready in 25.6016s. overall 84.1919s
1970-01-01 02:05:53.454078+0200 backboardd[12:211] [Common] start HID system start...
GenericARMPE: the slower cores started too: 1 processors running
[HB] heartbeat 3, 30 s since start
[node hung up after 264355 bytes; no node came back]
```

## 2. `-[BKHIDSystemInterface startHIDSystem]` İç Mimarisi ve Kilitlenme Analizi

`/usr/libexec/backboardd` içindeki `BKHIDSystemInterface` (`0x100cc72c0`), [BackBoardHIDEventFoundation.framework](https://developer.limneos.net/index.php?ios=17.1&framework=BackBoardHIDEventFoundation.framework) özel framework'ü içerisinde yer alan `BKHIDSystem` sınıfını sarmalayan (wrapper) üst katman nesnesidir. `BKHIDSystemInterface`, `-system` property'si üzerinden alttaki `BKHIDSystem` nesnesine erişir ve tüm olay yönlendirme/işleme çağrılarını bu nesneye iletir.

### `BKHIDSystemInterface` Çalışma Zamanı Metod Tablosu

`capture-stable-hid4.log` içerisinde `libhakios.dylib` tarafından çalışma zamanında tespit edilen 25 metod ve mimari görevleri şunlardır:

| Kategori | Selector Adı | Mimari Görevi |
| :--- | :--- | :--- |
| **Yaşam Döngüsü & Başlatma** | `init` | `BKHIDSystem` nesnesini, `BKHIDEventDeliveryManager`, `BKHIDEventSenderCache` ve `IOHIDEventSystemCreate` yapılarını oluşturur. |
| **Yaşam Döngüsü & Başlatma** | `startHIDSystem` | `[self.system startHIDSystem]` üzerinden [`IOHIDEventSystemOpen`](https://github.com/aosm/IOHIDFamily/blob/master/tools/IOHIDEventSystemMonitor.c) çağrısını yapar, kernel `IOHIDSystem` bağlantısını ve SessionFilter eklentilerini başlatır. |
| **Yaşam Döngüsü & Başlatma** | `startEventRouting` | `BKHIDEventDeliveryManager` yönlendirme tablolarını ve kural çözümleyicilerini (`_BKRuleResolutionTranscript`) etkinleştirir. |
| **Yaşam Döngüsü & Başlatma** | `startEventProcessing` | `BKHIDPrimaryEventProcessor` ve `BKHIDEventProcessorRegistry` olay işleme zincirini başlatır. |
| **Yaşam Döngüsü & Başlatma** | `startServer` | `com.apple.iohideventsystem` Mach sunucu portunu ve XPC/BSService domain sunucusunu (`BKHIDDomainServiceServer`) başlatır. |
| **Yaşam Döngüsü & Başlatma** | `registerWatchdog` | HID olay kuyruğu kilitlenmelerini denetleyen watchdog zamanlayıcısını kaydeder. |
| **Alt Bileşen Erişimcileri (Getters)** | `system` | Alttaki `BKHIDSystem` (`BackBoardHIDEventFoundation`) nesnesini döner. |
| **Alt Bileşen Erişimcileri (Getters)** | `deliveryManager` | `BKHIDEventDeliveryManager` nesnesini döner. |
| **Alt Bileşen Erişimcileri (Getters)** | `senderCache` | `BKHIDEventSenderCache` nesnesini döner. |
| **Alt Bileşen Erişimcileri (Getters)** | `clientConnectionManager` | `BKHIDClientConnectionManager` nesnesini döner (**Adım 6: `HID connection manager` için zorunludur**). |
| **Alt Bileşen Erişimcileri (Getters)** | `HIDSystemChannel` / `setHIDSystemChannel:` | HID sistem kanalı referansını okur veya yazar. |
| **Olay Enjeksiyonu & Dağıtım** | `injectHIDEvent:` | Dışarıdan oluşturulan bir `IOHIDEventRef` nesnesini doğrudan HID işlem hattına enjekte eder. |
| **Olay Enjeksiyonu & Dağıtım** | `injectGSEvent:` | Eski tip GraphicsServices (`GSEventRef`) olaylarını enjekte eder. |
| **Olay Enjeksiyonu & Dağıtım** | `postEvent:toDestination:` | Belirli bir hedefe (`BKSHIDEventDeferringTarget` / PID) olay gönderir. |
| **Olay Enjeksiyonu & Dağıtım** | `postEvent:fromSender:` | Belirli bir göndericiden (`BKHIDEventSenderInfo`) gelen olayı dağıtır. |
| **Olay Enjeksiyonu & Dağıtım** | `systemEventOfType:matchingEvent:options:` | Sistem seviyesinde eşleşen HID olayını sorgular. |
| **Olay Enjeksiyonu & Dağıtım** | `dispatcherForEventType:` | Olay tipine göre `BKHIDEventDeliveryClient` / dispatcher döner. |
| **Olay Enjeksiyonu & Dağıtım** | `destinationsForEvent:fromSender:` | Bir olayın hangi hedef uygulamalara (örn. `SpringBoard`) gideceğini hesaplar. |
| **Olay Enjeksiyonu & Dağıtım** | `destinationsForEvent:fromSender:overrideSenderDescriptor:` | Gönderici tanımını ezerek hedef çözümlemesi yapar. |
| **IOKit Servis Eşleştirme & Özellikler** | `IOHIDServicesMatching:` | Eşleşen `BKIOHIDService` listesini döner. |
| **IOKit Servis Eşleştirme & Özellikler** | `registerIOHIDServicesCallback:matchingDictionary:target:refCon:` | Yeni HID servisleri eklendiğinde çağrılacak callback kaydeder. |
| **IOKit Servis Eşleştirme & Özellikler** | `unregisterIOHIDServicesCallback:matchingDictionary:target:refCon:` | Kayıtlı HID servis callback'ini kaldırır. |
| **IOKit Servis Eşleştirme & Özellikler** | `systemPropertyForKey:` / `setSystemProperty:forKey:` | `IOHIDEventSystemGetProperty` / `SetProperty` sarmalayıcıları. |
| **Durum Bildirimi & Bellek** | `hidSystem:receivedUpdatedDeviceOrientation:` | İvmeölçer/jiroskop üzerinden cihaz yönelimi değiştiğinde tetiklenir. |
| **Durum Bildirimi & Bellek** | `.cxx_destruct` | ARC ivar referanslarını serbest bırakır. |

### `HID system open` (Adım 3.1) ile `HID system start` (Adım 3.2) Arasındaki Fark

1. **Adım 3.1 (`HID system open` — `init` Aşaması):**
   - `+[BKHIDSystemInterface sharedInstance]` -> `-[BKHIDSystemInterface init]` -> `-[BKHIDSystem init]` çalışır.
   - Bu aşamada `IOKit.framework` içinden [`IOHIDEventSystemCreate(kCFAllocatorDefault)`](https://github.com/aosm/IOHIDFamily/blob/master/tools/IOHIDEventSystemMonitor.c) çağrılır.
   - `/System/Library/HIDPlugins/` altındaki eklenti paketleri (`PearlEventFilter`, `BiometricKit` vb.) belleğe yüklenir ve ön hazırlıkları yapılır. Logdaki `02:05:37.825` (`PearlEventFilter::pearlConnect -> -536870212` yani `kIOReturnNotFound = 0xe00002bc`) ve `02:05:51.470` (`BiometricKitXPCClient.m`) hataları bu eklentilerin donanım servislerini bulamayıp zaman aşımına uğramasından kaynaklanır. Bu nedenle `init` adımı **25.6016 saniye** sürer ancak başarıyla tamamlanarak `0x100e16650` döner.

2. **Adım 3.2 (`HID system start` — `startHIDSystem` Aşaması):**
   - `-[BKHIDSystemInterface startHIDSystem]` metodu, içerideki `BKHIDSystem` nesnesinin `-[BKHIDSystem startHIDSystem]` metodunu çağırır.
   - `-[BKHIDSystem startHIDSystem]` içerisinde [`IOHIDEventSystemOpen(_eventSystem, callback, target, refcon, 0)`](https://github.com/aosm/IOHIDFamily/blob/master/tools/IOHIDEventSystemMonitor.c) fonksiyonu çağrılır.
   - `IOHIDEventSystemOpen` şu dört kritik işlemi yürütür:
     1. **Kernel `IOHIDSystem` Bağlantısı:** [CVE-2016-1824 IOHIDFamily analizinde](https://marcograss.github.io/security/apple/cve/2016/05/16/cve-2016-1824-apple-iohidfamily-racecondition.html) belgelendiği üzere `IOServiceMatching("IOHIDSystem")` ile kernelde `IOHIDSystem` servisini arar ve `IOServiceOpen(service, mach_task_self(), 3 /* kIOHIDEventSystemConnectType */, &conn)` ile `IOHIDEventSystemUserClient` bağlantısı açar. Ardından `IOConnectCallMethod(conn, 0 /* createEventQueue */, ...)` ile çekirdek olay kuyruğunu (`kIOHIDEventSystemKernelQueueID`) oluşturur ve `IOConnectMapMemory` ile paylaşımlı `IODataQueue` belleğini kullanıcı alanına haritalar.
     2. **SessionFilter ve ServiceFilter Eklentilerinin `open` Çağrıları:** `/System/Library/HIDPlugins/SessionFilters/` ve `/System/Library/Extensions/IOHIDFamily.kext/PlugIns/` altındaki filtrelerin oturum açma (`open` / `scheduleWithDispatchQueue`) rutinlerini çalıştırır.
     3. **Gerçek Zamanlı (Real-Time) HID Thread Başlatma:** `IOHIDEventSystem` ve `BKHIDSystem`, donanım olaylarını düşük gecikmeyle okumak için `THREAD_TIME_CONSTRAINT_POLICY` (`SCHED_FIFO` benzeri Mach gerçek zamanlı zamanlama politikası) kullanan yüksek öncelikli bir iç sunucu thread'i başlatır.
     4. **Senkron Hazırlık Beklemesi (`dispatch_semaphore_wait` / `mach_msg_trap`):** Başlatıcı thread (`tid 211`), iç HID event loop thread'inin ve servis eşleştiricilerinin (`BKIOHIDServiceMatcher`) hazır sinyali üretmesini senkron olarak bekler.

### Kilitlenmenin (Hang / Deadlock) Kök Nedenleri

`capture-stable-hid4.log` verilerine göre `backboardd`'nin `start HID system start...` satırında kalmasının iki temel nedeni vardır:

1. **Eksik Servis / Senkron Semafor ve Mach Trap Bloklanması:**
   Açık kaynak XNU çekirdeğinde `IOHIDSystem` (`IOHIDFamily.kext`) bulunmadığında veya `IOHIDEventSystemOpen` içindeki bir `SessionFilter` eklentisi (veya `BKHIDSystem` içindeki `dispatch_sync` / `dispatch_semaphore_wait(..., DISPATCH_TIME_FOREVER)` bloğu) yanıt dönmeyen bir XPC/IOKit Mach portunu (`mach_msg_trap`) beklediğinde `backboardd` ana başlatma thread'i (`tid 211`) sonsuza kadar bloklanır.
2. **Tek Çekirdek (`1 CPU`) Üzerinde Gerçek Zamanlı Thread Açlığı (RT Starvation):**
   Log dosyanızdaki şu satırlar kritik bir donanım durumunu göstermektedir:
   - `memory 2815 MB, page size 16384, 1 cpu`
   - `[GIC] sgi 8 self-test: taken=0 (of 0 since boot); irq is not delivered`
   - `[TMR] idle: polled 0 times, 0 forced ticks (0 polls: the idle loop was never reached)`
   - `GenericARMPE: the slower cores started too: 1 processors running`
   Sistem şu anda **yalnızca tek çekirdek (`cpu 0`)** üzerinde çalışmaktadır ve GIC SGI (Software Generated Interrupt) kesmeleri henüz teslim edilmemektedir. `startHIDSystem` yüksek öncelikli (`THREAD_TIME_CONSTRAINT_POLICY`) bir HID thread'i oluşturduğunda veya bir iç spin/poll döngüsüne girdiğinde, tek CPU üzerinde daha düşük öncelikli thread'ler (veya sinyal üretecek XPC/dispatch worker thread'leri) CPU zamanı alamaz (preemption starvation) ve ~180 saniye sonra MSM8952 donanım watchdog'u cihazı sıfırlar.

## 3. Çözüm 1: Dylib Seviyesinde Hooking (`inject.c` / `libhakios.dylib`)

### Sadece `startHIDSystem` Metodunu NOP Yapmak Yeterli midir?

`-[BKHIDSystemInterface startHIDSystem]` metodunu tek başına NOP yapmak `start HID system start...` adımını geçmenizi sağlar; ancak `backboardd` başlatma listesinin (checklist) hemen devamında yer alan adımlarda çökme (`SIGABRT` / `EXC_BAD_ACCESS`) yaşanmaması için şu hususlara dikkat edilmelidir:

1. **`-[BKHIDSystemInterface init]` Zaten Başarıyla Tamamlandı (`0x100e16650`):**
   Logunuzda `hakios: -[BKHIDSystemInterface init] DONE -> 0x100e16650` görüldüğü üzere, `BKHIDSystemInterface` nesnesi ve onun içindeki `_system` (`BKHIDSystem`) nesnesi zaten bellekte oluşturulmuş durumdadır.
2. **Ardışık Başlatma Metodları (`startEventRouting`, `startEventProcessing`, `startServer`, `registerWatchdog`):**
   `backboardd`, `startHIDSystem` çağrısından hemen sonra sırasıyla:
   - `-[BKHIDSystemInterface startEventRouting]`
   - `-[BKHIDSystemInterface startEventProcessing]`
   - `-[BKHIDSystemInterface startServer]`
   - `-[BKHIDSystemInterface registerWatchdog]`
   metodlarını çağırır. Eğer `startHIDSystem` NOP yapılırsa, `_eventSystem` (`IOHIDEventSystemRef`) açılmamış olacağı için `startServer` veya `registerWatchdog` içinde kilitlenme ya da assertion hatası oluşabilir. Özellikle **`registerWatchdog` kesinlikle NOP yapılmalıdır**, aksi takdirde `backboardd` kendi HID kuyruğunu denetlerken zaman aşımına düşüp kendini kapatır.
3. **Adım 6 (`HID connection manager`) ve Nil Getter Koruması:**
   `backboardd` başlatma listesinin 6. adımı (`HID connection manager`), `-[BKHIDSystemInterface clientConnectionManager]` metodunu çağırır ve dönen `BKHIDClientConnectionManager` nesnesinin `nil` olmamasını bekler. Aynı şekilde olay dağıtımı sırasında `deliveryManager` ve `senderCache` getter'ları çağrılır. Eğer `startHIDSystem` bypass edildiği için bu ivar'lar `nil` kalmışsa, getter metodları çalışma zamanında dinamik bir **stub nesnesi (`HakiosHIDStubObject`)** dönecek şekilde korunmalıdır.

### Tam Kapsamlı `inject.c` (`libhakios.dylib`) Kodu

Aşağıdaki C / Objective-C runtime kodu şu dört işlevi tek bir modülde birleştirir:
1. **Runtime Ivar Dumper:** `BKHIDSystemInterface` ve içindeki `BKHIDSystem` nesnesinin tüm ivar isimlerini, tiplerini, bellek ofsetlerini ve o anki işaretçi değerlerini `/dev/console` (`stderr`) üzerine döker.
2. **Yaşam Döngüsü Hook'ları:** `startHIDSystem`, `startEventRouting`, `startEventProcessing`, `startServer` ve `registerWatchdog` metodlarını yakalar. `HAKIOS_BYPASS_HID_START` makrosu `1` iken güvenli şekilde bypass ederek `GS` (GraphicsServices) ve `window server` (`CA::WindowServer`) adımlarına geçişi sağlar.
3. **Evrensel Dummy/Stub Nesnesi (`HakiosHIDStubObject`):** `deliveryManager`, `senderCache` veya `clientConnectionManager` `nil` dönerse, kendisine gönderilen her Objective-C mesajını (`resolveInstanceMethod:` üzerinden) güvenli şekilde yutup `nil`/`0` dönen sahte bir nesne üretir.
4. **`/dev/hakios-touch` -> `BKHIDSystemInterface` Doğrudan Dokunmatik Enjeksiyon Köprüsü:** `IOHIDFamily.kext` olmadan `/dev/hakios-touch` cihazından okunan dokunmatik verilerini `IOHIDEventCreateDigitizerFingerEvent` ile paketleyip doğrudan `-[BKHIDSystemInterface injectHIDEvent:]` metoduna gönderen hazır thread altyapısını içerir.

```c
// ============================================================================
// libhakios.dylib - backboardd BKHIDSystemInterface Hooking & Touch Bridge
// Hedef: iOS 16.5 (Darwin 22.5.0) /usr/libexec/backboardd (ARM64)
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>

// Konsola tamponsuz (unbuffered) anında log basma makrosu
#define HAKIOS_LOG(fmt, ...) do { \
    fprintf(stderr, "hakios-hid: " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
} while (0)

// 1 = startHIDSystem ve alt adımlarını bypass et (GS ve WindowServer'a hemen geç)
// 0 = Orijinal metodları adım adım loglayarak çağır (IOHIDEventSystemOpen hook'u ile)
#define HAKIOS_BYPASS_HID_START 1

// 1 = IOHIDEventSystemOpen C fonksiyonunu da dlsym/hook seviyesinde yakala
#define HAKIOS_HOOK_IOHID_OPEN  1

// ----------------------------------------------------------------------------
// 1. Runtime Ivar & Property İnceleme Aracı
// ----------------------------------------------------------------------------
static void hakios_dump_ivars(id obj, const char *label) {
    if (!obj) {
        HAKIOS_LOG("[IVAR-DUMP] %s is nil!", label);
        return;
    }
    Class cls = object_getClass(obj);
    unsigned int count = 0;
    Ivar *ivars = class_copyIvarList(cls, &count);
    HAKIOS_LOG("[IVAR-DUMP] === %s (Class: %s @ %p, %u ivars) ===",
               label, class_getName(cls), obj, count);

    for (unsigned int i = 0; i < count; i++) {
        const char *name = ivar_getName(ivars[i]);
        const char *type = ivar_getTypeEncoding(ivars[i]);
        ptrdiff_t offset = ivar_getOffset(ivars[i]);
        void *ptr_val = *(void **)((uintptr_t)obj + offset);
        if (type && type[0] == '@' && ptr_val != NULL) {
            Class fieldCls = object_getClass((id)ptr_val);
            HAKIOS_LOG("  [+0x%03tx] %-36s (%-12s) = %p (%s)",
                       offset, name ? name : "?", type, ptr_val,
                       fieldCls ? class_getName(fieldCls) : "unknown");
        } else {
            HAKIOS_LOG("  [+0x%03tx] %-36s (%-12s) = %p",
                       offset, name ? name : "?", type ? type : "?", ptr_val);
        }
    }
    if (ivars) free(ivars);
}

// ----------------------------------------------------------------------------
// 2. Evrensel Fallback Stub Sınıfı (HakiosHIDStubObject)
//    Nil kalabilecek ivar/getter'lar çağrıldığında backboardd'nin çökmesini önler.
// ----------------------------------------------------------------------------
static id hakios_stub_catchall(id self, SEL _cmd) {
    const char *selName = sel_getName(_cmd);
    // Sık çağrılan description veya hash metodları dışında log bas
    if (strcmp(selName, "description") == 0) {
        return (id)CFSTR("<HakiosHIDStubObject>");
    }
    HAKIOS_LOG("[StubObject %p] handled selector: %s", self, selName);
    return nil;
}

static BOOL hakios_stub_resolveInstanceMethod(id self, SEL _cmd, SEL name) {
    class_addMethod((Class)self, name, (IMP)hakios_stub_catchall, "@@:");
    return YES;
}

static id hakios_create_stub_object(const char *roleName) {
    static Class sStubClass = Nil;
    static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&sLock);
    if (!sStubClass) {
        Class nsObj = objc_getClass("NSObject");
        sStubClass = objc_allocateClassPair(nsObj, "HakiosHIDStubObject", 0);
        if (sStubClass) {
            Class meta = object_getClass((id)sStubClass);
            class_addMethod(meta, sel_registerName("resolveInstanceMethod:"),
                            (IMP)hakios_stub_resolveInstanceMethod, "c@::");
            objc_registerClassPair(sStubClass);
        }
    }
    pthread_mutex_unlock(&sLock);

    id inst = class_createInstance(sStubClass, 0);
    HAKIOS_LOG("Created HakiosHIDStubObject %p for role '%s'", inst, roleName);
    return inst;
}

// Belirli bir nesnedeki nil olan nesne tipi (@) ivar'ına güvenli stub enjekte etme
static void hakios_ensure_ivar_nonnull(id obj, const char *ivarName, const char *roleName) {
    if (!obj || !ivarName) return;
    Ivar ivar = class_getInstanceVariable(object_getClass(obj), ivarName);
    if (!ivar) return;
    ptrdiff_t offset = ivar_getOffset(ivar);
    id *slot = (id *)((uintptr_t)obj + offset);
    if (*slot == nil) {
        *slot = hakios_create_stub_object(roleName);
        HAKIOS_LOG("Populated nil ivar '%s' (+0x%tx) on %p with stub %p",
                   ivarName, offset, obj, *slot);
    }
}

// ----------------------------------------------------------------------------
// 3. BKHIDSystemInterface ve BKHIDSystem Hook İmplementasyonları
// ----------------------------------------------------------------------------
static IMP orig_BKHID_startHIDSystem = NULL;
static IMP orig_BKHID_startEventRouting = NULL;
static IMP orig_BKHID_startEventProcessing = NULL;
static IMP orig_BKHID_startServer = NULL;
static IMP orig_BKHID_registerWatchdog = NULL;
static IMP orig_BKHID_deliveryManager = NULL;
static IMP orig_BKHID_senderCache = NULL;
static IMP orig_BKHID_clientConnectionManager = NULL;

static IMP orig_SubSys_startHIDSystem = NULL;

static void hooked_BKHID_startHIDSystem(id self, SEL _cmd) {
    HAKIOS_LOG(">>> -[BKHIDSystemInterface startHIDSystem] ENTER (self=%p)", self);

    // 1. BKHIDSystemInterface ivar'larını dök
    hakios_dump_ivars(self, "BKHIDSystemInterface (BEFORE startHIDSystem)");

    // 2. İçteki BKHIDSystem (_system) nesnesini bul ve ivar'larını dök
    id innerSystem = nil;
    SEL sysSel = sel_registerName("system");
    if ([self respondsToSelector:sysSel]) {
        innerSystem = ((id (*)(id, SEL))objc_msgSend)(self, sysSel);
        hakios_dump_ivars(innerSystem, "BKHIDSystem (self.system)");
    }

#if HAKIOS_BYPASS_HID_START
    HAKIOS_LOG("-[BKHIDSystemInterface startHIDSystem] BYPASS ACTIVE -> Skipping IOHIDEventSystemOpen hang!");

    // Eğer iç sistemde veya arayüzde kritik ivar'lar nil ise stub ile doldur
    hakios_ensure_ivar_nonnull(self, "_deliveryManager", "BKHIDEventDeliveryManager");
    hakios_ensure_ivar_nonnull(self, "_senderCache", "BKHIDEventSenderCache");
    hakios_ensure_ivar_nonnull(self, "_clientConnectionManager", "BKHIDClientConnectionManager");
    if (innerSystem) {
        hakios_ensure_ivar_nonnull(innerSystem, "_deliveryManager", "BKHIDEventDeliveryManager");
        hakios_ensure_ivar_nonnull(innerSystem, "_senderCache", "BKHIDEventSenderCache");
        hakios_ensure_ivar_nonnull(innerSystem, "_clientConnectionManager", "BKHIDClientConnectionManager");
    }
#else
    HAKIOS_LOG("-[BKHIDSystemInterface startHIDSystem] Calling original implementation...");
    ((void (*)(id, SEL))orig_BKHID_startHIDSystem)(self, _cmd);
#endif

    HAKIOS_LOG("<<< -[BKHIDSystemInterface startHIDSystem] EXIT");
}

static void hooked_BKHID_startEventRouting(id self, SEL _cmd) {
    HAKIOS_LOG(">>> -[BKHIDSystemInterface startEventRouting] ENTER");
#if HAKIOS_BYPASS_HID_START
    HAKIOS_LOG("<<< -[BKHIDSystemInterface startEventRouting] BYPASSED");
#else
    ((void (*)(id, SEL))orig_BKHID_startEventRouting)(self, _cmd);
    HAKIOS_LOG("<<< -[BKHIDSystemInterface startEventRouting] EXIT");
#endif
}

static void hooked_BKHID_startEventProcessing(id self, SEL _cmd) {
    HAKIOS_LOG(">>> -[BKHIDSystemInterface startEventProcessing] ENTER");
#if HAKIOS_BYPASS_HID_START
    HAKIOS_LOG("<<< -[BKHIDSystemInterface startEventProcessing] BYPASSED");
#else
    ((void (*)(id, SEL))orig_BKHID_startEventProcessing)(self, _cmd);
    HAKIOS_LOG("<<< -[BKHIDSystemInterface startEventProcessing] EXIT");
#endif
}

static void hooked_BKHID_startServer(id self, SEL _cmd) {
    HAKIOS_LOG(">>> -[BKHIDSystemInterface startServer] ENTER");
#if HAKIOS_BYPASS_HID_START
    // NOT: Eğer SpringBoard com.apple.iohideventsystem Mach portunda bloklanırsa,
    // sadece startHIDSystem'i bypass edip orig_BKHID_startServer çağrısını açabilirsiniz.
    HAKIOS_LOG("<<< -[BKHIDSystemInterface startServer] BYPASSED");
#else
    ((void (*)(id, SEL))orig_BKHID_startServer)(self, _cmd);
    HAKIOS_LOG("<<< -[BKHIDSystemInterface startServer] EXIT");
#endif
}

static void hooked_BKHID_registerWatchdog(id self, SEL _cmd) {
    // Watchdog kaydını her durumda devre dışı bırakıyoruz
    HAKIOS_LOG("-[BKHIDSystemInterface registerWatchdog] NOP (Watchdog disabled)");
}

// Getter Korumaları
static id hooked_BKHID_deliveryManager(id self, SEL _cmd) {
    id ret = orig_BKHID_deliveryManager
                 ? ((id (*)(id, SEL))orig_BKHID_deliveryManager)(self, _cmd)
                 : nil;
    if (!ret) {
        static id sFallback = nil;
        if (!sFallback) sFallback = hakios_create_stub_object("BKHIDEventDeliveryManager");
        ret = sFallback;
    }
    return ret;
}

static id hooked_BKHID_senderCache(id self, SEL _cmd) {
    id ret = orig_BKHID_senderCache
                 ? ((id (*)(id, SEL))orig_BKHID_senderCache)(self, _cmd)
                 : nil;
    if (!ret) {
        static id sFallback = nil;
        if (!sFallback) sFallback = hakios_create_stub_object("BKHIDEventSenderCache");
        ret = sFallback;
    }
    return ret;
}

static id hooked_BKHID_clientConnectionManager(id self, SEL _cmd) {
    id ret = orig_BKHID_clientConnectionManager
                 ? ((id (*)(id, SEL))orig_BKHID_clientConnectionManager)(self, _cmd)
                 : nil;
    HAKIOS_LOG("-[BKHIDSystemInterface clientConnectionManager] -> %p", ret);
    if (!ret) {
        static id sFallback = nil;
        if (!sFallback) sFallback = hakios_create_stub_object("BKHIDClientConnectionManager");
        ret = sFallback;
    }
    return ret;
}

// Alt katman BKHIDSystem (BackBoardHIDEventFoundation) hook'u
static void hooked_SubSys_startHIDSystem(id self, SEL _cmd) {
    HAKIOS_LOG(">>> -[BKHIDSystem startHIDSystem] ENTER (self=%p)", self);
#if HAKIOS_BYPASS_HID_START
    HAKIOS_LOG("<<< -[BKHIDSystem startHIDSystem] BYPASSED");
#else
    ((void (*)(id, SEL))orig_SubSys_startHIDSystem)(self, _cmd);
    HAKIOS_LOG("<<< -[BKHIDSystem startHIDSystem] EXIT");
#endif
}

// ----------------------------------------------------------------------------
// 4. /dev/hakios-touch -> -[BKHIDSystemInterface injectHIDEvent:] Köprüsü
// ----------------------------------------------------------------------------
typedef void *IOHIDEventRef;
typedef uint32_t IOHIDDigitizerTransducerType;
typedef uint32_t IOHIDDigitizerEventMask;
typedef uint32_t IOOptionBits;

// IOKit.framework içindeki dinamik sembol tipleri
typedef IOHIDEventRef (*fn_IOHIDEventCreateDigitizerEvent)(
    CFAllocatorRef allocator, uint64_t timeStamp, IOHIDDigitizerTransducerType type,
    uint32_t index, uint32_t identity, IOHIDDigitizerEventMask eventMask, uint32_t buttonMask,
    double x, double y, double z, double tipPressure, double barrelPressure,
    Boolean range, Boolean touch, IOOptionBits options);

typedef IOHIDEventRef (*fn_IOHIDEventCreateDigitizerFingerEvent)(
    CFAllocatorRef allocator, uint64_t timeStamp, uint32_t index, uint32_t identity,
    IOHIDDigitizerEventMask eventMask, double x, double y, double z,
    double tipPressure, double twist, Boolean range, Boolean touch, IOOptionBits options);

typedef void (*fn_IOHIDEventAppendEvent)(IOHIDEventRef event, IOHIDEventRef childEvent, IOOptionBits options);

// /dev/hakios-touch üzerinden okunacak örnek paket yapısı (kendi sürücünüze göre uyarlayın)
struct hakios_touch_packet {
    uint16_t x;       // 0..1080
    uint16_t y;       // 0..1920
    uint8_t  touching; // 1 = basılı, 0 = bırakıldı
    uint8_t  reserved[3];
};

static void *hakios_touch_bridge_thread(void *arg) {
    (void)arg;
    // WindowServer'ın ayağa kalkması için kısa bekleme
    sleep(5);

    int fd = open("/dev/hakios-touch", O_RDONLY);
    if (fd < 0) {
        HAKIOS_LOG("[TouchBridge] /dev/hakios-touch acilamadi, thread sonlandiriliyor.");
        return NULL;
    }

    fn_IOHIDEventCreateDigitizerEvent pCreateDigitizer =
        (fn_IOHIDEventCreateDigitizerEvent)dlsym(RTLD_DEFAULT, "IOHIDEventCreateDigitizerEvent");
    fn_IOHIDEventCreateDigitizerFingerEvent pCreateFinger =
        (fn_IOHIDEventCreateDigitizerFingerEvent)dlsym(RTLD_DEFAULT, "IOHIDEventCreateDigitizerFingerEvent");
    fn_IOHIDEventAppendEvent pAppendEvent =
        (fn_IOHIDEventAppendEvent)dlsym(RTLD_DEFAULT, "IOHIDEventAppendEvent");

    if (!pCreateDigitizer || !pCreateFinger || !pAppendEvent) {
        HAKIOS_LOG("[TouchBridge] IOHIDEvent sembolleri bulunamadi!");
        close(fd);
        return NULL;
    }

    Class bkCls = objc_getClass("BKHIDSystemInterface");
    SEL sharedSel = sel_registerName("sharedInstance");
    SEL injectSel = sel_registerName("injectHIDEvent:");

    HAKIOS_LOG("[TouchBridge] /dev/hakios-touch dinleniyor -> injectHIDEvent: aktif!");

    struct hakios_touch_packet pkt;
    bool prev_touch = false;

    while (read(fd, &pkt, sizeof(pkt)) == sizeof(pkt)) {
        double normX = (double)pkt.x / 1080.0;
        double normY = (double)pkt.y / 1920.0;
        bool isTouch = (pkt.touching != 0);

        // Event mask: kIOHIDDigitizerEventTouch (0x02) | kIOHIDDigitizerEventPosition (0x04) | kIOHIDDigitizerEventRange (0x01)
        uint32_t mask = (isTouch != prev_touch) ? (0x01 | 0x02) : 0x04;
        prev_touch = isTouch;

        uint64_t ts = mach_absolute_time();
        // Hand (Parent) Event: kIOHIDDigitizerTransducerTypeHand = 3
        IOHIDEventRef handEvent = pCreateDigitizer(
            kCFAllocatorDefault, ts, 3, 0, 0, mask, 0,
            normX, normY, 0.0, isTouch ? 1.0 : 0.0, 0.0,
            isTouch, isTouch, 0);

        if (handEvent) {
            IOHIDEventRef fingerEvent = pCreateFinger(
                kCFAllocatorDefault, ts, 1, 2, mask,
                normX, normY, 0.0, isTouch ? 1.0 : 0.0, 0.0,
                isTouch, isTouch, 0);
            if (fingerEvent) {
                pAppendEvent(handEvent, fingerEvent, 0);
                CFRelease(fingerEvent);
            }

            id hidInterface = ((id (*)(id, SEL))objc_msgSend)((id)bkCls, sharedSel);
            if (hidInterface) {
                ((void (*)(id, SEL, IOHIDEventRef))objc_msgSend)(hidInterface, injectSel, handEvent);
            }
            CFRelease(handEvent);
        }
    }

    close(fd);
    return NULL;
}

// ----------------------------------------------------------------------------
// 5. Kurulum Fonksiyonu (libhakios.dylib __attribute__((constructor)) içinden çağrılır)
// ----------------------------------------------------------------------------
static void hook_instance_method(Class cls, const char *selName, IMP newImp, IMP *origOut) {
    SEL sel = sel_registerName(selName);
    Method m = class_getInstanceMethod(cls, sel);
    if (!m) {
        HAKIOS_LOG("WARN: Method -[%s %s] bulunamadi!", class_getName(cls), selName);
        return;
    }
    *origOut = method_setImplementation(m, newImp);
    HAKIOS_LOG("Hooked -[%s %s] (orig=%p -> new=%p)",
               class_getName(cls), selName, *origOut, newImp);
}

__attribute__((constructor))
static void hakios_hid_init(void) {
    Class bkInterfaceCls = objc_getClass("BKHIDSystemInterface");
    if (!bkInterfaceCls) {
        // Sadece backboardd içinde BKHIDSystemInterface bulunur
        return;
    }

    HAKIOS_LOG("Installing BKHIDSystemInterface & BKHIDSystem hooks in backboardd...");

    hook_instance_method(bkInterfaceCls, "startHIDSystem",
                         (IMP)hooked_BKHID_startHIDSystem, &orig_BKHID_startHIDSystem);
    hook_instance_method(bkInterfaceCls, "startEventRouting",
                         (IMP)hooked_BKHID_startEventRouting, &orig_BKHID_startEventRouting);
    hook_instance_method(bkInterfaceCls, "startEventProcessing",
                         (IMP)hooked_BKHID_startEventProcessing, &orig_BKHID_startEventProcessing);
    hook_instance_method(bkInterfaceCls, "startServer",
                         (IMP)hooked_BKHID_startServer, &orig_BKHID_startServer);
    hook_instance_method(bkInterfaceCls, "registerWatchdog",
                         (IMP)hooked_BKHID_registerWatchdog, &orig_BKHID_registerWatchdog);

    hook_instance_method(bkInterfaceCls, "deliveryManager",
                         (IMP)hooked_BKHID_deliveryManager, &orig_BKHID_deliveryManager);
    hook_instance_method(bkInterfaceCls, "senderCache",
                         (IMP)hooked_BKHID_senderCache, &orig_BKHID_senderCache);
    hook_instance_method(bkInterfaceCls, "clientConnectionManager",
                         (IMP)hooked_BKHID_clientConnectionManager, &orig_BKHID_clientConnectionManager);

    Class bkSubSysCls = objc_getClass("BKHIDSystem");
    if (bkSubSysCls) {
        hook_instance_method(bkSubSysCls, "startHIDSystem",
                             (IMP)hooked_SubSys_startHIDSystem, &orig_SubSys_startHIDSystem);
    }

    // Arka plan dokunmatik köprüsü thread'ini başlat
    pthread_t tid;
    pthread_create(&tid, NULL, hakios_touch_bridge_thread, NULL);
    pthread_detach(tid);
}
```

## 4. Çözüm 2: XNU Çekirdek Seviyesinde `IOHIDSystem` ve `IOUserClient` Stub Sürücüsü

`backboardd` içindeki `IOHIDEventSystemOpen` çağrısının, `startEventRouting`, `startEventProcessing` ve `startServer` adımlarının hiçbir kullanıcı alanı bypass'ına gerek kalmadan doğal akışıyla çalışmasını istiyorsanız, XNU çekirdeğine (tıpkı `MiniIOMobileFramebuffer` ve `Hakios WDT: IOWatchdog` sürücülerinizde olduğu gibi) minimal bir **`IOHIDSystem`** `IOService` ve `IOUserClient` stub'ı ekleyebilirsiniz.

### `IOHIDSystem` Bağlantı Tipleri (`IOServiceOpen` `type` Parametresi)

Açık kaynak [IOHIDSystem.cpp](https://github.com/aosm/IOHIDFamily/blob/master/IOHIDSystem/IOHIDSystem.cpp), [Apple IOKit dokümantasyonu](https://developer.apple.com/documentation/iokit/1514178-anonymous/kiohidserverconnecttype) ve [Siguza'nın IOHIDeous analizi](https://blog.siguza.net/IOHIDeous/) uyarınca `IOHIDSystem::newUserClient` metoduna gelen `type` değerleri şunlardır:

| Bağlantı Sabiti (`IOHIDUserClient.h`) | `type` Değeri | Kernel UserClient Sınıfı | Açıklama ve Kullanım Yeri |
| :--- | :--- | :--- | :--- |
| `kIOHIDServerConnectType` | `0` | `IOHIDUserClient` | Klasik sunucu bağlantısı; `EvOffsets` ve `EvGlobals` paylaşımlı belleğini (`clientMemoryForType`) haritalar. |
| `kIOHIDParamConnectType` | `1` | `IOHIDParamUserClient` | Sistem seviyesi HID parametrelerini okuma/yazma (`IOHIDGetParameter` / `IOHIDSetParameter`) için kullanılır. |
| `kIOHIDEventSystemConnectType` | `3` | `IOHIDEventSystemUserClient` | **iOS `backboardd` (`IOHIDEventSystemOpen`) tarafından kullanılan ana bağlantı tipidir.** |

### `IOHIDSystem` UserClient External Method Selector Tablosu

[CVE-2016-1824 IOHIDEventSystemUserClient analizi](https://marcograss.github.io/security/apple/cve/2016/05/16/cve-2016-1824-apple-iohidfamily-racecondition.html) ve [IOHIDeous](https://blog.siguza.net/IOHIDeous/) kaynaklarına göre kullanıcı alanından `IOConnectCallMethod` ile çağrılan selector numaraları:

| UserClient `type` | Selector İndeksi | Metod Adı | Giriş / Çıkış Yapısı (`IOExternalMethodArguments`) | Stub Çekirdek Davranışı |
| :--- | :--- | :--- | :--- | :--- |
| **`3` (`EventSystem`)** | **`0`** | `createEventQueue` | Scalar In: `2` (`type`, `size`) \| Scalar Out: `1` (`queueID`) | `IOSharedDataQueue` oluşturur, `args->scalarOutput[0] = 1` (`queueID`) atar ve `kIOReturnSuccess` döner. |
| **`3` (`EventSystem`)** | **`1`** | `destroyEventQueue` | Scalar In: `2` (`type`, `queueID`) \| Scalar Out: `0` | Kuyruğu temizler ve `kIOReturnSuccess` döner. |
| **`3` (`EventSystem`)** | **`2`** | `tickle` | Scalar In: `1` (`activityType`) \| Scalar Out: `0` | Kullanıcı etkileşimi ile ekran uykusunu erteleme bildirimi; `kIOReturnSuccess` döner. |
| **`0` (`Server`)** | **`0`** | `createShmem` | Scalar In: `1` (`version`) \| Scalar Out: `0` | `EvOffsets` + `EvGlobals` paylaşımlı bellek bölgesini sıfırlar ve `kIOReturnSuccess` döner. |
| **`0` (`Server`)** | **`1`** | `releaseShmem` | Scalar In: `0` \| Scalar Out: `0` | `kIOReturnSuccess` döner. |
| **`0` (`Server`)** | **`2`** | `setEventEnable` | Scalar In: `1` (`enable`) \| Scalar Out: `0` | `kIOReturnSuccess` döner. |
| **`0` (`Server`)** | **`3` .. `14`** | `setCursorEnable`, `postEvent`, `setBounds` vb. | Çeşitli Scalar/Struct girişler | Tümüne `kIOReturnSuccess` döner. |

### XNU Çekirdek İçi `IOHIDSystem` + `HakiosHIDUserClient` C++ Kodu

Bu kodu XNU çekirdek ağacınızdaki `MiniIOMobileFramebuffer` / `HakiosBattery` sürücülerinizin yanına ekleyip başlatma sırasında (`hakios_hid_stub_init`) kaydedebilirsiniz:

```cpp
// ============================================================================
// XNU Kernel - Minimal IOHIDSystem & IOHIDEventSystemUserClient Stub Driver
// ============================================================================

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOSharedDataQueue.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>

// EvOffsets yapısı (kIOHIDServerConnectType = 0 paylaşımlı bellek başlığı)
struct HakiosEvOffsets {
    int evGlobalsOffset; // EvGlobals başlangıç ofseti
    int evShmemOffset;   // Özel shmem başlangıç ofseti
};

class IOHIDSystem : public IOService {
    OSDeclareDefaultStructors(IOHIDSystem)
public:
    virtual bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual IOReturn newUserClient(task_t owningTask, void *securityID,
                                   UInt32 type, OSDictionary *properties,
                                   IOUserClient **handler) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};

class HakiosHIDUserClient : public IOUserClient {
    OSDeclareDefaultStructors(HakiosHIDUserClient)
private:
    task_t fTask;
    UInt32 fConnectType;
    IOSharedDataQueue *fEventQueue;
    IOBufferMemoryDescriptor *fServerShmem;
public:
    bool initWithTaskAndType(task_t owningTask, UInt32 type);
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    virtual IOReturn externalMethod(uint32_t selector,
                                    IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target,
                                    void *reference) APPLE_KEXT_OVERRIDE;
    virtual IOReturn clientMemoryForType(UInt32 type,
                                         IOOptionBits *options,
                                         IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    virtual IOReturn registerNotificationPort(mach_port_t port,
                                              UInt32 type,
                                              UInt32 refCon) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE {
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(IOHIDSystem, IOService)
OSDefineMetaClassAndStructors(HakiosHIDUserClient, IOUserClient)

bool IOHIDSystem::init(OSDictionary *dictionary) {
    if (!IOService::init(dictionary)) return false;
    return true;
}

bool IOHIDSystem::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    setName("IOHIDSystem");
    setProperty("IOClass", "IOHIDSystem");
    setProperty("IOResourceMatch", "IOBSD");
    setProperty("HIDPowerOnDelayNS", 500000000ULL, 64);

    registerService();
    IOLog("HakiosHID: IOHIDSystem kernel stub registered and active!\n");
    return true;
}

IOReturn IOHIDSystem::setProperties(OSObject *properties) {
    // IOHIDSetParameter / IORegistryEntrySetCFProperties çağrılarının hata dönmemesi için
    if (OSDictionary *dict = OSDynamicCast(OSDictionary, properties)) {
        OSCollectionIterator *iter = OSCollectionIterator::withCollection(dict);
        if (iter) {
            const OSSymbol *key;
            while ((key = OSDynamicCast(OSSymbol, iter->getNextObject()))) {
                setProperty(key, dict->getObject(key));
            }
            iter->release();
        }
    }
    return kIOReturnSuccess;
}

IOReturn IOHIDSystem::newUserClient(task_t owningTask, void *securityID,
                                    UInt32 type, OSDictionary *properties,
                                    IOUserClient **handler) {
    (void)securityID;
    (void)properties;

    HakiosHIDUserClient *client = new HakiosHIDUserClient;
    if (!client || !client->initWithTaskAndType(owningTask, type)) {
        if (client) client->release();
        return kIOReturnNoMemory;
    }
    if (!client->attach(this) || !client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }

    *handler = client;
    IOLog("HakiosHID: newUserClient opened (type=%u)\n", (unsigned)type);
    return kIOReturnSuccess;
}

bool HakiosHIDUserClient::initWithTaskAndType(task_t owningTask, UInt32 type) {
    if (!IOUserClient::initWithTask(owningTask, NULL, 0, NULL)) return false;
    fTask = owningTask;
    fConnectType = type;

    // Type 3 (kIOHIDEventSystemConnectType) için 32 KB IOSharedDataQueue hazırla
    fEventQueue = IOSharedDataQueue::withCapacity(32768);

    // Type 0 (kIOHIDServerConnectType) için 32 KB EvOffsets + EvGlobals paylaşımlı bellek hazırla
    fServerShmem = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIOMemoryKernelUserShared | kIODirectionInOut, 32768, page_size);
    if (fServerShmem) {
        void *buf = fServerShmem->getBytesNoCopy();
        bzero(buf, 32768);
        struct HakiosEvOffsets *offsets = (struct HakiosEvOffsets *)buf;
        offsets->evGlobalsOffset = sizeof(struct HakiosEvOffsets);
        offsets->evShmemOffset   = 0x4000;
    }
    return true;
}

void HakiosHIDUserClient::free() {
    if (fEventQueue) {
        fEventQueue->release();
        fEventQueue = NULL;
    }
    if (fServerShmem) {
        fServerShmem->release();
        fServerShmem = NULL;
    }
    IOUserClient::free();
}

IOReturn HakiosHIDUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn HakiosHIDUserClient::externalMethod(uint32_t selector,
                                             IOExternalMethodArguments *args,
                                             IOExternalMethodDispatch *dispatch,
                                             OSObject *target,
                                             void *reference) {
    (void)dispatch;
    (void)target;
    (void)reference;

    IOLog("HakiosHID: externalMethod(connType=%u, selector=%u, scalarIn=%u, scalarOut=%u)\n",
          (unsigned)fConnectType, selector,
          args ? args->scalarInputCount : 0,
          args ? args->scalarOutputCount : 0);

    if (fConnectType == 3) { // kIOHIDEventSystemConnectType (IOHIDEventSystemUserClient)
        switch (selector) {
            case 0: // createEventQueue(type, size) -> queueID
                if (args && args->scalarOutputCount >= 1) {
                    args->scalarOutput[0] = 1; // Geçerli queueID = 1 dön
                }
                return kIOReturnSuccess;
            case 1: // destroyEventQueue(type, queueID)
            case 2: // tickle(activityType)
            default:
                return kIOReturnSuccess;
        }
    }

    // kIOHIDServerConnectType (0) ve kIOHIDParamConnectType (1) için tüm selector'lara başarı dön
    if (args && args->scalarOutputCount >= 1) {
        args->scalarOutput[0] = 0;
    }
    return kIOReturnSuccess;
}

IOReturn HakiosHIDUserClient::clientMemoryForType(UInt32 type,
                                                  IOOptionBits *options,
                                                  IOMemoryDescriptor **memory) {
    (void)options;
    IOLog("HakiosHID: clientMemoryForType(connType=%u, memType=%u)\n",
          (unsigned)fConnectType, (unsigned)type);

    if (fConnectType == 3 && fEventQueue) {
        IOMemoryDescriptor *qMem = fEventQueue->getMemoryDescriptor();
        if (qMem) {
            *memory = qMem; // getMemoryDescriptor() zaten retain edilmiş nesne döner
            return kIOReturnSuccess;
        }
    }

    if (fServerShmem) {
        fServerShmem->retain();
        *memory = fServerShmem;
        return kIOReturnSuccess;
    }

    return kIOReturnNoMemory;
}

IOReturn HakiosHIDUserClient::registerNotificationPort(mach_port_t port,
                                                       UInt32 type,
                                                       UInt32 refCon) {
    (void)refCon;
    IOLog("HakiosHID: registerNotificationPort(connType=%u, type=%u)\n",
          (unsigned)fConnectType, (unsigned)type);
    if (fEventQueue) {
        fEventQueue->setNotificationPort(port);
    }
    return kIOReturnSuccess;
}

// Bringup başlatma fonksiyonu (hakios_virtio_input_init veya hakios_buttons_init içinden çağrılabilir)
extern "C" void hakios_iohid_stub_init(void) {
    IOService *root = IOService::getPlatform();
    if (!root) return;
    IOHIDSystem *hidSys = new IOHIDSystem;
    if (hidSys) {
        if (hidSys->init() && hidSys->attach(root)) {
            hidSys->start(root);
        } else {
            hidSys->release();
        }
    }
}
```

## 5. Uygulama ve Doğrulama Yol Haritası

1. **Adım 1 — Dylib Bypass ile `window server` Doğrulaması (`libhakios.dylib`):**
   - Bölüm 3'teki `inject.c` kodunu `HAKIOS_BYPASS_HID_START 1` olarak derleyip `/redwood/lib/libhakios.dylib` içine ekleyin.
   - Cihazı önyüklediğinizde `/dev/console` üzerinde `-[BKHIDSystemInterface startHIDSystem] BYPASS ACTIVE` logunu, `BKHIDSystemInterface` ve `BKHIDSystem` ivar dökümünü (`[IVAR-DUMP]`) ve hemen ardından `backboardd`'nin **4. adım (`GS`)**, **5. adım (`window server`)** ve **6. adım (`HID connection manager`)** aşamalarına geçtiğini kontrol edin.
2. **Adım 2 — Ivar Dökümü ve `startServer` Kontrolü:**
   - Konsola basılan `[IVAR-DUMP]` tablosunda `BKHIDSystemInterface` ve `BKHIDSystem` içindeki ivar'ların (`_deliveryManager`, `_senderCache`, `_clientConnectionManager`, `_eventSystem`) hangilerinin `init` sonrasında dolu olduğunu inceleyin.
   - Eğer `SpringBoard` (PID 7) `com.apple.iohideventsystem` Mach portuna bağlanmak için bekliyorsa, `hooked_BKHID_startServer` içindeki bypass'ı kaldırıp yalnızca `startHIDSystem` metodunu bypasslı bırakarak `orig_BKHID_startServer` çağrısının çalışmasını sağlayın.
3. **Adım 3 — Çekirdek `IOHIDSystem` Stub ve Dokunmatik Enjeksiyonu:**
   - Bölüm 4'teki `IOHIDSystem` + `HakiosHIDUserClient` C++ sınıfını XNU çekirdeğine derleyip `HAKIOS_BYPASS_HID_START 0` yaptığınızda `IOHIDEventSystemOpen` çağrısının kernelde `HakiosHID: newUserClient opened (type=3)` ve `externalMethod(connType=3, selector=0)` loglarını basarak doğal yoldan tamamlandığını doğrulayın.
   - Ekran (`CA::WindowServer` -> `MiniIOMobileFramebuffer`) görüntüyü çizdiğinde `/dev/hakios-touch` köprü thread'i üzerinden parmak koordinatlarını `-[BKHIDSystemInterface injectHIDEvent:]` ile enjekte ederek Lock Screen üzerinde dokunmatik tepkisini test edin.
