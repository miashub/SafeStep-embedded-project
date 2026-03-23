#include "app_status.h"
#include <stdio.h>
#include <stdbool.h>

#include "uart1_drv.h"
#include "rtc.h"
#include "pressure.h"
#include "ldr.h"
#include "pir.h"
#include "alert.h"
#include "pwm.h"
#include "dht22.h"
#include "timing.h"

void app_send_off_status(void)
{
    char tx_buffer[220];
    uint8_t hr = 0, min = 0, sec = 0;

    read_rtc(&hr, &min, &sec);

    snprintf(tx_buffer, sizeof(tx_buffer),
             "{\"system\":\"off\",\"time\":\"%02u:%02u:%02u\","
             "\"pressure_raw\":0,\"bed_exit\":0,"
             "\"ldr\":0,\"dark\":0,\"pir\":0,"
             "\"brightness\":0,\"temperature\":0.0,\"humidity\":0.0,"
             "\"alert\":0}\n",
             hr, min, sec);

    uart1_puts(tx_buffer);
}

void app_send_paused_status(void)
{
    char tx_buffer[220];
    uint8_t hr = 0, min = 0, sec = 0;

    read_rtc(&hr, &min, &sec);

    snprintf(tx_buffer, sizeof(tx_buffer),
             "{\"system\":\"paused\",\"time\":\"%02u:%02u:%02u\","
             "\"pressure_raw\":0,\"bed_exit\":0,"
             "\"ldr\":0,\"dark\":0,\"pir\":0,"
             "\"brightness\":0,\"temperature\":0.0,\"humidity\":0.0,"
             "\"alert\":0}\n",
             hr, min, sec);

    uart1_puts(tx_buffer);
}

void app_send_running_status(uint8_t bed_exit_uart_pending)
{
    char tx_buffer[260];

    uint8_t hr = 0, min = 0, sec = 0;
    uint16_t pressure_raw = 0U;
    uint8_t bed_exit_uart = 0U;
    uint16_t light_val = 0U;
    uint8_t pir_val = 0U;
    uint8_t dark_val = 0U;
    uint8_t brightness = 0U;
    alert_type_t active_alert;
    uint8_t alert_code;

    int temp_x10 = 0;
    int hum_x10 = 0;
    int dht_status = -1;

    read_rtc(&hr, &min, &sec);

    pressure_raw = pressure_get_raw();
    bed_exit_uart = bed_exit_uart_pending ? 1U : 0U;

    light_val = read_ldr();
    pir_val = read_pir();

    light_val = read_ldr();
    pir_val = read_pir();

    dark_val = pwm_is_dark(light_val) ? 1U : 0U;

    {
        uint8_t motion_hold = pir_light_hold_active(millis()) ? 1U : 0U;

        if (dark_val && motion_hold)
        {
            brightness = pwm_get_duty_from_ldr(light_val);
        }
        else
        {
            brightness = 0U;
        }
    }

    active_alert = alert_get_active();
    alert_code = (uint8_t)active_alert;

    dht_status = read_dht22(&temp_x10, &hum_x10);

    if (dht_status == 0)
    {
        snprintf(tx_buffer, sizeof(tx_buffer),
                 "{\"system\":\"running\",\"time\":\"%02u:%02u:%02u\","
                 "\"pressure_raw\":%u,\"bed_exit\":%u,"
                 "\"ldr\":%u,\"dark\":%u,\"pir\":%u,"
                 "\"brightness\":%u,"
                 "\"temperature\":%d.%d,\"humidity\":%d.%d,"
                 "\"alert\":%u}\n",
                 hr, min, sec,
                 pressure_raw, bed_exit_uart,
                 light_val, dark_val, pir_val,
                 brightness,
                 temp_x10 / 10, (temp_x10 < 0 ? -temp_x10 : temp_x10) % 10,
                 hum_x10 / 10, hum_x10 % 10,
                 alert_code);
    }
    else
    {
        snprintf(tx_buffer, sizeof(tx_buffer),
                 "{\"system\":\"running\",\"time\":\"%02u:%02u:%02u\","
                 "\"pressure_raw\":%u,\"bed_exit\":%u,"
                 "\"ldr\":%u,\"dark\":%u,\"pir\":%u,"
                 "\"brightness\":%u,"
                 "\"temperature\":null,\"humidity\":null,"
                 "\"alert\":%u}\n",
                 hr, min, sec,
                 pressure_raw, bed_exit_uart,
                 light_val, dark_val, pir_val,
                 brightness,
                 alert_code);
    }

    uart1_puts(tx_buffer);
}
