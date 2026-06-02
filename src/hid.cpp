#include "hid.h"
#include <Arduino.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <tusb.h>

// Owned here so all keystroke logic stays in this module.
// Declared extern in main.cpp solely for the USB.begin() ordering comment.
USBHIDKeyboard Keyboard;

static uint8_t _delayMs = HID_DEFAULT_DELAY_MS;

// ============================================================
// Initialisation
// ============================================================

void hidBegin() {
    Keyboard.begin();
}

void hidSetKeystrokeDelay(uint8_t ms) {
    _delayMs = ms;
}

// ============================================================
// Internal helpers
// ============================================================

// Poll until USB host has enumerated the device, up to HID_USB_TIMEOUT_MS.
// USB.ready() wraps tud_ready() from the ESP32 Arduino TinyUSB stack.
static bool waitUsbReady() {
    uint32_t start = millis();
    while (!tud_ready()) {
        if (millis() - start >= HID_USB_TIMEOUT_MS) return false;
        delay(10);
    }
    return true;
}

// ============================================================
// Public API
// ============================================================

HidResult typePassword(const char* pw, bool sendEnter) {
    if (!pw || pw[0] == '\0') return HidResult::EMPTY_PW;
    if (!waitUsbReady())      return HidResult::NOT_READY;

    for (size_t i = 0; pw[i] != '\0'; i++) {
        Keyboard.print(pw[i]);
        if (_delayMs > 0) delay(_delayMs);
    }

    if (sendEnter) Keyboard.write(KEY_RETURN);

    return HidResult::OK;
}
