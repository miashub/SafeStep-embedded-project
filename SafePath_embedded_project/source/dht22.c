#include "dht22.h"
#include "timing.h"

/* ----------------------------
 * Pin Definitions
 * ---------------------------- */
#define DHT_PORT        PORTB
#define DHT_GPIO        GPIOB
#define DHT_PIN         10

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

void dht22_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;
    DHT_PORT->PCR[DHT_PIN] = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;
}

int read_dht22(int *temperature_x10, int *humidity_x10)
{
    uint8_t data[5] = {0};

    DHT_GPIO->PDDR |= (1U << DHT_PIN);
    DHT_GPIO->PCOR = (1U << DHT_PIN);

    delay_us(2000);

    DHT_GPIO->PDDR &= ~(1U << DHT_PIN);

    if (dht_wait_pin(1, 100) == -1) return -1;
    if (dht_wait_pin(0, 250) == -1) return -1;
    if (dht_wait_pin(1, 250) == -1) return -1;
    if (dht_wait_pin(0, 250) == -1) return -1;

    for (int i = 0; i < 40; i++)
    {
        if (dht_wait_pin(1, 150) == -1) return -1;

        int time_high = dht_wait_pin(0, 150);
        if (time_high == -1) return -1;

        if (time_high > 40)
        {
            data[i / 8] |= (uint8_t)(1U << (7 - (i % 8)));
        }
    }

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (data[4] != checksum) return -2;

    *humidity_x10 = ((int)data[0] << 8) | data[1];

    int temp = ((int)data[2] << 8) | data[3];
    *temperature_x10 = temp & 0x7FFF;
    if (temp & 0x8000)
    {
        *temperature_x10 *= -1;
    }

    return 0;
}
