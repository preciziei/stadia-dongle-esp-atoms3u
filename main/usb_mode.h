#pragma once

#include <stdint.h>

typedef enum {
    USB_MODE_XINPUT = 0,
    USB_MODE_HID = 1,
} usb_mode_t;

/* Load the cached mode from NVS namespace "usbmode" (default XInput). */
void usb_mode_init(void);
usb_mode_t usb_mode_get(void);

/*
 * Persist mode, zero the recovery boot counter, and esp_restart().
 * Clearing "recovery"/"boots" is required: main.c recovery_check() forgets
 * every BLE bond after 5 boots inside 30 s, and a mode toggle is a reboot.
 */
void usb_mode_set_and_restart(usb_mode_t mode);
