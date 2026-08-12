// OnePage C61 — 7-key input (3 side GPIO keys + 4 front ADC-ladder keys),
// built on espressif/button. Pin/ADC config is parameterized via
// board_keys_cfg_t (pass NULL for the C61 defaults below).

#include "board.h"

#ifdef CONFIG_IDF_TARGET_ESP32C61

#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "button_adc.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "board_keys";

// Startup grace: the front ADC-ladder node reads ~0 mV (= ENTER band) while it
// settles for the first ~2 s after boot, firing spurious ENTER presses. Drop
// all key events during this window.
#define KEYS_STARTUP_GRACE_US 2500000
static int64_t s_start_us;

// Built-in C61 defaults (docs/hardware_io.md §4):
//   side keys GPIO2/6/9 (active-low); front ladder GPIO4 = ADC1_CH2.
static const board_keys_cfg_t DEFAULT_CFG = {
    .gpio_wake = 2, .gpio_prev = 6, .gpio_next = 9,
    .adc_unit = 1, .adc_channel = 2,
};

// Front 4-key ADC ladder thresholds (mV). Measured on this board (front ADC
// node GPIO4 = ADC1_CH2, rest ~3100 mV): BACK ~2592, LEFT ~1956, RIGHT ~1316,
// ENTER ~0. Tight windows with dead zones between them so a press/release
// transient sweeping across levels lands in a "no key" gap instead of a
// neighbouring key's band (the wide theoretical midpoints mis-fired e.g.
// BACK -> a stray ENTER). Re-measure with board_front_key_mv() per board.
typedef struct { onepage_key_t key; uint16_t min_mv, max_mv; } adc_key_t;
static const adc_key_t s_adc_keys[] = {
    { ONEPAGE_KEY_BACK,  2400, 2800 },
    { ONEPAGE_KEY_LEFT,  1780, 2140 },
    { ONEPAGE_KEY_RIGHT, 1140, 1500 },
    { ONEPAGE_KEY_ENTER,    0,  250 },
};
#define ADC_KEY_COUNT (sizeof(s_adc_keys) / sizeof(s_adc_keys[0]))

static button_handle_t  s_handles[ONEPAGE_KEY_COUNT];
static onepage_key_cb_t s_cb;
static void            *s_cb_ud;

static const char *s_key_names[ONEPAGE_KEY_COUNT] = {
    "WAKE", "PREV", "NEXT", "BACK", "LEFT", "RIGHT", "ENTER",
};

static void btn_trampoline(void *arg, void *usr_data)
{
    if (esp_timer_get_time() - s_start_us < KEYS_STARTUP_GRACE_US) return;  // boot ADC-settle grace

    onepage_key_t key = (onepage_key_t)(intptr_t)usr_data;
    button_event_t be = iot_button_get_event((button_handle_t)arg);

    onepage_key_event_t ev;
    switch (be) {
    case BUTTON_PRESS_DOWN:        ev = ONEPAGE_KEY_DOWN;         break;
    case BUTTON_PRESS_UP:          ev = ONEPAGE_KEY_UP;           break;
    case BUTTON_SINGLE_CLICK:      ev = ONEPAGE_KEY_CLICK;        break;
    case BUTTON_DOUBLE_CLICK:      ev = ONEPAGE_KEY_DOUBLE;       break;
    case BUTTON_LONG_PRESS_START:  ev = ONEPAGE_KEY_LONG;         break;
    case BUTTON_LONG_PRESS_UP:     ev = ONEPAGE_KEY_LONG_RELEASE; break;
    default: return;
    }
    if (s_cb) s_cb(key, ev, s_cb_ud);
}

static esp_err_t register_events(button_handle_t h, onepage_key_t key)
{
    static const button_event_t evts[] = {
        BUTTON_PRESS_DOWN, BUTTON_PRESS_UP, BUTTON_SINGLE_CLICK,
        BUTTON_DOUBLE_CLICK, BUTTON_LONG_PRESS_START, BUTTON_LONG_PRESS_UP,
    };
    for (size_t i = 0; i < sizeof(evts) / sizeof(evts[0]); i++) {
        esp_err_t err = iot_button_register_cb(h, evts[i], NULL,
                                               btn_trampoline, (void *)(intptr_t)key);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t board_keys_init(const board_keys_cfg_t *cfg)
{
    s_start_us = esp_timer_get_time();   // begin boot ADC-settle grace window
    if (!cfg) cfg = &DEFAULT_CFG;
    const button_config_t bcfg = { .long_press_time = 800, .short_press_time = 0 };

    const struct { onepage_key_t key; int gpio; } gpio_keys[] = {
        { ONEPAGE_KEY_WAKE, cfg->gpio_wake },
        { ONEPAGE_KEY_PREV, cfg->gpio_prev },
        { ONEPAGE_KEY_NEXT, cfg->gpio_next },
    };
    for (size_t i = 0; i < sizeof(gpio_keys) / sizeof(gpio_keys[0]); i++) {
        if (gpio_keys[i].gpio < 0) continue;   // key disabled on this board
        button_gpio_config_t g = {
            .gpio_num = gpio_keys[i].gpio,
            .active_level = 0,          // active-low (pressed = grounded)
            .enable_power_save = false, // runtime scan only; deep-sleep wake done elsewhere
            .disable_pull = false,      // internal pull-up
        };
        esp_err_t err = iot_button_new_gpio_device(&bcfg, &g, &s_handles[gpio_keys[i].key]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GPIO key %s (GPIO%d) failed: %s",
                     s_key_names[gpio_keys[i].key], gpio_keys[i].gpio, esp_err_to_name(err));
            return err;
        }
        err = register_events(s_handles[gpio_keys[i].key], gpio_keys[i].key);
        if (err != ESP_OK) return err;
    }

    adc_unit_t unit = (cfg->adc_unit == 2) ? ADC_UNIT_2 : ADC_UNIT_1;
    // Reuse the battery's ADC1 handle (board_c61.c) instead of creating a 2nd unit.
    extern adc_oneshot_unit_handle_t *board_internal_adc1(void);
    adc_oneshot_unit_handle_t *shared_adc = (unit == ADC_UNIT_1) ? board_internal_adc1() : NULL;
    for (size_t i = 0; i < ADC_KEY_COUNT; i++) {
        button_adc_config_t a = {
            .adc_handle = shared_adc,
            .unit_id = unit,
            .adc_channel = cfg->adc_channel,
            .button_index = (uint8_t)i,
            .min = s_adc_keys[i].min_mv,
            .max = s_adc_keys[i].max_mv,
        };
        esp_err_t err = iot_button_new_adc_device(&bcfg, &a, &s_handles[s_adc_keys[i].key]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC key %s (%d~%dmV) failed: %s",
                     s_key_names[s_adc_keys[i].key], a.min, a.max, esp_err_to_name(err));
            return err;
        }
        err = register_events(s_handles[s_adc_keys[i].key], s_adc_keys[i].key);
        if (err != ESP_OK) return err;
    }

    ESP_LOGI(TAG, "7 keys ready (3 GPIO + 4 ADC ladder)");
    return ESP_OK;
}

void board_keys_set_cb(onepage_key_cb_t cb, void *user)
{
    s_cb = cb;
    s_cb_ud = user;
}

const char *onepage_key_name(onepage_key_t key)
{
    return (key < ONEPAGE_KEY_COUNT) ? s_key_names[key] : "?";
}

const char *onepage_key_event_name(onepage_key_event_t ev)
{
    switch (ev) {
    case ONEPAGE_KEY_DOWN:         return "DOWN";
    case ONEPAGE_KEY_UP:           return "UP";
    case ONEPAGE_KEY_CLICK:        return "CLICK";
    case ONEPAGE_KEY_DOUBLE:       return "DOUBLE";
    case ONEPAGE_KEY_LONG:         return "LONG";
    case ONEPAGE_KEY_LONG_RELEASE: return "LONG_UP";
    default:                       return "?";
    }
}

#endif /* CONFIG_IDF_TARGET_ESP32C61 */
