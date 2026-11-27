/*
 * usb_xbox.c - USB descriptors for multiple Xbox 360-style gamepad
 * interfaces plus optional HID utility interface.
 */

#include "usb_xbox.h"

#include "bridge.h"
#include "dongle_config.h"
#include "dongle_state.h"
#include "hid_extra.h"
#include "hid_gamepad.h"
#include "usb_mode.h"
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
    const bool hid = usb_mode_get() == USB_MODE_HID;
    for (uint8_t i = 0; i < DONGLE_MAX_CONTROLLERS; i++) {
        const dongle_controller_status_t *c = &st.controllers[i];

        /*
         * Every advertised gamepad needs an explicit state after USB
         * attach/resume, including interfaces with no BLE controller.
         */
        bridge_send_neutral(c->usb_gamepad_index);
        if (!c->connected) continue;

        if (hid) {
            if (c->stadia.raw_len >= 9) {
                uint8_t report[DONGLE_HID_REPORT_SIZE];
                stadia_to_hid_gamepad(c->stadia.raw, report);
                bridge_send_report(c->usb_gamepad_index, report, sizeof(report));
            }
        } else if (c->xbox_valid) {
            bridge_send_report(c->usb_gamepad_index, c->xbox, DONGLE_XBOX_REPORT_SIZE);
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

/*
 * HID mode is per-interface class (device class 0x00), so no IAD. Three
 * gamepad interfaces only: the utility keyboard would show up on Android as
 * an extra device. No OUT endpoint — rumble is an XInput-only feature.
 * IN endpoints match the Xbox pad map: 0x81, 0x84, 0x82.
 */
#define HID_GAMEPAD_FUNCTION_LEN 25
#define HID_CFG_LEN (9 + (DONGLE_MAX_CONTROLLERS * HID_GAMEPAD_FUNCTION_LEN))

static const tusb_desc_device_t s_hid_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = 64,
    .idVendor = DONGLE_HID_USB_VID,
    .idProduct = DONGLE_HID_USB_PID,
    .bcdDevice = 0x0114,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const uint8_t s_hid_cfg_desc[HID_CFG_LEN] = {
    0x09, 0x02, HID_CFG_LEN & 0xff, HID_CFG_LEN >> 8,
    DONGLE_MAX_CONTROLLERS,
    0x01,
    0x00,
    0xa0,
    0xfa,

    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x04,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    HID_GAMEPAD_REPORT_DESC_LEN & 0xff, HID_GAMEPAD_REPORT_DESC_LEN >> 8,
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x01,

    0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x05,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    HID_GAMEPAD_REPORT_DESC_LEN & 0xff, HID_GAMEPAD_REPORT_DESC_LEN >> 8,
    0x07, 0x05, 0x84, 0x03, 0x10, 0x00, 0x01,

    0x09, 0x04, 0x02, 0x00, 0x01, 0x03, 0x00, 0x00, 0x06,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    HID_GAMEPAD_REPORT_DESC_LEN & 0xff, HID_GAMEPAD_REPORT_DESC_LEN >> 8,
    0x07, 0x05, 0x82, 0x03, 0x10, 0x00, 0x01,
};

_Static_assert(sizeof(s_hid_cfg_desc) == HID_CFG_LEN, "HID config descriptor size mismatch");
_Static_assert(HID_CFG_LEN == 9 + (3 * 25), "HID config descriptor length drifted");

static const char *s_hid_str_desc[] = {
    (char[]){0x09, 0x04},
    "Stadia Dongle",
    "HID Gamepad",
    "000000000001",
    "Gamepad 1",
    "Gamepad 2",
    "Gamepad 3",
};

static int send_active_report(uint8_t index, const uint8_t *data, size_t len)
{
    if (usb_mode_get() == USB_MODE_HID) return hid_gamepad_send_report(index, data, len);
    return xbox_send_report(index, data, len);
}

static void load_neutral_report(ble_to_usb_msg_t *msg, uint8_t index)
{
    memset(msg, 0, sizeof(*msg));
    msg->gamepad_index = index;
    if (usb_mode_get() == USB_MODE_HID) {
        stadia_hid_neutral(msg->data);
        msg->len = DONGLE_HID_REPORT_SIZE;
    } else {
        msg->data[0] = 0x00;
        msg->data[1] = 0x14;
        msg->len = DONGLE_XBOX_REPORT_SIZE;
    }
}

static void remember_report(uint8_t last_report[][DONGLE_XBOX_REPORT_SIZE],
                            size_t *last_len, bool *last_valid,
                            const ble_to_usb_msg_t *msg)
{
    if (msg->gamepad_index >= DONGLE_MAX_CONTROLLERS) return;
    size_t n = msg->len;
    if (n > DONGLE_XBOX_REPORT_SIZE) n = DONGLE_XBOX_REPORT_SIZE;
    uint8_t i = msg->gamepad_index;
    memset(last_report[i], 0, DONGLE_XBOX_REPORT_SIZE);
    if (n) memcpy(last_report[i], msg->data, n);
    last_len[i] = n;
    last_valid[i] = n > 0;
}

void usb_xbox_task(void *arg)
{
    (void)arg;
    ble_to_usb_msg_t msg = {0};
    ble_to_usb_msg_t pending[DONGLE_MAX_CONTROLLERS] = {0};
    bool have_report[DONGLE_MAX_CONTROLLERS] = {0};
    TickType_t last_sent[DONGLE_MAX_CONTROLLERS] = {0};
    uint8_t last_report[DONGLE_MAX_CONTROLLERS][DONGLE_XBOX_REPORT_SIZE];
    size_t last_len[DONGLE_MAX_CONTROLLERS] = {0};
    bool last_report_valid[DONGLE_MAX_CONTROLLERS] = {0};

    /* Start every virtual gamepad in the neutral report for the active mode. */
    for (uint8_t i = 0; i < DONGLE_MAX_CONTROLLERS; i++) {
        load_neutral_report(&pending[i], i);
        have_report[i] = true;
        remember_report(last_report, last_len, last_report_valid, &pending[i]);
    }

    while (1) {
        if (xQueueReceive(ble_to_usb_queue, &msg, pdMS_TO_TICKS(2)) == pdTRUE) {
            if (msg.gamepad_index < DONGLE_MAX_CONTROLLERS &&
                msg.len > 0 && msg.len <= DONGLE_XBOX_REPORT_SIZE) {
                pending[msg.gamepad_index] = msg;
                have_report[msg.gamepad_index] = true;
                remember_report(last_report, last_len, last_report_valid, &msg);
            }
        }

        while (xQueueReceive(ble_to_usb_queue, &msg, 0) == pdTRUE) {
            if (msg.gamepad_index < DONGLE_MAX_CONTROLLERS &&
                msg.len > 0 && msg.len <= DONGLE_XBOX_REPORT_SIZE) {
                pending[msg.gamepad_index] = msg;
                have_report[msg.gamepad_index] = true;
                remember_report(last_report, last_len, last_report_valid, &msg);
            }
        }

        for (uint8_t i = 0; i < DONGLE_MAX_CONTROLLERS; i++) {
            if (!have_report[i]) continue;
            int res = send_active_report(i, pending[i].data, pending[i].len);
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
                int res = send_active_report(i, last_report[i], last_len[i]);
                if (res == 1) last_sent[i] = now;
            }
        }
    }
}

void usb_xbox_init(void)
{
    xbox_dev_init();
    hid_gamepad_init();
    s_flag_lock = xSemaphoreCreateMutex();
    configASSERT(s_flag_lock != NULL);
    s_host_idle = false;
    s_last_in_log_us = esp_timer_get_time();

    const bool hid = usb_mode_get() == USB_MODE_HID;
    tinyusb_config_t cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = { .skip_setup = false, .self_powered = false },
        .task = { .size = 4096, .priority = 4, .xCoreID = 1 },
        .event_cb = tinyusb_event_handler,
        .event_arg = NULL,
    };
    cfg.descriptor.qualifier = NULL;
    cfg.descriptor.high_speed_config = NULL;
    if (hid) {
        cfg.descriptor.device = &s_hid_device_desc;
        cfg.descriptor.string = s_hid_str_desc;
        cfg.descriptor.string_count = 7;
        cfg.descriptor.full_speed_config = s_hid_cfg_desc;
    } else {
        cfg.descriptor.device = &s_device_desc;
        cfg.descriptor.string = s_str_desc;
        cfg.descriptor.string_count = 4;
        cfg.descriptor.full_speed_config = s_cfg_desc;
    }
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    /* HID mode does not open the Xbox driver, which is what normally starts
     * this timer. Without it the host looks idle from the first poll. */
    xbox_note_bus_activity();
    xTaskCreate(usb_state_monitor_task, "usb_state", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "USB initialised mode=%s controllers=%u utility_hid=%u cfg_len=%u",
             hid ? "HID" : "XInput", (unsigned)DONGLE_MAX_CONTROLLERS,
             hid ? 0u : (unsigned)DONGLE_ENABLE_UTILITY_HID,
             hid ? (unsigned)HID_CFG_LEN : (unsigned)XBOX_CFG_LEN);
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
    /* HID mode must not advertise XUSB20 or Windows binds WinUSB to the pads. */
    if (usb_mode_get() != USB_MODE_XINPUT) return NULL;
    return s_bos_desc;
}
