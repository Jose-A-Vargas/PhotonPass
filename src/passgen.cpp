#include "passgen.h"
#include <esp_random.h>
#include <string.h>
#include <ctype.h>

// ============================================================
// Password charsets
// ============================================================

// 0x20–0x7E (95 printable ASCII chars, verified by count)
static const char CHARSET_FULL_ASCII[] =
    " !\"#$%&'()*+,-./0123456789:;<=>?@"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
    "abcdefghijklmnopqrstuvwxyz{|}~";

static const char CHARSET_STANDARD[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%^&*";

static const char CHARSET_HEX[]    = "0123456789ABCDEF";

static const char CHARSET_BASE64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// ============================================================
// Phonotactic arrays — passphrase engine
//
// Seven syllable templates (matching steps.txt spec):
//   V      vowel only
//   CV     simple onset + vowel
//   VC     vowel + simple coda
//   CVC    simple onset + vowel + simple coda
//   CCVC   cluster onset + vowel + simple coda
//   CVCC   simple onset + vowel + cluster coda
//   CCVCC  cluster onset + vowel + cluster coda
//
// Single-syllable pool lower bound:
//   V:     14
//   CV:    18 × 14         =    252
//   VC:    14 × 14         =    196
//   CVC:   18 × 14 × 14   =  3,528
//   CCVC:  25 × 14 × 14   =  4,900
//   CVCC:  18 × 14 × 25   =  6,300
//   CCVCC: 25 × 14 × 25   =  8,750
//                     total: 23,940
//
// Two-syllable words: 23,940² ≈ 573 million — total pool >> 500,000.
// ============================================================

// "C" slots in templates
static const char* const SIMPLE_ONSETS[] = {
    "b","d","f","g","h","j","k","l","m","n","p","r","s","t","v","w","y","z"
};
static const uint8_t SIMPLE_ONSET_COUNT = 18;

// "CC" slots in templates
static const char* const CLUSTER_ONSETS[] = {
    "bl","br","ch","cl","cr","dr","fl","fr","gl","gr",
    "kl","pl","pr","sc","sh","sk","sl","sm","sn","sp","st","sw","th","tr","tw"
};
static const uint8_t CLUSTER_ONSET_COUNT = 25;

// "V" slots — single vowels and digraphs unified
static const char* const VOWELS[] = {
    "a","e","i","o","u","ai","ay","ea","ee","ie","oa","oo","ou","ue"
};
static const uint8_t VOWEL_COUNT = 14;

// terminal "C" slots
static const char* const SIMPLE_CODAS[] = {
    "b","d","f","g","k","l","m","n","p","r","s","t","x","z"
};
static const uint8_t SIMPLE_CODA_COUNT = 14;

// terminal "CC" slots
static const char* const CLUSTER_CODAS[] = {
    "ck","ft","ld","lf","lk","lm","lp","lt","mb","mp",
    "nd","ng","nk","nt","pt","rd","rk","rm","rn","rp","rs","rt","sk","sp","st"
};
static const uint8_t CLUSTER_CODA_COUNT = 25;

// ============================================================
// Internal helpers
// ============================================================

// Uniform random index in [0, n) via rejection sampling — no modulo bias.
static uint32_t randBelow(uint32_t n) {
    if (n <= 1) return 0;
    uint32_t threshold = (0xFFFFFFFFu / n) * n;
    uint32_t r;
    do { r = esp_random(); } while (r >= threshold);
    return r % n;
}

// Append frag into buf[off..bufLen-1]. Returns false if it would overflow.
// Deliberately does NOT write the null terminator — callers do that once at the end.
static bool append(char* buf, size_t& off, size_t bufLen, const char* frag) {
    size_t len = strlen(frag);
    if (off + len >= bufLen) return false;
    memcpy(buf + off, frag, len);
    off += len;
    return true;
}

// Build one syllable into buf starting at *off.
// capitalize: uppercase the first character written (first syllable of each word).
// Returns false if the buffer cannot fit the syllable.
static bool buildSyllable(char* buf, size_t& off, size_t bufLen, bool capitalize) {
    // Template selection (0–6 maps to V, CV, VC, CVC, CCVC, CVCC, CCVCC)
    uint8_t tmpl = (uint8_t)randBelow(7);

    const char* onset = nullptr;
    const char* vowel = VOWELS[randBelow(VOWEL_COUNT)];
    const char* coda  = nullptr;

    switch (tmpl) {
        case 0: /* V */                                                                         break;
        case 1: /* CV    */ onset = SIMPLE_ONSETS[randBelow(SIMPLE_ONSET_COUNT)];              break;
        case 2: /* VC    */                          coda = SIMPLE_CODAS[randBelow(SIMPLE_CODA_COUNT)];   break;
        case 3: /* CVC   */ onset = SIMPLE_ONSETS[randBelow(SIMPLE_ONSET_COUNT)];
                                                     coda = SIMPLE_CODAS[randBelow(SIMPLE_CODA_COUNT)];   break;
        case 4: /* CCVC  */ onset = CLUSTER_ONSETS[randBelow(CLUSTER_ONSET_COUNT)];
                                                     coda = SIMPLE_CODAS[randBelow(SIMPLE_CODA_COUNT)];   break;
        case 5: /* CVCC  */ onset = SIMPLE_ONSETS[randBelow(SIMPLE_ONSET_COUNT)];
                                                     coda = CLUSTER_CODAS[randBelow(CLUSTER_CODA_COUNT)]; break;
        case 6: /* CCVCC */ onset = CLUSTER_ONSETS[randBelow(CLUSTER_ONSET_COUNT)];
                                                     coda = CLUSTER_CODAS[randBelow(CLUSTER_CODA_COUNT)]; break;
    }

    bool isFirst = capitalize;  // only the very first char of the word gets uppercased

    if (onset) {
        char tmp[4] = {};
        strncpy(tmp, onset, 3);
        if (isFirst) { tmp[0] = (char)toupper((unsigned char)tmp[0]); isFirst = false; }
        if (!append(buf, off, bufLen, tmp)) return false;
    }

    {
        char tmp[4] = {};
        strncpy(tmp, vowel, 3);
        if (isFirst) { tmp[0] = (char)toupper((unsigned char)tmp[0]); }
        if (!append(buf, off, bufLen, tmp)) return false;
    }

    if (coda) {
        if (!append(buf, off, bufLen, coda)) return false;
    }

    return true;
}

// ============================================================
// Public API
// ============================================================

bool generatePassword(char* buf, uint8_t length, CharProfile profile) {
    if (!buf || length == 0) return false;

    const char* charset;
    uint32_t    charsetLen;

    switch (profile) {
        case CharProfile::FULL_ASCII:
            charset    = CHARSET_FULL_ASCII;
            charsetLen = sizeof(CHARSET_FULL_ASCII) - 1;  // 95
            break;
        case CharProfile::STANDARD:
            charset    = CHARSET_STANDARD;
            charsetLen = sizeof(CHARSET_STANDARD) - 1;    // 70
            break;
        case CharProfile::HEXADECIMAL:
            charset    = CHARSET_HEX;
            charsetLen = sizeof(CHARSET_HEX) - 1;         // 16
            break;
        case CharProfile::BASE64:
            charset    = CHARSET_BASE64;
            charsetLen = sizeof(CHARSET_BASE64) - 1;      // 64
            break;
        default:
            return false;
    }

    for (uint8_t i = 0; i < length; i++) {
        buf[i] = charset[randBelow(charsetLen)];
    }
    buf[length] = '\0';
    return true;
}

bool generatePassphrase(char* buf, size_t bufLen, uint8_t wordCount, char separator) {
    if (!buf || bufLen < 2 || wordCount < 1 || wordCount > 4) return false;

    size_t off = 0;

    for (uint8_t w = 0; w < wordCount; w++) {
        if (w > 0) {
            if (off + 1 >= bufLen) return false;
            buf[off++] = separator;
        }

        uint8_t syllableCount = (uint8_t)(randBelow(3) + 1);  // 1–3 syllables

        for (uint8_t s = 0; s < syllableCount; s++) {
            if (!buildSyllable(buf, off, bufLen, s == 0)) return false;
        }
    }

    buf[off] = '\0';
    return true;
}
