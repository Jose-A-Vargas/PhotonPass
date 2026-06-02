#include <Arduino.h>
#include <WiFi.h>
#include <USB.h>
#include <LittleFS.h>

#include "config.h"
#include "crypto.h"
#include "vault.h"
#include "passgen.h"
#include "scanner.h"
#include "hid.h"
#include "sync.h"
#include "backup.h"

// ============================================================
// Harness
// ============================================================

static uint16_t gPassed = 0;
static uint16_t gFailed = 0;

static void ok(const char* name) {
    Serial.printf("  [PASS] %s\n", name);
    gPassed++;
}

static void fail(const char* name, const char* reason = "") {
    if (reason[0]) Serial.printf("  [FAIL] %s (%s)\n", name, reason);
    else           Serial.printf("  [FAIL] %s\n", name);
    gFailed++;
}

static void section(const char* title) {
    Serial.printf("\n=== %s ===\n", title);
}

// ============================================================
// 1. Crypto
// ============================================================
static void testCrypto() {
    section("Crypto");

    const char    phrase[]  = "testpassphrase";
    const uint8_t token[]   = { 0xDE, 0xAD, 0xBE, 0xEF };
    uint8_t       salt[PBKDF2_SALT_LEN];
    memset(salt, 0x5A, sizeof(salt));

    uint8_t key[AES_KEY_LEN] = {};
    CryptoResult r = deriveKey(phrase, strlen(phrase), token, sizeof(token), salt, key);
    if (r == CryptoResult::OK) ok("deriveKey");
    else                       fail("deriveKey", "non-OK result");

    Serial.print("  Derived key: ");
    for (int i = 0; i < AES_KEY_LEN; i++) Serial.printf("%02X", key[i]);
    Serial.println();

    // encryptGCM / decryptGCM round-trip
    const char   plainStr[] = "Hello, PhotonPass!";
    const size_t plen       = sizeof(plainStr) - 1;
    const char   aadStr[]   = "test-aad";
    const size_t aadLen     = sizeof(aadStr) - 1;

    uint8_t nonce[GCM_NONCE_LEN];
    generateNonce(nonce);

    uint8_t cipher[sizeof(plainStr)];
    uint8_t tag[GCM_TAG_LEN];
    r = encryptGCM(key, nonce,
                   (const uint8_t*)aadStr, aadLen,
                   (const uint8_t*)plainStr, plen,
                   cipher, tag);
    if (r == CryptoResult::OK) ok("encryptGCM");
    else                       fail("encryptGCM");

    uint8_t recovered[sizeof(plainStr)] = {};
    r = decryptGCM(key, nonce, (const uint8_t*)aadStr, aadLen,
                   cipher, plen, tag, recovered);
    if (r == CryptoResult::OK && memcmp(recovered, plainStr, plen) == 0)
        ok("decryptGCM round-trip");
    else
        fail("decryptGCM round-trip");

    // Wrong key → ERR_AUTH
    uint8_t badKey[AES_KEY_LEN];
    memset(badKey, 0xFF, sizeof(badKey));
    r = decryptGCM(badKey, nonce, (const uint8_t*)aadStr, aadLen,
                   cipher, plen, tag, recovered);
    if (r == CryptoResult::ERR_AUTH) ok("decryptGCM rejects wrong key");
    else                             fail("decryptGCM rejects wrong key", "expected ERR_AUTH");

    // Tampered ciphertext → ERR_AUTH
    uint8_t tampered[sizeof(plainStr)];
    memcpy(tampered, cipher, plen);
    tampered[0] ^= 0x01;
    r = decryptGCM(key, nonce, (const uint8_t*)aadStr, aadLen,
                   tampered, plen, tag, recovered);
    if (r == CryptoResult::ERR_AUTH) ok("decryptGCM rejects tampered ciphertext");
    else                             fail("decryptGCM rejects tampered ciphertext", "expected ERR_AUTH");

    // secureClear
    uint8_t wipe[64];
    memset(wipe, 0xAB, sizeof(wipe));
    secureClear(wipe, sizeof(wipe));
    bool allZero = true;
    for (size_t i = 0; i < sizeof(wipe); i++) if (wipe[i]) { allZero = false; break; }
    if (allZero) ok("secureClear zeroes buffer");
    else         fail("secureClear zeroes buffer");
}

// ============================================================
// 2. Password / passphrase generator
// ============================================================
static void testPassgen() {
    section("Password Generator");

    char buf[64];

    // FULL_ASCII — every char must be 0x20–0x7E
    bool gen = generatePassword(buf, 20, CharProfile::FULL_ASCII);
    bool printable = true;
    for (int i = 0; i < 20; i++) if (buf[i] < 0x20 || buf[i] > 0x7E) { printable = false; break; }
    if (gen && strlen(buf) == 20 && printable) ok("FULL_ASCII length + charset");
    else                                       fail("FULL_ASCII length + charset");
    Serial.printf("  Sample: %s\n", buf);

    // STANDARD
    gen = generatePassword(buf, 24, CharProfile::STANDARD);
    if (gen && strlen(buf) == 24) ok("STANDARD length");
    else                          fail("STANDARD length");
    Serial.printf("  Sample: %s\n", buf);

    // HEX — only 0-9 A-F
    gen = generatePassword(buf, 16, CharProfile::HEXADECIMAL);
    bool allHex = true;
    for (int i = 0; i < 16; i++) {
        char c = buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) { allHex = false; break; }
    }
    if (gen && strlen(buf) == 16 && allHex) ok("HEX length + charset");
    else                                    fail("HEX length + charset");
    Serial.printf("  Sample: %s\n", buf);

    // BASE64 — A-Z a-z 0-9 + /
    gen = generatePassword(buf, 32, CharProfile::BASE64);
    bool allB64 = true;
    const char* b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 32; i++) if (!strchr(b64chars, buf[i])) { allB64 = false; break; }
    if (gen && strlen(buf) == 32 && allB64) ok("BASE64 length + charset");
    else                                    fail("BASE64 length + charset");
    Serial.printf("  Sample: %s\n", buf);

    // Passphrases — 1 through 4 words
    char pbuf[PASSPHRASE_MAX_BUF];
    for (uint8_t wc = 1; wc <= 4; wc++) {
        gen = generatePassphrase(pbuf, sizeof(pbuf), wc, '-');
        char label[48];
        snprintf(label, sizeof(label), "passphrase %u word(s)", wc);
        if (gen && strlen(pbuf) >= wc) ok(label);
        else                           fail(label);
        Serial.printf("  Sample: %s\n", pbuf);
    }
}

// ============================================================
// 3. Vault — serialize / LittleFS round-trip / lock
// ============================================================
static void testVault() {
    section("Vault");

    VaultState vs;
    generateSalt(vs.salt);

    const char    phrase[] = "vault-test-phrase";
    const uint8_t token[]  = { 0x01, 0x02, 0x03, 0x04 };
    CryptoResult cr = deriveKey(phrase, strlen(phrase), token, sizeof(token), vs.salt, vs.masterKey);
    if (cr != CryptoResult::OK) { fail("vault setup: deriveKey"); return; }
    vs.unlocked   = true;
    vs.unlockedAt = millis();

    VaultRecord r1;
    generateUUID(r1.uuid);
    r1.type        = RecordType::WEB_SERVICE;
    r1.domain      = "example.com";
    r1.username    = "alice";
    r1.password    = "p@ssw0rd!";
    r1.queryValue  = "example";
    r1.lastChanged = 1716000000UL;

    VaultRecord r2;
    generateUUID(r2.uuid);
    r2.type        = RecordType::API_TOKEN;
    r2.domain      = "api.service.io";
    r2.username    = "";
    r2.password    = "tok_abc123XYZ";
    r2.queryValue  = "service";
    r2.lastChanged = 1716100000UL;

    vs.records.push_back(r1);
    vs.records.push_back(r2);

    // Save to a scratch path to avoid clobbering any real vault.bin
    bool saved = saveVault(vs, "/test_vault.bin");
    if (saved) ok("saveVault");
    else     { fail("saveVault"); return; }

    // Load into a fresh state
    VaultState vs2;
    memcpy(vs2.masterKey, vs.masterKey, AES_KEY_LEN);
    memcpy(vs2.salt,      vs.salt,      PBKDF2_SALT_LEN);
    vs2.unlocked = true;

    bool loaded = loadVault(vs2, "/test_vault.bin");
    if (!loaded) { fail("loadVault"); return; }
    ok("loadVault");

    if (vs2.records.size() == 2) ok("record count after load");
    else                         fail("record count after load", "expected 2");

    if (!vs2.records.empty()) {
        bool match = vs2.records[0].domain   == "example.com"
                  && vs2.records[0].username == "alice"
                  && vs2.records[0].password == "p@ssw0rd!"
                  && vs2.records[0].type     == RecordType::WEB_SERVICE;
        if (match) ok("record fields round-trip");
        else       fail("record fields round-trip");
    }

    // rotatePassword — history grows, current password updates
    vs2.records[0].rotatePassword("newpass!", 1716050000UL);
    if (vs2.records[0].password == "newpass!"
        && vs2.records[0].history.size() == 1
        && vs2.records[0].history[0].password == "p@ssw0rd!")
        ok("rotatePassword pushes history");
    else
        fail("rotatePassword pushes history");

    // lock() — master key and records wiped
    vs2.lock();
    bool keyZero = true;
    for (int i = 0; i < AES_KEY_LEN; i++) if (vs2.masterKey[i]) { keyZero = false; break; }
    if (!vs2.unlocked && keyZero && vs2.records.empty())
        ok("lock() clears key and records");
    else
        fail("lock() clears key and records");

    // Wrong key → loadVault must fail
    VaultState vs3;
    memset(vs3.masterKey, 0xFF, AES_KEY_LEN);
    memcpy(vs3.salt, vs.salt, PBKDF2_SALT_LEN);
    vs3.unlocked = true;
    bool badLoad = loadVault(vs3, "/test_vault.bin");
    if (!badLoad) ok("loadVault rejects wrong key");
    else          fail("loadVault rejects wrong key");

    LittleFS.remove("/test_vault.bin");
}

// ============================================================
// 4. Sync — QR frame encode / decode / decrypt / merge
// ============================================================

// 20 KB plaintext buffer — static keeps it off the stack
static uint8_t sSyncPlain[(size_t)SYNC_MAX_FRAMES * SYNC_CHUNK_BYTES];

static void testSync() {
    section("Sync (QR Frame Protocol)");

    // Known 32-byte preshare key: bytes 0x00..0x1F
    uint8_t rawKey[AES_KEY_LEN];
    for (int i = 0; i < AES_KEY_LEN; i++) rawKey[i] = (uint8_t)i;

    // Build source vault
    VaultState vs;
    generateSalt(vs.salt);
    memcpy(vs.masterKey, rawKey, AES_KEY_LEN);
    vs.unlocked = true;

    VaultRecord r;
    generateUUID(r.uuid);
    r.type        = RecordType::WEB_SERVICE;
    r.domain      = "sync-test.example";
    r.username    = "bob";
    r.password    = "SyncPass99!";
    r.queryValue  = "synctest";
    r.lastChanged = 1716200000UL;
    vs.records.push_back(r);

    uint8_t savedUuid[16];
    memcpy(savedUuid, r.uuid, 16);

    // prepareSyncOutbound
    if (!prepareSyncOutbound(vs, rawKey)) { fail("prepareSyncOutbound"); return; }
    ok("prepareSyncOutbound");

    uint8_t total = syncOutboundTotal();
    Serial.printf("  Frame count: %u\n", total);
    if (total >= 1) ok("syncOutboundTotal >= 1");
    else          { fail("syncOutboundTotal >= 1"); return; }

    // Build frame strings and feed to SyncReceiver
    syncReceiver.reset();
    bool allParsed = true;
    char frameBuf[SYNC_FRAME_STR_MAX];
    for (uint8_t i = 0; i < total; i++) {
        syncOutboundGetFrame(i, frameBuf, sizeof(frameBuf));
        if (i == 0) Serial.printf("  Frame 0: %s\n", frameBuf);
        SyncFrame sf;
        if (!parseSyncFrame(frameBuf, &sf)) { allParsed = false; break; }
        syncReceiver.addFrame(sf);
    }
    if (allParsed) ok("parseSyncFrame all frames");
    else           fail("parseSyncFrame all frames");

    if (!syncReceiver.isComplete()) { fail("SyncReceiver::isComplete"); syncOutboundClear(); return; }
    ok("SyncReceiver::isComplete");

    // Decrypt
    size_t plainLen = 0;
    bool dec = syncReceiver.decrypt(rawKey, sSyncPlain, sizeof(sSyncPlain), &plainLen);
    if (dec && plainLen > 0) ok("SyncReceiver::decrypt");
    else                   { fail("SyncReceiver::decrypt"); syncOutboundClear(); return; }

    // Merge into empty vault
    VaultState dst;
    uint8_t merged = mergeSyncRecords(dst, sSyncPlain, plainLen);
    if (merged == 1 && dst.records.size() == 1) ok("mergeSyncRecords count");
    else                                         fail("mergeSyncRecords count");

    if (!dst.records.empty()
        && dst.records[0].domain   == "sync-test.example"
        && dst.records[0].password == "SyncPass99!"
        && memcmp(dst.records[0].uuid, savedUuid, 16) == 0)
        ok("mergeSyncRecords field integrity");
    else
        fail("mergeSyncRecords field integrity");

    // Merge policy: payload (t=1716200000) overwrites older local (t=1716000000)
    VaultState withOlder;
    VaultRecord older;
    memcpy(older.uuid, savedUuid, 16);
    older.type        = RecordType::WEB_SERVICE;
    older.domain      = "old-domain.example";
    older.password    = "OldPass";
    older.lastChanged = 1716000000UL;
    withOlder.records.push_back(older);
    mergeSyncRecords(withOlder, sSyncPlain, plainLen);
    bool newerWon = false;
    for (auto& rec : withOlder.records)
        if (memcmp(rec.uuid, savedUuid, 16) == 0 && rec.domain == "sync-test.example")
            { newerWon = true; break; }
    if (newerWon) ok("merge: newer lastChanged wins");
    else          fail("merge: newer lastChanged wins");

    // Merge policy: local newer (t=1716999999) is preserved over payload (t=1716200000)
    VaultState withNewer;
    VaultRecord newer;
    memcpy(newer.uuid, savedUuid, 16);
    newer.type        = RecordType::WEB_SERVICE;
    newer.domain      = "local-updated.example";
    newer.password    = "NewestPass";
    newer.lastChanged = 1716999999UL;
    withNewer.records.push_back(newer);
    mergeSyncRecords(withNewer, sSyncPlain, plainLen);
    bool localKept = false;
    for (auto& rec : withNewer.records)
        if (memcmp(rec.uuid, savedUuid, 16) == 0 && rec.domain == "local-updated.example")
            { localKept = true; break; }
    if (localKept) ok("merge: local newer record preserved");
    else           fail("merge: local newer record preserved");

    // syncDecodeKey: key 0x00..0x1F → 64-char hex
    const char* hexKey = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F";
    uint8_t decoded[AES_KEY_LEN];
    bool keyOk = syncDecodeKey(hexKey, decoded);
    if (keyOk && memcmp(decoded, rawKey, AES_KEY_LEN) == 0) ok("syncDecodeKey");
    else                                                     fail("syncDecodeKey");

    bool keyBad = syncDecodeKey("ZZZZ", decoded);
    if (!keyBad) ok("syncDecodeKey rejects invalid hex");
    else         fail("syncDecodeKey rejects invalid hex");

    syncOutboundClear();
    secureClear(sSyncPlain, sizeof(sSyncPlain));
}

// ============================================================
// 5. Backup — DMZ export / import / error codes
// Note: backupExport() internally calls saveVault() → writes /vault.bin.
// ============================================================
static void testBackup() {
    section("Backup (DMZ Partition)");

    VaultState vs;
    generateSalt(vs.salt);

    const char    phrase[] = "backup-test-phrase";
    const uint8_t token[]  = { 0xBB, 0xCC, 0xDD, 0xEE };
    CryptoResult cr = deriveKey(phrase, strlen(phrase), token, sizeof(token), vs.salt, vs.masterKey);
    if (cr != CryptoResult::OK) { fail("backup setup: deriveKey"); return; }
    vs.unlocked   = true;
    vs.unlockedAt = millis();

    VaultRecord r;
    generateUUID(r.uuid);
    r.type        = RecordType::WEB_SERVICE;
    r.domain      = "backup-test.example";
    r.username    = "testuser";
    r.password    = "BackupPass123!";
    r.queryValue  = "backuptest";
    r.lastChanged = 1716300000UL;
    vs.records.push_back(r);

    uint8_t savedUuid[16];
    memcpy(savedUuid, r.uuid, 16);

    // Export — auto-generates BACKUP_KEY, writes /backup.bin on DMZ
    bool exported = backupExport(vs);
    if (exported) ok("backupExport");
    else        { fail("backupExport"); return; }

    // BACKUP_KEY must have been added to vs
    bool hasKey = false;
    for (auto& rec : vs.records)
        if (rec.type == RecordType::BACKUP_KEY) { hasKey = true; break; }
    if (hasKey) ok("backupExport auto-generates BACKUP_KEY");
    else        fail("backupExport auto-generates BACKUP_KEY");

    // vs.records is now [r, BACKUP_KEY]; remove r to simulate needing a restore
    vs.records.erase(vs.records.begin());

    // Import — should merge r back in
    int result = backupImport(vs);
    if (result >= 0) ok("backupImport success");
    else {
        char msg[24]; snprintf(msg, sizeof(msg), "returned %d", result);
        fail("backupImport success", msg);
        return;
    }

    bool found = false;
    for (auto& rec : vs.records)
        if (memcmp(rec.uuid, savedUuid, 16) == 0
            && rec.domain   == "backup-test.example"
            && rec.password == "BackupPass123!")
            { found = true; break; }
    if (found) ok("backupImport restored record by UUID");
    else       fail("backupImport restored record by UUID");

    // Vault with no BACKUP_KEY → must return -4
    VaultState empty;
    generateSalt(empty.salt);
    int r2 = backupImport(empty);
    if (r2 == -4) ok("backupImport returns -4 (no BACKUP_KEY)");
    else {
        char msg[32]; snprintf(msg, sizeof(msg), "got %d, want -4", r2);
        fail("backupImport returns -4 (no BACKUP_KEY)", msg);
    }
}

// ============================================================
// 6. Physical scan button — interactive, 5 s window
// ============================================================
static void testScanButton() {
    section("Scan Button (GPIO 12)");

    scanBtnBegin();
    Serial.println("  Press the scan button within 5 seconds...");
    Serial.println("  (leave unwired to skip — it will time out cleanly)");

    uint32_t deadline = millis() + 5000;
    uint8_t  presses  = 0;
    bool     prev     = false;

    while (millis() < deadline) {
        bool p = scanBtnPressed();
        if (p && !prev) {
            presses++;
            Serial.printf("  Press detected (%u)\n", presses);
        }
        prev = p;
        delay(10);
    }

    if (presses > 0) ok("scan button press detected");
    else             Serial.println("  [SKIP] no press (hardware not yet connected)");
}

// ============================================================
// 7. USB HID keyboard — interactive, types to host PC
// ============================================================
static void testHID() {
    section("USB HID Keyboard");
    Serial.println("  Open a text editor on your PC.");
    Serial.println("  The device will type a test string in 3 seconds...");
    delay(3000);

    const char testPw[] = "PhotonPass-HID-Test-1234!";
    HidResult hr = typePassword(testPw, true);

    switch (hr) {
        case HidResult::OK:
            ok("typePassword USB HID");
            break;
        case HidResult::NOT_READY:
            Serial.println("  [SKIP] USB host not ready (check cable or hub)");
            break;
        case HidResult::EMPTY_PW:
            fail("typePassword USB HID", "EMPTY_PW on non-empty string");
            break;
    }
}

// ============================================================
// Entry points
// ============================================================
void setup() {
    WiFi.mode(WIFI_OFF);
    btStop();

    hidBegin();
    USB.begin();

    Serial.begin(115200);
    delay(2000);  // let USB CDC enumerate before printing

    Serial.println("\n\nPhotonPass Test Suite");
    Serial.println("=====================");
    Serial.printf("Build: %s %s\n", __DATE__, __TIME__);

    if (!LittleFS.begin(true)) {
        Serial.println("FATAL: LittleFS.begin() failed");
        while (true) delay(1000);
    }
    Serial.println("LittleFS: OK");

    testCrypto();
    testPassgen();
    testVault();
    testSync();
    testBackup();
    testScanButton();
    testHID();

    Serial.println();
    Serial.println("=====================");
    Serial.printf("Results: %u passed, %u failed\n", gPassed, gFailed);
    if (gFailed == 0) Serial.println("All tests passed!");
    else              Serial.printf("%u test(s) need attention.\n", gFailed);
}

void loop() {
    // After all tests complete: type the test string on every D10→GND press.
    static bool prev = false;
    bool p    = scanBtnPressed();
    if (p && !prev) {
        const char testPw[] = "PhotonPass-HID-Test-1234!";
        HidResult hr = typePassword(testPw, true);
        if (hr == HidResult::OK) Serial.println("HID: sent");
        else                     Serial.println("HID: not ready");
    }
    prev = p;
    delay(10);
}
