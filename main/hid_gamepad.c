/*
 * hid_gamepad.c — custom TinyUSB class driver for three standard HID gamepads.
 *
 * Interfaces 0–2, interrupt IN only (EP 0x81, 0x84, 0x82), no boot subclass,
 * 1 ms bInterval. No OUT endpoint, so rumble is not available. Endpoints match
 * the XInput pad map so the utility keyboard (EP 0x83) can stay off.
 * Modelled on hid_extra.c: GET_DESCRIPTOR(report) plus SET_IDLE / GET_REPORT.
 */

#include "hid_gamepad.h"

#include "bridge.h"
#include "dongle_config.h"
#include "xbox_dev.h"

#include "class/hid/hid.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "esp_log.h"
#include "tusb.h"

#include <string.h>

static const char *TAG = "HID_PAD";

typedef struct {
    uint8_t index;
    uint8_t interface_number;
    uint8_t ep_in;
    uint8_t idle;
    bool open;
    uint8_t last_report[DONGLE_HID_REPORT_SIZE];
    uint8_t ctrl_buf[DONGLE_HID_REPORT_SIZE] TU_ATTR_ALIGNED(4);
    uint8_t in_buf[16] TU_ATTR_ALIGNED(4);
} hid_pad_t;

static uint8_t s_rhport;
static hid_pad_t s_pads[DONGLE_MAX_CONTROLLERS] = {
    { .index = 0, .interface_number = 0, .ep_in = 0x81 },
    { .index = 1, .interface_number = 1, .ep_in = 0x84 },
    { .index = 2, .interface_number = 2, .ep_in = 0x82 },
};

/*
 * Generic Desktop / Game Pad, no report ID.
 * 16 buttons (1-based usages, unused bits stay 0):
 *   1 A, 2 B, 4 X, 5 Y, 7 LB, 8 RB, 11 Select, 12 Start, 13 Guide, 14 L3, 15 R3
 * Hat switch logical 0..7, null when the 4-bit field is 8.
 * X/Y left stick, Z/Rz right stick, 8-bit, center 0x80.
 * Brake / Accelerator (Simulation Controls page 0x02) are LT / RT.
 * Report is 9 bytes: 2 button + 1 hat + 4 axes + 2 triggers.
 */
static const uint8_t s_hid_report_desc[HID_GAMEPAD_REPORT_DESC_LEN] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x05,        /* Usage (Game Pad) */
    0xA1, 0x01,        /* Collection (Application) */
    0x05, 0x09,        /*   Usage Page (Button) */
    0x19, 0x01,        /*   Usage Minimum (Button 1) */
    0x29, 0x10,        /*   Usage Maximum (Button 16) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x10,        /*   Report Count (16) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */
    0x09, 0x39,        /*   Usage (Hat switch) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x07,        /*   Logical Maximum (7) */
    0x35, 0x00,        /*   Physical Minimum (0) */
    0x46, 0x3B, 0x01,  /*   Physical Maximum (315) */
    0x65, 0x14,        /*   Unit (English Rotation, Angular Position) */
    0x75, 0x04,        /*   Report Size (4) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x42,        /*   Input (Data,Var,Abs,Null) */
    0x65, 0x00,        /*   Unit (None) */
    0x45, 0x00,        /*   Physical Maximum (0) */
    0x75, 0x04,        /*   Report Size (4) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x03,        /*   Input (Const,Var,Abs) padding */
    0x09, 0x30,        /*   Usage (X) */
    0x09, 0x31,        /*   Usage (Y) */
    0x09, 0x32,        /*   Usage (Z) */
    0x09, 0x35,        /*   Usage (Rz) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,  /*   Logical Maximum (255) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x04,        /*   Report Count (4) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0x05, 0x02,        /*   Usage Page (Simulation Controls) */
    0x09, 0xC5,        /*   Usage (Brake) */
    0x09, 0xC4,        /*   Usage (Accelerator) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,  /*   Logical Maximum (255) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x02,        /*   Report Count (2) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0xC0,              /* End Collection */
};

_Static_assert(sizeof(s_hid_report_desc) == HID_GAMEPAD_REPORT_DESC_LEN,
               "HID gamepad report descriptor length drifted");

const uint8_t *hid_gamepad_report_descriptor(void)
{
    return s_hid_report_desc;
}

static hid_pad_t *pad_by_itf(uint8_t itf)
{
    for (uint8_t i = 0; i < DONGLE_MAX_CONTROLLERS; i++) {
        if (s_pads[i].interface_number == itf) return &s_pads[i];
    }
    return NULL;
}

static void pads_reset_state(void)
{
    for (uint8_t i = 0; i < DONGLE_MAX_CONTROLLERS; i++) {
        s_pads[i].open = false;
        s_pads[i].idle = 0;
        stadia_hid_neutral(s_pads[i].last_report);
    }
}

void hid_gamepad_init(void)
{
    pads_reset_state();
}

static void hid_driver_init(void)
{
    pads_reset_state();
}

static bool hid_driver_deinit(void)
{
    pads_reset_state();
    return true;
}

static void hid_driver_reset(uint8_t rhport)
{
    (void)rhport;
    pads_reset_state();
}

static uint16_t hid_driver_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc,
                                uint16_t max_len)
{
    if (itf_desc->bInterfaceClass != TUSB_CLASS_HID) return 0;
    hid_pad_t *pad = pad_by_itf(itf_desc->bInterfaceNumber);
    if (!pad) return 0;

    uint16_t const drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) +
                                        sizeof(tusb_hid_descriptor_hid_t) +
                                        itf_desc->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
    TU_VERIFY(max_len >= drv_len, 0);

    uint8_t const *p_desc = tu_desc_next(itf_desc);
    if (tu_desc_type(p_desc) != HID_DESC_TYPE_HID) return 0;
    p_desc = tu_desc_next(p_desc);

    uint8_t ep_out = 0;
    uint8_t ep_in = 0;
    TU_ASSERT(usbd_open_edpt_pair(rhport, p_desc, itf_desc->bNumEndpoints,
                                  TUSB_XFER_INTERRUPT, &ep_out, &ep_in), 0);
    (void)ep_out;

    s_rhport = rhport;
    pad->ep_in = ep_in;
    pad->open = true;
    ESP_LOGI(TAG, "HID gamepad %u opened: itf=%u ep_in=0x%02x",
             pad->index, itf_desc->bInterfaceNumber, ep_in);
    return drv_len;
}

static bool hid_driver_control(uint8_t rhport, uint8_t stage,
                               tusb_control_request_t const *request)
{
    if (request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE) return false;
    hid_pad_t *pad = pad_by_itf(tu_u16_low(request->wIndex));
    if (!pad) return false;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        if (request->bRequest != TUSB_REQ_GET_DESCRIPTOR) return false;
        uint8_t desc_type = tu_u16_high(request->wValue);
        if (desc_type == HID_DESC_TYPE_REPORT) {
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_hid_report_desc,
                                    sizeof(s_hid_report_desc));
        }
        if (desc_type == HID_DESC_TYPE_HID) {
            static const uint8_t hid_desc[] = {
                0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
                HID_GAMEPAD_REPORT_DESC_LEN & 0xff, HID_GAMEPAD_REPORT_DESC_LEN >> 8,
            };
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)hid_desc,
                                    sizeof(hid_desc));
        }
        return false;
    }

    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) return false;

    if (request->bRequest == HID_REQ_CONTROL_SET_IDLE) {
        if (stage == CONTROL_STAGE_SETUP) {
            pad->idle = tu_u16_high(request->wValue);
            return tud_control_status(rhport, request);
        }
        return true;
    }

    if (request->bRequest == HID_REQ_CONTROL_GET_IDLE &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        return tud_control_xfer(rhport, request, &pad->idle, 1);
    }

    if (request->bRequest == HID_REQ_CONTROL_GET_REPORT &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        if (tu_u16_high(request->wValue) != HID_REPORT_TYPE_INPUT) return false;
        memcpy(pad->ctrl_buf, pad->last_report, DONGLE_HID_REPORT_SIZE);
        uint16_t len = request->wLength;
        if (len > DONGLE_HID_REPORT_SIZE) len = DONGLE_HID_REPORT_SIZE;
        if (len == 0) return tud_control_status(rhport, request);
        return tud_control_xfer(rhport, request, pad->ctrl_buf, len);
    }

    /* SET_PROTOCOL and other class requests: ACK so enumeration is not stalled. */
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
        static uint8_t zero[8];
        uint16_t len = request->wLength < sizeof(zero) ? request->wLength : sizeof(zero);
        if (len == 0) return tud_control_status(rhport, request);
        return tud_control_xfer(rhport, request, zero, len);
    }
    return tud_control_status(rhport, request);
}

static bool hid_driver_xfer(uint8_t rhport, uint8_t ep_addr,
                            xfer_result_t result, uint32_t xferred_bytes)
{
    (void)rhport;
    if (result != XFER_RESULT_SUCCESS) {
        ESP_LOGW(TAG, "HID gamepad transfer failed ep=0x%02x result=%d bytes=%lu",
                 ep_addr, result, (unsigned long)xferred_bytes);
        return true;
    }
    /* Keep the host-idle timer honest. HID mode has no Xbox IN completions. */
    if (ep_addr & 0x80) xbox_note_in_complete();
    (void)xferred_bytes;
    return true;
}

static usbd_class_driver_t const s_driver = {
    .name = "HID_GAMEPAD",
    .init = hid_driver_init,
    .deinit = hid_driver_deinit,
    .reset = hid_driver_reset,
    .open = hid_driver_open,
    .control_xfer_cb = hid_driver_control,
    .xfer_cb = hid_driver_xfer,
    .xfer_isr = NULL,
    .sof = NULL,
};

usbd_class_driver_t const *hid_gamepad_class_driver(void)
{
    return &s_driver;
}

int hid_gamepad_send_report(uint8_t gamepad_index, const uint8_t *report, size_t len)
{
    if (gamepad_index >= DONGLE_MAX_CONTROLLERS || !report ||
        len != DONGLE_HID_REPORT_SIZE) {
        return -1;
    }
    hid_pad_t *pad = &s_pads[gamepad_index];
    memcpy(pad->last_report, report, DONGLE_HID_REPORT_SIZE);
    if (!tud_connected() || !tud_ready() || tud_suspended() || !pad->open || !pad->ep_in) {
        return -1;
    }
    if (usbd_edpt_busy(s_rhport, pad->ep_in)) return 0;

    memcpy(pad->in_buf, report, DONGLE_HID_REPORT_SIZE);
    if (!usbd_edpt_claim(s_rhport, pad->ep_in)) return 0;
    bool ok = usbd_edpt_xfer(s_rhport, pad->ep_in, pad->in_buf, DONGLE_HID_REPORT_SIZE);
    if (!ok) usbd_edpt_release(s_rhport, pad->ep_in);
    return ok ? 1 : 0;
}
