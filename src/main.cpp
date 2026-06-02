#include <Arduino.h>
#include <LittleFS.h>
#include <USB.h>
#include <WiFi.h>
#include <string.h>
#include "config.h"
#include "crypto.h"
#include "vault.h"
#include "display.h"
#include "scanner.h"
#include "passgen.h"
#include "hid.h"
#include "sync.h"
#include "backup.h"

// Keyboard is defined in hid.cpp; referenced here only for documentation.
extern USBHIDKeyboard Keyboard;

// ============================================================
// Globals
// ============================================================

VaultState gVault;

// Temporary MFA buffers — wiped immediately after key derivation
static uint8_t gToken[SCANNER_FRAME_MAXLEN];
static uint16_t gTokenLen = 0;
static char    gPassphrase[128];

// Active query and matched record (copied out of vault so it can be wiped)
static QueryResult gQueryResult;
static VaultRecord gMatchedRecord;
static bool        gHasMatch = false;

// Failed unlock counter — survives state changes, reset only on success
static uint8_t gFailCount = 0;

// Sync state
static uint8_t gSyncPeerVaultIdx[50]; // vault record indices of PRESHARE_KEY records
static uint8_t gSyncPeerCount   = 0;
static uint8_t gSyncPeerIdx     = 0;
static uint8_t gSyncMerged      = 0;  // records updated (inbound result)
static uint8_t gSyncFramesSent  = 0;  // total frames shown (outbound result)
static bool    gSyncIsOutbound  = false;

// Search state
static char    gSearchQuery[64]                  = {};
static uint8_t gSearchResults[VAULT_MAX_RECORDS] = {};
static uint8_t gSearchResultCount = 0;
static uint8_t gSearchPage        = 0;

// ============================================================
// State machine
// ============================================================

enum class State : uint8_t {
    LOCKED,       // vault locked, tap-to-start prompt
    MFA_SCAN,     // waiting for physical QR token via GM60
    MFA_PHRASE,   // accepting passphrase via E-Ink keyboard
    DERIVING,     // PBKDF2 running, vault decrypt in progress
    UNLOCKED,     // vault open, idle, waiting for query scan
    QUERYING,     // query frame received, searching records
    OUTPUT_SEL,   // asking user: keyboard or QR?
    OUTPUT_HID,   // typing password via USB HID
    OUTPUT_QR,    // showing password QR on E-Ink
    LOCKED_OUT,    // MAX_UNLOCK_ATTEMPTS exceeded, reboot required
    SYNC_MENU,     // choose Send or Receive
    SYNC_PEER_SEL, // pick which PRESHARE_KEY peer to send to
    SYNC_OUTBOUND, // streaming QR frames for peer to scan
    SYNC_INBOUND,  // receiving PPSS: QR frames from peer
    SYNC_RESULT,    // show sync outcome then return to UNLOCKED
    SEARCH_INPUT,   // soft keyboard to enter search query
    SEARCH_RESULTS, // scrollable list of matching records
    BACKUP_MENU,    // choose Export or Import
    BACKUP_EXPORT,  // one-shot: encrypt vault → DMZ /backup.bin
    BACKUP_IMPORT,  // one-shot: read DMZ /backup.bin, merge records
};

static State gState       = State::LOCKED;
static bool  gStateEntered = true;  // true on the first loop() after a transition

static void enterState(State s) {
    gState        = s;
    gStateEntered = true;
}

// ============================================================
// Utilities
// ============================================================

static void lockDownRadios() {
    WiFi.mode(WIFI_OFF);
    btStop();
}

static void wipeAuthBuffers() {
    secureClear(gToken,      sizeof(gToken));
    secureClear(gPassphrase, sizeof(gPassphrase));
    gTokenLen = 0;
}

// Read only the vault header to get the PBKDF2 salt before key derivation.
// Returns false if no vault file exists (first boot).
static bool peekVaultSalt(uint8_t* outSalt) {
    File f = LittleFS.open(VAULT_FILE_PATH, FILE_READ);
    if (!f) return false;
    VaultHeader hdr;
    bool ok = (f.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr));
    f.close();
    if (ok) memcpy(outSalt, hdr.salt, PBKDF2_SALT_LEN);
    return ok;
}

// Search vault for the first record matching any query value in qr.
static const VaultRecord* findRecord(const QueryResult& qr) {
    for (uint8_t qi = 0; qi < qr.count; qi++) {
        for (const auto& r : gVault.records) {
            if (r.queryValue == qr.values[qi]) return &r;
        }
    }
    return nullptr;
}

static const char* recordTypeName(RecordType t) {
    switch (t) {
        case RecordType::WEB_SERVICE:  return "WEB";
        case RecordType::PC_ACCOUNT:   return "PC";
        case RecordType::API_TOKEN:    return "API";
        case RecordType::ENCRYPT_KEY:  return "KEY";
        case RecordType::PRESHARE_KEY: return "SYNC";
        case RecordType::BACKUP_KEY:   return "BKUP";
        default:                       return "???";
    }
}

static bool containsCI(const char* haystack, const char* needle) {
    if (!needle || !needle[0]) return true;
    if (!haystack) return false;
    size_t nLen = strlen(needle);
    for (size_t i = 0; haystack[i]; i++) {
        bool match = true;
        for (size_t j = 0; j < nLen; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static void doSearch(const char* query) {
    gSearchResultCount = 0;
    gSearchPage        = 0;
    for (uint8_t i = 0; i < (uint8_t)gVault.records.size(); i++) {
        const VaultRecord& r = gVault.records[i];
        if (containsCI(r.domain.c_str(),        query) ||
            containsCI(r.username.c_str(),       query) ||
            containsCI(r.queryValue.c_str(),     query) ||
            containsCI(recordTypeName(r.type),   query)) {
            gSearchResults[gSearchResultCount++] = i;
        }
    }
}

// ============================================================
// State handlers
// ============================================================

// ------ LOCKED ------
static void handleLocked() {
    if (gStateEntered) {
        display.showMessage("PhotonPass", "Tap to unlock");
        gStateEntered = false;
    }
    if (display.readTouch().pressed) enterState(State::MFA_SCAN);
}

// ------ MFA_SCAN ------
static void handleMfaScan() {
    if (gStateEntered) {
        display.showMessage("Step 1 of 2", "Aim at token QR, press scan button");
        gStateEntered = false;
    }

    if (!scanBtnPressed()) return;

    // Button pressed — poll for frame up to SCAN_BTN_TIMEOUT_MS
    uint32_t deadline = millis() + SCAN_BTN_TIMEOUT_MS;
    while (millis() < deadline) {
        scanner.update();
        if (scanner.hasFrame()) break;
        delay(10);
    }

    if (!scanner.hasFrame()) {
        display.showMessage("Step 1 of 2", "No QR detected — try again");
        return;
    }

    ScanFrame f = scanner.takeFrame();
    if (f.type == ScanFrame::Type::TOKEN && f.rawLen > 0) {
        uint16_t copyLen = f.rawLen < (uint16_t)sizeof(gToken)
                           ? f.rawLen : (uint16_t)sizeof(gToken) - 1;
        memcpy(gToken, f.raw, copyLen);
        gTokenLen = copyLen;
        enterState(State::MFA_PHRASE);
    } else {
        display.showMessage("Step 1 of 2", "Wrong QR type — try again");
    }
}

// ------ MFA_PHRASE ------
// softKeyboard() is blocking — the loop pauses here until Enter or the
// user cancels (empty confirm). Session timeout does not fire during this
// state; add a keyboard-internal timeout in a future iteration if needed.
static void handleMfaPhrase() {
    bool ok = display.softKeyboard(gPassphrase, sizeof(gPassphrase),
                                    "Step 2 of 2: Enter passphrase");
    gStateEntered = false;  // softKeyboard handled all display work

    if (ok && gPassphrase[0] != '\0') {
        enterState(State::DERIVING);
    } else {
        wipeAuthBuffers();
        enterState(State::LOCKED);
    }
}

// ------ DERIVING ------
static void handleDeriving() {
    if (gStateEntered) {
        display.showMessage("Unlocking...", "Deriving key, please wait");
        gStateEntered = false;
    }

    bool vaultExists = peekVaultSalt(gVault.salt);
    if (!vaultExists) {
        // First boot — generate a fresh salt; vault will be created empty.
        generateSalt(gVault.salt);
    }

    CryptoResult kr = deriveKey(
        gPassphrase, strlen(gPassphrase),
        gToken,      gTokenLen,
        gVault.salt,
        gVault.masterKey
    );

    wipeAuthBuffers();  // token + passphrase not needed past this point

    if (kr != CryptoResult::OK) {
        display.showMessage("Error", "Key derivation failed");
        delay(2000);
        enterState(State::LOCKED);
        return;
    }

    if (!vaultExists) {
        // Create an empty vault so future boots can load it normally.
        gVault.unlocked   = true;
        gVault.unlockedAt = millis();
        saveVault(gVault);
        gFailCount = 0;
        enterState(State::UNLOCKED);
        return;
    }

    if (!loadVault(gVault)) {
        // GCM auth failure — wrong key or tampered vault.bin
        gFailCount++;
        secureClear(gVault.masterKey, AES_KEY_LEN);

        if (gFailCount >= MAX_UNLOCK_ATTEMPTS) {
            enterState(State::LOCKED_OUT);
        } else {
            char msg[40];
            snprintf(msg, sizeof(msg), "%d attempt(s) left",
                     MAX_UNLOCK_ATTEMPTS - gFailCount);
            display.showMessage("Wrong key", msg);
            delay(2000);
            enterState(State::LOCKED);
        }
        return;
    }

    gFailCount = 0;
    enterState(State::UNLOCKED);
}

// ------ UNLOCKED ------
static void handleUnlocked() {
    if (gStateEntered) {
        display.fillScreen(false);
        char countStr[24];
        snprintf(countStr, sizeof(countStr), "%u record(s)", (unsigned)gVault.records.size());
        display.drawTextCentered(80,  "Vault Open");
        display.drawTextCentered(115, countStr);
        display.drawTextCentered(150, "Press scan button to query");
        // Bottom buttons: [SEARCH] — [BACKUP] — [SYNC]
        display.drawRect(13,  244, 96, 42);  display.drawText(22,  270, "SEARCH");
        display.drawRect(152, 244, 96, 42);  display.drawText(160, 270, "BACKUP");
        display.drawRect(291, 244, 96, 42);  display.drawText(307, 270, "SYNC");
        display.refresh();
        gStateEntered = false;
    }

    if (millis() - gVault.unlockedAt > SESSION_TIMEOUT_MS) {
        gVault.lock();
        display.showMessage("Locked", "Session timed out");
        delay(1500);
        enterState(State::LOCKED);
        return;
    }

    TouchPoint tp = display.readTouch();
    if (tp.pressed && tp.y >= 244 && tp.y < 286) {
        gVault.unlockedAt = millis();
        if (tp.x >= 13  && tp.x < 109) { enterState(State::SEARCH_INPUT); return; }
        if (tp.x >= 152 && tp.x < 248) { enterState(State::BACKUP_MENU);  return; }
        if (tp.x >= 291)               { enterState(State::SYNC_MENU);    return; }
    }

    // Physical scan button — wait up to SCAN_BTN_TIMEOUT_MS for a QUERY frame
    if (!scanBtnPressed()) return;

    uint32_t deadline = millis() + SCAN_BTN_TIMEOUT_MS;
    while (millis() < deadline) {
        scanner.update();
        if (scanner.hasFrame()) break;
        delay(10);
    }

    if (!scanner.hasFrame()) return;  // nothing scanned — stay in UNLOCKED

    ScanFrame f = scanner.takeFrame();
    if (f.type != ScanFrame::Type::QUERY) return;
    if (scanner.parseQuery(f, gQueryResult) && gQueryResult.count > 0) {
        gVault.unlockedAt = millis();
        enterState(State::QUERYING);
    }
}

// ------ QUERYING ------
static void handleQuerying() {
    if (!gStateEntered) return;
    gStateEntered = false;

    const VaultRecord* match = findRecord(gQueryResult);
    if (!match) {
        display.showMessage("Not found", "No matching record in vault");
        delay(2000);
        gVault.unlockedAt = millis();  // reset timer; user is still active
        enterState(State::UNLOCKED);
        return;
    }

    gMatchedRecord = *match;  // deep-copy; wiped after output
    gHasMatch      = true;

    char info[48];
    snprintf(info, sizeof(info), "%.46s", match->domain.c_str());
    display.showMessage(info, match->username.c_str());
    delay(1200);
    enterState(State::OUTPUT_SEL);
}

// ------ OUTPUT_SEL ------
static void handleOutputSel() {
    if (gStateEntered) {
        display.fillScreen(false);
        display.drawTextCentered(36, "Deliver password via:");

        // Left — Keyboard (HID)
        display.drawRect(14, 80, 172, 120);
        display.drawText(34, 148, "Keyboard");

        // Right — QR Code
        display.drawRect(214, 80, 172, 120);
        display.drawText(246, 148, "QR Code");

        // Cancel — bottom centre
        display.drawRect(150, 240, 100, 46);
        display.drawText(168, 268, "CANCEL");

        display.refresh();
        gStateEntered = false;
    }

    // Decoded password is live in gMatchedRecord — enforce session timeout here
    // so it can't linger in RAM if the user walks away mid-prompt.
    if (millis() - gVault.unlockedAt > SESSION_TIMEOUT_MS) {
        secureClear((void*)gMatchedRecord.password.data(),
                    gMatchedRecord.password.size());
        gHasMatch = false;
        gVault.lock();
        display.showMessage("Locked", "Session timed out");
        delay(1500);
        enterState(State::LOCKED);
        return;
    }

    TouchPoint tp = display.readTouch();
    if (!tp.pressed) return;

    if (tp.x >= 14 && tp.x < 186 && tp.y >= 80 && tp.y < 200) {
        enterState(State::OUTPUT_HID);
    } else if (tp.x >= 214 && tp.x < 386 && tp.y >= 80 && tp.y < 200) {
        enterState(State::OUTPUT_QR);
    } else if (tp.x >= 150 && tp.x < 250 && tp.y >= 240) {
        // Cancel — wipe the decoded record and return to idle
        secureClear((void*)gMatchedRecord.password.data(),
                    gMatchedRecord.password.size());
        gHasMatch = false;
        gVault.unlockedAt = millis();
        enterState(State::UNLOCKED);
    }
}

// ------ OUTPUT_HID ------
static void handleOutputHid() {
    if (!gStateEntered) return;
    gStateEntered = false;

    if (!gHasMatch) { enterState(State::UNLOCKED); return; }

    HidResult res = typePassword(gMatchedRecord.password.c_str());

    secureClear((void*)gMatchedRecord.password.data(),
                gMatchedRecord.password.size());
    gHasMatch = false;

    if (res == HidResult::OK) {
        display.showMessage("Delivered", "Password typed via keyboard");
    } else if (res == HidResult::NOT_READY) {
        display.showMessage("Error", "USB host not connected");
    }
    // EMPTY_PW shouldn't occur — vault records always have a password

    delay(1500);
    gVault.unlockedAt = millis();
    enterState(State::UNLOCKED);
}

// ------ OUTPUT_QR ------
static void handleOutputQr() {
    if (!gStateEntered) return;
    gStateEntered = false;

    if (!gHasMatch) { enterState(State::UNLOCKED); return; }

    // maxSize=150 → ~4 px/module at typical password lengths (~1 mm on 4.2" panel).
    // Readable by the GM60 at 5–15 cm; unresolvable by cameras beyond ~50 cm.
    // ECC_LOW is the QR-spec minimum (7% recovery) — no lower option exists.
    display.showQR(DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2, 150,
                   gMatchedRecord.password.c_str(),
                   gMatchedRecord.domain.c_str());

    // Short dedicated timeout — limits camera capture window independently of
    // the session timeout.  User can re-query if they need the QR again.
    uint32_t qrShownAt = millis();
    while (true) {
        if (display.readTouch().pressed) break;
        if (millis() - qrShownAt        > QR_DISPLAY_TIMEOUT_MS) break;
        if (millis() - gVault.unlockedAt > SESSION_TIMEOUT_MS)    break;
        delay(50);
    }

    secureClear((void*)gMatchedRecord.password.data(),
                gMatchedRecord.password.size());
    gHasMatch = false;
    gVault.unlockedAt = millis();
    enterState(State::UNLOCKED);
}

// ------ LOCKED_OUT ------
static void handleLockedOut() {
    if (gStateEntered) {
        display.showMessage("LOCKED OUT", "Reboot required");
        gStateEntered = false;
    }
    // No exit — hardware reboot is the only recovery path
}

// ============================================================
// Sync handlers
// ============================================================

// ------ SYNC_MENU ------
static void handleSyncMenu() {
    if (gStateEntered) {
        display.fillScreen(false);
        display.drawTextCentered(38, "Sync with Peer");
        // SEND button (left)
        display.drawRect(20, 80, 170, 120);
        display.drawText(65, 148, "SEND");
        // RECEIVE button (right)
        display.drawRect(210, 80, 170, 120);
        display.drawText(234, 148, "RECEIVE");
        // BACK button (bottom centre)
        display.drawRect(150, 240, 100, 46);
        display.drawText(168, 268, "BACK");
        display.refresh();
        gStateEntered = false;
    }

    TouchPoint tp = display.readTouch();
    if (!tp.pressed) return;

    if (tp.x >= 20 && tp.x < 190 && tp.y >= 80 && tp.y < 200) {
        enterState(State::SYNC_PEER_SEL);
    } else if (tp.x >= 210 && tp.x < 380 && tp.y >= 80 && tp.y < 200) {
        syncReceiver.reset();
        enterState(State::SYNC_INBOUND);
    } else if (tp.x >= 150 && tp.x < 250 && tp.y >= 240 && tp.y < 286) {
        gVault.unlockedAt = millis();
        enterState(State::UNLOCKED);
    }
}

// ------ SYNC_PEER_SEL ------
static void drawPeerSelScreen() {
    display.fillScreen(false);
    display.drawTextCentered(30, "Select Peer");

    if (gSyncPeerCount == 0) {
        display.drawTextCentered(130, "No peers configured.");
        display.drawTextCentered(158, "Add PRESHARE_KEY records.");
        display.drawRect(150, 240, 100, 46);
        display.drawText(168, 268, "BACK");
    } else {
        const VaultRecord& peer = gVault.records[gSyncPeerVaultIdx[gSyncPeerIdx]];

        // Navigation arrows
        display.drawRect(8,   100, 60, 60);
        display.drawText(22,  138, "<");
        display.drawRect(332, 100, 60, 60);
        display.drawText(346, 138, ">");

        // Peer name (domain field)
        char name[30];
        snprintf(name, sizeof(name), "%.28s", peer.domain.c_str());
        display.drawTextCentered(132, name);

        // Position indicator
        char pos[16];
        snprintf(pos, sizeof(pos), "%u of %u", gSyncPeerIdx + 1, gSyncPeerCount);
        display.drawTextCentered(163, pos);

        // Action buttons
        display.drawRect(20,  240, 120, 46);
        display.drawText(43,  268, "CANCEL");
        display.drawRect(260, 240, 120, 46);
        display.drawText(295, 268, "SEND");
    }
    display.refresh();
}

static void handleSyncPeerSel() {
    if (gStateEntered) {
        gSyncPeerCount = 0;
        gSyncPeerIdx   = 0;
        for (uint8_t i = 0; i < (uint8_t)gVault.records.size() && gSyncPeerCount < 50; i++) {
            if (gVault.records[i].type == RecordType::PRESHARE_KEY)
                gSyncPeerVaultIdx[gSyncPeerCount++] = i;
        }
        drawPeerSelScreen();
        gStateEntered = false;
        return;
    }

    TouchPoint tp = display.readTouch();
    if (!tp.pressed) return;

    // Wait for lift to avoid re-triggering
    while (display.readTouch().pressed) delay(10);
    delay(30);

    if (gSyncPeerCount == 0) {
        if (tp.x >= 150 && tp.x < 250 && tp.y >= 240)
            enterState(State::SYNC_MENU);
        return;
    }

    if (tp.x >= 8 && tp.x < 68 && tp.y >= 100 && tp.y < 160) {
        // Left arrow
        if (gSyncPeerIdx > 0) { gSyncPeerIdx--; drawPeerSelScreen(); }

    } else if (tp.x >= 332 && tp.y >= 100 && tp.y < 160) {
        // Right arrow
        if (gSyncPeerIdx < gSyncPeerCount - 1) { gSyncPeerIdx++; drawPeerSelScreen(); }

    } else if (tp.x >= 20 && tp.x < 140 && tp.y >= 240) {
        // CANCEL
        enterState(State::SYNC_MENU);

    } else if (tp.x >= 260 && tp.y >= 240) {
        // SEND — decode key and prepare outbound payload
        uint8_t preshareKey[AES_KEY_LEN];
        const VaultRecord& rec = gVault.records[gSyncPeerVaultIdx[gSyncPeerIdx]];

        if (!syncDecodeKey(rec.password.c_str(), preshareKey)) {
            secureClear(preshareKey, sizeof(preshareKey));
            display.showMessage("Key Error", "Invalid PRESHARE_KEY format");
            delay(2000);
            enterState(State::SYNC_MENU);
            return;
        }

        display.showMessage("Preparing...", "Encrypting records");
        bool ready = prepareSyncOutbound(gVault, preshareKey);
        secureClear(preshareKey, sizeof(preshareKey));

        if (!ready) {
            display.showMessage("Sync Error", "Payload too large or vault empty");
            delay(2000);
            enterState(State::SYNC_MENU);
            return;
        }

        gSyncIsOutbound = true;
        enterState(State::SYNC_OUTBOUND);
    }
}

// ------ SYNC_OUTBOUND ------
// Blocking: iterates all QR frames, waiting for a tap between each.
static void handleSyncOutbound() {
    if (!gStateEntered) return;
    gStateEntered = false;

    uint8_t total = syncOutboundTotal();
    char frameStr[SYNC_FRAME_STR_MAX];
    char label[32];

    for (uint8_t i = 0; i < total; i++) {
        syncOutboundGetFrame(i, frameStr, sizeof(frameStr));
        snprintf(label, sizeof(label), "%u/%u  Tap: next", i + 1, total);

        if (!display.showQR(DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2, 272, frameStr, label)) {
            syncOutboundClear();
            display.showMessage("QR Error", "Frame too large for display");
            delay(2000);
            gVault.unlockedAt = millis();
            enterState(State::UNLOCKED);
            return;
        }

        gVault.unlockedAt = millis();

        // Wait for tap (advance) or session timeout (lock)
        while (true) {
            if (display.readTouch().pressed) break;
            if (millis() - gVault.unlockedAt > SESSION_TIMEOUT_MS) {
                syncOutboundClear();
                gVault.lock();
                display.showMessage("Locked", "Session timed out");
                delay(1500);
                enterState(State::LOCKED);
                return;
            }
            delay(50);
        }
        while (display.readTouch().pressed) delay(10);
        delay(30);
    }

    gSyncFramesSent = total;
    syncOutboundClear();
    gSyncMerged = 0;
    enterState(State::SYNC_RESULT);
}

// ------ SYNC_INBOUND ------
static void handleSyncInbound() {
    if (gStateEntered) {
        syncReceiver.reset();
        display.showMessage("Receiving...", "Scan peer QR stream with scanner");
        gStateEntered = false;
    }

    if (millis() - gVault.unlockedAt > SESSION_TIMEOUT_MS) {
        gVault.lock();
        display.showMessage("Locked", "Session timed out");
        delay(1500);
        enterState(State::LOCKED);
        return;
    }

    scanner.update();
    if (!scanner.hasFrame()) return;

    ScanFrame f = scanner.takeFrame();
    if (f.type != ScanFrame::Type::SYNC) return;

    SyncFrame sf;
    if (!parseSyncFrame(f.raw, &sf)) return;

    bool done = syncReceiver.addFrame(sf);
    gVault.unlockedAt = millis();  // each frame extends the idle timer

    // Update display on first frame and on completion
    if (sf.frameIdx == 0 || done) {
        char status[40];
        snprintf(status, sizeof(status), "Frame %u/%u received",
                 sf.frameIdx + 1, sf.totalFrames);
        display.showMessage(done ? "Stream complete" : "Receiving...", status);
    }

    if (!done) return;

    // All frames in — try each PRESHARE_KEY record as decryption key
    display.showMessage("Decrypting...", "Please wait");

    gSyncPeerCount = 0;
    for (uint8_t i = 0; i < (uint8_t)gVault.records.size() && gSyncPeerCount < 50; i++) {
        if (gVault.records[i].type == RecordType::PRESHARE_KEY)
            gSyncPeerVaultIdx[gSyncPeerCount++] = i;
    }

    const size_t plainBufLen = (size_t)SYNC_MAX_FRAMES * SYNC_CHUNK_BYTES;
    uint8_t* plain = (uint8_t*)malloc(plainBufLen);
    if (!plain) {
        display.showMessage("Error", "Out of memory");
        delay(2000);
        gVault.unlockedAt = millis();
        enterState(State::UNLOCKED);
        return;
    }

    bool    decrypted = false;
    size_t  plainLen  = 0;

    for (uint8_t i = 0; i < gSyncPeerCount && !decrypted; i++) {
        uint8_t preshareKey[AES_KEY_LEN];
        const VaultRecord& rec = gVault.records[gSyncPeerVaultIdx[i]];
        if (!syncDecodeKey(rec.password.c_str(), preshareKey)) continue;
        decrypted = syncReceiver.decrypt(preshareKey, plain, plainBufLen, &plainLen);
        secureClear(preshareKey, sizeof(preshareKey));
    }

    if (!decrypted) {
        secureClear(plain, plainBufLen);
        free(plain);
        display.showMessage("Auth Failed", "No matching peer key");
        delay(2500);
        gVault.unlockedAt = millis();
        enterState(State::UNLOCKED);
        return;
    }

    gSyncMerged = mergeSyncRecords(gVault, plain, plainLen);
    secureClear(plain, plainBufLen);
    free(plain);

    if (gSyncMerged > 0) saveVault(gVault);

    gSyncIsOutbound = false;
    enterState(State::SYNC_RESULT);
}

// ------ SYNC_RESULT ------
static void handleSyncResult() {
    if (!gStateEntered) return;
    gStateEntered = false;

    char msg[48];
    if (gSyncIsOutbound) {
        snprintf(msg, sizeof(msg), "%u frame(s) sent", gSyncFramesSent);
        display.showMessage("Sync Sent", msg);
    } else {
        snprintf(msg, sizeof(msg), "%u record(s) updated", gSyncMerged);
        display.showMessage(gSyncMerged > 0 ? "Sync Done" : "Already up to date", msg);
    }

    delay(2500);
    gVault.unlockedAt = millis();
    enterState(State::UNLOCKED);
}

// ============================================================
// Backup handlers
// ============================================================

// ------ BACKUP_MENU ------
static void handleBackupMenu() {
    if (gStateEntered) {
        display.fillScreen(false);
        display.drawTextCentered(38, "Backup Vault");
        display.drawRect(20,  80, 170, 120);  display.drawText(50,  148, "EXPORT");
        display.drawRect(210, 80, 170, 120);  display.drawText(240, 148, "IMPORT");
        display.drawRect(150, 240, 100, 46);  display.drawText(168, 268, "BACK");
        display.refresh();
        gStateEntered = false;
    }

    TouchPoint tp = display.readTouch();
    if (!tp.pressed) return;

    if (tp.x >= 20 && tp.x < 190 && tp.y >= 80 && tp.y < 200) {
        enterState(State::BACKUP_EXPORT);
    } else if (tp.x >= 210 && tp.x < 380 && tp.y >= 80 && tp.y < 200) {
        enterState(State::BACKUP_IMPORT);
    } else if (tp.x >= 150 && tp.x < 250 && tp.y >= 240) {
        gVault.unlockedAt = millis();
        enterState(State::UNLOCKED);
    }
}

// ------ BACKUP_EXPORT ------
static void handleBackupExport() {
    if (!gStateEntered) return;
    gStateEntered = false;

    display.showMessage("Exporting...", "Writing encrypted backup to DMZ");

    bool ok = backupExport(gVault);

    if (ok) {
        display.showMessage("Backup Saved", "Encrypted backup written to DMZ");
    } else {
        display.showMessage("Export Failed", "Mount error or vault empty");
    }
    delay(2500);
    gVault.unlockedAt = millis();
    enterState(State::UNLOCKED);
}

// ------ BACKUP_IMPORT ------
static void handleBackupImport() {
    if (!gStateEntered) return;
    gStateEntered = false;

    display.showMessage("Importing...", "Reading backup from DMZ");

    int result = backupImport(gVault);

    if (result >= 0) {
        char msg[40];
        snprintf(msg, sizeof(msg), "%d record(s) updated", result);
        display.showMessage(result > 0 ? "Import Done" : "Already up to date", msg);
    } else if (result == -1) {
        display.showMessage("Import Failed", "No backup file found on DMZ");
    } else if (result == -2) {
        display.showMessage("Import Failed", "Invalid backup format");
    } else if (result == -3) {
        display.showMessage("Import Failed", "Backup auth failed");
    } else {
        display.showMessage("Import Failed", "No matching BACKUP_KEY in vault");
    }
    delay(2500);
    gVault.unlockedAt = millis();
    enterState(State::UNLOCKED);
}

// ============================================================
// Search handlers
// ============================================================

static const uint16_t kSrchRowH  = 46;
static const uint16_t kSrchRowY0 = 44;
static const uint8_t  kSrchRows  = 4;
static const uint16_t kSrchNavY  = 248;

static void drawSearchResultsScreen() {
    display.fillScreen(false);

    char hdr[36];
    snprintf(hdr, sizeof(hdr), "Results: %u match(es)", gSearchResultCount);
    display.drawTextCentered(22, hdr);

    uint8_t startIdx = gSearchPage * kSrchRows;
    for (uint8_t row = 0; row < kSrchRows; row++) {
        uint8_t ri = startIdx + row;
        if (ri >= gSearchResultCount) break;

        const VaultRecord& r = gVault.records[gSearchResults[ri]];
        uint16_t ry = kSrchRowY0 + row * kSrchRowH;

        display.drawRect(8, ry, 384, kSrchRowH - 2);
        display.drawText(14, ry + 30, recordTypeName(r.type));

        char dom[22];
        snprintf(dom, sizeof(dom), "%.20s", r.domain.c_str());
        display.drawText(62, ry + 30, dom);

        char usr[16];
        snprintf(usr, sizeof(usr), "%.14s", r.username.c_str());
        display.drawText(268, ry + 30, usr);
    }

    uint8_t totalPages = gSearchResultCount > 0
        ? (gSearchResultCount + kSrchRows - 1) / kSrchRows : 1;

    if (gSearchPage > 0) {
        display.drawRect(8, kSrchNavY, 88, 42);
        display.drawText(18, kSrchNavY + 28, "< PREV");
    }
    display.drawRect(152, kSrchNavY, 96, 42);
    display.drawText(170, kSrchNavY + 28, "BACK");
    if (gSearchPage < totalPages - 1) {
        display.drawRect(304, kSrchNavY, 88, 42);
        display.drawText(314, kSrchNavY + 28, "NEXT >");
    }

    display.refresh();
}

// ------ SEARCH_INPUT ------
static void handleSearchInput() {
    if (!gStateEntered) return;
    gStateEntered = false;

    secureClear(gSearchQuery, sizeof(gSearchQuery));
    bool ok = display.softKeyboard(gSearchQuery, sizeof(gSearchQuery), "Search vault");

    if (ok && gSearchQuery[0] != '\0') {
        doSearch(gSearchQuery);
        enterState(State::SEARCH_RESULTS);
    } else {
        secureClear(gSearchQuery, sizeof(gSearchQuery));
        gVault.unlockedAt = millis();
        enterState(State::UNLOCKED);
    }
}

// ------ SEARCH_RESULTS ------
static void handleSearchResults() {
    if (gStateEntered) {
        if (gSearchResultCount == 0) {
            display.showMessage("No results", "No matching records");
            delay(2000);
            gVault.unlockedAt = millis();
            enterState(State::SEARCH_INPUT);
            return;
        }
        drawSearchResultsScreen();
        gStateEntered = false;
    }

    if (millis() - gVault.unlockedAt > SESSION_TIMEOUT_MS) {
        gVault.lock();
        display.showMessage("Locked", "Session timed out");
        delay(1500);
        enterState(State::LOCKED);
        return;
    }

    TouchPoint tp = display.readTouch();
    if (!tp.pressed) return;

    while (display.readTouch().pressed) delay(10);
    delay(30);

    gVault.unlockedAt = millis();

    // Navigation row
    if (tp.y >= kSrchNavY) {
        uint8_t totalPages = (gSearchResultCount + kSrchRows - 1) / kSrchRows;
        if (totalPages == 0) totalPages = 1;

        if (tp.x >= 8 && tp.x < 96 && gSearchPage > 0) {
            gSearchPage--;
            drawSearchResultsScreen();
        } else if (tp.x >= 152 && tp.x < 248) {
            secureClear(gSearchQuery, sizeof(gSearchQuery));
            enterState(State::UNLOCKED);
        } else if (tp.x >= 304 && gSearchPage < totalPages - 1) {
            gSearchPage++;
            drawSearchResultsScreen();
        }
        return;
    }

    // Result row tap
    if (tp.y >= kSrchRowY0 && tp.y < (uint16_t)(kSrchRowY0 + kSrchRows * kSrchRowH)) {
        uint8_t row = (tp.y - kSrchRowY0) / kSrchRowH;
        uint8_t ri  = gSearchPage * kSrchRows + row;
        if (ri < gSearchResultCount) {
            gMatchedRecord = gVault.records[gSearchResults[ri]];
            gHasMatch      = true;

            char info[48];
            snprintf(info, sizeof(info), "%.46s", gMatchedRecord.domain.c_str());
            display.showMessage(info, gMatchedRecord.username.c_str());
            delay(1200);
            enterState(State::OUTPUT_SEL);
        }
    }
}

// ============================================================
// Arduino entry points
// ============================================================

void setup() {
    Serial.begin(115200);
    hidBegin();   // registers HID descriptor with TinyUSB
    USB.begin();  // starts USB stack — must be after hidBegin()
    delay(1500);

    lockDownRadios();

    if (!LittleFS.begin(true)) {
        // Without storage the device is inoperable — halt
        while (true) delay(1000);
    }

    display.begin();
    scanner.begin();

    enterState(State::LOCKED);
}

void loop() {
    switch (gState) {
        case State::LOCKED:     handleLocked();     break;
        case State::MFA_SCAN:   handleMfaScan();    break;
        case State::MFA_PHRASE: handleMfaPhrase();  break;
        case State::DERIVING:   handleDeriving();   break;
        case State::UNLOCKED:   handleUnlocked();   break;
        case State::QUERYING:   handleQuerying();   break;
        case State::OUTPUT_SEL: handleOutputSel();  break;
        case State::OUTPUT_HID: handleOutputHid();  break;
        case State::OUTPUT_QR:     handleOutputQr();     break;
        case State::LOCKED_OUT:    handleLockedOut();    break;
        case State::SYNC_MENU:     handleSyncMenu();     break;
        case State::SYNC_PEER_SEL: handleSyncPeerSel();  break;
        case State::SYNC_OUTBOUND: handleSyncOutbound(); break;
        case State::SYNC_INBOUND:  handleSyncInbound();  break;
        case State::SYNC_RESULT:    handleSyncResult();    break;
        case State::SEARCH_INPUT:   handleSearchInput();   break;
        case State::SEARCH_RESULTS: handleSearchResults(); break;
        case State::BACKUP_MENU:    handleBackupMenu();    break;
        case State::BACKUP_EXPORT:  handleBackupExport();  break;
        case State::BACKUP_IMPORT:  handleBackupImport();  break;
    }
    delay(20);
}
