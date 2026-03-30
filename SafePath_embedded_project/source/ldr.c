/* ------------ ldr.c --------------------
 * LDR sensor module
 * Reads ambient light level through ADC and updates
 * the cached raw value and dark/light state.
 */

#include "MK66F18.h"
#include <stdbool.h>
#include <stdint.h>

#include "ldr.h"
#include "nv_storage.h"

/* LDR hardware configuration */
#define LDR_PORT    PORTB
#define LDR_PIN     6U
#define LDR_ADC     ADC1
#define LDR_ADC_CH  12U

/* Latest LDR state */
static uint16_t g_ldr_raw = 0U;
static bool g_ldr_dark = false;

/* Set up the LDR pin for analog input */
void ldr_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;
    LDR_PORT->PCR[LDR_PIN] = PORT_PCR_MUX(0);

    g_ldr_raw = 0U;
    g_ldr_dark = false;
}

/* Read one ADC sample from the LDR */
uint16_t read_ldr(void)
{
    LDR_ADC->SC1[0] = LDR_ADC_CH;

    while ((LDR_ADC->SC1[0] & ADC_SC1_COCO_MASK) == 0U)
    {
        /* wait for conversion to complete */
    }

    return (uint16_t)LDR_ADC->R[0];
}

/* Refresh the stored LDR reading and dark/light status */
void ldr_update(void)
{
    const uint16_t threshold = (uint16_t)nv_get_ldr_threshold();

    g_ldr_raw = read_ldr();
    g_ldr_dark = (g_ldr_raw <= threshold);
}

/* Return the most recent raw ADC value */
uint16_t ldr_get_raw(void)
{
    return g_ldr_raw;
}

/* Return true when the current reading is below the threshold */
bool ldr_is_dark(void)
{
    return g_ldr_dark;
}
