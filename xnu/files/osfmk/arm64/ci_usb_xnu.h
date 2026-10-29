#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void     ci_usb_xnu_init(void);
void     ci_usb_xnu_putc(char c);
void     ci_usb_xnu_putc_raw(char c);
void     ci_usb_xnu_puts(const char *s);
void     ci_usb_xnu_poll(void);
int      ci_usb_xnu_getc(bool wait);
uint32_t ci_usb_xnu_rx_available(void);
uint32_t ci_usb_xnu_rx_read(void *dst, uint32_t max_len);

#ifdef __cplusplus
}
#endif
