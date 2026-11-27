/*
 * usb_xbox.c - USB descriptors for multiple Xbox 360-style gamepad
 * interfaces plus optional HID utility interface.
 */

#include "usb_xbox.h"

#include "bridge.h"
#include "dongle_config.h"
#include "dongle_state.h"
#include "hid_extra.h"
#include "web_server.h"
#include "xbox_dev.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "tusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "USB";
static SemaphoreHandle_t s_flag_lock;
static bool s_last_polled_suspended;
static volatile bool s_resync_reports_requested;
static volatile bool s_host_idle;
static uint32_t s_logged_in_count;
static int64_t s_last_in_log_us;

#define USB_HOST_IDLE_US 3000000u

static void set_resync_requested(bool val)
{
    if (s_flag_lock) xSemaphoreTake(s_flag_lock, portMAX_DELAY);
    s_resync_reports_requested = val;
    if (s_flag_lock) xSemaphoreGive(s_flag_lock);
}

static bool take_resync_if_bus_ready(void)
{
    /* tud_* is called outside s_flag_lock. The TinyUSB task can be inside
     * the event callback, holding its own lock and waiting for this one. */
    if (tud_suspended() || !tud_ready() || !s_flag_lock) return false;
    xSemaphoreTake(s_flag_lock, portMAX_DELAY);
    bool run = s_resync_reports_requested;
    if (run) s_resync_reports_requested = false;
    xSemaphoreGive(s_flag_lock);
    if (run && (tud_suspended() || !tud_ready())) {
        set_resync_requested(true);
        return false;
    }
    return run;
}

static void request_usb_report_resync(void)
{
    set_resync_requested(true);
}

static void enqueue_latest_reports(void)
{
    dongle_status_t st;
    dongle_state_get_status(&st);
    for (uint8_t i = 0; i < DONGLE_MAX_CONTROLLERS; i++) {
        const dongle_controller_status_t *c = &st.controllers[i];

        /*
         * Every advertised Xbox interface needs an explicit state after USB
         * attach/resume, including interfaces with no BLE controller.
         */
        bridge_send_neutral(c->usb_gamepad_index);

        if (c->connected && c->xbox_valid) {
            ble_to_usb_msg_t msg = {
                .gamepad_index = c->usb_gamepad_index,
                .len = DONGLE_XBOX_REPORT_SIZE,
            };
            memcpy(msg.data, c->xbox, DONGLE_XBOX_REPORT_SIZE);
            if (xQueueSendToBack(ble_to_usb_queue, &msg, 0) != pdTRUE) {
                ble_to_usb_msg_t dummy;
                xQueueReceive(ble_to_usb_queue, &dummy, 0);
                xQueueSendToBack(ble_to_usb_queue, &msg, 0);
            }
        }
    }
}

static void tinyusb_event_handler(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (!event) return;

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        dongle_state_set_usb_configured(true);
        dongle_state_set_usb_suspended(false);
        dongle_state_set_usb_host_idle(false);
        s_host_idle = false;
        xbox_note_bus_activity();
        request_usb_report_resync();
        ESP_LOGI(TAG, "USB attached/configured");
        break;
    case TINYUSB_EVENT_DETACHED:
        dongle_state_set_usb_configured(false);
        dongle_state_set_usb_suspended(false);
        dongle_state_set_usb_remote_wakeup_enabled(false);
        dongle_state_set_usb_host_idle(false);
        s_host_idle = false;
        ESP_LOGI(TAG, "USB detached");
        break;
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    case TINYUSB_EVENT_SUSPENDED:
        dongle_state_set_usb_suspended(true);
        dongle_state_set_usb_host_idle(false);
        s_host_idle = false;
        dongle_state_set_usb_remote_wakeup_enabled(event->suspended.remote_wakeup);
        web_server_notify_usb_suspend(true);
        ESP_LOGI(TAG, "USB suspended, remote_wakeup_enabled=%d", event->suspended.remote_wakeup);
        break;
#endif
#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
    case TINYUSB_EVENT_RESUMED:
        dongle_state_set_usb_suspended(false);
        dongle_state_set_usb_remote_wakeup_enabled(false);
        dongle_state_set_usb_host_idle(false);
        s_host_idle = false;
        xbox_note_bus_activity();
        request_usb_report_resync();
        web_server_notify_usb_suspend(false);
        ESP_LOGI(TAG, "USB resumed");
        break;
#endif
    default:
        break;
    }
}

static void usb_state_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        bool suspended = tud_suspended();
        if (suspended != s_last_polled_suspended) {
            s_last_polled_suspended = suspended;
            dongle_state_set_usb_suspended(suspended);
            web_server_notify_usb_suspend(suspended);
            if (suspended) {
                /* Remote wake is armed only by TINYUSB_EVENT_SUSPENDED.
                 * Assuming it here made the button look like it woke a host
                 * that had not actually suspended this device. */
                dongle_state_set_usb_host_idle(false);
                s_host_idle = false;
            } else {
                dongle_state_set_usb_remote_wakeup_enabled(false);
                dongle_state_set_usb_host_idle(false);
                s_host_idle = false;
                xbox_note_bus_activity();
                request_usb_report_resync();
            }
            ESP_LOGI(TAG, "USB poll state: suspended=%d ready=%d mounted=%d",
                     suspended, tud_ready(), tud_mounted());
        }
        if (take_resync_if_bus_ready()) {
            vTaskDelay(pdMS_TO_TICKS(120));
            xbox_recover_after_resume();
            enqueue_latest_reports();
            xbox_note_bus_activity();
            ESP_LOGI(TAG, "USB reports resynchronised after resume/attach");
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_in_log_us > 1000000) {
            uint32_t count = xbox_in_complete_count();
            if (count != s_logged_in_count) {
                ESP_LOGI(TAG, "USB IN completions %u (+%u over 1 s)",
                         (unsigned)count, (unsigned)(count - s_logged_in_count));
                s_logged_in_count = count;
            }
            s_last_in_log_us = now_us;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,
    .bDeviceClass = 0xEF,
    .bDeviceSubClass = 0x02,
    .bDeviceProtocol = 0x01,
    .bMaxPacketSize0 = 64,
    /* 0x028E is an Xbox 360 pad. 0x0289 is the original Xbox in Linux xpad,
     * so that driver parsed these 20-byte reports with the wrong layout.
     * Windows binds on the XUSB20 compatible ID and ignores the PID. */
    .idVendor = 0x045E,
    .idProduct = 0x028E,
    .bcdDevice = 0x0114,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

#define XBOX_FUNCTION_LEN 47
#define HID_UTILITY_FUNCTION_LEN 33
#define XBOX_CFG_LEN (9 + (DONGLE_MAX_CONTROLLERS * XBOX_FUNCTION_LEN) + \
                     (DONGLE_ENABLE_UTILITY_HID ? HID_UTILITY_FUNCTION_LEN : 0))
#define USB_NUM_INTERFACES (DONGLE_MAX_CONTROLLERS + (DONGLE_ENABLE_UTILITY_HID ? 1 : 0))

/* IN bInterval 1 = 1 ms. A genuine Xbox 360 pad requests 4 ms; polling four
 * times as often removes up to 3 ms of queueing delay. OUT (rumble) stays at 8 ms. */
#define XBOX_PAD_DESC(itf, ep_in, ep_out) \
    0x08, 0x0B, (itf), 0x01, 0xff, 0x5d, 0x01, 0x00, \
    0x09, 0x04, (itf), 0x00, 0x02, 0xff, 0x5d, 0x01, 0x00, \
    0x10, 0x21, 0x10, 0x01, 0x01, 0x24, (ep_in), 0x14, \
    0x03, 0x00, 0x03, 0x13, (ep_out), 0x00, 0x03, 0x00, \
    0x07, 0x05, (ep_in), 0x03, 0x20, 0x00, 0x01, \
    0x07, 0x05, (ep_out), 0x03, 0x20, 0x00, 0x08

static const uint8_t s_cfg_desc[XBOX_CFG_LEN] = {
    0x09, 0x02, XBOX_CFG_LEN & 0xff, XBOX_CFG_LEN >> 8,
    USB_NUM_INTERFACES,
    0x01,
    0x00,
    0xa0,
    0xfa,

    XBOX_PAD_DESC(0, 0x81, 0x02),
    XBOX_PAD_DESC(1, 0x84, 0x05),
    XBOX_PAD_DESC(2, 0x82, 0x03),

#if DONGLE_ENABLE_UTILITY_HID
    /*
     * HID utility: interface DONGLE_MAX_CONTROLLERS, EP 0x83 IN.
     * Advertise as a boot keyboard so Windows is more likely to expose and
     * arm this interface for USB wake. The report descriptor still carries
     * keyboard and consumer-control reports for normal operation.
     */
    0x08, 0x0B, DONGLE_MAX_CONTROLLERS, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x04, DONGLE_MAX_CONTROLLERS, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    HID_EXTRA_REPORT_DESC_LEN & 0xff, HID_EXTRA_REPORT_DESC_LEN >> 8,
    0x07, 0x05, 0x83, 0x03, 0x10, 0x00, 0x0a,
#endif
};

_Static_assert(XBOX_CFG_LEN == 9 + (3 * 47) + 33, "config descriptor length drifted");
_Static_assert(sizeof(s_cfg_desc) == XBOX_CFG_LEN, "config descriptor size mismatch");
_Static_assert(USB_NUM_INTERFACES == 4, "expected 3 Xbox interfaces plus utility HID");

static const char *s_str_desc[] = {
    (char[]){0x09, 0x04},
    "Microsoft",
    "Controller",
    "000000000001",
};

void usb_xbox_task(void *arg)
{
    (void)arg;
    ble_to_usb_msg_t msg = {0};
    ble_to_usb_msg_t pending[DONGLE_MAX_CONTROLLERS] = {0};
    bool have_report[DONGLE_MAX_CONTROLLERS] = {0};
    TickType_t last_sent[DONGLE_MAX_CONTROLLERS] = {0};
    uint8_t last_report[DONGLE_MAX_CONTROLLERS][DONGLE_XBOX_REPORT_SIZE];
    bool last_report_valid[DONGLE_MAX_CONTROLLERS] = {0};

    /* Start every virtual gamepad in a known neutral state. */
    for (uint8_t i = 0; i < DONGLE_MAX_CONTROLLERS; i++) {
        pending[i].gamepad_index = i;
        pending[i].data[0] = 0x00;
        pending[i].data[1] = 0x14;
        pending[i].len = DONGLE_XBOX_REPORT_SIZE;
        have_report[i] = true;
        memcpy(last_report[i], pending[i].data, DONGLE_XBOX_REPORT_SIZE);
        last_report_valid[i] = true;
    }

    while (1) {
        if (xQueueReceive(ble_to_usb_queue, &msg, pdMS_TO_TICKS(2)) == pdTRUE) {
            if (msg.gamepad_index < DONGLE_MAX_CONTROLLERS) {
                pending[msg.gamepad_index] = msg;
                have_report[msg.gamepad_index] = true;
                if (msg.len >= DONGLE_XBOX_REPORT_SIZE) {
                    memcpy(last_report[msg.gamepad_index], msg.data, DONGLE_XBOX_REPORT_SIZE);
                    last_report_valid[msg.gamepad_index] = true;
                }
            }
        }

        while (xQueueReceive(ble_to_usb_queue, &msg, 0) == pdTRUE) {
            if (msg.gamepad_index < DONGLE_MAX_CONTROLLERS) {
                pending[msg.gamepad_index] = msg;
                have_report[msg.gamepad_index] = true;
                if (msg.len >= DONGLE_XBOX_REPORT_SIZE) {
                    memcpy(last_report[msg.gamepad_index], msg.data, DONGLE_XBOX_REPORT_SIZE);
                    last_report_valid[msg.gamepad_index] = true;
                }
            }
        }

        for (uint8_t i = 0; i < DONGLE_MAX_CONTROLLERS; i++) {
            if (!have_report[i]) continue;
            int res = xbox_send_report(i, pending[i].data, pending[i].len);
            if (res == 1 || res == -1) {
                have_report[i] = false;
                last_sent[i] = xTaskGetTickCount();
            }
            /* res == 0: IN endpoint still waiting for the host. Keep the
             * pending report queued. A busy endpoint is not a failed
             * enumeration — Linux xpad does not submit an IN URB until
             * something opens the pad. */
        }

        /* Host idle is "configured, not suspended, no completed IN transfer".
         * It must not set usb_suspended: that flag stops the AP and drives
         * the sleep LED / remote-wake path. */
        bool idle = tud_mounted() && tud_ready() && !tud_suspended() &&
                    xbox_us_since_in_complete() > USB_HOST_IDLE_US;
        if (idle != s_host_idle) {
            s_host_idle = idle;
            dongle_state_set_usb_host_idle(idle);
            ESP_LOGI(TAG, "USB host %s", idle ? "idle (no IN completion for 3 s)" : "polling");
        }

        // Keepalive: replay the latest desired report every 1000 ms to keep
        // Windows from thinking the controller disappeared.  Replaying the
        // desired state preserves held buttons while ensuring a disconnect
        // neutral received during USB suspend replaces any stale held state.
        TickType_t now = xTaskGetTickCount();
        for (uint8_t i = 0; i < DONGLE_MAX_CONTROLLERS; i++) {
            if (!have_report[i] && (now - last_sent[i]) > pdMS_TO_TICKS(1000) &&
                tud_ready() && !tud_suspended() && last_report_valid[i]) {
                int res = xbox_send_report(i, last_report[i], DONGLE_XBOX_REPORT_SIZE);
                if (res == 1) last_sent[i] = now;
            }
        }
    }
}

void usb_xbox_init(void)
{
    xbox_dev_init();
    s_flag_lock = xSemaphoreCreateMutex();
    configASSERT(s_flag_lock != NULL);
    s_host_idle = false;
    s_last_in_log_us = esp_timer_get_time();

    tinyusb_config_t cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = { .skip_setup = false, .self_powered = false },
        .task = { .size = 4096, .priority = 4, .xCoreID = 1 },
        .descriptor = {
            .device = &s_device_desc,
            .qualifier = NULL,
            .string = s_str_desc,
            .string_count = 4,
            .full_speed_config = s_cfg_desc,
            .high_speed_config = NULL,
        },
        .event_cb = tinyusb_event_handler,
        .event_arg = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    xTaskCreate(usb_state_monitor_task, "usb_state", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "Xbox USB initialised controllers=%u utility_hid=%u cfg_len=%u",
             (unsigned)DONGLE_MAX_CONTROLLERS, (unsigned)DONGLE_ENABLE_UTILITY_HID,
             (unsigned)XBOX_CFG_LEN);
}

#define MS_OS_20_SET_HEADER_DESCRIPTOR        0x00
#define MS_OS_20_SUBSET_HEADER_CONFIGURATION  0x01
#define MS_OS_20_SUBSET_HEADER_FUNCTION       0x02
#define MS_OS_20_FEATURE_COMPATBLE_ID         0x03

#define MS_OS_20_FUNCTION_LEN 0x1C
#define MS_OS_20_DESC_LEN (0x0A + 0x08 + (DONGLE_MAX_CONTROLLERS * MS_OS_20_FUNCTION_LEN))

#define MS_OS_20_PAD(itf) \
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), \
    (itf), 0, U16_TO_U8S_LE(MS_OS_20_FUNCTION_LEN), \
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), \
    'X', 'U', 'S', 'B', '2', '0', 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

const uint8_t s_ms_os_20_desc[MS_OS_20_DESC_LEN] = {
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    MS_OS_20_PAD(0),
    MS_OS_20_PAD(1),
    MS_OS_20_PAD(2),
};

_Static_assert(sizeof(s_ms_os_20_desc) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor size mismatch");
_Static_assert(MS_OS_20_DESC_LEN == 0x0A + 0x08 + (3 * 0x1C), "MS OS 2.0 length drifted");

#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t s_bos_desc[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, 1)
};

uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_bos_desc;
}
