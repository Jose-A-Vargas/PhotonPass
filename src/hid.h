#pragma once
#include <stdint.h>

// ============================================================
// USB HID keyboard output module
//
// Owns the USBHIDKeyboard instance. Call hidBegin() before USB.begin()
// so the HID device is registered with TinyUSB before the stack starts.
// ============================================================

#define HID_DEFAULT_DELAY_MS  8     // inter-keystroke pause (ms)
#define HID_USB_TIMEOUT_MS    3000  // max wait for USB host enumeration

enum class HidResult : uint8_t {
    OK        = 0,
    NOT_READY = 1,  // USB host not connected / device not enumerated within timeout
    EMPTY_PW  = 2,  // null or zero-length password
};

// Register HID device with TinyUSB. Call before USB.begin() in setup().
void hidBegin();

// Set per-keystroke delay in milliseconds (default HID_DEFAULT_DELAY_MS).
// Lower values risk dropped characters on slow host apps; 6–12 ms is safe.
void hidSetKeystrokeDelay(uint8_t ms);

// Type every character of pw via USB HID keyboard, one keystroke at a time.
// sendEnter: append KEY_RETURN after the final character (default true).
// Blocks until all keystrokes are sent or USB timeout is reached.
HidResult typePassword(const char* pw, bool sendEnter = true);
