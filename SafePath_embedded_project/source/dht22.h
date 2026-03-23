#ifndef DHT22_H
#define DHT22_H

#include "MK66F18.h"
#include <stdint.h>

void dht22_init(void);
int read_dht22(int *temperature_x10, int *humidity_x10);

#endif
