/*
 * bsp_onepage_c61 — PDM microphone capture + software PDM->PCM decode.
 *
 * The ESP32-C61 has no hardware PDM2PCM, so I2S RX yields a RAW PDM bitstream.
 * On this board the DMA delivers pair-duplicated mono words; we take every other
 * word (stride 2) to de-duplicate and run a 3rd-order CIC decimator (R=128) to
 * produce 16 kHz voice PCM (= 2048000/128, confirmed with a reference tone),
 * followed by a DC blocker, an over-drive gain, a 1st-order low-pass, and a soft
 * limiter. Correct playback also requires that no samples are dropped, so the
 * capture is decoupled from SD writes in main.c (a slow SD write must never stall
 * the I2S reads); the enlarged DMA ring below covers short decode/preemption gaps.
 *
 * Usage:
 *   board_mic_init();               // once, after board_init()
 *   board_mic_start();              // begin a capture (enables the channel)
 *   while (recording) {
 *       int n = board_mic_read(pcm, 200);   // pcm must hold BOARD_MIC_FRAME_SAMPLES
 *       // ... write n samples ...
 *   }
 *   board_mic_stop();               // end the capture
 */
#include "board.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"

static const char *TAG = "board_mic";

/* Pins (see docs/hardware_io.md). */
#define PDM_CLK_GPIO        7
#define PDM_DIN_GPIO        3

/* Clock / decode plan. Configured oversample 2.048 MHz; raw data is pair-
 * duplicated so stride 2 de-duplicates. A 3rd-order CIC decimate-by-128 gives
 * 16 kHz PCM (= 2048000/128, confirmed with a 440 Hz reference tone) and the
 * cleanest voice: the tighter CIC low-pass rejects PDM quantization noise.
 * (R=64 doubled the output rate but let HF noise fold into the audio band ->
 * muddy, "male"-sounding. R=128 = correct timbre, confirmed on hardware.)
 * NOTE: the earlier "fast / slow / male / hiss" symptoms were all one bug --
 * dropped samples -- not a clock/pitch issue. Keep RAW_READ_WORDS small so a
 * decode pass can't starve the DMA ring (see board_mic_init), and keep capture
 * decoupled from SD writes in main.c. */
#define PDM_OVERSAMPLE_HZ   2048000
#define PDM_RAW_STRIDE      2            /* de-duplicate: raw data is pair-duplicated */
#define CIC_R               128          /* decimate-by-128: cleanest voice (rejects PDM HF noise) */
#define CIC_RCUBED          (128LL * 128LL * 128LL)
#define PDM_OUT_GAIN        120000       /* over-drive: speech modulation is shallow */
#define LP_NUM              11           /* 1st-order low-pass alpha = LP_NUM/16 */
#define RAW_READ_WORDS      2048         /* small chunk -> short (~1.6ms) decode gap so DMA isn't starved */

static i2s_chan_handle_t s_rx = NULL;
static int16_t s_raw[RAW_READ_WORDS];

/* Decoder state (reset on each board_mic_start). */
static uint32_t cic_i1, cic_i2, cic_i3, cic_c1, cic_c2, cic_c3;
static int      cic_cnt;
static int32_t  hp_xm1, hp_ym1, lp1;

static void decode_reset(void)
{
    cic_i1 = cic_i2 = cic_i3 = 0;
    cic_c1 = cic_c2 = cic_c3 = 0;
    cic_cnt = 0;
    hp_xm1 = hp_ym1 = lp1 = 0;
}

/* Soft limiter: gently compress peaks toward +/-32000 instead of hard-clipping,
 * which removes the buzzy distortion that hard clipping adds to loud speech. */
static inline int32_t soft_limit(int32_t v)
{
    const int32_t K = 22000;   /* linear below the knee */
    const int32_t H = 10000;   /* asymptote = K + H = 32000, never reaches +/-32767 */
    if (v >  K) { int32_t e =  v - K; return   K + (int32_t)((int64_t)e * H / (e + H)); }
    if (v < -K) { int32_t e = -v - K; return -(K + (int32_t)((int64_t)e * H / (e + H))); }
    return v;
}

/* Decode a raw-PDM buffer into PCM; returns number of PCM samples produced. */
static int decode(const int16_t *raw, size_t bytes, int16_t *pcm_out)
{
    const uint16_t *w = (const uint16_t *)raw;
    int nwords = (int)(bytes / 2);
    int n = 0;
    for (int iw = 0; iw < nwords; iw += PDM_RAW_STRIDE) {
        uint16_t word = w[iw];
        for (int b = 15; b >= 0; b--) {
            uint32_t bit = (word >> b) & 1u;
            cic_i1 += bit;
            cic_i2 += cic_i1;
            cic_i3 += cic_i2;
            if (++cic_cnt >= CIC_R) {
                cic_cnt = 0;
                uint32_t v = cic_i3;
                uint32_t d1 = v - cic_c1;  cic_c1 = v;
                uint32_t d2 = d1 - cic_c2; cic_c2 = d1;
                uint32_t d3 = d2 - cic_c3; cic_c3 = d2;
                int64_t centered = (int64_t)d3 * 2 - CIC_RCUBED;
                int32_t x = (int32_t)(centered * PDM_OUT_GAIN / CIC_RCUBED);
                int32_t y = x - hp_xm1 + hp_ym1 - (hp_ym1 >> 6);   /* DC blocker */
                hp_xm1 = x; hp_ym1 = y;
                lp1 += ((y - lp1) * LP_NUM) >> 4;     /* 1st-order low-pass, cut hiss */
                pcm_out[n++] = (int16_t)soft_limit(lp1);
            }
        }
    }
    return n;
}

esp_err_t board_mic_init(void)
{
    if (s_rx) return ESP_OK;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // ~10 KB DMA ring (~20 ms) so a decode gap or a burst of higher-priority task
    // preemption can't starve the ring and drop samples. Kept modest (default is
    // ~3 KB; 32 KB previously starved Wi-Fi's internal RAM) to stay Wi-Fi-safe.
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 640;
    esp_err_t e = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (e != ESP_OK) return e;

    i2s_pdm_rx_config_t cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_OVERSAMPLE_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = PDM_CLK_GPIO,
            .din = PDM_DIN_GPIO,
            .invert_flags = { .clk_inv = false },
        },
    };
    e = i2s_channel_init_pdm_rx_mode(s_rx, &cfg);
    if (e != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        ESP_LOGE(TAG, "PDM RX init failed: %d", e);
        return e;
    }
    ESP_LOGI(TAG, "PDM mic ready (CLK=%d DIN=%d, %d Hz PCM out)", PDM_CLK_GPIO, PDM_DIN_GPIO, BOARD_MIC_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t board_mic_start(void)
{
    if (!s_rx) return ESP_ERR_INVALID_STATE;
    decode_reset();
    return i2s_channel_enable(s_rx);
}

esp_err_t board_mic_stop(void)
{
    if (!s_rx) return ESP_ERR_INVALID_STATE;
    return i2s_channel_disable(s_rx);
}

int board_mic_read(int16_t *pcm_out, int timeout_ms)
{
    if (!s_rx || !pcm_out) return -1;
    size_t br = 0;
    if (i2s_channel_read(s_rx, (char *)s_raw, sizeof(s_raw), &br, timeout_ms) != ESP_OK) {
        return 0;   /* timeout / no data this round */
    }
    return decode(s_raw, br, pcm_out);
}
