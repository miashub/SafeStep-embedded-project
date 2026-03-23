#ifndef UART1_DRV_H
#define UART1_DRV_H

#include "MK66F18.h"
#include <stdbool.h>
#include <stdint.h>

#define RX_BUF_SIZE 128

extern volatile char rx_buf[RX_BUF_SIZE];
extern volatile uint16_t rx_idx;
extern volatile bool line_ready;

void uart1_init(void);
void uart1_puts(const char *str);

#endif
