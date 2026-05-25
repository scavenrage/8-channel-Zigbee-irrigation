/*
 * relay_seq.c — Relay logic and sequence (8-channel Zigbee irrigation, ESP32-C6-Zero)
 */

#include "relay_seq.h"
#include "config.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "RELAY_SEQ";

/* ── GPIOs ────────────────────────────────────────────────────────────── */
static const gpio_num_t RELAY_GPIO[RELAY_COUNT] = {
    GPIO_RELAY_0, GPIO_RELAY_1, GPIO_RELAY_2, GPIO_RELAY_3,
    GPIO_RELAY_4, GPIO_RELAY_5, GPIO_RELAY_6, GPIO_RELAY_7,
};

/* ── NVS ─────────────────────────────────────────────────────────────── */
#define NVS_NS "relay_tmr"

static uint8_t s_timer_min[RELAY_COUNT];

static void nvs_load(void)
{
    for (int i = 0; i < RELAY_COUNT; i++)
        s_timer_min[i] = TIMER_DEFAULT;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    char key[8];
    for (int i = 0; i < RELAY_COUNT; i++) {
        snprintf(key, sizeof(key), "t%d", i);
        nvs_get_u8(h, key, &s_timer_min[i]);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Timers from NVS: %d %d %d %d %d %d %d %d min",
             s_timer_min[0], s_timer_min[1], s_timer_min[2], s_timer_min[3],
             s_timer_min[4], s_timer_min[5], s_timer_min[6], s_timer_min[7]);
}

static void nvs_save(uint8_t relay)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[8];
    snprintf(key, sizeof(key), "t%d", relay);
    nvs_set_u8(h, key, s_timer_min[relay]);
    nvs_commit(h);
    nvs_close(h);
}

/* ── State ───────────────────────────────────────────────────────────── */
static bool      s_relay_on[RELAY_COUNT] = {false};
static bool      s_seq_active            = false;
static int       s_seq_relay             = -1;   /* current relay in sequence */
static TickType_t s_seq_end              = 0;    /* timer expiry tick */

static relay_seq_cb_t s_cb = NULL;

/* ── Hardware helpers ─────────────────────────────────────────────────── */
static void _hw_on(uint8_t r)  { gpio_set_level(RELAY_GPIO[r], 1); }
static void _hw_off(uint8_t r) { gpio_set_level(RELAY_GPIO[r], 0); }

static void _relay_on_hw(uint8_t r)
{
    _hw_on(r);
    s_relay_on[r] = true;
    ESP_LOGI(TAG, "Relay %d ON", r);
    if (s_cb) s_cb((uint8_t)(EP_RELAY_BASE + r), true);
}

static void _relay_off_hw(uint8_t r)
{
    _hw_off(r);
    s_relay_on[r] = false;
    ESP_LOGI(TAG, "Relay %d OFF", r);
    if (s_cb) s_cb((uint8_t)(EP_RELAY_BASE + r), false);
}

/* ── Sequence: advance to next valid relay (timer > 0) ───────────────── */
static void _seq_advance(void)
{
    /* Turn off current relay */
    if (s_seq_relay >= 0 && s_seq_relay < RELAY_COUNT) {
        _hw_off((uint8_t)s_seq_relay);
        s_relay_on[s_seq_relay] = false;
        if (s_cb) s_cb((uint8_t)(EP_RELAY_BASE + s_seq_relay), false);
    }

    /* Find next relay with timer > 0 */
    do {
        s_seq_relay++;
    } while (s_seq_relay < RELAY_COUNT && s_timer_min[s_seq_relay] == 0);

    if (s_seq_relay < RELAY_COUNT) {
        uint32_t ms = (uint32_t)s_timer_min[s_seq_relay] * 60000UL;
        s_seq_end = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
        _hw_on((uint8_t)s_seq_relay);
        s_relay_on[s_seq_relay] = true;
        ESP_LOGI(TAG, "[SEQ] Relay %d ON (%d min)", s_seq_relay, s_timer_min[s_seq_relay]);
        if (s_cb) s_cb((uint8_t)(EP_RELAY_BASE + s_seq_relay), true);
    } else {
        /* All relays completed */
        s_seq_active = false;
        s_seq_relay  = -1;
        s_seq_end    = 0;
        ESP_LOGI(TAG, "[SEQ] Completed");
        if (s_cb) s_cb(EP_SEQUENCE, false);
    }
}

/* ── Sequence timer task ─────────────────────────────────────────────── */
static void seq_timer_task(void *pv)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!s_seq_active || s_seq_end == 0) continue;
        TickType_t now = xTaskGetTickCount();
        /* Overflow-safe: positive difference means now has passed s_seq_end */
        if ((TickType_t)(now - s_seq_end) < (TickType_t)(UINT32_MAX / 2)) {
            _seq_advance();
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

void relay_seq_init(relay_seq_cb_t cb)
{
    s_cb = cb;
    nvs_load();

    gpio_config_t out = {
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < RELAY_COUNT; i++) {
        out.pin_bit_mask = (1ULL << RELAY_GPIO[i]);
        gpio_config(&out);
        _hw_off((uint8_t)i);
    }

    xTaskCreate(seq_timer_task, "seq_timer", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "Init OK");
}

void relay_set(uint8_t relay, bool on)
{
    if (relay >= RELAY_COUNT) return;
    if (on) _relay_on_hw(relay);
    else    _relay_off_hw(relay);
}

bool relay_get(uint8_t relay)
{
    if (relay >= RELAY_COUNT) return false;
    return s_relay_on[relay];
}

void relay_set_timer(uint8_t relay, uint8_t minutes)
{
    if (relay >= RELAY_COUNT) return;
    if (minutes > TIMER_MAX_MIN) minutes = TIMER_MAX_MIN;
    s_timer_min[relay] = minutes;
    nvs_save(relay);
    ESP_LOGI(TAG, "Timer relay %d → %d min", relay, minutes);
}

uint8_t relay_get_timer(uint8_t relay)
{
    if (relay >= RELAY_COUNT) return TIMER_DEFAULT;
    return s_timer_min[relay];
}

void seq_start(void)
{
    if (s_seq_active) return;

    /* Check that at least one relay has timer > 0 */
    bool any = false;
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (s_timer_min[i] > 0) { any = true; break; }
    }
    if (!any) {
        ESP_LOGW(TAG, "[SEQ] All timers are zero — sequence not started");
        return;
    }

    /* Turn off any manually active relays */
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (s_relay_on[i]) {
            _hw_off((uint8_t)i);
            s_relay_on[i] = false;
            if (s_cb) s_cb((uint8_t)(EP_RELAY_BASE + i), false);
        }
    }

    s_seq_active = true;
    s_seq_relay  = -1;
    ESP_LOGI(TAG, "[SEQ] Start");
    if (s_cb) s_cb(EP_SEQUENCE, true);
    _seq_advance();
}

void seq_stop(void)
{
    if (!s_seq_active) return;

    if (s_seq_relay >= 0 && s_seq_relay < RELAY_COUNT) {
        _hw_off((uint8_t)s_seq_relay);
        s_relay_on[s_seq_relay] = false;
        if (s_cb) s_cb((uint8_t)(EP_RELAY_BASE + s_seq_relay), false);
    }
    s_seq_active = false;
    s_seq_relay  = -1;
    s_seq_end    = 0;
    ESP_LOGI(TAG, "[SEQ] Stopped");
    if (s_cb) s_cb(EP_SEQUENCE, false);
}

bool seq_get(void)            { return s_seq_active; }
int  seq_current_relay(void)  { return s_seq_relay; }

void relay_seq_stop_all(void)
{
    ESP_LOGI(TAG, "FULL STOP");
    if (s_seq_active) seq_stop();
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (s_relay_on[i]) _relay_off_hw((uint8_t)i);
    }
}
