#pragma once
#include <stdint.h>

#ifdef BOARD_EPD47_S3
// LilyGo T5 4.7" S3 v2.4
//   Panel  : 960 × 540, ED047TC1, parallel bus (epdiy)
//   Touch  : GT911, I2C 0x5D
//   Regions:
//     y=0..109   input area (prompt + text box)
//     y=110..116 separator
//     y=120..514 keyboard (4 rows × 95 px, 5 px gaps)
#  define DISPLAY_WIDTH   960
#  define DISPLAY_HEIGHT  540
#else
// Goodisplay GDEY042T81-T02
//   Panel  : 400 × 300, BW, SPI (GxEPD2)
//   Touch  : GT911, I2C 0x5D
//   Regions:
//     y=0..74   input area
//     y=75..79  separator
//     y=80..281 keyboard (4 rows × 46 px)
#  define DISPLAY_WIDTH   400
#  define DISPLAY_HEIGHT  300
#endif

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

    // Block until no finger is detected, then return.
    void waitRelease();

    // ---- Soft keyboard (legacy XIAO variant) ----

    bool softKeyboard(char* buf, uint8_t maxLen, const char* prompt);

    // ---- Keyboard widget (EPD47 variant) ----

    enum class KeyboardMode : uint8_t {
        QWERTY,           // number row + QWERTY + spacebar + OK
        QWERTY_NO_SPACE,  // number row + QWERTY + OK (no space)
        NUMPAD,           // 7-9/ 4-6* 1-3- .0DEL+ + OK
    };

    // Blocking keyboard entry. Rotation state persists between calls.
    // Returns true on OK/Enter, false on invalid args.
    bool keyboard(char* buf, uint8_t maxLen, const char* prompt,
                  KeyboardMode mode = KeyboardMode::QWERTY_NO_SPACE);

private:
    void _drawInputArea(const char* input, const char* prompt);
    void _drawKeys(uint8_t mode);
    void _labelKey(uint16_t kx, uint16_t ky, uint16_t kw, uint16_t kh,
                   const char* label, bool inverted = false);
    char _hitTestKey(uint16_t tx, uint16_t ty, uint8_t mode) const;

    void _kb2DrawAll(KeyboardMode mode, bool shifted, const char* buf);
    char _kb2HitTest(uint16_t tx, uint16_t ty, KeyboardMode mode, bool shifted) const;

#ifndef BOARD_EPD47_S3
    // Raw GT911 register access — XIAO variant only.
    // EPD47 variant uses file-scope static helpers instead.
    bool _gt911Read(uint16_t& x, uint16_t& y);
    bool _gt911Pressed();
    void _gt911WriteReg(uint16_t reg, uint8_t val);
    void _gt911ReadReg(uint16_t reg, uint8_t* buf, uint8_t len);
#endif
};

extern Display display;
