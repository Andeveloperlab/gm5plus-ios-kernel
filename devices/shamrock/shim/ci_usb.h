#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ci_hw.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/*
 * Endpoint Queue Head (dQH) - 64 bytes
 * 16 entries = 1024 bytes total (2048-byte aligned)
 */
struct __attribute__((packed)) ci_dqh {
	uint32_t cap;
	uint32_t curr_dtd;
	uint32_t next_dtd;
	uint32_t token;
	uint32_t page[5];
	uint32_t reserved;
	uint8_t  setup_buffer[8];
	uint32_t reserved2[4];
};
_Static_assert(sizeof(struct ci_dqh) == 64, "ci_dqh size must be 64 bytes");

/*
 * Device Transfer Descriptor (dTD) - 32 bytes
 */
struct __attribute__((packed)) ci_dtd {
	uint32_t next_dtd;
	uint32_t token;
	uint32_t page[5];
	uint32_t reserved;
};
_Static_assert(sizeof(struct ci_dtd) == 32, "ci_dtd size must be 32 bytes");

struct __attribute__((packed)) usb_setup_req {
	uint8_t  bmRequestType;
	uint8_t  bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
};
_Static_assert(sizeof(struct usb_setup_req) == 8, "usb_setup_req must be 8 bytes");

enum ci_op_reg {
	OP_USBMODE,
	OP_ENDPTSETUPSTAT,
	OP_ENDPTPRIME,
	OP_ENDPTFLUSH,
	OP_ENDPTSTAT,
	OP_ENDPTCOMPLETE,
};

/* Public API */
int  ci_check_preconditions(uintptr_t base, uintptr_t phy_base);
int  ci_usb_init(void);
void ci_usb_poll(void);
void ci_log_write(const void *data, uint32_t len);
void ci_log_puts(const char *s);
uint32_t ci_rx_read(void *dst, uint32_t max_len);
uint32_t ci_rx_available(void);

