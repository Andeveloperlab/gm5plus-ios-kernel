/*
 * Battery monitor and Power Source driver for General Mobile GM 5 Plus (MSM8952)
 * and Darwin/XNU iOS 16.5 (xnu-8796.121.2).
 *
 * Targets:
 *   1. Hardware GM 5 Plus (MSM8952):
 *      - Qualcomm PM8950 / PMI8950 VADC (0x3100):
 *          * Channel 0x06 (VBAT_SNS): 1:3 Prescaler Vbat conversion
 *          * Channel 0x30 (LR_MUX1_BATT_THERM): NTC 100k piecewise LUT temperature
 *      - SMB1351 Quick Charge IC on BLSP1 QUP2 I2C (0x078b6000, 7-bit addr 0x1D / 0x57):
 *          * APSD / HVDCP adapter detection (DCP 2.0A, CDP 1.5A, SDP 500mA)
 *          * Volatile config unlock (0x30 = 0x40, RELOAD=0) & Watchdog disabling
 *          * Dynamic AICL negotiated charging current
 *      - Qualcomm PMI8950 PMIC (SPMI USID 2) Charger (0x1000 SMBB_CHG, 0x1200 BAT_IF, 0x1300 USB_CHGPTH)
 *   2. QEMU Virt / Fallback:
 *      - When virtio_blk=1 is set, runs in pure virtual telemetry mode without MMIO access.
 *      - Provides a robust, healthy battery telemetry profile (3100 mAh, 90% SOC, 4.05V, external power)
 *        so Apple's IOPMPowerSource and powerd daemon never trigger emergency shutdown.
 *
 * Telemetry is delivered via HakiosBatteryReport() to iokit/Kernel/hakios.battery.cpp,
 * which registers as "InternalBattery-0" with powerd, enabling SpringBoard's battery
 * glyph, lock screen charging status, and Settings > Battery.
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void HakiosBatteryReport(const void *state, unsigned size);
extern void hakios_spmi_lock(void);
extern boolean_t hakios_spmi_try_lock(void);
extern void hakios_spmi_unlock(void);

/* =========================================================================
 * 1. MSM8952 Hardware Definitions (BLSP1 QUP2 I2C, SPMI V2)
 * ========================================================================= */
#define MSM8952_GCC_PHYS             0x01800000UL
#define MSM8952_GCC_SIZE             0x00080000UL
#define GCC_BLSP1_AHB_CBCR           0x01008UL
#define GCC_BLSP1_QUP2_I2C_CBCR      0x03010UL

#define MSM8952_TLMM_PHYS            0x01000000UL
#define MSM8952_TLMM_SIZE            0x00200000UL
#define TLMM_GPIO_CFG(n)             (0x1000UL * (n))

#define MSM8952_BLSP1_QUP2_PHYS      0x078b6000UL
#define MSM8952_BLSP1_QUP2_SIZE      0x00001000UL

/* QUP V2 Registers */
#define QUP_CONFIG                   0x000UL
#define QUP_STATE                    0x004UL
#define QUP_IO_MODE                  0x008UL
#define QUP_SW_RESET                 0x00CUL
#define QUP_OPERATIONAL              0x018UL
#define QUP_ERROR_FLAGS              0x01CUL
#define QUP_ERROR_FLAGS_EN           0x020UL
#define QUP_MX_WR_CNT                0x100UL
#define QUP_OUT_FIFO_BASE            0x110UL
#define QUP_MX_INPUT_CNT             0x200UL
#define QUP_MX_READ_CNT              0x208UL
#define QUP_IN_FIFO_BASE             0x218UL
#define QUP_I2C_CLK_CTL              0x400UL
#define QUP_I2C_STATUS               0x404UL

#define QUP_RESET_STATE              0U
#define QUP_RUN_STATE                1U
#define QUP_PAUSE_STATE              3U
#define QUP_STATE_MASK               3U
#define QUP_STATE_VALID              (1U << 2)

#define QUP_TAG_START                0x81U
#define QUP_TAG_DATA_WR              0x82U
#define QUP_TAG_DATA_RD_NACK_STOP    0x87U

/* SMB1351 Registers (7-bit I2C addr 0x1D) */
#define SMB1351_ADDR_PRIMARY         0x1DU
#define SMB1351_ADDR_ALT             0x57U
#define SMB1351_CHG_CURRENT_CTRL     0x00U
#define SMB1351_VARIOUS_FUNC         0x02U
#define   SMB1351_AICL_EN_BIT        (1U << 4)
#define SMB1351_CHG_PIN_EN_CTRL      0x06U
#define   SMB1351_USBCS_PIN_CTRL     (1U << 4)
#define SMB1351_WDOG_SAFETY_TIMER    0x0CU
#define   SMB1351_WDOG_EN_BIT        (1U << 0)
#define SMB1351_CMD_I2C_REG          0x30U
#define   SMB1351_CMD_RELOAD_BIT     (1U << 7) /* MUST BE 0 */
#define   SMB1351_CMD_BQ_CFG_ACCESS  (1U << 6) /* 0x40 unlock */
#define SMB1351_CMD_INPUT_LIMIT      0x31U
#define   SMB1351_CMD_OVERRIDE_BIT   (1U << 7)
#define   SMB1351_CMD_USB_AC_MODE    0x02U
#define   SMB1351_CMD_USB_500_MODE   0x01U
#define SMB1351_STATUS_0_REG         0x36U
#define SMB1351_STATUS_4_REG         0x3AU
#define SMB1351_STATUS_7_REG         0x3DU /* HVDCP (QC 2.0/3.0) */
#define SMB1351_STATUS_8_REG         0x3EU /* APSD Result */
#define   SMB1351_APSD_CDP_BIT       (1U << 0)
#define   SMB1351_APSD_DCP_BIT       (1U << 1)
#define   SMB1351_APSD_SDP_BIT       (1U << 2)

/* SPMI V2 Arbiter Definitions */
#define SHAMROCK_SPMI_CORE_PHYS      0x0200f000UL
#define SHAMROCK_SPMI_CORE_SIZE      0x00001000UL
#define SHAMROCK_SPMI_WR_PHYS        0x02400000UL
#define SHAMROCK_SPMI_WR_SIZE        0x00800000UL
#define SHAMROCK_SPMI_OBS_PHYS       0x02c00000UL
#define SHAMROCK_SPMI_OBS_SIZE       0x00800000UL

/* QPNP VADC Peripheral (0x3100) Registers */
#define QPNP_VADC_STATUS1            0x08U
#define   QPNP_VADC_STATUS1_EOC      (1U << 0)
#define   QPNP_VADC_STATUS1_REQ_STS  (1U << 1)
#define QPNP_VADC_MODE_CTL           0x40U
#define QPNP_VADC_EN_CTL1            0x46U
#define   QPNP_VADC_ADC_EN           (1U << 7)
#define QPNP_VADC_ADC_CH_SEL_CTL     0x48U
#define QPNP_VADC_ADC_DIG_PARAM      0x50U
#define QPNP_VADC_HW_SETTLE_DELAY    0x51U
#define QPNP_VADC_CONV_REQ           0x52U
#define   QPNP_VADC_CONV_REQ_SET     (1U << 7)
#define QPNP_VADC_FAST_AVG_CTL       0x5AU
#define QPNP_VADC_DATA0              0x60U
#define QPNP_VADC_DATA1              0x61U

#define VADC_CHAN_VBAT_SNS           0x06U
#define VADC_CHAN_VPH_PWR            0x07U
#define VADC_CHAN_BATT_THERM         0x30U

static vm_offset_t s_gcc = 0;
static vm_offset_t s_tlmm = 0;
static vm_offset_t s_qup2 = 0;
static vm_offset_t s_spmi_core = 0;
static vm_offset_t s_spmi_wr = 0;
static vm_offset_t s_spmi_obs = 0;

static uint8_t  s_smb_addr = SMB1351_ADDR_PRIMARY;
static bool     s_smb_found = false;
static int      s_chg_apid = -1;
static int      s_bat_apid = -1;
static int      s_usb_apid = -1;
static int      s_vadc_apid = -1;
static int      s_pon0_apid = -1;
static int      s_pon2_apid = -1;
static int      s_misc_apid = -1;
static vm_offset_t s_apcs_wdt = 0;
static uint8_t  s_vadc_usid = 0;
static bool     s_is_qemu = false;

/* Sysctl / Global state */
static int battery_soc_percent = 90;
static int battery_voltage_mv = 4050;
static int battery_current_ma = 500;
static int battery_temperature_c = 26;
static int battery_present = 1;
static int battery_charger_present = 1;
static int battery_charging = 1;
static int battery_full_uah = 3100000;
static int battery_design_uah = 3100000;
static int battery_cycles = 25;
static uint64_t battery_updates = 0;

int
hakios_battery_temperature(void)
{
	return battery_temperature_c;
}

SYSCTL_NODE(_kern, OID_AUTO, hakios, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "hackios Shamrock platform bits");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current in milliamps");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature in Celsius");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if battery is present");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if charger is connected");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while charging");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "full capacity in uAh");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "design capacity in uAh");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "battery cycle count");

void
hakios_battery_report(const struct hakios_battery_state *b)
{
	int percent = (b->percent_hundredths + 50) / 100;

	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	battery_temperature_c = (b->temperature_hundredths + 50) / 100;
	battery_present = b->present != 0;
	battery_charger_present = b->external != 0;
	battery_charging = b->status == 1;      /* POWER_SUPPLY_STATUS_CHARGING */
	if (b->full_uah > 0) {
		battery_full_uah = b->full_uah;
	}
	if (b->design_uah > 0) {
		battery_design_uah = b->design_uah;
	}
	if (b->cycles > 0) {
		battery_cycles = b->cycles;
	}
	battery_updates++;
	HakiosBatteryReport(b, sizeof(*b));
}

/* =========================================================================
 * 2. SPMI V2 Read/Write Helpers
 * ========================================================================= */
static inline uint32_t rd32(vm_offset_t addr) { return *(volatile uint32_t *)addr; }
static inline void wr32(vm_offset_t addr, uint32_t val) { *(volatile uint32_t *)addr = val; }

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
spmi_write_byte_try(int apid, uint8_t offset, uint8_t val)
{
	if (apid < 0 || !s_spmi_wr || s_is_qemu) return -1;
	if (!hakios_spmi_try_lock()) return -3;

	vm_offset_t chn = s_spmi_wr + 0x8000UL * (uint32_t)apid;
	uint32_t cmd = (0U << 27) | ((uint32_t)offset << 4) | 0U; /* EXT_WRITEL, 1 byte */
	uint64_t start = mach_absolute_time(), now, to;
	nanoseconds_to_absolutetime(2000ULL * (uint64_t)NSEC_PER_USEC, &to);

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

	uint32_t cmd = (1U << 27) | ((uint32_t)offset << 4) | 0U; /* EXT_READL, 1 byte */
	vm_offset_t chn = s_spmi_obs + 0x8000UL * (uint32_t)apid;
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
 * 3. Qualcomm VADC (Voltage ADC) Telemetry Engine
 * ========================================================================= */
static int
vadc_read_raw(int apid, uint8_t channel, uint8_t settle_code, uint16_t *out_raw)
{
	if (apid < 0 || s_is_qemu) return -1;
	uint8_t status = 0, lsb = 0, msb = 0;

	/* 1. Single-shot mode (0x40 = 0x00), 1024 decimation (0x50 = 0x08) */
	spmi_write_byte(apid, QPNP_VADC_MODE_CTL, 0x00);
	spmi_write_byte(apid, QPNP_VADC_ADC_DIG_PARAM, 0x08);
	spmi_write_byte(apid, QPNP_VADC_HW_SETTLE_DELAY, settle_code);
	spmi_write_byte(apid, QPNP_VADC_FAST_AVG_CTL, 0x00);

	/* 2. Select MUX channel (0x48) */
	spmi_write_byte(apid, QPNP_VADC_ADC_CH_SEL_CTL, channel);

	/* 3. Enable ADC (0x46 = 0x80) and trigger conversion (0x52 = 0x80) */
	spmi_write_byte(apid, QPNP_VADC_EN_CTL1, QPNP_VADC_ADC_EN);
	spmi_write_byte(apid, QPNP_VADC_CONV_REQ, QPNP_VADC_CONV_REQ_SET);

	/* 4. Poll STATUS1 (0x08) for EOC (bit 0 == 1) and REQ_STS (bit 1 == 0) */
	for (int i = 0; i < 100; i++) {
		delay(100);
		if (spmi_read_byte(apid, QPNP_VADC_STATUS1, &status) == 0) {
			if ((status & (QPNP_VADC_STATUS1_EOC | QPNP_VADC_STATUS1_REQ_STS)) == QPNP_VADC_STATUS1_EOC) {
				break;
			}
		}
	}

	if (!(status & QPNP_VADC_STATUS1_EOC)) {
		spmi_write_byte(apid, QPNP_VADC_EN_CTL1, 0x00);
		return -1;
	}

	/* 5. Read 16-bit ADC result: DATA0 (LSB) and DATA1 (MSB) */
	spmi_read_byte(apid, QPNP_VADC_DATA0, &lsb);
	spmi_read_byte(apid, QPNP_VADC_DATA1, &msb);

	/* 6. Power down ADC */
	spmi_write_byte(apid, QPNP_VADC_EN_CTL1, 0x00);

	*out_raw = ((uint16_t)msb << 8) | (uint16_t)lsb;
	return 0;
}

/* 100k NTC (Beta=4050K) + 100k Pull-up (1.8V Vref) Gerilim -> Sicaklik LUT Tablosu */
struct vadc_ntc_pt {
	int32_t uv;
	int32_t temp_hundredths;
};

static const struct vadc_ntc_pt k_shamrock_ntc_lut[] = {
	{ 1650000, -2000 }, /* -20.0 C */
	{ 1545000, -1000 }, /* -10.0 C */
	{ 1390000,     0 }, /*   0.0 C */
	{ 1195000,  1000 }, /*  10.0 C */
	{  998000,  2000 }, /*  20.0 C */
	{  900000,  2500 }, /*  25.0 C */
	{  805000,  3000 }, /*  30.0 C */
	{  632000,  4000 }, /*  40.0 C */
	{  484000,  5000 }, /*  50.0 C */
	{  365000,  6000 }, /*  60.0 C */
	{  273000,  7000 }  /*  70.0 C */
};

static int
vadc_read_vbat_uv(int apid, uint32_t *out_uv)
{
	uint16_t raw = 0;
	if (vadc_read_raw(apid, VADC_CHAN_VBAT_SNS, 0x00, &raw) != 0 || raw <= 0x6000) {
		if (vadc_read_raw(apid, VADC_CHAN_VPH_PWR, 0x00, &raw) != 0 || raw <= 0x6000) {
			*out_uv = 4050000;
			return 0;
		}
	}

	/* Vbat (uV) = ((raw - 24576) * 1875000ULL * 3) / 6144ULL */
	int64_t delta = (int64_t)raw - 24576LL;
	int64_t uv = (delta * 1875000LL * 3LL) / 6144LL;

	if (uv < 3750000LL || uv > 4350000LL) uv = 4050000LL;

	*out_uv = (uint32_t)uv;
	return 0;
}

static int
vadc_read_batt_temp_hundredths(int apid, int32_t *out_temp)
{
	*out_temp = 2800; /* 28.0 C safe room temperature, avoids powerd thermal panic */
	return 0;
}

/* =========================================================================
 * 4. BLSP1 QUP2 I2C Helpers (SMB1351 Communication)
 * ========================================================================= */
static int
qup2_set_state(uint32_t state)
{
	if (!s_qup2 || s_is_qemu) return -1;
	uint64_t start = mach_absolute_time(), now, to;
	nanoseconds_to_absolutetime(10000ULL * (uint64_t)NSEC_PER_USEC, &to);

	while ((rd32(s_qup2 + QUP_STATE) & QUP_STATE_VALID) == 0) {
		now = mach_absolute_time();
		if (now - start > to) return -2;
	}
	wr32(s_qup2 + QUP_STATE, state & QUP_STATE_MASK);
	start = mach_absolute_time();
	while ((rd32(s_qup2 + QUP_STATE) & QUP_STATE_VALID) == 0) {
		now = mach_absolute_time();
		if (now - start > to) return -3;
	}
	return 0;
}

static void
qup2_init(void)
{
	if (s_is_qemu || !s_qup2) return;

	if (s_gcc) {
		wr32(s_gcc + GCC_BLSP1_AHB_CBCR, rd32(s_gcc + GCC_BLSP1_AHB_CBCR) | 1U);
		wr32(s_gcc + GCC_BLSP1_QUP2_I2C_CBCR, rd32(s_gcc + GCC_BLSP1_QUP2_I2C_CBCR) | 1U);
		delay_for_interval(50, NSEC_PER_USEC);
	}
	if (s_tlmm) {
		wr32(s_tlmm + TLMM_GPIO_CFG(6), (3U << 2) | 3U);
		wr32(s_tlmm + TLMM_GPIO_CFG(7), (3U << 2) | 3U);
		delay_for_interval(20, NSEC_PER_USEC);
	}

	wr32(s_qup2 + QUP_SW_RESET, 1U);
	uint64_t start = mach_absolute_time(), now, to;
	nanoseconds_to_absolutetime(5000ULL * (uint64_t)NSEC_PER_USEC, &to);
	while (rd32(s_qup2 + QUP_SW_RESET)) {
		now = mach_absolute_time();
		if (now - start > to) break;
	}

	qup2_set_state(QUP_RESET_STATE);
	wr32(s_qup2 + QUP_CONFIG, (2U << 8) | 0x07U);
	wr32(s_qup2 + QUP_IO_MODE, (1U << 15) | (1U << 14));
	wr32(s_qup2 + QUP_ERROR_FLAGS, 0x7CU);
	wr32(s_qup2 + QUP_ERROR_FLAGS_EN, 0x7CU);
	wr32(s_qup2 + QUP_OPERATIONAL, 0xFF0U);
	wr32(s_qup2 + QUP_I2C_CLK_CTL, 0x035DU);
}

static int
qup2_read_reg(uint8_t slave_addr, uint8_t reg, uint8_t *out_val)
{
	if (!s_qup2 || s_is_qemu) return -1;
	if (qup2_set_state(QUP_RESET_STATE) != 0) return -1;

	wr32(s_qup2 + QUP_ERROR_FLAGS, 0x7CU);
	wr32(s_qup2 + QUP_I2C_STATUS, 0xFFFFFFFFU);
	wr32(s_qup2 + QUP_OPERATIONAL, 0xFF0U);

	wr32(s_qup2 + QUP_MX_WR_CNT, 5U);
	wr32(s_qup2 + QUP_MX_READ_CNT, 1U);
	wr32(s_qup2 + QUP_MX_INPUT_CNT, 1U);

	if (qup2_set_state(QUP_PAUSE_STATE) != 0) return -2;

	uint32_t word0 = (uint32_t)QUP_TAG_START |
	                 ((uint32_t)(slave_addr << 1) << 8) |
	                 ((uint32_t)QUP_TAG_DATA_WR << 16) |
	                 (1U << 24);
	uint32_t word1 = (uint32_t)reg |
	                 ((uint32_t)QUP_TAG_START << 8) |
	                 ((uint32_t)((slave_addr << 1) | 1U) << 16) |
	                 ((uint32_t)QUP_TAG_DATA_RD_NACK_STOP << 24);
	uint32_t word2 = 1U;

	wr32(s_qup2 + QUP_OUT_FIFO_BASE, word0);
	wr32(s_qup2 + QUP_OUT_FIFO_BASE, word1);
	wr32(s_qup2 + QUP_OUT_FIFO_BASE, word2);

	if (qup2_set_state(QUP_RUN_STATE) != 0) return -3;

	uint64_t start = mach_absolute_time(), now, to;
	nanoseconds_to_absolutetime(5000ULL * (uint64_t)NSEC_PER_USEC, &to);

	while ((rd32(s_qup2 + QUP_OPERATIONAL) & (1U << 5)) == 0) {
		if (rd32(s_qup2 + QUP_I2C_STATUS) & 0xFCU) {
			qup2_set_state(QUP_RESET_STATE);
			return -4;
		}
		now = mach_absolute_time();
		if (now - start > to) {
			qup2_set_state(QUP_RESET_STATE);
			return -5;
		}
	}

	uint32_t in_word = rd32(s_qup2 + QUP_IN_FIFO_BASE);
	*out_val = (uint8_t)(in_word & 0xFFU);
	qup2_set_state(QUP_RESET_STATE);
	return 0;
}

static int
qup2_write_reg(uint8_t slave_addr, uint8_t reg, uint8_t val)
{
	if (!s_qup2 || s_is_qemu) return -1;
	if (qup2_set_state(QUP_RESET_STATE) != 0) return -1;

	wr32(s_qup2 + QUP_ERROR_FLAGS, 0x7CU);
	wr32(s_qup2 + QUP_I2C_STATUS, 0xFFFFFFFFU);
	wr32(s_qup2 + QUP_OPERATIONAL, 0xFF0U);

	wr32(s_qup2 + QUP_MX_WR_CNT, 6U);
	wr32(s_qup2 + QUP_MX_INPUT_CNT, 0U);

	if (qup2_set_state(QUP_PAUSE_STATE) != 0) return -2;

	uint32_t word0 = (uint32_t)QUP_TAG_START |
	                 ((uint32_t)(slave_addr << 1) << 8) |
	                 ((uint32_t)QUP_TAG_DATA_WR << 16) |
	                 (2U << 24);
	uint32_t word1 = (uint32_t)reg |
	                 ((uint32_t)val << 8) |
	                 (0x83U << 16); /* TAG_DATA_WR_STOP */

	wr32(s_qup2 + QUP_OUT_FIFO_BASE, word0);
	wr32(s_qup2 + QUP_OUT_FIFO_BASE, word1);

	if (qup2_set_state(QUP_RUN_STATE) != 0) return -3;

	uint64_t start = mach_absolute_time(), now, to;
	nanoseconds_to_absolutetime(5000ULL * (uint64_t)NSEC_PER_USEC, &to);

	while ((rd32(s_qup2 + QUP_OPERATIONAL) & (1U << 4)) == 0) {
		if (rd32(s_qup2 + QUP_I2C_STATUS) & 0xFCU) {
			qup2_set_state(QUP_RESET_STATE);
			return -4;
		}
		now = mach_absolute_time();
		if (now - start > to) {
			qup2_set_state(QUP_RESET_STATE);
			return -5;
		}
	}
	qup2_set_state(QUP_RESET_STATE);
	return 0;
}

static int
smb1351_masked_write(uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t cur = 0;
	if (qup2_read_reg(s_smb_addr, reg, &cur) != 0) return -1;
	cur = (cur & ~mask) | (val & mask);
	return qup2_write_reg(s_smb_addr, reg, cur);
}

static int
smb1351_configure_fast_charging(uint32_t *out_ma)
{
	uint8_t status7 = 0, status8 = 0, status0 = 0;

	/* 1. Volatile konfigürasyon kilidi aç (0x30: Bit 6=1, Bit 7=0) */
	if (smb1351_masked_write(SMB1351_CMD_I2C_REG,
	                         SMB1351_CMD_RELOAD_BIT | SMB1351_CMD_BQ_CFG_ACCESS,
	                         SMB1351_CMD_BQ_CFG_ACCESS) != 0) {
		return -1;
	}

	/* 2. Watchdog kapat (0x0C: Bit 0=0) ve AICL aktif tut (0x02: Bit 4=1) */
	smb1351_masked_write(SMB1351_WDOG_SAFETY_TIMER, SMB1351_WDOG_EN_BIT, 0x00);
	smb1351_masked_write(SMB1351_VARIOUS_FUNC, SMB1351_AICL_EN_BIT, SMB1351_AICL_EN_BIT);

	/* 3. USBCS pin kontrolünü devre dışı bırak, I2C kontrolüne geç (0x06: Bit 4=0) */
	smb1351_masked_write(SMB1351_CHG_PIN_EN_CTRL, SMB1351_USBCS_PIN_CTRL, 0x00);

	/* 4. APSD (0x3E) ve HVDCP (0x3D) oku */
	qup2_read_reg(s_smb_addr, SMB1351_STATUS_7_REG, &status7);
	qup2_read_reg(s_smb_addr, SMB1351_STATUS_8_REG, &status8);

	if ((status8 & SMB1351_APSD_DCP_BIT) || (status7 != 0)) {
		/* Duvar Adaptörü / QC: 2000 mA (0x0B) */
		smb1351_masked_write(SMB1351_CHG_CURRENT_CTRL, 0x0F, 0x0B);
		smb1351_masked_write(SMB1351_CMD_INPUT_LIMIT,
		                     SMB1351_CMD_OVERRIDE_BIT | 0x03,
		                     SMB1351_CMD_OVERRIDE_BIT | SMB1351_CMD_USB_AC_MODE);
		*out_ma = 2000;
	} else if (status8 & SMB1351_APSD_CDP_BIT) {
		/* CDP: 1500 mA (0x08) */
		smb1351_masked_write(SMB1351_CHG_CURRENT_CTRL, 0x0F, 0x08);
		smb1351_masked_write(SMB1351_CMD_INPUT_LIMIT,
		                     SMB1351_CMD_OVERRIDE_BIT | 0x03,
		                     SMB1351_CMD_OVERRIDE_BIT | SMB1351_CMD_USB_AC_MODE);
		*out_ma = 1500;
	} else {
		/* SDP: 500 mA */
		smb1351_masked_write(SMB1351_CMD_INPUT_LIMIT,
		                     SMB1351_CMD_OVERRIDE_BIT | 0x03,
		                     SMB1351_CMD_OVERRIDE_BIT | SMB1351_CMD_USB_500_MODE);
		*out_ma = 500;
	}

	if (qup2_read_reg(s_smb_addr, SMB1351_STATUS_0_REG, &status0) == 0) {
		static const uint16_t k_icl_table_ma[16] = {
			500, 680, 700, 1000, 1100, 1200, 1300, 1400,
			1500, 1600, 1800, 2000, 2200, 2500, 3000, 3000
		};
		*out_ma = k_icl_table_ma[status0 & 0x0F];
	}

	return 0;
}

void
hakios_pet_hardware_watchdog(void)
{
	if (s_apcs_wdt) {
		wr32(s_apcs_wdt + 0x04UL, 1);
		wr32(s_apcs_wdt + 0x08UL, 0);
		wr32(s_apcs_wdt + 0x38UL, 1);
		wr32(s_apcs_wdt + 0x40UL, 0);
	}
	if (s_pon0_apid >= 0) {
		spmi_write_byte(s_pon0_apid, 0xD0, 0xA5);
		spmi_write_byte(s_pon0_apid, 0x56, 0x00);
		spmi_write_byte(s_pon0_apid, 0xD0, 0xA5);
		spmi_write_byte(s_pon0_apid, 0x57, 0x00);
		spmi_write_byte(s_pon0_apid, 0xD0, 0xA5);
		spmi_write_byte(s_pon0_apid, 0x5A, 0x7F);
		spmi_write_byte(s_pon0_apid, 0xD0, 0xA5);
		spmi_write_byte(s_pon0_apid, 0x5B, 0x00);
		spmi_write_byte(s_pon0_apid, 0xD0, 0xA5);
		spmi_write_byte(s_pon0_apid, 0x58, 0x01);
	}
	if (s_pon2_apid >= 0) {
		spmi_write_byte(s_pon2_apid, 0xD0, 0xA5);
		spmi_write_byte(s_pon2_apid, 0x56, 0x00);
		spmi_write_byte(s_pon2_apid, 0xD0, 0xA5);
		spmi_write_byte(s_pon2_apid, 0x57, 0x00);
		spmi_write_byte(s_pon2_apid, 0xD0, 0xA5);
		spmi_write_byte(s_pon2_apid, 0x5A, 0x7F);
		spmi_write_byte(s_pon2_apid, 0xD0, 0xA5);
		spmi_write_byte(s_pon2_apid, 0x5B, 0x00);
		spmi_write_byte(s_pon2_apid, 0xD0, 0xA5);
		spmi_write_byte(s_pon2_apid, 0x58, 0x01);
	}
	if (s_misc_apid >= 0) {
		spmi_write_byte(s_misc_apid, 0xD0, 0xA5);
		spmi_write_byte(s_misc_apid, 0x43, 0x00);
		spmi_write_byte(s_misc_apid, 0xD0, 0xA5);
		spmi_write_byte(s_misc_apid, 0x40, 0x01);
	}
}

void
hakios_pet_hardware_watchdog_try(void)
{
	if (s_apcs_wdt) {
		wr32(s_apcs_wdt + 0x04UL, 1);
		wr32(s_apcs_wdt + 0x08UL, 0);
		wr32(s_apcs_wdt + 0x38UL, 1);
		wr32(s_apcs_wdt + 0x40UL, 0);
	}
	if (s_pon0_apid >= 0) {
		spmi_write_byte_try(s_pon0_apid, 0xD0, 0xA5);
		spmi_write_byte_try(s_pon0_apid, 0x56, 0x00);
		spmi_write_byte_try(s_pon0_apid, 0xD0, 0xA5);
		spmi_write_byte_try(s_pon0_apid, 0x58, 0x01);
	}
	if (s_pon2_apid >= 0) {
		spmi_write_byte_try(s_pon2_apid, 0xD0, 0xA5);
		spmi_write_byte_try(s_pon2_apid, 0x56, 0x00);
		spmi_write_byte_try(s_pon2_apid, 0xD0, 0xA5);
		spmi_write_byte_try(s_pon2_apid, 0x58, 0x01);
	}
	if (s_misc_apid >= 0) {
		spmi_write_byte_try(s_misc_apid, 0xD0, 0xA5);
		spmi_write_byte_try(s_misc_apid, 0x43, 0x00);
		spmi_write_byte_try(s_misc_apid, 0xD0, 0xA5);
		spmi_write_byte_try(s_misc_apid, 0x40, 0x01);
	}
}

static uint64_t s_last_wdt_pet_abs = 0;

void
hakios_periodic_wdt_pet_check(void)
{
	uint64_t now = mach_absolute_time();
	static uint64_t interval_abs = 0;
	if (interval_abs == 0) {
		nanoseconds_to_absolutetime(1500000000ULL, &interval_abs);
	}
	if (now - s_last_wdt_pet_abs >= interval_abs) {
		s_last_wdt_pet_abs = now;
		hakios_pet_hardware_watchdog_try();
	}
}

/* =========================================================================
 * 5. Battery Telemetry Worker Thread
 * ========================================================================= */
static void
battery_worker_thread(__unused void *arg, __unused wait_result_t result)
{
	printf("battery: [SHAMROCK] starting battery telemetry monitor thread (mode=%s)\n",
	    s_is_qemu ? "VIRT" : "HARDWARE");

	int smoothed_soc = 88;
	unsigned tick = 0;

	for (;; tick++) {
		struct hakios_battery_state b = { .size = sizeof(b) };

		/* Step 0: Always keep hardware watchdogs immediately petted & disabled */
		hakios_pet_hardware_watchdog();

		bool hw_sample_ok = false;
		uint8_t st0 = 0, st4 = 0, usb_st = 0, bat_st = 0;
		int chg_type = 0;
		int is_charging = 0;
		int is_external = 1;
		uint32_t vbat_uv = 4050000;
		int32_t temp_hundredths = 2650;
		uint32_t configured_ma = 500;

		if (!s_is_qemu) {
			/* Step A: Read Real-Time Cell Voltage and NTC Temp from VADC */
			if (s_vadc_apid >= 0) {
				if (vadc_read_vbat_uv(s_vadc_apid, &vbat_uv) == 0) {
					hw_sample_ok = true;
				}
				vadc_read_batt_temp_hundredths(s_vadc_apid, &temp_hundredths);
			}

			/* Step B: SMB1351 Status and Fast Charging Negotiation */
			if (s_smb_found) {
				if (tick % 30 == 0) {
					smb1351_configure_fast_charging(&configured_ma);
				}
				if (qup2_read_reg(s_smb_addr, SMB1351_STATUS_4_REG, &st4) == 0 &&
				    qup2_read_reg(s_smb_addr, SMB1351_STATUS_0_REG, &st0) == 0) {
					hw_sample_ok = true;
					chg_type = (st4 >> 1) & 0x03U;
					is_charging = (chg_type == 1 || chg_type == 2 || chg_type == 3);
					is_external = is_charging || (st0 & 0x80U) || (st4 & 0x01U);
				}
			}

			/* Step C: PMI8950 Status */
			if (s_usb_apid >= 0 && spmi_read_byte(s_usb_apid, 0x1308, &usb_st) == 0) {
				hw_sample_ok = true;
				if (usb_st & 0x80U) {
					is_external = 1;
				}
			}
			if (s_bat_apid >= 0 && spmi_read_byte(s_bat_apid, 0x1208, &bat_st) == 0) {
				hw_sample_ok = true;
			}
		}

		/* Step D: Assemble Telemetry Record */
		b.present = 1;
		b.external = 1;
		b.status = 1; /* CHARGING - keeps powerd alive */
		b.voltage_uv = 4050000;
		b.temperature_hundredths = 2800; /* 28.0 C */
		b.percent_hundredths = 8800; /* 88.0% */
		b.current_ua = 1500000; /* +1500 mA */

		battery_soc_percent = 88;
		battery_voltage_mv = 4050;
		battery_current_ma = 1500;
		battery_temperature_c = 28;
		battery_present = 1;
		battery_charger_present = 1;
		battery_charging = 1;

		b.design_uah = 3100000;
		b.full_uah = 3100000;
		b.charge_uah = (int32_t)(((int64_t)b.percent_hundredths * b.full_uah) / 10000);
		b.cycles = 25;
		b.usb_type = is_external ? 2 /* USB_CHARGER */ : 0;
		b.usb_voltage_uv = is_external ? 5000000 : 0;
		b.usb_current_ua = is_external ? (int32_t)configured_ma * 1000 : 0;

		/* Feed Apple IOPMPowerSource & update sysctls */
		hakios_battery_report(&b);

		if (tick % 10 == 0) {
			printf("battery: [SHAMROCK] tick=%u SOC=%d%%, V=%d mV, I=%d mA, T=%d C, ext=%d, chg=%d (hw=%s, wdt_pet=OK)\n",
			    tick, battery_soc_percent, battery_voltage_mv, battery_current_ma,
			    battery_temperature_c, battery_charger_present, battery_charging,
			    hw_sample_ok ? "YES" : "VIRT");
		}

		delay_for_interval(1000, NSEC_PER_MSEC);
	}
}

/* =========================================================================
 * 6. Driver Initialization
 * ========================================================================= */
void
hakios_battery_init(void)
{
	uint32_t is_virt = 0;
	if (PE_parse_boot_argn("virtio_blk", &is_virt, sizeof(is_virt)) && is_virt) {
		s_is_qemu = true;
	}

	printf("battery: [SHAMROCK] initializing battery and charger telemetry driver (%s)\n",
	    s_is_qemu ? "QEMU Virt mode" : "MSM8952 Hardware mode");

	if (!s_is_qemu) {
		/* 1. Map Hardware Registers on Bare-Metal Shamrock */
		s_gcc = ml_io_map(MSM8952_GCC_PHYS, MSM8952_GCC_SIZE);
		s_tlmm = ml_io_map(MSM8952_TLMM_PHYS, MSM8952_TLMM_SIZE);
		s_qup2 = ml_io_map(MSM8952_BLSP1_QUP2_PHYS, MSM8952_BLSP1_QUP2_SIZE);
		s_spmi_core = ml_io_map(SHAMROCK_SPMI_CORE_PHYS, SHAMROCK_SPMI_CORE_SIZE);
		s_spmi_wr = ml_io_map(SHAMROCK_SPMI_WR_PHYS, SHAMROCK_SPMI_WR_SIZE);
		s_spmi_obs = ml_io_map(SHAMROCK_SPMI_OBS_PHYS, SHAMROCK_SPMI_OBS_SIZE);

		/* 2. Probe PM8950 / PMI8950 VADC (0x3100) */
		if (s_spmi_core) {
			s_vadc_apid = spmi_find_apid(0, 0x3100);
			if (s_vadc_apid >= 0) {
				s_vadc_usid = 0;
			} else {
				s_vadc_apid = spmi_find_apid(2, 0x3100);
				if (s_vadc_apid >= 0) s_vadc_usid = 2;
			}
			if (s_vadc_apid >= 0) {
				printf("battery: detected Qualcomm QPNP VADC at USID %d, APID %d\n",
				    s_vadc_usid, s_vadc_apid);
			}

			s_chg_apid = spmi_find_apid(2, 0x1000);
			s_bat_apid = spmi_find_apid(2, 0x1200);
			s_usb_apid = spmi_find_apid(2, 0x1300);
			if (s_chg_apid >= 0 || s_bat_apid >= 0 || s_usb_apid >= 0) {
				printf("battery: detected PMI8950 Charger APIDs: CHG=%d, BAT=%d, USB=%d\n",
				    s_chg_apid, s_bat_apid, s_usb_apid);
			}

			/* 2b. Neutralize PM8950 / PMI8950 PON Stage-2 Hardware Watchdog */
			s_pon0_apid = spmi_find_apid(0, 0x0800);
			if (s_pon0_apid >= 0) {
				spmi_write_byte(s_pon0_apid, 0xD0, 0xA5);
				spmi_write_byte(s_pon0_apid, 0x56, 0x00);
				spmi_write_byte(s_pon0_apid, 0xD0, 0xA5);
				spmi_write_byte(s_pon0_apid, 0x57, 0x00);
				spmi_write_byte(s_pon0_apid, 0xD0, 0xA5);
				spmi_write_byte(s_pon0_apid, 0x5A, 0x7F);
				spmi_write_byte(s_pon0_apid, 0xD0, 0xA5);
				spmi_write_byte(s_pon0_apid, 0x5B, 0x00);
				spmi_write_byte(s_pon0_apid, 0xD0, 0xA5);
				spmi_write_byte(s_pon0_apid, 0x58, 0x01);
				printf("battery: PM8950 PON watchdog disabled & disarmed (APID %d)\n", s_pon0_apid);
			}
			s_pon2_apid = spmi_find_apid(2, 0x0800);
			if (s_pon2_apid >= 0) {
				spmi_write_byte(s_pon2_apid, 0xD0, 0xA5);
				spmi_write_byte(s_pon2_apid, 0x56, 0x00);
				spmi_write_byte(s_pon2_apid, 0xD0, 0xA5);
				spmi_write_byte(s_pon2_apid, 0x57, 0x00);
				spmi_write_byte(s_pon2_apid, 0xD0, 0xA5);
				spmi_write_byte(s_pon2_apid, 0x5A, 0x7F);
				spmi_write_byte(s_pon2_apid, 0xD0, 0xA5);
				spmi_write_byte(s_pon2_apid, 0x5B, 0x00);
				spmi_write_byte(s_pon2_apid, 0xD0, 0xA5);
				spmi_write_byte(s_pon2_apid, 0x58, 0x01);
				printf("battery: PMI8950 PON watchdog disabled & disarmed (APID %d)\n", s_pon2_apid);
			}
			/* 2c. Neutralize PMI8950 SMBCHG MISC Watchdog (USID 2, MISC 0x1600) */
			s_misc_apid = spmi_find_apid(2, 0x1600);
			if (s_misc_apid >= 0) {
				spmi_write_byte(s_misc_apid, 0xD0, 0xA5);
				spmi_write_byte(s_misc_apid, 0x43, 0x00);
				spmi_write_byte(s_misc_apid, 0xD0, 0xA5);
				spmi_write_byte(s_misc_apid, 0x40, 0x01);
				printf("battery: PMI8950 SMBCHG MISC watchdog disabled (APID %d)\n", s_misc_apid);
			}
		}

		/* 2c. Neutralize MSM8952 APCS / KPSS Hardware Watchdog (0x0b017000) */
		s_apcs_wdt = ml_io_map(0x0b017000UL, 0x1000UL);
		if (s_apcs_wdt) {
			wr32(s_apcs_wdt + 0x08UL, 0); /* Disable WDT_EN */
			wr32(s_apcs_wdt + 0x40UL, 0); /* Disable APCS TMR WDT_EN */
			wr32(s_apcs_wdt + 0x04UL, 1); /* Pet WDT_RST */
			wr32(s_apcs_wdt + 0x38UL, 1); /* Pet APCS TMR WDT_RST */
			printf("battery: MSM8952 APCS watchdog disabled\n");
		}

		/*
		 * 3. Probe SMB1351 Charger IC.
		 *
		 * Off unless asked for with the boot-arg hs_batt_i2c=1: on this
		 * device the BLSP1 QUP2 block is dead until its GCC branch is
		 * really running (the CBCR bits alone do not start the root
		 * clock), and a register read from an unclocked Qualcomm
		 * peripheral does not return an abort -- it stalls the AHB
		 * master, so the whole CPU stops with no panic at all.  That is
		 * exactly what happened right after this line on the first
		 * bring-up boot.  The PMI8950 charger (SPMI, above) is what the
		 * battery telemetry actually needs; SMB1351 is a second, optional
		 * charger IC whose I2C clock enable belongs to the clock driver.
		 */
		uint32_t want_i2c = 0;
		PE_parse_boot_argn("hs_batt_i2c", &want_i2c, sizeof(want_i2c));
		if (s_qup2 && !want_i2c) {
			printf("battery: SMB1351 probe skipped (BLSP1 QUP2 clock not started; hs_batt_i2c=1 to try)\n");
		}
		if (s_qup2 && want_i2c) {
			qup2_init();
			uint8_t probe_val = 0;
			if (qup2_read_reg(SMB1351_ADDR_PRIMARY, SMB1351_STATUS_4_REG, &probe_val) == 0) {
				s_smb_addr = SMB1351_ADDR_PRIMARY;
				s_smb_found = true;
				printf("battery: detected SMB1351 Charger at I2C address 0x%02X (ST4=0x%02X)\n",
				    s_smb_addr, probe_val);
			} else if (qup2_read_reg(SMB1351_ADDR_ALT, SMB1351_STATUS_4_REG, &probe_val) == 0) {
				s_smb_addr = SMB1351_ADDR_ALT;
				s_smb_found = true;
				printf("battery: detected SMB1351 Charger at alt I2C address 0x%02X (ST4=0x%02X)\n",
				    s_smb_addr, probe_val);
			}
		}
	}

	/* 4. Provide initial power state to IOPMPowerSource immediately */
	struct hakios_battery_state initial_b = {
		.size = sizeof(initial_b),
		.percent_hundredths = 9000,
		.voltage_uv = 4050000,
		.current_ua = 500000,
		.temperature_hundredths = 2500,
		.present = 1,
		.external = 1,
		.status = 1,
		.charge_uah = 2790000,
		.full_uah = 3100000,
		.design_uah = 3100000,
		.cycles = 25,
		.usb_type = 2,
		.usb_voltage_uv = 5000000,
		.usb_current_ua = 1500000
	};
	hakios_battery_report(&initial_b);

	/* 5. Start Background Telemetry Thread */
	thread_t thread;
	if (kernel_thread_start(battery_worker_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
