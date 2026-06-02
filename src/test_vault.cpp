// Vault isolated test — serial output, 115200 baud
// Build:   pio run -e vault_test
// Flash:   pio run -e vault_test --target upload
// Monitor: pio device monitor -e vault_test

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "crypto.h"
#include "vault.h"

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
// 1. Serialization — no filesystem, pure in-memory
// ============================================================

static void testSerialization() {
    section("Serialization (in-memory)");

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

    // Add a history entry to r1
    r1.history.push_back({ "oldpass1", 1715000000UL });
    r1.history.push_back({ "oldpass2", 1715500000UL });

    std::vector<VaultRecord> src = { r1, r2 };
    auto blob = serializeRecords(src);

    if (!blob.empty()) ok("serializeRecords produces output");
    else             { fail("serializeRecords produces output"); return; }

    Serial.printf("  Serialized bytes: %u\n", (unsigned)blob.size());

    std::vector<VaultRecord> dst;
    bool ok2 = deserializeRecords(blob.data(), blob.size(), dst);
    if (ok2) ok("deserializeRecords returns true");
    else   { fail("deserializeRecords returns true"); return; }

    if (dst.size() == 2) ok("record count preserved");
    else { char m[24]; snprintf(m, sizeof(m), "got %u", (unsigned)dst.size()); fail("record count preserved", m); return; }

    // r1 field checks
    bool fields = dst[0].domain      == "example.com"
               && dst[0].username    == "alice"
               && dst[0].password    == "p@ssw0rd!"
               && dst[0].queryValue  == "example"
               && dst[0].lastChanged == 1716000000UL
               && dst[0].type        == RecordType::WEB_SERVICE
               && memcmp(dst[0].uuid, r1.uuid, 16) == 0;
    if (fields) ok("r1 fields round-trip");
    else        fail("r1 fields round-trip");

    // r2 field checks (empty username)
    bool fields2 = dst[1].domain   == "api.service.io"
                && dst[1].username == ""
                && dst[1].password == "tok_abc123XYZ"
                && dst[1].type     == RecordType::API_TOKEN;
    if (fields2) ok("r2 fields round-trip (empty username)");
    else         fail("r2 fields round-trip (empty username)");

    // History round-trip
    bool hist = dst[0].history.size() == 2
             && dst[0].history[0].password  == "oldpass1"
             && dst[0].history[0].changedAt == 1715000000UL
             && dst[0].history[1].password  == "oldpass2"
             && dst[0].history[1].changedAt == 1715500000UL;
    if (hist) ok("history round-trip");
    else      fail("history round-trip");

    // Empty record list
    std::vector<VaultRecord> empty;
    auto emptyBlob = serializeRecords(empty);
    std::vector<VaultRecord> emptyOut;
    bool emptyOk = deserializeRecords(emptyBlob.data(), emptyBlob.size(), emptyOut);
    if (emptyOk && emptyOut.empty()) ok("empty record list round-trip");
    else                             fail("empty record list round-trip");

    // Truncated blob → must return false
    std::vector<VaultRecord> junk;
    bool bad = deserializeRecords(blob.data(), 4, junk);
    if (!bad) ok("deserializeRecords rejects truncated data");
    else      fail("deserializeRecords rejects truncated data");
}

// ============================================================
// 2. rotatePassword — history semantics
// ============================================================

static void testRotatePassword() {
    section("rotatePassword");

    VaultRecord r;
    r.type     = RecordType::WEB_SERVICE;   // maxHistory = VAULT_HISTORY_WEB
    r.password = "first";
    r.lastChanged = 1000;

    // First rotation: "first" → history, "second" is current
    r.rotatePassword("second", 2000);
    if (r.password == "second" && r.lastChanged == 2000) ok("password updated after rotate");
    else fail("password updated after rotate");

    if (r.history.size() == 1 && r.history[0].password == "first" && r.history[0].changedAt == 1000)
        ok("previous password pushed to history[0]");
    else fail("previous password pushed to history[0]");

    // Second rotation: "second" → history[0], "first" demoted to history[1]
    r.rotatePassword("third", 3000);
    if (r.password == "third"
        && r.history.size() == 2
        && r.history[0].password == "second"
        && r.history[1].password == "first")
        ok("history grows in reverse-chronological order");
    else fail("history grows in reverse-chronological order");

    // Rotate empty password — should not push empty entry
    VaultRecord r2;
    r2.type     = RecordType::WEB_SERVICE;
    r2.password = "";
    r2.lastChanged = 100;
    r2.rotatePassword("first!", 200);
    if (r2.password == "first!" && r2.history.empty())
        ok("rotate from empty password does not add empty history entry");
    else fail("rotate from empty password does not add empty history entry");

    // Cap test — fill to maxHistory then add one more
    VaultRecord rc;
    rc.type     = RecordType::WEB_SERVICE;
    rc.password = "pw0";
    rc.lastChanged = 0;
    uint8_t cap = rc.maxHistory();
    Serial.printf("  WEB_SERVICE maxHistory = %u\n", cap);

    for (uint8_t i = 1; i <= cap + 1; i++) {
        char pw[16]; snprintf(pw, sizeof(pw), "pw%u", i);
        rc.rotatePassword(pw, i * 1000UL);
    }

    if ((uint8_t)rc.history.size() == cap) ok("history capped at maxHistory");
    else { char m[32]; snprintf(m, sizeof(m), "size=%u want %u", (unsigned)rc.history.size(), cap); fail("history capped at maxHistory", m); }
}

// ============================================================
// 3. LittleFS save / load round-trip
// ============================================================

static void testSaveLoad() {
    section("Save / Load (LittleFS)");

    VaultState vs;
    generateSalt(vs.salt);

    const char    phrase[] = "vault-test-phrase";
    const uint8_t token[]  = { 0x01, 0x02, 0x03, 0x04 };
    CryptoResult cr = deriveKey(phrase, strlen(phrase), token, sizeof(token), vs.salt, vs.masterKey);
    if (cr != CryptoResult::OK) { fail("deriveKey for save/load"); return; }
    vs.unlocked   = true;
    vs.unlockedAt = millis();

    VaultRecord r;
    generateUUID(r.uuid);
    r.type        = RecordType::WEB_SERVICE;
    r.domain      = "saveload.example";
    r.username    = "bob";
    r.password    = "S@veL0ad!";
    r.queryValue  = "sl";
    r.lastChanged = 1716000000UL;
    r.history.push_back({ "OldPass", 1715000000UL });
    vs.records.push_back(r);

    uint8_t savedUuid[16];
    memcpy(savedUuid, r.uuid, 16);

    // saveVault on locked state must fail
    VaultState locked;
    locked.unlocked = false;
    if (!saveVault(locked, "/test_vault.bin")) ok("saveVault rejects locked state");
    else fail("saveVault rejects locked state");

    bool saved = saveVault(vs, "/test_vault.bin");
    if (saved) ok("saveVault");
    else     { fail("saveVault"); return; }

    // Load with correct key
    VaultState vs2;
    memcpy(vs2.masterKey, vs.masterKey, AES_KEY_LEN);
    memcpy(vs2.salt,      vs.salt,      PBKDF2_SALT_LEN);
    vs2.unlocked = true;

    bool loaded = loadVault(vs2, "/test_vault.bin");
    if (loaded) ok("loadVault");
    else      { fail("loadVault"); return; }

    if (vs2.unlocked) ok("loadVault sets unlocked=true");
    else              fail("loadVault sets unlocked=true");

    if (vs2.records.size() == 1) ok("record count after load");
    else { char m[24]; snprintf(m, sizeof(m), "got %u", (unsigned)vs2.records.size()); fail("record count after load", m); return; }

    auto& loaded_r = vs2.records[0];
    bool fields = loaded_r.domain      == "saveload.example"
               && loaded_r.username    == "bob"
               && loaded_r.password    == "S@veL0ad!"
               && loaded_r.queryValue  == "sl"
               && loaded_r.lastChanged == 1716000000UL
               && loaded_r.type        == RecordType::WEB_SERVICE
               && memcmp(loaded_r.uuid, savedUuid, 16) == 0;
    if (fields) ok("record fields preserved through save/load");
    else        fail("record fields preserved through save/load");

    bool histOk = loaded_r.history.size() == 1
               && loaded_r.history[0].password  == "OldPass"
               && loaded_r.history[0].changedAt == 1715000000UL;
    if (histOk) ok("history preserved through save/load");
    else        fail("history preserved through save/load");

    // Overwrite and reload — nonce must differ each save but data must survive
    vs2.records[0].rotatePassword("UpdatedPass!", 1716050000UL);
    bool resaved = saveVault(vs2, "/test_vault.bin");
    if (resaved) ok("overwrite save");
    else         fail("overwrite save");

    VaultState vs3;
    memcpy(vs3.masterKey, vs.masterKey, AES_KEY_LEN);
    memcpy(vs3.salt,      vs.salt,      PBKDF2_SALT_LEN);
    vs3.unlocked = true;
    bool reloaded = loadVault(vs3, "/test_vault.bin");
    if (reloaded && !vs3.records.empty() && vs3.records[0].password == "UpdatedPass!")
        ok("reload after overwrite");
    else
        fail("reload after overwrite");

    LittleFS.remove("/test_vault.bin");
}

// ============================================================
// 4. Auth — wrong key, tampered file
// ============================================================

static void testAuth() {
    section("Authentication");

    VaultState vs;
    generateSalt(vs.salt);

    const char    phrase[] = "auth-test-phrase";
    const uint8_t token[]  = { 0xAA, 0xBB };
    deriveKey(phrase, strlen(phrase), token, sizeof(token), vs.salt, vs.masterKey);
    vs.unlocked   = true;
    vs.unlockedAt = millis();

    VaultRecord r;
    generateUUID(r.uuid);
    r.type = RecordType::OTHER;
    r.domain = "auth.test"; r.password = "secret";
    r.lastChanged = 1234567890UL;
    vs.records.push_back(r);

    saveVault(vs, "/test_auth.bin");

    // Wrong key → must fail
    VaultState bad;
    memset(bad.masterKey, 0xFF, AES_KEY_LEN);
    memcpy(bad.salt, vs.salt, PBKDF2_SALT_LEN);
    bad.unlocked = true;
    if (!loadVault(bad, "/test_auth.bin")) ok("wrong key rejected");
    else                                   fail("wrong key rejected");

    // Missing file → must fail
    VaultState missing;
    memcpy(missing.masterKey, vs.masterKey, AES_KEY_LEN);
    missing.unlocked = true;
    if (!loadVault(missing, "/no_such_file.bin")) ok("missing file returns false");
    else                                          fail("missing file returns false");

    // Bit-flip in ciphertext → GCM must reject
    File f = LittleFS.open("/test_auth.bin", FILE_READ);
    size_t fsz = f.size();
    std::vector<uint8_t> raw(fsz);
    f.read(raw.data(), fsz);
    f.close();

    // Flip a byte in the ciphertext region (after the 72-byte header)
    if (fsz > sizeof(VaultHeader) + 4) {
        raw[sizeof(VaultHeader) + 4] ^= 0x01;
        File fw = LittleFS.open("/test_auth_tampered.bin", FILE_WRITE);
        fw.write(raw.data(), fsz);
        fw.close();

        VaultState tampered;
        memcpy(tampered.masterKey, vs.masterKey, AES_KEY_LEN);
        memcpy(tampered.salt, vs.salt, PBKDF2_SALT_LEN);
        tampered.unlocked = true;
        if (!loadVault(tampered, "/test_auth_tampered.bin")) ok("tampered ciphertext rejected (GCM)");
        else                                                 fail("tampered ciphertext rejected (GCM)");

        LittleFS.remove("/test_auth_tampered.bin");
    } else {
        Serial.println("  [SKIP] file too small to tamper");
    }

    LittleFS.remove("/test_auth.bin");
}

// ============================================================
// 5. VaultState::lock()
// ============================================================

static void testLock() {
    section("VaultState::lock()");

    VaultState vs;
    generateSalt(vs.salt);
    memset(vs.masterKey, 0xAB, AES_KEY_LEN);
    vs.unlocked   = true;
    vs.unlockedAt = 12345;

    VaultRecord r;
    r.password = "super-secret";
    r.history.push_back({ "old-secret", 0 });
    vs.records.push_back(r);

    vs.lock();

    if (!vs.unlocked) ok("lock() clears unlocked flag");
    else              fail("lock() clears unlocked flag");

    if (vs.unlockedAt == 0) ok("lock() clears unlockedAt");
    else                    fail("lock() clears unlockedAt");

    bool keyZero = true;
    for (int i = 0; i < AES_KEY_LEN; i++) if (vs.masterKey[i]) { keyZero = false; break; }
    if (keyZero) ok("lock() zeroes masterKey");
    else         fail("lock() zeroes masterKey");

    if (vs.records.empty()) ok("lock() clears records");
    else                    fail("lock() clears records");
}

// ============================================================
// Entry points
// ============================================================

void setup() {
    WiFi.mode(WIFI_OFF);
    btStop();

    Serial.begin(115200);
    delay(2000);

    Serial.println("\n\nPhotonPass Vault Test");
    Serial.println("=====================");
    Serial.printf("Build: %s %s\n", __DATE__, __TIME__);

    if (!LittleFS.begin(true)) {
        Serial.println("FATAL: LittleFS.begin() failed");
        while (true) delay(1000);
    }
    Serial.println("LittleFS: OK");

    testSerialization();
    testRotatePassword();
    testSaveLoad();
    testAuth();
    testLock();

    Serial.println();
    Serial.println("=====================");
    Serial.printf("Results: %u passed, %u failed\n", gPassed, gFailed);
    if (gFailed == 0) Serial.println("All tests passed!");
    else              Serial.printf("%u test(s) need attention.\n", gFailed);
}

void loop() {}
