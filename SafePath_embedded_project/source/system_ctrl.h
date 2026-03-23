#ifndef SYSTEM_CTRL_H
#define SYSTEM_CTRL_H

#include <stdint.h>

void sw3_init(void);
uint8_t sw3_is_pressed(void);

void sw2_init(void);
uint8_t sw2_is_pressed(void);

#endif
