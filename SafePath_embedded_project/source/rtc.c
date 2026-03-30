/* --------------------------------------------------------------------------
 *  File: rtc.c
 *
 *  Module: Real-Time Clock (DS3231 via I2C)
 *
 *  Description:
 *  This module provides initialization and access functions for the DS3231
 *  RTC module. It supports reading/writing time, handling power loss,
 *  and initializing the RTC using compile-time date and time.
 *
 *  Features:
 *   - I2C communication with DS3231
 *   - BCD ↔ Decimal conversions
 *   - Automatic time recovery after power loss
 *   - Safe register read/write abstraction
 *
 *  AI Usage Declaration:
 *  AI assistance was used to improve documentation, comment clarity,
 *  and structure. The RTC logic, register handling, and integration
 *  were implemented and verified manually.
 * -------------------------------------------------------------------------- */

#include "rtc.h"
#include <string.h>

/* ==========================================================================
 *  Private Helper Functions
 * ========================================================================== */

/**
 * @brief Convert BCD (Binary-Coded Decimal) to decimal.
 */
static uint8_t bcd_to_dec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10U) + (val & 0x0FU));
}

/**
 * @brief Convert decimal to BCD format.
 */
static uint8_t dec_to_bcd(uint8_t val)
{
    return (uint8_t)(((val / 10U) << 4) | (val % 10U));
}

/**
 * @brief Convert month string (e.g., "Mar") to numeric value.
 */
static uint8_t month_from_str(const char *m)
{
    if (strncmp(m, "Jan", 3U) == 0) return 1U;
    if (strncmp(m, "Feb", 3U) == 0) return 2U;
    if (strncmp(m, "Mar", 3U) == 0) return 3U;
    if (strncmp(m, "Apr", 3U) == 0) return 4U;
    if (strncmp(m, "May", 3U) == 0) return 5U;
    if (strncmp(m, "Jun", 3U) == 0) return 6U;
    if (strncmp(m, "Jul", 3U) == 0) return 7U;
    if (strncmp(m, "Aug", 3U) == 0) return 8U;
    if (strncmp(m, "Sep", 3U) == 0) return 9U;
    if (strncmp(m, "Oct", 3U) == 0) return 10U;
    if (strncmp(m, "Nov", 3U) == 0) return 11U;
    if (strncmp(m, "Dec", 3U) == 0) return 12U;
    return 1U;
}

/**
 * @brief Wait for I2C transfer completion with timeout.
 */
static void i2c_wait(void)
{
    uint32_t timeout = 50000U;

    while (((RTC_I2C->S & I2C_S_IICIF_MASK) == 0U) && timeout)
    {
        timeout--;
    }

    RTC_I2C->S = I2C_S_IICIF_MASK;
}

/**
 * @brief Generate I2C START condition.
 */
static void i2c_start(void)
{
    RTC_I2C->C1 |= (I2C_C1_TX_MASK | I2C_C1_MST_MASK);
}

/**
 * @brief Generate I2C STOP condition.
 */
static void i2c_stop(void)
{
    RTC_I2C->C1 &= ~(I2C_C1_MST_MASK | I2C_C1_TX_MASK);

    for (volatile int i = 0; i < 500; i++)
    {
        /* small delay for bus stabilization */
    }
}

/**
 * @brief Write a single byte over I2C.
 */
static void i2c_write_byte(uint8_t data)
{
    RTC_I2C->D = data;
    i2c_wait();
}

/* ==========================================================================
 *  Low-Level RTC Register Access
 * ========================================================================== */
/* AI-assisted: improved documentation clarity for I2C register access */

/**
 * @brief Read a register from DS3231 RTC.
 */
static uint8_t rtc_read_register(uint8_t reg)
{
    i2c_start();
    i2c_write_byte((uint8_t)((DS3231_ADDR << 1) | 0U));
    i2c_write_byte(reg);

    RTC_I2C->C1 |= I2C_C1_RSTA_MASK;
    i2c_write_byte((uint8_t)((DS3231_ADDR << 1) | 1U));

    RTC_I2C->C1 &= ~I2C_C1_TX_MASK;
    RTC_I2C->C1 |= I2C_C1_TXAK_MASK;

    (void)RTC_I2C->D;
    i2c_wait();

    i2c_stop();
    return RTC_I2C->D;
}

/**
 * @brief Write a value to a DS3231 register.
 */
static void rtc_write_register(uint8_t reg, uint8_t value)
{
    i2c_start();
    i2c_write_byte((uint8_t)((DS3231_ADDR << 1) | 0U));
    i2c_write_byte(reg);
    i2c_write_byte(value);
    i2c_stop();
}

/**
 * @brief Ensure RTC oscillator runs during battery backup.
 */
static void rtc_enable_oscillator(void)
{
    uint8_t ctrl = rtc_read_register(0x0E);
    ctrl &= (uint8_t)~0x80U;
    rtc_write_register(0x0E, ctrl);
}

/* ==========================================================================
 *  Initialization Helpers
 * ========================================================================== */

/**
 * @brief Set RTC using firmware build date/time.
 *
 * AI-assisted: improved parsing explanation for clarity.
 */
static void rtc_set_from_build_datetime(void)
{
    const char *date_str = __DATE__;
    const char *time_str = __TIME__;

    rtc_datetime_t dt;

    dt.month = month_from_str(date_str);

    dt.day = (date_str[4] == ' ') ?
             (uint8_t)(date_str[5] - '0') :
             (uint8_t)(((date_str[4] - '0') * 10U) + (date_str[5] - '0'));

    dt.year =
        (uint16_t)((date_str[7] - '0') * 1000U +
                   (date_str[8] - '0') * 100U +
                   (date_str[9] - '0') * 10U +
                   (date_str[10] - '0'));

    dt.hour   = (uint8_t)(((time_str[0] - '0') * 10U) + (time_str[1] - '0'));
    dt.minute = (uint8_t)(((time_str[3] - '0') * 10U) + (time_str[4] - '0'));
    dt.second = (uint8_t)(((time_str[6] - '0') * 10U) + (time_str[7] - '0'));

    rtc_set_datetime(&dt);
}

/* ==========================================================================
 *  Public RTC Functions
 * ========================================================================== */

/**
 * @brief Check if RTC lost power (OSF flag).
 */
bool rtc_lost_power(void)
{
    uint8_t status = rtc_read_register(0x0F);
    return ((status & 0x80U) != 0U);
}

/**
 * @brief Clear Oscillator Stop Flag (OSF).
 */
void rtc_clear_osf(void)
{
    uint8_t status = rtc_read_register(0x0F);
    status &= (uint8_t)~0x80U;
    rtc_write_register(0x0F, status);
}

/**
 * @brief Set full date and time.
 */
void rtc_set_datetime(const rtc_datetime_t *dt)
{
    if (dt == NULL) return;

    uint8_t year_2digit = (uint8_t)(dt->year % 100U);

    i2c_start();
    i2c_write_byte((uint8_t)((DS3231_ADDR << 1) | 0U));
    i2c_write_byte(0x00);

    i2c_write_byte(dec_to_bcd(dt->second));
    i2c_write_byte(dec_to_bcd(dt->minute));
    i2c_write_byte(dec_to_bcd(dt->hour));
    i2c_write_byte(dec_to_bcd(1U));
    i2c_write_byte(dec_to_bcd(dt->day));
    i2c_write_byte(dec_to_bcd(dt->month));
    i2c_write_byte(dec_to_bcd(year_2digit));

    i2c_stop();
}

/**
 * @brief Read current date and time from RTC.
 */
int rtc_read_datetime(rtc_datetime_t *dt)
{
    if (dt == NULL) return -1;

    uint8_t sec_bcd, min_bcd, hour_bcd, day_bcd, month_bcd, year_bcd;

    i2c_start();
    i2c_write_byte((DS3231_ADDR << 1) | 0U);
    i2c_write_byte(0x00);

    RTC_I2C->C1 |= I2C_C1_RSTA_MASK;
    i2c_write_byte((DS3231_ADDR << 1) | 1U);

    RTC_I2C->C1 &= ~I2C_C1_TX_MASK;
    RTC_I2C->C1 &= ~I2C_C1_TXAK_MASK;

    (void)RTC_I2C->D;
    i2c_wait();

    sec_bcd = RTC_I2C->D; i2c_wait();
    min_bcd = RTC_I2C->D; i2c_wait();
    hour_bcd = RTC_I2C->D; i2c_wait();
    (void)RTC_I2C->D; i2c_wait();
    day_bcd = RTC_I2C->D; i2c_wait();
    month_bcd = RTC_I2C->D; i2c_wait();

    RTC_I2C->C1 |= I2C_C1_TXAK_MASK;
    year_bcd = RTC_I2C->D; i2c_wait();

    i2c_stop();

    dt->second = bcd_to_dec(sec_bcd & 0x7F);
    dt->minute = bcd_to_dec(min_bcd & 0x7F);
    dt->hour   = bcd_to_dec(hour_bcd & 0x3F);
    dt->day    = bcd_to_dec(day_bcd & 0x3F);
    dt->month  = bcd_to_dec(month_bcd & 0x1F);
    dt->year   = 2000U + bcd_to_dec(year_bcd);

    return 0;
}

/**
 * @brief Set only time (keep date intact).
 */
void set_rtc(uint8_t h, uint8_t m, uint8_t s)
{
    rtc_datetime_t dt;

    if (rtc_read_datetime(&dt) != 0)
    {
        dt.year = 2026U;
        dt.month = 1U;
        dt.day = 1U;
    }

    dt.hour = h;
    dt.minute = m;
    dt.second = s;

    rtc_set_datetime(&dt);
}

/**
 * @brief Initialize RTC hardware and recover from power loss.
 */
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

    rtc_enable_oscillator();

    if (rtc_lost_power())
    {
        rtc_set_from_build_datetime();
        rtc_clear_osf();
    }
}

/**
 * @brief Read only time (hour, minute, second).
 */
int read_rtc(uint8_t *h, uint8_t *m, uint8_t *s)
{
    rtc_datetime_t dt;

    if ((h == NULL) || (m == NULL) || (s == NULL))
    {
        return -1;
    }

    if (rtc_read_datetime(&dt) != 0)
    {
        return -1;
    }

    *h = dt.hour;
    *m = dt.minute;
    *s = dt.second;

    return 0;
}