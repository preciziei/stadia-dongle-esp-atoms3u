#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device/usbd_pvt.h"

/*
 * Report descriptor length. hid_gamepad.c static-asserts this against the
 * byte array. usb_xbox.c copies it into the HID class descriptor.
 */
#define HID_GAMEPAD_REPORT_DESC_LEN 90

void hid_gamepad_init(void);
int hid_gamepad_send_report(uint8_t gamepad_index, const uint8_t *report, size_t len);
const uint8_t *hid_gamepad_report_descriptor(void);
usbd_class_driver_t const *hid_gamepad_class_driver(void);
