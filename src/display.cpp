#include "display.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>

//#ifdef BOARD_EPD47_S3
// ============================================================
// EPD47 S3 — LilyGo T5 4.7" S3 v2.4  (T5_47_PLUS board)
// Display : ED047TC1, 960×540, parallel bus (LilyGo epdiy fork)
// Touch   : TouchClass from <touch.h>
//           SDA=18, SCL=17, INT=47
//
// Font: font/firasans.h — located inside the LilyGo-EPD47 examples.
// The platformio.ini build_flags add the include path so that
//   #include "font/firasans.h"
// resolves to:
//   .pio/libdeps/epd47_s3/LilyGo-EPD47/examples/demo/font/firasans.h
// ============================================================

#define T5_47_PLUS  // selects S3 pins in any LilyGo board-helper headers

#include "epd_driver.h"
#include "ed047tc1.h"        // epd_start_frame / epd_output_row / epd_skip / epd_switch_buffer
#include "mono.h"            // extern const GFXfont MonoFont  (generated from consola.ttf)
#include <qrcode.h>

static inline void secureClear(void* p, size_t n) { memset(p, 0, n); }

static void dispLog(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print("[DISP] ");
    Serial.println(buf);
}

// 4bpp grayscale — 0x00 = black, 0xFF = white (two pixels per byte)
#define EPD47_BLACK 0x00
#define EPD47_WHITE 0xFF

// GT911 touch controller — direct I2C register access.
// Using TouchClass from the library caused corruption of its stored Wire pointer
// after EPD DMA operations; direct reads via Wire avoid that entirely.
#define GT911_ADDR        0x5D
#define GT911_REG_STATUS  0x814E
#define GT911_REG_POINT1  0x8150

static uint8_t* _fb        = nullptr;
static int      _charWidth = 0;   // advance width of one glyph (monospaced — all chars equal)

static char _prevTitle[128] = {};
static char _prevBody[128]  = {};

Display display;

// ============================================================
// GT911 direct I2C helpers — re-init Wire each time in case
// the EPD power cycle has disturbed the I2C peripheral state.
// ============================================================

static void _gt911Init() {
    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    delay(200);  // GT911 self-init time
    uint8_t pid[4] = {};
    Wire.beginTransmission(GT911_ADDR);
    Wire.write((uint8_t)(0x8140 >> 8));
    Wire.write((uint8_t)(0x8140 & 0xFF));
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)GT911_ADDR, (uint8_t)4);
    for (uint8_t i = 0; i < 4 && Wire.available(); i++) pid[i] = Wire.read();
    if (pid[0] == '9' && pid[1] == '1' && pid[2] == '1')
        Serial.println("[EPD47] GT911 found at 0x5D");
    else
        Serial.printf("[EPD47] GT911 id: %c%c%c (check SDA/SCL)\n", pid[0], pid[1], pid[2]);
}

static void _gt911WriteReg(uint16_t reg, uint8_t val) {
    Wire.beginTransmission(GT911_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    Wire.endTransmission();
}

static bool _gt911Read(uint16_t& x, uint16_t& y) {
    uint8_t status = 0;
    Wire.beginTransmission(GT911_ADDR);
    Wire.write((uint8_t)(GT911_REG_STATUS >> 8));
    Wire.write((uint8_t)(GT911_REG_STATUS & 0xFF));
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)GT911_ADDR, (uint8_t)1);
    if (Wire.available()) status = Wire.read();

    if (!(status & 0x80) || (status & 0x0F) == 0) {
        _gt911WriteReg(GT911_REG_STATUS, 0x00);
        return false;
    }

    uint8_t pt[8] = {};
    Wire.beginTransmission(GT911_ADDR);
    Wire.write((uint8_t)(GT911_REG_POINT1 >> 8));
    Wire.write((uint8_t)(GT911_REG_POINT1 & 0xFF));
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)GT911_ADDR, (uint8_t)8);
    for (uint8_t i = 0; i < 8 && Wire.available(); i++) pt[i] = Wire.read();

    // On this board the axes are swapped vs standard GT911:
    // bytes 0-1 = raw Y (inverted), bytes 2-3 = raw X.
    uint16_t raw_y = (uint16_t)pt[0] | ((uint16_t)pt[1] << 8);
    x = (uint16_t)pt[2] | ((uint16_t)pt[3] << 8);
    y = (raw_y <= EPD_HEIGHT) ? (uint16_t)(EPD_HEIGHT - raw_y) : 0;
    _gt911WriteReg(GT911_REG_STATUS, 0x00);
    return true;
}

// ============================================================
// Framebuffer & screen-push helpers
// ============================================================

static void _fbClear() {
    memset(_fb, EPD47_WHITE, EPD_WIDTH * EPD_HEIGHT / 2);
}

static void _afterEpd() {
    epd_poweroff_all();
    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    // Wire.begin() resets the I2C bus, which causes the GT911 to re-assert its
    // status register with the last touch point. Clear it so the next readTouch()
    // doesn't see a stale event.
    delay(20);
    Wire.beginTransmission(GT911_ADDR);
    Wire.write((uint8_t)(GT911_REG_STATUS >> 8));
    Wire.write((uint8_t)(GT911_REG_STATUS & 0xFF));
    Wire.write(0x00);
    Wire.endTransmission();
}

static void _pushScreen() {
    epd_poweron();
    // Two passes: first clears previous content, second renders new content cleanly.
    // Avoids the full-white flash of epd_clear_area while still removing ghosting.
    epd_draw_grayscale_image(epd_full_screen(), _fb);
    epd_draw_grayscale_image(epd_full_screen(), _fb);
    _afterEpd();
}

// Partial-area push — top `height` rows only (single pass, less ghosting risk).
static void _pushTopBand(int16_t height) {
    Rect_t area = {.x = 0, .y = 0, .width = EPD_WIDTH, .height = height};
    epd_poweron();
    epd_draw_grayscale_image(area, _fb);
    _afterEpd();
}

// White-fill a rectangle inside the framebuffer.
static void _fbFillWhite(int x, int y, int w, int h) {
    for (int row = y; row < y + h && row < EPD_HEIGHT; row++) {
        int off = row * (EPD_WIDTH / 2) + (x / 2);
        int len = w / 2;
        if (off + len <= EPD_WIDTH * EPD_HEIGHT / 2)
            memset(_fb + off, EPD47_WHITE, len);
    }
}

// ============================================================
// Direct Update (DU) — per-pixel selective waveform
// ============================================================

// Build one row of 2bpp drive data from the 4bpp framebuffer.
//   0b10 = drive white  (nibble >= 8 in fb)
//   0b01 = drive black  (nibble <  8 in fb)
//   0b00 = zero voltage (column outside `area` — panel pixel untouched)
static void _buildDuRow(uint8_t *out, const uint8_t *fb, int y, Rect_t area) {
    memset(out, 0, EPD_WIDTH / 4);
    const uint8_t *row = fb + y * (EPD_WIDTH / 2);
    for (int x = area.x; x < area.x + area.width; x++) {
        uint8_t nibble = (x & 1) ? (row[x >> 1] >> 4) : (row[x >> 1] & 0xF);
        uint8_t drive  = (nibble >= 8) ? 0b10 : 0b01;
        out[x >> 2]   |= drive << (2 * (x & 3));
    }
    // No reorder: natural byte order matches what the ESP32-S3 I2S expects.
    // reorder_line_buffer() in epd_push_pixels only works unnoticed because
    // epd_clear_area_cycles always passes uniform data (0xAA/0x55 in every byte),
    // for which swapping 16-bit halves is a no-op.
}

// Drive `area` pixels to match `fb` content in `passes` full frame scans.
// `pre_clears` full-white passes run first to dissolve ghosting before the DU.
// Pixels outside `area` receive zero voltage in all passes — never touched.
static void _duUpdate(Rect_t area, const uint8_t *fb, int time_dus, int pre_clears, int passes) {
    uint8_t row_buf[EPD_WIDTH / 4];

    // Pre-clear: drive every pixel in the rect white. Row data is uniform so
    // build it once and reuse for all rows (same as epd_push_pixels does).
    if (pre_clears > 0) {
        memset(row_buf, 0, EPD_WIDTH / 4);
        for (int x = area.x; x < area.x + area.width; x++)
            row_buf[x >> 2] |= 0b10 << (2 * (x & 3));

        for (int p = 0; p < pre_clears; p++) {
            epd_start_frame();
            bool started = false;
            int postSkip = 0;
            for (int y = 0; y < EPD_HEIGHT; y++) {
                if (y < area.y) { epd_skip(); continue; }
                if (y >= area.y + area.height) {
                    if (postSkip == 0) {
                        epd_switch_buffer();
                        memset(epd_get_current_buffer(), 0, EPD_WIDTH / 4);
                        epd_switch_buffer();
                        memset(epd_get_current_buffer(), 0, EPD_WIDTH / 4);
                    }
                    if (postSkip < 2) epd_output_row(10);
                    else              epd_skip();
                    postSkip++;
                    continue;
                }
                if (!started) {
                    epd_switch_buffer();
                    memcpy(epd_get_current_buffer(), row_buf, EPD_WIDTH / 4);
                    epd_switch_buffer();
                    memcpy(epd_get_current_buffer(), row_buf, EPD_WIDTH / 4);
                    started = true;
                }
                epd_output_row(time_dus);
            }
            memset(epd_get_current_buffer(), 0, EPD_WIDTH / 4);
            epd_output_row(time_dus);
            epd_end_frame();
        }
    }

    // DU passes: per-pixel voltage from framebuffer content.
    for (int p = 0; p < passes; p++) {
        epd_start_frame();
        bool started = false;
        int postSkip = 0;

        for (int y = 0; y < EPD_HEIGHT; y++) {
            if (y < area.y) {
                epd_skip();
                continue;
            }
            if (y >= area.y + area.height) {
                // Zero both buffers on the first post-area row so the source
                // outputs zero voltage after the inevitable 1-row pipeline bleed.
                if (postSkip == 0) {
                    epd_switch_buffer();
                    memset(epd_get_current_buffer(), 0, EPD_WIDTH / 4);
                    epd_switch_buffer();
                    memset(epd_get_current_buffer(), 0, EPD_WIDTH / 4);
                }
                // Two output_rows: first latches last-area-row data (unavoidable),
                // second latches zeros — all subsequent epd_skip()s see zero voltage.
                if (postSkip < 2) epd_output_row(10);
                else              epd_skip();
                postSkip++;
                continue;
            }
            _buildDuRow(row_buf, fb, y, area);
            if (!started) {
                epd_switch_buffer();
                memcpy(epd_get_current_buffer(), row_buf, EPD_WIDTH / 4);
                epd_switch_buffer();
                memcpy(epd_get_current_buffer(), row_buf, EPD_WIDTH / 4);
                started = true;
            } else {
                memcpy(epd_get_current_buffer(), row_buf, EPD_WIDTH / 4);
            }
            epd_output_row(time_dus);
        }
        memset(epd_get_current_buffer(), 0, EPD_WIDTH / 4);
        epd_output_row(time_dus);
        epd_end_frame();
    }
}

// ============================================================
// Text helpers — single font (MonoFont), cursor = baseline
// ============================================================

static int _textWidth(const char* text) {
    int32_t x1, y1, tw, th, cx = 0, cy = 0;
    get_text_bounds((GFXfont*)&MonoFont, text,
                    &cx, &cy, &x1, &y1, &tw, &th, NULL);
    return (int)tw;
}

static void _drawStr(int32_t cx, int32_t cy, const char* text) {
    FontProperties props = {};
    props.fg_color       = 0;    // black
    props.bg_color       = 15;   // white
    props.fallback_glyph = '?';  // substitute for chars not in font
    props.flags          = 0;
    write_mode((GFXfont*)&MonoFont, text, &cx, &cy, _fb, BLACK_ON_WHITE, &props);
}

// ============================================================
// Initialisation
// ============================================================

bool Display::begin() {
    epd_init();  // no arguments in LilyGo fork

    _fb = (uint8_t*)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (!_fb) {
        Serial.println("[EPD47] ps_calloc failed — PSRAM not available?");
        return false;
    }
    _fbClear();

    _charWidth = _textWidth("M");
    Serial.printf("[EPD47] MonoFont line_height=%d ascender=%d charWidth=%d\n",
                  MonoFont.advance_y, MonoFont.ascender, _charWidth);

    _gt911Init();

    epd_poweron();
    epd_clear();
    epd_poweroff_all();
    return true;
}

// ============================================================
// Drawing primitives — write to _fb only, no screen push
// ============================================================

void Display::fillScreen(bool black) {
    memset(_fb, black ? EPD47_BLACK : EPD47_WHITE, EPD_WIDTH * EPD_HEIGHT / 2);
}

void Display::drawText(uint16_t x, uint16_t y, const char* text) {
    _drawStr((int32_t)x, (int32_t)y, text);
}

void Display::drawTextLarge(uint16_t x, uint16_t y, const char* text) {
    _drawStr((int32_t)x, (int32_t)y, text);  // one font size in MonoFont
}

void Display::drawTextCentered(uint16_t y, const char* text) {
    int w  = _textWidth(text);
    int32_t cx = (DISPLAY_WIDTH - w) / 2;
    if (cx < 0) cx = 0;
    int32_t cy = (int32_t)y;
    writeln((GFXfont*)&MonoFont, text, &cx, &cy, _fb);
}

void Display::drawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    epd_draw_rect(x, y, w, h, EPD47_BLACK, _fb);
}

void Display::fillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black) {
    epd_fill_rect(x, y, w, h, black ? EPD47_BLACK : EPD47_WHITE, _fb);
}

// ============================================================
// Screen update
// ============================================================

void Display::refresh() {
    _prevTitle[0] = 0;
    _prevBody[0]  = 0;
    epd_poweron();
    epd_clear_area(epd_full_screen());
    epd_draw_grayscale_image(epd_full_screen(), _fb);
    _afterEpd();
}

void Display::clear() {
    _prevTitle[0] = 0;
    _prevBody[0]  = 0;
    _fbClear();
    epd_poweron();
    epd_clear();
    _afterEpd();
}

// ============================================================
// Scene helpers (draw to _fb + push)
// ============================================================

// Extract a compact (packed-row, no stride gaps) pixel buffer from _fb.
// Required because epd_draw_grayscale_image on a non-full-width Rect_t
// expects data_ptr to point to area.width/2 bytes per row, not EPD_WIDTH/2.
// x must be even. Caller must free().
static uint8_t* _compactFromFb(int x, int y, int w, int h) {
    int rowBytes = w / 2;
    if (rowBytes <= 0 || h <= 0) return nullptr;
    uint8_t* buf = (uint8_t*)malloc(rowBytes * h);
    if (!buf) return nullptr;
    for (int r = 0; r < h; r++)
        memcpy(buf + r * rowBytes,
               _fb + (y + r) * (EPD_WIDTH / 2) + x / 2,
               rowBytes);
    return buf;
}

static void _pushRect(int x, int y, int w, int h) {
    uint8_t* compact = _compactFromFb(x, y, w, h);
    if (!compact) return;
    Rect_t r = {x, y, w, h};
    epd_clear_area_cycles(r, 3, 50);
    epd_draw_grayscale_image(r, compact);
    free(compact);
}

static void _renderLine(const char* oldStr, const char* newStr,
                        int32_t baselineY, int above, int below) {
    bool oldEmpty = !oldStr || !*oldStr;
    bool newEmpty = !newStr || !*newStr;
    if (oldEmpty && newEmpty) return;
    if (!oldEmpty && !newEmpty && strcmp(oldStr, newStr) == 0) return;

    int oldLen = oldEmpty ? 0 : (int)strlen(oldStr);
    int newLen = newEmpty ? 0 : (int)strlen(newStr);
    int oldW   = oldLen * _charWidth;
    int newW   = newLen * _charWidth;
    int oldX   = oldEmpty ? 0 : (DISPLAY_WIDTH - oldW) / 2;
    int newX   = newEmpty ? 0 : (DISPLAY_WIDTH - newW) / 2;
    int uY     = (int)baselineY - above;
    int uH     = above + below;

    // Clear changed columns in framebuffer, then draw the full new string.
    int clearL, clearW;
    if (!oldEmpty && !newEmpty && oldLen == newLen) {
        int pre = 0;
        while (pre < oldLen && oldStr[pre] == newStr[pre]) pre++;
        int suf = 0;
        while (suf < oldLen - pre && oldStr[oldLen-1-suf] == newStr[newLen-1-suf]) suf++;
        clearL = newX + pre * _charWidth;
        clearW = (newLen - pre - suf) * _charWidth;
    } else {
        int uL = oldEmpty ? newX          : (newEmpty ? oldX          : min(oldX, newX));
        int uR = oldEmpty ? (newX + newW) : (newEmpty ? (oldX + oldW) : max(oldX + oldW, newX + newW));
        clearL = uL;
        clearW = uR - uL;
    }
    if (clearW > 0)
        epd_fill_rect(clearL, uY, clearW, uH, EPD47_WHITE, _fb);
    if (!newEmpty)
        _drawStr(newX, baselineY, newStr);

    // DU drive on the text union: each pixel gets voltage matching _fb.
    // Zero voltage outside the rect — no bleed, no adjacent pixels touched.
    int pushL = max(0,         oldEmpty ? newX          : (newEmpty ? oldX          : min(oldX, newX)));
    int pushR = min(EPD_WIDTH, oldEmpty ? (newX + newW) : (newEmpty ? (oldX + oldW) : max(oldX + oldW, newX + newW)));

    dispLog("renderLine old='%s' new='%s' du=[%d,%d]",
            oldEmpty ? "" : oldStr, newEmpty ? "" : newStr, pushL, pushR);

    if (pushR > pushL) {
        Rect_t r = {pushL, uY, pushR - pushL, uH};
        _duUpdate(r, _fb, 200, 2, 2);
    }
}

void Display::showMessage(const char* title, const char* body) {
    // MonoFont: ascender=39, descender=11.  Two lines centred, 70px apart.
    static const int32_t kTitleY = 235;
    static const int32_t kBodyY  = 305;
    static const int kAbove = 44, kBelow = 16;

    epd_poweron();
    _renderLine(_prevTitle, title, kTitleY, kAbove, kBelow);
    _renderLine(_prevBody,  body,  kBodyY,  kAbove, kBelow);
    _afterEpd();

    strncpy(_prevTitle, title ? title : "", sizeof(_prevTitle) - 1);
    strncpy(_prevBody,  body  ? body  : "", sizeof(_prevBody)  - 1);
}

bool Display::showQR(uint16_t cx, uint16_t cy, uint16_t maxSize,
                     const char* data, const char* label) {
    QRCode qrcode;
    uint8_t qrBuf[qrcode_getBufferSize(QR_MAX_VERSION)];
    int8_t err = -1;
    uint8_t version = 1;

    for (; version <= QR_MAX_VERSION && err != 0; version++)
        err = qrcode_initText(&qrcode, qrBuf, version, ECC_LOW, data);

    if (err != 0) { secureClear(qrBuf, sizeof(qrBuf)); return false; }

    uint8_t modules = qrcode.size + 2;
    uint8_t scale   = maxSize / modules;
    if (scale == 0) { secureClear(qrBuf, sizeof(qrBuf)); return false; }

    uint16_t side = (uint16_t)scale * modules;
    int x0 = (int)cx - side / 2;
    int y0 = (int)cy - side / 2;

    _fbClear();

    for (uint8_t row = 0; row < qrcode.size; row++) {
        for (uint8_t col = 0; col < qrcode.size; col++) {
            if (qrcode_getModule(&qrcode, col, row)) {
                epd_fill_rect(x0 + (col + 1) * scale,
                              y0 + (row + 1) * scale,
                              scale, scale, EPD47_BLACK, _fb);
            }
        }
    }

    if (label) {
        int lw = _textWidth(label);
        _drawStr((DISPLAY_WIDTH - lw) / 2, y0 + (int)side + 36, label);
    }

    _pushScreen();
    secureClear(qrBuf, sizeof(qrBuf));
    return true;
}

// ============================================================
// Touch — direct GT911 reads; no TouchClass involved.
// Y axis inverted per LilyGo touchtest example.
// ============================================================

TouchPoint Display::readTouch() {
    TouchPoint tp = {0, 0, false};
    uint16_t rx = 0, ry = 0;
    if (_gt911Read(rx, ry)) {
        tp.x       = rx;
        tp.y       = ry;
        tp.pressed = true;
    }
    return tp;
}

void Display::waitRelease() {
    // readTouch() just cleared the GT911 status register, so we must wait
    // for it to re-assert before polling — GT911 refreshes at ~100 Hz (10 ms).
    delay(30);
    uint16_t x, y;
    while (_gt911Read(x, y)) delay(10);
    delay(40);
}

// ============================================================
// Keyboard layout for 960 × 540
//
// Input area : y=0..109   (prompt label + text box)
// Separator  : y=110..116
// Row 0      : y=120, h=95  — q…p  (10 keys, w=90, step=95, start x=8)
// Row 1      : y=220, h=95  — a…l  ( 9 keys, w=90, step=95, start x=55)
// Row 2      : y=320, h=95  — SFT(130) + z…m(7×90) + DEL(130)
// Row 3      : y=420, h=95  — TOG(130) + SPACE(674) + OK(130)
// ============================================================

#define K_BKSP  '\b'
#define K_ENTER '\n'
#define K_SHIFT '\x01'
#define K_TOGL  '\x02'

#define KB_KEYS  31
#define KB_Y0    120
#define KB_Y1    220
#define KB_Y2    320
#define KB_Y3    420
#define KB_H      95

struct KeyRect { uint16_t x, y, w, h; };

static const KeyRect KEY_RECTS[KB_KEYS] = {
    // Row 0 — q…p  (x=8+i*95, w=90)
    {  8,KB_Y0,90,KB_H},{103,KB_Y0,90,KB_H},{198,KB_Y0,90,KB_H},{293,KB_Y0,90,KB_H},
    {388,KB_Y0,90,KB_H},{483,KB_Y0,90,KB_H},{578,KB_Y0,90,KB_H},{673,KB_Y0,90,KB_H},
    {768,KB_Y0,90,KB_H},{863,KB_Y0,90,KB_H},
    // Row 1 — a…l  (x=55+i*95, w=90)
    { 55,KB_Y1,90,KB_H},{150,KB_Y1,90,KB_H},{245,KB_Y1,90,KB_H},{340,KB_Y1,90,KB_H},
    {435,KB_Y1,90,KB_H},{530,KB_Y1,90,KB_H},{625,KB_Y1,90,KB_H},{720,KB_Y1,90,KB_H},
    {815,KB_Y1,90,KB_H},
    // Row 2 — SFT(130) z x c v b n m(7×90) DEL(130)
    { 15,KB_Y2,130,KB_H},
    {150,KB_Y2,90,KB_H},{245,KB_Y2,90,KB_H},{340,KB_Y2,90,KB_H},
    {435,KB_Y2,90,KB_H},{530,KB_Y2,90,KB_H},{625,KB_Y2,90,KB_H},{720,KB_Y2,90,KB_H},
    {815,KB_Y2,130,KB_H},
    // Row 3 — TOG(130) SPACE(674) OK(130)
    {  8,KB_Y3,130,KB_H},{143,KB_Y3,674,KB_H},{822,KB_Y3,130,KB_H},
};

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
static const char KEYS_NUM[KB_KEYS] = {
    '1','2','3','4','5','6','7','8','9','0',
    '!','@','#','$','%','^','&','*','(',
    '-','_','.',',',';',':','/','\\',K_BKSP,
    K_TOGL,' ',K_ENTER
};

// ============================================================
// Keyboard drawing helpers
// ============================================================

void Display::_drawInputArea(const char* input, const char* prompt) {
    _fbFillWhite(0, 0, EPD_WIDTH, 110);

    _drawStr(12, 36, prompt);

    epd_draw_rect(8, 44, EPD_WIDTH - 16, 60, EPD47_BLACK, _fb);

    char withCursor[130];
    snprintf(withCursor, sizeof(withCursor), "%s_", input);
    _drawStr(16, 88, withCursor);
}

void Display::_labelKey(uint16_t kx, uint16_t ky, uint16_t kw, uint16_t kh,
                        const char* label, bool inverted) {
    uint8_t bg = inverted ? EPD47_BLACK : EPD47_WHITE;

    epd_fill_rect(kx, ky, kw, kh, bg, _fb);
    epd_draw_rect(kx, ky, kw, kh, EPD47_BLACK, _fb);

    int lw = _textWidth(label);
    int32_t lx = (int32_t)kx + ((int32_t)kw - lw) / 2;
    if (lx < (int32_t)kx) lx = (int32_t)kx + 2;
    int32_t ly = (int32_t)ky + (int32_t)kh / 2 + 10;

    // Temporarily invert text colour for shift-active key
    if (inverted) {
        // Write inverted: clear key area already black; draw white-on-black
        // LilyGo's writeln uses BLACK_ON_WHITE by default; use write_mode for invert
        FontProperties props = {};
        props.fg_color     = 15;  // white (4-bit: 0=black, 15=white)
        props.bg_color     = 0;
        props.fallback_glyph = 0;
        props.flags        = DRAW_BACKGROUND;
        write_mode((GFXfont*)&MonoFont, label, &lx, &ly, _fb,
                   WHITE_ON_BLACK, &props);
    } else {
        writeln((GFXfont*)&MonoFont, label, &lx, &ly, _fb);
    }
}

void Display::_drawKeys(uint8_t mode) {
    const char* keyMap = (mode == 2) ? KEYS_NUM
                       : (mode == 1) ? KEYS_UPPER
                                     : KEYS_LOWER;

    epd_fill_rect(0, 110, EPD_WIDTH, 7, EPD47_BLACK, _fb);

    for (uint8_t i = 0; i < KB_KEYS; i++) {
        const KeyRect& r = KEY_RECTS[i];
        char ch = keyMap[i];
        char label[5] = {};

        if      (ch == K_BKSP)  strncpy(label, "DEL", 4);
        else if (ch == K_ENTER) strncpy(label, "OK",  4);
        else if (ch == K_SHIFT) strncpy(label, "SFT", 4);
        else if (ch == K_TOGL)  strncpy(label, mode == 2 ? "ABC" : "123", 4);
        else if (ch == ' ')     strncpy(label, " ",   4);
        else                    label[0] = ch;

        _labelKey(r.x, r.y, r.w, r.h, label, ch == K_SHIFT && mode == 1);
    }
}

char Display::_hitTestKey(uint16_t tx, uint16_t ty, uint8_t mode) const {
    const char* keyMap = (mode == 2) ? KEYS_NUM
                       : (mode == 1) ? KEYS_UPPER
                                     : KEYS_LOWER;
    for (uint8_t i = 0; i < KB_KEYS; i++) {
        const KeyRect& r = KEY_RECTS[i];
        if (tx >= r.x && tx < (uint16_t)(r.x + r.w) &&
            ty >= r.y && ty < (uint16_t)(r.y + r.h))
            return keyMap[i];
    }
    return 0;
}

// ============================================================
// keyboard() — multi-mode keyboard widget with rotation
// ============================================================

// Persists across keyboard() calls — toggled by the ROT button.
static bool _kb2Rotated = false;

// Layout (fits 960 × 540 exactly):
//   Input row  :  80 px   — ROT button + text box
//   OK zone    :  60 px   — small OK button floating above the 0 key column
//   4 key rows : 100 px each  →  400 px
//   Total      : 540 px  (no separator)
//
// Normal:  input(0..79) | ok(80..139) | keys(140..539)
// Rotated: keys(0..399) | ok(400..459) | input(460..539)
static constexpr int KB2_ROW_H  = 80;   // 5 QWERTY rows × 80 = 400 px
static constexpr int KB2_IN_H   = 80;
static constexpr int KB2_OK_H   = 60;

// Layout is always drawn in normal orientation. When _kb2Rotated, the
// framebuffer is flipped 180° in memory before being sent to the panel,
// and touch coordinates are inverted — no layout changes needed.
static int _kb2InY()       { return 0; }
static int _kb2OkY()       { return KB2_IN_H; }
static int _kb2RowY(int r) { return KB2_IN_H + KB2_OK_H + r * KB2_ROW_H; }

// Rotate the framebuffer 180°: reverse row order AND reverse+nibble-swap each row.
// Nibble swap is required because 4bpp stores two pixels per byte (even col = low
// nibble, odd col = high nibble), so reversing column order also swaps the nibbles.
static void _fb180Rotate() {
    uint8_t tmp[EPD_WIDTH / 2];
    for (int r = 0; r < EPD_HEIGHT / 2; r++) {
        uint8_t *rowA = _fb + r                    * (EPD_WIDTH / 2);
        uint8_t *rowB = _fb + (EPD_HEIGHT - 1 - r) * (EPD_WIDTH / 2);
        for (int i = 0; i < EPD_WIDTH / 2; i++) {
            uint8_t b = rowA[EPD_WIDTH / 2 - 1 - i];
            tmp[i] = (b >> 4) | (b << 4);
        }
        for (int i = 0; i < EPD_WIDTH / 2; i++) {
            uint8_t b = rowB[EPD_WIDTH / 2 - 1 - i];
            rowA[i] = (b >> 4) | (b << 4);
        }
        memcpy(rowB, tmp, EPD_WIDTH / 2);
    }
}

// QWERTY key X positions:
//   Row 0/1 (10 keys): x = 4 + i*96,  w = 92
//   Row 2   ( 9 keys): x = 48 + i*96, w = 92  (centred)
//   Row 3 SFT:  x=0   w=142
//   Row 3 z..m: x=146+i*96  w=92  (7 letters)
//   Row 3 DEL:  x=818  w=142
//   OK (QWERTY): floats in OK zone above 0 key (x=868, w=92) — text-width sized
//
// NUMPAD: 4 cols × w=236 step=240;  OK floats in OK zone above col 1 (x=240)

void Display::_kb2DrawAll(KeyboardMode mode, bool shifted, const char* buf) {
    _fbClear();

    // Input row: ROT button left, text box right
    int iy = _kb2InY();
    _labelKey(3, (uint16_t)(iy+3), 63, (uint16_t)(KB2_IN_H-6), "\xE2\x86\xB7");
    epd_draw_rect(70, iy+3, EPD_WIDTH-73, KB2_IN_H-6, EPD47_BLACK, _fb);
    char curs[130];
    snprintf(curs, sizeof(curs), "%s_", buf ? buf : "");
    { int32_t tx = 78, ty = iy + 52;
      writeln((GFXfont*)&MonoFont, curs, &tx, &ty, _fb); }

    // OK zone: small button floating above the 0-key column
    { int okW = _textWidth("OK") + 40;
      int oy  = _kb2OkY();
      int okX = (mode == KeyboardMode::NUMPAD)
                ? 240 + (236 - okW) / 2          // above numpad col 1 (0 key)
                : 868 + (92  - okW) / 2;          // above QWERTY 0 key
      int okY = oy + (KB2_OK_H - KB2_ROW_H/2) / 2;  // vertically centred in zone
      _labelKey((uint16_t)okX, (uint16_t)oy, (uint16_t)okW, (uint16_t)KB2_OK_H, "OK"); }

    // Keyboard rows
    char lbl[5];
    if (mode == KeyboardMode::NUMPAD) {
        static const char NP[4][4] = {
            {'7','8','9','/'}, {'4','5','6','*'}, {'1','2','3','-'}, {'.','0',0,'+'}
        };
        for (int r = 0; r < 4; r++) {
            int ry = _kb2RowY(r);
            for (int c = 0; c < 4; c++) {
                char ch = (r == 3 && c == 2) ? K_BKSP : NP[r][c];
                if (ch == K_BKSP) strncpy(lbl, "DEL", 4);
                else { lbl[0] = ch; lbl[1] = 0; }
                _labelKey((uint16_t)(c*240), (uint16_t)ry, 236, KB2_ROW_H, lbl);
            }
        }
    } else {
        // Row 0: symbols
        static const char SYM[12] = {'!','@','#','$','%','^','&','*','(',')','-','+'};
        for (int i = 0; i < 12; i++) {
            lbl[0] = SYM[i]; lbl[1] = 0;
            _labelKey((uint16_t)(i*80), (uint16_t)_kb2RowY(0), 78, KB2_ROW_H, lbl);
        }
        // Row 1: 1..0
        static const char NUMS[10] = {'1','2','3','4','5','6','7','8','9','0'};
        for (int i = 0; i < 10; i++) {
            lbl[0] = NUMS[i]; lbl[1] = 0;
            _labelKey((uint16_t)(4+i*96), (uint16_t)_kb2RowY(1), 92, KB2_ROW_H, lbl);
        }
        // Row 2: QWERTY
        const char* qw = shifted ? "QWERTYUIOP" : "qwertyuiop";
        for (int i = 0; i < 10; i++) {
            lbl[0] = qw[i]; lbl[1] = 0;
            _labelKey((uint16_t)(4+i*96), (uint16_t)_kb2RowY(2), 92, KB2_ROW_H, lbl);
        }
        // Row 3: ASDF
        const char* as = shifted ? "ASDFGHJKL" : "asdfghjkl";
        for (int i = 0; i < 9; i++) {
            lbl[0] = as[i]; lbl[1] = 0;
            _labelKey((uint16_t)(48+i*96), (uint16_t)_kb2RowY(3), 92, KB2_ROW_H, lbl);
        }
        // Row 4: SFT + ZXCVBNM + DEL
        _labelKey(0,   (uint16_t)_kb2RowY(4), 142, KB2_ROW_H, "SFT", shifted);
        const char* zx = shifted ? "ZXCVBNM" : "zxcvbnm";
        for (int i = 0; i < 7; i++) {
            lbl[0] = zx[i]; lbl[1] = 0;
            _labelKey((uint16_t)(146+i*96), (uint16_t)_kb2RowY(4), 92, KB2_ROW_H, lbl);
        }
        _labelKey(818, (uint16_t)_kb2RowY(4), 142, KB2_ROW_H, "DEL");
    }
}

char Display::_kb2HitTest(uint16_t tx, uint16_t ty, KeyboardMode mode, bool shifted) const {
    // OK zone (between input and keyboard rows)
    { int oy = _kb2OkY();
      if (ty >= oy && ty < oy + KB2_OK_H) {
          int okW = _textWidth("OK") + 40;
          int okX = (mode == KeyboardMode::NUMPAD)
                    ? 240 + (236 - okW) / 2
                    : 868 + (92  - okW) / 2;
          return (tx >= okX && (int)tx < okX + okW) ? K_ENTER : 0; } }

    if (mode == KeyboardMode::NUMPAD) {
        static const char NP[4][4] = {
            {'7','8','9','/'}, {'4','5','6','*'}, {'1','2','3','-'}, {'.','0',K_BKSP,'+'}
        };
        for (int r = 0; r < 4; r++) {
            int ry = _kb2RowY(r);
            if (ty < ry || ty >= ry + KB2_ROW_H) continue;
            int c = tx / 240;
            return (c >= 0 && c < 4) ? NP[r][c] : 0;
        }
        return 0;
    }

    // Row 0: symbols
    { int ry = _kb2RowY(0);
      if (ty >= ry && ty < ry + KB2_ROW_H) {
          static const char SYM[12] = {'!','@','#','$','%','^','&','*','(',')','-','+'};
          int i = (int)tx / 80;
          return (i >= 0 && i < 12) ? SYM[i] : 0; } }
    // Row 1: numbers
    { int ry = _kb2RowY(1);
      if (ty >= ry && ty < ry + KB2_ROW_H) {
          static const char N[10] = {'1','2','3','4','5','6','7','8','9','0'};
          int i = ((int)tx - 4) / 96;
          return (i >= 0 && i < 10) ? N[i] : 0; } }
    // Row 2: QWERTY
    { int ry = _kb2RowY(2);
      if (ty >= ry && ty < ry + KB2_ROW_H) {
          const char* r = shifted ? "QWERTYUIOP" : "qwertyuiop";
          int i = ((int)tx - 4) / 96;
          return (i >= 0 && i < 10) ? r[i] : 0; } }
    // Row 3: ASDF
    { int ry = _kb2RowY(3);
      if (ty >= ry && ty < ry + KB2_ROW_H) {
          const char* r = shifted ? "ASDFGHJKL" : "asdfghjkl";
          int i = ((int)tx - 48) / 96;
          return (i >= 0 && i < 9) ? r[i] : 0; } }
    // Row 4: SFT + ZXCVBNM + DEL
    { int ry = _kb2RowY(4);
      if (ty >= ry && ty < ry + KB2_ROW_H) {
          if (tx < 142)  return K_SHIFT;
          if (tx >= 818) return K_BKSP;
          const char* r = shifted ? "ZXCVBNM" : "zxcvbnm";
          int i = ((int)tx - 146) / 96;
          return (i >= 0 && i < 7) ? r[i] : 0; } }
    return 0;
}

bool Display::keyboard(char* buf, uint8_t maxLen, const char* prompt, KeyboardMode mode) {
    if (!buf || maxLen < 2) return false;
    buf[0] = '\0';
    uint8_t len = 0;
    bool shifted = false;

    auto fullRedraw = [&]() {
        _kb2DrawAll(mode, shifted, buf);
        if (_kb2Rotated) _fb180Rotate();
        epd_poweron();
        Rect_t full = {0, 0, EPD_WIDTH, EPD_HEIGHT};
        _duUpdate(full, _fb, 200, 0, 2);
        _afterEpd();
    };

    // Fast path: only refresh the input row (80 px).
    // For non-rotated: DU just rows 0..KB2_IN_H-1 — keyboard on panel is untouched.
    // For rotated: fall back to full redraw (input is at panel bottom after the flip).
    auto inputRedraw = [&]() {
        epd_fill_rect(70, 3, EPD_WIDTH-73, KB2_IN_H-6, EPD47_WHITE, _fb);
        char curs[130];
        snprintf(curs, sizeof(curs), "%s_", buf);
        int32_t itx = 78, ity = 52;
        writeln((GFXfont*)&MonoFont, curs, &itx, &ity, _fb);
        epd_poweron();
        if (!_kb2Rotated) {
            Rect_t r = {0, 0, EPD_WIDTH, KB2_IN_H};
            _duUpdate(r, _fb, 200, 1, 1);
        } else {
            _kb2DrawAll(mode, shifted, buf);
            _fb180Rotate();
            Rect_t full = {0, 0, EPD_WIDTH, EPD_HEIGHT};
            _duUpdate(full, _fb, 200, 0, 2);
        }
        _afterEpd();
    };

    fullRedraw();

    while (true) {
        uint16_t tx, ty;
        if (!_gt911Read(tx, ty)) { delay(20); continue; }
        waitRelease();
        delay(40);

        // Invert touch coordinates when rotated so hit tests always use normal space.
        if (_kb2Rotated) {
            tx = (uint16_t)(EPD_WIDTH  - 1 - tx);
            ty = (uint16_t)(EPD_HEIGHT - 1 - ty);
        }

        // ROT button — top-left of input area (always at 0,0 in normal space)
        if (tx >= 3 && tx < 66 && ty < (uint16_t)KB2_IN_H) {
            _kb2Rotated = !_kb2Rotated;
            epd_poweron();
            epd_clear();
            _afterEpd();
            fullRedraw();
            continue;
        }

        char ch = _kb2HitTest(tx, ty, mode, shifted);
        if (ch == 0) continue;

        switch (ch) {
            case K_BKSP:
                if (len > 0) { buf[--len] = '\0'; inputRedraw(); }
                break;
            case K_ENTER:
                return len > 0;
            case K_SHIFT:
                shifted = !shifted;
                fullRedraw();   // key labels change
                break;
            default:
                if (len < maxLen - 1) {
                    buf[len++] = ch;
                    buf[len]   = '\0';
                    bool wasShifted = shifted;
                    shifted = false;
                    if (wasShifted) fullRedraw();   // key labels revert to lower
                    else            inputRedraw();  // text only
                }
                break;
        }
    }
}

// ============================================================
// Soft keyboard
// ============================================================

bool Display::softKeyboard(char* buf, uint8_t maxLen, const char* prompt) {
    if (!buf || maxLen < 2 || !prompt) return false;

    uint8_t len  = 0;
    uint8_t mode = 0;
    buf[0] = '\0';

    auto fullRedraw = [&]() {
        _fbClear();
        _drawInputArea(buf, prompt);
        _drawKeys(mode);
        _pushScreen();
    };

    // Partial update — only refreshes the top 117 px (input area + separator).
    // epd_draw_grayscale_image reads EPD_WIDTH*117/2 bytes from _fb[0],
    // which is exactly rows 0..116.
    auto inputRedraw = [&]() {
        _fbFillWhite(0, 0, EPD_WIDTH, 110);
        _drawInputArea(buf, prompt);
        _pushTopBand(117);
    };

    fullRedraw();

    while (true) {
        uint16_t tx = 0, ty = 0;
        if (!_gt911Read(tx, ty)) { delay(20); continue; }

        waitRelease();
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
                    if (mode == 1) { mode = 0; modeChanged = true; }
                    else           { inputRedraw(); }
                }
                break;
        }

        if (modeChanged) fullRedraw();
    }
}

// #else
// // ============================================================
// // XIAO ESP32S3 + Goodisplay GDEY042T81-T02 (existing, unchanged)
// // ============================================================
// #include "crypto.h"
// #include <SPI.h>

// // ============================================================
// // GxEPD2 — Goodisplay GDEY042T81-T02 (400 x 300, BW)
// // Requires GxEPD2 >= 1.5.0. If the class name fails to resolve,
// // check your installed version; fallback: GxEPD2_420 (Waveshare)
// // ============================================================
// #include <GxEPD2_BW.h>
// #include <epd/GxEPD2_420_GDEY042T81.h>
// #include <Fonts/FreeSansBold9pt7b.h>   // key labels, prompt
// #include <Fonts/FreeSans12pt7b.h>      // input text

// static GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
//     _epd(GxEPD2_420_GDEY042T81(PIN_EINK_CS, PIN_EINK_DC,
//                                 PIN_EINK_RST, PIN_EINK_BUSY));

// // ============================================================
// // QR code — ricmoo/QRCode library
// // ============================================================
// #include <qrcode.h>

// // GT911 constants (defined at top of file, before #ifdef)

// // ============================================================
// // Keyboard layout constants
// //
// // Physical grid (31 key slots):
// //   Row 0 (y=80,  h=46): 10 keys — indices 0–9
// //   Row 1 (y=132, h=46):  9 keys — indices 10–18
// //   Row 2 (y=184, h=46):  9 keys — indices 19–27  (slots: SFT,7 letters,DEL)
// //   Row 3 (y=236, h=46):  3 keys — indices 28–30  (TOG, SPACE, OK)
// //
// // Special return codes:
// //   '\b'  = backspace (DEL key)
// //   '\n'  = confirm   (OK  key)
// //   '\x01'= shift
// //   '\x02'= toggle alpha/numeric
// // ============================================================
// #define KB_ROWS     4
// #define KB_KEYS     31

// #define K_BKSP  '\b'
// #define K_ENTER '\n'
// #define K_SHIFT '\x01'
// #define K_TOGL  '\x02'

// #define KB_Y0   80
// #define KB_Y1  132
// #define KB_Y2  184
// #define KB_Y3  236
// #define KB_H    46

// struct KeyRect { uint16_t x, y, w, h; };

// static const KeyRect KEY_RECTS[KB_KEYS] = {
//     // Row 0 — q…p   (x=11+i*38, w=36)
//     {11,KB_Y0,36,KB_H},{49,KB_Y0,36,KB_H},{87,KB_Y0,36,KB_H},{125,KB_Y0,36,KB_H},
//     {163,KB_Y0,36,KB_H},{201,KB_Y0,36,KB_H},{239,KB_Y0,36,KB_H},{277,KB_Y0,36,KB_H},
//     {315,KB_Y0,36,KB_H},{353,KB_Y0,36,KB_H},
//     // Row 1 — a…l   (x=21+i*40, w=38)
//     {21,KB_Y1,38,KB_H},{61,KB_Y1,38,KB_H},{101,KB_Y1,38,KB_H},{141,KB_Y1,38,KB_H},
//     {181,KB_Y1,38,KB_H},{221,KB_Y1,38,KB_H},{261,KB_Y1,38,KB_H},{301,KB_Y1,38,KB_H},
//     {341,KB_Y1,38,KB_H},
//     // Row 2 — SFT z…m DEL
//     {33,KB_Y2,54,KB_H},                                          // SFT / numeric '-'
//     {89,KB_Y2,30,KB_H},{121,KB_Y2,30,KB_H},{153,KB_Y2,30,KB_H}, // z x c  / _ . ,
//     {185,KB_Y2,30,KB_H},{217,KB_Y2,30,KB_H},{249,KB_Y2,30,KB_H},{281,KB_Y2,30,KB_H}, // v b n m / ; : /
//     {313,KB_Y2,54,KB_H},                                          // DEL
//     // Row 3 — TOG SPACE OK
//     {28,KB_Y3,80,KB_H},{112,KB_Y3,176,KB_H},{292,KB_Y3,80,KB_H},
// };

// // Characters emitted per key slot, indexed [slot]
// static const char KEYS_LOWER[KB_KEYS] = {
//     'q','w','e','r','t','y','u','i','o','p',
//     'a','s','d','f','g','h','j','k','l',
//     K_SHIFT,'z','x','c','v','b','n','m',K_BKSP,
//     K_TOGL,' ',K_ENTER
// };
// static const char KEYS_UPPER[KB_KEYS] = {
//     'Q','W','E','R','T','Y','U','I','O','P',
//     'A','S','D','F','G','H','J','K','L',
//     K_SHIFT,'Z','X','C','V','B','N','M',K_BKSP,
//     K_TOGL,' ',K_ENTER
// };
// // Numeric: row 0 = 1-9+0, row 1 = symbols, row 2 repurposes SFT slot as '-'
// static const char KEYS_NUM[KB_KEYS] = {
//     '1','2','3','4','5','6','7','8','9','0',
//     '!','@','#','$','%','^','&','*','(',
//     '-','_','.',',',';',':','/','\\',K_BKSP,
//     K_TOGL,' ',K_ENTER
// };

// // ============================================================
// // Display global instance
// // ============================================================
// Display display;

// // ============================================================
// // GT911 I2C helpers (polling mode — no interrupt handler needed)
// // ============================================================

// #define GT911_ADDR        0x5D
// #define GT911_REG_STATUS  0x814E
// #define GT911_REG_POINT1  0x8150

// void Display::_gt911WriteReg(uint16_t reg, uint8_t val) {
//     Wire.beginTransmission(GT911_ADDR);
//     Wire.write((uint8_t)(reg >> 8));
//     Wire.write((uint8_t)(reg & 0xFF));
//     Wire.write(val);
//     Wire.endTransmission();
// }

// void Display::_gt911ReadReg(uint16_t reg, uint8_t* buf, uint8_t len) {
//     Wire.beginTransmission(GT911_ADDR);
//     Wire.write((uint8_t)(reg >> 8));
//     Wire.write((uint8_t)(reg & 0xFF));
//     Wire.endTransmission(false);
//     Wire.requestFrom((uint8_t)GT911_ADDR, len);
//     for (uint8_t i = 0; i < len && Wire.available(); i++) buf[i] = Wire.read();
// }

// bool Display::_gt911Read(uint16_t& x, uint16_t& y) {
//     uint8_t status = 0;
//     _gt911ReadReg(GT911_REG_STATUS, &status, 1);

//     if (!(status & 0x80) || (status & 0x0F) == 0) {
//         _gt911WriteReg(GT911_REG_STATUS, 0x00);  // clear ready flag
//         return false;
//     }

//     uint8_t pt[8] = {};
//     _gt911ReadReg(GT911_REG_POINT1, pt, 8);
//     // pt[0]=track_id, pt[1]=x_lo, pt[2]=x_hi, pt[3]=y_lo, pt[4]=y_hi
//     x = (uint16_t)pt[1] | ((uint16_t)pt[2] << 8);
//     y = (uint16_t)pt[3] | ((uint16_t)pt[4] << 8);

//     _gt911WriteReg(GT911_REG_STATUS, 0x00);
//     return true;
// }

// bool Display::_gt911Pressed() {
//     uint16_t x, y;
//     return _gt911Read(x, y);
// }

// // ============================================================
// // Initialisation
// // ============================================================

// bool Display::begin() {
//     SPI.begin(PIN_EINK_SCK, /*MISO=*/-1, PIN_EINK_MOSI, /*SS=*/-1);
//     _epd.init(0, true, 10, false);
//     _epd.setRotation(0);

//     Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
//     delay(200);  // GT911 self-init time after power-on

//     // Verify GT911 is alive: product ID registers 0x8140–0x8143 = "911\0"
//     uint8_t pid[4] = {};
//     _gt911ReadReg(0x8140, pid, 4);
//     if (pid[0] != '9' || pid[1] != '1' || pid[2] != '1') {
//         // Try alternate I2C address 0x14 (INT low at init)
//         // If this also fails, hardware may need inspection
//     }

//     clear();
//     return true;
// }

// // ============================================================
// // Drawing primitives (write to GxEPD2 framebuffer, no screen push)
// // ============================================================

// void Display::fillScreen(bool black) {
//     _epd.fillScreen(black ? GxEPD_BLACK : GxEPD_WHITE);
// }

// void Display::drawText(uint16_t x, uint16_t y, const char* text) {
//     _epd.setFont(&FreeSansBold9pt7b);
//     _epd.setTextColor(GxEPD_BLACK);
//     _epd.setCursor(x, y);
//     _epd.print(text);
// }

// void Display::drawTextLarge(uint16_t x, uint16_t y, const char* text) {
//     _epd.setFont(&FreeSans12pt7b);
//     _epd.setTextColor(GxEPD_BLACK);
//     _epd.setCursor(x, y);
//     _epd.print(text);
// }

// void Display::drawTextCentered(uint16_t y, const char* text) {
//     _epd.setFont(&FreeSansBold9pt7b);
//     _epd.setTextColor(GxEPD_BLACK);
//     int16_t bx, by; uint16_t bw, bh;
//     _epd.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
//     _epd.setCursor((DISPLAY_WIDTH - bw) / 2 - bx, y);
//     _epd.print(text);
// }

// void Display::drawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
//     _epd.drawRect(x, y, w, h, GxEPD_BLACK);
// }

// void Display::fillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black) {
//     _epd.fillRect(x, y, w, h, black ? GxEPD_BLACK : GxEPD_WHITE);
// }

// void Display::refresh() {
//     _epd.display(false);  // full update using current framebuffer
// }

// // ============================================================
// // Scene helpers
// // ============================================================

// void Display::clear() {
//     _epd.setFullWindow();
//     _epd.firstPage();
//     do { _epd.fillScreen(GxEPD_WHITE); } while (_epd.nextPage());
// }

// void Display::showMessage(const char* title, const char* body) {
//     _epd.setFullWindow();
//     _epd.firstPage();
//     do {
//         _epd.fillScreen(GxEPD_WHITE);
//         if (title) {
//             _epd.setFont(&FreeSans12pt7b);
//             _epd.setTextColor(GxEPD_BLACK);
//             int16_t bx, by; uint16_t bw, bh;
//             _epd.getTextBounds(title, 0, 0, &bx, &by, &bw, &bh);
//             _epd.setCursor((DISPLAY_WIDTH - bw) / 2 - bx, 80);
//             _epd.print(title);
//         }
//         if (body) {
//             _epd.setFont(&FreeSansBold9pt7b);
//             _epd.setTextColor(GxEPD_BLACK);
//             int16_t bx, by; uint16_t bw, bh;
//             _epd.getTextBounds(body, 0, 0, &bx, &by, &bw, &bh);
//             _epd.setCursor((DISPLAY_WIDTH - bw) / 2 - bx, 130);
//             _epd.print(body);
//         }
//     } while (_epd.nextPage());
// }

// bool Display::showQR(uint16_t cx, uint16_t cy, uint16_t maxSize,
//                      const char* data, const char* label) {
//     QRCode qrcode;
//     uint8_t qrBuf[qrcode_getBufferSize(QR_MAX_VERSION)];
//     int8_t err = -1;
//     uint8_t version = 1;

//     for (; version <= QR_MAX_VERSION && err != 0; version++)
//         err = qrcode_initText(&qrcode, qrBuf, version, ECC_LOW, data);

//     if (err != 0) {
//         secureClear(qrBuf, sizeof(qrBuf));
//         return false;
//     }

//     uint8_t modules  = qrcode.size + 2;  // +2 quiet-zone border
//     uint8_t scale    = maxSize / modules;
//     if (scale == 0) {
//         secureClear(qrBuf, sizeof(qrBuf));
//         return false;
//     }

//     uint16_t side = (uint16_t)scale * modules;
//     uint16_t x0   = cx - side / 2;
//     uint16_t y0   = cy - side / 2;

//     _epd.setFullWindow();
//     _epd.firstPage();
//     do {
//         _epd.fillScreen(GxEPD_WHITE);
//         _epd.fillRect(x0, y0, side, side, GxEPD_WHITE);

//         for (uint8_t row = 0; row < qrcode.size; row++) {
//             for (uint8_t col = 0; col < qrcode.size; col++) {
//                 if (qrcode_getModule(&qrcode, col, row)) {
//                     _epd.fillRect(x0 + (col + 1) * scale,
//                                   y0 + (row + 1) * scale,
//                                   scale, scale, GxEPD_BLACK);
//                 }
//             }
//         }

//         if (label) {
//             _epd.setFont(&FreeSansBold9pt7b);
//             _epd.setTextColor(GxEPD_BLACK);
//             int16_t bx, by; uint16_t bw, bh;
//             _epd.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
//             _epd.setCursor((DISPLAY_WIDTH - bw) / 2 - bx, y0 + side + 18);
//             _epd.print(label);
//         }
//     } while (_epd.nextPage());

//     // qrBuf contains QR codewords derived from the input data (may be a password).
//     // Zero it before returning so it doesn't linger on the stack.
//     secureClear(qrBuf, sizeof(qrBuf));
//     return true;
// }

// // ============================================================
// // Touch
// // ============================================================

// TouchPoint Display::readTouch() {
//     TouchPoint tp = {0, 0, false};
//     tp.pressed = _gt911Read(tp.x, tp.y);
//     return tp;
// }

// void Display::waitRelease() {
//     while (_gt911Pressed()) delay(10);
//     delay(40);
// }

// // ============================================================
// // Keyboard — internal helpers
// // ============================================================

// void Display::_drawInputArea(const char* input, const char* prompt) {
//     _epd.fillRect(0, 0, DISPLAY_WIDTH, 75, GxEPD_WHITE);

//     // Prompt label
//     _epd.setFont(&FreeSansBold9pt7b);
//     _epd.setTextColor(GxEPD_BLACK);
//     _epd.setCursor(8, 18);
//     _epd.print(prompt);

//     // Input box
//     _epd.drawRect(4, 28, DISPLAY_WIDTH - 8, 40, GxEPD_BLACK);

//     // Typed text + cursor
//     _epd.setFont(&FreeSans12pt7b);
//     _epd.setCursor(10, 54);
//     _epd.print(input);
//     _epd.print('_');
// }

// void Display::_labelKey(uint16_t kx, uint16_t ky, uint16_t kw, uint16_t kh,
//                         const char* label, bool inverted) {
//     uint16_t fg = inverted ? GxEPD_WHITE : GxEPD_BLACK;
//     uint16_t bg = inverted ? GxEPD_BLACK : GxEPD_WHITE;

//     _epd.fillRect(kx, ky, kw, kh, bg);
//     _epd.drawRect(kx, ky, kw, kh, GxEPD_BLACK);

//     _epd.setFont(&FreeSansBold9pt7b);
//     _epd.setTextColor(fg);
//     int16_t bx, by; uint16_t bw, bh;
//     _epd.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
//     _epd.setCursor(kx + (kw - bw) / 2 - bx,
//                    ky + (kh + bh) / 2 - by);
//     _epd.print(label);
// }

// void Display::_drawKeys(uint8_t mode) {
//     const char* keyMap = (mode == 2) ? KEYS_NUM
//                        : (mode == 1) ? KEYS_UPPER
//                                      : KEYS_LOWER;

//     // Separator bar
//     _epd.fillRect(0, 75, DISPLAY_WIDTH, 5, GxEPD_BLACK);

//     for (uint8_t i = 0; i < KB_KEYS; i++) {
//         const KeyRect& r = KEY_RECTS[i];
//         char ch = keyMap[i];

//         char label[5] = {};

//         if (ch == K_BKSP)       { strncpy(label, "DEL",  4); }
//         else if (ch == K_ENTER) { strncpy(label, "OK",   4); }
//         else if (ch == K_SHIFT) { strncpy(label, "SFT",  4); }
//         else if (ch == K_TOGL)  { strncpy(label, mode == 2 ? "ABC" : "123", 4); }
//         else if (ch == ' ')     { strncpy(label, " ",    4); }
//         else                    { label[0] = ch; }

//         bool inverted = (ch == K_SHIFT && mode == 1);  // shift key lit when uppercase
//         _labelKey(r.x, r.y, r.w, r.h, label, inverted);
//     }
// }

// // Returns the key character for a touch point, or 0 if no key hit.
// char Display::_hitTestKey(uint16_t tx, uint16_t ty, uint8_t mode) const {
//     const char* keyMap = (mode == 2) ? KEYS_NUM
//                        : (mode == 1) ? KEYS_UPPER
//                                      : KEYS_LOWER;
//     for (uint8_t i = 0; i < KB_KEYS; i++) {
//         const KeyRect& r = KEY_RECTS[i];
//         if (tx >= r.x && tx < (uint16_t)(r.x + r.w) &&
//             ty >= r.y && ty < (uint16_t)(r.y + r.h)) {
//             return keyMap[i];
//         }
//     }
//     return 0;
// }

// // ============================================================
// // Soft keyboard — blocking entry widget
// // ============================================================

// bool Display::softKeyboard(char* buf, uint8_t maxLen, const char* prompt) {
//     if (!buf || maxLen < 2 || !prompt) return false;

//     uint8_t len  = 0;
//     uint8_t mode = 0;  // 0=lower, 1=upper, 2=numeric
//     buf[0] = '\0';

//     // Draws full keyboard + input area using GxEPD2 page loop.
//     auto fullRedraw = [&]() {
//         _epd.setFullWindow();
//         _epd.firstPage();
//         do {
//             _epd.fillScreen(GxEPD_WHITE);
//             _drawInputArea(buf, prompt);
//             _drawKeys(mode);
//         } while (_epd.nextPage());
//     };

//     // Redraws only the input area (top 80 px) via partial window.
//     auto inputRedraw = [&]() {
//         // x and w must be multiples of 8 for partial window alignment
//         _epd.setPartialWindow(0, 0, DISPLAY_WIDTH, 80);
//         _epd.firstPage();
//         do {
//             _epd.fillScreen(GxEPD_WHITE);
//             _drawInputArea(buf, prompt);
//             _drawKeys(mode);  // drawn to buffer; only 0..79 rows hit the panel
//         } while (_epd.nextPage());
//     };

//     fullRedraw();

//     while (true) {
//         uint16_t tx, ty;
//         if (!_gt911Read(tx, ty)) { delay(20); continue; }

//         // Wait for finger lift before processing (avoids repeat triggers)
//         while (_gt911Pressed()) delay(10);
//         delay(40);

//         char key = _hitTestKey(tx, ty, mode);
//         if (key == 0) continue;

//         bool modeChanged = false;

//         switch (key) {
//             case K_BKSP:
//                 if (len > 0) { buf[--len] = '\0'; inputRedraw(); }
//                 break;

//             case K_ENTER:
//                 return len > 0;

//             case K_SHIFT:
//                 mode = (mode == 0) ? 1 : 0;
//                 modeChanged = true;
//                 break;

//             case K_TOGL:
//                 mode = (mode == 2) ? 0 : 2;
//                 modeChanged = true;
//                 break;

//             default:
//                 if (len < maxLen - 1) {
//                     buf[len++] = key;
//                     buf[len]   = '\0';
//                     // Single-shot shift: return to lowercase after one uppercase char
//                     if (mode == 1) { mode = 0; modeChanged = true; }
//                     else           { inputRedraw(); }
//                 }
//                 break;
//         }

//         if (modeChanged) fullRedraw();
//     }
// }

// #endif  // BOARD_EPD47_S3 / else
