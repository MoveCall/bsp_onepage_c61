// BSP implementation for the Moink ESP32-C61 board.
// All SoC/pin-specific code for C61 lives here; the application sees only board.h.

#include "board.h"

#ifdef CONFIG_IDF_TARGET_ESP32C61

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "drivers/moui_drv_ssd1677.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_bt.h"
#include "soc/rtc.h"
#include "esp_private/esp_clk.h"
#include "esp_sleep.h"

static const char *TAG = "board_c61";

/* ── Pin map (see docs/hardware_io.md) ─────────────────────────────────── */

#define PIN_MOSI    GPIO_NUM_23
#define PIN_SCLK    GPIO_NUM_22
#define PIN_CS      GPIO_NUM_25   // EPD CS
#define PIN_DC      GPIO_NUM_8
#define PIN_RST     GPIO_NUM_27   // EPD_RST + SD/MIC power-enable (high = active)
#define PIN_BUSY    GPIO_NUM_29
#define SPI_HOST_ID SPI2_HOST     // EPD + SD share this bus
#define SPI_FREQ_HZ (10 * 1000 * 1000)

// C61 single-transfer limit is SPI_MS_DATA_BITLEN = 0x3FFFF bits (~32767 B),
// one bit below the S3's 1<<18. Keep chunks well under it.
#define SPI_CHUNK   16384

#define EPD_W 800                 // SSD1677 source (<=960); 480x800 is NOT drivable
#define EPD_H 480                 // gate (<=680)
#define EPD_BUF_SIZE (EPD_W * EPD_H / 8)

#define PIN_SD_CS    GPIO_NUM_26
#define PIN_SD_CD    GPIO_NUM_28   // card-detect
#define PIN_CHG_EN   GPIO_NUM_10   // charge control; pull low to read true battery V
#define PIN_USB_DET  GPIO_NUM_11  // LM66200 ST (open-drain, ext pull-up 3.3V): low = USB present
#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_ADC_CHANNEL  ADC_CHANNEL_3   // GPIO5 = ADC1_CH3
#define PIN_PDM_CLK  GPIO_NUM_7
#define PIN_PDM_DIN  GPIO_NUM_3
#define PIN_KEY_WAKE GPIO_NUM_2   // LP-capable deep-sleep wake key (active-low)

/* ── SPI bus + SSD1677 bridge ──────────────────────────────────────────── */

static spi_device_handle_t s_spi;

static esp_err_t spi_bus_setup(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);

    // Power the peripheral rail on (RST/PWR_EN high) and let it settle.
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_config_t in_cfg = { .pin_bit_mask = (1ULL << PIN_BUSY), .mode = GPIO_MODE_INPUT };
    gpio_config(&in_cfg);

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = GPIO_NUM_24,   // MISO (SD only)
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUF_SIZE,
    };
    esp_err_t ret = spi_bus_initialize(SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) return ret;

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = SPI_FREQ_HZ,
        .spics_io_num = PIN_CS,
        .queue_size = 1,
    };
    return spi_bus_add_device(SPI_HOST_ID, &devcfg, &s_spi);
}

static void bridge_write_cmd(uint8_t cmd, void *user)
{
    (void)user;
    gpio_set_level(PIN_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(s_spi, &t);
}

static void bridge_write_data(const uint8_t *data, uint32_t len, void *user)
{
    (void)user;
    gpio_set_level(PIN_DC, 1);
    while (len > 0) {
        uint32_t chunk = (len > SPI_CHUNK) ? SPI_CHUNK : len;
        spi_transaction_t t = { .length = chunk * 8, .tx_buffer = data };
        spi_device_polling_transmit(s_spi, &t);
        data += chunk;
        len -= chunk;
    }
}

static void bridge_write_data_byte(uint8_t d, void *user)
{
    (void)user;
    gpio_set_level(PIN_DC, 1);
    spi_transaction_t t = { .length = 8, .tx_buffer = &d };
    spi_device_polling_transmit(s_spi, &t);
}

static void bridge_wait_busy(void *user)
{
    (void)user;
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (gpio_get_level(PIN_BUSY) == 1) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - start) > 5000) {
            ESP_LOGW(TAG, "EPD busy timeout");
            break;
        }
    }
}

static void bridge_hw_reset(void *user)
{
    (void)user;
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ── moui HAL (timing / log) ───────────────────────────────────────────── */

static moui_time_ms_t hal_get_time(const moui_hal_t *hal)
{
    (void)hal;
    return (moui_time_ms_t)(esp_timer_get_time() / 1000);
}

static void hal_delay(const moui_hal_t *hal, uint32_t ms)
{
    (void)hal;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void hal_log(const moui_hal_t *hal, const char *fmt, ...)
{
    (void)hal;
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ESP_LOGI(TAG, "%s", buf);
}

static moui_hal_t s_hal = {
    .display_flush = NULL,
    .get_time_ms   = hal_get_time,
    .delay_ms      = hal_delay,
    .log           = hal_log,
    .priv          = NULL,
};

/* ── Display ───────────────────────────────────────────────────────────── */

static moui_drv_ssd1677_t s_epd;

moui_backend_t *board_display_init(void)
{
    moui_ssd1677_cfg_t cfg = {
        .write_cmd        = bridge_write_cmd,
        .write_data       = bridge_write_data,
        .write_data_byte  = bridge_write_data_byte,
        .wait_busy        = bridge_wait_busy,
        .hw_reset         = bridge_hw_reset,
        .user             = NULL,
        .width            = EPD_W,
        .height           = EPD_H,
        .gate_scan_dir    = 0x02,   // SM interlaced scan (GxEPD2-compatible)
        .use_internal_lut = true,   // OTP waveform
        .use_otp_voltages = true,   // skip 3.7" voltage regs
        .anti_ghosting    = true,   // partial: invert-restore the dirty rect so a moving cursor leaves no ghost (localized flash, no full-screen blink)
        .fast_temp        = 0x5A,   // fixed high temp -> fast full-refresh waveform
        .mirror_y         = true,   // panel gates reversed: reverse Y data
    };
    if (moui_drv_ssd1677_init(&s_epd, &cfg) != 0) {
        ESP_LOGE(TAG, "SSD1677 init failed");
        return NULL;
    }
    moui_backend_fb_set_rotation(&s_epd.fb_be, MOUI_ROTATION_270);  // -> portrait 480x800
    return moui_drv_ssd1677_backend(&s_epd);
}

void board_display_force_full(void)
{
    s_epd.initial_refresh = true;   // next flush does a clean full refresh
}

void board_display_sleep(void)
{
    moui_backend_t *be = moui_drv_ssd1677_backend(&s_epd);
    if (be->sleep) be->sleep(be);
}

void board_display_set_partial(bool force_partial)
{
    moui_drv_ssd1677_set_partial(&s_epd, force_partial);
}

/* ── Keys — implemented in board_keys.c ────────────────────────────────── */

/* ── Power (battery ADC / USB detect / charge control) ─────────────────── */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t          s_cali;
static bool                       s_cali_ok;

static void power_init(void)
{
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BAT_ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &ccfg));

    adc_cali_curve_fitting_config_t cal = {
        .unit_id = BAT_ADC_UNIT, .chan = BAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) == ESP_OK);

    gpio_config_t out = { .pin_bit_mask = 1ULL << PIN_CHG_EN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(PIN_CHG_EN, 1);  // allow charging by default

    // USB present = LM66200 ST status pin (open-drain, active-low): pulls GPIO11
    // low when USB power is valid; needs a pull-up so "unplugged" reads high.
    gpio_config_t in_usb = {
        .pin_bit_mask = 1ULL << PIN_USB_DET,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in_usb);

    gpio_config_t in_cd = {
        .pin_bit_mask = 1ULL << PIN_SD_CD,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // mechanical CD switch
    };
    gpio_config(&in_cd);
}

int board_battery_mv(void)
{
    gpio_set_level(PIN_CHG_EN, 0);          // pause charging to read true voltage
    vTaskDelay(pdMS_TO_TICKS(30));

    int acc = 0, raw = 0;
    for (int i = 0; i < 16; i++)
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) == ESP_OK) acc += raw;
    raw = acc / 16;

    gpio_set_level(PIN_CHG_EN, 1);          // resume charging

    int mv = 0;
    if (s_cali_ok) adc_cali_raw_to_voltage(s_cali, raw, &mv);
    return mv * 2;   // 1:1 divider (equal resistors): ADC reads half the cell -> x2 for true cell mV
}

bool board_usb_plugged(void) { return gpio_get_level(PIN_USB_DET) == 0; }  // LM66200 ST: low = USB present

void board_charge_enable(bool on) { gpio_set_level(PIN_CHG_EN, on ? 1 : 0); }

// Shared ADC1 oneshot handle (created in power_init for the battery). board_keys.c
// reuses it for the front ADC-ladder keys so it doesn't create a second ADC1 unit
// (adc_oneshot_new_unit on an already-claimed unit fails).
adc_oneshot_unit_handle_t *board_internal_adc1(void) { return &s_adc; }

// Diagnostic: read the front ADC-ladder node (GPIO5 = ADC1_CH2) in mV, for
// recalibrating the per-key thresholds in board_keys.c on a given board.
// The channel is configured by board_keys_init (button_adc); call after it.
// Uses the battery channel's curve-fit cali (close enough for ladder bands).
int board_front_key_mv(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_2, &raw) != ESP_OK) return -1;
    int mv = raw;
    if (s_cali_ok) adc_cali_raw_to_voltage(s_cali, raw, &mv);
    return mv;
}

/* ── Power gate & deep sleep ───────────────────────────────────────────── */

void board_peripherals_power(bool on)
{
    gpio_set_level(PIN_RST, on ? 1 : 0);   // GPIO27 high = SD/MIC/EPD powered
}

void board_sleep_enter(uint32_t wake_flags, uint32_t timer_ms)
{
    board_display_sleep();                 // park EPD controller cleanly first

    // Silence shared SPI/PDM lines so they can't back-power the cut-off rail.
    gpio_set_level(PIN_SCLK, 0);
    gpio_set_level(PIN_MOSI, 0);
    gpio_set_level(PIN_CS, 0);
    gpio_set_level(PIN_SD_CS, 0);
    gpio_set_level(PIN_PDM_CLK, 0);

    board_peripherals_power(false);        // cut SD/MIC power (GPIO27 low)

    if (wake_flags & BOARD_WAKE_KEY)
        // IDF 6.x: esp_deep_sleep_enable_gpio_wakeup() was removed. On C61 the HP
        // peripherals power down in deep sleep, so use the HP-powerdown variant
        // (see docs/hardware_io.md §7).
        esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << PIN_KEY_WAKE, ESP_GPIO_WAKEUP_GPIO_LOW);
    if ((wake_flags & BOARD_WAKE_TIMER) && timer_ms)
        esp_sleep_enable_timer_wakeup((uint64_t)timer_ms * 1000);

    esp_deep_sleep_start();                // does not return
}

board_wake_cause_t board_wake_cause(void)
{
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_GPIO:      return BOARD_WAKE_BY_KEY;
    case ESP_SLEEP_WAKEUP_TIMER:     return BOARD_WAKE_BY_TIMER;
    case ESP_SLEEP_WAKEUP_UNDEFINED: return BOARD_WAKE_POWERON;
    default:                         return BOARD_WAKE_OTHER;
    }
}

/* ── Storage (microSD over shared SPI) ─────────────────────────────────── */

static sdmmc_card_t *s_card;
static int           s_sd_mb = -1;

esp_err_t board_sd_mount(const char *mount_point)
{
    // Ensure MISO has a pull-up and the card rail is settled before probing.
    gpio_set_pull_mode(GPIO_NUM_24, GPIO_PULLUP_ONLY);   // MISO
    gpio_set_level(PIN_RST, 1);                          // GPIO27 high = SD powered
    vTaskDelay(pdMS_TO_TICKS(20));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI_HOST_ID;                 // reuse the already-initialized SPI2
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SPI_HOST_ID;
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t e = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot, &mcfg, &s_card);
    if (e != ESP_OK) {
        s_sd_mb = -1;
        return e;
    }
    s_sd_mb = (int)(((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    return ESP_OK;
}

void board_sd_unmount(const char *mount_point)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(mount_point, s_card);
        s_card = NULL;
        s_sd_mb = -1;
    }
}

int  board_sd_size_mb(void) { return s_sd_mb; }
bool board_sd_present(void) { return gpio_get_level(PIN_SD_CD) == 0; }  // assume low = inserted

/* ── RTC slow clock (external 32.768kHz crystal) ───────────────────────── */

bool board_rtc_xtal_ok(void)
{
    return rtc_clk_slow_src_get() == SOC_RTC_SLOW_CLK_SRC_XTAL32K;
}

int board_rtc_slow_hz(void)
{
    uint32_t cal = esp_clk_slowclk_cal_get();   // Q13.19: period = cal / 2^19 us
    return cal ? (int)(((uint64_t)1000000 << 19) / cal) : 0;
}

/* ── Microphone (PDM RX) ───────────────────────────────────────────────── */
// PDM capture + software PDM->PCM decode live in board_mic.c
// (board_mic_init / board_mic_start / board_mic_read / board_mic_stop).

/* ── Wireless (native on C61) ──────────────────────────────────────────── */
static bool s_net_inited;

static void net_init_once(void)
{
    if (s_net_inited) return;
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return;
    esp_netif_create_default_wifi_sta();
    s_net_inited = true;
}

int board_wifi_scan(board_ap_t *out, int max)
{
    net_init_once();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return -1;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    int count = -1;
    if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        count = n;
        if (out && max > 0) {
            uint16_t want = (n < max) ? n : (uint16_t)max;
            static wifi_ap_record_t recs[16];
            if (want > 16) want = 16;
            esp_wifi_scan_get_ap_records(&want, recs);
            for (int i = 0; i < want; i++) {
                strncpy(out[i].ssid, (char *)recs[i].ssid, sizeof(out[i].ssid) - 1);
                out[i].ssid[sizeof(out[i].ssid) - 1] = 0;
                out[i].rssi = recs[i].rssi;
                out[i].channel = recs[i].primary;
            }
        }
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    return count;
}

esp_err_t board_ble_init(void)
{
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_bt_controller_init(&cfg);
    if (e != ESP_OK) return e;
    return esp_bt_controller_enable(ESP_BT_MODE_BLE);
}

/* ── Init / capabilities / HAL ─────────────────────────────────────────── */

esp_err_t board_init(void)
{
    esp_err_t ret = spi_bus_setup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    power_init();
    return ESP_OK;
}

uint32_t board_caps(void)
{
    return BOARD_CAP_WIFI | BOARD_CAP_BLE | BOARD_CAP_MIC |
           BOARD_CAP_SD   | BOARD_CAP_BATTERY;
}

const moui_hal_t *board_hal(void) { return &s_hal; }

#endif /* CONFIG_IDF_TARGET_ESP32C61 */
