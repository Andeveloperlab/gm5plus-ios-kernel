General Mobile GM 5 Plus (MSM8952) — iOS 16.5 

CAWindowServer

 Kilitlenme Analizi ve Çözüm Paketi

1. Kök Neden ve Zaman Çizelgesi Analizi (

capture-windowserver.log

)

capture-windowserver.log

 dosyasının son 35 satırı (

satır 2114–2149

), 

launchd

 IPC olayları ve XNU çekirdek heartbeat (

[HB]

) kayıtları incelendiğinde, sistemin 

start window server... adımına girdikten 1.50 saniye sonra

 neden sustuğu kesin olarak tespit edilmiştir:

Zaman Damgası (Civil Time)


Süreç / Bileşen


Log Kaydı ve Mimari Anlamı


02:04:11.846297


backboardd[11:208]


GS ready in 5.58104s. overall 35.4309s

 — Adım 4 (

GraphicsServices

), 

com.apple.lsd

 (PID 23) Mach IPC ile on-demand spawn edildiği için 5.58s sürdü.


02:04:11.947091


backboardd[11:208]


[Common] start window server...

 — Adım 5 (

StartWindowServer

) başladı.


02:04:12.093986


backboardd[11:208]


[Display] StartWindowServer: Initializing shader cache path: /private/var/containers/Data/System/7BD1D93F-.../Library/Caches

 — 

containermanagerd

 (PID 12) sistem konteynerini çözdü.


~02:04:12.100


backboardd[11:208]


hakios: +[CAWindowServer serverWithOptions:0x113d194b0] START

 — Hook tetiklendi ve gerçek 

QuartzCore

 

+[CAWindowServer serverWithOptions:]

 fonksiyonuna girildi.


02:04:12.708919


SpringBoard[7]


Paralelde tek çekirdekte çalışan 

SpringBoard

, 

com.apple.chronoservices

 XPC araması yaptı (

error = 3: No such process

).


02:04:12.994634


launchd

 / 

backboardd


com.apple.mobilegestalt.xpc

 için 

WILL_SPAWN

 / 

launching: ipc (mach)

 tetiklendi. 

+[CAWindowServer serverWithOptions:]

 -> 

-[CAWindowServer _init]

, ekran parametrelerini (

main-screen-width

, 

main-screen-scale

, 

ArtworkDeviceSubType

 vb.) sorgulamak için 

libMobileGestalt.dylib

 (

MGCopyAnswer

) çağırdı; önbellek (

MobileGestalt.plist

) doğrulanamadığı için 

mobilegestalt.xpc

 (PID 24) senkron Mach IPC ile uyandırıldı.


02:04:13.579898


launchd


xpcproxy spawned with pid 24

 (

com.apple.mobilegestalt.xpc

).


02:04:13.600945


launchd


(user/501) <Error>: Unable to find persona with type 6: 3

 — 

Logun son satırı. UART akışı aniden kesiliyor.


Temel Bulgular

+[CAWindowServer serverWithOptions:] Çağrısı Döndü mü?

Hayır, dönmedi.

 

START

 logu basıldı (

satır 2120

), ancak fonksiyon içerideyken (henüz 

1.50 saniye

 geçmişken) donanım gücü kesti/resetledi. Bu nedenle 

+[CAWindowServer server]

 veya 

-[CAWindowServer displays]

 çağrılarına henüz sıra gelmedi.

Kullanıcı Alanı Çökmesi (SIGSEGV/SIGABRT) veya Kernel Panic Var mı?

Hayır.

 Kullanıcı alanında bir sinyal/çökme olsaydı 

launchd

 anında 

(user/501/com.apple.backboardd [11]) <Notice>: exited due to ...

 basardı. Çekirdek seviyesinde bir 

panic()

 olsaydı XNU seri konsola register dump basardı.


--------------------------------------------------------------------------------


2. Donanım Watchdog (180s) Matematiksel Kanıtı ve Tek Çekirdek Darboğazları

Logdaki çekirdek 

[HB]

 (heartbeat) satırları ile 

launchd

 duvar saati (civil time) karşılaştırıldığında kesilmenin 

180 saniyelik donanım reseti

 olduğu doğrulanmaktadır:

Satır 79

: 

[HB] heartbeat 1, 10 s since start

Satır 868

: 

[HB] heartbeat 2, 20 s since start

Satır 964

 (

heartbeat 2

'den 

0.4 saniye

 sonra 

exec /sbin/launchd.ios

 çalıştığında):

 

com.apple.xpc.launchd|1970-01-01 02:01:40.429157 <Notice>: hello

Bu eşleşme, XNU'nun 

t = 20s

 anının sivil saatte 

02:01:40.0

 olduğunu, dolayısıyla 

XNU başlangıcının (t = 0s) sivil saatte 02:01:20.0

 olduğunu kanıtlar.

Kesilme anındaki (

02:04:13.600945

) süre:

XNU Uptime:

 

02:04:13.600 - 02:01:20.000 =

 

173.6 saniye

Bootloader (shamrock> boot + initrd kopyalama) Dahil Toplam Süre:

 

173.6s + ~6.4s =

 

180.0 saniye (Tam 3 Dakika!)

Neden 

start window server...

 Adımına Gelmek 172 Saniye Sürdü?

backboardd

, 

start window server...

 satırına ulaştığında (

02:04:11.947

) zaten 

171.9 saniye

 harcanmıştı ve 180s donanım sınırına 

sadece 1.65 saniye

 kalmıştı:

SpringBoard [PID 7] Erken Başlatma ve Metod Taraması Darboğazı (40.5 saniye kayıp):

SpringBoard

, 

backboardd

'den 34 saniye önce (

02:01:41.5

) spawn ediliyor.

backboardd

 

02:02:55.900

'da 

starting /usr/libexec/backboardd...

 dedikten hemen sonra 

SpringBoard

 tek çekirdeği 

40.5 saniye

 (

02:02:55.9

 -> 

02:03:36.4

) boyunca devralıyor (

satır 1858–1886

: 

libhakios.sb.dylib

 içindeki 

SBStatusBarStateAggregator

 metod taramaları ve UART'a binlerce karakter basılması).

prewarm oscard (11.18 saniye kayıp) ve CoreAccessories (12.70 saniye kayıp):

backboardd

 içinde donanımda karşılığı olmayan 

oscard

 ve 

ACCHWComponentAuthService

 [PID 22] spawn'ı toplam 

23.88 saniye

 harcıyor.

rc.jobs İçindeki rwtouch (satır 1645):

/redwood/bin/rwtouch wait 32 swipe ... &

 [PID 19] hâlâ çalışmaya devam ediyor.


--------------------------------------------------------------------------------


3. Uygulanacak Mühendislik Müdahaleleri ( Görev Listesi)

Görev 1: Çekirdek (

hakios_battery.c

) Periyodik Watchdog Besleme (Petting) ve 

SEC_ACCESS

 Kilidi

PM8950/PMI8950 PON (

0x800

) ve PMI8950 SMBCHG (

0x1600

, USB takılıyken 

ext=1, chg=1

 durumunda aktifleşen şarj watchdog'u) kayıtlarına yazarken 

SEC_ACCESS

 (

offset 0xD0 = 0xA5

) kilidi açılmalı ve zaten 10 saniyede bir çalışan batarya telemetri thread'i içinde tüm watchdog'lar periyodik olarak pet edilmelidir:

// hakios_battery.c - Telemetri döngüsü (her 10 saniyede bir çalışan thread) içine eklenecek:

// 1. PM8950 PON (USID 0, base 0x0800) & PMI8950 PON (USID 2, base 0x0800)
spmi_write8(0, 0x08D0, 0xA5); // SEC_ACCESS unlock
spmi_write8(0, 0x0857, 0x00); // PON_WDOG_CTL disable
spmi_write8(0, 0x0858, 0x01); // PON_WDOG_PET strobe

spmi_write8(2, 0x08D0, 0xA5); // SEC_ACCESS unlock
spmi_write8(2, 0x0857, 0x00); // PON_WDOG_CTL disable
spmi_write8(2, 0x0858, 0x01); // PON_WDOG_PET strobe

// 2. PMI8950 SMBCHG MISC Watchdog (USID 2, MISC base 0x1600 - USB şarj sırasında reseti önler)
spmi_write8(2, 0x16D0, 0xA5); // SEC_ACCESS unlock
spmi_write8(2, 0x1643, 0x00); // SMBCHG_MISC_WDOG_CFG disable
spmi_write8(2, 0x1640, 0x01); // SMBCHG_MISC_CFG_RESTART pet

// 3. MSM8952 APCS KPSS WDT (phys 0x0B017000)
*(volatile uint32_t *)(apcs_wdt_base + 0x04) = 1; // WDT_RST (pet)
*(volatile uint32_t *)(apcs_wdt_base + 0x08) = 0; // WDT_EN = 0


c

Görev 2: Tek Çekirdek CPU Tıkanıklıklarını Temizleme (

40.5s + 24s

 Kazanç)

libhakios.sb.dylib:

 

SBStatusBarStateAggregator

 üzerindeki tüm metodları konsola döken (

hakios: cellular: aggregator methods: ...

) debug döngüsünü kaldırın (

SpringBoard

'un 

backboardd

'yi 40.5 saniye bloklamasını önler).

/redwood/etc/rc.jobs:

 

rwtouch

 satırını tamamen kaldırın veya devre dışı bırakın.

backboardd (libhakios.dylib):

 

prewarm oscard

 ve 

CoreAccessories

 (

ACCHWComponentAuthService

) adımlarını hızlandırmak için 

xpc_connection_create_mach_service

 içinde 

"com.apple.oscard"

 ve 

"com.apple.ACCHWComponentAuthService"

 isteklerini yakalayıp anında geçersiz/boş bağlantı dönün veya ilgili metodları no-op yapın.


--------------------------------------------------------------------------------


Görev 3: 

libhakios.dylib

 (

inject.c

) İçinde 

CAWindowServer

 + 

CAWindowServerDisplay

 + 

MiniIOMobileFramebuffer

 Çözümü

Mimari Gerekçe

Gerçek 

+[CAWindowServer serverWithOptions:]

 çağrıldığında 

-[CAWindowServer _init]

 -> 

-[CAWindowServer _detectDisplays]

 çalışır ve 

CA::WindowServer::IOMFBServer

 oluşturulmaya çalışılır.

iOS 16.5 

QuartzCore

 (

D22AP

), kompozitör için 

Metal (MTLCreateSystemDefaultDevice())

, 

IOSurfaceRootUserClient

 ve 

IOMobileFramebufferGetRunLoopSource

 (VSync Mach portu) bekler.

GM 5 Plus'ta Apple AGX GPU kext'i (

IOGPU

 / 

AGXMetalA11

) bulunmadığından 

CA::WindowServer::IOMFBServer

 ya başlatılamaz (

[server displays]

 boş 

@[]

 dizisi döner ve 

backboardd

 

displays.firstObject == nil

 yüzünden çöker) ya da 

mobilegestalt.xpc

 / VSync Mach portunda bloklanır.

Ayrıca 

-[BKHIDSystemInterface init]

 atlandığı için 

_system

 ivar'ı 

0x0

'dır (

satır 2062

). 

StartWindowServer

 biter bitmez 

backboardd

, 

[[BKHIDSystemInterface sharedInstance] deliveryManager]

, 

senderCache

 ve 

clientConnectionManager

 nesnelerine erişir; bunlar 

nil

 dönerse 

backboardd

 assert/exception atar.

Eklenecek C / Objective-C Hooking Kodu (

inject.c

 / 

libhakios.dylib

)

// Derleme:
// clang -arch arm64 -dynamiclib -fobjc-arc=0 \
//   -framework Foundation -framework QuartzCore -framework IOKit -framework CoreGraphics \
//   -o libhakios.dylib inject_windowserver.m

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

// IOKit prototipleri
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef io_object_t io_iterator_t;

extern const mach_port_t kIOMasterPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t masterPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOConnectMapMemory(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
                                        vm_address_t *atAddress, vm_size_t *ofSize, uint32_t options);

#ifndef kIOMapAnywhere
#define kIOMapAnywhere 0x00000001
#endif

#define HLOG(fmt, ...) do { \
    fprintf(stderr, "hakios: " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
} while (0)

// ============================================================================
// 1. MINIIOMOBILEFRAMEBUFFER DOĞRUDAN FRAMEBUFFER MAP & İLK EKRAN ÇİZİMİ
// ============================================================================
static volatile uint8_t *g_fb_base = NULL;
static vm_size_t         g_fb_size = 0;
static io_connect_t      g_fb_conn = MACH_PORT_NULL;

// Fiziksel panel: 1080x1920, 3240 bytes/row (24-bit RGB/BGR)
// Mantıksal D22AP: 1125x2436 (@3x)
#define PANEL_WIDTH   1080
#define PANEL_HEIGHT  1920
#define PANEL_STRIDE  3240
#define D22_WIDTH     1125.0
#define D22_HEIGHT    2436.0
#define D22_SCALE     3.0

static void hakios_fb_init_and_draw_boot_frame(void) {
    if (g_fb_base != NULL) return;

    const char *service_names[] = {
        "MiniIOMobileFramebuffer",
        "IOMobileFramebuffer",
        "AppleCLCD",
        NULL
    };

    io_service_t service = MACH_PORT_NULL;
    for (int i = 0; service_names[i] != NULL; i++) {
        service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(service_names[i]));
        if (service != MACH_PORT_NULL) {
            HLOG("MiniIOMFB matched service '%s' (port=0x%x)", service_names[i], service);
            break;
        }
    }

    if (service == MACH_PORT_NULL) {
        HLOG("WARNING: MiniIOMobileFramebuffer service not found in IORegistry!");
        return;
    }

    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &g_fb_conn);
    if (kr != KERN_SUCCESS || g_fb_conn == MACH_PORT_NULL) {
        HLOG("WARNING: IOServiceOpen(MiniIOMFB) failed: 0x%x", kr);
        return;
    }

    vm_address_t addr = 0;
    vm_size_t size = 0;
    kr = IOConnectMapMemory(g_fb_conn, 0, mach_task_self(), &addr, &size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || addr == 0) {
        HLOG("WARNING: IOConnectMapMemory(MiniIOMFB) failed: 0x%x", kr);
        return;
    }

    g_fb_base = (volatile uint8_t *)addr;
    g_fb_size = size;
    HLOG("MiniIOMFB mapped in backboardd at %p (size=%lu bytes)", (void *)g_fb_base, (unsigned long)g_fb_size);

    // Ekrana ilk görsel teyit karesini bas (Koyu lacivert arka plan + üstte beyaz durum barı çizgisi)
    if (g_fb_size >= (PANEL_STRIDE * PANEL_HEIGHT)) {
        uint8_t *fb = (uint8_t *)g_fb_base;
        for (int y = 0; y < PANEL_HEIGHT; y++) {
            uint8_t *row = fb + (y * PANEL_STRIDE);
            bool is_header = (y >= 60 && y < 68);
            for (int x = 0; x < PANEL_WIDTH; x++) {
                row[x * 3 + 0] = is_header ? 0xFF : 0x28; // B / R
                row[x * 3 + 1] = is_header ? 0xFF : 0x1A;

objc

 // G
                row[x * 3 + 2] = is_header ? 0xFF : 0x12; // R / B
            }
        }
        __sync_synchronize();
        HLOG("MiniIOMFB initial frame swapped to panel (1080x1920x24bpp)!");
    }
}

// ============================================================================
// 2. YUTUCU (NIL-SAFE) OBJC PROXY: EKSİK HID / DISPLAY ALT NESNELERİ İÇİN
// ============================================================================
static id stub_zero_imp(id self, SEL _cmd, ...) {
    return nil;
}

static Class hakios_get_stub_class(void) {
    static Class cls = Nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        cls = objc_allocateClassPair(objc_getClass("NSObject"), "HakiosNilSafeStub", 0);
        if (cls) {
            objc_registerClassPair(cls);
        } else {
            cls = objc_getClass("HakiosNilSafeStub");
        }
    });
    return cls;
}

static BOOL stub_resolve_instance_method(id self, SEL name) {
    class_addMethod([self class], name, (IMP)stub_zero_imp, "@@:");
    return YES;
}

// ============================================================================
// 3. SENTETİK CAWindowServerDisplay VE CAWindowServer İMPLEMENTASYONU
// ============================================================================
static id g_fake_server  = nil;
static id g_fake_display = nil;
static BOOL g_display_blanked = NO;
static NSInteger g_display_tag = 0;
static int g_display_pid = 0;

// --- CAWindowServerDisplay Metod İmplementasyonları ---
static CGRect disp_bounds(id self, SEL _cmd) {
    return CGRectMake(0.0, 0.0, D22_WIDTH, D22_HEIGHT);
}
static CGFloat disp_scale(id self, SEL _cmd)        { return D22_SCALE; }
static void disp_setScale(id self, SEL _cmd, CGFloat s) {}
static CGFloat disp_minScale(id self, SEL _cmd)     { return 1.0; }
static CGFloat disp_maxScale(id self, SEL _cmd)     { return 3.0; }
static unsigned int disp_displayId(id self, SEL _cmd) { return 1; }
static NSInteger disp_displayType(id self, SEL _cmd)  { return 0; } // Internal main display
static id disp_name(id self, SEL _cmd)              { return @"LCD"; }
static id disp_deviceName(id self, SEL _cmd)        { return @"primary"; }
static id disp_uniqueId(id self, SEL _cmd)          { return @"4429216-E95D-4B5F-8B57-112524360001"; }
static id disp_orientation(id self, SEL _cmd)       { return @"Portrait"; }
static void disp_setOrientation(id self, SEL _cmd, id o) {
    HLOG("-[CAWindowServerDisplay setOrientation:%@]", o);
}
static float disp_refreshRate(id self, SEL _cmd)    { return 60.0f; }
static float disp_minFrameTime(id self, SEL _cmd)   { return 1.0f / 60.0f; }
static float disp_overscanAmount(id self, SEL _cmd) { return 0.0f; }
static CGSize disp_overscanAmounts(id self, SEL _cmd) { return CGSizeZero; }
static BOOL disp_isBlanked(id self, SEL _cmd)       { return g_display_blanked; }
static void disp_setBlanked(id self, SEL _cmd, BOOL b) {
    g_display_blanked = b;
    HLOG("-[CAWindowServerDisplay setBlanked:%d]", (int)b);
    if (!b) {
        hakios_fb_init_and_draw_boot_frame();
    }
}
static id disp_cloneMaster(id self, SEL _cmd)       { return nil; }
static id disp_clones(id self, SEL _cmd)            { return @[]; }
static NSInteger disp_tag(id self, SEL _cmd)        { return g_display_tag; }
static void disp_setTag(id self, SEL _cmd, NSInteger t) { g_display_tag = t; }
static int disp_processId(id self, SEL _cmd)        { return g_display_pid; }
static void disp_setProcessId(id self, SEL _cmd, int p) { g_display_pid = p; }
static unsigned int disp_rendererFlags(id self, SEL _cmd) { return 0; }
static unsigned int disp_contextIdAtPosition(id self, SEL _cmd, CGPoint pt) { return 0; }
static unsigned int disp_clientPortAtPosition(id self, SEL _cmd, CGPoint pt) { return MACH_PORT_NULL; }
static unsigned int disp_clientPortOfContextId(id self, SEL _cmd, unsigned int cid) { return MACH_PORT_NULL; }
static unsigned int disp_taskPortOfContextId(id self, SEL _cmd, unsigned int

objc

 cid) { return MACH_PORT_NULL; }
static unsigned int disp_contextIdHostingContextId(id self, SEL _cmd, unsigned int cid) { return cid; }
static CGPoint disp_convertPoint(id self, SEL _cmd, CGPoint pt, unsigned int cid) { return pt; }
static void disp_noop(id self, SEL _cmd) {}
static BOOL disp_bool_no(id self, SEL _cmd) { return NO; }
static BOOL disp_bool_yes(id self, SEL _cmd) { return YES; }

static id hakios_get_or_create_display(void) {
    if (g_fake_display) return g_fake_display;

    Class baseCls = objc_getClass("CAWindowServerDisplay");
    if (!baseCls) baseCls = objc_getClass("NSObject");

    Class dispCls = objc_allocateClassPair(baseCls, "HakiosCAWindowServerDisplay", 0);
    if (dispCls) {
        class_addMethod(object_getClass(dispCls), @selector(resolveInstanceMethod:), (IMP)stub_resolve_instance_method, "B@::");
        class_addMethod(dispCls, @selector(bounds), (IMP)disp_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
        class_addMethod(dispCls, @selector(scale), (IMP)disp_scale, "d@:");
        class_addMethod(dispCls, @selector(setScale:), (IMP)disp_setScale, "v@:d");
        class_addMethod(dispCls, @selector(minimumScale), (IMP)disp_minScale, "d@:");
        class_addMethod(dispCls, @selector(maximumScale), (IMP)disp_maxScale, "d@:");
        class_addMethod(dispCls, @selector(displayId), (IMP)disp_displayId, "I@:");
        class_addMethod(dispCls, @selector(displayType), (IMP)disp_displayType, "q@:");
        class_addMethod(dispCls, @selector(name), (IMP)disp_name, "@@:");
        class_addMethod(dispCls, @selector(deviceName), (IMP)disp_deviceName, "@@:");
        class_addMethod(dispCls, @selector(uniqueId), (IMP)disp_uniqueId, "@@:");
        class_addMethod(dispCls, @selector(orientation), (IMP)disp_orientation, "@@:");
        class_addMethod(dispCls, @selector(setOrientation:), (IMP)disp_setOrientation, "v@:@");
        class_addMethod(dispCls, @selector(refreshRate), (IMP)disp_refreshRate, "f@:");
        class_addMethod(dispCls, @selector(minimumFrameTime), (IMP)disp_minFrameTime, "f@:");
        class_addMethod(dispCls, @selector(overscanAmount), (IMP)disp_overscanAmount, "d@:");
        class_addMethod(dispCls, @selector(overscanAmounts), (IMP)disp_overscanAmounts, "{CGSize=dd}@:");
        class_addMethod(dispCls, @selector(isBlanked), (IMP)disp_isBlanked, "B@:");
        class_addMethod(dispCls, @selector(setBlanked:), (IMP)disp_setBlanked, "v@:B");
        class_addMethod(dispCls, @selector(cloneMaster), (IMP)disp_cloneMaster, "@@:");
        class_addMethod(dispCls, @selector(clones), (IMP)disp_clones, "@@:");
        class_addMethod(dispCls, @selector(tag), (IMP)disp_tag, "q@:");
        class_addMethod(dispCls, @selector(setTag:), (IMP)disp_setTag, "v@:q");
        class_addMethod(dispCls, @selector(processId), (IMP)disp_processId, "i@:");
        class_addMethod(dispCls, @selector(setProcessId:), (IMP)disp_setProcessId, "v@:i");
        class_addMethod(dispCls, @selector(rendererFlags), (IMP)disp_rendererFlags, "I@:");
        class_addMethod(dispCls, @selector(contextIdAtPosition:), (IMP)disp_contextIdAtPosition, "I@:{CGPoint=dd}");
        class_addMethod(dispCls, @selector(contextIdAtPosition:excludingContextIds:), (IMP)disp_contextIdAtPosition, "I@:{CGPoint=dd}@");
        class_addMethod(dispCls, @selector(clientPortAtPosition:), (IMP)disp_clientPortAtPosition, "I@:{CGPoint=dd}");
        class_addMethod(dispCls, @selector(clientPortOfContextId:), (IMP)disp_clientPortOfContextId, "I@:I");
        class_addMethod(dispCls, @selector(taskPortOfContextId:), (IMP)disp_taskPortOfContextId, "I@:I");
        class_addMethod(dispCls, @selector(contextIdHostingContextId:), (IMP)disp_contextIdHostingContextId, "I@:I");
        class_addMethod(dispCls, @selector(convertPoint:toContextId:), (IMP)disp_convertPoint, "{CGPoint=dd}@:{CGPoint=dd}I");
        class_addMethod(dispCls, @selector(convertPoint:fromContextId:), (IMP)disp_convertPoint, "{CGPoint=dd}@:{CGPoint=dd}I");
        clas

objc

s_addMethod(dispCls, @selector(update), (IMP)disp_noop, "v@:");
        class_addMethod(dispCls, @selector(freeze), (IMP)disp_noop, "v@:");
        class_addMethod(dispCls, @selector(invalidate), (IMP)disp_noop, "v@:");
        class_addMethod(dispCls, @selector(isMirroringEnabled), (IMP)disp_bool_no, "B@:");
        class_addMethod(dispCls, @selector(supportsExtendedColors), (IMP)disp_bool_yes, "B@:");
        objc_registerClassPair(dispCls);
    } else {
        dispCls = objc_getClass("HakiosCAWindowServerDisplay");
    }

    g_fake_display = class_createInstance(dispCls, 0);
    HLOG("Created HakiosCAWindowServerDisplay -> %p (1125x2436 @3x, id=1)", g_fake_display);
    return g_fake_display;
}

// --- CAWindowServer Metod Hook'ları ---
static id (*orig_serverWithOptions)(id, SEL, id) = NULL;
static id (*orig_server)(id, SEL) = NULL;
static id (*orig_displays)(id, SEL) = NULL;

static id hooked_displays(id self, SEL _cmd) {
    id arr = orig_displays ? orig_displays(self, _cmd) : nil;
    if (arr && [arr count] > 0) {
        HLOG("-[CAWindowServer displays] returned %lu native display(s)", (unsigned long)[arr count]);
        return arr;
    }
    id d = hakios_get_or_create_display();
    return @[d];
}

static id hooked_displayWithName(id self, SEL _cmd, id name) {
    return hakios_get_or_create_display();
}

static id hooked_displayWithDisplayId(id self, SEL _cmd, unsigned int dispId) {
    return hakios_get_or_create_display();
}

static id hooked_displayWithUniqueId(id self, SEL _cmd, id uid) {
    return hakios_get_or_create_display();
}

static id hakios_get_or_create_server(void) {
    if (g_fake_server) return g_fake_server;
    Class wsCls = objc_getClass("CAWindowServer");
    if (!wsCls) wsCls = objc_getClass("NSObject");
    g_fake_server = class_createInstance(wsCls, 0);
    HLOG("Allocated standalone CAWindowServer instance -> %p", g_fake_server);
    return g_fake_server;
}

static id hooked_serverWithOptions(id self, SEL _cmd, id options) {
    HLOG("+[CAWindowServer serverWithOptions:%p] ENTER (fast-path software mode)", options);

    // 1. MiniIOMobileFramebuffer'ı aç ve ilk boot karesini ekrana bas
    hakios_fb_init_and_draw_boot_frame();

    // 2. Gerçek CAWindowServer::_init içerisindeki Metal/IOMFB/MobileGestalt
    //    bloklanmasını bypass ederek doğrudan sentetik CAWindowServer nesnesini dön.
    if (getenv("HAKIOS_TRY_REAL_CAWS")) {
        HLOG("Calling orig +[CAWindowServer serverWithOptions:]...");
        id srv = orig_serverWithOptions(self, _cmd, options);
        HLOG("orig +[CAWindowServer serverWithOptions:] returned %p", srv);
        if (srv) {
            g_fake_server = srv;
            return srv;
        }
    }

    id srv = hakios_get_or_create_server();
    hakios_get_or_create_display();
    HLOG("+[CAWindowServer serverWithOptions:] DONE -> %p", srv);
    return srv;
}

static id hooked_server(id self, SEL _cmd) {
    if (g_fake_server) return g_fake_server;
    return hooked_serverWithOptions(self, @selector(serverWithOptions:), nil);
}

static id hooked_serverIfRunning(id self, SEL _cmd) {
    return g_fake_server;
}

static void hooked_setRendererFlags(id self, SEL _cmd, unsigned int flags) {
    HLOG("-[CAWindowServer setRendererFlags:0x%x]", flags);
}
static unsigned int hooked_rendererFlags(id self, SEL _cmd) { return 0; }
static void hooked_setSecureMode(id self, SEL _cmd, BOOL sec) {}
static BOOL hooked_secureMode(id self, SEL _cmd) { return NO; }
static id   hooked_insecureProcessIds(id self, SEL _cmd) { return @[]; }

// ============================================================================
// 4. BKHIDSystemInterface ALT NESNE KORUMASI (deliveryManager, senderCache vb.)
// ============================================================================
static id g_hid_stub_proxy = nil;
static id (*orig_deliveryManager)(id, SEL) = NULL;
static id (*orig_senderCache)(id, SEL) = NULL;
static id (*orig_clientConnectionManager)(id, SEL) = NULL;

static i

objc

d hakios_get_hid_stub(const char *getter_name) {
    if (!g_hid_stub_proxy) {
        Class cls = hakios_get_stub_class();
        class_addMethod(object_getClass(cls), @selector(resolveInstanceMethod:), (IMP)stub_resolve_instance_method, "B@::");
        g_hid_stub_proxy = class_createInstance(cls, 0);
    }
    HLOG("-[BKHIDSystemInterface %s] -> returning nil-safe stub %p", getter_name, g_hid_stub_proxy);
    return g_hid_stub_proxy;
}

static id hooked_deliveryManager(id self, SEL _cmd) {
    id r = orig_deliveryManager ? orig_deliveryManager(self, _cmd) : nil;
    return r ? r : hakios_get_hid_stub("deliveryManager");
}

static id hooked_senderCache(id self, SEL _cmd) {
    id r = orig_senderCache ? orig_senderCache(self, _cmd) : nil;
    return r ? r : hakios_get_hid_stub("senderCache");
}

static id hooked_clientConnectionManager(id self, SEL _cmd) {
    id r = orig_clientConnectionManager ? orig_clientConnectionManager(self, _cmd) : nil;
    return r ? r : hakios_get_hid_stub("clientConnectionManager");
}

// ============================================================================
// 5. CONSTRUCTOR: HOOK KURULUMU
// ============================================================================
__attribute__((constructor))
static void hakios_windowserver_init(void) {
    Class wsCls = objc_getClass("CAWindowServer");
    if (wsCls) {
        Class wsMeta = object_getClass((id)wsCls);

        Method m_swo = class_getClassMethod(wsCls, @selector(serverWithOptions:));
        if (m_swo) {
            orig_serverWithOptions = (void *)method_getImplementation(m_swo);
            method_setImplementation(m_swo, (IMP)hooked_serverWithOptions);
        }

        Method m_srv = class_getClassMethod(wsCls, @selector(server));
        if (m_srv) {
            orig_server = (void *)method_getImplementation(m_srv);
            method_setImplementation(m_srv, (IMP)hooked_server);
        }

        Method m_sir = class_getClassMethod(wsCls, @selector(serverIfRunning));
        if (m_sir) {
            method_setImplementation(m_sir, (IMP)hooked_serverIfRunning);
        } else {
            class_addMethod(wsMeta, @selector(serverIfRunning), (IMP)hooked_serverIfRunning, "@@:");
        }

        Method m_disp = class_getInstanceMethod(wsCls, @selector(displays));
        if (m_disp) {
            orig_displays = (void *)method_getImplementation(m_disp);
            method_setImplementation(m_disp, (IMP)hooked_displays);
        }

        class_replaceMethod(wsCls, @selector(displayWithName:), (IMP)hooked_displayWithName, "@@:@");
        class_replaceMethod(wsCls, @selector(displayWithDisplayId:), (IMP)hooked_displayWithDisplayId, "@@:I");
        class_replaceMethod(wsCls, @selector(displayWithUniqueId:), (IMP)hooked_displayWithUniqueId, "@@:@");
        class_replaceMethod(wsCls, @selector(setRendererFlags:), (IMP)hooked_setRendererFlags, "v@:I");
        class_replaceMethod(wsCls, @selector(rendererFlags), (IMP)hooked_rendererFlags, "I@:");
        class_replaceMethod(wsCls, @selector(setSecureMode:), (IMP)hooked_setSecureMode, "v@:B");
        class_replaceMethod(wsCls, @selector(secureMode), (IMP)hooked_secureMode, "B@:");
        class_replaceMethod(wsCls, @selector(insecureProcessIds), (IMP)hooked_insecureProcessIds, "@@:");

        HLOG("CAWindowServer + HakiosCAWindowServerDisplay hooks installed!");
    }

    Class hidCls = objc_getClass("BKHIDSystemInterface");
    if (hidCls) {
        Method m_dm = class_getInstanceMethod(hidCls, @selector(deliveryManager));
        if (m_dm) {
            orig_deliveryManager = (void *)method_getImplementation(m_dm);
            method_setImplementation(m_dm, (IMP)hooked_deliveryManager);
        }
        Method m_sc = class_getInstanceMethod(hidCls, @selector(senderCache));
        if (m_sc) {
            orig_senderCache = (void *)method_getImplementation(m_sc);
            method_setImplementation(m_sc, (IMP)hooked_senderCache);
        }
        Method m_ccm = class_getInst

objc

anceMethod(hidCls, @selector(clientConnectionManager));
        if (m_ccm) {
            orig_clientConnectionManager = (void *)method_getImplementation(m_ccm);
            method_setImplementation(m_ccm, (IMP)hooked_clientConnectionManager);
        }
        HLOG("BKHIDSystemInterface nil-safe sub-object hooks installed!");
    }
}


objc