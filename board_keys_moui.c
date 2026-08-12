// OnePage C61 — optional moui input adapter: turn physical key presses into
// moui navigation events pushed onto a screen-manager input queue. Fires on
// key-DOWN for immediate, one-per-press response (CLICK adds a double-click
// wait and merges fast repeats into DOUBLE — wrong for menu nav), with a short
// per-key software debounce to swallow front ADC-ladder bounce.

#include "board.h"

#ifdef CONFIG_IDF_TARGET_ESP32C61

#include "esp_log.h"
#include "esp_timer.h"
#include "input/moui_input.h"
#include "input/moui_indev.h"   // moui_key_t

static const char *TAG = "keys_moui";

#define NAV_DEBOUNCE_US 120000   // ignore same-key re-fire within 120 ms

// Physical key -> moui_key_t (reader semantics).
static const moui_key_t s_map[ONEPAGE_KEY_COUNT] = {
    [ONEPAGE_KEY_WAKE]  = MOUI_KEY_BACK,
    [ONEPAGE_KEY_PREV]  = MOUI_KEY_UP,
    [ONEPAGE_KEY_NEXT]  = MOUI_KEY_DOWN,
    [ONEPAGE_KEY_BACK]  = MOUI_KEY_BACK,
    [ONEPAGE_KEY_LEFT]  = MOUI_KEY_LEFT,
    [ONEPAGE_KEY_RIGHT] = MOUI_KEY_RIGHT,
    [ONEPAGE_KEY_ENTER] = MOUI_KEY_ENTER,
};

static moui_input_queue_t *s_queue;

static moui_event_type_t key_to_ev(moui_key_t k)
{
    switch (k) {
    case MOUI_KEY_UP:
    case MOUI_KEY_LEFT:  return MOUI_EV_ENCODER_CCW;
    case MOUI_KEY_DOWN:
    case MOUI_KEY_RIGHT: return MOUI_EV_ENCODER_CW;
    case MOUI_KEY_ENTER: return MOUI_EV_ENCODER_PRESS;
    case MOUI_KEY_BACK:
    case MOUI_KEY_HOME:  return MOUI_EV_ENCODER_BACK;
    default:             return MOUI_EV_NONE;
    }
}

static void feed(onepage_key_t key, onepage_key_event_t ev, void *user)
{
    (void)user;
    if (!s_queue || key >= ONEPAGE_KEY_COUNT || ev != ONEPAGE_KEY_DOWN) return;
    moui_event_type_t t = key_to_ev(s_map[key]);
    if (t == MOUI_EV_NONE) return;

    // ENTER re-confirm: espressif/button's get_adc_voltage() returns 0 mV on a
    // failed/uncalibrated adc_oneshot_read, and ENTER's band starts at 0 (the key
    // is a 0-ohm short to GND, real value ~0 mV), so any glitchy read looks like
    // an ENTER press. Re-read the ladder node here: a real press still sits at ~0,
    // a transient/failed read has snapped back to rest (~3100). Drop if not at ~0.
    // (Boot's sustained ~0 window is handled separately by the startup grace.)
    if (key == ONEPAGE_KEY_ENTER) {
        int mv = board_front_key_mv();
        if (mv < 0 || mv > 300) {
            ESP_LOGW(TAG, "ENTER rejected (adc=%dmV, node not pressed)", mv);
            return;
        }
    }

    // Per-key debounce: a single press of an ADC-ladder key can wobble across
    // the band edge and re-fire DOWN; swallow repeats within the window.
    static int64_t last_us[ONEPAGE_KEY_COUNT];
    int64_t now = esp_timer_get_time();
    if (now - last_us[key] < NAV_DEBOUNCE_US) return;
    last_us[key] = now;

    ESP_LOGI(TAG, "nav: %s", onepage_key_name(key));
    moui_input_event_t e = { .type = t };
    moui_input_queue_push(s_queue, &e);
}

void board_keys_attach_moui(moui_input_queue_t *queue)
{
    s_queue = queue;
    board_keys_set_cb(feed, NULL);
}

#endif /* CONFIG_IDF_TARGET_ESP32C61 */
