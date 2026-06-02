#include "display.h"
#include "config.h"
#include "crypto.h"
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

// ============================================================
// GxEPD2 — Goodisplay GDEY042T81-T02 (400 x 300, BW)
// Requires GxEPD2 >= 1.5.0. If the class name fails to resolve,
// check your installed version; fallback: GxEPD2_420 (Waveshare)
// ============================================================
#include <GxEPD2_BW.h>
#include <epd/GxEPD2_420_GDEY042T81.h>
#include <Fonts/FreeSansBold9pt7b.h>   // key labels, prompt
#include <Fonts/FreeSans12pt7b.h>      // input text

static GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
    _epd(GxEPD2_420_GDEY042T81(PIN_EINK_CS, PIN_EINK_DC,
                                PIN_EINK_RST, PIN_EINK_BUSY));

// ============================================================
// QR code — ricmoo/QRCode library
// ============================================================
#include <qrcode.h>

// ============================================================
// GT911 capacitive touch controller
// Default I2C address when INT is high at power-on.
// ============================================================
#define GT911_ADDR      0x5D
#define GT911_REG_STATUS  0x814E
#define GT911_REG_POINT1  0x8150

// ============================================================
// Keyboard layout constants
//
// Physical grid (31 key slots):
//   Row 0 (y=80,  h=46): 10 keys — indices 0–9
//   Row 1 (y=132, h=46):  9 keys — indices 10–18
//   Row 2 (y=184, h=46):  9 keys — indices 19–27  (slots: SFT,7 letters,DEL)
//   Row 3 (y=236, h=46):  3 keys — indices 28–30  (TOG, SPACE, OK)
//
// Special return codes:
//   '\b'  = backspace (DEL key)
//   '\n'  = confirm   (OK  key)
//   '\x01'= shift
//   '\x02'= toggle alpha/numeric
// ============================================================
#define KB_ROWS     4
#define KB_KEYS     31

#define K_BKSP  '\b'
#define K_ENTER '\n'
#define K_SHIFT '\x01'
#define K_TOGL  '\x02'

#define KB_Y0   80
#define KB_Y1  132
#define KB_Y2  184
#define KB_Y3  236
#define KB_H    46

struct KeyRect { uint16_t x, y, w, h; };

static const KeyRect KEY_RECTS[KB_KEYS] = {
    // Row 0 — q…p   (x=11+i*38, w=36)
    {11,KB_Y0,36,KB_H},{49,KB_Y0,36,KB_H},{87,KB_Y0,36,KB_H},{125,KB_Y0,36,KB_H},
    {163,KB_Y0,36,KB_H},{201,KB_Y0,36,KB_H},{239,KB_Y0,36,KB_H},{277,KB_Y0,36,KB_H},
    {315,KB_Y0,36,KB_H},{353,KB_Y0,36,KB_H},
    // Row 1 — a…l   (x=21+i*40, w=38)
    {21,KB_Y1,38,KB_H},{61,KB_Y1,38,KB_H},{101,KB_Y1,38,KB_H},{141,KB_Y1,38,KB_H},
    {181,KB_Y1,38,KB_H},{221,KB_Y1,38,KB_H},{261,KB_Y1,38,KB_H},{301,KB_Y1,38,KB_H},
    {341,KB_Y1,38,KB_H},
    // Row 2 — SFT z…m DEL
    {33,KB_Y2,54,KB_H},                                          // SFT / numeric '-'
    {89,KB_Y2,30,KB_H},{121,KB_Y2,30,KB_H},{153,KB_Y2,30,KB_H}, // z x c  / _ . ,
    {185,KB_Y2,30,KB_H},{217,KB_Y2,30,KB_H},{249,KB_Y2,30,KB_H},{281,KB_Y2,30,KB_H}, // v b n m / ; : /
    {313,KB_Y2,54,KB_H},                                          // DEL
    // Row 3 — TOG SPACE OK
    {28,KB_Y3,80,KB_H},{112,KB_Y3,176,KB_H},{292,KB_Y3,80,KB_H},
};

// Characters emitted per key slot, indexed [slot]
static const char KEYS_LOWER[KB_KEYS] = {
    'q','w','e','r','t','y','u','i','o','p',
    'a','s','d','f','g','h','j','k','l',
    K_SHIFT,'z','x','c','v','b','n','m',K_BKSP,
    K_TOGL,' ',K_ENTER
};
static const char KEYS_UPPER[KB_KEYS] = {
    'Q','W','E','R','T','Y','U','I','O','P',
    'A','S','D','F','G','H','J','K','L',
    K_SHIFT,'Z','X','C','V','B','N','M',K_BKSP,
    K_TOGL,' ',K_ENTER
};
// Numeric: row 0 = 1-9+0, row 1 = symbols, row 2 repurposes SFT slot as '-'
static const char KEYS_NUM[KB_KEYS] = {
    '1','2','3','4','5','6','7','8','9','0',
    '!','@','#','$','%','^','&','*','(',
    '-','_','.',',',';',':','/','\\',K_BKSP,
    K_TOGL,' ',K_ENTER
};

// ============================================================
// Display global instance
// ============================================================
Display display;

// ============================================================
// GT911 I2C helpers (polling mode — no interrupt handler needed)
// ============================================================

void Display::_gt911WriteReg(uint16_t reg, uint8_t val) {
    Wire.beginTransmission(GT911_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    Wire.endTransmission();
}

void Display::_gt911ReadReg(uint16_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(GT911_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)GT911_ADDR, len);
    for (uint8_t i = 0; i < len && Wire.available(); i++) buf[i] = Wire.read();
}

bool Display::_gt911Read(uint16_t& x, uint16_t& y) {
    uint8_t status = 0;
    _gt911ReadReg(GT911_REG_STATUS, &status, 1);

    if (!(status & 0x80) || (status & 0x0F) == 0) {
        _gt911WriteReg(GT911_REG_STATUS, 0x00);  // clear ready flag
        return false;
    }

    uint8_t pt[8] = {};
    _gt911ReadReg(GT911_REG_POINT1, pt, 8);
    // pt[0]=track_id, pt[1]=x_lo, pt[2]=x_hi, pt[3]=y_lo, pt[4]=y_hi
    x = (uint16_t)pt[1] | ((uint16_t)pt[2] << 8);
    y = (uint16_t)pt[3] | ((uint16_t)pt[4] << 8);

    _gt911WriteReg(GT911_REG_STATUS, 0x00);
    return true;
}

bool Display::_gt911Pressed() {
    uint16_t x, y;
    return _gt911Read(x, y);
}

// ============================================================
// Initialisation
// ============================================================

bool Display::begin() {
    SPI.begin(PIN_EINK_SCK, /*MISO=*/-1, PIN_EINK_MOSI, /*SS=*/-1);
    _epd.init(0, true, 10, false);
    _epd.setRotation(0);

    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    delay(200);  // GT911 self-init time after power-on

    // Verify GT911 is alive: product ID registers 0x8140–0x8143 = "911\0"
    uint8_t pid[4] = {};
    _gt911ReadReg(0x8140, pid, 4);
    if (pid[0] != '9' || pid[1] != '1' || pid[2] != '1') {
        // Try alternate I2C address 0x14 (INT low at init)
        // If this also fails, hardware may need inspection
    }

    clear();
    return true;
}

// ============================================================
// Drawing primitives (write to GxEPD2 framebuffer, no screen push)
// ============================================================

void Display::fillScreen(bool black) {
    _epd.fillScreen(black ? GxEPD_BLACK : GxEPD_WHITE);
}

void Display::drawText(uint16_t x, uint16_t y, const char* text) {
    _epd.setFont(&FreeSansBold9pt7b);
    _epd.setTextColor(GxEPD_BLACK);
    _epd.setCursor(x, y);
    _epd.print(text);
}

void Display::drawTextLarge(uint16_t x, uint16_t y, const char* text) {
    _epd.setFont(&FreeSans12pt7b);
    _epd.setTextColor(GxEPD_BLACK);
    _epd.setCursor(x, y);
    _epd.print(text);
}

void Display::drawTextCentered(uint16_t y, const char* text) {
    _epd.setFont(&FreeSansBold9pt7b);
    _epd.setTextColor(GxEPD_BLACK);
    int16_t bx, by; uint16_t bw, bh;
    _epd.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    _epd.setCursor((DISPLAY_WIDTH - bw) / 2 - bx, y);
    _epd.print(text);
}

void Display::drawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    _epd.drawRect(x, y, w, h, GxEPD_BLACK);
}

void Display::fillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black) {
    _epd.fillRect(x, y, w, h, black ? GxEPD_BLACK : GxEPD_WHITE);
}

void Display::refresh() {
    _epd.display(false);  // full update using current framebuffer
}

// ============================================================
// Scene helpers
// ============================================================

void Display::clear() {
    _epd.setFullWindow();
    _epd.firstPage();
    do { _epd.fillScreen(GxEPD_WHITE); } while (_epd.nextPage());
}

void Display::showMessage(const char* title, const char* body) {
    _epd.setFullWindow();
    _epd.firstPage();
    do {
        _epd.fillScreen(GxEPD_WHITE);
        if (title) {
            _epd.setFont(&FreeSans12pt7b);
            _epd.setTextColor(GxEPD_BLACK);
            int16_t bx, by; uint16_t bw, bh;
            _epd.getTextBounds(title, 0, 0, &bx, &by, &bw, &bh);
            _epd.setCursor((DISPLAY_WIDTH - bw) / 2 - bx, 80);
            _epd.print(title);
        }
        if (body) {
            _epd.setFont(&FreeSansBold9pt7b);
            _epd.setTextColor(GxEPD_BLACK);
            int16_t bx, by; uint16_t bw, bh;
            _epd.getTextBounds(body, 0, 0, &bx, &by, &bw, &bh);
            _epd.setCursor((DISPLAY_WIDTH - bw) / 2 - bx, 130);
            _epd.print(body);
        }
    } while (_epd.nextPage());
}

bool Display::showQR(uint16_t cx, uint16_t cy, uint16_t maxSize,
                     const char* data, const char* label) {
    QRCode qrcode;
    uint8_t qrBuf[qrcode_getBufferSize(QR_MAX_VERSION)];
    int8_t err = -1;
    uint8_t version = 1;

    for (; version <= QR_MAX_VERSION && err != 0; version++)
        err = qrcode_initText(&qrcode, qrBuf, version, ECC_LOW, data);

    if (err != 0) {
        secureClear(qrBuf, sizeof(qrBuf));
        return false;
    }

    uint8_t modules  = qrcode.size + 2;  // +2 quiet-zone border
    uint8_t scale    = maxSize / modules;
    if (scale == 0) {
        secureClear(qrBuf, sizeof(qrBuf));
        return false;
    }

    uint16_t side = (uint16_t)scale * modules;
    uint16_t x0   = cx - side / 2;
    uint16_t y0   = cy - side / 2;

    _epd.setFullWindow();
    _epd.firstPage();
    do {
        _epd.fillScreen(GxEPD_WHITE);
        _epd.fillRect(x0, y0, side, side, GxEPD_WHITE);

        for (uint8_t row = 0; row < qrcode.size; row++) {
            for (uint8_t col = 0; col < qrcode.size; col++) {
                if (qrcode_getModule(&qrcode, col, row)) {
                    _epd.fillRect(x0 + (col + 1) * scale,
                                  y0 + (row + 1) * scale,
                                  scale, scale, GxEPD_BLACK);
                }
            }
        }

        if (label) {
            _epd.setFont(&FreeSansBold9pt7b);
            _epd.setTextColor(GxEPD_BLACK);
            int16_t bx, by; uint16_t bw, bh;
            _epd.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
            _epd.setCursor((DISPLAY_WIDTH - bw) / 2 - bx, y0 + side + 18);
            _epd.print(label);
        }
    } while (_epd.nextPage());

    // qrBuf contains QR codewords derived from the input data (may be a password).
    // Zero it before returning so it doesn't linger on the stack.
    secureClear(qrBuf, sizeof(qrBuf));
    return true;
}

// ============================================================
// Touch
// ============================================================

TouchPoint Display::readTouch() {
    TouchPoint tp = {0, 0, false};
    tp.pressed = _gt911Read(tp.x, tp.y);
    return tp;
}

// ============================================================
// Keyboard — internal helpers
// ============================================================

void Display::_drawInputArea(const char* input, const char* prompt) {
    _epd.fillRect(0, 0, DISPLAY_WIDTH, 75, GxEPD_WHITE);

    // Prompt label
    _epd.setFont(&FreeSansBold9pt7b);
    _epd.setTextColor(GxEPD_BLACK);
    _epd.setCursor(8, 18);
    _epd.print(prompt);

    // Input box
    _epd.drawRect(4, 28, DISPLAY_WIDTH - 8, 40, GxEPD_BLACK);

    // Typed text + cursor
    _epd.setFont(&FreeSans12pt7b);
    _epd.setCursor(10, 54);
    _epd.print(input);
    _epd.print('_');
}

void Display::_labelKey(uint16_t kx, uint16_t ky, uint16_t kw, uint16_t kh,
                        const char* label, bool inverted) {
    uint16_t fg = inverted ? GxEPD_WHITE : GxEPD_BLACK;
    uint16_t bg = inverted ? GxEPD_BLACK : GxEPD_WHITE;

    _epd.fillRect(kx, ky, kw, kh, bg);
    _epd.drawRect(kx, ky, kw, kh, GxEPD_BLACK);

    _epd.setFont(&FreeSansBold9pt7b);
    _epd.setTextColor(fg);
    int16_t bx, by; uint16_t bw, bh;
    _epd.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
    _epd.setCursor(kx + (kw - bw) / 2 - bx,
                   ky + (kh + bh) / 2 - by);
    _epd.print(label);
}

void Display::_drawKeys(uint8_t mode) {
    const char* keyMap = (mode == 2) ? KEYS_NUM
                       : (mode == 1) ? KEYS_UPPER
                                     : KEYS_LOWER;

    // Separator bar
    _epd.fillRect(0, 75, DISPLAY_WIDTH, 5, GxEPD_BLACK);

    for (uint8_t i = 0; i < KB_KEYS; i++) {
        const KeyRect& r = KEY_RECTS[i];
        char ch = keyMap[i];

        char label[5] = {};

        if (ch == K_BKSP)       { strncpy(label, "DEL",  4); }
        else if (ch == K_ENTER) { strncpy(label, "OK",   4); }
        else if (ch == K_SHIFT) { strncpy(label, "SFT",  4); }
        else if (ch == K_TOGL)  { strncpy(label, mode == 2 ? "ABC" : "123", 4); }
        else if (ch == ' ')     { strncpy(label, " ",    4); }
        else                    { label[0] = ch; }

        bool inverted = (ch == K_SHIFT && mode == 1);  // shift key lit when uppercase
        _labelKey(r.x, r.y, r.w, r.h, label, inverted);
    }
}

// Returns the key character for a touch point, or 0 if no key hit.
char Display::_hitTestKey(uint16_t tx, uint16_t ty, uint8_t mode) const {
    const char* keyMap = (mode == 2) ? KEYS_NUM
                       : (mode == 1) ? KEYS_UPPER
                                     : KEYS_LOWER;
    for (uint8_t i = 0; i < KB_KEYS; i++) {
        const KeyRect& r = KEY_RECTS[i];
        if (tx >= r.x && tx < (uint16_t)(r.x + r.w) &&
            ty >= r.y && ty < (uint16_t)(r.y + r.h)) {
            return keyMap[i];
        }
    }
    return 0;
}

// ============================================================
// Soft keyboard — blocking entry widget
// ============================================================

bool Display::softKeyboard(char* buf, uint8_t maxLen, const char* prompt) {
    if (!buf || maxLen < 2 || !prompt) return false;

    uint8_t len  = 0;
    uint8_t mode = 0;  // 0=lower, 1=upper, 2=numeric
    buf[0] = '\0';

    // Draws full keyboard + input area using GxEPD2 page loop.
    auto fullRedraw = [&]() {
        _epd.setFullWindow();
        _epd.firstPage();
        do {
            _epd.fillScreen(GxEPD_WHITE);
            _drawInputArea(buf, prompt);
            _drawKeys(mode);
        } while (_epd.nextPage());
    };

    // Redraws only the input area (top 80 px) via partial window.
    auto inputRedraw = [&]() {
        // x and w must be multiples of 8 for partial window alignment
        _epd.setPartialWindow(0, 0, DISPLAY_WIDTH, 80);
        _epd.firstPage();
        do {
            _epd.fillScreen(GxEPD_WHITE);
            _drawInputArea(buf, prompt);
            _drawKeys(mode);  // drawn to buffer; only 0..79 rows hit the panel
        } while (_epd.nextPage());
    };

    fullRedraw();

    while (true) {
        uint16_t tx, ty;
        if (!_gt911Read(tx, ty)) { delay(20); continue; }

        // Wait for finger lift before processing (avoids repeat triggers)
        while (_gt911Pressed()) delay(10);
        delay(40);

        char key = _hitTestKey(tx, ty, mode);
        if (key == 0) continue;

        bool modeChanged = false;

        switch (key) {
            case K_BKSP:
                if (len > 0) { buf[--len] = '\0'; inputRedraw(); }
                break;

            case K_ENTER:
                return len > 0;

            case K_SHIFT:
                mode = (mode == 0) ? 1 : 0;
                modeChanged = true;
                break;

            case K_TOGL:
                mode = (mode == 2) ? 0 : 2;
                modeChanged = true;
                break;

            default:
                if (len < maxLen - 1) {
                    buf[len++] = key;
                    buf[len]   = '\0';
                    // Single-shot shift: return to lowercase after one uppercase char
                    if (mode == 1) { mode = 0; modeChanged = true; }
                    else           { inputRedraw(); }
                }
                break;
        }

        if (modeChanged) fullRedraw();
    }
}
