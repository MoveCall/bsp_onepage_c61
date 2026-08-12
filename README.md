# bsp_onepage_c61

Board Support Package for the **OnePage / 壹頁** e-paper reader — ESP32-C61 board.

One-call hardware bring-up for the OnePage C61 board: e-paper display, 7-key
input, microSD, battery / USB / charge sensing, PDM microphone, Wi-Fi / BLE,
and the 32.768 kHz RTC crystal — all behind a small `board_*` API. Your
application never touches GPIO numbers, SPI, ADC or the SoC peripheral headers.

- **Display** is returned as a [moui](https://github.com/movecall/moui) backend
  (the OnePage UI framework). 
- **Input** is the OnePage 7-key layout (3 side GPIO keys + 4 front ADC-ladder keys).

## Capabilities

`board_caps()` returns: `WIFI | BLE | MIC | SD | BATTERY`.

## Pin map (ESP32-C61)

| Signal | GPIO | Notes |
|--------|------|-------|
| SPI SCLK | 22 | EPD + SD shared bus |
| SPI MOSI | 23 | shared |
| SPI MISO | 24 | SD only |
| EPD CS | 25 | |
| EPD DC | 8 | strapping; needs external pull-up |
| EPD RST | 27 | **also SD/MIC power-enable** (high = active) |
| EPD BUSY | 29 | |
| SD CS | 26 | |
| SD CD | 28 | card-detect (assumed low = inserted) |
| BAT ADC | 5 | ADC1_CH3 (divided battery voltage) |
| CHG_EN | 10 | charge control (pulled low while measuring) |
| USB_DET | 11 | USB present (high = plugged) |
| KEY WAKE | 2 | side key, active-low, LP wake-capable |
| KEY PREV | 6 | side key, active-low |
| KEY NEXT | 9 | side key, active-low (also boot strap) |
| FRONT keys | 4 | ADC1_CH2 ladder: Back / Left / Right / Enter |
| PDM CLK / DIN | 7 / 3 | microphone |
| RTC XTAL | 0 / 1 | 32.768 kHz crystal |

Display panel: Osptek **EPD0426A02**, 4.26" 800×480 B/W, controller **SSD1677**,
driven in native 800×480 then software-rotated to 480×800 portrait.

## Dependencies

- ESP-IDF ≥ 5.0 (tested v5.5.2), target `esp32c61`
- `espressif/button` — pulled automatically (see `idf_component.yml`)
- `moui` — display returns a `moui_backend_t`; add it to your project
  (`EXTRA_COMPONENT_DIRS` → `moui/src`, or as a git dependency)

## Install

```bash
idf.py add-dependency "movecall/bsp_onepage_c61"
```

Required `sdkconfig.defaults` (this board):

```
CONFIG_IDF_TARGET="esp32c61"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHFREQ_40M=y          # 80MHz fails image-hash on this board
CONFIG_PARTITION_TABLE_CUSTOM=y           # Wi-Fi/BLE firmware needs a big app partition
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"   # factory ≥ 2MB
CONFIG_RTC_CLK_SRC_EXT_CRYS=y             # external 32.768kHz crystal
CONFIG_BT_ENABLED=y
# moui display + fonts used by your UI:
CONFIG_MOUI_USE_DRV_SSD1677=y
CONFIG_MOUI_USE_FONT_INTER_24=y
```

## Quick start

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "moui.h"

static moui_screen_mgr_t mgr;
static moui_screen_t     scr;
static moui_widget_t     canvas;

static void draw(moui_widget_t *w, moui_draw_ctx_t *ctx) {
    moui_draw_fill_rect(ctx, &(moui_rect_t){0, 0, w->bounds.w, w->bounds.h}, MOUI_WHITE);
    moui_font_draw_str(ctx, &moui_font_inter_24, 40, 80, "Hello OnePage", MOUI_BLACK);
}
static const moui_widget_vtable_t vt = { .draw = draw };

static void key_cb(onepage_key_t key, onepage_key_event_t ev, void *user) {
    printf("KEY %s : %s\n", onepage_key_name(key), onepage_key_event_name(ev));
}

void app_main(void) {
    board_init();                                   // power rail + SPI bus
    moui_backend_t *be = board_display_init();      // EPD ready as a moui backend

    board_keys_init(NULL);                          // NULL = C61 default pins
    board_keys_set_cb(key_cb, NULL);

    moui_screen_mgr_init_be(&mgr, be, board_hal());
    moui_screen_init(&scr);
    moui_widget_init(&canvas, &vt);
    canvas.bounds = (moui_rect_t){0, 0, mgr.be->width, mgr.be->height};
    moui_screen_add_widget(&scr, &canvas);
    moui_screen_push(&mgr, &scr);

    moui_screen_mgr_set_refresh(&mgr, MOUI_REFRESH_SMART);
    moui_screen_mgr_mark_dirty(&mgr);               // trigger first full refresh
    moui_screen_mgr_tick(&mgr, 0, 0.1f);
}
```

## API overview (`board.h`)

| Group | Functions |
|-------|-----------|
| Init | `board_init`, `board_caps`, `board_hal` |
| Display | `board_display_init` → `moui_backend_t*`, `board_display_force_full`, `board_display_sleep` |
| Keys | `board_keys_init(cfg)`, `board_keys_set_cb`, `onepage_key_name`, `onepage_key_event_name` |
| Storage | `board_sd_mount`, `board_sd_unmount`, `board_sd_size_mb`, `board_sd_present` |
| Power | `board_battery_mv`, `board_usb_plugged`, `board_charge_enable` |
| RTC | `board_rtc_xtal_ok`, `board_rtc_slow_hz` |
| Mic | `board_mic_init`, `board_mic_start`, `board_mic_read`, `board_mic_stop` (16 kHz mono PCM) |
| Wireless | `board_wifi_scan`, `board_ble_init` |

## Configuration & porting

- **Key pins are parameterized.** Pass a `board_keys_cfg_t` to `board_keys_init()`
  (side-key GPIOs + front ADC unit/channel); `NULL` uses the C61 defaults. Set a
  GPIO to `<0` to disable that key.
- **ADC ladder thresholds** (front keys) are theoretical midpoints in
  `board_keys.c`; recalibrate against measured ADC values for production.

## Hardware notes / gotchas

- **Flash 40 MHz**: 80 MHz triggers `image is corrupt` boot-loops on this board.
- **SPI single transfer ≤ 32767 bytes** on C61 (`SPI_MS_DATA_BITLEN`); the bridge
  chunks at 16 KB. (S3's limit is 32768 — one bit higher.)
- **GPIO27 is shared**: EPD reset *and* SD/MIC power-enable. Driving it low cuts
  SD/mic power; `board_init` raises it.
- **SD_CD polarity** assumes low = inserted — verify on your hardware.
- **Battery**: the sense node is a **1:1 divider** (two equal resistors), so
  `board_battery_mv()` already applies the `x2` and returns the **true cell
  voltage** in mV — no further scaling needed by the caller.
- **RTC**: the external 32.768 kHz crystal needs its bias resistor (5M–10MΩ)
  populated; `board_rtc_xtal_ok()` reports false (RC fallback) otherwise.
- Flash this board with `--no-stub -b 115200` if the USB-Serial-JTAG link drops
  at high speed.

## License

MIT — see [LICENSE](LICENSE).
