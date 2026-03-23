#ifndef PWM_H
#define PWM_H

#include <stdint.h>

void pwm_init(void);
void pwm_set_duty_percent(uint8_t percent);
uint8_t pwm_get_duty_from_ldr(uint16_t ldr_value);
uint8_t pwm_is_dark(uint16_t ldr_value);
void pwm_update_from_sensors(uint16_t ldr_value, uint8_t pir_value);

#endif
