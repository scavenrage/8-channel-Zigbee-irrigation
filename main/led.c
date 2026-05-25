/*
 * led.c — WS2812B RGB LED (GPIO8)
 */

#include "led.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "LED";

#define LED_GPIO        8
#define LED_BRIGHTNESS  25   /* 0-255 — ~10%, avoids glare */

/* Colour table with R and G swapped (WS2812B GRB→RGB workaround)
 * Format: { G_sent, R_sent, B_sent } */
static const uint8_t COLOR_TABLE[][3] = {
    [LED_STATE_SEARCHING]    = {  0,               0,              LED_BRIGHTNESS }, /* Blue   */
    [LED_STATE_CONNECTED]    = {  LED_BRIGHTNESS,  0,              0              }, /* Green  */
    [LED_STATE_ACTIVE]       = {  0,               LED_BRIGHTNESS, LED_BRIGHTNESS }, /* Purple */
    [LED_STATE_SEQUENCE]     = {  LED_BRIGHTNESS,  LED_BRIGHTNESS, 0              }, /* Yellow */
    [LED_STATE_FACTORY_RESET]= {  LED_BRIGHTNESS,  LED_BRIGHTNESS, LED_BRIGHTNESS }, /* White  */
};

static led_strip_handle_t s_strip;

void led_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num   = LED_GPIO,
        .max_leds         = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &s_strip));
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "Init OK (GPIO%d, brightness=%d)", LED_GPIO, LED_BRIGHTNESS);
}

void led_set_state(led_state_t state)
{
    const uint8_t *c = COLOR_TABLE[state];
    led_strip_set_pixel(s_strip, 0, c[0], c[1], c[2]);
    led_strip_refresh(s_strip);
}
