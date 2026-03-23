#ifndef TIMING_H
#define TIMING_H

#include "MK66F18.h"
#include <stdint.h>

extern volatile uint32_t msTicks;

void timing_init(void);
uint32_t millis(void);
void delay_us(uint32_t us);
uint32_t timing_get_core_mhz(void);

#endif
