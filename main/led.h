/*
 * led.h — WS2812B RGB LED (GPIO8) — status indicator
 *
 * Colours:
 *   Blue   — searching for Zigbee network
 *   Green  — connected, all zones off
 *   Purple — at least one relay active (manual)
 *   Yellow — automatic sequence running
 *   White  — factory reset in progress
 */

#pragma once

typedef enum {
    LED_STATE_SEARCHING,      /* Blue   — searching for Zigbee network  */
    LED_STATE_CONNECTED,      /* Green  — connected, all off             */
    LED_STATE_ACTIVE,         /* Purple — relay active                   */
    LED_STATE_SEQUENCE,       /* Yellow — sequence running               */
    LED_STATE_FACTORY_RESET,  /* White  — factory reset in progress      */
} led_state_t;

void led_init(void);
void led_set_state(led_state_t state);
