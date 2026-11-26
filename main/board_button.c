#include "board_button.h"

#include "dongle_state.h"
#include "hid_extra.h"
#include "web_server.h"

#include "driver/gpio.h"
#include "esp_log.h"
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

static void board_button_task(void *arg)
{
    (void)arg;
    int stable = 1; /* released (active-low) */
    int debounce = 0;
    int last_raw = 1;

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
            /* Falling edge: pressed (active-low) */
            if (prev == 1 && stable == 0) {
                board_button_on_short_press();
            }
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
    xTaskCreate(board_button_task, "board_btn", 3072, NULL, 1, NULL);
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
