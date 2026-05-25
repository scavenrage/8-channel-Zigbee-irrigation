/*
 * main.h — Internal API for main.c (8-channel Zigbee irrigation, ESP32-C6-Zero)
 *
 * Exposes only the functions that need to be called from other modules.
 */

#pragma once

#include <stdint.h>

/**
 * Notifies main.c that the timer for relay_idx has changed (from LCD menu).
 * Updates the Analog Output Zigbee attribute if the network is connected.
 * Thread-safe: uses esp_zb_scheduler_alarm internally.
 */
void zb_notify_timer(uint8_t relay_idx);

/** Zigbee connection state exposed to ui.c for the HOME display. */
typedef enum {
    ZB_CONN_CONNECTING = 0,   /* searching / steering */
    ZB_CONN_CONNECTED  = 1,   /* connected to network */
} zb_conn_state_t;

/** Returns the current Zigbee connection state. Thread-safe (atomic read). */
zb_conn_state_t zb_get_conn_state(void);
