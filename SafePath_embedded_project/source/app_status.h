#ifndef APP_STATUS_H
#define APP_STATUS_H

#include <stdint.h>
#include "alert.h"

void app_send_off_status(void);
void app_send_paused_status(void);
void app_send_running_status(uint8_t bed_exit_uart_pending);

#endif
