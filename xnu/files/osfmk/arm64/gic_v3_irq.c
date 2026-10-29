/*
 * Minimal GICv3 support for generic (non-Apple) ARMv8 boards.
 *
 * Open-source XNU has no interrupt controller driver: on Apple hardware that
 * is a kext (AppleInterruptController), and the timer arrives as FIQ. Here
 * everything is a Group 1 IRQ, which is what an EL1 guest gets from a stock
 * hypervisor or from QEMU's virt machine:
 *
 *   - the CPU interface is driven through the ICC_*_EL1 system registers
 *   - the EL1 virtual timer PPI is routed to the kernel's rtclock
 *   - with more than one processor (psci_smp.c), two software-generated
 *     interrupts carry the kernel's signals between them: cpu_signal() and
 *     the order to stop when the system goes back to the boot loader
 *   - a shared peripheral interrupt (SPI, numbers 32 to 1019) is a driver's once it
 *     is attached (gic_v3_spi_attach()): configured, routed to the processor that
 *     attaches it, enabled, and its handler called in interrupt context for each
 *     one that arrives, before it is ended
 *   - any other interrupt that fires is masked and counted, until a real
 *     driver registers a handler for it
 *
 * Every processor has its own CPU interface, redistributor frame and timer,
 * and sets them up for itself (gic_v3_irq_cpu_init(), from PE_init_cpu()).
 *
 * Registers come from the device tree node /arm-io/gic:
 *   reg = <distributor offset, size, redistributor offset, size>
 */
#include <mach/mach_types.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>   /* printf(): goes to the console, kprintf() does not on this board */
#include <kern/simple_lock.h>
#include <os/atomic_private.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_cpu.h>
#include <arm/machine_routines.h>
#include <arm/rtclock.h>
#include <arm64/psci_smp.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>

#if defined(GENERIC_GIC_V3_IRQ)

#define GICD_CTLR               0x0000
#define GICD_TYPER              0x0004
#define GICD_CTLR_ENABLE_G1NS   (1u << 1)
#define GICD_CTLR_ARE_NS        (1u << 4)
#define GICD_CTLR_RWP           (1u << 31)
#define GICD_IGROUPR(n)         (0x0080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x0100 + 4 * (n))
#define GICD_ICENABLER(n)       (0x0180 + 4 * (n))
#define GICD_ICPENDR(n)         (0x0280 + 4 * (n))
#define GICD_ICACTIVER(n)       (0x0380 + 4 * (n))
#define GICD_IPRIORITYR(n)      (0x0400 + (n))
#define GICD_ICFGR(n)           (0x0c00 + 4 * (n))
#define GICD_IGRPMODR(n)        (0x0d00 + 4 * (n))
#define GICD_IROUTER(n)         (0x6000 + 8 * (n))

#define GICR_FRAME_SIZE         0x20000     /* RD_base + SGI_base per CPU */
#define GICR_TYPER              0x0008
#define GICR_TYPER_LAST         (1u << 4)
#define GICR_WAKER              0x0014
#define GICR_WAKER_PROCSLEEP    (1u << 1)
#define GICR_WAKER_CHILDASLEEP  (1u << 2)
#define GICR_SGI_BASE           0x10000
#define GICR_IGROUPR0           (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0         (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0         (GICR_SGI_BASE + 0x0180)
#define GICR_ICPENDR0           (GICR_SGI_BASE + 0x0280)
#define GICR_ICACTIVER0         (GICR_SGI_BASE + 0x0380)
#define GICR_IPRIORITYR(n)      (GICR_SGI_BASE + 0x0400 + (n))
#define GICR_IGRPMODR0          (GICR_SGI_BASE + 0x0d00)

#define GIC_SGI_SIGNAL          0           /* cpu_signal(): another processor wants this one's attention */
#define GIC_SGI_STOP            1           /* psci_smp_stop_here(): the system is going back to the boot loader */
#define GIC_INTID_VTIMER        27          /* EL1 virtual timer PPI */
#define GIC_INTID_SPI_MIN       32
#define GIC_INTID_SPURIOUS_MIN  1020
#define GIC_SPI_MOST            48          /* attached at a time */
#define GIC_PRIORITY_DEFAULT    0xa0

/* GICv2 CPU Interface registers (MMIO) */
#define GICC_CTLR               0x0000
#define GICC_PMR                0x0004
#define GICC_BPR                0x0008
#define GICC_IAR                0x000C
#define GICC_EOIR               0x0010
#define GICC_RPR                0x0014
#define GICC_HPPIR              0x0018
#define GICC_AIAR               0x0020
#define GICC_AEOIR              0x0024
#define GICC_AHPPIR             0x0028
#define GICC_IIDR               0x00FC
#define GICC_DIR                0x1000

/* GICD_CTLR bits (GICv2): both group enables, and DS (one security state, everything Group 1) */
#define GICD_CTLR_ENABLEGRP0    (1u << 0)
#define GICD_CTLR_ENABLEGRP1    (1u << 1)
#define GICD_CTLR_DS            (1u << 6)
#define GICC_CTLR_ENABLEGRP0    (1u << 0)
#define GICC_CTLR_ENABLEGRP1    (1u << 1)

/* GICv2 Distributor registers (MMIO) */
#define GICD_ITARGETSR(n)       (0x0800 + 4 * (n))
#define GICD_SGIR               0x0F00

static bool is_gicv2;
static vm_offset_t gicc_base;
static vm_offset_t gicd_base, gicr_base;
static vm_size_t   gicr_size;
static vm_offset_t gicr_cpu_base[MAX_CPUS]; /* each processor's redistributor frame */
static vm_offset_t s_gic_apcs_wdt; /* MSM8952 APCS KPSS WDT MMIO */

/*
 * Acknowledged, not yet EOI'd; 0: none. The signal between processors is interrupt 0 and reads
 * as 0 from ICC_IAR1_EL1, hence the mark: without it a core switched off in the middle of that
 * signal (which is where every core but one is during a panic) kept it active, and with the
 * active priority still set in its CPU interface the next kernel on that core saw no interrupt
 * at all.
 */
#define GIC_IAR_TAKEN           (1ULL << 63)
static uint64_t gic_active_iar[MAX_CPUS];
uint64_t        gic_unhandled_count;
uint32_t        gic_last_unhandled;
uint64_t        gic_vtimer_ticks;       /* BRINGUP: virtual timer interrupts taken so far */

/*
 * BRINGUP: prove the interrupt path end to end without depending on the timer. SGI 8 is
 * enabled in the CPU interface and, at report time, sent to this very processor. If its
 * counter moves the GIC delivers, the handler runs and the vector is right -- and then the
 * virtual timer's silence is a timer problem, not an interrupt problem.
 */
#define GIC_SGI_PROBE           8
uint64_t        gic_sgi_probe_ticks;
static uint32_t gic_v2_ctlr_before, gic_v2_gicc_before;     /* what the firmware left behind */

/*
 * The attached SPIs. A slot is filled before its interrupt is enabled and emptied after it has
 * been disabled and no handler of it runs any more (busy), so the handler path reads the table
 * without a lock.
 */
struct gic_spi {
	uint32_t intid;                     /* 0: a free slot */
	void   (*handler)(void *context, uint32_t intid);
	void    *context;
	uint32_t busy;                      /* handlers of it running now */
	uint64_t count;
};
static struct gic_spi gic_spi[GIC_SPI_MOST];
static decl_simple_lock_data(, gic_spi_lock);

void gic_v3_irq_cpu_init(void);
void gic_v3_irq_quiesce(void);
kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
void gic_v3_spi_mask(uint32_t intid);
void gic_v3_spi_unmask(uint32_t intid);
void gic_v3_spi_detach(uint32_t intid);
uint64_t gic_v3_spi_count(uint32_t intid);
void gic_v3_irq_cpu_shutdown(void);
void gic_v3_irq_signal(uint32_t phys_id);
void gic_v3_irq_signal_stop(uint32_t phys_id);

static inline uint32_t
gic_read32(vm_offset_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void
gic_write32(vm_offset_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

static void
gic_mask(uint32_t intid)
{
	if (is_gicv2) {
		if (intid < 32) {
			gic_write32(gicd_base + GICD_ICENABLER(0), 1u << intid);
		} else {
			gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
		}
		return;
	}
	if (intid < 32) {
		gic_write32(gicr_cpu_base[cpu_number()] + GICR_ICENABLER0, 1u << intid);
	} else {
		gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
	}
}

/*
 * A software-generated interrupt for the processor with that MPIDR (its two
 * low affinity fields, as in the device tree's "reg").
 * ICC_SGI1R_EL1: Aff1 in [23:16], the interrupt in [27:24], and in [15:0] a
 * bit for every Aff0 addressed -- which is why Aff0 has to be below 16.
 */
static void
gic_send_sgi(uint32_t phys_id, uint32_t sgi)
{
	if (is_gicv2) {
		uint32_t target_list;
		if (phys_id == 0xffffffffu) {
			target_list = (1u << 24); /* all other CPUs */
		} else {
			int target_cpu = 0;
			for (int c = 0; c < ml_get_cpu_count(); c++) {
				if (cpu_datap(c)->cpu_phys_id == phys_id) {
					target_cpu = c;
					break;
				}
			}
			target_list = ((1u << target_cpu) & 0xff) << 16;
		}
		gic_write32(gicd_base + GICD_SGIR, target_list | (sgi & 0xf));
		return;
	}
	uint64_t aff0 = phys_id & 0xff, aff1 = (phys_id >> 8) & 0xff;

	__builtin_arm_wsr64("ICC_SGI1R_EL1", ((uint64_t)sgi << 24) | (aff1 << 16) | (1ULL << (aff0 & 15)));
	__builtin_arm_isb(ISB_SY);
}

/* The platform's half of cpu_signal(): interrupt that processor (GenericARMCPU::signalCPU()). */
void
gic_v3_irq_signal(uint32_t phys_id)
{
	gic_send_sgi(phys_id, GIC_SGI_SIGNAL);
}

void
gic_v3_irq_signal_stop(uint32_t phys_id)
{
	gic_send_sgi(phys_id, GIC_SGI_STOP);
}

/* An SPI that a driver has attached: its handler called. false: nobody has it. */
static bool
gic_spi_handle(uint32_t intid)
{
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		struct gic_spi *spi = &gic_spi[i];

		if (os_atomic_load(&spi->intid, acquire) == intid) {
			os_atomic_inc(&spi->busy, acquire);
			if (os_atomic_load(&spi->intid, relaxed) == intid) {
				spi->count++;
				spi->handler(spi->context, intid);
			}
			os_atomic_dec(&spi->busy, release);
			return true;
		}
	}
	return false;
}

static void
gic_distributor_settle(void)
{
	while (gic_read32(gicd_base + GICD_CTLR) & GICD_CTLR_RWP) {
		/* wait for the change to take effect */
	}
}

void
gic_v3_spi_mask(uint32_t intid)
{
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
		gic_distributor_settle();
	}
}

void
gic_v3_spi_unmask(uint32_t intid)
{
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic_write32(gicd_base + GICD_ISENABLER(intid / 32), 1u << (intid % 32));
	}
}

/*
 * A driver takes an SPI: edge- or level-triggered, Group 1, the default priority, routed to the
 * processor this runs on, and enabled. The handler runs in interrupt context, with interrupts off,
 * before the interrupt is ended; for a level-triggered one it has to quiet the source or mask the
 * interrupt (gic_v3_spi_mask()). Thread context.
 */
kern_return_t
gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context)
{
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	struct gic_spi *slot = NULL;
	uint32_t config;

	if (gicd_base == 0 || intid < GIC_INTID_SPI_MIN || intid >= GIC_INTID_SPURIOUS_MIN || handler == NULL) {
		return KERN_INVALID_ARGUMENT;
	}
	simple_lock(&gic_spi_lock, LCK_GRP_NULL);
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			simple_unlock(&gic_spi_lock);
			return KERN_NAME_EXISTS;
		}
		if (slot == NULL && gic_spi[i].intid == 0 && gic_spi[i].busy == 0) {
			slot = &gic_spi[i];
		}
	}
	if (slot == NULL) {
		simple_unlock(&gic_spi_lock);
		return KERN_RESOURCE_SHORTAGE;
	}
	slot->handler = handler;
	slot->context = context;
	slot->count = 0;
	os_atomic_store(&slot->intid, intid, release);
	simple_unlock(&gic_spi_lock);

	gic_v3_spi_mask(intid);
	gic_write32(gicd_base + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
	gic_write32(gicd_base + GICD_ICACTIVER(intid / 32), 1u << (intid % 32));

	if (is_gicv2) {
		int cpu = cpu_number();
		uint32_t shift = (intid % 4) * 8;
		uint32_t t_reg = GICD_ITARGETSR(intid / 4);
		uint32_t p_reg = GICD_IPRIORITYR(intid / 4);

		uint32_t t_val = gic_read32(gicd_base + t_reg);
		t_val &= ~(0xffu << shift);
		t_val |= ((1u << cpu) << shift);
		gic_write32(gicd_base + t_reg, t_val);

		uint32_t p_val = gic_read32(gicd_base + p_reg);
		p_val &= ~(0xffu << shift);
		p_val |= (GIC_PRIORITY_DEFAULT << shift);
		gic_write32(gicd_base + p_reg, p_val);

		config = gic_read32(gicd_base + GICD_ICFGR(intid / 16)) & ~(3u << (intid % 16 * 2));
		gic_write32(gicd_base + GICD_ICFGR(intid / 16), config | (edge ? 2u : 0u) << (intid % 16 * 2));
		gic_v3_spi_unmask(intid);
		return KERN_SUCCESS;
	}

	gic_write32(gicd_base + GICD_IGROUPR(intid / 32), gic_read32(gicd_base + GICD_IGROUPR(intid / 32)) | 1u << (intid % 32));
	gic_write32(gicd_base + GICD_IGRPMODR(intid / 32), gic_read32(gicd_base + GICD_IGRPMODR(intid / 32)) & ~(1u << (intid % 32)));
	*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(intid)) = GIC_PRIORITY_DEFAULT;
	config = gic_read32(gicd_base + GICD_ICFGR(intid / 16)) & ~(3u << (intid % 16 * 2));
	gic_write32(gicd_base + GICD_ICFGR(intid / 16), config | (edge ? 2u : 0u) << (intid % 16 * 2));
	/* GICD_IROUTER: Aff3 in [39:32], Aff2.Aff1.Aff0 in [23:0]; bit 31 (any processor) clear */
	*(volatile uint64_t *)(gicd_base + GICD_IROUTER(intid)) = (mpidr & 0xff00000000ull) | (mpidr & 0xffffff);
	gic_distributor_settle();
	gic_v3_spi_unmask(intid);
	return KERN_SUCCESS;
}

/* ... and gives it back: disabled, nothing pending, and no handler of it running when this returns. */
void
gic_v3_spi_detach(uint32_t intid)
{
	gic_v3_spi_mask(intid);
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS_MIN) {
		gic_write32(gicd_base + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
	}
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			os_atomic_store(&gic_spi[i].intid, 0, release);
			while (os_atomic_load(&gic_spi[i].busy, acquire) != 0) {
				/* a handler of it on another processor is finishing */
			}
		}
	}
}

/* How many have arrived since it was attached. */
uint64_t
gic_v3_spi_count(uint32_t intid)
{
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			return gic_spi[i].count;
		}
	}
	return 0;
}

static void
gic_irq_handler(__unused void *target, __unused void *refCon, __unused void *nub, __unused int source)
{
	int cpu = cpu_number();

	for (;;) {
		uint32_t intid;
		uint64_t iar = 0;
		bool acked_group1 = false;

		if (is_gicv2) {
			uint32_t iar32 = gic_read32(gicc_base + GICC_IAR);
			intid = iar32 & 0x3ff;
			iar = iar32;
			/*
			 * A controller in a single security state (DS=1, what MSM8952 comes out of
			 * ABL with) has no Group 0: the Group 0 acknowledge register then reads
			 * spurious for ever while the interrupt waits in the Group 1 one. Ask there
			 * too before calling this spurious. (With two states, the second read just
			 * returns spurious when nothing in Group 1 is pending.)
			 */
			if (intid >= GIC_INTID_SPURIOUS_MIN) {
				uint32_t ai = gic_read32(gicc_base + GICC_AIAR);

				if ((ai & 0x3ff) < GIC_INTID_SPURIOUS_MIN) {
					iar32 = ai;
					intid = ai & 0x3ff;
					iar = ai;
					acked_group1 = true;
				}
			}
		} else {
			iar = __builtin_arm_rsr64("ICC_IAR1_EL1");
			intid = (uint32_t)(iar & 0xffffff);
		}

		if (intid >= GIC_INTID_SPURIOUS_MIN) {
			break;
		}
		gic_active_iar[cpu] = iar | GIC_IAR_TAKEN;

		if (intid == GIC_INTID_VTIMER) {
			/* same sequence as the FIQ path in sleh_fiq() */
			gic_vtimer_ticks++;
			getCpuDatap()->cpu_decrementer = -1; /* Large */

			/* Periodic hardware watchdog petting directly in timer interrupt */
			if ((gic_vtimer_ticks & 0x1ff) == 0 && s_gic_apcs_wdt) {
				*(volatile uint32_t *)(s_gic_apcs_wdt + 0x04UL) = 1;
				*(volatile uint32_t *)(s_gic_apcs_wdt + 0x08UL) = 0;
				*(volatile uint32_t *)(s_gic_apcs_wdt + 0x38UL) = 1;
				*(volatile uint32_t *)(s_gic_apcs_wdt + 0x40UL) = 0;
			}

			rtclock_intr(TRUE);
		} else if (intid == GIC_SGI_PROBE) {
			gic_sgi_probe_ticks++;
		} else if (intid == GIC_SGI_SIGNAL) {
			cpu_signal_handler();
		} else if (intid == GIC_SGI_STOP) {
			/* comes back only if nobody has in fact asked for that */
			psci_smp_stop_here();
		} else if (intid >= GIC_INTID_SPI_MIN && gic_spi_handle(intid)) {
			/* a driver's */
		} else {
			/* nobody owns this interrupt yet: keep it from storming */
			gic_mask(intid);
			gic_unhandled_count++;
			gic_last_unhandled = intid;
		}

		if (is_gicv2) {
			gic_write32(gicc_base + (acked_group1 ? GICC_AEOIR : GICC_EOIR), (uint32_t)iar);
		} else {
			__builtin_arm_wsr64("ICC_EOIR1_EL1", iar);
			__builtin_arm_isb(ISB_SY);
		}
		gic_active_iar[cpu] = 0;
	}
}

/*
 * Leave the controller in a state another environment can take over from:
 * finish any interrupt that this processor acknowledged but did not complete.
 * Used when control goes back to the boot loader (see loader_return.c), and
 * by a processor that is about to be switched off for that (psci_smp.c).
 */
void
gic_v3_irq_quiesce(void)
{
	int cpu;

	if (gicd_base == 0) {
		return;         /* as early as this there is no interrupt, nor a cpu number to ask for */
	}
	cpu = cpu_number();
	/* (what drivers had of the shared interrupts goes with this kernel: none of them may fire under the loader) */
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		uint32_t intid = gic_spi[i].intid;

		if (intid != 0) {
			gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
		}
	}
	if (gic_active_iar[cpu] != 0) {
		if (is_gicv2) {
			gic_write32(gicc_base + GICC_EOIR, (uint32_t)(gic_active_iar[cpu] & ~GIC_IAR_TAKEN));
		} else {
			__builtin_arm_wsr64("ICC_EOIR1_EL1", gic_active_iar[cpu] & ~GIC_IAR_TAKEN);
			__builtin_arm_isb(ISB_SY);
		}
		gic_active_iar[cpu] = 0;
	}
}

/*
 * This processor is leaving for good: the firmware is about to be asked to switch it off
 * (psci_smp_cpu_off(), and the boot processor in the loader's stub when the kernel has a core of
 * its own). Nothing may be left that could interrupt it. The redistributor outlives the core, and
 * so does what is pending in it: the "stop" signal that reached a core whose interrupts were off
 * (every core is in that state while another one panics), or its timer. A core asked to go with
 * an interrupt pending and the CPU interface still passing it on does not go cleanly, and the
 * next kernel that is started on it hangs.
 */
void
gic_v3_irq_cpu_shutdown(void)
{
	vm_offset_t gicr;

	if (gicd_base == 0) {
		return;
	}
	gic_v3_irq_quiesce();
	if (is_gicv2) {
		if (gicc_base != 0) {
			gic_write32(gicc_base + GICC_CTLR, 0);
		}
		return;
	}
	gicr = gicr_cpu_base[cpu_number()];
	if (gicr != 0) {
		gic_write32(gicr + GICR_ICENABLER0, 0xffffffffu);
		gic_write32(gicr + GICR_ICPENDR0, 0xffffffffu);
		gic_write32(gicr + GICR_ICACTIVER0, 0xffffffffu);
	}
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 0);
	__builtin_arm_isb(ISB_SY);
}

static vm_offset_t
gic_find_redistributor(void)
{
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	/* GICR_TYPER[63:32] packs Aff3.Aff2.Aff1.Aff0 */
	uint32_t affinity = (uint32_t)(((mpidr >> 32) & 0xff) << 24 | (mpidr & 0xffffff));

	for (vm_offset_t frame = gicr_base; frame + GICR_FRAME_SIZE <= gicr_base + gicr_size; frame += GICR_FRAME_SIZE) {
		uint64_t typer = *(volatile uint64_t *)(frame + GICR_TYPER);

		if ((uint32_t)(typer >> 32) == affinity) {
			return frame;
		}
		if (typer & GICR_TYPER_LAST) {
			break;
		}
	}
	panic("gic: no redistributor for MPIDR 0x%llx", mpidr);
}

static bool
gic_str_contains(const char *s, const char *sub)
{
	if (!s || !sub) return false;
	size_t slen = strlen(s), sublen = strlen(sub);
	if (sublen > slen) return false;
	for (size_t i = 0; i <= slen - sublen; i++) {
		if (strncmp(s + i, sub, sublen) == 0) return true;
	}
	return false;
}

/* Per-CPU bring-up: every processor calls this for itself, the boot processor first. */
void
gic_v3_irq_cpu_init(void)
{
	int cpu = cpu_number();
	vm_offset_t gicr;
	uint32_t mine = 1u << GIC_INTID_VTIMER;

	if (gicd_base == 0) {
		DTEntry entry;
		void const *prop;
		unsigned int prop_size;

		if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
		    SecureDTGetProperty(entry, "reg", &prop, &prop_size) != kSuccess ||
		    prop_size < 4 * sizeof(uint64_t)) {
			panic("gic: no usable /arm-io/gic node in the device tree");
		}
		uint64_t const *reg = prop;
		vm_offset_t soc_base = pe_arm_get_soc_base_phys();

		if (SecureDTGetProperty(entry, "compatible", &prop, &prop_size) == kSuccess) {
			const char *compat = (const char *)prop;
			for (unsigned int o = 0; o < prop_size; o += strlen(compat + o) + 1) {
				if (gic_str_contains(compat + o, "gic-v2") ||
				    gic_str_contains(compat + o, "cortex-a15-gic") ||
				    gic_str_contains(compat + o, "gic-400")) {
					is_gicv2 = true;
					break;
				}
			}
		}

		simple_lock_init(&gic_spi_lock, 0);
		gicd_base = ml_io_map(soc_base + reg[0], reg[1]);
		if (is_gicv2) {
			gicc_base = ml_io_map(soc_base + reg[2], reg[3]);
			if (gicd_base == 0 || gicc_base == 0) {
				panic("gic: cannot map the v2 distributor/cpu interface");
			}
		} else {
			gicr_base = ml_io_map(soc_base + reg[2], reg[3]);
			gicr_size = reg[3];
			if (gicd_base == 0 || gicr_base == 0) {
				panic("gic: cannot map the distributor/redistributor");
			}
		}
	} else if (!is_gicv2 && gicr_cpu_base[cpu] != 0) {
		return;
	}

	if (is_gicv2) {
		/* GICv2 CPU interface configuration */
		gic_v2_ctlr_before = gic_read32(gicd_base + GICD_CTLR);
		gic_v2_gicc_before = gic_read32(gicc_base + GICC_CTLR);

		gic_write32(gicc_base + GICC_PMR, 0xff);
		gic_write32(gicc_base + GICC_BPR, 0);

		/* Virtual timer PPI 27 */
		gic_write32(gicd_base + GICD_ISENABLER(0), 1u << GIC_INTID_VTIMER);
		*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(GIC_INTID_VTIMER)) = GIC_PRIORITY_DEFAULT;

		/* SGI 8: the self-test the report sends itself (see GIC_SGI_PROBE) */
		gic_write32(gicd_base + GICD_ICACTIVER(0), 1u << GIC_SGI_PROBE);
		gic_write32(gicd_base + GICD_ICPENDR(0), 1u << GIC_SGI_PROBE);
		*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(GIC_SGI_PROBE)) = GIC_PRIORITY_DEFAULT;
		gic_write32(gicd_base + GICD_ISENABLER(0), 1u << GIC_SGI_PROBE);

		if (ml_get_cpu_count() > 1) {
			uint32_t sgis = (1u << GIC_SGI_SIGNAL) | (1u << GIC_SGI_STOP);
			gic_write32(gicd_base + GICD_ICACTIVER(0), sgis);
			gic_write32(gicd_base + GICD_ICPENDR(0), sgis);
			*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(GIC_SGI_SIGNAL)) = GIC_PRIORITY_DEFAULT;
			*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(GIC_SGI_STOP)) = GIC_PRIORITY_DEFAULT;
			gic_write32(gicd_base + GICD_ISENABLER(0), sgis);
		}

		/*
		 * Enable the Distributor and the CPU interface.
		 *
		 * Which bit does it depends on the security states: GICD_CTLR.DS = 0 here (what the
		 * report shows), so this is the non-secure view, where bit 0 is EnableGrp1NS and bit 1
		 * is reserved. Writing both is harmless in either view and covers a GIC in a single
		 * security state as well. The enable bits alone were not enough: the interrupts also
		 * had to be moved to Group 1 NS (below), or the CPU interface had nothing it was
		 * allowed to deliver.
		 */
		uint32_t gicd_ctlr = gic_read32(gicd_base + GICD_CTLR);
		bool ds = (gicd_ctlr & GICD_CTLR_DS) != 0;
		uint32_t ours = (1u << GIC_INTID_VTIMER) | (1u << GIC_SGI_PROBE) |
		    (1u << GIC_SGI_SIGNAL) | (1u << GIC_SGI_STOP);

		/*
		 * Group assignment, and the reason nothing reached this kernel before. MSM8952 comes
		 * out of ABL with two security states (GICD_CTLR.DS = 0) and with every SGI and PPI
		 * left in Group 0, which is the secure group: reads as IGROUPR0 = 0. This kernel runs
		 * non-secure, and from the non-secure view the same bits mean Group 1 NS, so a set
		 * bit is what puts an interrupt where we can take it. Without this the virtual timer
		 * was enabled, armed and pending, and the CPU interface had nothing it was allowed to
		 * deliver: no timeouts, no usleep(), while the boot went on being synchronous.
		 */
		gic_write32(gicd_base + GICD_IGROUPR(0),
		    gic_read32(gicd_base + GICD_IGROUPR(0)) | ours);
		gic_write32(gicd_base + GICD_CTLR, gicd_ctlr | GICD_CTLR_ENABLEGRP0 | GICD_CTLR_ENABLEGRP1);
		gic_write32(gicc_base + GICC_CTLR, GICC_CTLR_ENABLEGRP0 | GICC_CTLR_ENABLEGRP1);

		ml_install_interrupt_handler(NULL, 0, NULL, gic_irq_handler, NULL);
		if (!s_gic_apcs_wdt) {
			s_gic_apcs_wdt = ml_io_map(0x0b017000, 0x1000);
		}
		/* kprintf here: this runs before the console exists; the report prints the same values */
		kprintf("gic: v2, distributor 0x%lx (ctlr 0x%x, ds %d), cpu %d's interface 0x%lx, virtual timer on PPI %d\n",
		    (unsigned long)gicd_base, gic_read32(gicd_base + GICD_CTLR), ds ? 1 : 0,
		    cpu, (unsigned long)gicc_base, GIC_INTID_VTIMER);

		/* The self-test SGI, now pending: taken as soon as this processor has interrupts on. */
		gic_write32(gicd_base + GICD_SGIR, (2u << 24) | GIC_SGI_PROBE);
		return;
	}

	/* System-register CPU interface */
	__builtin_arm_wsr64("ICC_SRE_EL1", __builtin_arm_rsr64("ICC_SRE_EL1") | 1);
	__builtin_arm_isb(ISB_SY);

	gicr = gicr_cpu_base[cpu] = gic_find_redistributor();

	uint32_t waker = gic_read32(gicr + GICR_WAKER);
	if (waker & (GICR_WAKER_PROCSLEEP | GICR_WAKER_CHILDASLEEP)) {
		gic_write32(gicr + GICR_WAKER, waker & ~GICR_WAKER_PROCSLEEP);
		while (gic_read32(gicr + GICR_WAKER) & GICR_WAKER_CHILDASLEEP) {
			/* spin until the redistributor is awake */
		}
	}

	/*
	 * The interrupts between processors, where there is more than one. A core that an earlier
	 * kernel switched off in the middle of one (a return to the boot loader, started again
	 * since) may still have it marked active, and would then never see another.
	 */
	if (ml_get_cpu_count() > 1) {
		uint32_t sgis = (1u << GIC_SGI_SIGNAL) | (1u << GIC_SGI_STOP);

		gic_write32(gicr + GICR_ICACTIVER0, sgis);
		gic_write32(gicr + GICR_ICPENDR0, sgis);        /* ... or one that it never took */
		*(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_SGI_SIGNAL)) = GIC_PRIORITY_DEFAULT;
		*(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_SGI_STOP)) = GIC_PRIORITY_DEFAULT;
		mine |= sgis;
	}

	/* Virtual timer PPI (and those): Group 1 (IRQ), default priority, enabled */
	gic_write32(gicr + GICR_IGROUPR0, gic_read32(gicr + GICR_IGROUPR0) | mine);
	gic_write32(gicr + GICR_IGRPMODR0, gic_read32(gicr + GICR_IGRPMODR0) & ~mine);
	*(volatile uint8_t *)(gicr + GICR_IPRIORITYR(GIC_INTID_VTIMER)) = GIC_PRIORITY_DEFAULT;
	gic_write32(gicr + GICR_ISENABLER0, mine);

	/* Distributor: affinity routing, Group 1 enabled (normally already so) */
	uint32_t ctlr = gic_read32(gicd_base + GICD_CTLR);
	if ((ctlr & (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)) != (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)) {
		gic_write32(gicd_base + GICD_CTLR, ctlr | GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS);
		while (gic_read32(gicd_base + GICD_CTLR) & GICD_CTLR_RWP) {
			/* wait for the write to take effect */
		}
	}

	/*
	 * CPU interface: accept all priorities, no preemption grouping, Group 1 on. And no priority
	 * active: a core that the firmware switches off and on again need not lose that, and a
	 * kernel that left in the middle of an interrupt without ending it would leave this one
	 * waiting for an end that never comes.
	 */
	__builtin_arm_wsr64("ICC_AP1R0_EL1", 0);
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xff);
	__builtin_arm_wsr64("ICC_BPR1_EL1", 0);
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
	__builtin_arm_isb(ISB_SY);

	ml_install_interrupt_handler(NULL, 0, NULL, gic_irq_handler, NULL);
	kprintf("gic: v3, distributor %p, cpu %d's redistributor %p, virtual timer on PPI %d\n",
	    (void *)gicd_base, cpu, (void *)gicr, GIC_INTID_VTIMER);
}

/*
 * BRINGUP: what the interrupt path looks like from the kernel side, in one line each. Printed late
 * enough (the end of bsd_init) that the USB console is up, so it comes out in the run's log and not
 * only on the panel. If vtimer_ticks is 0 by then, the timer interrupt has never reached the
 * kernel: the kernel's timeouts and usleep() then never expire, which is exactly what a first
 * userland run showed at "sleep and wall clock agree". The SGI self-test tells the two halves
 * apart: an SGI sent by the processor to itself needs no timer at all, just delivery.
 */
static void
gic_sgi_probe_send(void)
{
	uint64_t start, before;

	if (gicd_base == 0) {
		return;
	}
	start = __builtin_arm_rsr64("CNTVCT_EL0");
	before = gic_sgi_probe_ticks;
	/* TargetListFilter 2 = this processor; the SGI's group decides which acknowledge path it takes */
	gic_write32(gicd_base + GICD_SGIR, (2u << 24) | GIC_SGI_PROBE);
	while (gic_sgi_probe_ticks == before &&
	    __builtin_arm_rsr64("CNTVCT_EL0") - start < 4000000) {
		/* let the interrupt be taken */
	}
}

/*
 * BRINGUP: which of our writes the controller takes. It has been dropping the ones to the
 * SGI/PPI bank (the group register reads back 0 and a freshly enabled SGI does not appear),
 * which leaves this kernel with no interrupt at all. These four answers say whether the whole
 * distributor is closed to us, whether only that bank is, and whether this is a single security
 * state or a secure/non-secure pair in which the firmware never gave the non-secure state any
 * private interrupt.
 */
static void
gic_v2_probe(void)
{
	uint32_t typer = gic_read32(gicd_base + GICD_TYPER);
	uint32_t iidr = gic_read32(gicc_base + GICC_IIDR);
	uint32_t spi_priority_before = *(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(32));
	uint32_t spi_priority_after;
	uint32_t group0, isen15, ctlr, ctlr_ds;

	/* A global interrupt's priority: harmless, and not in the banked block. */
	*(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(32)) = 0xb8;
	__builtin_arm_dsb(DSB_SY);
	spi_priority_after = *(volatile uint8_t *)(gicd_base + GICD_IPRIORITYR(32));

	/* The banked one, where the timer lives. */
	gic_write32(gicd_base + GICD_IGROUPR(0), 0xffffffffu);
	__builtin_arm_dsb(DSB_SY);
	group0 = gic_read32(gicd_base + GICD_IGROUPR(0));
	gic_write32(gicd_base + GICD_ISENABLER(0), 1u << 15);
	__builtin_arm_dsb(DSB_SY);
	isen15 = (gic_read32(gicd_base + GICD_ISENABLER(0)) >> 15) & 1;

	ctlr = gic_read32(gicd_base + GICD_CTLR);
	gic_write32(gicd_base + GICD_CTLR, ctlr | GICD_CTLR_DS);   /* only the secure state may */
	__builtin_arm_dsb(DSB_SY);
	ctlr_ds = gic_read32(gicd_base + GICD_CTLR);

	printf("[GIC] probe: TYPER=0x%x (lines=%u, cpus=%u, sec-ext=%u) IIDR=0x%x\n",
	    typer, (typer & 0x1fu) + 1, (typer >> 5) & 0x7u, (typer >> 10) & 1u, iidr);
	printf("[GIC] probe: SPI32 prio %u->%u (%s); IGROUPR0=0xffffffff reads 0x%x; ISEN0 bit15 %s; CTLR 0x%x, +DS reads 0x%x (%s)\n",
	    spi_priority_before, spi_priority_after,
	    spi_priority_before != spi_priority_after ? "written" : "dropped",
	    group0, isen15 ? "written" : "dropped", ctlr, ctlr_ds,
	    (ctlr_ds & GICD_CTLR_DS) ? "single security state" : "two states, and DS is not ours to set");
}

void
gic_v3_irq_report(void)
{
	uint64_t daif = __builtin_arm_rsr64("DAIF");
	uint64_t el = __builtin_arm_rsr64("CurrentEL") >> 2;
	uint64_t cntv_ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");
	uint64_t cntv_tval = __builtin_arm_rsr64("CNTV_TVAL_EL0");
	uint64_t cntkctl = __builtin_arm_rsr64("CNTKCTL_EL1");

	if (is_gicv2) {
		printf("[GIC] v2 gicd=0x%lx gicc=0x%lx CTLR:0x%x was 0x%x IGROUPR0=0x%x ISEN0=0x%x GICC_CTLR:0x%x was 0x%x PMR=0x%x BPR=0x%x HPPIR=0x%x AHPPIR=0x%x\n",
		    (unsigned long)gicd_base, (unsigned long)gicc_base,
		    gic_read32(gicd_base + GICD_CTLR), gic_v2_ctlr_before,
		    gic_read32(gicd_base + GICD_IGROUPR(0)), gic_read32(gicd_base + GICD_ISENABLER(0)),
		    gic_read32(gicc_base + GICC_CTLR), gic_v2_gicc_before,
		    gic_read32(gicc_base + GICC_PMR), gic_read32(gicc_base + GICC_BPR),
		    gic_read32(gicc_base + GICC_HPPIR), gic_read32(gicc_base + GICC_AHPPIR));
	} else {
		printf("[GIC] v3 gicd=0x%lx gicr=0x%lx CTLR=0x%x\n",
		    (unsigned long)gicd_base, (unsigned long)gicr_base,
		    gic_read32(gicd_base + GICD_CTLR));
	}
	printf("[TMR] CNTV_CTL=0x%lx CNTV_TVAL=0x%lx CNTKCTL=0x%lx DAIF=0x%lx EL%lu decrementer=0x%llx ticks=%lu unhandled=%lu/%u\n",
	    (unsigned long)cntv_ctl, (unsigned long)cntv_tval, (unsigned long)cntkctl,
	    (unsigned long)daif, (unsigned long)el,
	    (unsigned long long)getCpuDatap()->cpu_decrementer,
	    (unsigned long)gic_vtimer_ticks, (unsigned long)gic_unhandled_count, gic_last_unhandled);

	if (is_gicv2) {
		uint64_t sgi_before;

		gic_v2_probe();

		sgi_before = gic_sgi_probe_ticks;
		gic_sgi_probe_send();
		printf("[GIC] sgi %d self-test: taken=%lu (of %lu since boot); irq is %s\n",
		    GIC_SGI_PROBE, (unsigned long)(gic_sgi_probe_ticks - sgi_before),
		    (unsigned long)gic_sgi_probe_ticks,
		    gic_sgi_probe_ticks != sgi_before ? "delivered" :
		    (daif & 0x80u ? "masked (DAIF.I)" : "not delivered"));
	}
}

#endif /* GENERIC_GIC_V3_IRQ */
