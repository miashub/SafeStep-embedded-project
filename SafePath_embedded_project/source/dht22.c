/* ------------ dht22.c -------------------- */

#include "dht22.h"
#include "MK66F18.h"
#include "timing.h"
#include <stdint.h>

/* ----------------------------
 * Pin Definitions
 * ---------------------------- */
#define DHT_PORT        PORTB
#define DHT_GPIO        GPIOB
#define DHT_PIN         10U

/* ----------------------------
 * Internal helper
 * Wait until pin becomes 'state'
 * Returns elapsed time in us
 * Returns -1 on timeout
 * ---------------------------- */
static int dht_wait_pin(int state, int timeout_us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t timeout_ticks = (uint32_t)timeout_us * timing_get_core_mhz();

    while (((DHT_GPIO->PDIR >> DHT_PIN) & 1U) != (uint32_t)state)
    {
        if ((DWT->CYCCNT - start) > timeout_ticks)
        {
            return -1;
        }
    }

    return (int)((DWT->CYCCNT - start) / timing_get_core_mhz());
}

/* ----------------------------
 * Init
 * Default to GPIO input with pull-up
 * ---------------------------- */
void dht22_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;

    DHT_PORT->PCR[DHT_PIN] =
        PORT_PCR_MUX(1) |
        PORT_PCR_PE_MASK |
        PORT_PCR_PS_MASK;

    /* input mode by default */
    DHT_GPIO->PDDR &= ~(1U << DHT_PIN);
}

/* ----------------------------
 * Read DHT22
 * temperature_x10 = temperature * 10
 * humidity_x10    = humidity * 10
 *
 * Returns:
 *   0  = success
 *  -1  = timeout / protocol error
 *  -2  = checksum error
 *  -3  = null pointer
 * ---------------------------- */
int read_dht22(int *temperature_x10, int *humidity_x10)
{
    uint8_t data[5] = {0U, 0U, 0U, 0U, 0U};
    uint8_t checksum;
    int temp_raw;

    if ((temperature_x10 == 0) || (humidity_x10 == 0))
    {
        return -3;
    }

    /* Start signal:
     * MCU drives line low for at least 1 ms
     */
    DHT_GPIO->PDDR |= (1U << DHT_PIN);   /* output */
    DHT_GPIO->PCOR = (1U << DHT_PIN);    /* drive low */

    delay_us(2000U);

    /* Release line and let pull-up bring it high */
    DHT_GPIO->PSOR = (1U << DHT_PIN);
    DHT_GPIO->PDDR &= ~(1U << DHT_PIN);  /* input */

    /* Sensor response sequence */
    if (dht_wait_pin(1, 100) == -1) return -1;
    if (dht_wait_pin(0, 250) == -1) return -1;
    if (dht_wait_pin(1, 250) == -1) return -1;
    if (dht_wait_pin(0, 250) == -1) return -1;

    /* Read 40 bits */
    for (int i = 0; i < 40; i++)
    {
        int time_high;

        if (dht_wait_pin(1, 150) == -1) return -1;

        time_high = dht_wait_pin(0, 150);
        if (time_high == -1) return -1;

        /* DHT22:
         * shorter high pulse -> 0
         * longer high pulse  -> 1
         */
        if (time_high > 40)
        {
            data[i / 8] |= (uint8_t)(1U << (7 - (i % 8)));
        }
    }

    checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (data[4] != checksum)
    {
        return -2;
    }

    *humidity_x10 = (((int)data[0]) << 8) | data[1];

    temp_raw = (((int)data[2]) << 8) | data[3];
    *temperature_x10 = temp_raw & 0x7FFF;

    if ((temp_raw & 0x8000) != 0)
    {
        *temperature_x10 = -(*temperature_x10);
    }

    return 0;
}
