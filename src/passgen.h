#pragma once
#include <stdint.h>
#include <stddef.h>

enum class CharProfile : uint8_t {
    FULL_ASCII,  // every printable glyph 0x20–0x7E (95 chars)
    STANDARD,    // alphanumeric + !@#$%^&*          (70 chars)
    HEXADECIMAL, // 0-9 A-F                          (16 chars)
    BASE64,      // A-Z a-z 0-9 + /                  (64 chars)
};

// Max buffer size for generatePassphrase():
// 4 words × 3 syllables × 6 chars + 3 separators + null = 76 bytes; 80 is safe.
#define PASSPHRASE_MAX_BUF  80

// Fill buf with 'length' random characters drawn from profile.
// Uses rejection sampling on esp_random() for uniform distribution (no modulo bias).
// buf must be at least length+1 bytes. Returns false on invalid args.
bool generatePassword(char* buf, uint8_t length, CharProfile profile);

// Fill buf with a pronounceable pseudo-word passphrase.
//   wordCount  : 1–4 words
//   separator  : character placed between words (e.g. '-', ' ', '.')
// buf must be at least PASSPHRASE_MAX_BUF bytes.
// Each word is 1–3 syllables; first letter of each word is capitalised.
// Syllable pool (1-syllable words alone): ~24,000 combinations.
// Total word pool across 1–3-syllable words: well in excess of 500,000.
// No heap allocation — all state lives in static arrays and stack locals.
bool generatePassphrase(char* buf, size_t bufLen, uint8_t wordCount, char separator = '-');
