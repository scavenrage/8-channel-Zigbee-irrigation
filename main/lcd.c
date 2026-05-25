/*
 * lcd.c — LCD1602 driver via PCF8574 (software bit-bang I2C)
 *
 * Bypasses the hardware I2C driver to eliminate sporadic errors
 * of the I2C peripheral on ESP32-C6 with ESP-IDF 5.1.x.
 *
 * esp_rom_delay_us() performs a busy-wait at precise clock cycles,
 * completely immune to FreeRTOS scheduling → zero timing errors.
 *
 * PCF8574 bit layout:
 *   bit 0 (P0) = RS       bit 1 (P1) = RW (always 0)
 *   bit 2 (P2) = EN       bit 3 (P3) = BL (backlight)
 *   bit 4 (P4) = D4       bit 5 (P5) = D5
 *   bit 6 (P6) = D6       bit 7 (P7) = D7
 */

#include "lcd.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "LCD";

/* ── Bit-bang I2C ─────────────────────────────────────────────────────── */

/* Half-period: 10 µs → ~50 kHz */
#define BB_HALF_US  10

static inline void bb_delay(void)  { esp_rom_delay_us(BB_HALF_US); }
static inline void bb_sda(int v)   { gpio_set_level(LCD_I2C_SDA, v); }
static inline void bb_scl(int v)   { gpio_set_level(LCD_I2C_SCL, v); }

static void bb_start(void)
{
    bb_sda(1); bb_delay();
    bb_scl(1); bb_delay();
    bb_sda(0); bb_delay();   /* SDA falls while SCL high → START condition */
    bb_scl(0); bb_delay();
}

static void bb_stop(void)
{
    bb_sda(0); bb_delay();
    bb_scl(1); bb_delay();
    bb_sda(1); bb_delay();   /* SDA rises while SCL high → STOP condition */
}

/* Transmits one byte MSB-first, reads ACK bit. Returns 0=ACK, 1=NACK. */
static int bb_write_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        bb_sda((b >> i) & 1);
        bb_delay();
        bb_scl(1); bb_delay();
        bb_scl(0); bb_delay();
    }
    bb_sda(1);                               /* release SDA: slave can pull LOW */
    bb_delay();
    bb_scl(1); bb_delay();
    int ack = gpio_get_level(LCD_I2C_SDA);  /* 0 = ACK, 1 = NACK */
    bb_scl(0); bb_delay();
    return ack;
}

/* Complete I2C transaction: START + address + data + STOP */
static void bb_write(const uint8_t *data, size_t len)
{
    bb_start();
    bb_write_byte((uint8_t)(LCD_I2C_ADDR << 1));  /* 7-bit address + write */
    for (size_t i = 0; i < len; i++) {
        bb_write_byte(data[i]);
    }
    bb_stop();
}

/* ── PCF8574 constant bitmasks ────────────────────────────────────────── */
#define PCF_RS  (1 << 0)
#define PCF_RW  (1 << 1)
#define PCF_EN  (1 << 2)
#define PCF_BL  (1 << 3)

/* ── HD44780 DDRAM row offsets ────────────────────────────────────────── */
static const uint8_t ROW_OFFSET[2] = { 0x00, 0x40 };

/* ── Backlight state ──────────────────────────────────────────────────── */
static uint8_t s_bl = PCF_BL;

/* ── Send a complete LCD byte (2 nibbles, 4 I2C bytes) ───────────────── */
static void _send_byte(uint8_t byte, uint8_t rs)
{
    uint8_t data_u = byte & 0xF0;
    uint8_t data_l = (byte << 4) & 0xF0;
    uint8_t ctrl   = s_bl | (rs ? PCF_RS : 0);
    uint8_t data_t[4];
    data_t[0] = data_u | ctrl | PCF_EN;
    data_t[1] = data_u | ctrl;
    data_t[2] = data_l | ctrl | PCF_EN;
    data_t[3] = data_l | ctrl;
    bb_write(data_t, 4);
}

#define _cmd(b)  _send_byte((b), 0)
#define _data(b) _send_byte((b), 1)

/* ── Init ─────────────────────────────────────────────────────────────── */
void lcd_init(void)
{
    /* GPIO open-drain with internal pull-up: compatible with I2C bus */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LCD_I2C_SDA) | (1ULL << LCD_I2C_SCL),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    bb_sda(1); bb_scl(1);   /* bus idle before init */

    /* HD44780 reset by instruction sequence (datasheet §4.7.2) */
    esp_rom_delay_us(50000);
    _cmd(0x30); esp_rom_delay_us(5000);
    _cmd(0x30); esp_rom_delay_us(200);
    _cmd(0x30); esp_rom_delay_us(10000);
    _cmd(0x20); esp_rom_delay_us(10000);   /* switch to 4-bit mode */

    _cmd(0x28); esp_rom_delay_us(1000);    /* 4-bit, 2 rows, 5×8 */
    _cmd(0x08); esp_rom_delay_us(1000);    /* display OFF */
    _cmd(0x01); esp_rom_delay_us(2000);    /* clear */
    _cmd(0x06); esp_rom_delay_us(1000);    /* entry mode: cursor advances */
    _cmd(0x0C); esp_rom_delay_us(1000);    /* display ON, cursor OFF */

    ESP_LOGI(TAG, "Init OK (bit-bang ~%dkHz, addr=0x%02X)",
             1000 / (BB_HALF_US * 2), LCD_I2C_ADDR);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void lcd_clear(void)
{
    _cmd(0x01);
    esp_rom_delay_us(5000);
}

void lcd_set_cursor(uint8_t col, uint8_t row)
{
    if (row >= LCD_ROWS) row = LCD_ROWS - 1;
    if (col >= LCD_COLS) col = LCD_COLS - 1;
    _cmd((uint8_t)(0x80 | (ROW_OFFSET[row] + col)));
}

void lcd_print(const char *str)
{
    while (*str) {
        _data((uint8_t)*str++);
    }
}

void lcd_printf(uint8_t col, uint8_t row, const char *fmt, ...)
{
    char buf[LCD_COLS + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    size_t len = strlen(buf);
    while (len < LCD_COLS) buf[len++] = ' ';
    buf[LCD_COLS] = '\0';

    lcd_set_cursor(col, row);
    lcd_print(buf);
}

void lcd_backlight(bool on)
{
    s_bl = on ? PCF_BL : 0;
    uint8_t d = s_bl;
    bb_write(&d, 1);
}

void lcd_display(bool on)
{
    /* 0x0C = display ON, cursor OFF, blink OFF
     * 0x08 = display OFF (DDRAM content preserved) */
    _cmd(on ? 0x0C : 0x08);
}
