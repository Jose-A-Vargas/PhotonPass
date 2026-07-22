#pragma once

#ifdef BOARD_EPD47_S3
// ============================================================
// Hardware Pins — LilyGo T5 4.7" S3 v2.4  (T5_47_PLUS)
// EPD parallel bus managed internally by LilyGo epdiy fork.
// Touch I2C pins confirmed from examples/touchtest/pins.h.
// ============================================================
#define PIN_TOUCH_SDA       18
#define PIN_TOUCH_SCL       17
#define PIN_TOUCH_INT       47   // not used in polling mode, here for reference

#else
// ============================================================
// Hardware Pins — XIAO ESP32S3 Plus
// SPI: SCK=D8, MOSI=D10  (MISO unused — E-Ink is write-only)
// I2C: SDA=D4, SCL=D5
// UART1: TX=D6, RX=D7  (GM60 scanner)
// ============================================================
#define PIN_EINK_RST        D0
#define PIN_EINK_CS         D1
#define PIN_EINK_DC         D2
#define PIN_EINK_BUSY       D3
#define PIN_EINK_SCK        D8
#define PIN_EINK_MOSI       D10

#define PIN_TOUCH_SDA       D4
#define PIN_TOUCH_SCL       D5
#define PIN_TOUCH_INT       D9  // repurposed MISO — safe since E-Ink is write-only
#endif

#define PIN_SCANNER_TX      D6
#define PIN_SCANNER_RX      D7
#define SCANNER_BAUD        9600

// Physical scan-trigger button — active-low, internal pull-up.
// Wired in parallel with the GM60 trigger input so one press simultaneously
// signals the MCU and fires the scanner hardware.
// Assign to whichever GPIO is free on your board (XIAO ESP32S3 Plus module pad).
#ifndef PIN_SCAN_BTN
#define PIN_SCAN_BTN        39   // MTCK back pad (GPIO 39) — all D0-D10 are taken by display/touch/scanner
#endif
#define SCAN_BTN_TIMEOUT_MS 3000  // max wait for a frame after button press

// ============================================================
// Security
// ============================================================
#define PBKDF2_ITERATIONS   10000
#define SESSION_TIMEOUT_MS  120000UL  // 2 minutes of inactivity
#define QR_DISPLAY_TIMEOUT_MS 30000UL // password QR auto-clears after 30 s
#define MAX_UNLOCK_ATTEMPTS 5

// ============================================================
// Crypto sizes
// ============================================================
#define AES_KEY_LEN         32   // AES-256
#define GCM_NONCE_LEN       12   // 96-bit GCM nonce
#define GCM_TAG_LEN         16   // 128-bit auth tag
#define PBKDF2_SALT_LEN     32   // per-device salt

// ============================================================
// Vault
// ============================================================
#define VAULT_FILE_PATH         "/vault.bin"
#define VAULT_VERSION           1
#define VAULT_MAX_RECORDS       256
#define VAULT_HISTORY_WEB       10   // most record types
#define VAULT_HISTORY_PC        40   // PC_ACCOUNT records

// vault.bin magic bytes: "PPV1"
#define VAULT_MAGIC_0  0x50
#define VAULT_MAGIC_1  0x50
#define VAULT_MAGIC_2  0x56
#define VAULT_MAGIC_3  0x31
