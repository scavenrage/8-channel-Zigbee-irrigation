/*
 * config.h — 8-channel Zigbee irrigation controller (ESP32-C6-Zero)
 *
 * All hardware constants for the project.
 * Modify here to adapt the pinout without touching the rest of the code.
 */

#pragma once

#include "driver/gpio.h"

/* ── Relays (active HIGH — 2N7002 with 10kΩ pull-down on gate) ─────────
 * The pull-down keeps the gate LOW during boot:
 *   GPIO5  → VDD_SPI = 3.3V  (correct for the internal SiP flash) ✓
 *   GPIO15 → same pattern as irrigation, works ✓
 */
#define RELAY_COUNT     8

#define GPIO_RELAY_0    GPIO_NUM_14
#define GPIO_RELAY_1    GPIO_NUM_15
#define GPIO_RELAY_2    GPIO_NUM_18
#define GPIO_RELAY_3    GPIO_NUM_19
#define GPIO_RELAY_4    GPIO_NUM_20
#define GPIO_RELAY_5    GPIO_NUM_21
#define GPIO_RELAY_6    GPIO_NUM_22
#define GPIO_RELAY_7    GPIO_NUM_5    /* strapping VDD_SPI: LOW at boot → 3.3V ✓ */

/* ── Buttons (active LOW, internal pull-up) ────────────────────────────
 * GPIO4 (BTN_RIGHT) is strapping JTAG_SEL_IN:
 *   pull-up HIGH at boot → USB-JTAG enabled (default behaviour ✓)
 */
#define GPIO_BTN_LEFT   GPIO_NUM_2
#define GPIO_BTN_OK     GPIO_NUM_3
#define GPIO_BTN_RIGHT  GPIO_NUM_4

/* ── LCD1602 display with PCF8574 I2C module ────────────────────────────
 * GPIO0 and GPIO1 are not strapping pins → PCF8574 module pull-ups
 * do not interfere at boot ✓
 * Remove the onboard pull-ups and replace with 3.3V external pull-ups
 * to keep signal levels within ESP32 tolerances.
 */
#define LCD_I2C_PORT    I2C_NUM_0
#define LCD_I2C_SDA     GPIO_NUM_0
#define LCD_I2C_SCL     GPIO_NUM_1
#define LCD_I2C_FREQ_HZ 50000        /* 50 kHz — reduced for level shifter */
#define LCD_I2C_ADDR    0x27         /* 0x3F for PCF8574AT variant */
#define LCD_COLS        16
#define LCD_ROWS        2

/* ── Backlight / display sleep ─────────────────────────────────────────── */
#define LCD_BACKLIGHT_TIMEOUT_MS  20000   /* ms of inactivity before display sleep */

/* ── WS2812B RGB LED (onboard, not exposed on header) ──────────────────── */
/* GPIO8 — managed by led.c */

/* ── Relay timers (used by the sequence) ───────────────────────────────────
 * 0 = relay skipped in sequence
 * 1-60 = duration in minutes
 */
#define TIMER_MIN_MIN    0
#define TIMER_MAX_MIN   60
#define TIMER_DEFAULT    5    /* minutes — initial value if NVS is empty */
