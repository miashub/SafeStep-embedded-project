#include "pressure.h"
#include "MK66F18.h"
#include <stdbool.h>
#include <stdint.h>

/* ----------------------------
 * Hardware config
 * ---------------------------- */
#define FSR_PORT        PORTB
#define FSR_PIN         7
#define FSR_ADC         ADC1
#define FSR_ADC_CH      13

/* ----------------------------
 * Tuning parameters
 * ---------------------------- */
#define PRESSURE_BASELINE_SAMPLES        40U

/* Mat is considered clear near baseline */
#define MAT_CLEAR_MARGIN                 35U

/* ----------------------------
 * Bed-exit tuning (RAW based)
 * ---------------------------- */
#define BED_EXIT_STEP_THRESHOLD          220U
#define BED_EXIT_DERIV_THRESHOLD         70
#define BED_EXIT_COOLDOWN_MS             60000U

/* ----------------------------
 * Fall tuning (RAW based, less strict)
 * ---------------------------- */
#define FALL_STEP_THRESHOLD              800U
#define FALL_DERIV_THRESHOLD             90
#define FALL_CONFIRM_MS                  250U
#define FALL_IGNORE_AFTER_BED_EXIT_MS    1500U

typedef enum
{
    PRESSURE_IDLE = 0,
    PRESSURE_WAIT_CONFIRM_FALL
} pressure_state_t;

/* ----------------------------
 * Runtime state
 * ---------------------------- */
static volatile uint16_t g_raw = 0;
static volatile uint16_t g_baseline = 0;

/* We keep these names for compatibility with your main */
static volatile uint16_t g_filtered = 0;
static volatile int16_t  g_derivative = 0;
static volatile int16_t  g_raw_derivative = 0;

static volatile bool g_mat_clear = true;
static volatile bool g_bed_exit_event = false;
static volatile bool g_fall_detected = false;
static volatile bool g_bed_exit_enabled = false;

static pressure_state_t g_state = PRESSURE_IDLE;

static uint16_t prev_raw = 0;
static uint32_t clear_since_ms = 0U;

/* bed-exit control */
static bool g_bed_exit_armed = false;
static uint32_t g_bed_exit_cooldown_until = 0U;

/* fall control */
static uint32_t fall_candidate_start_ms = 0U;
static uint32_t g_fall_ignore_until = 0U;

/* ----------------------------
 * ADC low-level
 * ---------------------------- */
static void fsr_hw_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;
    SIM->SCGC3 |= SIM_SCGC3_ADC1_MASK;

    FSR_PORT->PCR[FSR_PIN] = PORT_PCR_MUX(0);

    FSR_ADC->CFG1 = ADC_CFG1_MODE(1); /* 12-bit */
    FSR_ADC->SC2 = 0;
    FSR_ADC->SC3 = ADC_SC3_AVGE_MASK | ADC_SC3_AVGS(3);
}

static uint16_t fsr_read_raw_hw(void)
{
    FSR_ADC->SC1[0] = FSR_ADC_CH;

    while (!(FSR_ADC->SC1[0] & ADC_SC1_COCO_MASK))
    {
        /* wait */
    }

    return FSR_ADC->R[0];
}

/* ----------------------------
 * Public API
 * ---------------------------- */
void pressure_init(void)
{
    fsr_hw_init();

    uint32_t baseline_sum = 0U;

    for (uint32_t i = 0; i < PRESSURE_BASELINE_SAMPLES; i++)
    {
        uint16_t s = fsr_read_raw_hw();
        baseline_sum += s;

        for (volatile uint32_t d = 0; d < 30000U; d++)
        {
            /* small delay */
        }
    }

    g_baseline = (uint16_t)(baseline_sum / PRESSURE_BASELINE_SAMPLES);

    g_raw = g_baseline;
    g_filtered = g_baseline;

    prev_raw = g_baseline;
    g_raw_derivative = 0;
    g_derivative = 0;

    g_mat_clear = true;
    g_bed_exit_event = false;
    g_fall_detected = false;
    g_bed_exit_enabled = false;

    g_bed_exit_armed = false;
    g_bed_exit_cooldown_until = 0U;

    g_fall_ignore_until = 0U;
    fall_candidate_start_ms = 0U;

    g_state = PRESSURE_IDLE;
    clear_since_ms = 0U;
}

void pressure_runtime_reset(void)
{
    g_bed_exit_event = false;
    g_fall_detected = false;

    g_bed_exit_armed = false;
    g_bed_exit_cooldown_until = 0U;

    g_fall_ignore_until = 0U;
    fall_candidate_start_ms = 0U;

    g_state = PRESSURE_IDLE;
    clear_since_ms = 0U;

    prev_raw = g_raw;
    g_raw_derivative = 0;
    g_derivative = 0;
}

void pressure_enable_bed_exit(bool enable)
{
    g_bed_exit_enabled = enable;

    if (!enable)
    {
        g_bed_exit_event = false;
        g_bed_exit_armed = false;
        clear_since_ms = 0U;
    }
}

void pressure_update(uint32_t now_ms)
{
    g_bed_exit_event = false;

    /* Read raw pressure */
    g_raw = fsr_read_raw_hw();
    g_filtered = g_raw;

    g_raw_derivative = (int16_t)g_raw - (int16_t)prev_raw;
    g_derivative = g_raw_derivative;
    prev_raw = g_raw;

    bool is_clear = (g_raw <= (uint16_t)(g_baseline + MAT_CLEAR_MARGIN));
    g_mat_clear = is_clear;

    if (is_clear)
    {
        if (clear_since_ms == 0U)
        {
            clear_since_ms = now_ms;
        }

        if ((now_ms - clear_since_ms) >= 200U)
        {
            g_bed_exit_armed = true;
        }
    }
    else
    {
        clear_since_ms = 0U;
    }

    /* ----------------------------
     * FALL detection FIRST
     * ---------------------------- */
    if ((int32_t)(now_ms - g_fall_ignore_until) >= 0)
    {
        switch (g_state)
        {
            case PRESSURE_IDLE:
                if (!is_clear &&
                    (g_raw >= (uint16_t)(g_baseline + FALL_STEP_THRESHOLD)) &&
                    (g_raw_derivative >= FALL_DERIV_THRESHOLD))
                {
                    g_state = PRESSURE_WAIT_CONFIRM_FALL;
                    fall_candidate_start_ms = now_ms;
                }
                break;

            case PRESSURE_WAIT_CONFIRM_FALL:
                if (!is_clear &&
                    (g_raw >= (uint16_t)(g_baseline + FALL_STEP_THRESHOLD)))
                {
                    if ((now_ms - fall_candidate_start_ms) >= FALL_CONFIRM_MS)
                    {
                        g_fall_detected = true;
                        g_state = PRESSURE_IDLE;
                        fall_candidate_start_ms = 0U;
                    }
                }
                else
                {
                    g_state = PRESSURE_IDLE;
                    fall_candidate_start_ms = 0U;
                }
                break;

            default:
                g_state = PRESSURE_IDLE;
                fall_candidate_start_ms = 0U;
                break;
        }
    }

    /* If fall is active, do not also create bed-exit from same impact */
    if (g_fall_detected)
    {
        return;
    }

    /* ----------------------------
     * Bed-exit detection
     * Only if signal is NOT a fall-level impact
     * ---------------------------- */
    if (g_bed_exit_enabled &&
        g_bed_exit_armed &&
        ((int32_t)(now_ms - g_bed_exit_cooldown_until) >= 0) &&
        !is_clear &&
        (g_raw >= (uint16_t)(g_baseline + BED_EXIT_STEP_THRESHOLD)) &&
        (g_raw_derivative >= BED_EXIT_DERIV_THRESHOLD) &&
        (g_raw < (uint16_t)(g_baseline + FALL_STEP_THRESHOLD)))
    {
        g_bed_exit_event = true;
        g_bed_exit_armed = false;
        g_bed_exit_cooldown_until = now_ms + BED_EXIT_COOLDOWN_MS;
    }
}


uint16_t pressure_get_raw(void)
{
    return g_raw;
}

uint16_t pressure_get_filtered(void)
{
    return g_filtered; /* now same as raw, kept for compatibility */
}

uint16_t pressure_get_baseline(void)
{
    return g_baseline;
}

int16_t pressure_get_derivative(void)
{
    return g_derivative;
}

int16_t pressure_get_raw_derivative(void)
{
    return g_raw_derivative;
}

bool pressure_is_mat_clear(void)
{
    return g_mat_clear;
}

bool pressure_bed_exit_event(void)
{
    return g_bed_exit_event;
}

bool pressure_fall_detected(void)
{
    return g_fall_detected;
}

void pressure_clear_fall_flag(void)
{
    g_fall_detected = false;
}
