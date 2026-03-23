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

/*
 * Demo version:
 * Keep only essential UART JSON fields:
 * - system
 * - time
 * - pressure_raw
 * - bed_exit
 * - ldr
 * - dark
 * - pir
 * - alert
 *
 * Removed for demo speed:
 * - temperature
 * - humidity
 * - status
 * - pwm
 * - led
 * - temp_alert
 * - count
 */

void app_send_off_status(void)
{
    char tx_buffer[160];
    uint8_t hr = 0, min = 0, sec = 0;

    read_rtc(&hr, &min, &sec);

    snprintf(tx_buffer, sizeof(tx_buffer),
             "{\"system\":\"off\", \"time\":\"%02u:%02u:%02u\", "
             "\"pressure_raw\":0, \"bed_exit\":0, "
             "\"ldr\":0, \"dark\":0, \"pir\":0, \"alert\":0}\n",
             hr, min, sec);

    uart1_puts(tx_buffer);
}

void app_send_paused_status(void)
{
    char tx_buffer[160];
    uint8_t hr = 0, min = 0, sec = 0;

    read_rtc(&hr, &min, &sec);

    snprintf(tx_buffer, sizeof(tx_buffer),
             "{\"system\":\"paused\", \"time\":\"%02u:%02u:%02u\", "
             "\"pressure_raw\":0, \"bed_exit\":0, "
             "\"ldr\":0, \"dark\":0, \"pir\":0, \"alert\":0}\n",
             hr, min, sec);

    uart1_puts(tx_buffer);
}

void app_send_running_status(uint8_t bed_exit_uart_pending)
{
    char tx_buffer[160];

    uint8_t hr = 0, min = 0, sec = 0;
    uint16_t pressure_raw = 0U;
    uint8_t bed_exit_uart = 0U;
    uint16_t light_val = 0U;
    uint8_t pir_val = 0U;
    uint8_t dark_val = 0U;
    alert_type_t active_alert;
    uint8_t alert_code;

    read_rtc(&hr, &min, &sec);

    pressure_raw = pressure_get_raw();
    bed_exit_uart = bed_exit_uart_pending ? 1U : 0U;

    light_val = read_ldr();
    pir_val = read_pir();

    if (pwm_is_dark(light_val))
    {
        dark_val = 1U;
    }

    active_alert = alert_get_active();
    alert_code = (uint8_t)active_alert;

    snprintf(tx_buffer, sizeof(tx_buffer),
             "{\"system\":\"running\", \"time\":\"%02u:%02u:%02u\", "
             "\"pressure_raw\":%u, \"bed_exit\":%u, "
             "\"ldr\":%u, \"dark\":%u, \"pir\":%u, \"alert\":%u}\n",
             hr, min, sec,
             pressure_raw, bed_exit_uart,
             light_val, dark_val, pir_val, alert_code);

    uart1_puts(tx_buffer);
}
