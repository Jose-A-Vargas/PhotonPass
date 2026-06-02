// PassGen interactive CLI — 115200 baud serial
// Build:   pio run -e passgen_test
// Flash:   pio run -e passgen_test --target upload
// Monitor: pio device monitor -e passgen_test
//
// Commands:
//   pw <len> [A|S|H|B]   password  (A=ascii S=standard H=hex B=base64)
//   pp <words> [sep]     passphrase  (1-4 words, separator char)
//   <n>                  repeat last command n times
//   help / ?             show menu

#include <Arduino.h>
#include <WiFi.h>
#include "passgen.h"

// ============================================================
// State
// ============================================================

static char    sLine[128];
static uint8_t sLineLen = 0;

enum class LastCmd : uint8_t { NONE, PW, PP };
static LastCmd     sLastCmd     = LastCmd::NONE;
static CharProfile sLastProfile = CharProfile::STANDARD;
static uint8_t     sLastPwLen   = 20;
static uint8_t     sLastWords   = 3;
static char        sLastSep     = '-';

// ============================================================
// Serial line reader — echo + backspace support
// ============================================================

static bool pollLine() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            sLine[sLineLen] = '\0';
            sLineLen = 0;
            Serial.println();
            return true;
        }
        if ((c == 8 || c == 127) && sLineLen > 0) {
            sLineLen--;
            Serial.print("\b \b");
            continue;
        }
        if (sLineLen < sizeof(sLine) - 1) {
            sLine[sLineLen++] = c;
            Serial.print(c);
        }
    }
    return false;
}

// ============================================================
// Generators
// ============================================================

static const char* profileName(CharProfile p) {
    switch (p) {
        case CharProfile::FULL_ASCII:   return "ascii";
        case CharProfile::STANDARD:     return "standard";
        case CharProfile::HEXADECIMAL:  return "hex";
        case CharProfile::BASE64:       return "base64";
        default:                        return "?";
    }
}

static void runPasswords(uint8_t len, CharProfile profile, uint8_t count) {
    char buf[256];
    Serial.printf("  [pw len=%u profile=%s]\n", len, profileName(profile));
    for (uint8_t i = 0; i < count; i++) {
        if (generatePassword(buf, len, profile))
            Serial.printf("  %s\n", buf);
        else
            Serial.println("  [error]");
    }
}

static void runPassphrases(uint8_t words, char sep, uint8_t count) {
    char buf[PASSPHRASE_MAX_BUF];
    Serial.printf("  [pp words=%u sep='%c']\n", words, sep);
    for (uint8_t i = 0; i < count; i++) {
        if (generatePassphrase(buf, sizeof(buf), words, sep))
            Serial.printf("  %s\n", buf);
        else
            Serial.println("  [error]");
    }
}

// ============================================================
// Help
// ============================================================

static void printHelp() {
    Serial.println();
    Serial.println("  pw <len> [profile]   generate password");
    Serial.println("    A  full ASCII printable (0x20-0x7E)");
    Serial.println("    S  standard alphanumeric + !@#$%^&*  (default)");
    Serial.println("    H  hex  0-9 A-F");
    Serial.println("    B  base64  A-Z a-z 0-9 + /");
    Serial.println();
    Serial.println("  pp <words> [sep]     generate passphrase");
    Serial.println("    words  1-4  (default 3)");
    Serial.println("    sep    separator character  (default -)");
    Serial.println();
    Serial.println("  <n>                  repeat last command n times");
    Serial.println("  help / ?             show this menu");
    Serial.println();
}

// ============================================================
// Command parser
// ============================================================

static void processLine(const char* line) {
    char tmp[128];
    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char* tok = strtok(tmp, " \t");
    if (!tok || tok[0] == '\0') { Serial.print("> "); return; }

    char* endptr;

    // Plain number → repeat last command
    long n = strtol(tok, &endptr, 10);
    if (*endptr == '\0' && n >= 1 && n <= 100) {
        if (sLastCmd == LastCmd::PW)
            runPasswords(sLastPwLen, sLastProfile, (uint8_t)n);
        else if (sLastCmd == LastCmd::PP)
            runPassphrases(sLastWords, sLastSep, (uint8_t)n);
        else
            Serial.println("  (no previous command — run pw or pp first)");
        Serial.print("> ");
        return;
    }

    // pw
    if (strcasecmp(tok, "pw") == 0) {
        uint8_t    len  = sLastPwLen;
        CharProfile prof = sLastProfile;

        char* a1 = strtok(nullptr, " \t");
        if (a1) {
            long v = strtol(a1, &endptr, 10);
            if (*endptr != '\0' || v < 1 || v > 92) {
                Serial.println("  length must be 1-92");
                Serial.print("> "); return;
            }
            len = (uint8_t)v;
        }

        char* a2 = strtok(nullptr, " \t");
        if (a2) {
            switch (toupper((unsigned char)a2[0])) {
                case 'A': prof = CharProfile::FULL_ASCII;  break;
                case 'S': prof = CharProfile::STANDARD;    break;
                case 'H': prof = CharProfile::HEXADECIMAL; break;
                case 'B': prof = CharProfile::BASE64;      break;
                default:
                    Serial.println("  profile must be A S H or B");
                    Serial.print("> "); return;
            }
        }

        sLastCmd = LastCmd::PW; sLastPwLen = len; sLastProfile = prof;
        runPasswords(len, prof, 1);
        Serial.print("> ");
        return;
    }

    // pp
    if (strcasecmp(tok, "pp") == 0) {
        uint8_t words = sLastWords;
        char    sep   = sLastSep;

        char* a1 = strtok(nullptr, " \t");
        if (a1) {
            long v = strtol(a1, &endptr, 10);
            if (*endptr != '\0' || v < 1 || v > 4) {
                Serial.println("  words must be 1-4");
                Serial.print("> "); return;
            }
            words = (uint8_t)v;
        }

        char* a2 = strtok(nullptr, " \t");
        if (a2 && a2[0] != '\0') sep = a2[0];

        sLastCmd = LastCmd::PP; sLastWords = words; sLastSep = sep;
        runPassphrases(words, sep, 1);
        Serial.print("> ");
        return;
    }

    // help
    if (strcasecmp(tok, "help") == 0 || strcmp(tok, "?") == 0) {
        printHelp();
        Serial.print("> ");
        return;
    }

    Serial.printf("  unknown: '%s'  (type help)\n", tok);
    Serial.print("> ");
}

// ============================================================
// Entry points
// ============================================================

void setup() {
    WiFi.mode(WIFI_OFF);
    btStop();

    Serial.begin(115200);
    delay(2000);

    Serial.println("\nPhotonPass PassGen CLI");
    Serial.println("======================");
    printHelp();
    Serial.print("> ");
}

void loop() {
    if (pollLine())
        processLine(sLine);
}
