#ifndef DHT22_H
#define DHT22_H

#include <stdbool.h>
#include <stdint.h>

void dht22_init(void);
int read_dht22(int *temperature_x10, int *humidity_x10);

int dht22_sample_and_update(uint32_t now_ms);

bool dht22_temp_high_alert_active(void);
bool dht22_temp_low_alert_active(void);
bool dht22_temp_drop_alert_active(void);
bool dht22_temp_alert_active(void);

int dht22_get_last_temp_x10(void);
int dht22_get_last_humidity_x10(void);

#endif /* DHT22_H */
