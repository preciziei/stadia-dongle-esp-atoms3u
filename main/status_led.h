#pragma once

#include "usb_mode.h"

void status_led_init(void);
void status_led_flash_remote_wake(void);
/* Five green strobes toward XInput, five orange toward HID, then restart. */
void status_led_play_mode_change(usb_mode_t mode);

