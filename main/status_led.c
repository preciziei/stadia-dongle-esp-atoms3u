#include "status_led.h"

#include "dongle_state.h"

#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef CONFIG_DONGLE_LED_ENABLED
#define CONFIG_DONGLE_LED_ENABLED 0
#endif

#ifndef CONFIG_DONGLE_LED_GPIO
#define CONFIG_DONGLE_LED_GPIO 35
#endif

#ifndef CONFIG_DONGLE_LED_BRIGHTNESS
#define CONFIG_DONGLE_LED_BRIGHTNESS 32
#endif

#if !defined(CONFIG_DONGLE_LED_TYPE_ADDRESSABLE) && !defined(CONFIG_DONGLE_LED_TYPE_NOOP)
#define CONFIG_DONGLE_LED_TYPE_ADDRESSABLE 1
#endif

#ifndef CONFIG_DONGLE_LED_TYPE_ADDRESSABLE
#define CONFIG_DONGLE_LED_TYPE_ADDRESSABLE 0
#endif

#define RMT_LED_RESOLUTION_HZ 10000000
#define LED_TASK_PERIOD_MS 40
#define LED_FADE_MS 320
#define WAKE_FLASH_US 250000

static const char *TAG = "LED";

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

static rmt_channel_handle_t s_led_chan;
static rmt_encoder_handle_t s_led_encoder;
static int64_t s_wake_flash_until_us;
static volatile bool s_mode_animating;

static const rmt_symbol_word_t ws2812_zero = {
    .level0 = 1,
    .duration0 = 0.3 * RMT_LED_RESOLUTION_HZ / 1000000,
    .level1 = 0,
    .duration1 = 0.9 * RMT_LED_RESOLUTION_HZ / 1000000,
};

static const rmt_symbol_word_t ws2812_one = {
    .level0 = 1,
    .duration0 = 0.9 * RMT_LED_RESOLUTION_HZ / 1000000,
    .level1 = 0,
    .duration1 = 0.3 * RMT_LED_RESOLUTION_HZ / 1000000,
};

static const rmt_symbol_word_t ws2812_reset = {
    .level0 = 0,
    .duration0 = RMT_LED_RESOLUTION_HZ / 1000000 * 50 / 2,
    .level1 = 0,
    .duration1 = RMT_LED_RESOLUTION_HZ / 1000000 * 50 / 2,
};

static size_t led_encoder_cb(const void *data, size_t data_size,
                             size_t symbols_written, size_t symbols_free,
                             rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;
    if (symbols_free < 8) return 0;

    const size_t data_pos = symbols_written / 8;
    const uint8_t *bytes = data;
    if (data_pos < data_size) {
        size_t pos = 0;
        for (int bit = 0x80; bit != 0; bit >>= 1) {
            symbols[pos++] = (bytes[data_pos] & bit) ? ws2812_one : ws2812_zero;
        }
        return pos;
    }

    symbols[0] = ws2812_reset;
    *done = true;
    return 1;
}

typedef enum {
    LED_PAT_ERROR = 0,
    LED_PAT_OTA,
    LED_PAT_SUSPEND,
    LED_PAT_BOOT,
    LED_PAT_PAIR,
    LED_PAT_CONNECT,
    LED_PAT_CONNECTED,
    LED_PAT_WEBUI,
    LED_PAT_IDLE,
} led_pattern_t;

static rgb_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (rgb_t) { .r = r, .g = g, .b = b };
}

/* Lift shadow detail, then apply the brightness cap. A zero channel stays
 * zero so hues stay pure (blue is not a dim white). Non-zero channels get a
 * floor so mixed colors survive on this LED. */
static uint8_t encode_channel(uint8_t perceptual)
{
    if (perceptual == 0) return 0;

    float lifted = powf((float)perceptual / 255.0f, 0.60f);
    int drive = (int)(lifted * (float)CONFIG_DONGLE_LED_BRIGHTNESS + 0.5f);
    int floor = CONFIG_DONGLE_LED_BRIGHTNESS / 6;
    if (floor < 12) floor = 12;
    if (floor > CONFIG_DONGLE_LED_BRIGHTNESS) floor = CONFIG_DONGLE_LED_BRIGHTNESS;
    if (drive < floor) drive = floor;
    if (drive > 255) drive = 255;
    return (uint8_t)drive;
}

static rgb_t encode_rgb(rgb_t c)
{
    return rgb(encode_channel(c.r), encode_channel(c.g), encode_channel(c.b));
}

static uint8_t breathe(uint32_t elapsed_ms, uint32_t period_ms, uint8_t lo, uint8_t hi)
{
    if (period_ms == 0 || hi <= lo) return hi;
    float phase = (float)(elapsed_ms % period_ms) / (float)period_ms;
    float w = 0.5f - 0.5f * cosf(phase * 6.2831853f);
    return (uint8_t)(lo + (uint8_t)((float)(hi - lo) * w));
}

static uint8_t lerp_u8(uint8_t a, uint8_t b, uint8_t t)
{
    return (uint8_t)(a + ((int)b - (int)a) * (int)t / 255);
}

static uint8_t smoothstep_u8(uint8_t t)
{
    uint32_t tt = (uint32_t)t * t;
    uint32_t three = 765u - 2u * t;
    return (uint8_t)((tt * three) / (255u * 255u));
}

static rgb_t lerp_rgb(rgb_t a, rgb_t b, uint8_t t)
{
    return rgb(lerp_u8(a.r, b.r, t), lerp_u8(a.g, b.g, t), lerp_u8(a.b, b.b, t));
}

static bool seeking_new_controller(const dongle_status_t *st)
{
    return st->pairing_mode ||
           st->state == DONGLE_STATE_PAIRING ||
           st->state == DONGLE_STATE_NO_BOND_SETUP ||
           (st->stored_bonds == 0 && st->state == DONGLE_STATE_SCANNING);
}

static bool reconnecting(const dongle_status_t *st)
{
    return st->state == DONGLE_STATE_CONNECTING ||
           (st->stored_bonds > 0 && st->connected_count == 0 &&
            (st->state == DONGLE_STATE_SCANNING || st->state == DONGLE_STATE_DISCONNECTED));
}

/* Solid white, with one brief dip per connected controller so 2 and 3 pads
 * are countable, and a cyan tick when the host is not reading the pad. */
static rgb_t connected_color(const dongle_status_t *st, uint32_t elapsed_ms)
{
    int count = st->connected_count;
    if (count < 1) count = 1;
    if (count > 3) count = 3;

    uint8_t level = 255;
    if (count >= 2) {
        uint32_t phase = elapsed_ms % 3000;
        for (int i = 0; i < count; i++) {
            uint32_t center = 180u + (uint32_t)i * 260u;
            uint32_t dist = phase > center ? phase - center : center - phase;
            if (dist < 80) {
                level = (uint8_t)((dist * 255u) / 80u);
                break;
            }
        }
    }

    uint32_t tick = elapsed_ms % 2800;
    if (st->usb_host_idle && tick < 120) return rgb(0, level, level);
    /* HID mode keeps the dip count, tinted orange so the mode is visible. */
    if (usb_mode_get() == USB_MODE_HID) return rgb(level, (uint8_t)(level / 3), 0);
    return rgb(level, level, level);
}

static rgb_t color_for_state(const dongle_status_t *st, uint32_t elapsed_ms, led_pattern_t *pattern)
{
    if (st->state == DONGLE_STATE_ERROR) {
        *pattern = LED_PAT_ERROR;
        return rgb(breathe(elapsed_ms, 480, 40, 255), 0, 0);
    }

    if (st->state == DONGLE_STATE_OTA_UPDATE) {
        *pattern = LED_PAT_OTA;
        uint8_t w = breathe(elapsed_ms, 900, 0, 255);
        return rgb((uint8_t)(255u - w), 0, 255);
    }

    if (st->usb_suspended) {
        *pattern = LED_PAT_SUSPEND;
        uint8_t p = breathe(elapsed_ms, 2600, 90, 255);
        return rgb(p, 0, p);
    }

    if (st->state == DONGLE_STATE_BOOTING) {
        *pattern = LED_PAT_BOOT;
        return rgb(255, 96, 0);
    }

    if (seeking_new_controller(st)) {
        *pattern = LED_PAT_PAIR;
        uint8_t p = breathe(elapsed_ms, 520, 30, 255);
        return rgb(p, (uint8_t)(p * 2 / 5), 0);
    }

    if (reconnecting(st)) {
        *pattern = LED_PAT_CONNECT;
        uint8_t p = breathe(elapsed_ms, 700, 40, 255);
        return rgb(p, p, p);
    }

    if (st->connected_count >= 1) {
        *pattern = LED_PAT_CONNECTED;
        return connected_color(st, elapsed_ms);
    }

    if (st->webui_active) {
        *pattern = LED_PAT_WEBUI;
        uint8_t p = breathe(elapsed_ms, 2400, 80, 255);
        return rgb(0, p, p);
    }

    *pattern = LED_PAT_IDLE;
    uint8_t p = breathe(elapsed_ms, 2400, 40, 180);
    return rgb(p, (uint8_t)(p * 2 / 5), 0);
}

static void led_set(rgb_t c)
{
#if CONFIG_DONGLE_LED_ENABLED && CONFIG_DONGLE_LED_TYPE_ADDRESSABLE
    if (!s_led_chan || !s_led_encoder) return;

    const uint8_t grb[3] = { c.g, c.r, c.b };
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(s_led_chan, s_led_encoder, grb, sizeof(grb), &tx_config);
    if (err == ESP_OK) {
        rmt_tx_wait_all_done(s_led_chan, pdMS_TO_TICKS(10));
    }
#else
    (void)c;
#endif
}

static bool led_hw_init(void)
{
#if CONFIG_DONGLE_LED_ENABLED && CONFIG_DONGLE_LED_TYPE_ADDRESSABLE
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = CONFIG_DONGLE_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_LED_RESOLUTION_HZ,
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT channel init failed: %s", esp_err_to_name(err));
        return false;
    }

    rmt_simple_encoder_config_t encoder_cfg = {
        .callback = led_encoder_cb,
    };
    err = rmt_new_simple_encoder(&encoder_cfg, &s_led_encoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT encoder init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = rmt_enable(s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT enable failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Status LED enabled on GPIO %d brightness %d", CONFIG_DONGLE_LED_GPIO,
             CONFIG_DONGLE_LED_BRIGHTNESS);
    return true;
#else
    ESP_LOGI(TAG, "Status LED disabled/no-op");
    return false;
#endif
}

static void status_led_task(void *arg)
{
    (void)arg;
    const bool hw_ok = led_hw_init();
    const int64_t start_us = esp_timer_get_time();
    led_pattern_t pattern = LED_PAT_BOOT;
    rgb_t shown = rgb(0, 0, 0);
    rgb_t fade_from = shown;
    uint16_t fade_ms = LED_FADE_MS;
    bool flash_was = false;
    bool started = false;

    while (1) {
        if (s_mode_animating) {
            vTaskDelay(pdMS_TO_TICKS(LED_TASK_PERIOD_MS));
            continue;
        }
        dongle_status_t st;
        dongle_state_get_status(&st);

        int64_t now_us = esp_timer_get_time();
        uint32_t elapsed_ms = (uint32_t)((now_us - start_us) / 1000);
        bool flash = now_us < s_wake_flash_until_us;
        led_pattern_t next = LED_PAT_BOOT;
        rgb_t target = color_for_state(&st, elapsed_ms, &next);

        if (flash) {
            shown = rgb(255, 255, 255);
            fade_ms = LED_FADE_MS;
            flash_was = true;
        } else {
            if (!started || flash_was || next != pattern) {
                fade_from = shown;
                fade_ms = 0;
                pattern = next;
                flash_was = false;
                started = true;
            }
            if (fade_ms < LED_FADE_MS) {
                fade_ms += LED_TASK_PERIOD_MS;
                if (fade_ms > LED_FADE_MS) fade_ms = LED_FADE_MS;
                uint8_t t = smoothstep_u8((uint8_t)((fade_ms * 255u) / LED_FADE_MS));
                shown = lerp_rgb(fade_from, target, t);
            } else {
                shown = target;
            }
        }

        if (hw_ok) led_set(encode_rgb(shown));
        vTaskDelay(pdMS_TO_TICKS(LED_TASK_PERIOD_MS));
    }
}

void status_led_init(void)
{
#if CONFIG_DONGLE_LED_ENABLED
    xTaskCreate(status_led_task, "status_led", 4096, NULL, 1, NULL);
#endif
}

void status_led_flash_remote_wake(void)
{
    s_wake_flash_until_us = esp_timer_get_time() + WAKE_FLASH_US;
}

void status_led_play_mode_change(usb_mode_t mode)
{
    s_mode_animating = true;
    vTaskDelay(pdMS_TO_TICKS(LED_TASK_PERIOD_MS * 2));

    rgb_t on = (mode == USB_MODE_HID) ? rgb(255, 85, 0) : rgb(0, 255, 0);
    rgb_t off = rgb(0, 0, 0);
    for (int i = 0; i < 5; i++) {
        led_set(encode_rgb(on));
        vTaskDelay(pdMS_TO_TICKS(120));
        led_set(encode_rgb(off));
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    usb_mode_set_and_restart(mode);
    s_mode_animating = false;
}
