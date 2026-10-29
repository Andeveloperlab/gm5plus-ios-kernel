/*
 * Backlight (White LED / WLED) Driver for General Mobile GM 5 Plus (MSM8952)
 * and Darwin/XNU iOS 16.5 (xnu-8796.121.2).
 *
 * Targets:
 *   1. Hardware GM 5 Plus (MSM8952 / PMI8950 Dual-Peripheral WLED):
 *      - CTRL Block: 0xD800 (USID 2 or 3)
 *          * 0xD846 : WLED_MOD_EN_REG (0x80)
 *          * 0xD84C : WLED_SWITCHING_FREQ (0x0B = 800 kHz)
 *          * 0xD84D : WLED_OVP_CFG (0x02 = 29.5V)
 *          * 0xD84E : WLED_ILIM_CFG (0x04 = 980 mA)
 *      - SINK Block: 0xD900 (USID 2 or 3)
 *          * 0xD947 : WLED_SYNC_REG (0x07 -> 0x00 Latch Pulse)
 *          * 0xD94F : WLED_CURR_SINK_REG (0xE0 = 3 strings enabled)
 *          * 0xD960 + n*0x10 : MOD_EN (0x80)
 *          * 0xD962 + n*0x10 : FULL_SCALE (0x14 = 20 mA)
 *          * 0xD963 + n*0x10 : BRIGHTNESS_LSB
 *          * 0xD964 + n*0x10 : BRIGHTNESS_MSB (12-bit, 0..4095)
 *   2. Interfaces:
 *      - Character device: /dev/hakios-backlight
 *      - Sysctl: kern.hakios.backlight_level (0..4095, Read/Write)
 *      - Safe QEMU Virt fallback without hardware aborts.
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

/* =========================================================================
 * 1. SPMI Arbiter & QPNP WLED Definitions
 * ========================================================================= */
#define SHAMROCK_SPMI_CORE_PHYS      0x0200f000UL
#define SHAMROCK_SPMI_CORE_SIZE      0x00001000UL
#define SHAMROCK_SPMI_WR_PHYS        0x02400000UL
#define SHAMROCK_SPMI_WR_SIZE        0x00800000UL
#define SHAMROCK_SPMI_OBS_PHYS       0x02c00000UL
#define SHAMROCK_SPMI_OBS_SIZE       0x00800000UL

/* CTRL (0xD800) Register Offsets */
#define WLED_MOD_EN_REG              0x46U
#define   WLED_MOD_EN_BIT            (1U << 7) /* 0x80 */
#define WLED_SWITCHING_FREQ_REG      0x4CU
#define WLED_OVP_CFG_REG             0x4DU
#define WLED_ILIM_CFG_REG            0x4EU

/* SINK (0xD900) Register Offsets */
#define WLED_SYNC_REG                0x47U
#define   WLED_SYNC_3STRINGS         0x07U
#define   WLED_SYNC_RESET            0x00U
#define WLED_CURR_SINK_REG           0x4FU
#define   WLED_CURR_SINK_3STRINGS    (0x07U << 5) /* 0xE0: Bits [7:5]! */

#define WLED_SINK_MOD_EN_REG(n)      (0x60U + ((n) * 0x10U))
#define WLED_FULL_SCALE_REG(n)       (0x62U + ((n) * 0x10U))
#define WLED_BRIGHTNESS_LSB_REG(n)   (0x63U + ((n) * 0x10U))
#define WLED_BRIGHTNESS_MSB_REG(n)   (0x64U + ((n) * 0x10U))

static vm_offset_t s_spmi_core = 0;
static vm_offset_t s_spmi_wr = 0;
static vm_offset_t s_spmi_obs = 0;

static int  s_wled_ctrl_apid = -1;
static int  s_wled_sink_apid = -1;
static uint8_t s_wled_usid = 2;
static bool s_hw_present = false;
static bool s_is_qemu = false;

static int  s_current_brightness = 2048; /* Default 50% */

static inline uint32_t rd32(vm_offset_t addr) { return *(volatile uint32_t *)addr; }
static inline void wr32(vm_offset_t addr, uint32_t val) { *(volatile uint32_t *)addr = val; }

/* =========================================================================
 * 2. SPMI Arbiter Helpers
 * ========================================================================= */
static int
spmi_find_apid(uint8_t usid, uint16_t pid)
{
	if (!s_spmi_core || s_is_qemu) return -1;
	uint32_t ppid = ((uint32_t)usid << 8) | ((uint32_t)(pid >> 8) & 0xFFU);
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t entry = rd32(s_spmi_core + 0x800UL + 4UL * i);
		if (entry && ((entry >> 8) & 0xFFFU) == ppid) {
			return (int)i;
		}
	}
	return -1;
}

static int
spmi_write_byte(int apid, uint8_t offset, uint8_t val)
{
	if (apid < 0 || !s_spmi_wr || s_is_qemu) return -1;

	vm_offset_t chn = s_spmi_wr + 0x8000UL * (uint32_t)apid;
	uint32_t cmd = (0U << 27) | ((uint32_t)offset << 4) | 0U;
	uint64_t start = mach_absolute_time(), now, to;
	nanoseconds_to_absolutetime(2000ULL * (uint64_t)NSEC_PER_USEC, &to);

	hakios_spmi_lock();
	wr32(chn + 0x10UL, (uint32_t)val);
	wr32(chn + 0x00UL, cmd);
	while ((rd32(chn + 0x08UL) & 1U) == 0) {
		now = mach_absolute_time();
		if (now - start > to) {
			hakios_spmi_unlock();
			return -2;
		}
	}
	hakios_spmi_unlock();
	return 0;
}

/* =========================================================================
 * 3. WLED Hardware Control
 * ========================================================================= */
static void
wled_hw_init(void)
{
	if (!s_hw_present || s_wled_ctrl_apid < 0 || s_wled_sink_apid < 0) return;

	/* 1. OVP = 29.5V (0x02), Boost ILIM = 980mA (0x04), Frekans = 800kHz (0x0B) */
	spmi_write_byte(s_wled_ctrl_apid, WLED_OVP_CFG_REG, 0x02U);
	spmi_write_byte(s_wled_ctrl_apid, WLED_ILIM_CFG_REG, 0x04U);
	spmi_write_byte(s_wled_ctrl_apid, WLED_SWITCHING_FREQ_REG, 0x0BU);

	/* 2. Modulator enable (0x80) and 20 mA full scale (0x14) for all 3 strings */
	for (int n = 0; n < 3; n++) {
		spmi_write_byte(s_wled_sink_apid, WLED_SINK_MOD_EN_REG(n), 0x80U);
		spmi_write_byte(s_wled_sink_apid, WLED_FULL_SCALE_REG(n), 0x14U);
	}

	/* 3. Enable 3 strings (0xD94F = 0xE0) and Boost Module (0xD846 = 0x80) */
	spmi_write_byte(s_wled_sink_apid, WLED_CURR_SINK_REG, WLED_CURR_SINK_3STRINGS);
	spmi_write_byte(s_wled_ctrl_apid, WLED_MOD_EN_REG, WLED_MOD_EN_BIT);
}

void
hakios_backlight_set_level(int brightness)
{
	if (brightness < 0) brightness = 0;
	if (brightness > 4095) brightness = 4095;
	s_current_brightness = brightness;

	if (!s_hw_present || s_wled_ctrl_apid < 0 || s_wled_sink_apid < 0) {
		return;
	}

	uint8_t lsb = (uint8_t)(brightness & 0xFF);
	uint8_t msb = (uint8_t)((brightness >> 8) & 0x0F);

	/* 1. Write LSB and MSB for all 3 LED strings */
	for (int n = 0; n < 3; n++) {
		spmi_write_byte(s_wled_sink_apid, WLED_BRIGHTNESS_LSB_REG(n), lsb);
		spmi_write_byte(s_wled_sink_apid, WLED_BRIGHTNESS_MSB_REG(n), msb);
	}

	/* 2. Latch pulse via WLED_SYNC (0xD947 = 0x07 -> 0x00) */
	spmi_write_byte(s_wled_sink_apid, WLED_SYNC_REG, WLED_SYNC_3STRINGS);
	spmi_write_byte(s_wled_sink_apid, WLED_SYNC_REG, WLED_SYNC_RESET);

	/* 3. Power management */
	if (brightness == 0) {
		spmi_write_byte(s_wled_ctrl_apid, WLED_MOD_EN_REG, 0x00U);
	} else {
		spmi_write_byte(s_wled_sink_apid, WLED_CURR_SINK_REG, WLED_CURR_SINK_3STRINGS);
		spmi_write_byte(s_wled_ctrl_apid, WLED_MOD_EN_REG, WLED_MOD_EN_BIT);
	}
}

/* =========================================================================
 * 4. Character Device /dev/hakios-backlight
 * ========================================================================= */
static int
backlight_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
backlight_close(__unused dev_t dev, __unused int flags, __unused int fmt, __unused struct proc *p)
{
	return 0;
}

static int
backlight_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "%d\n", s_current_brightness);
	if (uio_resid(uio) < len) len = (int)uio_resid(uio);
	return uiomove((caddr_t)buf, len, uio);
}

static int
backlight_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	char buf[32];
	int len = (int)uio_resid(uio);
	if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
	int error = uiomove((caddr_t)buf, len, uio);
	if (error) return error;
	buf[len] = '\0';

	int val = 0;
	if (sscanf(buf, "%d", &val) == 1) {
		hakios_backlight_set_level(val);
	}
	return 0;
}

static const struct cdevsw backlight_cdevsw = {
	.d_open = backlight_open, .d_close = backlight_close, .d_read = backlight_read, .d_write = backlight_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* =========================================================================
 * 5. Sysctl Interface
 * ========================================================================= */
SYSCTL_DECL(_kern_hakios);

static int
backlight_sysctl_handler SYSCTL_HANDLER_ARGS
{
	#pragma unused(arg1, arg2)
	int val = s_current_brightness;
	int error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || !req->newptr) return error;

	hakios_backlight_set_level(val);
	return 0;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, backlight_level,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, backlight_sysctl_handler, "I", "LCD Backlight Brightness (0..4095)");

/* =========================================================================
 * 6. Driver Initialization
 * ========================================================================= */
void
hakios_backlight_init(void)
{
	uint32_t is_virt = 0;
	if (PE_parse_boot_argn("virtio_blk", &is_virt, sizeof(is_virt)) && is_virt) {
		s_is_qemu = true;
	}

	printf("backlight: [SHAMROCK] initializing LCD WLED backlight driver (%s)\n",
	    s_is_qemu ? "QEMU Virt mode" : "MSM8952 Hardware mode");

	if (!s_is_qemu) {
		/* 1. Map SPMI Arbiter Hardware */
		s_spmi_core = ml_io_map(SHAMROCK_SPMI_CORE_PHYS, SHAMROCK_SPMI_CORE_SIZE);
		s_spmi_wr = ml_io_map(SHAMROCK_SPMI_WR_PHYS, SHAMROCK_SPMI_WR_SIZE);
		s_spmi_obs = ml_io_map(SHAMROCK_SPMI_OBS_PHYS, SHAMROCK_SPMI_OBS_SIZE);

		/* 2. Probe USID 2 and USID 3 for CTRL (0xD800) and SINK (0xD900) */
		if (s_spmi_core) {
			for (uint8_t u = 2; u <= 3; u++) {
				int c_ap = spmi_find_apid(u, 0xD800);
				int s_ap = spmi_find_apid(u, 0xD900);
				if (c_ap >= 0 && s_ap >= 0) {
					s_wled_ctrl_apid = c_ap;
					s_wled_sink_apid = s_ap;
					s_wled_usid = u;
					break;
				}
			}
		}

		if (s_wled_ctrl_apid >= 0 && s_wled_sink_apid >= 0) {
			printf("backlight: detected Qualcomm PMI8950 WLED at USID %d (CTRL APID %d, SINK APID %d)\n",
			    s_wled_usid, s_wled_ctrl_apid, s_wled_sink_apid);
			s_hw_present = true;
			wled_hw_init();
			hakios_backlight_set_level(s_current_brightness);
		} else {
			printf("backlight: PMI8950 WLED peripherals not found on SPMI\n");
		}
	} else {
		printf("backlight: running in virtual mode under QEMU\n");
	}

	/* 3. Register /dev/hakios-backlight character device */
	int major = cdevsw_add(-1, &backlight_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-backlight") == NULL) {
		printf("backlight: failed to create /dev/hakios-backlight node\n");
	} else {
		printf("backlight: /dev/hakios-backlight registered successfully (major %d)\n", major);
	}
}
