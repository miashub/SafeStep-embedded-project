#ifndef ALERT_H
#define ALERT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    ALERT_NONE = 0,
    ALERT_UNUSUAL_MOTION = 1,
    ALERT_FALL = 2
} alert_type_t;

/**
 * @brief Initialize alert GPIOs and internal state.
 */
void alert_init(void);

/**
 * @brief Reset runtime alert state.
 * Keeps GPIO config but clears active alert, patterns, and pending events.
 */
void alert_runtime_reset(void);

/**
 * @brief Update alert state machine.
 *
 * @param now_ms Current system time in milliseconds
 * @param fall_detected_input Current fall-detected input
 * @param unusual_motion_input Current unusual motion input
 */
void alert_update(uint32_t now_ms, bool fall_detected_input, bool unusual_motion_input);

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
 * @brief Return the alert type that was acknowledged by button press.
 * Returns ALERT_NONE if no new acknowledge event is pending.
 *
 * This function consumes the pending acknowledge event.
 */
alert_type_t alert_take_acknowledged_type(void);


void alert_acknowledge_from_software(void);

#endif /* ALERT_H */
