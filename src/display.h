#pragma once
#include <stdint.h>

// ============================================================
// Hardware: Goodisplay GDEY042T81-T02
//   Panel  : 400 x 300 px, BW, SPI
//   Touch  : GT911 capacitive controller, I2C (addr 0x5D)
//
// Display regions used by this driver:
//   y=0..74   — input area  (prompt + typed text)
//   y=75..79  — separator bar
//   y=80..281 — keyboard    (4 rows × 46 px + 3 × 6 px gaps)
// ============================================================

#define DISPLAY_WIDTH    400
#define DISPLAY_HEIGHT   300
#define QR_MAX_VERSION   10   // ECC_LOW v10 → max ~134 chars

struct TouchPoint {
    uint16_t x;
    uint16_t y;
    bool     pressed;
};

class Display {
public:
    // Initialise SPI panel and I2C touch. Returns false on fatal error.
    bool begin();

    // ---- Drawing primitives (write to framebuffer, no screen push) ----

    void fillScreen(bool black = false);
    void drawText(uint16_t x, uint16_t y, const char* text);
    void drawTextLarge(uint16_t x, uint16_t y, const char* text);
    void drawTextCentered(uint16_t y, const char* text);
    void drawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void fillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black = true);

    // Push framebuffer to panel — full refresh (~2 s).
    void refresh();

    // ---- Scene-level helpers (draw + refresh internally) ----

    // White screen.
    void clear();

    // Two-line text screen (title bold, body normal).
    void showMessage(const char* title, const char* body = nullptr);

    // QR code centred at (cx, cy) in a maxSize×maxSize area, optional label below.
    // Returns false if data exceeds QR capacity.
    bool showQR(uint16_t cx, uint16_t cy, uint16_t maxSize,
                const char* data, const char* label = nullptr);

    // ---- Touch ----

    // Non-blocking single-point poll.
    TouchPoint readTouch();

    // ---- Soft keyboard ----

    // Blocking: draws QWERTY/numeric keyboard, accumulates keypresses into buf.
    // Confirm with Enter key. buf is always null-terminated on return.
    // Returns true on Enter, false if called with invalid args.
    bool softKeyboard(char* buf, uint8_t maxLen, const char* prompt);

private:
    void _drawInputArea(const char* input, const char* prompt);
    void _drawKeys(uint8_t mode);
    void _labelKey(uint16_t kx, uint16_t ky, uint16_t kw, uint16_t kh,
                   const char* label, bool inverted = false);
    char _hitTestKey(uint16_t tx, uint16_t ty, uint8_t mode) const;

    bool _gt911Read(uint16_t& x, uint16_t& y);
    bool _gt911Pressed();
    void _gt911WriteReg(uint16_t reg, uint8_t val);
    void _gt911ReadReg(uint16_t reg, uint8_t* buf, uint8_t len);
};

extern Display display;
