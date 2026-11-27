#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Custom TinyUSB class driver for Xbox 360 controller.
// Replaces the generic vendor class to use direct interrupt endpoint transfers.
void xbox_dev_init(void);
int xbox_send_report(uint8_t gamepad_index, const uint8_t *report, size_t len);
void xbox_recover_after_resume(void);
/* Restart the "host is polling" timer. Call on attach/resume, before any IN
 * completion, so a quiet bus is not immediately treated as host-idle. */
void xbox_note_bus_activity(void);
/* A completed gamepad IN transfer: restarts the host-idle timer and bumps
 * the completion counter used by the USB log. */
void xbox_note_in_complete(void);
uint32_t xbox_us_since_in_complete(void);
uint32_t xbox_in_complete_count(void);
