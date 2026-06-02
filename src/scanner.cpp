#include "scanner.h"
#include "config.h"
#include <Arduino.h>
#include <string.h>

Scanner scanner;

// ============================================================
// Initialisation
// ============================================================

void Scanner::begin() {
    Serial1.begin(SCANNER_BAUD, SERIAL_8N1, PIN_SCANNER_RX, PIN_SCANNER_TX);
    _len        = 0;
    _frameReady = false;
    _lastByteAt = 0;
    scanBtnBegin();
}

// ============================================================
// Scan button (active-low, internal pull-up)
// ============================================================

void scanBtnBegin() {
    pinMode(PIN_SCAN_BTN, INPUT_PULLUP);
}

bool scanBtnPressed() {
    if (digitalRead(PIN_SCAN_BTN) != LOW) return false;
    // Confirm the pin stays low for the debounce window
    delay(SCAN_BTN_DEBOUNCE_MS);
    return digitalRead(PIN_SCAN_BTN) == LOW;
}

// ============================================================
// Frame accumulation
// ============================================================

void Scanner::update() {
    if (_frameReady) return;  // caller must takeFrame() before we accumulate more

    while (Serial1.available()) {
        char c = (char)Serial1.read();
        _lastByteAt = millis();

        if (c == '\n') {
            // Strip trailing CR if present (GM60 sends CR+LF by default)
            if (_len > 0 && _buf[_len - 1] == '\r') _len--;
            _finalizeFrame();
            return;
        }

        // Silently drop bytes that would overflow — oversized scans are invalid
        if (_len < SCANNER_FRAME_MAXLEN - 1) {
            _buf[_len++] = c;
        }
    }

    // Timeout path: non-empty buffer with no new bytes for SCANNER_FRAME_TIMEOUT_MS
    if (_len > 0 && (millis() - _lastByteAt) >= SCANNER_FRAME_TIMEOUT_MS) {
        _finalizeFrame();
    }
}

// ============================================================
// Frame finalisation
// ============================================================

void Scanner::_finalizeFrame() {
    if (_len == 0) return;

    _buf[_len] = '\0';

    _frame.rawLen = _len;
    memcpy(_frame.raw, _buf, _len + 1);  // include null terminator

    // Classify by prefix
    if (strncmp(_buf, SCANNER_PPQ_PREFIX, SCANNER_PPQ_PREFIX_LEN) == 0) {
        _frame.type = ScanFrame::Type::QUERY;
    } else if (strncmp(_buf, SCANNER_PPSS_PREFIX, SCANNER_PPSS_PREFIX_LEN) == 0) {
        _frame.type = ScanFrame::Type::SYNC;
    } else {
        _frame.type = ScanFrame::Type::TOKEN;
    }

    _len        = 0;
    _lastByteAt = 0;
    _frameReady = true;
}

// ============================================================
// Public interface
// ============================================================

ScanFrame Scanner::takeFrame() {
    ScanFrame f = _frame;
    _frame      = ScanFrame{};
    _frameReady = false;
    return f;
}

bool Scanner::parseQuery(const ScanFrame& frame, QueryResult& out) const {
    if (frame.type != ScanFrame::Type::QUERY) return false;

    out.count = 0;
    const char* p = frame.raw;

    while (*p && out.count < SCANNER_QUERY_MAX) {
        // Each segment must start with the PPQ:: prefix
        if (strncmp(p, SCANNER_PPQ_PREFIX, SCANNER_PPQ_PREFIX_LEN) != 0) {
            // Skip to the next | or end — malformed segment
            const char* next = strchr(p, '|');
            if (!next) break;
            p = next + 1;
            continue;
        }

        p += SCANNER_PPQ_PREFIX_LEN;  // advance past "PPQ::"

        // Find end of this segment
        const char* sep = strchr(p, '|');
        size_t      segLen = sep ? (size_t)(sep - p) : strlen(p);

        if (segLen == 0) {
            // Empty value after prefix — skip
            p = sep ? sep + 1 : p + segLen;
            continue;
        }

        // Clamp and copy into output slot
        size_t copyLen = segLen < SCANNER_QUERY_VAL_LEN - 1
                         ? segLen
                         : SCANNER_QUERY_VAL_LEN - 1;

        memcpy(out.values[out.count], p, copyLen);
        out.values[out.count][copyLen] = '\0';
        out.count++;

        p = sep ? sep + 1 : p + segLen;
    }

    return out.count > 0;
}
