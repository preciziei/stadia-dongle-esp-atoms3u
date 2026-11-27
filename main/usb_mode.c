#include "usb_mode.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "USB_MODE";
static usb_mode_t s_mode = USB_MODE_XINPUT;

void usb_mode_init(void)
{
    nvs_handle_t nvs;
    uint8_t mode = USB_MODE_XINPUT;
    if (nvs_open("usbmode", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "mode", &mode);
        nvs_close(nvs);
    }
    if (mode != USB_MODE_HID) mode = USB_MODE_XINPUT;
    s_mode = (usb_mode_t)mode;
    ESP_LOGI(TAG, "USB mode %s", s_mode == USB_MODE_HID ? "HID" : "XInput");
}

usb_mode_t usb_mode_get(void)
{
    return s_mode;
}

static void clear_recovery_boots(void)
{
    nvs_handle_t nvs;
    if (nvs_open("recovery", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Could not open recovery NVS to clear boot counter");
        return;
    }
    nvs_set_u8(nvs, "boots", 0);
    nvs_commit(nvs);
    nvs_close(nvs);
}

void usb_mode_set_and_restart(usb_mode_t mode)
{
    if (mode != USB_MODE_HID) mode = USB_MODE_XINPUT;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("usbmode", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs, "mode", (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
        return;
    }

    clear_recovery_boots();
    ESP_LOGI(TAG, "USB mode saved (%s); restarting",
             mode == USB_MODE_HID ? "HID" : "XInput");
    esp_restart();
}
