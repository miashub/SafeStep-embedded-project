#include "MK66F18.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "system_ctrl.h"
#include "ldr.h"
#include "pwm.h"
#include "pir.h"
#include "pressure.h"
#include "lcd.h"
#include "alert.h"
#include "rtc.h"
#include "timing.h"
#include "dht22.h"
#include "board_led.h"
#include "uart1_drv.h"
#include "app_status.h"
#include "app_display.h"

/* ----------------------------
 * System State
 * ---------------------------- */
typedef enum
{
    SYSTEM_OFF = 0,
    SYSTEM_RUNNING,
    SYSTEM_PAUSED
} system_state_t;

/* ----------------------------
 * Runtime Context
 * ---------------------------- */
typedef struct
{
    uint32_t last_sensor_read;
    uint32_t last_pressure_update;
    uint32_t last_lcd_update;

    uint32_t bed_exit_lcd_until;
    uint32_t alert_ack_lcd_until;
    uint32_t mode_message_until;

    alert_type_t last_ack_type;

    bool bed_exit_uart_pending;
    bool fall_capture_sent;
} app_runtime_t;

/* ----------------------------
 * State Transition Helpers
 * ---------------------------- */
static void app_reset_runtime_flags(app_runtime_t *app)
{
    app->bed_exit_uart_pending = false;
    app->fall_capture_sent = false;
    app->bed_exit_lcd_until = 0U;
    app->alert_ack_lcd_until = 0U;
    app->last_ack_type = ALERT_NONE;
}

static void system_enter_running(app_runtime_t *app, system_state_t *state)
{
    *state = SYSTEM_RUNNING;

    pressure_runtime_reset();
    pressure_enable_bed_exit(true);
    alert_runtime_reset();

    app_reset_runtime_flags(app);

    led_set(0, 1, 0);
    lcd_set_backlight(1);
    lcd_display_on();
    lcd_clear();
    lcd_show_message("System Running", "");
    app->mode_message_until = millis() + 2000U;
    app->last_lcd_update = millis();

    uart1_puts("{\"status\":\"ok\",\"system\":\"running\",\"msg\":\"System Running\"}\n");
}

static void system_enter_paused(app_runtime_t *app, system_state_t *state)
{
    *state = SYSTEM_PAUSED;

    pressure_enable_bed_exit(false);
    pressure_runtime_reset();
    alert_clear();
    alert_runtime_reset();

    app_reset_runtime_flags(app);

    led_set(0, 0, 1);
    pwm_set_duty_percent(0);

    lcd_set_backlight(1);
    lcd_display_on();
    lcd_clear();
    lcd_show_message("System Paused", "");
    app->mode_message_until = millis() + 2000U;
    app->last_lcd_update = millis();

    uart1_puts("{\"status\":\"ok\",\"system\":\"paused\",\"msg\":\"System Paused\"}\n");
}

static void system_enter_off(app_runtime_t *app, system_state_t *state)
{
    pressure_enable_bed_exit(false);
    pressure_runtime_reset();
    alert_clear();
    alert_runtime_reset();

    app_reset_runtime_flags(app);

    led_set(1, 0, 0);
    pwm_set_duty_percent(0);

    lcd_set_backlight(1);
    lcd_display_on();
    lcd_clear();
    lcd_show_message("System OFF in", "3 sec");
    app->mode_message_until = millis() + 3000U;
    app->last_lcd_update = millis();

    uart1_puts("{\"status\":\"ok\",\"system\":\"shutdown_pending\",\"msg\":\"System OFF in 3 sec\"}\n");

    {
        uint32_t shutdown_start = millis();
        while ((millis() - shutdown_start) < 3000U)
        {
            /* wait */
        }
    }

    *state = SYSTEM_OFF;

    led_set(1, 0, 0);
    pwm_set_duty_percent(0);
    lcd_clear();
    lcd_set_backlight(0);
    lcd_display_off();

    uart1_puts("{\"status\":\"off\",\"system\":\"off\",\"msg\":\"System OFF\"}\n");
}

/* ----------------------------
 * Main Application
 * ---------------------------- */
int main(void)
{
    timing_init();

    led_init();
    uart1_init();
    dht22_init();
    pressure_init();
    ldr_init();
    rtc_init();
    lcd_init();
    pwm_init();
    pir_init();
    sw3_init();
    sw2_init();
    alert_init();

    app_runtime_t app = {
        .last_sensor_read = millis(),
        .last_pressure_update = millis(),
        .last_lcd_update = millis(),
        .bed_exit_lcd_until = 0U,
        .alert_ack_lcd_until = 0U,
        .mode_message_until = 0U,
        .last_ack_type = ALERT_NONE,
        .bed_exit_uart_pending = false,
        .fall_capture_sent = false
    };

    system_state_t system_state = SYSTEM_OFF;

    uint8_t last_sw3_state = 0U;
    uint8_t last_sw2_state = 0U;
    uint32_t last_sw3_time = 0U;
    uint32_t last_sw2_time = 0U;

    led_set(1, 0, 0);
    pwm_set_duty_percent(0);
    lcd_set_backlight(0);
    lcd_display_off();

    pressure_enable_bed_exit(false);
    pressure_runtime_reset();
    alert_runtime_reset();

    uart1_puts("{\"status\":\"off\",\"system\":\"off\",\"msg\":\"System OFF\"}\n");

    while (1)
    {
        uint32_t now = millis();
        uint8_t sw3_now = sw3_is_pressed();
        uint8_t sw2_now = sw2_is_pressed();

        /* ----------------------------
         * Physical Button: SW3
         * OFF <-> RUNNING
         * ---------------------------- */
        if (sw3_now && !last_sw3_state)
        {
            if ((now - last_sw3_time) > 250U)
            {
                last_sw3_time = now;

                if (system_state == SYSTEM_OFF)
                {
                    system_enter_running(&app, &system_state);
                }
                else
                {
                    system_enter_off(&app, &system_state);
                }
            }
        }
        last_sw3_state = sw3_now;

        /* ----------------------------
         * Physical Button: SW2
         * RUNNING <-> PAUSED
         * ---------------------------- */
        if (sw2_now && !last_sw2_state)
        {
            if ((now - last_sw2_time) > 250U)
            {
                last_sw2_time = now;

                if (system_state == SYSTEM_RUNNING)
                {
                    system_enter_paused(&app, &system_state);
                }
                else if (system_state == SYSTEM_PAUSED)
                {
                    system_enter_running(&app, &system_state);
                }
            }
        }
        last_sw2_state = sw2_now;

        /* ----------------------------
         * UART Commands
         * ---------------------------- */
        if (line_ready)
        {
            if (strcmp((char *)rx_buf, "SYS_ON") == 0)
            {
                if (system_state == SYSTEM_OFF)
                {
                    system_enter_running(&app, &system_state);
                }
            }
            else if (strcmp((char *)rx_buf, "SYS_OFF") == 0)
            {
                if (system_state != SYSTEM_OFF)
                {
                    system_enter_off(&app, &system_state);
                }
            }
            else if (strcmp((char *)rx_buf, "PAUSE") == 0)
            {
                if (system_state == SYSTEM_RUNNING)
                {
                    system_enter_paused(&app, &system_state);
                }
                else if (system_state == SYSTEM_PAUSED)
                {
                    system_enter_running(&app, &system_state);
                }
            }
            else if (strcmp((char *)rx_buf, "ACK") == 0)
            {
                alert_type_t active_before_ack = alert_get_active();

                if (active_before_ack != ALERT_NONE)
                {
                    alert_acknowledge_from_software();

                    app.last_ack_type = active_before_ack;
                    app.alert_ack_lcd_until = millis() + 5000U;

                    if (active_before_ack == ALERT_FALL)
                    {
                        pressure_clear_fall_flag();
                        app.fall_capture_sent = false;
                    }
                }
            }
            else if (strcmp((char *)rx_buf, "STATUS") == 0)
            {
                app.last_sensor_read = 0U;
            }

            rx_idx = 0;
            line_ready = false;
        }

        /* ----------------------------
         * Running-time sensor/alert updates
         * ---------------------------- */
        if (system_state == SYSTEM_RUNNING)
        {
            if ((now - app.last_pressure_update) >= 25U)
            {
                app.last_pressure_update = now;

                pressure_update(now);
                pir_update(now);

                if (pressure_bed_exit_event())
                {
                    app.bed_exit_uart_pending = true;
                    app.bed_exit_lcd_until = now + 5000U;
                }

                {
                    bool unusual_motion_now = pir_unusual_motion_event();
                    bool fall_now = pressure_fall_detected();

                    alert_update(now, fall_now, unusual_motion_now);

                    /* Motion-light logic: LED on only when dark and PIR hold is active.
                       Fall override is handled inside pwm_update_from_sensors(). */
                    {
                        uint16_t light_val = read_ldr();
                        uint8_t motion_for_light = pir_light_hold_active(now) ? 1U : 0U;

                        pwm_update_from_sensors(light_val, motion_for_light);
                    }

                    /* Capture image for temporary LDR-based unusual motion event */
                    if (unusual_motion_now)
                    {
                        uart1_puts("{\"cmd\":\"capture\",\"event\":\"motion\"}\n");
                    }

                    /* Capture image for fall event */
                    if (fall_now && !app.fall_capture_sent)
                    {
                        uart1_puts("{\"cmd\":\"capture\",\"event\":\"fall\"}\n");
                        app.fall_capture_sent = true;
                    }

                    {
                        alert_type_t acked_type = alert_take_acknowledged_type();
                        if (acked_type != ALERT_NONE)
                        {
                            app.last_ack_type = acked_type;
                            app.alert_ack_lcd_until = now + 5000U;

                            if (acked_type == ALERT_FALL)
                            {
                                pressure_clear_fall_flag();
                                app.fall_capture_sent = false;
                            }
                        }
                    }

                    if (!fall_now && (alert_get_active() != ALERT_FALL))
                    {
                        app.fall_capture_sent = false;
                    }
                }
            }
        }

        /* ----------------------------
         * LCD refresh
         * ---------------------------- */
        if (system_state == SYSTEM_RUNNING)
        {
            if ((now - app.last_lcd_update) >= 100U)
            {
                app.last_lcd_update = now;

                app_update_lcd(now,
                               app.mode_message_until,
                               app.bed_exit_lcd_until,
                               app.alert_ack_lcd_until,
                               app.last_ack_type);
            }
        }

        /* ----------------------------
         * Periodic UART status
         * ---------------------------- */
        if (system_state == SYSTEM_OFF)
        {
            if ((now - app.last_sensor_read) > 2500U)
            {
                app.last_sensor_read = now;
                app_send_off_status();
            }

            pwm_set_duty_percent(0);
            continue;
        }

        if (system_state == SYSTEM_PAUSED)
        {
            if ((now - app.last_sensor_read) > 2500U)
            {
                app.last_sensor_read = now;
                app_send_paused_status();
            }

            pwm_set_duty_percent(0);
            continue;
        }

        if ((now - app.last_sensor_read) > 2500U)
        {
            app.last_sensor_read = now;
            app_send_running_status(app.bed_exit_uart_pending);
            app.bed_exit_uart_pending = false;
        }
    }
}
