#ifndef LDR_H
#define LDR_H

#include <stdbool.h>
#include <stdint.h>

void ldr_init(void);
uint16_t read_ldr(void);

void ldr_update(void);
uint16_t ldr_get_raw(void);
bool ldr_is_dark(void);

#endif
