#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdint.h>
#include "alert.h"

void app_update_lcd(uint32_t now,
                    uint32_t mode_message_until,
                    uint32_t bed_exit_lcd_until,
                    uint32_t alert_ack_lcd_until,
                    alert_type_t last_ack_type);

#endif
