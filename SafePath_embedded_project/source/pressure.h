#ifndef PRESSURE_H
#define PRESSURE_H

#include <stdbool.h>
#include <stdint.h>

void pressure_init(void);
void pressure_runtime_reset(void);
void pressure_enable_bed_exit(bool enable);
void pressure_update(uint32_t now_ms);

uint16_t pressure_get_raw(void);
uint16_t pressure_get_filtered(void);   /* kept for compatibility */
uint16_t pressure_get_baseline(void);

int16_t pressure_get_derivative(void);      /* now returns raw derivative */
int16_t pressure_get_raw_derivative(void);

bool pressure_is_mat_clear(void);
bool pressure_bed_exit_event(void);
bool pressure_fall_detected(void);
void pressure_clear_fall_flag(void);

#endif
