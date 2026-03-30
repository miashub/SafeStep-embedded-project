#ifndef ALERT_H
#define ALERT_H

#include <stdbool.h>
#include <stdint.h>

/* ----------------------------
 * Alert Types
 * ---------------------------- */
typedef enum
{
    ALERT_NONE = 0,
    ALERT_UNUSUAL_MOTION = 1,
    ALERT_FALL = 2,
    ALERT_TEMPERATURE = 3
} alert_type_t;

/* ----------------------------
 * Initialization
 * ---------------------------- */

/**
 * @brief Initialize alert GPIOs and internal state.
 */
void alert_init(void);

/**
 * @brief Reset runtime alert state.
 * Clears active alert, patterns, and pending events.
 */
void alert_runtime_reset(void);

/* ----------------------------
 * Core Update
 * ---------------------------- */

/**
 * @brief Update alert state machine.
 *
 * @param now_ms Current system time in milliseconds
 * @param fall_detected_input Current fall-detected input
 * @param unusual_motion_input Current unusual motion input
 * @param temp_alert_input Temperature alert input (from DHT module)
 */
void alert_update(uint32_t now_ms,
                  bool fall_detected_input,
                  bool unusual_motion_input,
                  bool temp_alert_input);

/* ----------------------------
 * Control / State
 * ---------------------------- */

/**
 * @brief Force clear current alert state.
 */
void alert_clear(void);

/**
 * @brief Get currently active alert type.
 */
alert_type_t alert_get_active(void);

/**
 * @brief True if an alert is currently active.
 */
bool alert_is_active(void);

/**
 * @brief Get acknowledged alert type (consumes event).
 */
alert_type_t alert_take_acknowledged_type(void);

/**
 * @brief Software-triggered acknowledge (same as button).
 */
void alert_acknowledge_from_software(void);

/* ----------------------------
 * Buzzer State (read-only)
 * ---------------------------- */

/**
 * @brief Returns true if buzzer output is currently ON.
 */
bool alert_is_buzzer_output_on(void);

#endif /* ALERT_H */
