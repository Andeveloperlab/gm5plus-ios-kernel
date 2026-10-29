/*
 * Haptics and Physical Vibration Motor Driver for General Mobile GM 5 Plus (MSM8952)
 * and Darwin/XNU iOS 16.5 (xnu-8796.121.2).
 *
 * Targets:
 *   1. Hardware GM 5 Plus (MSM8952):
 *      - Qualcomm PM8950 / PMI8950 PMIC SPMI Peripheral 0xC000 (qcom,qpnp-haptics)
 *      - Auto-probes SPMI USID 0, 1, 2, 3 to locate the haptic APID channel.
 *      - Registers & Calibration:
 *          0xC004 / 0xC005 : Peripheral TYPE (0x0B) / SUBTYPE (0x09)
 *          0xC046          : EN_CTL1 (bit 7: Module Enable)
 *          0xC048          : EN_CTL2 (bit 0: BRAKE_EN = 0x01, Active Braking!)
 *          0xC04C          : ACT_TYPE (1 = ERM)
 *          0xC04D          : WAV_SHAPE (1 = Square wave for max torque)
 *          0xC04E          : PLAY_MODE (0 = Direct Mode)
 *          0xC051          : VMAX (0x36 ~3.13V overdrive for TAP, 0x24 ~2.08V for LOOP)
 *          0xC052          : ILIM (1 = 800 mA)
 *          0xC054 / 0xC055 : RATE_CFG (0x01F4 = 2.5 ms wave -> 5 ms/phase brake)
 *          0xC05C          : BRAKE_REG (0x3F = 3-phase reverse polarity brake pattern)
 *          0xC070          : PLAY (bit 7: 1 = start playback, 0 = trigger hardware braking)
 *   2. Userland Integration:
 *      - Creates character device /dev/hakios-haptic.
 *      - Consumes PCM actuator frames from userland/src/audio_plugin.c (CoreHaptics / UIKit feedbacks).
 *      - Distinguishes short pulses (< 60 ms: TAP) and sustained vibrations (>= 60 ms: LOOP).
 *      - Auto-detects QEMU Virt and logs virtual actuator events cleanly without error.
 *   3. Interactive Sysctl:
 *      - kern.hakios.haptic_test: write 1 for 12ms crisp tap, 2 for 25ms tap, 3 for 200ms alert.
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
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

/* =========================================================================
 * 1. SPMI V2 Arbiter & QPNP Haptics Definitions
 * ========================================================================= */
#define SHAMROCK_SPMI_CORE_PHYS      0x0200f000UL
#define SHAMROCK_SPMI_CORE_SIZE      0x00001000UL
#define SHAMROCK_SPMI_WR_PHYS        0x02400000UL
#define SHAMROCK_SPMI_WR_SIZE        0x00800000UL
#define SHAMROCK_SPMI_OBS_PHYS       0x02c00000UL
#define SHAMROCK_SPMI_OBS_SIZE       0x00800000UL

/* QPNP Haptics Peripheral (0xC000) Registers */
#define QPNP_HAP_REG_TYPE            0x04U
#define QPNP_HAP_REG_SUBTYPE         0x05U
#define QPNP_HAP_STATUS1_REG         0x0AU
#define   QPNP_HAP_STATUS1_BUSY      (1U << 0)
#define QPNP_HAP_EN_CTL1_REG         0x46U
#define   QPNP_HAP_EN_BIT            (1U << 7)
#define QPNP_HAP_EN_CTL2_REG         0x48U
#define   QPNP_HAP_BRAKE_EN_BIT      (1U << 0) /* 0x01: Active Reverse-Polarity Braking */
#define QPNP_HAP_ACT_TYPE_REG        0x4CU
#define   QPNP_HAP_ACT_ERM           0x01U
#define QPNP_HAP_WAV_SHAPE_REG       0x4DU
#define   QPNP_HAP_WAV_SQUARE        0x01U     /* Square wave for maximum start torque */
#define QPNP_HAP_PLAY_MODE_REG       0x4EU
#define   QPNP_HAP_MODE_DIRECT       0x00U     /* Direct mode */
#define QPNP_HAP_VMAX_CFG_REG        0x51U
#define   QPNP_HAP_VMAX_OVERDRIVE    0x36U     /* ~3.13V overdrive for short TAP */
#define   QPNP_HAP_VMAX_STEADY       0x24U     /* ~2.08V steady for continuous LOOP */
#define QPNP_HAP_ILIM_CFG_REG        0x52U
#define   QPNP_HAP_ILIM_800MA        0x01U     /* 800 mA current limit */
#define QPNP_HAP_RATE_CFG1_REG       0x54U
#define QPNP_HAP_RATE_CFG2_REG       0x55U
#define QPNP_HAP_BRAKE_REG           0x5CU
#define   QPNP_HAP_BRAKE_PATTERN     0x3FU     /* Phases 1,2,3 = Full Reverse Vmax (11), Phase 4 = 0V (00) */
#define QPNP_HAP_PLAY_REG            0x70U
#define   QPNP_HAP_PLAY_BIT          (1U << 7)

/* Timing & Audio Thresholds */
#define ACTIVE_US                    30000ULL       /* writes within this window are part of one stream */
#define SILENT_BELOW                 256U           /* samples no larger than 256 of 32768 are silence */
#define TAP_US                       60000ULL       /* longer than this is a continuous vibration */
#define HAPTIC_MAX_MS                500U           /* safety cutoff for continuous vibration */

static vm_offset_t s_spmi_core = 0;
static vm_offset_t s_spmi_wr = 0;
static vm_offset_t s_spmi_obs = 0;

static int  s_hap_apid = -1;
static int  s_hap_usid = -1;
static bool s_hw_present = false;
static bool s_playing = false;
static bool s_is_qemu = false;

static volatile uint64_t s_heard_us = 0;
static volatile uint64_t s_quiet_us = 0;
static volatile uint64_t s_onset_us = 0;
static volatile uint32_t s_heard_peak = 0;

static uint64_t s_total_taps = 0;
static uint64_t s_total_loops = 0;
static uint64_t s_written_frames = 0;

static inline uint32_t rd32(vm_offset_t addr) { return *(volatile uint32_t *)addr; }
static inline void wr32(vm_offset_t addr, uint32_t val) { *(volatile uint32_t *)addr = val; }

static uint64_t
now_us(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* =========================================================================
 * 2. SPMI Read / Write Transactions
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
	uint32_t cmd = (0U << 27) | ((uint32_t)offset << 4) | 0U; /* EXT_WRITEL, 1 byte */
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

static int
spmi_read_byte(int apid, uint8_t offset, uint8_t *out_val)
{
	if (apid < 0 || !s_spmi_obs || s_is_qemu) return -1;

	vm_offset_t chn = s_spmi_obs + 0x8000UL * (uint32_t)apid;
	uint32_t cmd = (1U << 27) | ((uint32_t)offset << 4) | 0U; /* EXT_READL, 1 byte */
	uint64_t start = mach_absolute_time(), now, to;
	nanoseconds_to_absolutetime(2000ULL * (uint64_t)NSEC_PER_USEC, &to);

	hakios_spmi_lock();
	wr32(chn + 0x00UL, cmd);
	while ((rd32(chn + 0x08UL) & 1U) == 0) {
		now = mach_absolute_time();
		if (now - start > to) {
			hakios_spmi_unlock();
			return -2;
		}
	}
	*out_val = (uint8_t)(rd32(chn + 0x18UL) & 0xFFU);
	hakios_spmi_unlock();
	return 0;
}

/* =========================================================================
 * 3. Hardware Haptic Control with Active Braking
 * ========================================================================= */
static void
shamrock_haptic_vibrate(bool on)
{
	if (!s_hw_present || s_hap_apid < 0) {
		s_playing = on;
		return;
	}

	if (on) {
		spmi_write_byte(s_hap_apid, QPNP_HAP_VMAX_CFG_REG, QPNP_HAP_VMAX_STEADY);
		spmi_write_byte(s_hap_apid, QPNP_HAP_EN_CTL1_REG, QPNP_HAP_EN_BIT);
		spmi_write_byte(s_hap_apid, QPNP_HAP_PLAY_REG, QPNP_HAP_PLAY_BIT);
		s_playing = true;
	} else {
		/* Trigger hardware active braking */
		spmi_write_byte(s_hap_apid, QPNP_HAP_PLAY_REG, 0x00U);
		delay_for_interval(16, NSEC_PER_MSEC);
		spmi_write_byte(s_hap_apid, QPNP_HAP_EN_CTL1_REG, 0x00U);
		s_playing = false;
	}
}

/* Trigger a crisp Taptic-Engine-like tap (12 ms Overdrive + 16 ms Active Braking) */
static void
shamrock_haptic_crisp_tap(void)
{
	if (!s_hw_present || s_hap_apid < 0) {
		return;
	}

	/* 1. Overdrive Vmax (~3.13V) and Module Enable */
	spmi_write_byte(s_hap_apid, QPNP_HAP_VMAX_CFG_REG, QPNP_HAP_VMAX_OVERDRIVE);
	spmi_write_byte(s_hap_apid, QPNP_HAP_EN_CTL1_REG, QPNP_HAP_EN_BIT);

	/* 2. 12 ms forward drive pulse */
	spmi_write_byte(s_hap_apid, QPNP_HAP_PLAY_REG, QPNP_HAP_PLAY_BIT);
	delay_for_interval(12, NSEC_PER_MSEC);

	/* 3. PLAY=0 triggers active reverse-polarity braking (EN_CTL1 stays enabled!) */
	spmi_write_byte(s_hap_apid, QPNP_HAP_PLAY_REG, 0x00U);

	/* 4. Wait 16 ms for 3-phase braking to complete */
	delay_for_interval(16, NSEC_PER_MSEC);

	/* 5. Power down */
	spmi_write_byte(s_hap_apid, QPNP_HAP_EN_CTL1_REG, 0x00U);
}

/* =========================================================================
 * 4. Character Device /dev/hakios-haptic
 * ========================================================================= */
static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
haptic_close(__unused dev_t dev, __unused int flags, __unused int fmt, __unused struct proc *p)
{
	return 0;
}

static int
haptic_read(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return ENODEV;
}

static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];
			if (v > peak) peak = v;
		}
		s_written_frames += (uint64_t)bytes / 2;
		if (peak > SILENT_BELOW) {
			uint64_t now = now_us();
			if (s_heard_us == 0 || s_quiet_us > s_heard_us || now - s_heard_us >= ACTIVE_US) {
				s_onset_us = now;
				s_heard_peak = 0;
			}
			s_heard_us = now;
			if (peak > s_heard_peak) {
				s_heard_peak = peak;
			}
		} else {
			s_quiet_us = now_us();
		}
	}
	return error;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_close, .d_read = haptic_read, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
hakios_haptic_pretend(uint32_t peak)
{
	uint64_t now = now_us();

	if (peak <= SILENT_BELOW) {
		s_quiet_us = now;
		return;
	}
	if (s_heard_us == 0 || s_quiet_us > s_heard_us || now - s_heard_us >= ACTIVE_US) {
		s_onset_us = now;
		s_heard_peak = 0;
	}
	s_heard_us = now;
	if (peak > s_heard_peak) {
		s_heard_peak = peak;
	}
}

/* =========================================================================
 * 5. Haptic State Machine Worker Thread
 * ========================================================================= */
static void
haptic_worker_thread(__unused void *arg, __unused wait_result_t result)
{
	printf("haptic: [SHAMROCK] haptic feedback server started (/dev/hakios-haptic)\n");

	enum { IDLE, TAP, VIBRATING } state = IDLE;
	uint64_t loop_start_us = 0;

	for (;;) {
		uint64_t now = now_us();
		bool active = (s_heard_us != 0 && (now - s_heard_us < ACTIVE_US));

		switch (state) {
		case IDLE:
			if (active) {
				uint64_t duration = now - s_onset_us;
				if (duration < TAP_US) {
					/* Trigger a crisp 12 ms tap + active brake */
					state = TAP;
					s_total_taps++;
					if (s_total_taps <= 30 || s_total_taps % 100 == 0) {
						printf("haptic: [SHAMROCK] crisp tap #%llu played (peak %u, hw=%s)\n",
						    s_total_taps, s_heard_peak, s_hw_present ? "YES" : "VIRT");
					}
					shamrock_haptic_crisp_tap();
					state = IDLE;
				} else {
					/* Continuous vibration begins */
					state = VIBRATING;
					loop_start_us = now;
					s_total_loops++;
					printf("haptic: [SHAMROCK] continuous vibration #%llu started (peak %u)\n",
					    s_total_loops, s_heard_peak);
					shamrock_haptic_vibrate(true);
				}
			}
			break;

		case TAP:
			state = IDLE;
			break;

		case VIBRATING:
			if (!active || (now - loop_start_us > (uint64_t)HAPTIC_MAX_MS * 1000ULL)) {
				/* Activity stopped or hit safety cutoff */
				shamrock_haptic_vibrate(false);
				state = IDLE;
				if (!active) {
					printf("haptic: [SHAMROCK] continuous vibration finished normally\n");
				} else {
					printf("haptic: [SHAMROCK] continuous vibration capped at %u ms safety limit\n",
					    HAPTIC_MAX_MS);
				}
			}
			break;
		}

		delay_for_interval(5, NSEC_PER_MSEC);
	}
}

/* =========================================================================
 * 6. Interactive Sysctl for Manual / Script Testing
 * ========================================================================= */
SYSCTL_DECL(_kern_hakios);

static int
haptic_sysctl_trigger SYSCTL_HANDLER_ARGS
{
	#pragma unused(arg1, arg2)
	int val = 0;
	int error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || !req->newptr) return error;

	if (val == 1) {
		printf("haptic: manual crisp tap triggered (12ms + brake)\n");
		shamrock_haptic_crisp_tap();
	} else if (val == 2) {
		printf("haptic: manual medium pulse triggered (25 ms)\n");
		shamrock_haptic_vibrate(true);
		delay_for_interval(25, NSEC_PER_MSEC);
		shamrock_haptic_vibrate(false);
	} else if (val == 3) {
		printf("haptic: manual alert buzz triggered (200 ms)\n");
		shamrock_haptic_vibrate(true);
		delay_for_interval(200, NSEC_PER_MSEC);
		shamrock_haptic_vibrate(false);
	}
	return 0;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, haptic_test,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, haptic_sysctl_trigger, "I", "write 1: crisp tap, 2: medium tap, 3: long buzz");

/* =========================================================================
 * 7. Driver Initialization & Quiesce
 * ========================================================================= */
void
hakios_haptic_init(void)
{
	uint32_t is_virt = 0;
	if (PE_parse_boot_argn("virtio_blk", &is_virt, sizeof(is_virt)) && is_virt) {
		s_is_qemu = true;
	}

	printf("haptic: [SHAMROCK] initializing haptic & vibration motor driver (%s)\n",
	    s_is_qemu ? "QEMU Virt mode" : "MSM8952 Hardware mode");

	if (!s_is_qemu) {
		/* 1. Map SPMI Arbiter Hardware on Bare-Metal Shamrock */
		s_spmi_core = ml_io_map(SHAMROCK_SPMI_CORE_PHYS, SHAMROCK_SPMI_CORE_SIZE);
		s_spmi_wr = ml_io_map(SHAMROCK_SPMI_WR_PHYS, SHAMROCK_SPMI_WR_SIZE);
		s_spmi_obs = ml_io_map(SHAMROCK_SPMI_OBS_PHYS, SHAMROCK_SPMI_OBS_SIZE);

		/* 2. Probe PM8950 / PMI8950 for Peripheral 0xC000 (qcom,qpnp-haptics) */
		if (s_spmi_core) {
			for (uint8_t u = 0; u < 4; u++) {
				int ap = spmi_find_apid(u, 0xC000);
				if (ap >= 0) {
					s_hap_apid = ap;
					s_hap_usid = (int)u;
					break;
				}
			}
		}

		if (s_hap_apid >= 0) {
			uint8_t hap_type = 0, hap_subtype = 0;
			spmi_read_byte(s_hap_apid, QPNP_HAP_REG_TYPE, &hap_type);
			spmi_read_byte(s_hap_apid, QPNP_HAP_REG_SUBTYPE, &hap_subtype);
			printf("haptic: detected Qualcomm QPNP Haptics at USID %d, APID %d (TYPE=0x%02X, SUBTYPE=0x%02X)\n",
			    s_hap_usid, s_hap_apid, hap_type, hap_subtype);

			/* Calibrate QPNP Haptics according to frontier model guidelines */
			spmi_write_byte(s_hap_apid, QPNP_HAP_ACT_TYPE_REG, QPNP_HAP_ACT_ERM);    /* ERM motor (0x01) */
			spmi_write_byte(s_hap_apid, QPNP_HAP_WAV_SHAPE_REG, QPNP_HAP_WAV_SQUARE); /* Square wave (0x01) */
			spmi_write_byte(s_hap_apid, QPNP_HAP_PLAY_MODE_REG, QPNP_HAP_MODE_DIRECT);/* Direct mode (0x00 @ 0xC04E) */
			spmi_write_byte(s_hap_apid, QPNP_HAP_ILIM_CFG_REG, QPNP_HAP_ILIM_800MA);  /* 800 mA limit */
			spmi_write_byte(s_hap_apid, QPNP_HAP_RATE_CFG1_REG, 0xF4U);              /* 0x01F4 (2.5 ms T_wave) */
			spmi_write_byte(s_hap_apid, QPNP_HAP_RATE_CFG2_REG, 0x01U);
			spmi_write_byte(s_hap_apid, QPNP_HAP_BRAKE_REG, QPNP_HAP_BRAKE_PATTERN); /* 0x3F Active Brake Pattern */
			spmi_write_byte(s_hap_apid, QPNP_HAP_EN_CTL2_REG, QPNP_HAP_BRAKE_EN_BIT);/* 0xC048 = 0x01 BRAKE_EN! */
			spmi_write_byte(s_hap_apid, QPNP_HAP_PLAY_REG, 0x00U);                    /* Standby */
			s_hw_present = true;
		} else {
			printf("haptic: SPMI haptics not detected on hardware; running in Fallback mode\n");
		}
	} else {
		printf("haptic: running in pure virtual mode under QEMU\n");
	}

	/* 3. Register /dev/hakios-haptic Char Device for Userland (audio_plugin.c) */
	int major = cdevsw_add(-1, &haptic_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		printf("haptic: failed to create /dev/hakios-haptic node\n");
	} else {
		printf("haptic: /dev/hakios-haptic registered successfully (major %d)\n", major);
	}

	/* 4. Start Background Haptic Server Thread */
	thread_t thread;
	if (kernel_thread_start(haptic_worker_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
hakios_haptic_quiesce(void)
{
	if (s_hw_present && s_hap_apid >= 0) {
		shamrock_haptic_vibrate(false);
		spmi_write_byte(s_hap_apid, QPNP_HAP_EN_CTL1_REG, 0x00U);
	}
	printf("haptic: at run end: %llu taps, %llu continuous vibrations played from %llu audio frames\n",
	    s_total_taps, s_total_loops, s_written_frames);
}
