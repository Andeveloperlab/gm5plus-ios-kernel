/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include <stdbool.h>
#include "console.h"
#include "mmu.h"
#include "loader.h"
#include "ci_usb.h"
#include "shamrock_charger.h"

#define IMEM_RESTART_REASON	0x0860065cUL	/* MSM8952 IMEM restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x004ab000UL	/* MSM8952 MPM2_MPM_PS_HOLD */

#define GICD_BASE		0x0b000000UL
#define GICC_BASE		0x0b002000UL
#define GICC_IAR		(GICC_BASE + 0x0c)
#define GICC_EOIR		(GICC_BASE + 0x10)
#define VTIMER_INTID		27	/* Standard ARM Arch timer PPI 11 -> INTID 27 */
#define PTIMER_INTID		26	/* PPI 10 -> INTID 26 */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];
static void print_hex64(uint64_t val);
static void run_usb_shell(uint64_t dtb);

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : (after_s > 0 ? "in 20s" : "now"));
	for (int i = 0; i < 50000; i++)
		ci_usb_poll();
	if (after_s > 0)
		delay_ms(after_s * 1000);
	shamrock_set_fastboot_reboot_reason();
	__asm__ volatile("dsb sy" ::: "memory");
	/* Call QCOM_SCM_IO_DISABLE_PMIC_ARBITER (0x82000901 atomic, nargs=1, arg0=0) */
	if (smc(0x82000901UL, 1, 0, 0) != 0) {
		smc(0x82000902UL, 1, 0, 0); /* retry with SCM_IO_DISABLE_PMIC_ARBITER1 */
	}
	wr32(PSHOLD, 0);
	delay_ms(100);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint32_t iar = rd32(GICC_IAR);
	uint32_t id = iar & 0x3ff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wr32(GICC_EOIR, iar);
	isb();
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if ((slot & 3) == 1 || (slot & 3) == 2) {	/* IRQ or FIQ (any EL/stack pointer) */
		irq_handler();
		return;
	}

	/* Re-init USB CDC serial so diagnostic messages reach host PC */
	ci_usb_init();

	con_clear_top(RED, 220);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */

	ci_log_puts("\r\n=======================================================\r\n");
	ci_log_puts("   [!] SHIM EXCEPTION HANDLER (FATAL TRAP)!\r\n");
	ci_log_puts("=======================================================\r\n");
	ci_log_puts("Slot: "); print_hex64(slot); ci_log_puts("\r\n");
	ci_log_puts("CPU: "); print_hex64(rsr("mpidr_el1")); ci_log_puts("\r\n");
	ci_log_puts("ESR_EL1: "); print_hex64(rsr("esr_el1")); ci_log_puts("\r\n");
	ci_log_puts("ELR_EL1 (PC): "); print_hex64(rsr("elr_el1")); ci_log_puts("\r\n");
	ci_log_puts("FAR_EL1 (Addr): "); print_hex64(rsr("far_el1")); ci_log_puts("\r\n");
	ci_log_puts("X0: "); print_hex64(regs[0]); ci_log_puts("\r\n");
	ci_log_puts("X1: "); print_hex64(regs[1]); ci_log_puts("\r\n");
	ci_log_puts("X30: "); print_hex64(regs[29]); ci_log_puts("\r\n");

	/* Stay in USB shell instead of rebooting into Android */
	run_usb_shell(0);
}

/* ---- SMP ---- */

void secondary_main(uint64_t idx)
{
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GICv2 ---- */

static void test_gic(void)
{
	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	uint32_t cctlr = rd32(GICC_BASE);
	con_hex2("GICv2", "gicd=", dctlr, "gicc=", cctlr);
}

static uint64_t parse_hex(const char *s)
{
	uint64_t v = 0;
	while (*s == ' ' || *s == '\t') s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
	while (*s) {
		char c = *s++;
		if (c >= '0' && c <= '9') v = (v << 4) | (c - '0');
		else if (c >= 'a' && c <= 'f') v = (v << 4) | (c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v = (v << 4) | (c - 'A' + 10);
		else break;
	}
	return v;
}

static void print_hex32(uint32_t val)
{
	static const char hex[] = "0123456789ABCDEF";
	char buf[11];
	buf[0] = '0'; buf[1] = 'x';
	for (int i = 0; i < 8; i++)
		buf[2 + i] = hex[(val >> (28 - i * 4)) & 0xF];
	buf[10] = 0;
	ci_log_puts(buf);
}

static void print_hex64(uint64_t val)
{
	static const char hex[] = "0123456789ABCDEF";
	char buf[19];
	buf[0] = '0'; buf[1] = 'x';
	for (int i = 0; i < 16; i++)
		buf[2 + i] = hex[(val >> (60 - i * 4)) & 0xF];
	buf[18] = 0;
	ci_log_puts(buf);
}

static int strncmp_simple(const char *a, const char *b, int n)
{
	for (int i = 0; i < n; i++) {
		if (a[i] != b[i] || a[i] == 0 || b[i] == 0)
			return (unsigned char)a[i] - (unsigned char)b[i];
	}
	return 0;
}

static uint64_t g_saved_log_pa = 0;
static uint64_t g_saved_log_len = 0;

static void run_usb_shell(uint64_t dtb)
{
	ci_log_puts("\r\n[shamrock-usb-shell active]\r\n");
	ci_log_puts("Type 'help' for commands, 'chg' for battery, or 'boot' to run XNU.\r\n");
	ci_log_puts("shamrock> ");

	char cmd_buf[128];
	uint32_t cmd_idx = 0;
	uint64_t last_chg_keepalive = now();

	for (;;) {
		ci_usb_poll();

		/* Every ~2 seconds (38.4M ticks at 19.2MHz), pet charger & re-verify HC mode */
		uint64_t cur_ticks = now();
		if (cur_ticks - last_chg_keepalive >= 38400000ULL) {
			last_chg_keepalive = cur_ticks;
			shamrock_charger_keepalive();
		}

		uint8_t ch;
		if (ci_rx_read(&ch, 1) == 1) {
			if (ch == 0x03) { /* Ctrl-C */
				ci_log_puts("^C\r\nshamrock> ");
				cmd_idx = 0;
				continue;
			}
			if (ch == '\r' || ch == '\n') {
				ci_log_puts("\r\n");
				if (cmd_idx > 0) {
					cmd_buf[cmd_idx] = 0;

					if (strncmp_simple(cmd_buf, "help", 4) == 0) {
						ci_log_puts("Commands:\r\n");
						ci_log_puts("  help             - Show this help\r\n");
						ci_log_puts("  info             - Show system info\r\n");
						ci_log_puts("  chg              - Show live battery charging status\r\n");
						ci_log_puts("  log              - Dump captured XNU kernel log\r\n");
						ci_log_puts("  precond          - Check ChipIdea & PHY status\r\n");
						ci_log_puts("  peek <addr>      - Read 32-bit register (hex)\r\n");
						ci_log_puts("  poke <addr> <val>- Write 32-bit register (hex)\r\n");
						ci_log_puts("  boot / xnu       - Boot XNU kernel\r\n");
						ci_log_puts("  reboot / fastboot- Reboot to fastboot\r\n#\r\n");
					} else if (strncmp_simple(cmd_buf, "log", 3) == 0) {
						volatile uint32_t *plog = (volatile uint32_t *)0x831F4000UL;
						if (*plog == 0x53484D52U) {
							uint32_t len = plog[1];
							ci_log_puts("\r\n--- Persistent XNU RAM Log (0x831F4000) ---\r\n");
							const char *text = (const char *)0x831F4010UL;
							for (uint32_t i = 0; i < len && i < 48000; i++) {
								char cbuf[2] = {text[i], 0};
								ci_log_puts(cbuf);
							}
							ci_log_puts("\r\n--- End of Persistent Log ---\r\n#\r\n");
						} else if (g_saved_log_pa && g_saved_log_len) {
							ci_log_puts("\r\n--- Captured XNU Kernel Log ---\r\n");
							const char *log = (const char *)(uintptr_t)g_saved_log_pa;
							for (uint64_t i = 0; i < g_saved_log_len && i < 65536; i++) {
								char cbuf[2] = {log[i], 0};
								ci_log_puts(cbuf);
							}
							ci_log_puts("\r\n--- End of Kernel Log ---\r\n#\r\n");
						} else {
							ci_log_puts("No kernel log captured yet.\r\n#\r\n");
						}
					} else if (strncmp_simple(cmd_buf, "chg", 3) == 0) {
						char report[256];
						shamrock_charger_status_str(report, sizeof(report));
						ci_log_puts(report);
						ci_log_puts("#\r\n");
					} else if (strncmp_simple(cmd_buf, "info", 4) == 0) {
						ci_log_puts("Target: General Mobile GM 5 Plus (shamrock / MSM8952)\r\n");
						ci_log_puts("DTB at: "); print_hex64(dtb); ci_log_puts("\r\n");
						ci_log_puts("SCTLR_EL1: "); print_hex64(rsr("sctlr_el1")); ci_log_puts("\r\n");
						ci_log_puts("TCR_EL1: "); print_hex64(rsr("tcr_el1")); ci_log_puts("\r\n");
						ci_log_puts("PSCI: "); print_hex64(smc(PSCI_VERSION, 0, 0, 0)); ci_log_puts("\r\n#\r\n");
					} else if (strncmp_simple(cmd_buf, "gic", 3) == 0) {
						test_gic();
						ci_log_puts("#\r\n");
					} else if (strncmp_simple(cmd_buf, "precond", 7) == 0) {
						int rc = ci_check_preconditions(MSM8952_USB_BASE, MSM8952_USB_PHY_CSR_BASE);
						ci_log_puts("Precondition code: ");
						char code[4] = {'0' + (rc < 0 ? -rc : rc), '\r', '\n', 0};
						if (rc < 0) ci_log_puts("-");
						ci_log_puts(code);
						ci_log_puts("#\r\n");
					} else if (strncmp_simple(cmd_buf, "peek", 4) == 0) {
						uint64_t addr = parse_hex(cmd_buf + 4);
						uint32_t val = rd32(addr);
						ci_log_puts("["); print_hex64(addr); ci_log_puts("] = ");
						print_hex32(val); ci_log_puts("\r\n#\r\n");
					} else if (strncmp_simple(cmd_buf, "poke", 4) == 0) {
						const char *p = cmd_buf + 4;
						while (*p == ' ') p++;
						uint64_t addr = parse_hex(p);
						while (*p && *p != ' ') p++;
						while (*p == ' ') p++;
						uint64_t val = parse_hex(p);
						wr32(addr, (uint32_t)val);
						ci_log_puts("Written ["); print_hex64(addr); ci_log_puts("] = ");
						print_hex32((uint32_t)val); ci_log_puts("\r\n#\r\n");
					} else if (strncmp_simple(cmd_buf, "boot", 4) == 0 || strncmp_simple(cmd_buf, "xnu", 3) == 0) {
						ci_log_puts("Booting XNU kernel...\r\n#\r\n");
						loader_boot(dtb);
					} else if (strncmp_simple(cmd_buf, "reboot", 6) == 0 || strncmp_simple(cmd_buf, "fastboot", 8) == 0) {
						ci_log_puts("Rebooting...\r\n#\r\n");
						reset(0);
					} else {
						ci_log_puts("Unknown command: ");
						ci_log_puts(cmd_buf);
						ci_log_puts("\r\n#\r\n");
					}
					cmd_idx = 0;
				}
				ci_log_puts("shamrock> ");
			} else if (ch == '\b' || ch == 127) {
				if (cmd_idx > 0) {
					cmd_idx--;
					ci_log_puts("\b \b");
				}
			} else if (cmd_idx < sizeof(cmd_buf) - 1) {
				cmd_buf[cmd_idx++] = ch;
				char echo[2] = {(char)ch, 0};
				ci_log_puts(echo);
			}
		}
	}
}

void shim_main(uint64_t dtb)
{
	/* 1. Preconditions & Bring-up USB CDC-ACM Serial early */
	int pre = ci_check_preconditions(MSM8952_USB_BASE, MSM8952_USB_PHY_CSR_BASE);
	ci_usb_init();

	/* 2. Bring up SMB1351 (@0x1d on blsp1_i2c2) & PMI8950 (SPMI USID 2) charging */
	shamrock_charger_init();
	/* Pre-arm PM8950 PON_SOFT_RB_SPARE = FASTBOOT_MODE (0x08) + WARM_RESET so any reset lands in fastboot! */
	shamrock_set_fastboot_reboot_reason();

	con_clear(RED);
	con_str("SHIM", "hackios shamrock: xnu loader (MSM8952)");
	con_hex2("USB", "precond=", pre, "devaddr=", rd32(MSM8952_USB_BASE + 0x154));
	con_hex2("GENCFG", "cfg2=", rd32(MSM8952_USB_BASE + 0x00A0), "ahb=", rd32(MSM8952_USB_BASE + 0x0098));
	con_hex2("PORTSC", "portsc=", rd32(MSM8952_USB_BASE + 0x184), "usbcmd=", rd32(MSM8952_USB_BASE + 0x140));
	con_hex2("OTGSC",  "otgsc=",  rd32(MSM8952_USB_BASE + 0x1a4), "usbsts=", rd32(MSM8952_USB_BASE + 0x144));

	char chg_status[128];
	shamrock_charger_status_str(chg_status, sizeof(chg_status));
	con_str("CHG", shamrock_charger_is_present() ? "SMB1351 FAST-CHG 1.5A ACTIVE" : "PMI8950 SPMI CHG ACTIVE");

	ci_log_puts("\r\n=======================================================\r\n");
	ci_log_puts("   GM 5 Plus (shamrock) Bare-Metal Shim USB Debug Port\r\n");
	ci_log_puts("=======================================================\r\n");
	ci_log_puts(chg_status);
	ci_log_puts("Commands: help, info, chg, precond, peek, poke, boot, reboot\r\n");
	ci_log_puts("Type 'boot' to start XNU kernel payload.\r\n\r\n");

	con_str("SHELL", "Waiting for command (type 'boot' to run XNU)...");
	run_usb_shell(dtb);
}

void shim_on_xnu_return(uint64_t reason, uint64_t esr, uint64_t elr, uint64_t far, uint64_t log_pa, uint64_t log_len)
{
	g_saved_log_pa = log_pa;
	g_saved_log_len = log_len;

	ci_usb_init();
	shamrock_charger_init();
	shamrock_set_fastboot_reboot_reason();

	/* Clear only top 160px so all diagnostic pixel bands below remain intact! */
	con_clear_top(RED, 160);
	con_str("TRAP", "XNU kernel returned / exception caught!");
	con_hex2("EXC", "reason=", (uint32_t)reason, "esr=", (uint32_t)esr);
	con_hex2("PC", "elr=", (uint32_t)elr, "far=", (uint32_t)far);

	ci_log_puts("\r\n=======================================================\r\n");
	ci_log_puts("   [!] XNU KERNEL RETURN / EXCEPTION CAPTURED!\r\n");
	ci_log_puts("=======================================================\r\n");
	ci_log_puts("Reason: "); print_hex64(reason); ci_log_puts("\r\n");
	ci_log_puts("ESR_EL1: "); print_hex64(esr); ci_log_puts("\r\n");
	ci_log_puts("ELR_EL1 (PC): "); print_hex64(elr); ci_log_puts("\r\n");
	ci_log_puts("FAR_EL1 (Addr): "); print_hex64(far); ci_log_puts("\r\n");

	if (log_pa && log_len) {
		ci_log_puts("\r\n--- Captured XNU Kernel Log ---\r\n");
		const char *log = (const char *)(uintptr_t)log_pa;
		for (uint64_t i = 0; i < log_len && i < 65536; i++) {
			char ch[2] = {log[i], 0};
			ci_log_puts(ch);
		}
		ci_log_puts("\r\n--- End of Kernel Log ---\r\n");
	}

	run_usb_shell(0);
}
