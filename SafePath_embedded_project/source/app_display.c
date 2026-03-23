#include "app_display.h"
#include <stdio.h>
#include <string.h>

#include "lcd.h"
#include "dht22.h"
#include "ldr.h"
#include "pir.h"
#include "alert.h"

void app_update_lcd(uint32_t now,
                    uint32_t mode_message_until,
                    uint32_t bed_exit_lcd_until,
                    uint32_t alert_ack_lcd_until,
                    alert_type_t last_ack_type)
{
    int temp_x10 = 0;
    int hum_x10 = 0;
    int status = read_dht22(&temp_x10, &hum_x10);

    uint16_t light_val = read_ldr();
    uint8_t pir_val = read_pir();
    alert_type_t active_alert = alert_get_active();

    if ((int32_t)(mode_message_until - now) > 0)
    {
        return;
    }

    if (active_alert == ALERT_FALL)
    {
        lcd_show_message("FALL ALERT", "Press Ack Btn");
    }
    else if (active_alert == ALERT_UNUSUAL_MOTION)
    {
        lcd_show_message("UNUSUAL MOTION", "Press Ack Btn");
    }
    else if ((int32_t)(alert_ack_lcd_until - now) > 0)
    {
        if (last_ack_type == ALERT_FALL)
        {
            lcd_show_message("FALL", "ACKNOWLEDGED");
        }
        else if (last_ack_type == ALERT_UNUSUAL_MOTION)
        {
            lcd_show_message("UNUSUAL MOTION", "ACKNOWLEDGED");
        }
        else
        {
            lcd_show_message("ALERT", "ACKNOWLEDGED");
        }
    }
    else if ((int32_t)(bed_exit_lcd_until - now) > 0)
    {
        lcd_show_message("Bed Exit Alert", "Patient Left Bed");
    }
    else
    {
        if (status == 0)
        {
            lcd_display_sensors(temp_x10, hum_x10, light_val, pir_val);
        }
        else
        {
            char buf[17];

            lcd_set_cursor(0, 0);
            lcd_print("DHT Error!      ");

            lcd_set_cursor(0, 1);
            snprintf(buf, sizeof(buf), "L:%u M:%s", light_val, pir_val ? "YES" : "NO");
            lcd_print(buf);

            for (int i = (int)strlen(buf); i < 16; i++)
            {
                lcd_data(' ');
            }
        }
    }
}
