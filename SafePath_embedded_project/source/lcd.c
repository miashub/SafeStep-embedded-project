#include "lcd.h"
#include <stdio.h>
#include <string.h>

/* ----------------------------
 * Private LCD Driver State
 * ---------------------------- */
static uint8_t lcd_backlight_on = 1U;

/* ----------------------------
 * Low-Level I2C Helpers
 * ---------------------------- */

/**
 * @brief Wait for I2C transfer to complete.
 */
static void lcd_i2c_wait(void)
{
    uint32_t timeout = 50000U;

    while (((LCD_I2C->S & I2C_S_IICIF_MASK) == 0U) && timeout)
    {
        timeout--;
    }

    LCD_I2C->S = I2C_S_IICIF_MASK;
}

/**
 * @brief Generate I2C start condition.
 */
static void lcd_i2c_start(void)
{
    LCD_I2C->C1 |= (I2C_C1_TX_MASK | I2C_C1_MST_MASK);
}

/**
 * @brief Generate I2C stop condition.
 */
static void lcd_i2c_stop(void)
{
    LCD_I2C->C1 &= ~(I2C_C1_MST_MASK | I2C_C1_TX_MASK);

    for (volatile int i = 0; i < 500; i++)
    {
        /* small bus settle delay */
    }
}

/**
 * @brief Write one byte onto I2C bus.
 * @param data Byte to write
 */
static void lcd_i2c_write_byte(uint8_t data)
{
    LCD_I2C->D = data;
    lcd_i2c_wait();
}

/**
 * @brief Write raw output byte to PCF8574.
 * @param val Byte to send
 */
static void lcd_write_bus(uint8_t val)
{
    lcd_i2c_start();
    lcd_i2c_write_byte((LCD_ADDR << 1) | 0U);
    lcd_i2c_write_byte(val);
    lcd_i2c_stop();
}

/**
 * @brief Pulse LCD enable line.
 * @param val Current LCD bus value
 */
static void lcd_pulse_en(uint8_t val)
{
    lcd_write_bus(val | 0x04U);
    delay_us(1);
    lcd_write_bus((uint8_t)(val & ~0x04U));
    delay_us(50);
}

/**
 * @brief Send one 4-bit nibble to LCD.
 * @param nibble Upper nibble aligned in bits [7:4]
 * @param mode 0 = command, 1 = data
 */
static void lcd_send_nibble(uint8_t nibble, uint8_t mode)
{
    uint8_t val = (uint8_t)((nibble & 0xF0U) | mode | (lcd_backlight_on ? 0x08U : 0x00U));
    lcd_write_bus(val);
    lcd_pulse_en(val);
}

/**
 * @brief Send full byte in 4-bit mode.
 * @param data Byte to send
 * @param mode 0 = command, 1 = data
 */
static void lcd_send_byte(uint8_t data, uint8_t mode)
{
    lcd_send_nibble((uint8_t)(data & 0xF0U), mode);
    lcd_send_nibble((uint8_t)((data << 4) & 0xF0U), mode);
}

/* ----------------------------
 * Public LCD API
 * ---------------------------- */

void lcd_cmd(uint8_t cmd)
{
    lcd_send_byte(cmd, 0U);
}

void lcd_data(uint8_t data)
{
    lcd_send_byte(data, 1U);
}

void lcd_clear(void)
{
    lcd_cmd(0x01);
    delay_us(2000);
}

void lcd_display_on(void)
{
    lcd_cmd(0x0C);
}

void lcd_display_off(void)
{
    lcd_cmd(0x08);
}

void lcd_set_backlight(uint8_t on)
{
    lcd_backlight_on = on ? 1U : 0U;
    lcd_write_bus(lcd_backlight_on ? 0x08U : 0x00U);
}

void lcd_init(void)
{
    /* Enable clocks for PORTB and I2C0 */
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;
    SIM->SCGC4 |= SIM_SCGC4_I2C0_MASK;

    /* Configure PTB2 and PTB3 for I2C0 */
    LCD_PORT->PCR[LCD_SCL_PIN] =
        PORT_PCR_MUX(2) | PORT_PCR_ODE_MASK | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;

    LCD_PORT->PCR[LCD_SDA_PIN] =
        PORT_PCR_MUX(2) | PORT_PCR_ODE_MASK | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;

    /* Configure I2C0 */
    LCD_I2C->F = 0x35;
    LCD_I2C->C1 = I2C_C1_IICEN_MASK;

    /* LCD initialization sequence for 4-bit mode */
    delay_us(50000);
    lcd_send_nibble(0x30, 0U);
    delay_us(5000);
    lcd_send_nibble(0x30, 0U);
    delay_us(150);
    lcd_send_nibble(0x30, 0U);
    lcd_send_nibble(0x20, 0U);

    lcd_cmd(0x28);   /* 4-bit, 2-line, 5x8 font */
    lcd_cmd(0x0C);   /* display on, cursor off */
    lcd_cmd(0x06);   /* entry mode set */
    lcd_cmd(0x01);   /* clear display */
    delay_us(2000);
}

void lcd_set_cursor(uint8_t col, uint8_t row)
{
    const uint8_t row_offsets[] = {0x00U, 0x40U};
    lcd_cmd((uint8_t)(0x80U | (col + row_offsets[row])));
}

void lcd_print(const char *str)
{
    while (*str)
    {
        lcd_data((uint8_t)(*str++));
    }
}

void lcd_show_message(const char *line1, const char *line2)
{
    char buf[17];

    lcd_set_cursor(0, 0);
    snprintf(buf, sizeof(buf), "%-16.16s", (line1 != NULL) ? line1 : "");
    lcd_print(buf);

    lcd_set_cursor(0, 1);
    snprintf(buf, sizeof(buf), "%-16.16s", (line2 != NULL) ? line2 : "");
    lcd_print(buf);
}

void lcd_display_sensors(int temp_x10, int hum_x10, uint16_t light, uint8_t pir)
{
    char buf[17];

    int t_int = temp_x10 / 10;
    int t_frac = temp_x10 % 10;
    if (t_frac < 0)
    {
        t_frac = -t_frac;
    }

    int h_int = hum_x10 / 10;
    int h_frac = hum_x10 % 10;
    if (h_frac < 0)
    {
        h_frac = -h_frac;
    }

    lcd_set_cursor(0, 0);
    if (temp_x10 < 0 && t_int == 0)
    {
        snprintf(buf, sizeof(buf), "T:-0.%dC H:%d.%d", t_frac, h_int, h_frac);
    }
    else
    {
        snprintf(buf, sizeof(buf), "T:%d.%dC H:%d.%d", t_int, t_frac, h_int, h_frac);
    }
    lcd_print(buf);
    for (int i = (int)strlen(buf); i < 16; i++)
    {
        lcd_data(' ');
    }

    lcd_set_cursor(0, 1);
    snprintf(buf, sizeof(buf), "L:%u M:%s", light, pir ? "YES" : "NO");
    lcd_print(buf);
    for (int i = (int)strlen(buf); i < 16; i++)
    {
        lcd_data(' ');
    }
}
