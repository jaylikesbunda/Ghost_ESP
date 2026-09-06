#ifndef BADUSB_MANAGER_H
#define BADUSB_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define BADUSB_CONSUMER_VOLUME_UP   0x01
#define BADUSB_CONSUMER_VOLUME_DOWN 0x02
#define BADUSB_CONSUMER_MUTE        0x04

esp_err_t badusb_manager_init(void);
esp_err_t badusb_manager_start(void);
esp_err_t badusb_manager_stop(void);
esp_err_t badusb_manager_execute_file(const char *path);
esp_err_t badusb_manager_execute_buffer(char *buf, size_t len);
bool badusb_manager_is_active(void);
// Returns true when the active TinyUSB device is enumerated by a USB host.
bool badusb_manager_is_usb_mounted(void);
int badusb_manager_list_scripts(char scripts[][64], int max_scripts);

// Apply current settings (VID/PID/strings/layout) to USB descriptors
void badusb_manager_apply_settings(void);

// VBUS sense support: returns true if VSENSE pin is configured (>= 0)
bool badusb_has_vsense(void);

// Returns true if VBUS is detected (pin HIGH), or always true if no VSENSE pin
bool badusb_vsense_connected(void);

// Stream receive for remote script execution (C5 -> S3)
esp_err_t badusb_manager_prepare_receive(size_t size);
void badusb_manager_register_stream_handler(void);

// Send a mouse HID report (dx, dy are relative movement, buttons is bitmask,
// wheel is the vertical wheel delta in 8-bit signed units; 0 leaves wheel alone)
bool badusb_hid_mouse_send(int8_t dx, int8_t dy, uint8_t buttons);
bool badusb_hid_mouse_wheel_send(int8_t wheel, uint8_t buttons);
bool badusb_manager_send_consumer(uint8_t control_mask);

// Mouse jiggler: moves mouse periodically to keep PC awake
esp_err_t badusb_manager_mouse_jiggle_start(void);
esp_err_t badusb_manager_mouse_jiggle_stop(void);
bool badusb_manager_is_jiggling(void);

// Trackpad mode: HID mouse driven by remote (or local) trackpad input.
// Keeps the existing TinyUSB keyboard+mouse+consumer descriptor active and
// exposes the mouse interface for dx/dy/button reports. trackpad_move saturates
// each axis to int8 per report (extra magnitude is dropped). buttons uses
// the standard boot-mouse bitmask (1=Left, 2=Right, 4=Middle).
esp_err_t badusb_manager_trackpad_start(void);
esp_err_t badusb_manager_trackpad_stop(void);
bool badusb_manager_is_trackpad(void);
void badusb_manager_trackpad_move(int dx, int dy);
void badusb_manager_trackpad_button(uint8_t buttons);
void badusb_manager_trackpad_wheel(int delta);

// Dedicated Consumer Control mode for wired volume/mute control.
esp_err_t badusb_manager_consumer_start(void);
esp_err_t badusb_manager_consumer_stop(void);
bool badusb_manager_is_consumer_mode(void);

// Keyboard mode: real-time key forwarding via COMM_STREAM_CHANNEL_KEYBOARD
esp_err_t badusb_manager_keyboard_mode_start(void);
esp_err_t badusb_manager_keyboard_mode_stop(void);
bool badusb_manager_is_keyboard_mode(void);

// Send a single keypress (requires active HID device - keyboard mode or script mode)
bool badusb_manager_send_keypress(uint8_t modifier, uint8_t keycode);

// Type text through the active HID keyboard device
bool badusb_manager_send_text(const char *text);

#endif
