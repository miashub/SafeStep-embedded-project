#include "rtc.h"

/* ----------------------------
 * Private Helpers
 * ---------------------------- */
static uint8_t bcd_to_dec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10U) + (val & 0x0FU));
}

static uint8_t dec_to_bcd(uint8_t val)
{
    return (uint8_t)(((val / 10U) << 4) | (val % 10U));
}

static void i2c_wait(void)
{
    uint32_t timeout = 50000U;

    while (((RTC_I2C->S & I2C_S_IICIF_MASK) == 0U) && timeout)
    {
        timeout--;
    }

    RTC_I2C->S = I2C_S_IICIF_MASK;
}

static void i2c_start(void)
{
    RTC_I2C->C1 |= (I2C_C1_TX_MASK | I2C_C1_MST_MASK);
}

static void i2c_stop(void)
{
    RTC_I2C->C1 &= ~(I2C_C1_MST_MASK | I2C_C1_TX_MASK);

    for (volatile int i = 0; i < 500; i++)
    {
        /* small delay */
    }
}

static void i2c_write_byte(uint8_t data)
{
    RTC_I2C->D = data;
    i2c_wait();
}

/* ----------------------------
 * Public RTC Functions
 * ---------------------------- */
bool rtc_lost_power(void)
{
    i2c_start();
    i2c_write_byte((uint8_t)((DS3231_ADDR << 1) | 0U));
    i2c_write_byte(0x0F);

    RTC_I2C->C1 |= I2C_C1_RSTA_MASK;
    i2c_write_byte((uint8_t)((DS3231_ADDR << 1) | 1U));

    RTC_I2C->C1 &= ~I2C_C1_TX_MASK;
    RTC_I2C->C1 |= I2C_C1_TXAK_MASK;

    uint8_t dummy = RTC_I2C->D;
    (void)dummy;
    i2c_wait();

    i2c_stop();
    uint8_t status = RTC_I2C->D;

    return ((status & 0x80U) != 0U);
}

void rtc_clear_osf(void)
{
    i2c_start();
    i2c_write_byte((uint8_t)((DS3231_ADDR << 1) | 0U));
    i2c_write_byte(0x0F);
    i2c_write_byte(0x00);
    i2c_stop();
}

void set_rtc(uint8_t h, uint8_t m, uint8_t s)
{
    i2c_start();
    i2c_write_byte((uint8_t)((DS3231_ADDR << 1) | 0U));
    i2c_write_byte(0x00);
    i2c_write_byte(dec_to_bcd(s));
    i2c_write_byte(dec_to_bcd(m));
    i2c_write_byte(dec_to_bcd(h));
    i2c_stop();
}

void rtc_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK;
    SIM->SCGC4 |= SIM_SCGC4_I2C1_MASK;

    RTC_PORT->PCR[RTC_SCL_PIN] =
        PORT_PCR_MUX(2) | PORT_PCR_ODE_MASK | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;

    RTC_PORT->PCR[RTC_SDA_PIN] =
        PORT_PCR_MUX(2) | PORT_PCR_ODE_MASK | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;

    RTC_I2C->F = 0x35;
    RTC_I2C->C1 = I2C_C1_IICEN_MASK;

    if (rtc_lost_power())
    {
        const char *time_str = __TIME__;

        uint8_t h_tens = (time_str[0] == ' ') ? 0U : (uint8_t)(time_str[0] - '0');
        uint8_t h = (uint8_t)(h_tens * 10U + (uint8_t)(time_str[1] - '0'));
        uint8_t m = (uint8_t)((time_str[3] - '0') * 10U + (uint8_t)(time_str[4] - '0'));
        uint8_t s = (uint8_t)((time_str[6] - '0') * 10U + (uint8_t)(time_str[7] - '0'));

        set_rtc(h, m, s);
        rtc_clear_osf();
    }
}

int read_rtc(uint8_t *h, uint8_t *m, uint8_t *s)
{
    i2c_start();
    i2c_write_byte((uint8_t)((DS3231_ADDR << 1) | 0U));
    i2c_write_byte(0x00);

    RTC_I2C->C1 |= I2C_C1_RSTA_MASK;
    i2c_write_byte((uint8_t)((DS3231_ADDR << 1) | 1U));

    RTC_I2C->C1 &= ~I2C_C1_TX_MASK;
    RTC_I2C->C1 &= ~I2C_C1_TXAK_MASK;

    uint8_t dummy = RTC_I2C->D;
    (void)dummy;
    i2c_wait();

    uint8_t sec_bcd = RTC_I2C->D;
    i2c_wait();

    RTC_I2C->C1 |= I2C_C1_TXAK_MASK;
    uint8_t min_bcd = RTC_I2C->D;
    i2c_wait();

    i2c_stop();
    uint8_t hour_bcd = RTC_I2C->D;

    *s = bcd_to_dec((uint8_t)(sec_bcd & 0x7FU));
    *m = bcd_to_dec((uint8_t)(min_bcd & 0x7FU));
    *h = bcd_to_dec((uint8_t)(hour_bcd & 0x3FU));

    return 0;
}
