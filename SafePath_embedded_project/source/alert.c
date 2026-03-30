/* -------alert.c----------*/

#include "alert.h"
#include "MK66F18.h"
#include "pir.h"
#include <stdbool.h>
#include <stdint.h>

/* ----------------------------
 * GPIO Configuration
 * ---------------------------- */
#define ALERT_PORT          PORTC
#define ALERT_GPIO          GPIOC

#define BUZZER_PIN          16U
#define ACK_BUTTON_PIN      8U

/* ----------------------------
 * Timing
 * ---------------------------- */
#define ACK_DEBOUNCE_MS     50U

/* Fall pattern (urgent)
 * ON 120, OFF 80, ON 120, OFF 80, ON 180, OFF 450
 */
#define FALL_STEP0_MS       120U
#define FALL_STEP1_MS       80U
#define FALL_STEP2_MS       120U
#define FALL_STEP3_MS       80U
#define FALL_STEP4_MS       180U
#define FALL_STEP5_MS       450U

/* Non-fall pattern (temperature / unusual motion)
 * ON 120, OFF 350, ON 120, OFF 1000
 */
#define NONFALL_STEP0_MS    120U
#define NONFALL_STEP1_MS    350U
#define NONFALL_STEP2_MS    120U
#define NONFALL_STEP3_MS    1000U

#define UNUSUAL_MOTION_LED_HOLD_MS   10000U

/* ----------------------------
 * Private State
 * ---------------------------- */
typedef struct
{
    bool stable_pressed;
    bool last_raw_pressed;
    uint32_t last_change_ms;
} ack_button_state_t;

static volatile alert_type_t g_active_alert = ALERT_NONE;
static volatile alert_type_t g_acknowledged_type = ALERT_NONE;

static ack_button_state_t g_ack_button = { false, false, 0U };

static uint8_t g_pattern_step = 0U;
static uint32_t g_pattern_elapsed_ms = 0U;
static uint32_t g_last_update_ms = 0U;

static bool g_prev_motion_input = false;
static bool g_prev_fall_input = false;
static bool g_prev_temp_input = false;
static bool g_inputs_initialized = false;

/* Prevent re-triggering same acknowledged fall until input goes low */
static bool g_fall_wait_for_clear = false;

/* ----------------------------
 * Buzzer State
 * ---------------------------- */
static volatile bool g_buzzer_output_on = false;

/* ----------------------------
 * Low-Level Helpers
 * ---------------------------- */
static void buzzer_apply(bool on)
{
    if (on)
    {
        ALERT_GPIO->PSOR = (1UL << BUZZER_PIN);
        g_buzzer_output_on = true;
    }
    else
    {
        ALERT_GPIO->PCOR = (1UL << BUZZER_PIN);
        g_buzzer_output_on = false;
    }
}

static bool ack_button_raw_pressed(void)
{
    return ((ALERT_GPIO->PDIR & (1UL << ACK_BUTTON_PIN)) == 0U);
}

static bool ack_button_pressed_event(uint32_t now_ms)
{
    bool raw = ack_button_raw_pressed();

    if (raw != g_ack_button.last_raw_pressed)
    {
        g_ack_button.last_raw_pressed = raw;
        g_ack_button.last_change_ms = now_ms;
    }

    if ((now_ms - g_ack_button.last_change_ms) >= ACK_DEBOUNCE_MS)
    {
        if (g_ack_button.stable_pressed != raw)
        {
            g_ack_button.stable_pressed = raw;

            if (raw)
            {
                return true;
            }
        }
    }

    return false;
}

static void alert_start(alert_type_t type)
{
    if (g_active_alert != type)
    {
        g_active_alert = type;
        g_pattern_step = 0U;
        g_pattern_elapsed_ms = 0U;
    }
}

static void alert_stop(void)
{
    g_active_alert = ALERT_NONE;
    g_pattern_step = 0U;
    g_pattern_elapsed_ms = 0U;
    buzzer_apply(false);
}

/* ----------------------------
 * Pattern Logic
 * ---------------------------- */
static bool alert_compute_fall_output(void)
{
    switch (g_pattern_step)
    {
        case 0:
        case 2:
        case 4:
            return true;

        default:
            return false;
    }
}

static bool alert_compute_nonfall_output(void)
{
    switch (g_pattern_step)
    {
        case 0:
        case 2:
            return true;

        default:
            return false;
    }
}

static void alert_advance_fall_pattern(void)
{
    switch (g_pattern_step)
    {
        case 0:
            if (g_pattern_elapsed_ms >= FALL_STEP0_MS)
            {
                g_pattern_elapsed_ms = 0U;
                g_pattern_step = 1U;
            }
            break;

        case 1:
            if (g_pattern_elapsed_ms >= FALL_STEP1_MS)
            {
                g_pattern_elapsed_ms = 0U;
                g_pattern_step = 2U;
            }
            break;

        case 2:
            if (g_pattern_elapsed_ms >= FALL_STEP2_MS)
            {
                g_pattern_elapsed_ms = 0U;
                g_pattern_step = 3U;
            }
            break;

        case 3:
            if (g_pattern_elapsed_ms >= FALL_STEP3_MS)
            {
                g_pattern_elapsed_ms = 0U;
                g_pattern_step = 4U;
            }
            break;

        case 4:
            if (g_pattern_elapsed_ms >= FALL_STEP4_MS)
            {
                g_pattern_elapsed_ms = 0U;
                g_pattern_step = 5U;
            }
            break;

        default:
            if (g_pattern_elapsed_ms >= FALL_STEP5_MS)
            {
                g_pattern_elapsed_ms = 0U;
                g_pattern_step = 0U;
            }
            break;
    }
}

static void alert_advance_nonfall_pattern(void)
{
    switch (g_pattern_step)
    {
        case 0:
            if (g_pattern_elapsed_ms >= NONFALL_STEP0_MS)
            {
                g_pattern_elapsed_ms = 0U;
                g_pattern_step = 1U;
            }
            break;

        case 1:
            if (g_pattern_elapsed_ms >= NONFALL_STEP1_MS)
            {
                g_pattern_elapsed_ms = 0U;
                g_pattern_step = 2U;
            }
            break;

        case 2:
            if (g_pattern_elapsed_ms >= NONFALL_STEP2_MS)
            {
                g_pattern_elapsed_ms = 0U;
                g_pattern_step = 3U;
            }
            break;

        default:
            if (g_pattern_elapsed_ms >= NONFALL_STEP3_MS)
            {
                g_pattern_elapsed_ms = 0U;
                g_pattern_step = 0U;
            }
            break;
    }
}

/* ----------------------------
 * Public API
 * ---------------------------- */
void alert_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK;

    /* Buzzer output */
    ALERT_PORT->PCR[BUZZER_PIN] = PORT_PCR_MUX(1);
    ALERT_GPIO->PDDR |= (1UL << BUZZER_PIN);
    buzzer_apply(false);

    /* Ack button input with pull-up */
    ALERT_PORT->PCR[ACK_BUTTON_PIN] =
        PORT_PCR_MUX(1) |
        PORT_PCR_PE_MASK |
        PORT_PCR_PS_MASK;

    ALERT_GPIO->PDDR &= ~(1UL << ACK_BUTTON_PIN);

    alert_runtime_reset();
}

void alert_runtime_reset(void)
{
    bool raw_now = ack_button_raw_pressed();

    g_active_alert = ALERT_NONE;
    g_acknowledged_type = ALERT_NONE;

    g_ack_button.stable_pressed = raw_now;
    g_ack_button.last_raw_pressed = raw_now;
    g_ack_button.last_change_ms = 0U;

    g_pattern_step = 0U;
    g_pattern_elapsed_ms = 0U;
    g_last_update_ms = 0U;

    g_prev_motion_input = false;
    g_prev_fall_input = false;
    g_prev_temp_input = false;
    g_inputs_initialized = false;

    g_fall_wait_for_clear = false;

    buzzer_apply(false);
}

/*
 * Updated signature:
 * alert_update(now_ms, fall_detected_input, unusual_motion_input, temp_alert_input)
 */
void alert_update(uint32_t now_ms,
                  bool fall_detected_input,
                  bool unusual_motion_input,
                  bool temp_alert_input)
{
    uint32_t delta_ms;
    bool fall_rising;
    bool motion_rising;
    bool temp_rising;
    bool buzzer_on = false;

    if (g_last_update_ms == 0U)
    {
        g_last_update_ms = now_ms;
    }

    delta_ms = now_ms - g_last_update_ms;
    g_last_update_ms = now_ms;

    if (!g_inputs_initialized)
    {
        g_prev_fall_input = fall_detected_input;
        g_prev_motion_input = unusual_motion_input;
        g_prev_temp_input = temp_alert_input;
        g_inputs_initialized = true;
    }

    /* Once fall input clears, allow future fall again */
    if (!fall_detected_input)
    {
        g_fall_wait_for_clear = false;
    }

    /* ACK current alert */
    if (ack_button_pressed_event(now_ms))
    {
        if (g_active_alert != ALERT_NONE)
        {
            g_acknowledged_type = g_active_alert;

            if (g_active_alert == ALERT_FALL)
            {
                g_fall_wait_for_clear = true;
            }

            alert_stop();
        }
    }

    fall_rising = (fall_detected_input && !g_prev_fall_input);
    motion_rising = (unusual_motion_input && !g_prev_motion_input);
    temp_rising = (temp_alert_input && !g_prev_temp_input);

    g_prev_fall_input = fall_detected_input;
    g_prev_motion_input = unusual_motion_input;
    g_prev_temp_input = temp_alert_input;

    /* Priority: fall > temperature > unusual motion */
    if (fall_rising && !g_fall_wait_for_clear)
    {
        alert_start(ALERT_FALL);
    }
    else if (temp_rising)
    {
        if (g_active_alert == ALERT_NONE)
        {
            alert_start(ALERT_TEMPERATURE);
        }
    }
    else if (motion_rising)
    {
        if (g_active_alert == ALERT_NONE)
        {
            alert_start(ALERT_UNUSUAL_MOTION);

            /*
             * Extend normal motion-light hold to 10 seconds.
             * Keeps PWM behavior normal.
             */
            pir_force_light_hold(UNUSUAL_MOTION_LED_HOLD_MS, now_ms);
        }
    }

    /* Keep fall active while fall input is present */
    if ((g_active_alert == ALERT_FALL) && !fall_detected_input)
    {
        alert_stop();
        return;
    }

    /* Keep temperature alert active while temp alert input is present */
    if ((g_active_alert == ALERT_TEMPERATURE) && !temp_alert_input)
    {
        alert_stop();
        return;
    }

    /*
     * Unusual motion is edge-triggered, so it keeps running until ack/clear,
     * same as your earlier behavior.
     */

    if (g_active_alert == ALERT_NONE)
    {
        buzzer_apply(false);
        return;
    }

    g_pattern_elapsed_ms += delta_ms;

    if (g_active_alert == ALERT_FALL)
    {
        buzzer_on = alert_compute_fall_output();
        buzzer_apply(buzzer_on);
        alert_advance_fall_pattern();
    }
    else
    {
        /* temperature + unusual motion use same non-fall pattern */
        buzzer_on = alert_compute_nonfall_output();
        buzzer_apply(buzzer_on);
        alert_advance_nonfall_pattern();
    }
}

void alert_clear(void)
{
    alert_stop();
    g_acknowledged_type = ALERT_NONE;
    g_fall_wait_for_clear = false;
}

alert_type_t alert_get_active(void)
{
    return g_active_alert;
}

bool alert_is_active(void)
{
    return (g_active_alert != ALERT_NONE);
}

alert_type_t alert_take_acknowledged_type(void)
{
    alert_type_t t = g_acknowledged_type;
    g_acknowledged_type = ALERT_NONE;
    return t;
}

void alert_acknowledge_from_software(void)
{
    if (g_active_alert != ALERT_NONE)
    {
        g_acknowledged_type = g_active_alert;

        if (g_active_alert == ALERT_FALL)
        {
            g_fall_wait_for_clear = true;
        }

        alert_stop();
    }
}

bool alert_is_buzzer_output_on(void)
{
    return g_buzzer_output_on;
}
