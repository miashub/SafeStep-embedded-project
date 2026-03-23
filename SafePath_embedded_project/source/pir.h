#ifndef PIR_H
#define PIR_H

#include <stdint.h>
#include <stdbool.h>

void pir_init(void);
uint8_t read_pir(void);
void pir_update(uint32_t now_ms);
bool pir_unusual_motion_event(void);
bool pir_light_hold_active(uint32_t now_ms);

#endif
