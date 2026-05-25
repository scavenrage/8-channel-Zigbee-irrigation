/*
 * ui.h — User interface: buttons + LCD menu (8-channel Zigbee irrigation, ESP32-C6-Zero)
 *
 * Three buttons, active LOW (internal pull-up):
 *   BTN_LEFT  (GPIO2) — left arrow / decrement
 *   BTN_OK    (GPIO3) — confirm / enter
 *   BTN_RIGHT (GPIO4) — right arrow / increment
 *
 * Backlight rules:
 *   - Turns on at the first button press.
 *   - The first press with backlight off does NOT execute the action: it
 *     only wakes the display (and resets the timeout).
 *   - Turns off automatically after LCD_BACKLIGHT_TIMEOUT_MS of inactivity.
 *
 * Sequence active rule:
 *   - While a sequence is running, the "Timer settings" and "Manual control"
 *     submenus are disabled (display shows a warning, no state change).
 */

#pragma once

/**
 * Initialise button GPIOs and start the UI task (20 ms polling).
 * Must be called after relay_seq_init() and lcd_init().
 */
void ui_init(void);

/**
 * Request a display refresh at the next UI task iteration.
 * Can be called from any task (thread-safe: writes a volatile flag).
 * Used by main.c when a relay or sequence state changes.
 */
void ui_request_refresh(void);
