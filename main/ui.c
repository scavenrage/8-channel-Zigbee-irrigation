/*
 * ui.c — User interface: buttons + LCD menu (8-channel Zigbee irrigation, ESP32-C6-Zero)
 *
 * Task ui_task (20 ms tick):
 *   - Reads 3 buttons with edge-detection (active LOW, internal pull-up).
 *   - Manages the LCD menu state machine.
 *   - Controls the display sleep timeout (LCD_BACKLIGHT_TIMEOUT_MS).
 *
 * Menu states:
 *   HOME        — status screen (seq / manual relays / all off)
 *   MAIN_MENU   — selection among 3 items
 *   TIMER_SEL   — relay selection to view/edit its timer
 *   TIMER_EDIT  — edit the timer value of the selected relay
 *   MANUAL_SEL  — relay selection for manual on/off command
 *
 * LCD display strings: all exactly 16 chars or shorter (padded by lcd_printf).
 */

#include "ui.h"
#include "lcd.h"
#include "relay_seq.h"
#include "main.h"
#include "config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "UI";

/* ── Constants ────────────────────────────────────────────────────────── */
#define UI_TICK_MS          20
/* BL_TIMEOUT_TICKS = 0 means backlight always on */
#define BL_TIMEOUT_TICKS    (LCD_BACKLIGHT_TIMEOUT_MS / UI_TICK_MS)

/* Main menu items (index 0 is dynamic: Start/Stop sequence) */
#define MENU_ITEMS          3
static const char *MENU_LABEL[MENU_ITEMS] = {
    NULL,               /* dynamic — see _render_main_menu */
    "Timer settings",
    "Manual control",
};

/* ── UI state ─────────────────────────────────────────────────────────── */
typedef enum {
    UI_HOME,
    UI_MAIN_MENU,
    UI_TIMER_SEL,
    UI_TIMER_EDIT,
    UI_MANUAL_SEL,
} ui_state_t;

static ui_state_t s_state      = UI_HOME;
static uint8_t    s_menu_idx   = 0;    /* selected main menu item */
static uint8_t    s_relay_idx  = 0;    /* relay selected in timer/manual */
static uint8_t    s_edit_val   = 0;    /* timer value being edited */

/* Refresh flag requested by external tasks (volatile for cross-task access) */
static volatile bool s_refresh_requested = false;

/* ── Backlight ────────────────────────────────────────────────────────── */
static bool     s_bl_on    = true;   /* lcd_init() leaves BL on */
static uint32_t s_bl_ticks = 0;     /* inactivity tick counter */

static void _bl_bump(void)
{
    s_bl_ticks = 0;
    if (!s_bl_on) {
        s_bl_on = true;
        lcd_backlight(true);
    }
}

/* ── Buttons ──────────────────────────────────────────────────────────── */
static bool s_prev[3] = {true, true, true};   /* previous state (HIGH=released) */

typedef enum { BTN_L = 0, BTN_OK_K = 1, BTN_R = 2 } btn_id_t;

static const gpio_num_t BTN_GPIO[3] = {
    GPIO_BTN_LEFT,
    GPIO_BTN_OK,
    GPIO_BTN_RIGHT,
};

/* Returns true on falling edge (press), false otherwise */
static bool _btn_pressed(btn_id_t b)
{
    bool cur = (gpio_get_level(BTN_GPIO[b]) == 0);
    bool edge = cur && !s_prev[b];
    s_prev[b] = cur;
    return edge;
}

/* ── Display render ───────────────────────────────────────────────────── */

static void _render_home(void)
{
    if (seq_get()) {
        int r = seq_current_relay();
        lcd_printf(0, 0, "SEQ: EV%d", r + 1);
        lcd_printf(0, 1, "t=%dmin  OK=menu", relay_get_timer((uint8_t)r));
    } else {
        /* Count manually active relays */
        int on_count = 0;
        for (int i = 0; i < RELAY_COUNT; i++) {
            if (relay_get((uint8_t)i)) on_count++;
        }
        if (on_count > 0) {
            /* Show active EV bitmask */
            char bar[RELAY_COUNT + 1];
            for (int i = 0; i < RELAY_COUNT; i++)
                bar[i] = relay_get((uint8_t)i) ? '1' : '0';
            bar[RELAY_COUNT] = '\0';
            lcd_printf(0, 0, "EV: %s", bar);
            lcd_printf(0, 1, "OK=menu");
        } else {
            /* All off: show Zigbee connection state on row 2 */
            lcd_printf(0, 0, "All off");
            if (zb_get_conn_state() == ZB_CONN_CONNECTED)
                lcd_printf(0, 1, "Online   OK=menu");  /* 16 chars */
            else
                lcd_printf(0, 1, "Search   OK=menu");  /* 16 chars */
        }
    }
}

static void _render_main_menu(void)
{
    /* Row 0: selected item (index 0 dynamic: Start/Stop sequence) */
    const char *label = (s_menu_idx == 0)
        ? (seq_get() ? "Stop sequence" : "Start sequence")
        : MENU_LABEL[s_menu_idx];
    lcd_printf(0, 0, "%s", label);
    /* Row 1: navigation hint */
    lcd_printf(0, 1, "< back       OK>");  /* 16 chars */
}

static void _render_timer_sel(void)
{
    uint8_t t = relay_get_timer(s_relay_idx);
    lcd_printf(0, 0, "Timer EV%d:%3dmin", s_relay_idx + 1, t);  /* 16 chars */
    if (s_relay_idx == 0)
        lcd_printf(0, 1, "<back        OK>");  /* 16 chars */
    else
        lcd_printf(0, 1, "< EV     OK=edit");  /* 16 chars */
}

static void _render_timer_edit(void)
{
    lcd_printf(0, 0, "EV%d:[%3d] min", s_relay_idx + 1, s_edit_val);
    lcd_printf(0, 1, "<-min  +min>  OK");  /* 16 chars */
}

static void _render_manual_sel(void)
{
    bool on = relay_get(s_relay_idx);
    lcd_printf(0, 0, "EV%d: %s", s_relay_idx + 1, on ? "ON" : "OFF");
    if (s_relay_idx == 0)
        lcd_printf(0, 1, "<back        OK>");  /* 16 chars */
    else
        lcd_printf(0, 1, "< EV     OK=tog");   /* 15 chars, padded */
}

static void _render(void)
{
    switch (s_state) {
        case UI_HOME:        _render_home();        break;
        case UI_MAIN_MENU:   _render_main_menu();   break;
        case UI_TIMER_SEL:   _render_timer_sel();   break;
        case UI_TIMER_EDIT:  _render_timer_edit();  break;
        case UI_MANUAL_SEL:  _render_manual_sel();  break;
    }
}

/* ── State transitions ───────────────────────────────────────────────── */

static void _handle_home(bool left, bool ok, bool right)
{
    /* BTN_OK → open main menu */
    if (ok) {
        s_menu_idx = 0;
        s_state = UI_MAIN_MENU;
        _render();
    }
    /* left/right ignored in HOME */
    (void)left; (void)right;
}

static void _handle_main_menu(bool left, bool ok, bool right)
{
    if (left) {
        /* Return to HOME */
        s_state = UI_HOME;
        _render();
        return;
    }
    if (right) {
        s_menu_idx = (uint8_t)((s_menu_idx + 1) % MENU_ITEMS);
        _render();
        return;
    }
    if (ok) {
        switch (s_menu_idx) {
            case 0: /* Start / Stop sequence */
                if (seq_get()) {
                    seq_stop();
                } else {
                    seq_start();
                }
                s_state = UI_HOME;
                _render();
                break;

            case 1: /* Timer settings */
                if (seq_get()) {
                    /* Sequence running: warn and do not enter */
                    lcd_printf(0, 0, "SEQ running!");    /* padded to 16 */
                    lcd_printf(0, 1, "Stop seq. first!"); /* 16 chars */
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    _render(); /* redraw menu */
                } else {
                    s_relay_idx = 0;
                    s_state = UI_TIMER_SEL;
                    _render();
                }
                break;

            case 2: /* Manual control */
                if (seq_get()) {
                    lcd_printf(0, 0, "SEQ running!");    /* padded to 16 */
                    lcd_printf(0, 1, "Stop seq. first!"); /* 16 chars */
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    _render();
                } else {
                    s_relay_idx = 0;
                    s_state = UI_MANUAL_SEL;
                    _render();
                }
                break;
        }
    }
}

static void _handle_timer_sel(bool left, bool ok, bool right)
{
    if (left) {
        if (s_relay_idx == 0) {
            /* Return to main menu */
            s_state = UI_MAIN_MENU;
        } else {
            s_relay_idx--;
        }
        _render();
        return;
    }
    if (right) {
        if (s_relay_idx < RELAY_COUNT - 1) s_relay_idx++;
        _render();
        return;
    }
    if (ok) {
        /* Enter timer edit mode */
        s_edit_val = relay_get_timer(s_relay_idx);
        s_state = UI_TIMER_EDIT;
        _render();
    }
}

static void _handle_timer_edit(bool left, bool ok, bool right)
{
    if (left) {
        /* Decrement timer (0 = skip in sequence) */
        if (s_edit_val > TIMER_MIN_MIN) s_edit_val--;
        _render();
        return;
    }
    if (right) {
        if (s_edit_val < TIMER_MAX_MIN) s_edit_val++;
        _render();
        return;
    }
    if (ok) {
        /* Save, notify Zigbee, return to relay selection */
        relay_set_timer(s_relay_idx, s_edit_val);
        zb_notify_timer(s_relay_idx);   /* update HA attribute if connected */
        ESP_LOGI(TAG, "Timer R%d saved: %d min", s_relay_idx, s_edit_val);
        s_state = UI_TIMER_SEL;
        _render();
    }
}

static void _handle_manual_sel(bool left, bool ok, bool right)
{
    if (left) {
        if (s_relay_idx == 0) {
            s_state = UI_MAIN_MENU;
        } else {
            s_relay_idx--;
        }
        _render();
        return;
    }
    if (right) {
        if (s_relay_idx < RELAY_COUNT - 1) s_relay_idx++;
        _render();
        return;
    }
    if (ok) {
        /* Toggle relay */
        bool cur = relay_get(s_relay_idx);
        relay_set(s_relay_idx, !cur);
        ESP_LOGI(TAG, "Relay %d → %s (manual)", s_relay_idx, cur ? "OFF" : "ON");
        _render();
    }
}

/* ── Main UI task ────────────────────────────────────────────────────── */
static void ui_task(void *pv)
{
    /* Initial render */
    _render();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));

        bool pressed_l  = _btn_pressed(BTN_L);
        bool pressed_ok = _btn_pressed(BTN_OK_K);
        bool pressed_r  = _btn_pressed(BTN_R);
        bool any = pressed_l || pressed_ok || pressed_r;

        if (any) {
            if (!s_bl_on) {
                /* Display asleep: any button press wakes without executing action.
                 * Sequence: display ON → update content → backlight ON
                 * (user immediately sees real state, not stale content) */
                s_bl_on = true;
                s_bl_ticks = 0;
                lcd_display(true);
                _render();
                lcd_backlight(true);
                s_refresh_requested = false;
                continue;
            }
            /* Backlight already on: reset timeout and handle the button */
            _bl_bump();
            s_refresh_requested = false;   /* render will happen inside handler */

            switch (s_state) {
                case UI_HOME:       _handle_home(pressed_l, pressed_ok, pressed_r);       break;
                case UI_MAIN_MENU:  _handle_main_menu(pressed_l, pressed_ok, pressed_r);  break;
                case UI_TIMER_SEL:  _handle_timer_sel(pressed_l, pressed_ok, pressed_r);  break;
                case UI_TIMER_EDIT: _handle_timer_edit(pressed_l, pressed_ok, pressed_r); break;
                case UI_MANUAL_SEL: _handle_manual_sel(pressed_l, pressed_ok, pressed_r); break;
            }
            continue;
        }

        /* ── Refresh requested by external task (e.g. relay state change) ─── */
        if (s_refresh_requested) {
            s_refresh_requested = false;
            if (s_bl_on) {
                _render();
            }
            /* If backlight is off, skip: display is invisible and we'd
             * waste I2C transactions for nothing. */
        }

        /* ── Display sleep timeout (0 = disabled) ───────────────────────── */
#if LCD_BACKLIGHT_TIMEOUT_MS > 0
        if (s_bl_on) {
            s_bl_ticks++;
            if (s_bl_ticks >= BL_TIMEOUT_TICKS) {
                s_bl_on = false;
                /* Turn off backlight first, then display:
                 * avoids a visible flash of blank display */
                lcd_backlight(false);
                lcd_display(false);
                if (s_state != UI_HOME) {
                    s_state = UI_HOME;
                }
            }
        }
#endif
    }
}

/* ── External refresh API ────────────────────────────────────────────── */
void ui_request_refresh(void)
{
    s_refresh_requested = true;
}

/* ── Initialisation ──────────────────────────────────────────────────── */
void ui_init(void)
{
    /* Configure buttons (INPUT with internal pull-up) */
    for (int i = 0; i < 3; i++) {
        gpio_config_t btn_cfg = {
            .pin_bit_mask = (1ULL << BTN_GPIO[i]),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&btn_cfg);
    }

    xTaskCreate(ui_task, "ui_task", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "Init OK");
}
