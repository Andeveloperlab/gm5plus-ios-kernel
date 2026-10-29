/*
 * bsd/dev/hakios.sdhci.c
 *
 * Qualcomm MSM8952 (Snapdragon 617) SDCC1 eMMC 5.1 Storage Driver for XNU.
 * Physical Base: 0x07824900 (the SDHCI register window of SDCC1; the vendor "core" block sits
 * 0x900 below it at 0x07824000 and holds no SDHCI register at all).
 *
 * Exposes /dev/disk0 (and /dev/rdisk0) representing the internal 32 GB eMMC,
 * and /dev/emmc_userdata for the main Android userdata partition where
 * unsealed APFS / HFS+ system volumes reside.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <kern/locks.h>
#include <kern/clock.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define MSM8952_SDC1_CORE_BASE   0x07824000ULL
#define MSM8952_SDC1_CORE_SIZE   0x00001000ULL
#define MSM8952_SDC1_PHYS_BASE   0x07824900ULL
#define MSM8952_SDC1_PHYS_SIZE   0x00001000ULL
#define MSM8952_GCC_PHYS_BASE    0x01800000ULL
#define MSM8952_GCC_PHYS_SIZE    0x00080000ULL

/* Qualcomm SDCC1 Core Registers (relative to 0x07824000) */
#define SDCC_MCI_HC_MODE         0x78
#define SDCC_CORE_PWRCTL_STATUS  0xDC
#define SDCC_CORE_PWRCTL_MASK    0xE0
#define SDCC_CORE_PWRCTL_CLEAR   0xE4
#define SDCC_CORE_PWRCTL_CTL     0xE8

/* Qualcomm GCC SDCC1 Clock Registers (relative to 0x01800000) */
#define GCC_SDCC1_CMD_RCGR       0x42004
#define GCC_SDCC1_CFG_RCGR       0x42008
#define GCC_SDCC1_M              0x4200C
#define GCC_SDCC1_N              0x42010
#define GCC_SDCC1_D              0x42014
#define GCC_SDCC1_APPS_CBCR      0x42018
#define GCC_SDCC1_AHB_CBCR       0x4201C
#define GCC_APCS_GPLL_ENA_VOTE   0x45000

/* Standard SDHCI Host Controller Registers */
#define SDHCI_DMA_ADDRESS        0x00
#define SDHCI_BLOCK_SIZE         0x04
#define SDHCI_BLOCK_COUNT        0x06
#define SDHCI_ARGUMENT           0x08
#define SDHCI_TRANSFER_MODE      0x0C
#define SDHCI_COMMAND            0x0E
#define SDHCI_RESPONSE           0x10
#define SDHCI_BUFFER             0x20
#define SDHCI_PRESENT_STATE      0x24
#define SDHCI_HOST_CONTROL       0x28
#define SDHCI_POWER_CONTROL      0x29
#define SDHCI_CLOCK_CONTROL      0x2C
#define SDHCI_TIMEOUT_CONTROL    0x2E
#define SDHCI_SOFTWARE_RESET     0x2F
#define SDHCI_INT_STATUS         0x30
#define SDHCI_INT_ENABLE         0x34
#define SDHCI_SIGNAL_ENABLE      0x38
#define SDHCI_CAPABILITIES       0x40
#define SDHCI_HOST_CONTROL2      0x3E

/* Present State Bits */
#define SDHCI_CMD_INHIBIT        0x00000001U
#define SDHCI_DATA_INHIBIT       0x00000002U
#define SDHCI_SPACE_AVAILABLE    0x00000400U
#define SDHCI_DATA_AVAILABLE     0x00000800U

/* Interrupt Status Bits */
#define SDHCI_INT_RESPONSE       0x00000001U
#define SDHCI_INT_DATA_END       0x00000002U
#define SDHCI_INT_SPACE_AVAIL    0x00000010U
#define SDHCI_INT_DATA_AVAIL     0x00000020U
#define SDHCI_INT_ERROR_MASK     0xFFFF8000U

/* Transfer Mode Bits */
#define SDHCI_TRNS_BLK_CNT_EN    0x0002U
#define SDHCI_TRNS_AUTO_CMD12    0x0004U
#define SDHCI_TRNS_READ          0x0010U
#define SDHCI_TRNS_MULTI         0x0020U

/* Command Flags */
#define SDHCI_CMD_RESP_NONE      0x00U
#define SDHCI_CMD_RESP_SHORT     0x02U
#define SDHCI_CMD_RESP_136       0x01U
#define SDHCI_CMD_RESP_48        0x02U     /* the same as SHORT: a 48-bit answer */
#define SDHCI_CMD_RESP_48B       0x03U
#define SDHCI_CMD_CRC            0x08U
#define SDHCI_CMD_INDEX          0x10U
#define SDHCI_CMD_DATA           0x20U
#define SDHCI_MAKE_CMD(c, f)     ((((c) & 0xff) << 8) | ((f) & 0xff))

#define EMMC_SECTOR_SIZE         512U
#define EMMC_DEFAULT_BLOCKS      61079552ULL /* 32 GB eMMC 5.1 (~29.1 GiB) */
#define EMMC_CHUNK_BYTES         (128 * 1024) /* one transfer to the card at a time */

static vm_offset_t sdhci_core_base = 0;
static vm_offset_t sdhci_base = 0;
static vm_offset_t gcc_base = 0;
static bool sdhci_ready = false;
static int sdhci_bmajor = -1;
static int sdhci_cmajor = -1;
static uint64_t emmc_total_sectors = EMMC_DEFAULT_BLOCKS;
/* disk1: a stretch of the card of its own, for an image that is not a partition of the phone
 * (boot-args emmc_slice_lba and emmc_slice_blocks; no such device without them). */
static uint64_t slice_base = 0;
static uint64_t slice_blocks = 0;
static bool slice_ready = false;

static LCK_GRP_DECLARE(sdhci_lck_grp, "hakios-sdhci");
static LCK_MTX_DECLARE(sdhci_lock, &sdhci_lck_grp);

static inline uint32_t rd32(uint32_t reg) {
	return *(volatile uint32_t *)(sdhci_base + reg);
}
static inline void wr32(uint32_t val, uint32_t reg) {
	*(volatile uint32_t *)(sdhci_base + reg) = val;
	__builtin_arm_dsb(0xF);
}
static inline uint16_t rd16(uint32_t reg) {
	return *(volatile uint16_t *)(sdhci_base + reg);
}
static inline void wr16(uint16_t val, uint32_t reg) {
	*(volatile uint16_t *)(sdhci_base + reg) = val;
	__builtin_arm_dsb(0xF);
}
static inline uint8_t rd8(uint32_t reg) {
	return *(volatile uint8_t *)(sdhci_base + reg);
}
static inline void wr8(uint8_t val, uint32_t reg) {
	*(volatile uint8_t *)(sdhci_base + reg) = val;
	__builtin_arm_dsb(0xF);
}

static void sdhci_reset_lines(uint8_t mask) {
	uint32_t cur = rd32(SDHCI_CLOCK_CONTROL) & 0x00FFFFFFU;
	wr32(cur | ((uint32_t)mask << 24), SDHCI_CLOCK_CONTROL);
	for (int i = 0; i < 100000; i++) {
		if (((rd32(SDHCI_CLOCK_CONTROL) >> 24) & mask) == 0)
			return;
	}
}

/*
 * A register poll that does not block. delay() in this kernel goes through the scheduler
 * (assert_wait_deadline, thread_block) and waits for a timer interrupt that never arrives here
 * -- time is kept by polling instead -- so DELAY(1) in a loop of half a million costs half a
 * million scheduler round trips: minutes for a hardware event that takes microseconds. These
 * polls are a bounded count of plain register reads (tens of microseconds in all), and the
 * caller is told what the registers said when one runs out.
 */
static bool sdhci_poll(uint32_t reg, uint32_t mask, bool set, unsigned spins) {
	for (unsigned i = 0; i < spins; i++) {
		if (((rd32(reg) & mask) != 0) == set) {
			return true;
		}
	}
	return false;
}

static void sdhci_say_state(const char *what, uint32_t lba) {
	printf("[SDHCI] %s lba %u: PRESENT_STATE=0x%08x INT_STATUS=0x%08x CLOCK=0x%04x POWER=0x%02x HOST=0x%02x\n",
	    what, lba, rd32(SDHCI_PRESENT_STATE), rd32(SDHCI_INT_STATUS), rd16(SDHCI_CLOCK_CONTROL),
	    rd8(SDHCI_POWER_CONTROL), rd8(SDHCI_HOST_CONTROL));
}

/*
 * The card, from nothing.
 *
 * What the loader hands over is a controller with the bus power off and the SD clock stopped
 * (POWER=0x00, CLOCK=0x8000 at attach): it shuts its own driver down before jumping here. The
 * card is therefore back in its idle state too, and has to be woken and identified again before
 * a single sector can be read -- at a clock slow enough for the first commands, then faster.
 *
 * CLOCK_CONTROL is the standard SDHCI one: [0] internal clock, [1] it is stable (read only),
 * [2] SD clock on the bus, [9:8]+[7:0] the ten-bit divider, clock = base / (2 * divider). The
 * base is the controller's own answer in CAPS[15:8] (MHz), so no frequency is assumed here.
 */
#define SDHCI_CLOCK_INT_EN       0x0001U
#define SDHCI_CLOCK_INT_STABLE   0x0002U
#define SDHCI_CLOCK_SD_EN        0x0004U

static uint32_t emmc_rca;

static inline uint32_t core_rd32(uint32_t reg) {
	return *(volatile uint32_t *)(sdhci_core_base + reg);
}
static inline void core_wr32(uint32_t val, uint32_t reg) {
	*(volatile uint32_t *)(sdhci_core_base + reg) = val;
	__builtin_arm_dsb(0xF);
}
static inline uint32_t gcc_rd32(uint32_t reg) {
	return *(volatile uint32_t *)(gcc_base + reg);
}
static inline void gcc_wr32(uint32_t val, uint32_t reg) {
	*(volatile uint32_t *)(gcc_base + reg) = val;
	__builtin_arm_dsb(0xF);
}

static void sdhci_handle_pwrctl(void) {
	if (!sdhci_core_base)
		return;
	uint32_t st = core_rd32(SDCC_CORE_PWRCTL_STATUS) & 0x0FU;
	if (!st)
		return;
	core_wr32(st, SDCC_CORE_PWRCTL_CLEAR);
	uint32_t ctl = 0;
	if (st & 0x03U)
		ctl |= 0x01U;
	if (st & 0x0CU)
		ctl |= 0x04U;
	core_wr32(ctl, SDCC_CORE_PWRCTL_CTL);
}

static void sdhci_wait_pwrctl(void) {
	if (!sdhci_core_base)
		return;
	/* Wait up to ~1ms (100000 MMIO reads) for the 400kHz SDCC domain to latch status into 0xDC */
	for (int i = 0; i < 100000; i++) {
		uint32_t st = core_rd32(SDCC_CORE_PWRCTL_STATUS) & 0x0FU;
		if (st) {
			core_wr32(st, SDCC_CORE_PWRCTL_CLEAR);
			uint32_t ctl = 0;
			if (st & 0x03U)
				ctl |= 0x01U;
			if (st & 0x0CU)
				ctl |= 0x04U;
			core_wr32(ctl, SDCC_CORE_PWRCTL_CTL);
			/* Wait a few more 400kHz cycles in case a second transition (e.g. IO_HIGH) follows */
			for (int j = 0; j < 20000; j++) {
				uint32_t st2 = core_rd32(SDCC_CORE_PWRCTL_STATUS) & 0x0FU;
				if (st2) {
					core_wr32(st2, SDCC_CORE_PWRCTL_CLEAR);
					uint32_t ctl2 = 0;
					if (st2 & 0x03U)
						ctl2 |= 0x01U;
					if (st2 & 0x0CU)
						ctl2 |= 0x04U;
					core_wr32(ctl2, SDCC_CORE_PWRCTL_CTL);
				}
			}
			return;
		}
	}
}

static void sdhci_gcc_set_rate(uint32_t hz) {
	if (!gcc_base)
		return;
	/* Ensure GPLL0 vote and SDCC1 AHB + APPS branch clocks are enabled */
	gcc_wr32(gcc_rd32(GCC_APCS_GPLL_ENA_VOTE) | 1U, GCC_APCS_GPLL_ENA_VOTE);
	gcc_wr32(gcc_rd32(GCC_SDCC1_AHB_CBCR) | 1U, GCC_SDCC1_AHB_CBCR);
	gcc_wr32(gcc_rd32(GCC_SDCC1_APPS_CBCR) | 1U, GCC_SDCC1_APPS_CBCR);

	if (hz <= 400000U) {
		/* 400 kHz from CXO (19.2 MHz, dual-edge MND: cfg=0x2017, M=1, N=0xfffffffc, D=0xfffffffb) */
		gcc_wr32(0x00002017U, GCC_SDCC1_CFG_RCGR);
		gcc_wr32(0x00000001U, GCC_SDCC1_M);
		gcc_wr32(0xFFFFFFFCU, GCC_SDCC1_N);
		gcc_wr32(0xFFFFFFFBU, GCC_SDCC1_D);
	} else {
		/* 50 MHz from GPLL0 (800 MHz, bypass MND: cfg=0x011f, M=0, N=0, D=0xffffffff) */
		gcc_wr32(0x0000011FU, GCC_SDCC1_CFG_RCGR);
		gcc_wr32(0x00000000U, GCC_SDCC1_M);
		gcc_wr32(0x00000000U, GCC_SDCC1_N);
		gcc_wr32(0xFFFFFFFFU, GCC_SDCC1_D);
	}
	gcc_wr32(0x00000003U, GCC_SDCC1_CMD_RCGR); /* ROOT_EN | UPDATE */
	for (int i = 0; i < 100000; i++) {
		if ((gcc_rd32(GCC_SDCC1_CMD_RCGR) & 1U) == 0)
			break;
	}
}

static bool sdhci_set_clock(uint32_t hz) {
	sdhci_gcc_set_rate(hz);
	/* On Qualcomm SDCC, GCC RCG provides the exact target clock, so SDHCI internal divider = 0 (1:1) */
	wr32(0x000E0001U, SDHCI_CLOCK_CONTROL);
	for (int i = 0; i < 200000; i++) {
		if (rd32(SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) {
			wr32(0x000E0007U, SDHCI_CLOCK_CONTROL);
			printf("[SDHCI] clock: GCC RCG set to %u Hz, CLOCK_CONTROL=0x%04x\n",
			    hz, (uint16_t)rd32(SDHCI_CLOCK_CONTROL));
			return true;
		}
	}
	printf("[SDHCI] clock: internal clock never settled (CLOCK_CONTROL=0x%04x)\n",
	    (uint16_t)rd32(SDHCI_CLOCK_CONTROL));
	return false;
}

static int sdhci_cmd(uint8_t idx, uint32_t arg, uint16_t flags, uint32_t *resp) {
	if (!sdhci_poll(SDHCI_PRESENT_STATE, SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT, false, 200000)) {
		printf("[SDHCI] cmd %u: the controller stays inhibited (PRESENT_STATE=0x%08x)\n", idx, rd32(SDHCI_PRESENT_STATE));
		return EBUSY;
	}
	wr32(0xFFFFFFFFU, SDHCI_INT_STATUS);
	wr32(arg, SDHCI_ARGUMENT);
	wr32((uint32_t)SDHCI_MAKE_CMD(idx, flags) << 16, SDHCI_TRANSFER_MODE);

	if ((flags & 0x03) == 0) {
		(void)sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE, true, 200000);
		wr32(0xFFFFFFFFU, SDHCI_INT_STATUS);
		return 0;
	}
	if (!sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE | SDHCI_INT_ERROR_MASK, true, 200000)) {
		printf("[SDHCI] cmd %u: no answer (INT_STATUS=0x%08x CLOCK=0x%04x POWER=0x%02x)\n",
		    idx, rd32(SDHCI_INT_STATUS), rd16(SDHCI_CLOCK_CONTROL), rd8(SDHCI_POWER_CONTROL));
		return ETIMEDOUT;
	}
	if (rd32(SDHCI_INT_STATUS) & SDHCI_INT_ERROR_MASK) {
		printf("[SDHCI] cmd %u: failed (INT_STATUS=0x%08x)\n", idx, rd32(SDHCI_INT_STATUS));
		sdhci_reset_lines(0x06);
		return EIO;
	}
	if (resp) {
		resp[0] = rd32(SDHCI_RESPONSE);
		resp[1] = rd32(SDHCI_RESPONSE + 4);
		resp[2] = rd32(SDHCI_RESPONSE + 8);
		resp[3] = rd32(SDHCI_RESPONSE + 12);
	}
	wr32(0xFFFFFFFFU, SDHCI_INT_STATUS);
	return 0;
}

/* One block of 512 bytes for a command already issued with the data flag. */
static int sdhci_data_in(void *dst) {
	uint32_t *buf32 = (uint32_t *)dst;

	if (!sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL | SDHCI_INT_ERROR_MASK, true, 500000)) {
		sdhci_say_state("data: nothing came back at", emmc_rca);
		sdhci_reset_lines(0x06);
		return ETIMEDOUT;
	}
	if (rd32(SDHCI_INT_STATUS) & SDHCI_INT_ERROR_MASK) {
		sdhci_say_state("data: the transfer failed at", emmc_rca);
		sdhci_reset_lines(0x06);
		return EIO;
	}
	wr32(SDHCI_INT_DATA_AVAIL, SDHCI_INT_STATUS);
	for (int w = 0; w < (EMMC_SECTOR_SIZE / 4); w++) {
		*buf32++ = rd32(SDHCI_BUFFER);
	}
	(void)sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_DATA_END, true, 500000);
	wr32(0xFFFFFFFFU, SDHCI_INT_STATUS);
	return 0;
}

/* BRINGUP: the whole block, both halves, so that a boot says what the loader left and what the
 * init above managed to change. Four words a line, the standard registers then the vendor's. */
static void sdhci_dump(const char *when) __attribute__((unused));
static void sdhci_dump(const char *when) {
	printf("[SDHCI] %s 00: %08x %08x %08x %08x\n", when, rd32(0x00), rd32(0x04), rd32(0x08), rd32(0x0C));
	printf("[SDHCI] %s 10: %08x %08x %08x %08x\n", when, rd32(0x10), rd32(0x14), rd32(0x18), rd32(0x1C));
	printf("[SDHCI] %s 20: %08x %08x %08x %08x\n", when, rd32(0x20), rd32(0x24), rd32(0x28), rd32(0x2C));
	printf("[SDHCI] %s 30: %08x %08x %08x %08x\n", when, rd32(0x30), rd32(0x34), rd32(0x38), rd32(0x3C));
	printf("[SDHCI] %s 40: %08x %08x %08x %08x\n", when, rd32(0x40), rd32(0x44), rd32(0x48), rd32(0x4C));
	printf("[SDHCI] %s 100: %08x %08x %08x %08x\n", when, rd32(0x100), rd32(0x104), rd32(0x108), rd32(0x10C));
	printf("[SDHCI] %s 110: %08x %08x %08x %08x\n", when, rd32(0x110), rd32(0x114), rd32(0x118), rd32(0x11C));
}

static int sdhci_read_sectors_pio(uint32_t lba, uint16_t count, void *dst);
static int sdhci_write_sectors_pio(uint32_t lba, uint16_t count, const void *src);

static int sdhci_card_init(void) {
	static uint8_t ext_csd[EMMC_SECTOR_SIZE];
	uint32_t resp[4];
	uint32_t ocr = 0;
	int err;

	/*
	 * Fast path: lk2nd already initialized eMMC (RCA=1, selected in Transfer state,
	 * 50 MHz GPLL0 clock, 8-bit bus, POWER=0x0B, CLOCK=0x0007, RESP0=0x00000700).
	 * Try reading LBA 0 directly before resetting the controller!
	 */
	if (sdhci_core_base) {
		core_wr32(0x00000000U, SDCC_CORE_PWRCTL_MASK);
		sdhci_handle_pwrctl();
	}
	wr32(0xFFFFFFFFU, SDHCI_INT_ENABLE);
	wr32(0x00000000U, SDHCI_SIGNAL_ENABLE);
	emmc_rca = 1U << 16;

	/* Ensure timeout counter is set to 0x0E without disturbing existing clock/power */
	wr32((rd32(SDHCI_CLOCK_CONTROL) & 0x0000FFFFU) | 0x000E0000U, SDHCI_CLOCK_CONTROL);

	if (sdhci_read_sectors_pio(0, 1, ext_csd) == 0 &&
	    ext_csd[510] == 0x55 && ext_csd[511] == 0xAA) {
		printf("[SDHCI] fast-path: lk2nd card state preserved! MBR magic 55AA read at 50 MHz (HOST=0x%02x POWER=0x%02x CLOCK=0x%04x)\n",
		    rd8(SDHCI_HOST_CONTROL), rd8(SDHCI_POWER_CONTROL), rd16(SDHCI_CLOCK_CONTROL));
		return 0;
	}

	printf("[SDHCI] waking the card up: power, clock, identification\n");

	/* 1. Set 400 kHz RCG clock BEFORE software reset */
	sdhci_gcc_set_rate(400000);

	/* 2. Disable CORE_PWRCTL_MASK (0xE0 = 0) so GIC SPI 253 cannot storm, then enable MCI_HC_MODE = 0x2001 */
	if (sdhci_core_base) {
		core_wr32(0x00000000U, SDCC_CORE_PWRCTL_MASK);
		core_wr32(0x00002001U, SDCC_MCI_HC_MODE);
		sdhci_handle_pwrctl();
	}

	/* 3. Issue SW_RESET_ALL via 32-bit write to 0x2C (bit 24 = SW_RESET_ALL) */
	wr32(0x010E0000U, SDHCI_CLOCK_CONTROL);
	for (int i = 0; i < 100000 && (rd32(SDHCI_CLOCK_CONTROL) & 0x01000000U); i++) {
	}

	/* 4. Re-assert MCI_HC_MODE = 0x2001 AFTER SW_RESET_ALL and wait for BUS_OFF PWRCTL handshake */
	if (sdhci_core_base) {
		core_wr32(0x00000000U, SDCC_CORE_PWRCTL_MASK);
		core_wr32(0x00002001U, SDCC_MCI_HC_MODE);
		sdhci_wait_pwrctl();
	}

	/* 5. Clear HS200/HS400 DLL select bits [21:19] in VENDOR_SPEC (0x10C) and set PWRSAVE [16] */
	wr32((rd32(0x10C) & ~(7U << 19)) | (1U << 16), 0x10C);

	/* 6. Bus power 3.3V (0x0F at 0x29), 1-bit host control (0x00 at 0x28) via 32-bit write */
	wr32(0x00000F00U, SDHCI_HOST_CONTROL);
	sdhci_wait_pwrctl();

	/* 7. Enable 400 kHz SD clock and timeout (0x000E0007 at 0x2C) */
	wr32(0x000E0007U, SDHCI_CLOCK_CONTROL);
	for (int i = 0; i < 100000; i++) {
		if ((rd32(SDHCI_CLOCK_CONTROL) & 0x0007U) == 0x0007U)
			break;
	}
	wr32(0xFFFFFFFFU, SDHCI_INT_ENABLE);
	wr32(0x00000000U, SDHCI_SIGNAL_ENABLE);

	printf("[SDHCI] clock: CLOCK_CONTROL=0x%04x POWER=0x%02x MCI_HC_MODE=0x%08x PWRCTL=0x%x/0x%x\n",
	    (uint16_t)rd32(SDHCI_CLOCK_CONTROL),
	    rd8(SDHCI_POWER_CONTROL),
	    sdhci_core_base ? core_rd32(SDCC_MCI_HC_MODE) : 0,
	    sdhci_core_base ? core_rd32(SDCC_CORE_PWRCTL_STATUS) : 0,
	    sdhci_core_base ? core_rd32(SDCC_CORE_PWRCTL_CTL) : 0);

	/* Give eMMC card 2ms (~74 clock cycles at 400kHz) to wake up before CMD0 */
	for (int i = 0; i < 100000; i++) {
		(void)rd32(SDHCI_PRESENT_STATE);
	}
	err = sdhci_cmd(0, 0, SDHCI_CMD_RESP_NONE, NULL);                /* CMD0: the card to idle */
	if (err != 0) {
		printf("[SDHCI] cmd 0 (go idle): %d\n", err);
	}
	/* Give eMMC card 2ms after CMD0 before first CMD1 */
	for (int i = 0; i < 100000; i++) {
		(void)rd32(SDHCI_PRESENT_STATE);
	}

	for (int i = 0; i < 200; i++) {
		err = sdhci_cmd(1, 0x40FF8080, SDHCI_CMD_RESP_SHORT, resp);  /* CMD1 R3 has no CRC/index */
		if (err == 0) {
			ocr = resp[0];
			if (ocr & 0x80000000U) {
				break;
			}
		} else {
			/* Re-send CMD0 if first CMD1 timed out while card was waking up */
			(void)sdhci_cmd(0, 0, SDHCI_CMD_RESP_NONE, NULL);
		}
		/* Wait ~1ms between CMD1 polls so card internal charge pump completes */
		for (int d = 0; d < 50000; d++) {
			(void)rd32(SDHCI_PRESENT_STATE);
		}
	}
	if (err != 0 || (ocr & 0x80000000U) == 0) {
		printf("[SDHCI] cmd 1 (send op cond): %d, OCR=0x%08x\n", err, ocr);
		return EIO;
	}
	printf("[SDHCI] OCR=0x%08x\n", ocr);

	err = sdhci_cmd(2, 0, SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC, resp); /* CMD2 R2 has CRC, no index */
	if (err != 0) {
		printf("[SDHCI] cmd 2 (send cid): %d\n", err);
		return EIO;
	}
	printf("[SDHCI] CID=%08x%08x%08x%08x\n", resp[0], resp[1], resp[2], resp[3]);

	emmc_rca = 1U << 16;
	err = sdhci_cmd(3, emmc_rca, SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, resp);   /* CMD3: assign RCA 1 */
	if (err != 0) {
		printf("[SDHCI] cmd 3 (set relative address): %d\n", err);
		return EIO;
	}
	printf("[SDHCI] RCA=0x%08x (R1=0x%08x)\n", emmc_rca, resp[0]);

	err = sdhci_cmd(7, emmc_rca, SDHCI_CMD_RESP_48B | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, resp);   /* CMD7: select */
	if (err != 0) {
		printf("[SDHCI] cmd 7 (select): %d\n", err);
		return EIO;
	}
	err = sdhci_cmd(16, EMMC_SECTOR_SIZE, SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, resp);
	if (err != 0) {
		printf("[SDHCI] cmd 16 (block length): %d\n", err);
		return EIO;
	}

	/* Switch to 50 MHz GPLL0 for data transfers */
	(void)sdhci_set_clock(50000000);

	/* EXT_CSD: how many sectors the card has (bytes 212..215) */
	wr32(0xFFFFFFFFU, SDHCI_INT_STATUS);
	wr32((1U << 16) | EMMC_SECTOR_SIZE, SDHCI_BLOCK_SIZE);
	wr32(0, SDHCI_ARGUMENT);
	wr32(((uint32_t)SDHCI_MAKE_CMD(8, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA) << 16) |
	     (SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN), SDHCI_TRANSFER_MODE);
	err = 0;
	if (!sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE | SDHCI_INT_ERROR_MASK, true, 200000) ||
	    (rd32(SDHCI_INT_STATUS) & SDHCI_INT_ERROR_MASK)) {
		err = EIO;
	} else {
		wr32(SDHCI_INT_RESPONSE, SDHCI_INT_STATUS);
		err = sdhci_data_in(ext_csd);
	}
	if (err == 0) {
		uint32_t sectors = (uint32_t)ext_csd[212] | ((uint32_t)ext_csd[213] << 8) |
		    ((uint32_t)ext_csd[214] << 16) | ((uint32_t)ext_csd[215] << 24);

		if (sectors > 1024 && sectors < 0x40000000U) {
			emmc_total_sectors = sectors;
		}
		printf("[SDHCI] EXT_CSD: rev %u.%u, %u sectors (%llu MB)\n", ext_csd[192] >> 4, ext_csd[192] & 0x0F,
		    sectors, (uint64_t)emmc_total_sectors * EMMC_SECTOR_SIZE / (1024 * 1024));
	} else {
		printf("[SDHCI] EXT_CSD: not read (%d), the size the loader said is kept\n", err);
	}

	printf("[SDHCI] card ready: HOST=0x%02x CLOCK=0x%04x\n", rd8(SDHCI_HOST_CONTROL), rd16(SDHCI_CLOCK_CONTROL));

	return 0;
}

static int sdhci_read_sectors_pio(uint32_t lba, uint16_t count, void *dst) {
	uint32_t *buf32 = (uint32_t *)dst;

	/* 1. Wait for command and data lines to be idle */
	if (!sdhci_poll(SDHCI_PRESENT_STATE, SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT, false, 200000)) {
		sdhci_say_state("read: the controller stays inhibited at", lba);
		return EBUSY;
	}

	wr32(0xFFFFFFFFU, SDHCI_INT_STATUS);
	wr32(((uint32_t)count << 16) | EMMC_SECTOR_SIZE, SDHCI_BLOCK_SIZE);
	wr32(lba, SDHCI_ARGUMENT);

	uint16_t mode = SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN;
	uint16_t cmd_idx = 17; /* CMD17: READ_SINGLE_BLOCK */
	if (count > 1) {
		mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
		cmd_idx = 18; /* CMD18: READ_MULTIPLE_BLOCK */
	}

	wr32(((uint32_t)SDHCI_MAKE_CMD(cmd_idx, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
	                               SDHCI_CMD_INDEX | SDHCI_CMD_DATA) << 16) | mode,
	     SDHCI_TRANSFER_MODE);

	/* 2. Wait for command response */
	if (!sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE | SDHCI_INT_ERROR_MASK, true, 200000)) {
		sdhci_say_state("read: no answer to the command at", lba);
		sdhci_reset_lines(0x06);
		return ETIMEDOUT;
	}
	if (rd32(SDHCI_INT_STATUS) & SDHCI_INT_ERROR_MASK) {
		sdhci_say_state("read: the command failed at", lba);
		sdhci_reset_lines(0x06);
		return EIO;
	}
	wr32(SDHCI_INT_RESPONSE, SDHCI_INT_STATUS);

	/* 3. Read sectors via 32-bit PIO FIFO */
	for (uint16_t b = 0; b < count; b++) {
		if (!sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL | SDHCI_INT_ERROR_MASK, true, 500000)) {
			sdhci_say_state("read: no data at", lba + b);
			sdhci_reset_lines(0x06);
			return ETIMEDOUT;
		}
		if (rd32(SDHCI_INT_STATUS) & SDHCI_INT_ERROR_MASK) {
			sdhci_say_state("read: the transfer failed at", lba + b);
			sdhci_reset_lines(0x06);
			return EIO;
		}
		wr32(SDHCI_INT_DATA_AVAIL, SDHCI_INT_STATUS);

		for (int w = 0; w < (EMMC_SECTOR_SIZE / 4); w++) {
			*buf32++ = rd32(SDHCI_BUFFER);
		}
	}

	/* 4. Wait for transfer complete */
	(void)sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_DATA_END, true, 500000);
	wr32(0xFFFFFFFFU, SDHCI_INT_STATUS);

	return 0;
}

/*
 * Sectors to the card, the same way the read above takes them: the FIFO, a sector at a time.
 * The card is the one the loader left clocked, so only the transfer itself is here; CMD24 is one
 * sector, CMD25 many with the stop command sent by the controller (AUTO_CMD12).
 */
static int sdhci_write_sectors_pio(uint32_t lba, uint16_t count, const void *src) {
	const uint32_t *buf32 = (const uint32_t *)src;

	if (!sdhci_poll(SDHCI_PRESENT_STATE, SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT, false, 200000)) {
		sdhci_say_state("write: the controller stays inhibited at", lba);
		return EBUSY;
	}

	wr32(0xFFFFFFFFU, SDHCI_INT_STATUS);
	wr32(((uint32_t)count << 16) | EMMC_SECTOR_SIZE, SDHCI_BLOCK_SIZE);
	wr32(lba, SDHCI_ARGUMENT);

	uint16_t mode = SDHCI_TRNS_BLK_CNT_EN;
	uint16_t cmd_idx = 24; /* CMD24: WRITE_SINGLE_BLOCK */
	if (count > 1) {
		mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
		cmd_idx = 25; /* CMD25: WRITE_MULTIPLE_BLOCK */
	}

	wr32(((uint32_t)SDHCI_MAKE_CMD(cmd_idx, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
	                               SDHCI_CMD_INDEX | SDHCI_CMD_DATA) << 16) | mode,
	     SDHCI_TRANSFER_MODE);

	if (!sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE | SDHCI_INT_ERROR_MASK, true, 200000)) {
		sdhci_say_state("write: no answer to the command at", lba);
		sdhci_reset_lines(0x06);
		return ETIMEDOUT;
	}
	if (rd32(SDHCI_INT_STATUS) & SDHCI_INT_ERROR_MASK) {
		sdhci_say_state("write: the command failed at", lba);
		sdhci_reset_lines(0x06);
		return EIO;
	}
	wr32(SDHCI_INT_RESPONSE, SDHCI_INT_STATUS);

	for (uint16_t b = 0; b < count; b++) {
		if (!sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_SPACE_AVAIL | SDHCI_INT_ERROR_MASK, true, 500000)) {
			sdhci_say_state("write: no room in the buffer at", lba + b);
			sdhci_reset_lines(0x06);
			return ETIMEDOUT;
		}
		if (rd32(SDHCI_INT_STATUS) & SDHCI_INT_ERROR_MASK) {
			sdhci_say_state("write: the transfer failed at", lba + b);
			sdhci_reset_lines(0x06);
			return EIO;
		}
		wr32(SDHCI_INT_SPACE_AVAIL, SDHCI_INT_STATUS);

		for (int w = 0; w < (EMMC_SECTOR_SIZE / 4); w++) {
			wr32(*buf32++, SDHCI_BUFFER);
		}
	}

	if (!sdhci_poll(SDHCI_INT_STATUS, SDHCI_INT_DATA_END | SDHCI_INT_ERROR_MASK, true, 500000) ||
	    (rd32(SDHCI_INT_STATUS) & SDHCI_INT_ERROR_MASK)) {
		sdhci_say_state("write: the transfer did not finish at", lba + count - 1);
		sdhci_reset_lines(0x06);
		return EIO;
	}
	wr32(0xFFFFFFFFU, SDHCI_INT_STATUS);

	return 0;
}

/*
 * BRINGUP: what the flash holds, so that the 2.5 GB iOS root image can be put in a part of it
 * that belongs to nothing else. Read once at boot, said in the log, and never read again.
 */
static uint8_t sdhci_gpt_buf[512 * 40];

/* The table, read once: its entry count (0 if there is none) and the size of an entry. */
static uint32_t sdhci_gpt_load(uint32_t *entry_size) {
	uint8_t *hdr = sdhci_gpt_buf + 512;
	uint64_t entries_lba;
	uint32_t entries, size, sectors;
	int rerr;

	rerr = sdhci_read_sectors_pio(0, 2, sdhci_gpt_buf);
	if (rerr != 0) {
		printf("[SDHCI] gpt: failed to read lba 0..1 (err=%d)\n", rerr);
		return 0;
	}
	if (memcmp(hdr, "EFI PART", 8) != 0) {
		printf("[SDHCI] gpt: lba 1 is not EFI PART (hdr=%02x %02x %02x %02x \"%c%c%c%c\")\n",
		    hdr[0], hdr[1], hdr[2], hdr[3],
		    hdr[0] >= 32 && hdr[0] < 127 ? hdr[0] : '.',
		    hdr[1] >= 32 && hdr[1] < 127 ? hdr[1] : '.',
		    hdr[2] >= 32 && hdr[2] < 127 ? hdr[2] : '.',
		    hdr[3] >= 32 && hdr[3] < 127 ? hdr[3] : '.');
		return 0;
	}
	entries_lba = *(uint64_t *)(void *)(hdr + 72);
	entries = *(uint32_t *)(void *)(hdr + 80);
	size = *(uint32_t *)(void *)(hdr + 84);
	if (entries == 0 || size < 128 || size > 512 || entries > 128) {
		printf("[SDHCI] gpt: invalid entries count=%u or size=%u\n", entries, size);
		return 0;
	}
	sectors = (entries * size + 511) / 512;
	if (sectors * 512 > sizeof(sdhci_gpt_buf))
		sectors = sizeof(sdhci_gpt_buf) / 512;
	rerr = sdhci_read_sectors_pio((uint32_t)entries_lba, (uint16_t)sectors, sdhci_gpt_buf);
	if (rerr != 0) {
		printf("[SDHCI] gpt: failed to read entries at lba %llu (err=%d)\n", entries_lba, rerr);
		return 0;
	}
	*entry_size = size;
	return entries;
}

/*
 * A partition by name (boot-arg emmc_slice_part=userdata): the stretch of the card that the
 * phone's own table gives that name, so that the image goes where the name says and no offset
 * has to be carried around by hand.
 */
static bool sdhci_gpt_find(const char *want, uint64_t *out_first, uint64_t *out_count) {
	uint32_t size = 0, entries = sdhci_gpt_load(&size);

	for (uint32_t i = 0; i < entries; i++) {
		const uint8_t *e = sdhci_gpt_buf + i * size;
		const uint16_t *name = (const uint16_t *)(const void *)(e + 56);
		uint64_t first = *(const uint64_t *)(const void *)(e + 32);
		uint64_t last = *(const uint64_t *)(const void *)(e + 40);
		int n = 0;

		if (first == 0 || last < first) {
			continue;
		}
		while (n < 36 && name[n] != 0 && want[n] != 0 && (char)name[n] == want[n]) {
			n++;
		}
		if (n > 0 && want[n] == 0 && (n == 36 || name[n] == 0)) {
			*out_first = first;
			*out_count = last - first + 1;
			return true;
		}
	}
	return false;
}

static void sdhci_print_gpt(void) {
	uint32_t size = 0, entries = sdhci_gpt_load(&size), printed = 0;

	if (entries == 0) {
		printf("[SDHCI] no GPT on the card\n");
		return;
	}
	printf("[SDHCI] GPT: %u entries\n", entries);
	for (uint32_t i = 0; i < entries; i++) {
		const uint8_t *e = sdhci_gpt_buf + i * size;
		const uint16_t *name = (const uint16_t *)(const void *)(e + 56);
		uint64_t first = *(uint64_t *)(const void *)(e + 32), last = *(uint64_t *)(const void *)(e + 40);
		char text[37];
		int n = 0;
		bool empty = true;

		for (int c = 0; c < 36 && name[c] != 0; c++) {
			text[n++] = name[c] < 0x80 ? (char)name[c] : '?';
			empty = false;
		}
		text[n] = 0;
		for (int c = 0; c < 16; c++) {
			if (e[c] != 0) {
				empty = false;
			}
		}
		if (empty) {
			continue;
		}
		printf("[SDHCI] GPT %2u: lba %llu..%llu (%llu MB) \"%s\"\n", i, first, last,
		    (last - first + 1) * EMMC_SECTOR_SIZE / (1024 * 1024), text);
		printed++;
	}
	printf("[SDHCI] GPT: %u partitions\n", printed);
}

/* ---- BSD Device Interface ---- */

static int sdhci_open(__unused dev_t dev, __unused int flag, __unused int devtype, __unused struct proc *p) {
	return sdhci_ready ? 0 : ENXIO;
}

static int sdhci_close(__unused dev_t dev, __unused int flag, __unused int devtype, __unused struct proc *p) {
	return 0;
}

static void sdhci_strategy(struct buf *bp) {
	uint64_t blkno = buf_blkno(bp);
	uint64_t base = 0;
	uint64_t count = emmc_total_sectors;
	uint32_t bcount = buf_count(bp);
	void *data = (void *)buf_dataptr(bp);
	int err = 0;

	if (!sdhci_ready) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	if (minor(buf_device(bp)) == 1) {
		/* disk1: a stretch of the same card, so that an image can live beside the phone's own partitions */
		if (!slice_ready) {
			buf_seterror(bp, ENXIO);
			buf_biodone(bp);
			return;
		}
		base = slice_base;
		count = slice_blocks;
	}
	if (bcount == 0) {
		buf_biodone(bp);
		return;
	}
	if (blkno + (bcount + EMMC_SECTOR_SIZE - 1) / EMMC_SECTOR_SIZE > count) {
		buf_seterror(bp, EIO);
		buf_biodone(bp);
		return;
	}

	lck_mtx_lock(&sdhci_lock);
	while (bcount != 0 && err == 0) {
		uint32_t bytes = bcount < EMMC_CHUNK_BYTES ? bcount : EMMC_CHUNK_BYTES;
		uint16_t sectors = (bytes + EMMC_SECTOR_SIZE - 1) / EMMC_SECTOR_SIZE;

		if (buf_flags(bp) & B_READ) {
			err = sdhci_read_sectors_pio((uint32_t)(base + blkno), sectors, data);
		} else {
			err = sdhci_write_sectors_pio((uint32_t)(base + blkno), sectors, data);
		}
		blkno += sectors;
		data = (uint8_t *)data + (size_t)sectors * EMMC_SECTOR_SIZE;
		bcount = bcount > (uint32_t)sectors * EMMC_SECTOR_SIZE ? bcount - (uint32_t)sectors * EMMC_SECTOR_SIZE : 0;
	}
	lck_mtx_unlock(&sdhci_lock);

	if (err) {
		buf_seterror(bp, err);
	} else {
		buf_setresid(bp, 0);
	}
	buf_biodone(bp);
}

static int sdhci_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p) {
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = EMMC_SECTOR_SIZE;
		return 0;
	case DKIOCSETBLOCKSIZE:
		if (*(uint32_t *)data != EMMC_SECTOR_SIZE)
			return EINVAL;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = emmc_total_sectors;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 1;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = 256; /* 128 KB max read */
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = 256;
		return 0;
	case DKIOCGETFEATURES:
		*(uint32_t *)data = 0;
		return 0;
	default:
		return ENOTTY;
	}
}

static int sdhci_size(__unused dev_t dev) {
	return sdhci_ready ? EMMC_SECTOR_SIZE : -1;
}

static int sdhci_read(dev_t dev, struct uio *uio, int ioflag) {
	return physio(sdhci_strategy, NULL, dev, B_READ, (u_int (*)(buf_t))sdhci_size, uio, EMMC_SECTOR_SIZE);
}

static int sdhci_write(dev_t dev, struct uio *uio, int ioflag) {
	return physio(sdhci_strategy, NULL, dev, B_WRITE, (u_int (*)(buf_t))sdhci_size, uio, EMMC_SECTOR_SIZE);
}

static const struct bdevsw sdhci_bdevsw = {
	.d_open     = sdhci_open,
	.d_close    = sdhci_close,
	.d_strategy = sdhci_strategy,
	.d_ioctl    = sdhci_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = sdhci_size,
	.d_type     = D_DISK,
};

static const struct cdevsw sdhci_cdevsw = {
	.d_open       = sdhci_open,
	.d_close      = sdhci_close,
	.d_read       = sdhci_read,
	.d_write      = sdhci_write,
	.d_ioctl      = sdhci_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

void hakios_sdhci_init(void) {
	extern vm_offset_t pe_arm_get_soc_base_phys(void);
	if (pe_arm_get_soc_base_phys() == 0)
		return; /* QEMU virt: do not touch Qualcomm SDHCI MMIO */

	uint32_t enabled = 1;
	(void)PE_parse_boot_argn("sdhci", &enabled, sizeof(enabled));
	if (!enabled)
		return;

	sdhci_core_base = ml_io_map(MSM8952_SDC1_CORE_BASE, MSM8952_SDC1_CORE_SIZE);
	sdhci_base = ml_io_map(MSM8952_SDC1_PHYS_BASE, MSM8952_SDC1_PHYS_SIZE);
	gcc_base = ml_io_map(MSM8952_GCC_PHYS_BASE, MSM8952_GCC_PHYS_SIZE);
	if (!sdhci_base || !sdhci_core_base || !gcc_base) {
		printf("[SDHCI] Failed to map MSM8952 SDCC1/GCC at 0x%llx\n", MSM8952_SDC1_PHYS_BASE);
		return;
	}

	lck_mtx_init(&sdhci_lock, &sdhci_lck_grp, LCK_ATTR_NULL);

	/*
	 * What the loader left the controller in: the driver does no reset nor clock setup of its
	 * own, so this line is the whole picture of whether it can talk to the card at all.
	 */
	printf("[SDHCI] controller: PRESENT_STATE=0x%08x INT_STATUS=0x%08x CLOCK=0x%04x POWER=0x%02x "
	    "HOST=0x%02x CAPS=0x%08x RESP0=0x%08x\n",
	    rd32(SDHCI_PRESENT_STATE), rd32(SDHCI_INT_STATUS), rd16(SDHCI_CLOCK_CONTROL),
	    rd8(SDHCI_POWER_CONTROL), rd8(SDHCI_HOST_CONTROL), rd32(0x40), rd32(SDHCI_RESPONSE));

	/* The loader shut its card driver down before jumping here: bring the card back. */
	{
		int card_err = sdhci_card_init();

		printf("[SDHCI] power register reads 0x%02x\n", rd8(SDHCI_POWER_CONTROL));
		if (card_err != 0) {
			printf("[SDHCI] the card did not come up: no sector can be read nor written\n");
		}
	}

	sdhci_bmajor = bdevsw_add(-1, &sdhci_bdevsw);
	if (sdhci_bmajor < 0) {
		printf("[SDHCI] bdevsw_add failed\n");
		return;
	}

	sdhci_cmajor = cdevsw_add_with_bdev(-1, &sdhci_cdevsw, sdhci_bmajor);
	if (sdhci_cmajor < 0) {
		printf("[SDHCI] cdevsw_add failed\n");
		return;
	}

	if (devfs_make_node(makedev(sdhci_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0660, "disk0") == NULL ||
	    devfs_make_node(makedev(sdhci_cmajor, 0), DEVFS_CHAR,  UID_ROOT, GID_OPERATOR, 0660, "rdisk0") == NULL) {
		printf("[SDHCI] devfs_make_node disk0/rdisk0 failed\n");
		return;
	}

	/*
	 * A stretch of the same card as a disk of its own (boot-args emmc_slice_lba,
	 * emmc_slice_blocks): an image goes there, and nothing of the phone's own is touched.
	 */
	{
		uint32_t lba32 = 0, blocks32 = 0;
		uint64_t lba, blocks;

		(void)PE_parse_boot_argn("emmc_slice_lba", &lba32, sizeof(lba32));
		(void)PE_parse_boot_argn("emmc_slice_blocks", &blocks32, sizeof(blocks32));
		lba = lba32;
		blocks = blocks32;
		{
			char part[40] = { 0 };
			uint64_t pfirst = 0, pcount = 0;

			(void)PE_parse_boot_argn("emmc_slice_part", part, sizeof(part));
			if (part[0] != 0) {
				if (sdhci_gpt_find(part, &pfirst, &pcount)) {
					lba = pfirst;
					blocks = pcount;
					printf("[SDHCI] emmc_slice_part=%s: lba %llu, %llu sectors\n", part, pfirst, pcount);
				} else {
					printf("[SDHCI] emmc_slice_part=%s: no such partition\n", part);
				}
			}
		}
		if (blocks != 0 && lba != 0 && lba + blocks <= emmc_total_sectors) {
			slice_base = lba;
			slice_blocks = blocks;
			if (devfs_make_node(makedev(sdhci_bmajor, 1), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0660, "disk1") != NULL &&
			    devfs_make_node(makedev(sdhci_cmajor, 1), DEVFS_CHAR,  UID_ROOT, GID_OPERATOR, 0660, "rdisk1") != NULL) {
				static uint8_t first[EMMC_SECTOR_SIZE];

				slice_ready = true;
				printf("[SDHCI] emmc slice: /dev/disk1 & /dev/rdisk1 = lba %llu..%llu (%llu MB)\n",
				    lba, lba + blocks - 1, blocks * EMMC_SECTOR_SIZE / (1024 * 1024));
				/* What is there now, so that the image that was put there can be seen to be there. */
				if (sdhci_read_sectors_pio((uint32_t)lba, 1, first) == 0) {
					uint32_t nx_block = *(const uint32_t *)(const void *)(first + 36);

					printf("[SDHCI] emmc slice first sector: %02x%02x%02x%02x%02x%02x%02x%02x "
					    "\"%c%c%c%c\" block=%u\n",
					    first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7],
					    first[32] >= ' ' && first[32] < 127 ? first[32] : '.',
					    first[33] >= ' ' && first[33] < 127 ? first[33] : '.',
					    first[34] >= ' ' && first[34] < 127 ? first[34] : '.',
					    first[35] >= ' ' && first[35] < 127 ? first[35] : '.',
					    nx_block);
				}
			}
		} else if (lba != 0 || blocks != 0) {
			printf("[SDHCI] emmc_slice_lba/blocks %llu/%llu out of %llu sectors: no slice\n",
			    lba, blocks, emmc_total_sectors);
		}
	}

	sdhci_ready = true;
	{
		uint32_t dump = 0;

		(void)PE_parse_boot_argn("emmc_gpt", &dump, sizeof(dump));
		if (dump) {
			sdhci_print_gpt();
		}
	}
	printf("[SDHCI] MSM8952 eMMC 5.1 online: /dev/disk0 & /dev/rdisk0 (%llu MB, sector=%u)\n",
	       (emmc_total_sectors * EMMC_SECTOR_SIZE) / (1024 * 1024), EMMC_SECTOR_SIZE);
}
