#ifndef RTC_H
#define RTC_H

#include "MK66F18.h"
#include <stdint.h>
#include <stdbool.h>

/* DS3231 on I2C1 / PTC10, PTC11 */
#define RTC_PORT        PORTC
#define RTC_SCL_PIN     10
#define RTC_SDA_PIN     11
#define RTC_I2C         I2C1
#define DS3231_ADDR     0x68

void rtc_init(void);
int read_rtc(uint8_t *h, uint8_t *m, uint8_t *s);
void set_rtc(uint8_t h, uint8_t m, uint8_t s);
bool rtc_lost_power(void);
void rtc_clear_osf(void);

#endif
