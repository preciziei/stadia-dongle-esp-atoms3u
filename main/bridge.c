/*
 * bridge.c — Translation between Stadia BLE input reports and Xbox 360 USB reports.
 *
 * Stadia BLE notification payload (10 bytes, no Report ID; byte 9 is padding,
 * observed constant 0x00 across a session with the Battery Service reporting
 * 51%, so it is not a battery level or a sequence counter — battery comes from
 * the Battery Service in ble_central.c):
 *   [0] D-pad hat (0=Up … 7=Up-L, >7=neutral)
 *   [1] bits 7=RS, 6=OPTIONS, 5=MENU, 4=STADIA_BTN
 *   [2] bits 6=A, 5=B, 4=X, 3=Y, 2=LB, 1=RB, 0=LS
 *   [3] Left stick X  (0–255)
 *   [4] Left stick Y  (0–255, 0=up)
 *   [5] Right stick X (0–255)
 *   [6] Right stick Y (0–255, 0=up)
 *   [7] Left trigger  (0–255)
 *   [8] Right trigger (0–255)
 *
 * Xbox 360 USB input report (20 bytes):
 *   [0]    0x00 (packet type)
 *   [1]    0x14 (packet length = 20)
 *   [2]    d-pad bits 0=Up,1=Down,2=Left,3=Right | btn bits 4=Start,5=Back,6=LS,7=RS
 *   [3]    btn bits 0=LB,1=RB,2=Guide,4=A,5=B,6=X,7=Y
 *   [4]    Left trigger  (0–255)
 *   [5]    Right trigger (0–255)
 *   [6–7]  Left stick X  (int16 LE, positive=right)
 *   [8–9]  Left stick Y  (int16 LE, positive=up)
 *   [10–11] Right stick X (int16 LE)
 *   [12–13] Right stick Y (int16 LE, positive=up)
 *   [14–19] 0x00 padding
 */

#include "bridge.h"
#include "usb_mode.h"
#include <stdio.h>
#include <string.h>

QueueHandle_t ble_to_usb_queue;
QueueHandle_t usb_to_ble_queue;

void bridge_init(void)
{
    ble_to_usb_queue = xQueueCreate(8 * DONGLE_MAX_CONTROLLERS, sizeof(ble_to_usb_msg_t));
    usb_to_ble_queue = xQueueCreate(4 * DONGLE_MAX_CONTROLLERS, sizeof(usb_to_ble_msg_t));
    configASSERT(ble_to_usb_queue != NULL);
    configASSERT(usb_to_ble_queue != NULL);
}

static void append_name(char *out, size_t out_len, const char *name)
{
    if (!out || out_len == 0 || !name) return;
    size_t used = strlen(out);
    if (used && used + 1 < out_len) {
        out[used++] = ',';
        out[used] = '\0';
    }
    snprintf(out + used, out_len - used, "%s", name);
}

void stadia_pressed_names(const stadia_controller_state_t *s, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!s) return;
    if (s->dpad_up) append_name(out, out_len, "dpad_up");
    if (s->dpad_down) append_name(out, out_len, "dpad_down");
    if (s->dpad_left) append_name(out, out_len, "dpad_left");
    if (s->dpad_right) append_name(out, out_len, "dpad_right");
    if (s->a) append_name(out, out_len, "a");
    if (s->b) append_name(out, out_len, "b");
    if (s->x) append_name(out, out_len, "x");
    if (s->y) append_name(out, out_len, "y");
    if (s->lb) append_name(out, out_len, "lb");
    if (s->rb) append_name(out, out_len, "rb");
    if (s->ls) append_name(out, out_len, "ls");
    if (s->rs) append_name(out, out_len, "rs");
    if (s->menu) append_name(out, out_len, "menu");
    if (s->options) append_name(out, out_len, "options");
    if (s->stadia) append_name(out, out_len, "stadia");
    if (s->assistant) append_name(out, out_len, "assistant");
    if (s->capture) append_name(out, out_len, "capture");
}

void bridge_send_report(uint8_t gamepad_index, const uint8_t *data, size_t len)
{
    ble_to_usb_msg_t msg = {
        .gamepad_index = gamepad_index,
        .len = len,
    };
    if (!data || len > sizeof(msg.data)) return;
    if (len) memcpy(msg.data, data, len);
    if (xQueueSendToBack(ble_to_usb_queue, &msg, 0) != pdTRUE) {
        ble_to_usb_msg_t dummy;
        xQueueReceive(ble_to_usb_queue, &dummy, 0);
        xQueueSendToBack(ble_to_usb_queue, &msg, 0);
    }
}

void bridge_send_neutral(uint8_t gamepad_index)
{
    uint8_t report[DONGLE_XBOX_REPORT_SIZE] = {0};
    size_t len;
    if (usb_mode_get() == USB_MODE_HID) {
        stadia_hid_neutral(report);
        len = DONGLE_HID_REPORT_SIZE;
    } else {
        report[0] = 0x00;
        report[1] = 0x14;
        len = DONGLE_XBOX_REPORT_SIZE;
    }
    bridge_send_report(gamepad_index, report, len);
}

// Stadia hat-switch (0–7) → Xbox 360 d-pad bitmask (bits 0=Up,1=Down,2=Left,3=Right)
static const uint8_t dpad_map[8] = {
    0x01, // Up
    0x09, // Up+Right
    0x08, // Right
    0x0A, // Down+Right
    0x02, // Down
    0x06, // Down+Left
    0x04, // Left
    0x05, // Up+Left
};

// Map 0–255 unsigned stick value to -32767..+32767 signed, optionally inverting axis.
// Clamps center dead-zone: raw 128 → 0, raw 0/255 → ±32767.
static int16_t map_stick(uint8_t v, int invert)
{
    int c = (int)v - 128;
    if (c < -127) c = -127;
    if (invert) c = -c;
    return (int16_t)(32767 * c / 127);
}

void stadia_parse_state(const uint8_t *s, size_t len, stadia_controller_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s || len == 0) return;
    size_t n = len > sizeof(out->raw) ? sizeof(out->raw) : len;
    memcpy(out->raw, s, n);
    out->raw_len = n;
    if (len < 9) return;

    switch (s[0]) {
    case 0: out->dpad_up = true; break;
    case 1: out->dpad_up = true; out->dpad_right = true; break;
    case 2: out->dpad_right = true; break;
    case 3: out->dpad_down = true; out->dpad_right = true; break;
    case 4: out->dpad_down = true; break;
    case 5: out->dpad_down = true; out->dpad_left = true; break;
    case 6: out->dpad_left = true; break;
    case 7: out->dpad_up = true; out->dpad_left = true; break;
    default: break;
    }

    out->assistant = (s[1] & (1 << 1)) != 0;
    out->capture = (s[1] & (1 << 0)) != 0;
    out->rs = (s[1] & (1 << 7)) != 0;
    out->options = (s[1] & (1 << 6)) != 0;
    out->menu = (s[1] & (1 << 5)) != 0;
    out->stadia = (s[1] & (1 << 4)) != 0;
    out->a = (s[2] & (1 << 6)) != 0;
    out->b = (s[2] & (1 << 5)) != 0;
    out->x = (s[2] & (1 << 4)) != 0;
    out->y = (s[2] & (1 << 3)) != 0;
    out->lb = (s[2] & (1 << 2)) != 0;
    out->rb = (s[2] & (1 << 1)) != 0;
    out->ls = (s[2] & (1 << 0)) != 0;
    out->lx = map_stick(s[3], 0);
    out->ly = map_stick(s[4], 1);
    out->rx = map_stick(s[5], 0);
    out->ry = map_stick(s[6], 1);
    out->lt = s[7];
    out->rt = s[8];
}

void stadia_to_xbox360(const uint8_t *s, uint8_t *x)
{
    if (!s || !x) return;
    memset(x, 0, 20);
    x[0] = 0x00; // packet type
    x[1] = 0x14; // packet length = 20

    // Byte 2: d-pad + Start / Back / LS / RS
    uint8_t b2 = (s[0] < 8) ? dpad_map[s[0]] : 0;
    if (s[1] & (1 << 5)) b2 |= (1 << 4); // MENU    → Start
    if (s[1] & (1 << 6)) b2 |= (1 << 5); // OPTIONS → Back
    if (s[2] & (1 << 0)) b2 |= (1 << 6); // LS
    if (s[1] & (1 << 7)) b2 |= (1 << 7); // RS
    x[2] = b2;

    // Byte 3: LB / RB / Guide / A / B / X / Y
    uint8_t b3 = 0;
    if (s[2] & (1 << 2)) b3 |= (1 << 0); // LB
    if (s[2] & (1 << 1)) b3 |= (1 << 1); // RB
    if (s[1] & (1 << 4)) b3 |= (1 << 2); // STADIA_BTN → Guide
    if (s[2] & (1 << 6)) b3 |= (1 << 4); // A
    if (s[2] & (1 << 5)) b3 |= (1 << 5); // B
    if (s[2] & (1 << 4)) b3 |= (1 << 6); // X
    if (s[2] & (1 << 3)) b3 |= (1 << 7); // Y
    x[3] = b3;

    x[4] = s[7]; // left trigger
    x[5] = s[8]; // right trigger

    int16_t lx = map_stick(s[3], 0);
    int16_t ly = map_stick(s[4], 1); // Stadia Y=0 is up → invert for Xbox (positive=up)
    int16_t rx = map_stick(s[5], 0);
    int16_t ry = map_stick(s[6], 1);

    x[6]  = (uint8_t)(lx & 0xFF); x[7]  = (uint8_t)(lx >> 8);
    x[8]  = (uint8_t)(ly & 0xFF); x[9]  = (uint8_t)(ly >> 8);
    x[10] = (uint8_t)(rx & 0xFF); x[11] = (uint8_t)(rx >> 8);
    x[12] = (uint8_t)(ry & 0xFF); x[13] = (uint8_t)(ry >> 8);
    // bytes 14–19 already zero
}

/*
 * Standard HID gamepad, Xbox One BT / Android Generic.kl button order.
 * 16 one-bit buttons, usages are 1-based; unused usages stay 0:
 *   1 A, 2 B, 4 X, 5 Y, 7 LB, 8 RB,
 *   11 Select/View (Options), 12 Start/Menu, 13 Guide (Stadia),
 *   14 L3, 15 R3
 * Then a hat nibble (0..7, null = 8) plus 4 bits of padding,
 * X/Y/Z/Rz as the raw 0–255 stick bytes (center 0x80; HID Y is not inverted),
 * and Brake/Accelerator as LT/RT.
 * Neutral is not all zeros: sticks sit at 0x80 and the hat is 8.
 */
void stadia_hid_neutral(uint8_t *out)
{
    if (!out) return;
    memset(out, 0, DONGLE_HID_REPORT_SIZE);
    out[2] = 0x08;
    out[3] = 0x80;
    out[4] = 0x80;
    out[5] = 0x80;
    out[6] = 0x80;
}

void stadia_to_hid_gamepad(const uint8_t *s, uint8_t *out)
{
    if (!out) return;
    if (!s) {
        stadia_hid_neutral(out);
        return;
    }

    uint16_t buttons = 0;
    if (s[2] & (1 << 6)) buttons |= 1u << 0;  /* A  = usage 1 */
    if (s[2] & (1 << 5)) buttons |= 1u << 1;  /* B  = usage 2 */
    if (s[2] & (1 << 4)) buttons |= 1u << 3;  /* X  = usage 4 */
    if (s[2] & (1 << 3)) buttons |= 1u << 4;  /* Y  = usage 5 */
    if (s[2] & (1 << 2)) buttons |= 1u << 6;  /* LB = usage 7 */
    if (s[2] & (1 << 1)) buttons |= 1u << 7;  /* RB = usage 8 */
    if (s[1] & (1 << 6)) buttons |= 1u << 10; /* Select/View = usage 11 */
    if (s[1] & (1 << 5)) buttons |= 1u << 11; /* Start/Menu  = usage 12 */
    if (s[1] & (1 << 4)) buttons |= 1u << 12; /* Guide       = usage 13 */
    if (s[2] & (1 << 0)) buttons |= 1u << 13; /* L3          = usage 14 */
    if (s[1] & (1 << 7)) buttons |= 1u << 14; /* R3          = usage 15 */

    memset(out, 0, DONGLE_HID_REPORT_SIZE);
    out[0] = (uint8_t)(buttons & 0xff);
    out[1] = (uint8_t)(buttons >> 8);
    out[2] = (s[0] < 8) ? (uint8_t)(s[0] & 0x0f) : 0x08;
    out[3] = s[3];
    out[4] = s[4];
    out[5] = s[5];
    out[6] = s[6];
    out[7] = s[7];
    out[8] = s[8];
}
