#include "shamrock_charger.h"

/* =========================================================================
 * 1. MSM8952 Hardware Base Addresses (GCC, TLMM, BLSP1 QUP2 I2C, SPMI)
 * ========================================================================= */
#define MSM8952_GCC_BASE            0x01800000UL
#define GCC_BLSP1_AHB_CBCR          (MSM8952_GCC_BASE + 0x01008UL)
#define GCC_BLSP1_QUP2_I2C_CBCR     (MSM8952_GCC_BASE + 0x03010UL)

#define MSM8952_TLMM_BASE           0x01000000UL
#define TLMM_GPIO_CFG(n)            (MSM8952_TLMM_BASE + 0x1000UL * (n))

#define MSM8952_BLSP1_I2C2_BASE     0x078b6000UL

/* QUP Register Offsets */
#define QUP_CONFIG                  0x000
#define QUP_STATE                   0x004
#define QUP_IO_MODE                 0x008
#define QUP_SW_RESET                0x00C
#define QUP_OPERATIONAL             0x018
#define QUP_ERROR_FLAGS             0x01C
#define QUP_ERROR_FLAGS_EN          0x020
#define QUP_MX_WR_CNT               0x100
#define QUP_OUT_FIFO_BASE           0x110
#define QUP_MX_INPUT_CNT            0x200
#define QUP_MX_READ_CNT             0x208
#define QUP_IN_FIFO_BASE            0x218
#define QUP_I2C_CLK_CTL             0x400
#define QUP_I2C_STATUS              0x404
#define QUP_I2C_MASTER_GEN          0x408

/* QUP State Values */
#define QUP_RESET_STATE             0U
#define QUP_RUN_STATE               1U
#define QUP_PAUSE_STATE             3U
#define QUP_STATE_MASK              3U
#define QUP_STATE_VALID             (1U << 2)

/* QUP V2 I2C Tags */
#define QUP_TAG_START               0x81U
#define QUP_TAG_DATA_WR             0x82U
#define QUP_TAG_DATA_WR_STOP        0x83U
#define QUP_TAG_DATA_RD             0x85U
#define QUP_TAG_DATA_RD_NACK_STOP   0x87U

/* =========================================================================
 * 2. SMB1351 Charger Definitions (7-bit I2C addr 0x1D on blsp1_i2c2)
 * ========================================================================= */
#define SMB1351_I2C_ADDR_PRIMARY    0x1DU
#define SMB1351_I2C_ADDR_ALT        0x57U

#define SMB1351_CHG_CURRENT_CTRL    0x00U
#define SMB1351_CHG_OTH_CURRENT     0x01U
#define SMB1351_VARIOUS_FUNC        0x02U
#define SMB1351_VFLOAT_REG          0x03U
#define SMB1351_CHG_PIN_EN_CTRL     0x06U
#define SMB1351_WDOG_SAFETY_TIMER   0x08U
#define SMB1351_VARIOUS_FUNC_2      0x0EU
#define SMB1351_CMD_I2C_REG         0x30U
#define SMB1351_CMD_INPUT_LIMIT     0x31U
#define SMB1351_CMD_CHG_REG         0x32U
#define SMB1351_STATUS_0_REG        0x36U
#define SMB1351_STATUS_4_REG        0x3AU
#define SMB1351_IRQ_A_REG           0x3BU
#define SMB1351_IRQ_G_REG           0x41U

static uint8_t g_smb_addr = SMB1351_I2C_ADDR_PRIMARY;
static int     g_smb_present = 0;

static inline uint32_t mmio_rd32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void mmio_wr32(uintptr_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint64_t timer_get_ticks(void)
{
	uint64_t val;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(val));
	return val;
}

static void delay_us(uint32_t us)
{
	uint64_t start = timer_get_ticks();
	uint64_t ticks = (uint64_t)us * 19ULL + ((uint64_t)us / 5ULL); /* 19.2 MHz */
	while ((timer_get_ticks() - start) < ticks) {
		__asm__ volatile("nop");
	}
}

/* =========================================================================
 * 3. Bare-Metal BLSP1 QUP2 I2C Master Driver (0x078b6000)
 * ========================================================================= */
static int qup_set_state(uint32_t state)
{
	uintptr_t base = MSM8952_BLSP1_I2C2_BASE;
	uint64_t start = timer_get_ticks();

	while (!(mmio_rd32(base + QUP_STATE) & QUP_STATE_VALID)) {
		if (timer_get_ticks() - start > 192000ULL) return -1;
	}
	mmio_wr32(base + QUP_STATE, state);
	start = timer_get_ticks();
	while ((mmio_rd32(base + QUP_STATE) & (QUP_STATE_VALID | QUP_STATE_MASK)) != (QUP_STATE_VALID | state)) {
		if (timer_get_ticks() - start > 192000ULL) return -1;
	}
	return 0;
}

static void qup_i2c2_init(void)
{
	uintptr_t base = MSM8952_BLSP1_I2C2_BASE;

	/* Enable BLSP1 AHB & QUP2 I2C APPS branch clocks if gated */
	mmio_wr32(GCC_BLSP1_AHB_CBCR, mmio_rd32(GCC_BLSP1_AHB_CBCR) | 1U);
	mmio_wr32(GCC_BLSP1_QUP2_I2C_CBCR, mmio_rd32(GCC_BLSP1_QUP2_I2C_CBCR) | 1U);
	delay_us(50);

	/* Ensure TLMM GPIO 6 (SDA) & GPIO 7 (SCL) are set to FUNC=3 (blsp_i2c2), Pull-Up (3), 2mA */
	mmio_wr32(TLMM_GPIO_CFG(6), (3U << 2) | 3U);
	mmio_wr32(TLMM_GPIO_CFG(7), (3U << 2) | 3U);
	delay_us(20);

	/* Soft reset QUP2 mini-core */
	mmio_wr32(base + QUP_SW_RESET, 1U);
	uint64_t start = timer_get_ticks();
	while (mmio_rd32(base + QUP_SW_RESET)) {
		if (timer_get_ticks() - start > 192000ULL) break;
	}

	qup_set_state(QUP_RESET_STATE);

	/* Configure QUP I2C mini-core (mode 2) with 8-bit V2 tag width (N = 7) */
	mmio_wr32(base + QUP_CONFIG, (2U << 8) | 0x07U);
	/* Enable FIFO Pack and Unpack */
	mmio_wr32(base + QUP_IO_MODE, (1U << 15) | (1U << 14));
	/* Clear and enable error flags */
	mmio_wr32(base + QUP_ERROR_FLAGS, 0x7CU);
	mmio_wr32(base + QUP_ERROR_FLAGS_EN, 0x7CU);
	mmio_wr32(base + QUP_OPERATIONAL, 0xFF0U);
	/* Configure 100 kHz I2C clock from 19.2 MHz XO (FS_DIV = 0x5D, HS_DIV = 3) */
	mmio_wr32(base + QUP_I2C_CLK_CTL, 0x035DU);
}

static int qup_i2c2_write_reg(uint8_t slave_addr, uint8_t reg, uint8_t val)
{
	uintptr_t base = MSM8952_BLSP1_I2C2_BASE;

	if (qup_set_state(QUP_RESET_STATE) != 0) return -1;

	mmio_wr32(base + QUP_ERROR_FLAGS, 0x7CU);
	mmio_wr32(base + QUP_I2C_STATUS, 0xFFFFFFFFU);
	mmio_wr32(base + QUP_OPERATIONAL, 0xFF0U);

	mmio_wr32(base + QUP_MX_WR_CNT, 6U);
	mmio_wr32(base + QUP_MX_READ_CNT, 0U);
	mmio_wr32(base + QUP_MX_INPUT_CNT, 0U);

	if (qup_set_state(QUP_PAUSE_STATE) != 0) return -1;

	uint32_t word0 = (uint32_t)QUP_TAG_START |
	                 ((uint32_t)(slave_addr << 1) << 8) |
	                 ((uint32_t)QUP_TAG_DATA_WR_STOP << 16) |
	                 (2U << 24);
	uint32_t word1 = (uint32_t)reg | ((uint32_t)val << 8);

	mmio_wr32(base + QUP_OUT_FIFO_BASE, word0);
	mmio_wr32(base + QUP_OUT_FIFO_BASE, word1);

	if (qup_set_state(QUP_RUN_STATE) != 0) return -1;

	uint64_t start = timer_get_ticks();
	while ((mmio_rd32(base + QUP_OPERATIONAL) & (1U << 4)) ||
	       (mmio_rd32(base + QUP_I2C_STATUS) & (1U << 8))) {
		if (mmio_rd32(base + QUP_I2C_STATUS) & 0xFCU) {
			qup_set_state(QUP_RESET_STATE);
			return -2; /* NACK or bus error */
		}
		if (timer_get_ticks() - start > 192000ULL) {
			qup_set_state(QUP_RESET_STATE);
			return -3; /* Timeout */
		}
	}
	delay_us(50);
	int err = (mmio_rd32(base + QUP_I2C_STATUS) & 0xFCU) ? -2 : 0;
	qup_set_state(QUP_RESET_STATE);
	return err;
}

static int qup_i2c2_read_reg(uint8_t slave_addr, uint8_t reg, uint8_t *out_val)
{
	uintptr_t base = MSM8952_BLSP1_I2C2_BASE;

	if (qup_set_state(QUP_RESET_STATE) != 0) return -1;

	mmio_wr32(base + QUP_ERROR_FLAGS, 0x7CU);
	mmio_wr32(base + QUP_I2C_STATUS, 0xFFFFFFFFU);
	mmio_wr32(base + QUP_OPERATIONAL, 0xFF0U);

	mmio_wr32(base + QUP_MX_WR_CNT, 9U);
	mmio_wr32(base + QUP_MX_READ_CNT, 2U);
	mmio_wr32(base + QUP_MX_INPUT_CNT, 2U);

	if (qup_set_state(QUP_PAUSE_STATE) != 0) return -1;

	uint32_t word0 = (uint32_t)QUP_TAG_START |
	                 ((uint32_t)(slave_addr << 1) << 8) |
	                 ((uint32_t)QUP_TAG_DATA_WR << 16) |
	                 (1U << 24);
	uint32_t word1 = (uint32_t)reg |
	                 ((uint32_t)QUP_TAG_START << 8) |
	                 ((uint32_t)((slave_addr << 1) | 1U) << 16) |
	                 ((uint32_t)QUP_TAG_DATA_RD_NACK_STOP << 24);
	uint32_t word2 = 1U;

	mmio_wr32(base + QUP_OUT_FIFO_BASE, word0);
	mmio_wr32(base + QUP_OUT_FIFO_BASE, word1);
	mmio_wr32(base + QUP_OUT_FIFO_BASE, word2);

	if (qup_set_state(QUP_RUN_STATE) != 0) return -1;

	uint64_t start = timer_get_ticks();
	while (!(mmio_rd32(base + QUP_OPERATIONAL) & ((1U << 5) | (1U << 8)))) {
		if (mmio_rd32(base + QUP_I2C_STATUS) & 0xFCU) {
			qup_set_state(QUP_RESET_STATE);
			return -2;
		}
		if (timer_get_ticks() - start > 192000ULL) {
			qup_set_state(QUP_RESET_STATE);
			return -3;
		}
	}
	delay_us(50);
	uint32_t in_word = mmio_rd32(base + QUP_IN_FIFO_BASE);
	*out_val = (uint8_t)((in_word >> 8) & 0xFFU);
	qup_set_state(QUP_RESET_STATE);
	return 0;
}

static int smb1351_masked_write(uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t cur = 0;
	int rc = qup_i2c2_read_reg(g_smb_addr, reg, &cur);
	if (rc != 0) return rc;
	cur = (uint8_t)((cur & ~mask) | (val & mask));
	return qup_i2c2_write_reg(g_smb_addr, reg, cur);
}

/* =========================================================================
 * 4. SPMI PMIC Arbiter V2 Helper for PMI8950 (qpnp-smbcharger, USID 2)
 * ========================================================================= */
#define SPMI_CORE_BASE              0x0200f000UL
#define SPMI_CHNLn_CMD(c)           (0x02400000UL + 0x8000UL * (c) + 0x00UL)
#define SPMI_CHNLn_STATUS(c)        (0x02400000UL + 0x8000UL * (c) + 0x08UL)
#define SPMI_CHNLn_WDATA0(c)        (0x02400000UL + 0x8000UL * (c) + 0x10UL)
#define SPMI_APID_MAP_REG(n)        (SPMI_CORE_BASE + 0x0800UL + 4UL * (n))

static int spmi_find_apid(uint8_t usid, uint16_t pid)
{
	uint32_t ppid = ((uint32_t)usid << 8) | ((uint32_t)(pid >> 8) & 0xFFU);
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t entry = mmio_rd32(SPMI_APID_MAP_REG(i));
		if (entry && ((entry >> 8) & 0xFFFU) == ppid) {
			return (int)i;
		}
	}
	return -1;
}

static int spmi_ext_write_byte(uint8_t usid, uint16_t addr, uint8_t val)
{
	int apid = spmi_find_apid(usid, addr & 0xFF00U);
	if (apid < 0) return -1;

	mmio_wr32(SPMI_CHNLn_WDATA0(apid), (uint32_t)val);
	/* Opcode 0 = EXT_WRITEL, byte_cnt = 0 (1 byte), offset = addr & 0xFF */
	uint32_t cmd = (0U << 27) | ((uint32_t)(addr & 0xFFU) << 4) | 0U;
	mmio_wr32(SPMI_CHNLn_CMD(apid), cmd);

	uint64_t start = timer_get_ticks();
	while ((mmio_rd32(SPMI_CHNLn_STATUS(apid)) & 1U) == 0) {
		if (timer_get_ticks() - start > 192000ULL) return -2;
	}
	return 0;
}

static int spmi_ext_read_byte(uint8_t usid, uint16_t addr, uint8_t *out_val)
{
	int apid = spmi_find_apid(usid, addr & 0xFF00U);
	if (apid < 0) return -1;

	/* In SPMI V2, read commands MUST go to observer channel (0x02C00000) */
	uint32_t cmd = (1U << 27) | ((uint32_t)(addr & 0xFFU) << 4) | 0U;
	mmio_wr32(0x02C00000UL + 0x8000UL * (uint32_t)apid + 0x00UL, cmd);

	uint64_t start = timer_get_ticks();
	while ((mmio_rd32(0x02C00000UL + 0x8000UL * (uint32_t)apid + 0x08UL) & 1U) == 0) {
		if (timer_get_ticks() - start > 192000ULL) return -2;
	}
	*out_val = (uint8_t)(mmio_rd32(0x02C00000UL + 0x8000UL * (uint32_t)apid + 0x18UL) & 0xFFU);
	return 0;
}

static void pmi8950_enable_usb_charging(void)
{
	/* Unlock and configure USB_CHGPTH (0x1300): 1500 mA High-Current mode */
	if (spmi_find_apid(2, 0x1300U) >= 0) {
		spmi_ext_write_byte(2, 0x13D0U, 0xA5U); /* SEC_ACCESS */
		spmi_ext_write_byte(2, 0x13F2U, 0x0FU); /* 1500 mA ICL */
		spmi_ext_write_byte(2, 0x1340U, 0x02U); /* USB500 */
		delay_us(1000);
		spmi_ext_write_byte(2, 0x1340U, 0x03U); /* High-Current (HC) mode, unsuspended */
	}
	/* Ensure BAT_IF (0x1200) charging is enabled (CMD_CHG 0x1242 bit 1 = 0) */
	if (spmi_find_apid(2, 0x1200U) >= 0) {
		spmi_ext_write_byte(2, 0x12D0U, 0xA5U); /* SEC_ACCESS */
		spmi_ext_write_byte(2, 0x1242U, 0x00U); /* Charging enabled (active low) */
	}
}

/* =========================================================================
 * 5. High-Level Shamrock Charger Bring-Up, Keepalive & Telemetry
 * ========================================================================= */
int shamrock_charger_is_present(void)
{
	return g_smb_present;
}

void shamrock_charger_init(void)
{
	/* 1. Initialize BLSP1 QUP2 I2C (0x078b6000) */
	qup_i2c2_init();

	/* 2. Probe SMB1351 at 0x1D (or fallback 0x57) */
	uint8_t dummy = 0;
	if (qup_i2c2_read_reg(SMB1351_I2C_ADDR_PRIMARY, SMB1351_STATUS_4_REG, &dummy) == 0) {
		g_smb_addr = SMB1351_I2C_ADDR_PRIMARY;
		g_smb_present = 1;
	} else if (qup_i2c2_read_reg(SMB1351_I2C_ADDR_ALT, SMB1351_STATUS_4_REG, &dummy) == 0) {
		g_smb_addr = SMB1351_I2C_ADDR_ALT;
		g_smb_present = 1;
	}

	if (g_smb_present) {
		/* Step A: Unlock volatile configuration registers (0x30 |= BIT(6) CMD_BQ_CFG_ACCESS_BIT) */
		smb1351_masked_write(SMB1351_CMD_I2C_REG, 0x41U, 0x40U);

		/* Step B: Disable watchdog timer in bare-metal mode so it doesn't trip (0x08 &= ~BIT(6)) */
		smb1351_masked_write(SMB1351_WDOG_SAFETY_TIMER, 0x40U, 0x00U);

		/* Step C: Enable I2C control of charging enable & USBCS (0x06: EN_BY_I2C_0_ENABLE=0x20, USBCS=0) */
		smb1351_masked_write(SMB1351_CHG_PIN_EN_CTRL, 0x70U, 0x20U);

		/* Step D: Enable I2C USB 2.0/3.0 selection (0x01 &= ~BIT(1)) */
		smb1351_masked_write(SMB1351_CHG_OTH_CURRENT, 0x02U, 0x00U);

		/* Step E: Enable I2C Suspend Control (BIT(7)) & AICL (BIT(4)) in 0x02 */
		smb1351_masked_write(SMB1351_VARIOUS_FUNC, 0x90U, 0x90U);

		/* Step F: Allow Fast Charge current (clear PRECHG_TO_FASTCHG_BIT BIT(1) in 0x0E) */
		smb1351_masked_write(SMB1351_VARIOUS_FUNC_2, 0x02U, 0x00U);

		/* Step G: Program Fast Charge Current = 1800 mA (0x4 << 4) and
		 * AC/HC Input Current Limit = 1500 mA (0x6) in 0x00 -> 0x46.
		 */
		qup_i2c2_write_reg(g_smb_addr, SMB1351_CHG_CURRENT_CTRL, 0x46U);

		/* Step H: Mandatory AICL Re-Run Handshake on CMD_INPUT_LIMIT_REG (0x31):
		 * First switch to USB500 mode (0x0A: CMD_INPUT_CURRENT_MODE_CMD | CMD_USB_500_MODE, SUSPEND=0),
		 * wait 2 ms, then switch to High-Current mode (0x09: CMD_INPUT_CURRENT_MODE_CMD | CMD_USB_HC_MODE).
		 */
		qup_i2c2_write_reg(g_smb_addr, SMB1351_CMD_INPUT_LIMIT, 0x0AU);
		delay_us(2000);
		qup_i2c2_write_reg(g_smb_addr, SMB1351_CMD_INPUT_LIMIT, 0x09U);

		/* Step I: Enable Battery Charging (BIT(1) = 1) and ensure OTG Boost is off (BIT(0) = 0) in 0x32 */
		smb1351_masked_write(SMB1351_CMD_CHG_REG, 0x03U, 0x02U);
	}

	/* 3. Also configure PMI8950 (qpnp-smbcharger on SPMI USID 2) */
	pmi8950_enable_usb_charging();

	/* 4. Disable PM8950 (USID 0) and PMI8950 (USID 2) PON Hardware Watchdog (0x0800) */
	spmi_ext_write_byte(0, 0x08D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(0, 0x0856U, 0x00U); /* WD_RST_S2_CTL: S2_RESET_EN = 0 */
	spmi_ext_write_byte(0, 0x08D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(0, 0x0857U, 0x00U); /* WD_RST_S2_CTL2 = 0 */
	spmi_ext_write_byte(0, 0x08D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(0, 0x085AU, 0x7FU); /* WD_RST_S2_TIMER = max */
	spmi_ext_write_byte(0, 0x08D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(0, 0x085BU, 0x00U); /* PS_HOLD_RESET_CTL = 0 */
	spmi_ext_write_byte(0, 0x08D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(0, 0x0858U, 0x01U); /* Pet PON WDT */

	spmi_ext_write_byte(2, 0x08D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(2, 0x0856U, 0x00U); /* WD_RST_S2_CTL: S2_RESET_EN = 0 */
	spmi_ext_write_byte(2, 0x08D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(2, 0x0857U, 0x00U); /* WD_RST_S2_CTL2 = 0 */
	spmi_ext_write_byte(2, 0x08D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(2, 0x085AU, 0x7FU); /* WD_RST_S2_TIMER = max */
	spmi_ext_write_byte(2, 0x08D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(2, 0x085BU, 0x00U); /* PS_HOLD_RESET_CTL = 0 */
	spmi_ext_write_byte(2, 0x08D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(2, 0x0858U, 0x01U); /* Pet PON WDT */

	/* Disable PMI8950 (USID 2) SMBCHG MISC Watchdog (0x1600) */
	spmi_ext_write_byte(2, 0x16D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(2, 0x1643U, 0x00U); /* WDOG_EN = 0 */
	spmi_ext_write_byte(2, 0x16D0U, 0xA5U); /* SEC_ACCESS unlock */
	spmi_ext_write_byte(2, 0x1640U, 0x01U); /* Pet MISC WDT */

	/* 5. Disable MSM8952 APCS / KPSS Hardware Watchdog (0x0b017000) */
	mmio_wr32(0x0b017008UL, 0x00U); /* WDT_EN = 0 */
	mmio_wr32(0x0b017040UL, 0x00U); /* APCS TMR WDT_EN = 0 */
	mmio_wr32(0x0b017004UL, 0x01U); /* WDT_RST pet */
	mmio_wr32(0x0b017038UL, 0x01U); /* APCS TMR WDT_RST pet */
}

void shamrock_charger_keepalive(void)
{
	/* Pet hardware watchdogs continuously during shim operation */
	spmi_ext_write_byte(0, 0x08D0U, 0xA5U);
	spmi_ext_write_byte(0, 0x0858U, 0x01U);
	spmi_ext_write_byte(2, 0x08D0U, 0xA5U);
	spmi_ext_write_byte(2, 0x0858U, 0x01U);
	spmi_ext_write_byte(2, 0x16D0U, 0xA5U);
	spmi_ext_write_byte(2, 0x1640U, 0x01U);
	mmio_wr32(0x0b017004UL, 0x01U);
	mmio_wr32(0x0b017038UL, 0x01U);

	if (!g_smb_present) return;

	uint8_t cmd_il = 0, status4 = 0;
	if (qup_i2c2_read_reg(g_smb_addr, SMB1351_CMD_INPUT_LIMIT, &cmd_il) == 0) {
		if ((cmd_il & 0x43U) != 0x01U) {
			smb1351_masked_write(SMB1351_CMD_I2C_REG, 0x40U, 0x40U);
			qup_i2c2_write_reg(g_smb_addr, SMB1351_CMD_INPUT_LIMIT, 0x0AU);
			delay_us(1000);
			qup_i2c2_write_reg(g_smb_addr, SMB1351_CMD_INPUT_LIMIT, 0x09U);
		}
	}
	if (qup_i2c2_read_reg(g_smb_addr, SMB1351_STATUS_4_REG, &status4) == 0) {
		if ((status4 & 0x01U) == 0) {
			smb1351_masked_write(SMB1351_CMD_CHG_REG, 0x03U, 0x02U);
		}
	}
}

static const uint16_t k_smb1351_icl_ma[16] = {
	500, 685, 1000, 1100, 1200, 1300, 1500, 1600,
	1700, 1800, 2000, 2000, 2200, 2500, 3000, 3000
};

static char *append_s(char *p, const char *end, const char *s)
{
	while (*s && p < end - 1)
		*p++ = *s++;
	return p;
}

static char *append_hex8(char *p, const char *end, uint8_t val)
{
	static const char hex[] = "0123456789abcdef";
	if (p + 4 < end) {
		*p++ = '0';
		*p++ = 'x';
		*p++ = hex[(val >> 4) & 0xF];
		*p++ = hex[val & 0xF];
	}
	return p;
}

static char *append_u32(char *p, const char *end, uint32_t val)
{
	char tmp[12];
	int idx = 0;
	if (val == 0) {
		if (p < end - 1) *p++ = '0';
		return p;
	}
	while (val > 0 && idx < (int)sizeof(tmp)) {
		tmp[idx++] = '0' + (val % 10);
		val /= 10;
	}
	while (idx > 0 && p < end - 1) {
		*p++ = tmp[--idx];
	}
	return p;
}

int shamrock_charger_status_str(char *buf, size_t maxlen)
{
	char *p = buf;
	char *end = buf + maxlen;

	if (!g_smb_present) {
		uint8_t usb_st = 0, cmd_il = 0, chg_st = 0, cmd_chg = 0;
		spmi_ext_read_byte(2, 0x1308U, &usb_st);
		spmi_ext_read_byte(2, 0x1340U, &cmd_il);
		spmi_ext_read_byte(2, 0x1008U, &chg_st);
		spmi_ext_read_byte(2, 0x1042U, &cmd_chg);

		p = append_s(p, end, "PMI8950@SPMI-USID2: FAST_CHG_ACTIVE (USBIN_ST=");
		p = append_hex8(p, end, usb_st);
		p = append_s(p, end, " CMD_IL=");
		p = append_hex8(p, end, cmd_il);
		p = append_s(p, end, " CHG_ST=");
		p = append_hex8(p, end, chg_st);
		p = append_s(p, end, " CMD_CHG=");
		p = append_hex8(p, end, cmd_chg);
		p = append_s(p, end, ")\r\n");
		*p = 0;
		return (int)(p - buf);
	}

	uint8_t st0 = 0, st4 = 0, cmd_il = 0, cfg0 = 0;
	qup_i2c2_read_reg(g_smb_addr, SMB1351_STATUS_0_REG, &st0);
	qup_i2c2_read_reg(g_smb_addr, SMB1351_STATUS_4_REG, &st4);
	qup_i2c2_read_reg(g_smb_addr, SMB1351_CMD_INPUT_LIMIT, &cmd_il);
	qup_i2c2_read_reg(g_smb_addr, SMB1351_CHG_CURRENT_CTRL, &cfg0);

	uint16_t aicl_ma = k_smb1351_icl_ma[st0 & 0x0FU];
	uint8_t chg_type = (st4 >> 1) & 0x03U;

	p = append_s(p, end, "SMB1351@");
	p = append_hex8(p, end, g_smb_addr);
	p = append_s(p, end, ": state=");
	if (chg_type == 2)
		p = append_s(p, end, "FAST_CHG");
	else if (chg_type == 1)
		p = append_s(p, end, "PRE_CHG");
	else if (chg_type == 3)
		p = append_s(p, end, "TAPER_CHG");
	else
		p = append_s(p, end, "NOT_CHG");

	p = append_s(p, end, " chg_en=");
	p = append_u32(p, end, (st4 & 1U));
	p = append_s(p, end, " aicl_done=");
	p = append_u32(p, end, (st0 >> 7) & 1U);
	p = append_s(p, end, " aicl_ma=");
	p = append_u32(p, end, aicl_ma);
	p = append_s(p, end, " mA (ST0=");
	p = append_hex8(p, end, st0);
	p = append_s(p, end, " ST4=");
	p = append_hex8(p, end, st4);
	p = append_s(p, end, " CMD_IL=");
	p = append_hex8(p, end, cmd_il);
	p = append_s(p, end, " CFG0=");
	p = append_hex8(p, end, cfg0);
	p = append_s(p, end, ")\r\n");
	*p = 0;

	return (int)(p - buf);
}

void shamrock_set_fastboot_reboot_reason(void)
{
	/* 1. Set FASTBOOT_MODE in Qualcomm IMEM RESTART_REASON_ADDR (0x0860065C) */
	*(volatile uint32_t *)(0x0860065CUL) = 2; /* FASTBOOT_MODE */

	/* 2. PM8950 PON (USID 0, PID 0x0800):
	 *   0x088F = PON_SOFT_RB_SPARE (bits [7:2] = reason = 2 for FASTBOOT_MODE)
	 */
	uint8_t spare = 0;
	spmi_ext_read_byte(0, 0x088FU, &spare);
	spare = (uint8_t)((spare & 0x03U) | (2U << 2));
	spmi_ext_write_byte(0, 0x088FU, spare);

	/* 3. Dual PMIC Reset Configuration:
	 * Both PM8950 (slave 0) AND PMI8950 (slave 2) must be configured for
	 * PON_PSHOLD_HARD_RESET (0x07) with S2_RESET_EN enabled.
	 * Exactly matching lk2nd's pm8994_reset_configure().
	 */
	uint8_t slaves[] = {0, 2};
	for (int i = 0; i < 2; i++) {
		spmi_ext_write_byte(slaves[i], 0x085BU, 0x00U); /* Disable S2_RESET_EN */
	}
	delay_us(300);
	for (int i = 0; i < 2; i++) {
		spmi_ext_write_byte(slaves[i], 0x085AU, 0x07U); /* PON_PSHOLD_HARD_RESET = 0x07 */
		spmi_ext_write_byte(slaves[i], 0x085BU, 0x80U); /* Enable S2_RESET_EN */
	}
}
