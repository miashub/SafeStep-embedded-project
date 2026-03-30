/* ------------pir.c-------------------- */

#include "pir.h"
#include "MK66F18.h"
#include <stdbool.h>
#include <stdint.h>

/* ----------------------------
 * Pin Configuration
 * ---------------------------- */
#define PIR_PORT    PORTC
#define PIR_GPIO    GPIOC
#define PIR_PIN     12U

/* ----------------------------
 * Timing Config
 * ---------------------------- */
#define PIR_LIGHT_HOLD_MS        5000U
#define PIR_EDGE_WINDOW_MS       20000U
#define PIR_UNUSUAL_EDGE_COUNT   5U

/* ----------------------------
 * Internal State
 * ---------------------------- */
static uint8_t pir_state = 0;
static uint8_t pir_last = 0;

static uint32_t last_rising_edge_ms = 0;
static uint32_t edge_times[10];
static uint8_t edge_index = 0;

static uint32_t light_hold_until = 0;

/* motion stats */
static uint32_t motion_start_ms = 0;
static uint32_t total_motion_time = 0;
static uint8_t motion_events = 0;

/* simulation */
static bool sim_active = false;
static uint32_t sim_end_ms = 0;

/* unusual motion */
static bool unusual_flag = false;

/* ----------------------------
 * Init
 * ---------------------------- */
void pir_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK;
    PIR_PORT->PCR[PIR_PIN] = PORT_PCR_MUX(1);

    PIR_GPIO->PDDR &= ~(1U << PIR_PIN);
}

/* ----------------------------
 * Raw Read
 * ---------------------------- */
uint8_t read_pir(void)
{
    uint8_t real = (PIR_GPIO->PDIR >> PIR_PIN) & 1U;

    if (sim_active)
        return 1U;

    return real;
}

/* ----------------------------
 * Update (call in main loop)
 * ---------------------------- */
void pir_update(uint32_t now_ms)
{
    /* handle simulation timeout */
    if (sim_active && now_ms >= sim_end_ms)
    {
        sim_active = false;
    }

    pir_state = read_pir();

    /* rising edge */
    if (pir_state && !pir_last)
    {
        last_rising_edge_ms = now_ms;

        /* store edge for unusual detection */
        edge_times[edge_index++] = now_ms;
        if (edge_index >= PIR_UNUSUAL_EDGE_COUNT)
            edge_index = 0;

        motion_events++;

        /* start motion duration */
        motion_start_ms = now_ms;

        /* extend light hold */
        light_hold_until = now_ms + PIR_LIGHT_HOLD_MS;
    }

    /* falling edge */
    if (!pir_state && pir_last)
    {
        if (motion_start_ms > 0)
        {
            total_motion_time += (now_ms - motion_start_ms);
            motion_start_ms = 0;
        }
    }

    /* check unusual motion */
    uint8_t count = 0;
    for (uint8_t i = 0; i < PIR_UNUSUAL_EDGE_COUNT; i++)
    {
        if ((now_ms - edge_times[i]) <= PIR_EDGE_WINDOW_MS)
            count++;
    }

    unusual_flag = (count >= PIR_UNUSUAL_EDGE_COUNT);

    pir_last = pir_state;
}

/* ----------------------------
 * Unusual Motion
 * ---------------------------- */
bool pir_unusual_motion_event(void)
{
    if (unusual_flag)
    {
        unusual_flag = false;
        return true;
    }
    return false;
}

bool pir_peek_unusual_motion(void)
{
    return unusual_flag;
}

/* ----------------------------
 * Light Hold
 * ---------------------------- */
bool pir_light_hold_active(uint32_t now_ms)
{
    return (now_ms < light_hold_until);
}

void pir_force_light_hold(uint32_t hold_ms, uint32_t now_ms)
{
    light_hold_until = now_ms + hold_ms;
}

/* ----------------------------
 * Motion Stats (ML)
 * ---------------------------- */
uint32_t pir_debug_current_high_ms(uint32_t now_ms)
{
    if (pir_state && motion_start_ms > 0)
        return (now_ms - motion_start_ms);

    return 0;
}

uint32_t pir_debug_window_high_ms(uint32_t now_ms)
{
    (void)now_ms;
    return total_motion_time;
}

uint32_t pir_debug_cooldown_left(uint32_t now_ms)
{
    if (light_hold_until > now_ms)
        return (light_hold_until - now_ms);

    return 0;
}

uint8_t pir_debug_high_count(void)
{
    return motion_events;
}

/* ----------------------------
 * Digital Twin Simulation
 * ---------------------------- */
void pir_simulate_motion_pulse(uint32_t now_ms, uint32_t duration_ms)
{
    sim_active = true;
    sim_end_ms = now_ms + duration_ms;

    /* behave like real trigger */
    motion_events++;
    motion_start_ms = now_ms;
    light_hold_until = now_ms + PIR_LIGHT_HOLD_MS;
}

bool pir_simulation_active(void)
{
    return sim_active;
}
