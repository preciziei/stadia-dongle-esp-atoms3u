#pragma once

#include <stdint.h>

#define DONGLE_MAX_CONTROLLERS 3
#define DONGLE_ENABLE_UTILITY_HID 1

#define DONGLE_MAX_RAW_REPORT_SIZE 32
#define DONGLE_XBOX_REPORT_SIZE 20
#define DONGLE_RUMBLE_REPORT_SIZE 4

/*
 * ESP32-S3 full-speed USB exposes 4 IN endpoints besides EP0 (TinyUSB
 * dwc2_esp32.h reports ep_in_count = 5, including EP0). Each Xbox pad takes
 * one IN endpoint and the utility HID takes one more, so three pads is the
 * maximum that still keeps WebHID, keyboard actions, and remote wake.
 *
 *   pad 0: interface 0, IN 0x81, OUT 0x02
 *   pad 1: interface 1, IN 0x84, OUT 0x05
 *   pad 2: interface 2, IN 0x82, OUT 0x03
 *   HID:   interface 3, IN 0x83
 */
#define DONGLE_USB_IN_ENDPOINTS (DONGLE_MAX_CONTROLLERS + (DONGLE_ENABLE_UTILITY_HID ? 1 : 0))

_Static_assert(DONGLE_MAX_CONTROLLERS >= 1, "need at least one controller");
_Static_assert(DONGLE_MAX_CONTROLLERS == 3, "USB descriptors are written for 3 pads");
_Static_assert(DONGLE_USB_IN_ENDPOINTS <= 4, "ESP32-S3 IN endpoint budget exceeded");
