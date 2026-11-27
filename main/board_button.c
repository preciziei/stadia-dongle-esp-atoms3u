#include "board_button.h"

#include "dongle_state.h"
#include "hid_extra.h"
#include "status_led.h"
#include "usb_mode.h"
#include "web_server.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>

#ifndef CONFIG_DONGLE_BUTTON_ENABLED
#define CONFIG_DONGLE_BUTTON_ENABLED 0
#endif

#ifndef CONFIG_DONGLE_BUTTON_GPIO
#define CONFIG_DONGLE_BUTTON_GPIO 41
#endif

#ifndef CONFIG_DONGLE_IR_TX_HOLD_LOW
#define CONFIG_DONGLE_IR_TX_HOLD_LOW 0
#endif

#ifndef CONFIG_DONGLE_IR_TX_GPIO
#define CONFIG_DONGLE_IR_TX_GPIO 12
#endif

#define BUTTON_POLL_MS 20
#define BUTTON_DEBOUNCE_MS 40
#define BUTTON_DEBOUNCE_TICKS ((BUTTON_DEBOUNCE_MS + BUTTON_POLL_MS - 1) / BUTTON_POLL_MS)
#define BUTTON_HOLD_US 500000

static const char *TAG = "BOARD";

static void ir_hold_inactive(void)
{
#if CONFIG_DONGLE_IR_TX_HOLD_LOW
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_DONGLE_IR_TX_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_DONGLE_IR_TX_GPIO, 0));
    ESP_LOGI(TAG, "IR TX held low on GPIO %d", CONFIG_DONGLE_IR_TX_GPIO);
#endif
}

#if CONFIG_DONGLE_BUTTON_ENABLED

static void board_button_on_short_press(void)
{
    dongle_status_t st;
    dongle_state_get_status(&st);
    if (st.usb_suspended) {
        bool sent = hid_extra_remote_wakeup();
        ESP_LOGI(TAG, "Button: remote wake sent=%s", sent ? "yes" : "no");
    }
    ESP_LOGI(TAG, "Button: start Web UI");
    web_server_start(true);
}

static void board_button_toggle_mode(void)
{
    usb_mode_t next = (usb_mode_get() == USB_MODE_HID) ? USB_MODE_XINPUT : USB_MODE_HID;
    ESP_LOGI(TAG, "Button held 0.5s: switch to %s",
             next == USB_MODE_HID ? "HID" : "XInput");
    status_led_play_mode_change(next);
}

static void board_button_task(void *arg)
{
    (void)arg;
    int stable = 1; /* released (active-low) */
    int debounce = 0;
    int last_raw = 1;
    int64_t press_us = 0;
    bool hold_fired = false;

    while (1) {
        int raw = gpio_get_level(CONFIG_DONGLE_BUTTON_GPIO);
        if (raw != last_raw) {
            last_raw = raw;
            debounce = 0;
        } else if (debounce < BUTTON_DEBOUNCE_TICKS) {
            debounce++;
        } else if (raw != stable) {
            int prev = stable;
            stable = raw;
            if (prev == 1 && stable == 0) {
                press_us = esp_timer_get_time();
                hold_fired = false;
            } else if (prev == 0 && stable == 1) {
                int64_t held_us = press_us ? esp_timer_get_time() - press_us : 0;
                if (!hold_fired) {
                    if (held_us >= BUTTON_HOLD_US) {
                        hold_fired = true;
                        board_button_toggle_mode();
                    } else {
                        board_button_on_short_press();
                    }
                }
                press_us = 0;
            }
        }

        if (stable == 0 && !hold_fired && press_us != 0 &&
            esp_timer_get_time() - press_us >= BUTTON_HOLD_US) {
            hold_fired = true;
            board_button_toggle_mode();
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static void button_start(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_DONGLE_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    xTaskCreate(board_button_task, "board_btn", 4096, NULL, 1, NULL);
    ESP_LOGI(TAG, "Board button enabled on GPIO %d (active-low)", CONFIG_DONGLE_BUTTON_GPIO);
}

#endif /* CONFIG_DONGLE_BUTTON_ENABLED */

void board_hw_init(void)
{
    ir_hold_inactive();
#if CONFIG_DONGLE_BUTTON_ENABLED
    button_start();
#endif
}
