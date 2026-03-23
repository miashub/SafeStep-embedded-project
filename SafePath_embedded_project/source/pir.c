#include "MK66F18.h"
#include "pir.h"

#define PIR_PORT    PORTC
#define PIR_GPIO    GPIOC
#define PIR_PIN     12

#define PIR_EDGE_WINDOW_MS         20000U   /* 20 sec */
#define PIR_UNUSUAL_EDGE_COUNT     5U
#define PIR_LIGHT_HOLD_MS          5000U    /* keep light on 5 sec */

static uint8_t  g_prev_pir = 0U;
static bool     g_initialized = false;

static uint32_t g_rise_times[PIR_UNUSUAL_EDGE_COUNT] = {0};
static uint8_t  g_rise_index = 0U;
static uint8_t  g_rise_count = 0U;

static bool     g_unusual_motion_event = false;
static uint32_t g_light_hold_until = 0U;

void pir_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK;

    PIR_PORT->PCR[PIR_PIN] = PORT_PCR_MUX(1);
    PIR_GPIO->PDDR &= ~(1U << PIR_PIN);

    g_prev_pir = read_pir();
    g_initialized = true;

    g_rise_index = 0U;
    g_rise_count = 0U;
    g_unusual_motion_event = false;
    g_light_hold_until = 0U;
}

uint8_t read_pir(void)
{
    return (uint8_t)((PIR_GPIO->PDIR >> PIR_PIN) & 0x01U);
}

void pir_update(uint32_t now_ms)
{
    uint8_t pir_now = read_pir();

    if (!g_initialized)
    {
        g_prev_pir = pir_now;
        g_initialized = true;
    }

    /* If PIR is currently high, keep light on for 5 sec from now */
    if (pir_now)
    {
        g_light_hold_until = now_ms + PIR_LIGHT_HOLD_MS;
    }

    /* Rising edge detection */
    if ((pir_now == 1U) && (g_prev_pir == 0U))
    {
        g_light_hold_until = now_ms + PIR_LIGHT_HOLD_MS;

        g_rise_times[g_rise_index] = now_ms;
        g_rise_index = (uint8_t)((g_rise_index + 1U) % PIR_UNUSUAL_EDGE_COUNT);

        if (g_rise_count < PIR_UNUSUAL_EDGE_COUNT)
        {
            g_rise_count++;
        }

        if (g_rise_count >= PIR_UNUSUAL_EDGE_COUNT)
        {
            uint8_t oldest_index = g_rise_index; /* next write position = oldest */
            uint32_t oldest_time = g_rise_times[oldest_index];

            if ((now_ms - oldest_time) <= PIR_EDGE_WINDOW_MS)
            {
                g_unusual_motion_event = true;

                /* reset counter after event so it becomes one clean event */
                g_rise_count = 0U;
                g_rise_index = 0U;
            }
        }
    }

    g_prev_pir = pir_now;
}

bool pir_unusual_motion_event(void)
{
    bool event = g_unusual_motion_event;
    g_unusual_motion_event = false;
    return event;
}

bool pir_light_hold_active(uint32_t now_ms)
{
    return ((int32_t)(g_light_hold_until - now_ms) > 0);
}
