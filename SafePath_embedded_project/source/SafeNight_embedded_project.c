/* --------------------------------------------------------------------------
 *  File: SafeNight_embedded_project.c
 *
 *  Project: SafeStep Night Monitoring System
 *
 *  Description:
 *  Main application for the FRDM-K66F based SafeStep system. This file
 *  coordinates sensor reading, alert detection, LCD updates, UART messaging,
 *  escalation flow, and overall system state control.
 *
 *  System Modes:
 *      - SYSTEM_OFF
 *      - SYSTEM_RUNNING
 *      - SYSTEM_PAUSED
 *
 *  AI Usage Declaration:
 *  AI assistance was used in this file to improve documentation, comment
 *  quality, code organization, and explanation of control flow. The final
 *  implementation logic, hardware integration, debugging, and validation
 *  were completed manually as part of the SafeStep project.
 * -------------------------------------------------------------------------- */

#include "MK66F18.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

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
#include "escalation.h"
#include "nv_storage.h"
#include "health_monitor.h"

/* ----------------------------
 * System State
 * ---------------------------- */
/**
 * @brief High-level operating modes of the SafeStep system.
 */
typedef enum
{
    SYSTEM_OFF = 0,   /* System disabled */
    SYSTEM_RUNNING,   /* Normal monitoring mode */
    SYSTEM_PAUSED     /* Monitoring temporarily paused */
} system_state_t;

/* ----------------------------
 * Runtime Context
 * ---------------------------- */
/* AI-assisted: documentation and comment refinement for this section. */
/**
 * @brief Stores runtime timing values and event flags used by the main loop.
 *
 * This structure centralizes temporary state information so the application
 * logic remains easier to maintain and extend.
 */
typedef struct
{
    uint32_t last_sensor_read;      /* Last periodic status transmission time */
    uint32_t last_pressure_update;  /* Last pressure/PIR processing time */
    uint32_t last_lcd_update;       /* Last LCD refresh time */
    uint32_t last_health_update;    /* Last health monitor update time */

    uint32_t bed_exit_lcd_until;    /* LCD timeout for bed-exit message */
    uint32_t alert_ack_lcd_until;   /* LCD timeout for alert-ack message */
    uint32_t mode_message_until;    /* LCD timeout for mode transition message */

    alert_type_t last_ack_type;     /* Most recently acknowledged alert type */

    bool bed_exit_uart_pending;     /* Send bed-exit event on next status packet */
    bool fall_capture_sent;         /* Prevent repeated fall capture requests */
    bool fall_escalation_started;   /* Prevent repeated escalation start */
} app_runtime_t;

/* ----------------------------
 * Temperature alert tuning
 * ---------------------------- */
#define TEMP_ALERT_THRESHOLD_X10      300   /* 30.0 C */

/* ----------------------------
 * Periodic timing
 * ---------------------------- */
#define PRESSURE_UPDATE_INTERVAL_MS   25U
#define LCD_UPDATE_INTERVAL_MS        100U
#define HEALTH_UPDATE_INTERVAL_MS     250U

#define OFF_STATUS_INTERVAL_MS        2500U
#define PAUSED_STATUS_INTERVAL_MS     1500U
#define RUNNING_STATUS_INTERVAL_MS    1000U

/* ----------------------------
 * UART debug helpers
 * ---------------------------- */
/**
 * @brief Remove leading and trailing whitespace from a UART command string.
 *
 * @param s Mutable null-terminated string buffer.
 */
static void trim_uart_line(char *s)
{
    size_t len;
    char *start = s;

    while ((*start == ' ') || (*start == '\t') || (*start == '\r') || (*start == '\n'))
    {
        start++;
    }

    if (start != s)
    {
        memmove(s, start, strlen(start) + 1U);
    }

    len = strlen(s);
    while (len > 0U)
    {
        char c = s[len - 1U];
        if ((c == ' ') || (c == '\t') || (c == '\r') || (c == '\n'))
        {
            s[len - 1U] = '\0';
            len--;
        }
        else
        {
            break;
        }
    }
}

/**
 * @brief Send a UART message and record the TX activity for health monitoring.
 *
 * @param msg Null-terminated UART message to send.
 */
static void uart_send_and_note(const char *msg)
{
    uint32_t now_ms = millis();
    uart1_puts(msg);
    health_monitor_note_k66f_uart_tx(now_ms);
}

/**
 * @brief Send a structured debug message over UART.
 *
 * This is useful during integration and debugging. If dashboard or serial
 * output becomes too noisy, the body of this function can be disabled.
 *
 * @param msg Debug text to wrap in a JSON field.
 */
static void uart_debug_rx(const char *msg)
{
    char tx[180];
    snprintf(tx, sizeof(tx), "{\"uart_debug\":\"%s\"}\n", msg);
    uart_send_and_note(tx);
}

/* ----------------------------
 * Safe output helper
 * ---------------------------- */
/**
 * @brief Force critical outputs to a safe inactive state.
 *
 * This is used during state transitions and whenever the system should stop
 * generating active responses.
 */
static void app_force_safe_outputs_off(void)
{
    pwm_set_duty_percent(0);
    alert_clear();
}

/* ----------------------------
 * Runtime reset helper
 * ---------------------------- */
/**
 * @brief Reset runtime event flags and LCD message timers.
 *
 * @param app Pointer to runtime context.
 */
static void app_reset_runtime_flags(app_runtime_t *app)
{
    app->bed_exit_uart_pending = false;
    app->fall_capture_sent = false;
    app->fall_escalation_started = false;

    app->bed_exit_lcd_until = 0U;
    app->alert_ack_lcd_until = 0U;
    app->mode_message_until = 0U;
    app->last_ack_type = ALERT_NONE;

    app->last_health_update = 0U;
}

/* ----------------------------
 * State Transition Helpers
 * ---------------------------- */
/* AI-assisted: documentation and structure refinement for transition logic. */
/**
 * @brief Transition the system into RUNNING mode.
 *
 * Reinitializes runtime subsystems, enables the display, updates LEDs,
 * and notifies the ESP32/dashboard over UART.
 *
 * @param app Pointer to runtime context.
 * @param state Pointer to current system state.
 */
static void system_enter_running(app_runtime_t *app, system_state_t *state)
{
    uint32_t now = millis();

    *state = SYSTEM_RUNNING;

    app_force_safe_outputs_off();

    /* Reset runtime-sensitive modules so monitoring restarts cleanly. */
    pressure_runtime_reset();
    alert_runtime_reset();
    escalation_reset();
    health_monitor_reset_runtime();

    app_reset_runtime_flags(app);

    /* Visual feedback for active monitoring mode. */
    led_set(0, 1, 0);

    lcd_set_backlight(1);
    lcd_display_on();
    lcd_clear();
    lcd_show_message("System Running", "");

    app->mode_message_until = now + 2000U;
    app->last_lcd_update = now;
    app->last_pressure_update = now;
    app->last_sensor_read = 0U;

    uart_send_and_note("{\"status\":\"ok\",\"system\":\"running\",\"msg\":\"System Running\"}\n");
}

/**
 * @brief Transition the system into PAUSED mode.
 *
 * In paused mode, monitoring is halted, outputs are disabled, and the
 * dashboard is informed of the state change.
 *
 * @param app Pointer to runtime context.
 * @param state Pointer to current system state.
 */
static void system_enter_paused(app_runtime_t *app, system_state_t *state)
{
    uint32_t now = millis();

    *state = SYSTEM_PAUSED;

    pressure_runtime_reset();
    alert_runtime_reset();
    escalation_reset();

    app_force_safe_outputs_off();
    app_reset_runtime_flags(app);

    /* Blue LED indicates paused state. */
    led_set(0, 0, 1);

    lcd_set_backlight(1);
    lcd_display_on();
    lcd_clear();
    lcd_show_message("System Paused", "");

    app->mode_message_until = now + 2000U;
    app->last_lcd_update = now;
    app->last_sensor_read = 0U;

    uart_send_and_note("{\"status\":\"ok\",\"system\":\"paused\",\"msg\":\"System Paused\"}\n");
}

/**
 * @brief Transition the system into OFF mode with a short shutdown delay.
 *
 * This gives the user visible feedback before the display is turned off.
 *
 * @param app Pointer to runtime context.
 * @param state Pointer to current system state.
 */
static void system_enter_off(app_runtime_t *app, system_state_t *state)
{
    uint32_t shutdown_start;

    pressure_runtime_reset();
    alert_runtime_reset();
    escalation_reset();

    app_force_safe_outputs_off();
    app_reset_runtime_flags(app);

    /* Red LED indicates system off / shutdown state. */
    led_set(1, 0, 0);

    lcd_set_backlight(1);
    lcd_display_on();
    lcd_clear();
    lcd_show_message("System OFF in", "3 sec");

    app->mode_message_until = millis() + 3000U;
    app->last_lcd_update = millis();

    uart_send_and_note("{\"status\":\"ok\",\"system\":\"shutdown_pending\",\"msg\":\"System OFF in 3 sec\"}\n");

    shutdown_start = millis();
    while ((millis() - shutdown_start) < 3000U)
    {
        /* Intentional blocking delay for visible shutdown countdown. */
    }

    *state = SYSTEM_OFF;

    app_force_safe_outputs_off();

    led_set(1, 0, 0);
    lcd_clear();
    lcd_set_backlight(0);
    lcd_display_off();

    app->last_sensor_read = 0U;

    uart_send_and_note("{\"status\":\"off\",\"system\":\"off\",\"msg\":\"System OFF\"}\n");
}

/* ----------------------------
 * Main Application
 * ---------------------------- */
/* AI-assisted: documentation and control-flow explanation refinement. */
/**
 * @brief Main entry point for the SafeStep embedded application.
 *
 * Responsibilities:
 *  - Initialize all hardware and software modules
 *  - Monitor physical button input for mode changes
 *  - Parse UART commands from the ESP32/dashboard
 *  - Process sensor data and update alerts
 *  - Trigger capture and escalation events when required
 *  - Refresh LCD content
 *  - Send periodic status JSON for dashboard display and logging
 *
 * @return int Never returns in normal embedded operation.
 */
int main(void)
{
    /* Core timing and non-volatile configuration setup. */
    timing_init();
    nv_init();

    /* Peripheral and subsystem initialization. */
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
    escalation_init();
    health_monitor_init();

    /* Runtime context initialization. */
    app_runtime_t app = {
        .last_sensor_read = millis(),
        .last_pressure_update = millis(),
        .last_lcd_update = millis(),
        .last_health_update = millis(),
        .bed_exit_lcd_until = 0U,
        .alert_ack_lcd_until = 0U,
        .mode_message_until = 0U,
        .last_ack_type = ALERT_NONE,
        .bed_exit_uart_pending = false,
        .fall_capture_sent = false,
        .fall_escalation_started = false
    };

    system_state_t system_state = SYSTEM_OFF;

    /* Button edge-detection state. */
    uint8_t last_sw3_state = 0U;
    uint8_t last_sw2_state = 0U;
    uint32_t last_sw3_time = 0U;
    uint32_t last_sw2_time = 0U;

    /* Safe startup defaults. */
    led_set(1, 0, 0);
    app_force_safe_outputs_off();

    lcd_set_backlight(0);
    lcd_display_off();

    pressure_runtime_reset();
    alert_runtime_reset();
    escalation_reset();

    health_monitor_set_all_off();

    uart_send_and_note("{\"status\":\"off\",\"system\":\"off\",\"msg\":\"System OFF\"}\n");

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
        /* AI-assisted: comment refinement for command-handling section. */
        if (line_ready)
        {
            char cmd_line[128];

            strncpy(cmd_line, (char *)rx_buf, sizeof(cmd_line) - 1U);
            cmd_line[sizeof(cmd_line) - 1U] = '\0';

            trim_uart_line(cmd_line);

            /* A valid command indicates ESP32-to-K66F RX path is active. */
            health_monitor_note_esp32_rx(now);

            {
                char dbg[160];
                snprintf(dbg, sizeof(dbg), "RX:%s", cmd_line);
                uart_debug_rx(dbg);
            }

            if (strcmp(cmd_line, "SYS_ON") == 0)
            {
                uart_debug_rx("CMD_MATCH:SYS_ON");

                if (system_state == SYSTEM_OFF)
                {
                    system_enter_running(&app, &system_state);
                }
            }
            else if (strcmp(cmd_line, "SYS_OFF") == 0)
            {
                uart_debug_rx("CMD_MATCH:SYS_OFF");

                if (system_state != SYSTEM_OFF)
                {
                    system_enter_off(&app, &system_state);
                }
            }
            else if (strcmp(cmd_line, "PAUSE") == 0)
            {
                uart_debug_rx("CMD_MATCH:PAUSE");

                if (system_state == SYSTEM_RUNNING)
                {
                    system_enter_paused(&app, &system_state);
                }
                else if (system_state == SYSTEM_PAUSED)
                {
                    system_enter_running(&app, &system_state);
                }
            }
            else if (strcmp(cmd_line, "ACK") == 0)
            {
                alert_type_t active_before_ack = alert_get_active();

                uart_debug_rx("CMD_MATCH:ACK");

                if (active_before_ack != ALERT_NONE)
                {
                    /* Apply dashboard/software acknowledgement to active alert. */
                    alert_acknowledge_from_software();
                    pressure_ack_reset();

                    app.last_ack_type = active_before_ack;
                    app.alert_ack_lcd_until = now + 5000U;
                    app.bed_exit_uart_pending = false;

                    if (active_before_ack == ALERT_FALL)
                    {
                        app.fall_capture_sent = false;
                        app.fall_escalation_started = false;
                        escalation_acknowledge();
                        uart_debug_rx("ACK_APPLIED:FALL");
                    }
                    else if (active_before_ack == ALERT_UNUSUAL_MOTION)
                    {
                        uart_debug_rx("ACK_APPLIED:MOTION");
                    }
                    else if (active_before_ack == ALERT_TEMPERATURE)
                    {
                        uart_debug_rx("ACK_APPLIED:TEMPERATURE");
                    }
                    else
                    {
                        uart_debug_rx("ACK_APPLIED:OTHER");
                    }

                    app.last_sensor_read = 0U;
                }
                else
                {
                    uart_debug_rx("ACK_IGNORED:NO_ACTIVE_ALERT");
                }
            }
            else if (strcmp(cmd_line, "STATUS") == 0)
            {
                /* Force an early status refresh for dashboard synchronization. */
                uart_debug_rx("CMD_MATCH:STATUS");
                app.last_sensor_read = 0U;
            }
            else if (strcmp(cmd_line, "DIAG_LINK") == 0)
            {
                /* Link heartbeat from ESP32/dashboard. */
                uart_debug_rx("CMD_MATCH:DIAG_LINK");
            }
            else if (strcmp(cmd_line, "MANUAL_CAPTURE") == 0)
            {
                uart_debug_rx("CMD_MATCH:MANUAL_CAPTURE");

                if (system_state == SYSTEM_RUNNING)
                {
                    uart_send_and_note("{\"cmd\":\"capture\",\"event\":\"manual\"}\n");
                    uart_debug_rx("MANUAL_CAPTURE_SENT");
                }
                else
                {
                    uart_debug_rx("MANUAL_CAPTURE_IGNORED:NOT_RUNNING");
                }
            }
            else if (strncmp(cmd_line, "ldr_setting_change:", 19) == 0)
            {
                uint32_t val = (uint32_t)atoi(&cmd_line[19]);

                nv_set_ldr_threshold(val);

                {
                    char msg[100];
                    snprintf(msg, sizeof(msg), "{\"ldr_threshold\":%lu}\n", (unsigned long)val);
                    uart_send_and_note(msg);
                }

                uart_debug_rx("CMD_MATCH:LDR_SET");
            }
            else if (strcmp(cmd_line, "ldr_setting_set_default") == 0)
            {
                nv_reset_ldr_threshold();
                uart_send_and_note("{\"ldr_threshold\":\"default\"}\n");
                uart_debug_rx("CMD_MATCH:LDR_DEFAULT");
            }
            else
            {
                char dbg[160];
                snprintf(dbg, sizeof(dbg), "CMD_UNKNOWN:%s", cmd_line);
                uart_debug_rx(dbg);
            }

            /* Clear receive state after command processing. */
            rx_idx = 0;
            line_ready = false;
        }

        /* ----------------------------
         * Running-time sensor/alert updates
         * ---------------------------- */
        if (system_state == SYSTEM_RUNNING)
        {
            if ((now - app.last_pressure_update) >= PRESSURE_UPDATE_INTERVAL_MS)
            {
                bool unusual_motion_now;
                bool fall_now;
                bool temp_alert_now = false;
                int temp_x10 = 0;
                int hum_x10 = 0;
                int dht_status;
                uint16_t light_val;
                uint8_t motion_for_light;
                alert_type_t acked_type;

                app.last_pressure_update = now;

                /* Update fast-changing sensor logic first. */
                pressure_update(now);
                pir_update(now);

                /* bed_exit_event() is a pulse, so capture it immediately. */
                if (pressure_bed_exit_event())
                {
                    app.bed_exit_uart_pending = true;
                    app.bed_exit_lcd_until = now + 5000U;
                    app.last_sensor_read = 0U;
                }

                unusual_motion_now = pir_unusual_motion_event();
                fall_now = pressure_fall_detected();

                /* Read environmental sensor and derive temperature alert. */
                dht_status = read_dht22(&temp_x10, &hum_x10);
                if ((dht_status == 0) && (temp_x10 >= TEMP_ALERT_THRESHOLD_X10))
                {
                    temp_alert_now = true;
                }

                /* Feed current event signals into the alert state machine. */
                alert_update(now, fall_now, unusual_motion_now, temp_alert_now);

                /* Start one-time fall escalation timer when a fall begins. */
                if (fall_now && !app.fall_escalation_started)
                {
                    escalation_start_fall(now);
                    app.fall_escalation_started = true;
                    app.last_sensor_read = 0U;
                }

                escalation_update(now, fall_now);

                /* Lighting control is based on LDR + motion hold state. */
                light_val = read_ldr();
                motion_for_light = pir_light_hold_active(now) ? 1U : 0U;
                pwm_update_from_sensors(light_val, motion_for_light);

                /* Motion-triggered image request. */
                if (unusual_motion_now)
                {
                    uart_send_and_note("{\"cmd\":\"capture\",\"event\":\"motion\"}\n");
                }

                /* Fall-triggered image request, sent only once per event. */
                if (fall_now && !app.fall_capture_sent)
                {
                    uart_send_and_note("{\"cmd\":\"capture\",\"event\":\"fall\"}\n");
                    app.fall_capture_sent = true;
                }

                /* Send escalation command once the escalation module requests it. */
                if (escalation_should_send_uart())
                {
                    uart_send_and_note("{\"cmd\":\"escalate\",\"event\":\"fall\",\"type\":\"email\"}\n");
                }

                /* Handle hardware/software acknowledgement reported by alert module. */
                acked_type = alert_take_acknowledged_type();
                if (acked_type != ALERT_NONE)
                {
                    app.last_ack_type = acked_type;
                    app.alert_ack_lcd_until = now + 5000U;

                    pressure_ack_reset();
                    app.bed_exit_uart_pending = false;

                    if (acked_type == ALERT_FALL)
                    {
                        app.fall_capture_sent = false;
                        app.fall_escalation_started = false;
                        escalation_acknowledge();
                    }

                    app.last_sensor_read = 0U;
                }

                /* When fall is fully cleared, reset fall-related runtime flags. */
                if (!pressure_fall_detected() && (alert_get_active() != ALERT_FALL))
                {
                    app.fall_capture_sent = false;
                    app.fall_escalation_started = false;
                    escalation_reset();
                }
            }

            if ((now - app.last_health_update) >= HEALTH_UPDATE_INTERVAL_MS)
            {
                app.last_health_update = now;
                health_monitor_update(now);
            }
        }

        /* ----------------------------
         * LCD refresh
         * ---------------------------- */
        if (system_state == SYSTEM_RUNNING)
        {
            if ((now - app.last_lcd_update) >= LCD_UPDATE_INTERVAL_MS)
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
            health_monitor_set_all_off();

            if ((now - app.last_sensor_read) > OFF_STATUS_INTERVAL_MS)
            {
                app.last_sensor_read = now;
                app_send_off_status();
                health_monitor_note_k66f_uart_tx(now);
            }

            app_force_safe_outputs_off();
            continue;
        }

        if (system_state == SYSTEM_PAUSED)
        {
            health_monitor_set_paused_mode();

            if ((now - app.last_sensor_read) > PAUSED_STATUS_INTERVAL_MS)
            {
                app.last_sensor_read = now;
                app_send_paused_status();
                health_monitor_note_k66f_uart_tx(now);
            }

            app_force_safe_outputs_off();
            continue;
        }

        if ((now - app.last_sensor_read) > RUNNING_STATUS_INTERVAL_MS)
        {
            app.last_sensor_read = now;

            /* Periodic status JSON supports dashboard display and logging. */
            app_send_running_status(app.bed_exit_uart_pending);
            health_monitor_note_k66f_uart_tx(now);

            /* Clear one-shot UART event flag after transmission. */
            app.bed_exit_uart_pending = false;
        }
    }
}