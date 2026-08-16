#pragma once

// Board Support Package (BSP) for the OnePage e-paper reader, ESP32-C61 board.
// The reader application and moui depend only on this header — never on GPIO
// numbers, esp_wifi/esp_bt, soc/* or adc_oneshot. Ship this component
// (movecall/bsp_onepage_c61) and call board_* to drive the hardware.

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "moui.h"
#include "board_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
} board_ap_t;

/* ── Keys: 7-key set (3 side GPIO + 4 front ADC ladder) ────────────────── */

typedef enum {
    ONEPAGE_KEY_WAKE = 0,   // side key (also deep-sleep wake source)
    ONEPAGE_KEY_PREV,       // side key — previous page
    ONEPAGE_KEY_NEXT,       // side key — next page
    ONEPAGE_KEY_BACK,       // front ADC — back
    ONEPAGE_KEY_LEFT,       // front ADC — left
    ONEPAGE_KEY_RIGHT,      // front ADC — right
    ONEPAGE_KEY_ENTER,      // front ADC — confirm
    ONEPAGE_KEY_COUNT,
} onepage_key_t;

typedef enum {
    ONEPAGE_KEY_DOWN = 0,
    ONEPAGE_KEY_UP,
    ONEPAGE_KEY_CLICK,
    ONEPAGE_KEY_DOUBLE,
    ONEPAGE_KEY_LONG,
    ONEPAGE_KEY_LONG_RELEASE,
} onepage_key_event_t;

typedef void (*onepage_key_cb_t)(onepage_key_t key, onepage_key_event_t ev, void *user);

// Key pin configuration. Pass NULL to board_keys_init() for the board's
// built-in C61 defaults; override here when porting to a different carrier.
typedef struct {
    int gpio_wake;      // side keys, active-low; set <0 to skip a key
    int gpio_prev;
    int gpio_next;
    int adc_unit;       // ADC unit for the front ladder (1 or 2)
    int adc_channel;    // ADC channel for the front ladder
} board_keys_cfg_t;

/* ── Init / capabilities ───────────────────────────────────────────────── */

esp_err_t board_init(void);            // power rail + shared SPI bus; call once first
uint32_t  board_caps(void);            // BOARD_CAP_* bitmask
const moui_hal_t *board_hal(void);     // moui timing/log HAL

/* ── Display (EPD) ─────────────────────────────────────────────────────── */

moui_backend_t *board_display_init(void);   // init panel, return moui backend
void            board_display_force_full(void);  // next flush = full refresh (clear ghosting)
void            board_display_set_partial(bool force_partial);  // partial-refresh mode (fast key nav)
void            board_display_force_full_partial_once(void);    // next partial flush = one clean full refresh
void            board_display_sleep(void);

/* ── Keys ──────────────────────────────────────────────────────────────── */

esp_err_t board_keys_init(const board_keys_cfg_t *cfg);  // NULL = board default
void      board_keys_set_cb(onepage_key_cb_t cb, void *user);
const char *onepage_key_name(onepage_key_t key);
const char *onepage_key_event_name(onepage_key_event_t ev);
// Diagnostic: front ADC-ladder voltage (mV) for recalibrating key thresholds.
int       board_front_key_mv(void);
// Route key presses into a moui input queue to drive UI navigation
// (PREV/LEFT→CCW, NEXT/RIGHT→CW, ENTER→PRESS, BACK/WAKE→BACK). Takes over the
// key callback; for custom handling set your own cb and call the mapping yourself.
void board_keys_attach_moui(moui_input_queue_t *queue);

/* ── Storage (microSD over the shared SPI bus) ─────────────────────────── */

esp_err_t board_sd_mount(const char *mount_point);
void      board_sd_unmount(const char *mount_point);
int       board_sd_size_mb(void);   // valid after a successful mount, else -1
bool      board_sd_present(void);   // card-detect line

/* ── Power ─────────────────────────────────────────────────────────────── */

int  board_battery_mv(void);        // divided ADC mV (pauses charging + avg + calibrate)
bool board_usb_plugged(void);
void board_charge_enable(bool on);

/* ── Power gate & deep sleep ───────────────────────────────────────────── */

// SD/MIC (and EPD) power rail via GPIO27. board_init() turns it on; pull low
// before deep sleep to cut SD/mic standby current.
void board_peripherals_power(bool on);

#define BOARD_WAKE_KEY    (1u << 0)   // KEY_WAKE (GPIO2, LP) low level
#define BOARD_WAKE_TIMER  (1u << 1)   // RTC timer

typedef enum {
    BOARD_WAKE_POWERON = 0,   // fresh boot / non-sleep reset
    BOARD_WAKE_BY_KEY,
    BOARD_WAKE_BY_TIMER,
    BOARD_WAKE_OTHER,
} board_wake_cause_t;

// Enter deep sleep: EPD deep sleep -> silence shared SPI/PDM lines (anti
// back-power) -> cut SD/MIC power -> arm wake sources -> esp_deep_sleep_start().
// Does NOT return. timer_ms used only if BOARD_WAKE_TIMER is set.
void board_sleep_enter(uint32_t wake_flags, uint32_t timer_ms);

// Wake/reset cause; query early in app_main to branch on how we woke up.
board_wake_cause_t board_wake_cause(void);

/* ── RTC slow clock (external 32.768kHz crystal) ───────────────────────── */

bool board_rtc_xtal_ok(void);       // true if XTAL32K is the active slow-clock source
int  board_rtc_slow_hz(void);       // measured slow-clock frequency (Hz)

/* ── Microphone (PDM) — optional, BOARD_CAP_MIC ────────────────────────── */

// PDM mic capture -> 16 kHz mono PCM (software CIC-3 decode + low-pass in board_mic.c).
// C61 has no hardware PDM2PCM; requires ESP-IDF >= 6.1-dev (5.5.x PDM RX driver bug).
#define BOARD_MIC_SAMPLE_RATE    16000
#define BOARD_MIC_FRAME_SAMPLES  512     // PCM samples returned per board_mic_read()
esp_err_t board_mic_init(void);          // once, after board_init()
esp_err_t board_mic_start(void);         // begin capture (resets decoder + enables channel)
esp_err_t board_mic_stop(void);          // end capture (disables channel)
int       board_mic_read(int16_t *pcm_out, int timeout_ms);  // up to BOARD_MIC_FRAME_SAMPLES; count, 0=timeout, <0=error

/* ── Wireless — native on C61 ──────────────────────────────────────────── */

int       board_wifi_scan(board_ap_t *out, int max);  // returns AP count, <0 on error
esp_err_t board_ble_init(void);

#ifdef __cplusplus
}
#endif
