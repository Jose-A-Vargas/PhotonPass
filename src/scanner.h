#pragma once
#include <stdint.h>
#include <stddef.h>

// ============================================================
// GM60 1D/2D barcode/QR reader — TTL UART driver
//
// The GM60 terminates every scan with CR+LF (\r\n).
// A frame is considered complete when \n is received, or when
// SCANNER_FRAME_TIMEOUT_MS elapses after the last received byte
// (guards against modules that omit the terminator).
//
// Three frame types:
//   QUERY  — companion app query QR, prefix "PPQ::"
//            e.g.  PPQ::github.com|PPQ::google.com
//   TOKEN  — raw physical MFA token; passed as-is to deriveKey()
//   SYNC   — device-to-device sync frame, prefix "PPSS::"
// ============================================================

#define SCANNER_FRAME_MAXLEN     512   // max raw bytes per scan
#define SCANNER_QUERY_MAX        8     // max account queries per frame
#define SCANNER_QUERY_VAL_LEN    96    // max chars per query value
#define SCANNER_FRAME_TIMEOUT_MS 200   // ms of silence → frame done
#define SCANNER_PPQ_PREFIX       "PPQ::"
#define SCANNER_PPQ_PREFIX_LEN   5
#define SCANNER_PPSS_PREFIX      "PPSS:"
#define SCANNER_PPSS_PREFIX_LEN  5

struct ScanFrame {
    enum class Type : uint8_t {
        NONE  = 0,
        QUERY = 1,  // PPQ:: prefixed companion-app query
        TOKEN = 2,  // raw MFA unlock token
        SYNC  = 3,  // PPSS: prefixed device-to-device sync frame
    };

    Type     type   = Type::NONE;
    char     raw[SCANNER_FRAME_MAXLEN];
    uint16_t rawLen = 0;
};

// Holds the parsed query values extracted from a QUERY frame.
// Each value is the bare identifier the vault uses for record lookup.
struct QueryResult {
    char    values[SCANNER_QUERY_MAX][SCANNER_QUERY_VAL_LEN];
    uint8_t count = 0;
};

class Scanner {
public:
    // Initialise Serial1 on the pins defined in config.h.
    void begin();

    // Non-blocking poll — call every loop() iteration.
    // Accumulates bytes; finalises a frame on \n or timeout.
    void update();

    bool      hasFrame() const { return _frameReady; }

    // Consume and return the pending frame. Clears the ready flag.
    ScanFrame takeFrame();

    // Parse a QUERY frame into individual lookup values.
    // Each |PPQ::<value> segment is extracted and null-terminated.
    // Returns false if the frame type is not QUERY or has no valid segments.
    bool parseQuery(const ScanFrame& frame, QueryResult& out) const;

private:
    char      _buf[SCANNER_FRAME_MAXLEN];
    uint16_t  _len         = 0;
    bool      _frameReady  = false;
    ScanFrame _frame;
    uint32_t  _lastByteAt  = 0;

    void _finalizeFrame();
};

extern Scanner scanner;

// ============================================================
// Physical scan-trigger button
// Separate from the Scanner class — the button is a GPIO input,
// not part of the UART driver.
// ============================================================

#define SCAN_BTN_DEBOUNCE_MS  30   // minimum press duration to register

// Configure PIN_SCAN_BTN as input with internal pull-up.
// Called once from Scanner::begin().
void scanBtnBegin();

// Returns true while the button is being held down (debounced, active-low).
// Non-blocking — reads the GPIO state directly.
bool scanBtnPressed();
