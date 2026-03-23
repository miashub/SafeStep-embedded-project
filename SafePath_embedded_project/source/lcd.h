#ifndef LCD_H
#define LCD_H

#include "MK66F18.h"
#include <stdint.h>

/**
 * @file lcd.h
 * @brief LCD1602 I2C driver for MK66F18 using PCF8574 backpack.
 *
 * Hardware:
 * - LCD1602 with I2C backpack
 * - I2C0 on PTB2 (SCL) and PTB3 (SDA)
 */

/* ----------------------------
 * LCD Hardware Configuration
 * ---------------------------- */
#define LCD_PORT        PORTB
#define LCD_SCL_PIN     2
#define LCD_SDA_PIN     3
#define LCD_I2C         I2C0
#define LCD_ADDR        0x27

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Microsecond delay function provided by main/system code.
 * @param us Delay in microseconds
 */
void delay_us(uint32_t us);

/**
 * @brief Initialize LCD and I2C0 interface.
 */
void lcd_init(void);

/**
 * @brief Send LCD command.
 * @param cmd Command byte
 */
void lcd_cmd(uint8_t cmd);

/**
 * @brief Send LCD data byte.
 * @param data Character byte
 */
void lcd_data(uint8_t data);

/**
 * @brief Set LCD cursor position.
 * @param col Column (0-15)
 * @param row Row (0-1)
 */
void lcd_set_cursor(uint8_t col, uint8_t row);

/**
 * @brief Print string to LCD.
 * @param str Null-terminated string
 */
void lcd_print(const char *str);

/**
 * @brief Clear LCD screen.
 */
void lcd_clear(void);

/**
 * @brief Turn LCD display on.
 */
void lcd_display_on(void);

/**
 * @brief Turn LCD display off.
 */
void lcd_display_off(void);

/**
 * @brief Turn LCD backlight on or off.
 * @param on 1 = on, 0 = off
 */
void lcd_set_backlight(uint8_t on);

/**
 * @brief Show two full lines on LCD.
 * @param line1 First line
 * @param line2 Second line
 */
void lcd_show_message(const char *line1, const char *line2);

/**
 * @brief Display sensor data on LCD.
 * @param temp_x10 Temperature in tenths of degrees Celsius
 * @param hum_x10 Humidity in tenths of percent
 * @param light LDR raw value
 * @param pir PIR state
 */
void lcd_display_sensors(int temp_x10, int hum_x10, uint16_t light, uint8_t pir);

#ifdef __cplusplus
}
#endif

#endif /* LCD_H */
