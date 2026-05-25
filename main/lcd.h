/*
 * lcd.h — LCD1602 driver via PCF8574 I2C (8-channel Zigbee irrigation, ESP32-C6-Zero)
 *
 * HD44780 interface in 4-bit mode via PCF8574 I2C expander.
 * PCF8574 pin mapping → HD44780:
 *   P0 = RS    P1 = RW (always 0)    P2 = EN    P3 = BL (backlight)
 *   P4 = D4    P5 = D5               P6 = D6    P7 = D7
 *
 * Backlight and display are controlled separately via lcd_backlight() and
 * lcd_display(); the inactivity timeout is managed by ui.c.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialise the I2C bus and the HD44780 controller in 4-bit mode.
 * Must be called once, before any other function.
 */
void lcd_init(void);

/** Clear the display and return cursor to (0,0). */
void lcd_clear(void);

/** Position the cursor at column col (0-15) and row row (0-1). */
void lcd_set_cursor(uint8_t col, uint8_t row);

/** Write string str from the current cursor position. */
void lcd_print(const char *str);

/**
 * Write a formatted string (printf-style) starting at (col, row).
 * Internal buffer is LCD_COLS+1 bytes; silently truncates beyond 16 chars.
 */
void lcd_printf(uint8_t col, uint8_t row, const char *fmt, ...);

/** Turn the backlight on (true) or off (false). */
void lcd_backlight(bool on);

/**
 * Turn the HD44780 display on (true) or off (false).
 * With display off, DDRAM content is preserved: it reappears on wake.
 * Use lcd_backlight() separately.
 */
void lcd_display(bool on);
