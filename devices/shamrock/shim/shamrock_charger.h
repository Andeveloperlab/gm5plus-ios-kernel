#pragma once
#include <stdint.h>
#include <stddef.h>

void shamrock_charger_init(void);
void shamrock_charger_keepalive(void);
int  shamrock_charger_status_str(char *buf, size_t maxlen);
int  shamrock_charger_is_present(void);
void shamrock_set_fastboot_reboot_reason(void);
