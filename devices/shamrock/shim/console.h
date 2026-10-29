#pragma once
#include <stdint.h>

#define FB_BASE		0x83200000UL	/* GM 5 Plus cont_splash_mem */
#define FB_W		1080
#define FB_H		1920
#define FB_STRIDE	(FB_W * 3)	/* 1080x1920 24bpp r8g8b8 (3240 bytes) */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint8_t *con_fb;

void con_clear(uint32_t bg);
void con_clear_top(uint32_t bg, int max_y);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
