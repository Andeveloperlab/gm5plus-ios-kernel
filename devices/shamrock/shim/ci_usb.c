#include "ci_usb.h"
#include "console.h"

#define LOG_RING_SIZE   4096
#define RX_RING_SIZE    2048

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
static inline uint64_t get_ticks(void)
{
	__asm__ volatile("isb" ::: "memory");
	return rsr("cntpct_el0");
}

/* DMA structures: dQH table MUST be 2048-byte aligned */
static struct ci_dqh dqh[16] __attribute__((aligned(2048)));

static struct ci_dtd dtd_ep0_out __attribute__((aligned(64)));
static struct ci_dtd dtd_ep0_in  __attribute__((aligned(64)));
static struct ci_dtd dtd_ep1_out __attribute__((aligned(64)));
static struct ci_dtd dtd_ep1_in  __attribute__((aligned(64)));
static struct ci_dtd dtd_ep2_in  __attribute__((aligned(64)));

static uint8_t ep0_rx_buf[128] __attribute__((aligned(64)));
static uint8_t ep0_tx_buf[512] __attribute__((aligned(64)));
static uint8_t ep1_rx_buf[512] __attribute__((aligned(64)));
static uint8_t ep1_tx_buf[512] __attribute__((aligned(64)));
static uint8_t ep2_tx_buf[64]  __attribute__((aligned(64)));

/* Software Ring Buffers */
static uint8_t  log_ring[LOG_RING_SIZE];
static volatile uint32_t log_head, log_tail, log_dropped;

static uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head, rx_tail;

/* Controller State */
static uintptr_t base = MSM8952_USB_BASE;
static uintptr_t cap_base = MSM8952_USB_BASE + 0x0140;
static uint32_t  lpm_active;
static uint32_t  bulk_max_pkt = 64;
static uint8_t   curr_config = 0;
static bool      configured = false;
static bool      ep1_rx_busy = false;
static bool      ep1_tx_busy = false;

/* CDC Line Coding state (115200 8N1) */
static uint8_t line_coding[7] = {
	0x00, 0xC2, 0x01, 0x00, /* 115200 baud */
	0x00,                   /* 1 stop bit */
	0x00,                   /* no parity */
	0x08                    /* 8 data bits */
};

/* Register access helpers */
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uintptr_t op_reg(enum ci_op_reg reg_name)
{
	switch (reg_name) {
	case OP_USBMODE:
		return cap_base + (lpm_active ? CI_LPM_USBMODE : CI_STD_USBMODE);
	case OP_ENDPTSETUPSTAT:
		return cap_base + (lpm_active ? CI_LPM_ENDPTSETUPSTAT : CI_STD_ENDPTSETUPSTAT);
	case OP_ENDPTPRIME:
		return cap_base + (lpm_active ? CI_LPM_ENDPTPRIME : CI_STD_ENDPTPRIME);
	case OP_ENDPTFLUSH:
		return cap_base + (lpm_active ? CI_LPM_ENDPTFLUSH : CI_STD_ENDPTFLUSH);
	case OP_ENDPTSTAT:
		return cap_base + (lpm_active ? CI_LPM_ENDPTSTAT : CI_STD_ENDPTSTAT);
	case OP_ENDPTCOMPLETE:
		return cap_base + (lpm_active ? CI_LPM_ENDPTCOMPLETE : CI_STD_ENDPTCOMPLETE);
	default:
		return 0;
	}
}

static uintptr_t ctrl_reg(int ep)
{
	return cap_base + (lpm_active ? CI_LPM_ENDPTCTRL(ep) : CI_STD_ENDPTCTRL(ep));
}

/* Cortex-A53 Cache Maintenance */
static void cache_clean(const void *p, uint32_t len)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)63;
	uintptr_t end = (uintptr_t)p + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void cache_inval(const void *p, uint32_t len)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)63;
	uintptr_t end = (uintptr_t)p + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ULPI Viewport Helpers */
static int ulpi_read(uint8_t addr, uint8_t *val)
{
	uintptr_t vp = cap_base + CI_ULPI_VIEWPORT;
	wr32(vp, ULPI_WRITE | ULPI_WAKEUP);
	uint64_t start = get_ticks();
	while (rd32(vp) & ULPI_WAKEUP) {
		if (get_ticks() - start > 192000) /* 10 ms timeout at 19.2MHz */
			break;
	}
	wr32(vp, ULPI_RUN | ULPI_ADDR(addr));
	start = get_ticks();
	while (rd32(vp) & ULPI_RUN) {
		if (get_ticks() - start > 192000) /* 10 ms timeout at 19.2MHz */
			return -1;
	}
	*val = (uint8_t)ULPI_DATA_RD(rd32(vp));
	return 0;
}

static int ulpi_write(uint8_t addr, uint8_t val)
{
	uintptr_t vp = cap_base + CI_ULPI_VIEWPORT;
	wr32(vp, ULPI_WRITE | ULPI_WAKEUP);
	uint64_t start = get_ticks();
	while (rd32(vp) & ULPI_WAKEUP) {
		if (get_ticks() - start > 192000) /* 10 ms timeout at 19.2MHz */
			break;
	}
	wr32(vp, ULPI_RUN | ULPI_WRITE | ULPI_ADDR(addr) | ULPI_DATA_WR(val));
	start = get_ticks();
	while (rd32(vp) & ULPI_RUN) {
		if (get_ticks() - start > 192000) /* 10 ms timeout at 19.2MHz */
			return -1;
	}
	return 0;
}

/* Precondition Verification */
int ci_check_preconditions(uintptr_t b, uintptr_t phy_b)
{
	uint32_t id = rd32(b + CI_ID);
	uint32_t cid = id & 0x3F;
	uint32_t nid = (id >> 8) & 0x3F;
	if ((cid ^ nid) != 0x3F)
		return -1;

	uint32_t caplen = rd32(b + CI_CAPLENGTH) & 0xFF;
	if (caplen != 0x40)
		return -1;

	uint32_t dcc = rd32(b + CI_DCCPARAMS) & 0x1F;
	if (dcc < 2)
		return -1;

	uint32_t phy_common = rd32(phy_b + USB_PHY_CSR_PHY_CTRL_COMMON0);
	if (phy_common & PHY_CTRL_SIDDQ)
		return -2;

	uint32_t portsc = rd32(b + 0x0140 + CI_PORTSC);
	if (!(portsc & PORTSC_PTS_ULPI))
		return -2;

	uint8_t dummy;
	if (ulpi_read(0x00, &dummy) != 0)
		return -3;

	return 0;
}

/* Standard Descriptors */
static const uint8_t dev_desc[] = {
	18,         /* bLength */
	0x01,       /* bDescriptorType = Device */
	0x00, 0x02, /* bcdUSB = 2.00 */
	0x02,       /* bDeviceClass = CDC */
	0x00,       /* bDeviceSubClass */
	0x00,       /* bDeviceProtocol */
	64,         /* bMaxPacketSize0 = 64 */
	0x09, 0x12, /* idVendor = 0x1209 */
	0x01, 0x00, /* idProduct = 0x0001 */
	0x00, 0x01, /* bcdDevice = 1.00 */
	1,          /* iManufacturer */
	2,          /* iProduct */
	3,          /* iSerialNumber */
	1           /* bNumConfigurations = 1 */
};

static const uint8_t cfg_desc[] = {
	/* Configuration Descriptor: 67 bytes, Bus-Powered (0x80), bMaxPower = 250 (500 mA) */
	9, 0x02, 67, 0, 2, 1, 0, 0x80, 0xFA,
	/* Interface 0: CDC Communication */
	9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
	/* CDC Header Functional Descriptor */
	5, 0x24, 0x00, 0x10, 0x01,
	/* CDC Call Management Functional Descriptor */
	5, 0x24, 0x01, 0x00, 0x01,
	/* CDC ACM Functional Descriptor */
	4, 0x24, 0x02, 0x02,
	/* CDC Union Functional Descriptor */
	5, 0x24, 0x06, 0x00, 0x01,
	/* EP2 IN (Interrupt) */
	7, 0x05, 0x82, 0x03, 10, 0, 32,
	/* Interface 1: CDC Data */
	9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
	/* EP1 OUT (Bulk) */
	7, 0x05, 0x01, 0x02, 0x00, 0x02, 0,
	/* EP1 IN (Bulk) */
	7, 0x05, 0x81, 0x02, 0x00, 0x02, 0
};

static const uint8_t str0_desc[] = {
	4, 0x03, 0x09, 0x04
};

static const uint8_t str1_desc[] = {
	30, 0x03,
	'G', 0, 'e', 0, 'n', 0, 'e', 0, 'r', 0, 'a', 0, 'l', 0, ' ', 0,
	'M', 0, 'o', 0, 'b', 0, 'i', 0, 'l', 0, 'e', 0
};

static const uint8_t str2_desc[] = {
	50, 0x03,
	'G', 0, 'M', 0, ' ', 0, '5', 0, ' ', 0, 'P', 0, 'l', 0, 'u', 0,
	's', 0, ' ', 0, 'S', 0, 'h', 0, 'i', 0, 'm', 0, ' ', 0, 'U', 0,
	'S', 0, 'B', 0, ' ', 0, 'S', 0, 'e', 0, 'r', 0, 'i', 0, 'a', 0,
	'l', 0
};

static const uint8_t str3_desc[] = {
	24, 0x03,
	'S', 0, 'H', 0, 'A', 0, 'M', 0, 'R', 0, 'O', 0, 'C', 0, 'K', 0,
	'0', 0, '0', 0, '1', 0
};

static const uint8_t dev_qual[] = {
	10, 0x06, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 1, 0
};

/* dTD and Queue Head Helpers */
static void build_dtd(struct ci_dtd *d, const void *buf, uint32_t len, int ioc)
{
	d->next_dtd = TD_TERMINATE;
	d->token = (len << 16) | (ioc ? TD_IOC : 0) | TD_STATUS_ACTIVE;
	d->page[0] = (uint32_t)(uintptr_t)buf;
	d->page[1] = (uint32_t)(((uintptr_t)buf + 0x1000) & ~0xFFFUL);
	d->page[2] = 0;
	d->page[3] = 0;
	d->page[4] = 0;
}

static void prime_ep(int ep, int dir, struct ci_dtd *d)
{
	int idx = ep * 2 + dir;
	dqh[idx].next_dtd = (uint32_t)(uintptr_t)d;
	dqh[idx].token &= ~(TD_STATUS_ACTIVE | TD_STATUS_HALTED);
	cache_clean(&dqh[idx], sizeof(struct ci_dqh));

	uint32_t mask = 1U << (ep + (dir ? 16 : 0));
	wr32(op_reg(OP_ENDPTPRIME), mask);
}

/* Control Transfers */
static void ep0_send_zlp(void)
{
	build_dtd(&dtd_ep0_in, ep0_tx_buf, 0, 1);
	cache_clean(&dtd_ep0_in, sizeof(dtd_ep0_in));
	prime_ep(0, 1, &dtd_ep0_in);
}

static void ep0_recv_zlp(void)
{
	build_dtd(&dtd_ep0_out, ep0_rx_buf, 0, 1);
	cache_clean(&dtd_ep0_out, sizeof(dtd_ep0_out));
	prime_ep(0, 0, &dtd_ep0_out);
}

static void ep0_send_data(const void *data, uint32_t len)
{
	if (len > sizeof(ep0_tx_buf)) len = sizeof(ep0_tx_buf);
	for (uint32_t i = 0; i < len; i++)
		ep0_tx_buf[i] = ((const uint8_t *)data)[i];
	cache_clean(ep0_tx_buf, len);
	build_dtd(&dtd_ep0_in, ep0_tx_buf, len, 1);
	cache_clean(&dtd_ep0_in, sizeof(dtd_ep0_in));
	prime_ep(0, 1, &dtd_ep0_in);
	ep0_recv_zlp();
}

static void ep2_send_serial_state(uint16_t state)
{
	ep2_tx_buf[0] = 0xA1; /* bmRequestType */
	ep2_tx_buf[1] = 0x20; /* SERIAL_STATE */
	ep2_tx_buf[2] = 0x00; ep2_tx_buf[3] = 0x00;
	ep2_tx_buf[4] = 0x00; ep2_tx_buf[5] = 0x00;
	ep2_tx_buf[6] = 0x02; ep2_tx_buf[7] = 0x00;
	ep2_tx_buf[8] = (uint8_t)(state & 0xFF);
	ep2_tx_buf[9] = (uint8_t)((state >> 8) & 0xFF);
	cache_clean(ep2_tx_buf, 10);
	build_dtd(&dtd_ep2_in, ep2_tx_buf, 10, 1);
	cache_clean(&dtd_ep2_in, sizeof(dtd_ep2_in));
	prime_ep(2, 1, &dtd_ep2_in);
}

static void ci_rx_prime(void)
{
	if (ep1_rx_busy || !configured)
		return;
	build_dtd(&dtd_ep1_out, ep1_rx_buf, bulk_max_pkt, 1);
	cache_inval(ep1_rx_buf, bulk_max_pkt);
	cache_clean(&dtd_ep1_out, sizeof(dtd_ep1_out));
	ep1_rx_busy = true;
	prime_ep(1, 0, &dtd_ep1_out);
}

static void ci_log_pump(void)
{
	if (ep1_tx_busy || !configured)
		return;

	uint32_t head = log_head;
	uint32_t tail = log_tail;
	if (head == tail)
		return;

	uint32_t avail = (head >= tail) ? (head - tail) : (LOG_RING_SIZE - tail);
	if (avail > bulk_max_pkt)
		avail = bulk_max_pkt;

	for (uint32_t i = 0; i < avail; i++) {
		ep1_tx_buf[i] = log_ring[(tail + i) % LOG_RING_SIZE];
	}
	cache_clean(ep1_tx_buf, avail);

	build_dtd(&dtd_ep1_in, ep1_tx_buf, avail, 1);
	cache_clean(&dtd_ep1_in, sizeof(dtd_ep1_in));

	log_tail = (tail + avail) % LOG_RING_SIZE;
	ep1_tx_busy = true;
	prime_ep(1, 1, &dtd_ep1_in);
}

/* EP0 Setup Request Handler */
static void handle_setup(const struct usb_setup_req *req)
{
	uint8_t type = req->bmRequestType & 0x60;

	if (type == 0x00) {
		/* Standard Request */
		switch (req->bRequest) {
		case 0x00: /* GET_STATUS */
		{
			uint16_t status = 0;
			ep0_send_data(&status, sizeof(status));
			break;
		}
		case 0x05: /* SET_ADDRESS */
		{
			uint8_t new_addr = (uint8_t)(req->wValue & 0x7F);
			ep0_send_zlp();
			wr32(cap_base + CI_DEVICEADDR, DEVICEADDR_USBADRA | DEVICEADDR_USBADR(new_addr));
			break;
		}
		case 0x06: /* GET_DESCRIPTOR */
		{
			uint8_t desc_type = req->wValue >> 8;
			uint8_t desc_idx  = req->wValue & 0xFF;
			const uint8_t *p = NULL;
			uint32_t len = 0;

			if (desc_type == 1) {
				p = dev_desc;
				len = sizeof(dev_desc);
			} else if (desc_type == 2) {
				p = cfg_desc;
				len = sizeof(cfg_desc);
			} else if (desc_type == 6) {
				p = dev_qual;
				len = sizeof(dev_qual);
			} else if (desc_type == 3) {
				if (desc_idx == 0) { p = str0_desc; len = sizeof(str0_desc); }
				else if (desc_idx == 1) { p = str1_desc; len = sizeof(str1_desc); }
				else if (desc_idx == 2) { p = str2_desc; len = sizeof(str2_desc); }
				else if (desc_idx == 3) { p = str3_desc; len = sizeof(str3_desc); }
			}

			if (p && len) {
				if (len > req->wLength) len = req->wLength;
				ep0_send_data(p, len);
			} else {
				/* Stall EP0 */
				wr32(ctrl_reg(0), rd32(ctrl_reg(0)) | ENDPTCTRL_RXS | ENDPTCTRL_TXS);
			}
			break;
		}
		case 0x08: /* GET_CONFIGURATION */
		{
			ep0_send_data(&curr_config, 1);
			break;
		}
		case 0x09: /* SET_CONFIGURATION */
		{
			curr_config = (uint8_t)(req->wValue & 0xFF);
			ep0_send_zlp();
			if (curr_config != 0) {
				uint32_t portsc = rd32(cap_base + CI_PORTSC);
				bulk_max_pkt = (portsc & PORTSC_HSP) ? 512 : 64;

				dqh[2].cap = QH_MAX_PKT(bulk_max_pkt) | QH_ZLT;
				dqh[2].next_dtd = TD_TERMINATE;
				dqh[3].cap = QH_MAX_PKT(bulk_max_pkt) | QH_ZLT;
				dqh[3].next_dtd = TD_TERMINATE;
				dqh[5].cap = QH_MAX_PKT(10) | QH_ZLT;
				dqh[5].next_dtd = TD_TERMINATE;
				cache_clean(&dqh[2], sizeof(struct ci_dqh) * 4);

				wr32(ctrl_reg(1), ENDPTCTRL_RXE | ENDPTCTRL_RXR | ENDPTCTRL_RXT_BULK |
				                  ENDPTCTRL_TXE | ENDPTCTRL_TXR | ENDPTCTRL_TXT_BULK);
				wr32(ctrl_reg(2), ENDPTCTRL_TXE | ENDPTCTRL_TXR | ENDPTCTRL_TXT_INT);

				configured = true;
				con_str("USB", "Host connected! CDC-ACM ready.");
				ci_rx_prime();
				ci_log_pump();
			}
			break;
		}
		default:
			ep0_send_zlp();
			break;
		}
	} else if (type == 0x20) {
		/* CDC Class Request */
		switch (req->bRequest) {
		case 0x20: /* SET_LINE_CODING */
		{
			build_dtd(&dtd_ep0_out, ep0_rx_buf, 7, 1);
			cache_inval(ep0_rx_buf, 7);
			cache_clean(&dtd_ep0_out, sizeof(dtd_ep0_out));
			prime_ep(0, 0, &dtd_ep0_out);
			ep0_send_zlp();
			break;
		}
		case 0x21: /* GET_LINE_CODING */
		{
			ep0_send_data(line_coding, sizeof(line_coding));
			break;
		}
		case 0x22: /* SET_CONTROL_LINE_STATE */
		{
			ep0_send_zlp();
			if (req->wValue & 1) {
				ep2_send_serial_state(0x03); /* DCD | DSR active */
			}
			break;
		}
		default:
			ep0_send_zlp();
			break;
		}
	} else {
		ep0_send_zlp();
	}
}

/* Initialization */
int ci_usb_init(void)
{
	lpm_active = (rd32(base + CI_HCCPARAMS) >> 17) & 1;

	/* 1. Device Prepare: Preserve aboot's ULPI session state! Do not issue destructive USBCMD_RST */
	wr32(base + CI_ABS_AHBMODE, 0x08); /* AHB transactor mode, posted data writes */

	/* Qualcomm MSM ChipIdea hardware workarounds and session valid control */
	wr32(base + CI_MSM_GENCONFIG, rd32(base + CI_MSM_GENCONFIG) & ~GENCONFIG_TXFIFO_IDLE_DIS);
	uint32_t gen2 = rd32(base + CI_MSM_GENCONFIG_2);
	gen2 &= ~GENCONFIG_2_TX_CLR_FIX;
	gen2 |= GENCONFIG_2_SESS_VLD_EN;
	wr32(base + CI_MSM_GENCONFIG_2, gen2);

	wr32(cap_base + CI_USBCMD, rd32(cap_base + CI_USBCMD) & ~USBCMD_RS);    /* Drop D+ briefly */

	for (volatile int d = 0; d < 200000; d++)
		__asm__ volatile("nop");

	wr32(op_reg(OP_ENDPTFLUSH), 0xFFFFFFFF);
	while (rd32(op_reg(OP_ENDPTFLUSH)))
		;

	/* Clear pending status & completions & OTG status bits */
	wr32(cap_base + CI_USBSTS, 0xFFFFFFFF);
	wr32(op_reg(OP_ENDPTCOMPLETE), 0xFFFFFFFF);
	wr32(cap_base + 0x0064, rd32(cap_base + 0x0064) | (0x3F << 16));

	/* Apply Femto PHY tuning sequence */
	ulpi_write(0x80, 0x73);
	ulpi_write(0x81, 0x68);
	ulpi_write(0x82, 0x24);

	/* Disable internal comparator and FORCE external VBUS valid on Femto PHY */
	ulpi_write(0x88, 0x01); /* Direct write */
	ulpi_write(0x89, 0x01); /* ULPI_SET(0x88) OTG_COMP_DISABLE */
	ulpi_write(0x96, 0x03); /* Direct write */
	ulpi_write(0x97, 0x03); /* ULPI_SET(0x96) VBUSVLDEXTSEL | VBUSVLDEXT */

	/* Device mode, Setup Lockout off */
	wr32(op_reg(OP_USBMODE), USBMODE_CM_DEVICE | USBMODE_SLOM);

	/* Zero interrupt threshold for immediate STS updates */
	wr32(cap_base + CI_USBCMD, USBCMD_ITC(0));

	/* Setup Queue Heads */
	for (int i = 0; i < 16; i++) {
		dqh[i].cap = 0;
		dqh[i].curr_dtd = 0;
		dqh[i].next_dtd = TD_TERMINATE;
		dqh[i].token = 0;
	}
	cache_clean(dqh, sizeof(dqh));

	wr32(cap_base + CI_ENDPTLISTADDR, (uint32_t)(uintptr_t)dqh);
	wr32(cap_base + CI_USBSTS, 0xFFFFFFFF);
	wr32(cap_base + CI_USBINTR, USBINTR_UIE | USBINTR_UEIE | USBINTR_PCIE | USBINTR_URE | USBINTR_SLE);

	/* 2. EP0 Start */
	dqh[0].cap = QH_MAX_PKT(64) | QH_ZLT | QH_IOS;
	dqh[0].next_dtd = TD_TERMINATE;
	dqh[1].cap = QH_MAX_PKT(64) | QH_ZLT;
	dqh[1].next_dtd = TD_TERMINATE;
	cache_clean(&dqh[0], sizeof(struct ci_dqh) * 2);

	wr32(ctrl_reg(0), ENDPTCTRL_RXE | ENDPTCTRL_TXE);

	/* 3. Device Run: Assert USBCMD.RS and USBCMD_SESS_VLD_CTRL to pull up D+ */
	wr32(cap_base + CI_USBCMD, rd32(cap_base + CI_USBCMD) | USBCMD_RS | USBCMD_SESS_VLD_CTRL);

	return 0;
}

/* Polling Loop */
void ci_usb_poll(void)
{
	uint32_t sts = rd32(cap_base + CI_USBSTS);
	if (sts)
		wr32(cap_base + CI_USBSTS, sts);

	/* Bus Reset */
	if (sts & USBSTS_URI) {
		wr32(cap_base + CI_DEVICEADDR, 0);
		wr32(op_reg(OP_ENDPTFLUSH), 0xFFFFFFFF);
		wr32(op_reg(OP_ENDPTCOMPLETE), 0xFFFFFFFF);
		configured = false;
		ep1_rx_busy = false;
		ep1_tx_busy = false;
		curr_config = 0;

		dqh[0].cap = QH_MAX_PKT(64) | QH_ZLT | QH_IOS;
		dqh[0].next_dtd = TD_TERMINATE;
		dqh[1].cap = QH_MAX_PKT(64) | QH_ZLT;
		dqh[1].next_dtd = TD_TERMINATE;
		cache_clean(&dqh[0], sizeof(struct ci_dqh) * 2);
	}

	/* Port Change */
	if (sts & USBSTS_PCI) {
		uint32_t portsc = rd32(cap_base + CI_PORTSC);
		bulk_max_pkt = (portsc & PORTSC_HSP) ? 512 : 64;
	}

	/* Setup Packet on EP0 */
	uint32_t setupstat = rd32(op_reg(OP_ENDPTSETUPSTAT));
	if (setupstat & 1) {
		wr32(op_reg(OP_ENDPTSETUPSTAT), 1);

		struct usb_setup_req req;
		do {
			wr32(cap_base + CI_USBCMD, rd32(cap_base + CI_USBCMD) | USBCMD_SUTW);
			cache_inval(&dqh[0], sizeof(struct ci_dqh));
			for (int i = 0; i < 8; i++)
				((uint8_t *)&req)[i] = dqh[0].setup_buffer[i];
		} while (!(rd32(cap_base + CI_USBCMD) & USBCMD_SUTW));

		wr32(cap_base + CI_USBCMD, rd32(cap_base + CI_USBCMD) & ~USBCMD_SUTW);

		wr32(op_reg(OP_ENDPTFLUSH), (1U << 0) | (1U << 16));
		while (rd32(op_reg(OP_ENDPTFLUSH)) & ((1U << 0) | (1U << 16)))
			;
		wr32(op_reg(OP_ENDPTCOMPLETE), (1U << 0) | (1U << 16));

		handle_setup(&req);
	}

	/* Transfer Complete */
	uint32_t complete = rd32(op_reg(OP_ENDPTCOMPLETE));
	if (complete) {
		wr32(op_reg(OP_ENDPTCOMPLETE), complete);

		/* EP1 OUT (Bulk RX) complete */
		if (complete & (1U << 1)) {
			ep1_rx_busy = false;
			cache_inval(&dtd_ep1_out, sizeof(dtd_ep1_out));
			cache_inval(ep1_rx_buf, bulk_max_pkt);

			uint32_t remaining = (dtd_ep1_out.token >> 16) & 0x7FFF;
			uint32_t received = bulk_max_pkt > remaining ? (bulk_max_pkt - remaining) : 0;

			for (uint32_t i = 0; i < received; i++) {
				uint32_t next = (rx_head + 1) % RX_RING_SIZE;
				if (next != rx_tail) {
					rx_ring[rx_head] = ep1_rx_buf[i];
					rx_head = next;
				}
			}
			ci_rx_prime();
		}

		/* EP1 IN (Bulk TX / Logging) complete */
		if (complete & (1U << 17)) {
			ep1_tx_busy = false;
			cache_inval(&dtd_ep1_in, sizeof(dtd_ep1_in));
			ci_log_pump();
		}

		/* EP2 IN (Interrupt) complete */
		if (complete & (1U << 18)) {
			cache_inval(&dtd_ep2_in, sizeof(dtd_ep2_in));
		}
	}

	ci_log_pump();
	ci_rx_prime();
}

/* Logging API */
void ci_log_write(const void *data, uint32_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	for (uint32_t i = 0; i < len; i++) {
		for (;;) {
			uint32_t next = (log_head + 1) % LOG_RING_SIZE;
			if (next != log_tail) {
				log_ring[log_head] = p[i];
				log_head = next;
				break;
			}
			/* Ring full: pump USB until space frees up (max ~5ms timeout) */
			uint64_t t0 = get_ticks();
			while (((log_head + 1) % LOG_RING_SIZE) == log_tail &&
			       (get_ticks() - t0 < 96000ULL)) {
				ci_usb_poll();
			}
			if (((log_head + 1) % LOG_RING_SIZE) == log_tail) {
				log_dropped++;
				break;
			}
		}
	}
	ci_log_pump();
}

void ci_log_puts(const char *s)
{
	uint32_t len = 0;
	while (s[len]) len++;
	ci_log_write(s, len);
}

/* RX Command API */
uint32_t ci_rx_available(void)
{
	uint32_t head = rx_head;
	uint32_t tail = rx_tail;
	return (head >= tail) ? (head - tail) : (RX_RING_SIZE + head - tail);
}

uint32_t ci_rx_read(void *dst, uint32_t max_len)
{
	uint8_t *d = (uint8_t *)dst;
	uint32_t count = 0;
	while (count < max_len && rx_head != rx_tail) {
		d[count++] = rx_ring[rx_tail];
		rx_tail = (rx_tail + 1) % RX_RING_SIZE;
	}
	return count;
}
