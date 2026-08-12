# bsp_onepage_c61

[![Component Registry](https://components.espressif.com/components/MoveCall/bsp_onepage_c61/badge.svg)](https://components.espressif.com/components/MoveCall/bsp_onepage_c61)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![ESP32-C61](https://img.shields.io/badge/Target-ESP32--C61-orange)

Board Support Package (BSP) for the **OnePage / 壹頁** e-paper reader — ESP32-C61 board.

[English](#english) | [中文说明](#中文说明)

---

<a name="english"></a>
## English

One-call hardware bring-up for the OnePage C61 board: e-paper display, 7-key input, microSD, battery / USB / charge sensing, PDM microphone, Wi-Fi / BLE, and external 32.768 kHz RTC crystal — all behind a clean, unified `board_*` C API. 

Your application code never needs to deal with raw GPIO numbers, SPI transactions, ADC calibration, or SoC peripheral registers directly.

### 🌟 Features & Capabilities

- **Display Backend**: Built-in support for Osptek **EPD0426A02** (4.26" 800×480 B/W, **SSD1677** controller) returned directly as a [`moui`](https://github.com/MoveCall/moui) UI backend (rotated to 480×800 portrait).
- **7-Key Input**: 3 side GPIO keys (Wake/Prev/Next) + 4 front ADC-ladder keys (Back/Left/Right/Enter), with optional one-line integration into `moui` event queues.
- **Storage**: MicroSD card mount/unmount over shared SPI bus with automatic card detect.
- **Power Management**: Battery ADC voltage sensing (auto 2x scaled for true cell mV), USB insertion detection, charging enable gate, and deep sleep entry with wakeup cause diagnostics.
- **Microphone**: PDM microphone capture with software CIC-3 decoding (16 kHz mono PCM output).
- **RTC & Wireless**: External 32.768 kHz crystal health check, Wi-Fi scanning, and BLE initialization.

`board_caps()` returns `WIFI | BLE | MIC | SD | BATTERY`.

---

### 📌 Pin Map (ESP32-C61)

| Signal | GPIO | Description & Notes |
|--------|------|---------------------|
| **SPI SCLK** | 22 | Shared bus (EPD + SD) |
| **SPI MOSI** | 23 | Shared bus (EPD + SD) |
| **SPI MISO** | 24 | SD only |
| **EPD CS** | 25 | EPD chip select |
| **EPD DC** | 8 | Strapping pin; requires external pull-up |
| **EPD RST** | 27 | **Shared power enable** for SD & MIC (active high) |
| **EPD BUSY** | 29 | EPD busy signal |
| **SD CS** | 26 | SD card chip select |
| **SD CD** | 28 | Card detect (active low = inserted) |
| **BAT ADC** | 5 | ADC1_CH3 (divided battery voltage sense) |
| **CHG_EN** | 10 | Battery charge control (pulled low while measuring) |
| **USB_DET** | 11 | USB insertion detection (high = plugged) |
| **KEY WAKE** | 2 | Side key (active low, LP wake-capable) |
| **KEY PREV** | 6 | Side key (active low) |
| **KEY NEXT** | 9 | Side key (active low, boot strap pin) |
| **FRONT KEYS** | 4 | ADC1_CH2 ladder: Back / Left / Right / Enter |
| **PDM CLK / DIN** | 7 / 3 | PDM Microphone interface |
| **RTC XTAL** | 0 / 1 | External 32.768 kHz crystal |

---

### 📦 Installation

Add `bsp_onepage_c61` to your ESP-IDF project:

```bash
idf.py add-dependency "MoveCall/bsp_onepage_c61"
```

#### Required `sdkconfig.defaults`

Ensure your project config includes the following settings required by the C61 hardware:

```ini
CONFIG_IDF_TARGET="esp32c61"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHFREQ_40M=y          # 80MHz fails image-hash on this board
CONFIG_PARTITION_TABLE_CUSTOM=y           # Wireless firmware requires >= 2MB app partition
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_RTC_CLK_SRC_EXT_CRYS=y             # External 32.768kHz crystal
CONFIG_BT_ENABLED=y
# moui display driver & fonts used by your UI:
CONFIG_MOUI_USE_DRV_SSD1677=y
CONFIG_MOUI_USE_FONT_INTER_24=y
```

---

### 🚀 Quick Start Example

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
    // 1. Initialize power rails and shared SPI bus
    board_init();

    // 2. Initialize display panel (SSD1677) & obtain moui backend
    moui_backend_t *be = board_display_init();

    // 3. Initialize keys (NULL = default C61 pin map)
    board_keys_init(NULL);
    board_keys_set_cb(key_cb, NULL);

    // 4. Initialize UI manager & render page
    moui_screen_mgr_init_be(&mgr, be, board_hal());
    moui_screen_init(&scr);
    moui_widget_init(&canvas, &vt);
    canvas.bounds = (moui_rect_t){0, 0, mgr.be->width, mgr.be->height};
    moui_screen_add_widget(&scr, &canvas);
    moui_screen_push(&mgr, &scr);

    moui_screen_mgr_set_refresh(&mgr, MOUI_REFRESH_SMART);
    moui_screen_mgr_mark_dirty(&mgr);
    moui_screen_mgr_tick(&mgr, 0, 0.1f);
}
```

---

### 📖 API Reference Summary (`board.h`)

| Domain | Key Functions |
|--------|---------------|
| **Init & System** | `board_init()`, `board_caps()`, `board_hal()`, `board_wake_cause()` |
| **Display** | `board_display_init()`, `board_display_force_full()`, `board_display_set_partial()`, `board_display_sleep()` |
| **Input / Keys** | `board_keys_init()`, `board_keys_set_cb()`, `board_keys_attach_moui()`, `board_front_key_mv()` |
| **Storage** | `board_sd_mount()`, `board_sd_unmount()`, `board_sd_size_mb()`, `board_sd_present()` |
| **Power** | `board_battery_mv()`, `board_usb_plugged()`, `board_charge_enable()`, `board_sleep_enter()` |
| **Microphone** | `board_mic_init()`, `board_mic_start()`, `board_mic_read()`, `board_mic_stop()` |
| **RTC / Wireless** | `board_rtc_xtal_ok()`, `board_rtc_slow_hz()`, `board_wifi_scan()`, `board_ble_init()` |

---

### ⚠️ Hardware Gotchas & Notes

- **Flash Speed (40 MHz)**: Do NOT set `CONFIG_ESPTOOLPY_FLASHFREQ_80M`. Flash frequency at 80 MHz triggers image hash corruption boot-loops on this board layout.
- **Shared SPI Buffer Limit**: ESP32-C61 limits single SPI transfers to $\le 32767$ bytes (`SPI_MS_DATA_BITLEN`). The BSP display bridge automatically handles 16 KB chunking.
- **GPIO 27 Multiplexing**: GPIO27 controls the EPD reset line *and* serves as the SD/MIC power rail enable. Pulling it low powers off SD/MIC. `board_init()` automatically sets it high.
- **Battery Sense Scaling**: The battery divider is $1:1$ (equal resistors). `board_battery_mv()` automatically calculates the $2\times$ factor and returns true cell mV.
- **Flashing Baud Rate**: If the USB-Serial-JTAG interface drops connection at high speeds, flash with `--no-stub -b 115200`.

---

<a name="中文说明"></a>
## 中文说明

**bsp_onepage_c61** 是 **壹頁 (OnePage)** 墨水屏阅读器 ESP32-C61 主板的开发板支持包（BSP）。

通过统一的 `board_*` C 语言接口，一行代码完成所有外设初始化。上层业务代码与 GUI 框架无需关心 GPIO 引脚定义、SPI 事务处理、ADC 转换或底层寄存器。

### 核心功能
* **墨水屏驱动**：适配 4.26 英寸 800×480 黑白电子纸面板（SSD1677 驱动），自动软件旋转为 480×800 竖屏，并封装为 [`moui`](https://github.com/MoveCall/moui) UI 后端。
* **按键管理**：支持 3 个侧边 GPIO 按键 + 4 个正面 ADC 阶梯分压按键，可无缝对接 `moui` 事件队列。
* **存储支持**：共享 SPI 总线挂载 MicroSD 卡，支持硬件卡检测。
* **电源管理**：电池电压测量（自动 $2\times$ 换算为真实毫伏值）、USB 插入检测、充电开关控制及深度休眠唤醒源管理。
* **PDM 麦克风**：具备 16 kHz 单声道 PCM 软件 CIC-3 解调采集。

---

## 📜 License

MIT License — see [LICENSE](LICENSE) for details.
