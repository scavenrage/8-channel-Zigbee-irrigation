/*
 * main.c — 8-channel Zigbee irrigation controller (ESP32-C6-Zero)
 *
 * Zigbee endpoints (HA profile):
 *   EP1..EP8  On/Off Output + Analog Output (timer 0-60 min)  → relay 0..7
 *   EP9       On/Off Output                                    → sequence
 *
 * Factory reset: hold the BOOT button (GPIO9) for 5 seconds.
 * The first 10s after boot are ignored to avoid false triggers.
 */

#include "relay_seq.h"
#include "lcd.h"
#include "ui.h"
#include "main.h"
#include "led.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_cluster.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "MAIN";

/* ── Zigbee configuration ────────────────────────────────────────────── */
#define INSTALLCODE_POLICY_ENABLE   false
#define MAX_CHILDREN                10
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK  /* all channels 11-26 */

#define ESP_MANUFACTURER_NAME  "\x09""Handmade!"
#define ESP_MODEL_IDENTIFIER   "\x0B""relay_timer"

#define ESP_ZB_ZR_CONFIG() {                                        \
    .esp_zb_role         = ESP_ZB_DEVICE_TYPE_ROUTER,               \
    .install_code_policy = INSTALLCODE_POLICY_ENABLE,               \
    .nwk_cfg.zczr_cfg    = { .max_children = MAX_CHILDREN },        \
}
#define ESP_ZB_DEFAULT_RADIO_CONFIG() { .radio_mode = ZB_RADIO_MODE_NATIVE }
#define ESP_ZB_DEFAULT_HOST_CONFIG()  { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE }

#define HA_DEVICE_ANALOG_OUTPUT  0x000C

/* ── Zigbee state ────────────────────────────────────────────────────── */
static bool s_zigbee_ready = false;

zb_conn_state_t zb_get_conn_state(void)
{
    return s_zigbee_ready ? ZB_CONN_CONNECTED : ZB_CONN_CONNECTING;
}

/* ── LED update ──────────────────────────────────────────────────────── */
static void update_led(void)
{
    if (!s_zigbee_ready) return;   /* colour managed by signal handler */

    if (seq_get()) {
        led_set_state(LED_STATE_SEQUENCE);
        return;
    }
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (relay_get((uint8_t)i)) {
            led_set_state(LED_STATE_ACTIVE);
            return;
        }
    }
    led_set_state(LED_STATE_CONNECTED);
}

/* ── Sync On/Off → Zigbee attribute (via scheduler_alarm) ───────────── */
/* Param encoding: bit7=state, bit0..6=endpoint (1-9) */
static void do_zigbee_sync_onoff(uint8_t param)
{
    if (!s_zigbee_ready) return;
    uint8_t ep  = param & 0x7F;
    uint8_t val = (param & 0x80) ? 1 : 0;
    esp_zb_zcl_set_attribute_val(ep,
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
        &val, false);
}

/* ── Sync timer → Analog Output attribute (via scheduler_alarm) ──────── */
/* param = relay index (0-7); reads current value from relay_get_timer */
static void do_zigbee_sync_timer(uint8_t param)
{
    if (!s_zigbee_ready) return;
    if (param >= RELAY_COUNT) return;
    float val = (float)relay_get_timer(param);
    uint8_t ep = (uint8_t)(EP_RELAY_BASE + param);
    esp_zb_zcl_set_attribute_val(ep,
        ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID,
        &val, false);
}

/* ── relay_seq callback → notify Zigbee + update LED + LCD ───────────── */
static void on_relay_changed(uint8_t ep, bool state)
{
    if (s_zigbee_ready) {
        uint8_t param = (ep & 0x7F) | (state ? 0x80 : 0x00);
        esp_zb_scheduler_alarm(do_zigbee_sync_onoff, param, 0);
    }
    update_led();
    ui_request_refresh();   /* update display only when something changes */
}

/* ── Public function: notify timer change from UI ────────────────────── */
/* Called by ui.c after relay_set_timer() to update the HA attribute */
void zb_notify_timer(uint8_t relay_idx)
{
    if (!s_zigbee_ready || relay_idx >= RELAY_COUNT) return;
    esp_zb_scheduler_alarm(do_zigbee_sync_timer, relay_idx, 0);
}

/* ── Build Basic cluster ─────────────────────────────────────────────── */
static esp_zb_attribute_list_t *create_basic_cluster(void)
{
    esp_zb_basic_cluster_cfg_t cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x01,
    };
    esp_zb_attribute_list_t *c = esp_zb_basic_cluster_create(&cfg);
    esp_zb_basic_cluster_add_attr(c, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                  ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(c, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                  ESP_MODEL_IDENTIFIER);
    return c;
}

/* ── Relay endpoint: On/Off + Analog Output (timer) ─────────────────── */
static void add_relay_endpoint(esp_zb_ep_list_t *ep_list, uint8_t ep, uint8_t relay_idx)
{
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    /* Basic */
    esp_zb_cluster_list_add_basic_cluster(cl, create_basic_cluster(),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Identify */
    esp_zb_identify_cluster_cfg_t id_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(cl,
        esp_zb_identify_cluster_create(&id_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* On/Off — initial state OFF */
    esp_zb_on_off_cluster_cfg_t oo = { .on_off = 0 };
    esp_zb_cluster_list_add_on_off_cluster(cl,
        esp_zb_on_off_cluster_create(&oo),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Analog Output — timer in minutes (0-60) */
    float def_val = (float)relay_get_timer(relay_idx);
    float min_val = (float)TIMER_MIN_MIN;
    float max_val = (float)TIMER_MAX_MIN;
    float res     = 1.0f;

    esp_zb_analog_output_cluster_cfg_t ao_cfg = {
        .out_of_service = 0,
        .present_value  = def_val,
        .status_flags   = 0,
    };
    esp_zb_attribute_list_t *ao = esp_zb_analog_output_cluster_create(&ao_cfg);
    esp_zb_analog_output_cluster_add_attr(ao,
        ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_MIN_PRESENT_VALUE_ID, &min_val);
    esp_zb_analog_output_cluster_add_attr(ao,
        ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_MAX_PRESENT_VALUE_ID, &max_val);
    esp_zb_analog_output_cluster_add_attr(ao,
        ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_RESOLUTION_ID, &res);
    esp_zb_cluster_list_add_analog_output_cluster(cl, ao,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t epcfg = {
        .endpoint           = ep,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cl, epcfg);
    ESP_LOGI(TAG, "EP%d: relay %d (timer NVS=%d min)", ep, relay_idx, (int)def_val);
}

/* ── Sequence endpoint: On/Off only ──────────────────────────────────── */
static void add_sequence_endpoint(esp_zb_ep_list_t *ep_list)
{
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    esp_zb_cluster_list_add_basic_cluster(cl, create_basic_cluster(),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t id_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(cl,
        esp_zb_identify_cluster_create(&id_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_on_off_cluster_cfg_t oo = { .on_off = 0 };
    esp_zb_cluster_list_add_on_off_cluster(cl,
        esp_zb_on_off_cluster_create(&oo),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t epcfg = {
        .endpoint           = EP_SEQUENCE,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cl, epcfg);
    ESP_LOGI(TAG, "EP%d: sequence", EP_SEQUENCE);
}

/* ── Command handlers from HA ────────────────────────────────────────── */
static esp_err_t handle_on_off(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    if (msg->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF)        return ESP_OK;
    if (msg->attribute.id != ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID)    return ESP_OK;
    if (msg->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_BOOL)    return ESP_OK;

    uint8_t ep    = msg->info.dst_endpoint;
    bool    state = *(bool *)msg->attribute.data.value;

    ESP_LOGI(TAG, "Zigbee On/Off → EP%d state=%s", ep, state ? "ON" : "OFF");

    if (ep >= EP_RELAY_BASE && ep < EP_RELAY_BASE + RELAY_COUNT) {
        uint8_t relay = ep - EP_RELAY_BASE;
        relay_set(relay, state);

    } else if (ep == EP_SEQUENCE) {
        if (state) seq_start();
        else       seq_stop();
    }

    return ESP_OK;
}

static esp_err_t handle_analog_output(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    if (msg->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT)             return ESP_OK;
    if (msg->attribute.id != ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID)  return ESP_OK;
    if (msg->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_SINGLE)               return ESP_OK;

    uint8_t ep      = msg->info.dst_endpoint;
    float   minutes = *(float *)msg->attribute.data.value;

    if (ep >= EP_RELAY_BASE && ep < EP_RELAY_BASE + RELAY_COUNT) {
        uint8_t relay = ep - EP_RELAY_BASE;
        uint8_t min_u8 = (minutes < 0.0f) ? 0
                       : (minutes > (float)TIMER_MAX_MIN) ? TIMER_MAX_MIN
                       : (uint8_t)minutes;
        relay_set_timer(relay, min_u8);
        ESP_LOGI(TAG, "Timer relay %d → %d min (from HA)", relay, min_u8);
    }
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t cb_id, const void *msg)
{
    switch (cb_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
        const esp_zb_zcl_set_attr_value_message_t *m = msg;
        if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT)
            return handle_analog_output(m);
        return handle_on_off(m);
    }
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
        return ESP_OK;
    default:
        ESP_LOGD(TAG, "Unhandled action: 0x%x", cb_id);
        return ESP_OK;
    }
}

/* ── Zigbee signals ──────────────────────────────────────────────────── */
static void bdb_start_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(
        esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK,
        , TAG, "BDB commissioning error");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *sig)
{
    uint32_t *p   = sig->p_app_signal;
    esp_err_t err = sig->esp_err_status;
    esp_zb_app_signal_type_t type = *p;

    switch (type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Stack initialised");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Starting steering...");
            led_set_state(LED_STATE_SEARCHING);
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "Init failed, retrying...");
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err == ESP_OK) {
            s_zigbee_ready = true;
            ui_request_refresh();   /* update HOME: connection state */
            ESP_LOGI(TAG, "Connected: channel %d, PAN 0x%04hx, addr 0x%04hx",
                     esp_zb_get_current_channel(),
                     esp_zb_get_pan_id(),
                     esp_zb_get_short_address());

            /* Sync current state → Zigbee */
            uint8_t val;
            for (uint8_t r = 0; r < RELAY_COUNT; r++) {
                val = relay_get(r) ? 1 : 0;
                esp_zb_zcl_set_attribute_val((uint8_t)(EP_RELAY_BASE + r),
                    ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                    ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &val, false);
            }
            val = seq_get() ? 1 : 0;
            esp_zb_zcl_set_attribute_val(EP_SEQUENCE,
                ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &val, false);

            update_led();
        } else {
            ESP_LOGW(TAG, "Steering failed, retrying in 5s...");
            led_set_state(LED_STATE_SEARCHING);
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 5000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        s_zigbee_ready = false;
        ui_request_refresh();
        ESP_LOGW(TAG, "LEAVE signal received — retrying steering in 5s...");
        led_set_state(LED_STATE_SEARCHING);
        esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                               ESP_ZB_BDB_MODE_NETWORK_STEERING, 5000);
        break;

    case ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
        if (s_zigbee_ready) {   /* guard: avoid multiple scheduling */
            s_zigbee_ready = false;
            ui_request_refresh();
            ESP_LOGW(TAG, "No active links (0x18) — forcing reconnection");
            led_set_state(LED_STATE_SEARCHING);
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;

    case ESP_ZB_NLME_STATUS_INDICATION:
        ESP_LOGD(TAG, "NLME Status Indication (0x32) — ignored");
        break;

    case ESP_ZB_ZDO_DEVICE_UNAVAILABLE:
        ESP_LOGD(TAG, "ZDO Device Unavailable (0x3c) — ignored");
        break;

    default:
        ESP_LOGI(TAG, "Signal: %s (0x%x) %s",
                 esp_zb_zdo_signal_to_string(type), type, esp_err_to_name(err));
        break;
    }
}

/* ── Zigbee application watchdog ─────────────────────────────────────── */
/*
 * zb_wdt_feed_cb is scheduled every 60s by the Zigbee scheduler.
 * If the stack silently hangs, it stops being called.
 * zb_watchdog_task checks every 30s: if feed missing for >3 min → restart.
 */
#define ZB_WDT_FEED_INTERVAL_MS   60000
#define ZB_WDT_TIMEOUT_MS         180000
#define ZB_WDT_CHECK_INTERVAL_MS  30000

static volatile TickType_t s_zb_last_feed = 0;

static void zb_wdt_feed_cb(uint8_t param)
{
    (void)param;
    s_zb_last_feed = xTaskGetTickCount();
    esp_zb_scheduler_alarm(zb_wdt_feed_cb, 0, ZB_WDT_FEED_INTERVAL_MS);
}

static void zb_watchdog_task(void *pv)
{
    vTaskDelay(pdMS_TO_TICKS(ZB_WDT_FEED_INTERVAL_MS + 30000));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ZB_WDT_CHECK_INTERVAL_MS));
        TickType_t elapsed = xTaskGetTickCount() - s_zb_last_feed;
        if (elapsed > pdMS_TO_TICKS(ZB_WDT_TIMEOUT_MS)) {
            ESP_LOGE("ZB_WDT", "Zigbee stack unresponsive for %lu s — restarting!",
                     (unsigned long)(elapsed * portTICK_PERIOD_MS / 1000));
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
}

/* ── Factory reset (BOOT button / GPIO9) ────────────────────────────── */
#define FACTORY_RESET_GPIO   GPIO_NUM_9
#define FACTORY_RESET_HOLD_S 5

static void factory_reset_task(void *pv)
{
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << FACTORY_RESET_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    /* Ignore first 10s to avoid false triggers at boot */
    vTaskDelay(pdMS_TO_TICKS(10000));

    for (;;) {
        if (gpio_get_level(FACTORY_RESET_GPIO) != 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        ESP_LOGW(TAG, "GPIO9 pressed — hold for %ds for factory reset...",
                 FACTORY_RESET_HOLD_S);

        bool held = true;
        for (int i = 0; i < FACTORY_RESET_HOLD_S * 10; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(FACTORY_RESET_GPIO) != 0) { held = false; break; }
        }

        if (!held) {
            ESP_LOGI(TAG, "GPIO9 released — cancelled");
            continue;
        }

        ESP_LOGW(TAG, "*** ZIGBEE FACTORY RESET — erasing partitions... ***");
        relay_seq_stop_all();
        led_set_state(LED_STATE_FACTORY_RESET);

        const char *zb_parts[] = { "zb_storage", "zb_fct" };
        for (int i = 0; i < 2; i++) {
            const esp_partition_t *p = esp_partition_find_first(
                ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, zb_parts[i]);
            if (p) {
                esp_err_t e = esp_partition_erase_range(p, 0, p->size);
                ESP_LOGW(TAG, "Partition '%s': %s",
                         zb_parts[i], e == ESP_OK ? "ERASED" : esp_err_to_name(e));
            } else {
                ESP_LOGE(TAG, "Partition '%s' NOT FOUND!", zb_parts[i]);
            }
        }
        ESP_LOGW(TAG, "Restarting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

/* ── Zigbee task ─────────────────────────────────────────────────────── */
static void esp_zb_task(void *pv)
{
    /* Platform config here: the 802.15.4 radio is initialised ONLY when
     * the Zigbee task is active, avoiding interference with other
     * peripherals (LCD, GPIO) when Zigbee is disabled. */
    esp_zb_platform_config_t plat_cfg = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&plat_cfg));

    esp_zb_cfg_t cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&cfg);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

    /* EP1..EP8: relays with On/Off + timer Analog Output */
    for (uint8_t r = 0; r < RELAY_COUNT; r++) {
        add_relay_endpoint(ep_list, (uint8_t)(EP_RELAY_BASE + r), r);
    }

    /* EP9: sequence (On/Off only) */
    add_sequence_endpoint(ep_list);

    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));

    /* Start the periodic watchdog feed */
    s_zb_last_feed = xTaskGetTickCount();
    esp_zb_scheduler_alarm(zb_wdt_feed_cb, 0, ZB_WDT_FEED_INTERVAL_MS);

    esp_zb_stack_main_loop();
}

/* ── Entry point ─────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    led_init();
    led_set_state(LED_STATE_SEARCHING);

    relay_seq_init(on_relay_changed);

    lcd_init();
    ui_init();

    xTaskCreate(factory_reset_task, "factory_rst", 2048, NULL, 1, NULL);
    xTaskCreate(zb_watchdog_task,   "zb_wdt",      2048, NULL, 2, NULL);
    xTaskCreate(esp_zb_task,        "Zigbee_main", 8192, NULL, 5, NULL);
}
