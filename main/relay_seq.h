/*
 * relay_seq.h — Relay logic and sequence (8-channel Zigbee irrigation, ESP32-C6-Zero)
 *
 * Relays 0..7 correspond to Zigbee endpoints EP_RELAY_BASE..EP_RELAY_BASE+7.
 *
 * Operating modes:
 *   Manual   : relay_set() turns a relay on/off with no timer. It stays in
 *              that state until changed (from display, Zigbee, or sequence).
 *   Sequence : seq_start() activates relays one at a time in order 0→7,
 *              each for its configured duration. Relays with timer=0 are
 *              skipped. The sequence closes automatically at the end.
 *
 * Timers:
 *   Value in minutes (uint8), range 0-60. 0 = skip in sequence.
 *   Persisted in NVS (namespace "relay_tmr", keys "t0".."t7").
 *   Modifiable via display or Zigbee Analog Output attribute.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RELAY_COUNT    8

/* Zigbee endpoints */
#define EP_RELAY_BASE  1   /* relays 0..7 → EP 1..8 */
#define EP_SEQUENCE    9   /* virtual sequence switch */

/*
 * Callback invoked on every state change (from any task).
 * ep    = Zigbee endpoint (1-8 = relay, 9 = sequence)
 * state = new state
 * Use esp_zb_scheduler_alarm to interact with the Zigbee stack.
 */
typedef void (*relay_seq_cb_t)(uint8_t ep, bool state);

/** Initialise GPIOs, load timers from NVS, start sequence timer task. */
void relay_seq_init(relay_seq_cb_t cb);

/* ── Individual relays (manual, no timer) ──────────────────────────── */
void    relay_set(uint8_t relay, bool on);
bool    relay_get(uint8_t relay);

/* ── Sequence timers (0-60 min, 0=skip) ─────────────────────────────── */
void    relay_set_timer(uint8_t relay, uint8_t minutes);
uint8_t relay_get_timer(uint8_t relay);

/* ── Sequence ───────────────────────────────────────────────────────── */
void    seq_start(void);
void    seq_stop(void);
bool    seq_get(void);
int     seq_current_relay(void);   /* relay active in sequence, -1 if none */

/* ── Full stop (turns everything off and stops sequence) ─────────────── */
void    relay_seq_stop_all(void);
