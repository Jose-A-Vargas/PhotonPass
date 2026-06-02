// PhotonPass full test suite — serial output, 115200 baud
// Build:   pio run -e test
// Flash:   pio run -e test --target upload
// Monitor: pio device monitor -e test
//
// Commands:
//   run    run all sections (serial only)
//   1-6    run a single section
//   h      run all, capture output, type it via HID keyboard, then return to serial

#include <Arduino.h>
#include <WiFi.h>
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
// Unified output — always prints to Serial; in HID mode also
// accumulates into gHidBuf for type-out after the run.
// ============================================================

#define HID_LOG_BUF_SIZE (32 * 1024)

static bool   gHidMode   = false;
static char   gHidBuf[HID_LOG_BUF_SIZE];
static size_t gHidBufLen = 0;

static void tprintf(const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    Serial.print(tmp);

    if (gHidMode) {
        size_t len = strlen(tmp);
        if (gHidBufLen + len < HID_LOG_BUF_SIZE - 1) {
            memcpy(gHidBuf + gHidBufLen, tmp, len);
            gHidBufLen += len;
        }
    }
}

// ============================================================
// Harness
// ============================================================

static uint16_t gPassed = 0;
static uint16_t gFailed = 0;

static void ok(const char* name) {
    tprintf("  [PASS] %s\n", name);
    gPassed++;
}

static void fail(const char* name, const char* reason = "") {
    if (reason[0]) tprintf("  [FAIL] %s  <<< %s\n", name, reason);
    else           tprintf("  [FAIL] %s\n", name);
    gFailed++;
}

static void section(const char* title) {
    tprintf("\n=== %s ===\n", title);
}

static void log(const char* msg) {
    tprintf("  ... %s\n", msg);
}

static void logHex(const char* label, const uint8_t* buf, size_t len, size_t cap = 16) {
    char line[160];
    int pos = snprintf(line, sizeof(line), "  %s: ", label);
    size_t show = len < cap ? len : cap;
    for (size_t i = 0; i < show && pos < (int)sizeof(line) - 4; i++)
        pos += snprintf(line + pos, sizeof(line) - pos, "%02X", buf[i]);
    if (len > cap)
        snprintf(line + pos, sizeof(line) - pos, "...(+%u)", (unsigned)(len - cap));
    tprintf("%s\n", line);
}

// ============================================================
// 1. Crypto
// ============================================================

static void testCrypto() {
    section("1. Crypto");

    const char    phrase[]  = "testpassphrase";
    const uint8_t token[]   = { 0xDE, 0xAD, 0xBE, 0xEF };
    uint8_t       salt[PBKDF2_SALT_LEN];
    memset(salt, 0x5A, sizeof(salt));

    log("deriving key from phrase + token + salt");
    uint8_t key[AES_KEY_LEN] = {};
    CryptoResult r = deriveKey(phrase, strlen(phrase), token, sizeof(token), salt, key);
    if (r == CryptoResult::OK) ok("deriveKey");
    else                       fail("deriveKey", "non-OK result");
    logHex("key", key, AES_KEY_LEN);

    const char   plainStr[] = "Hello, PhotonPass!";
    const size_t plen       = sizeof(plainStr) - 1;
    const char   aadStr[]   = "test-aad";
    const size_t aadLen     = sizeof(aadStr) - 1;

    uint8_t nonce[GCM_NONCE_LEN];
    generateNonce(nonce);
    logHex("nonce", nonce, GCM_NONCE_LEN);

    log("encrypting plaintext");
    uint8_t cipher[sizeof(plainStr)];
    uint8_t tag[GCM_TAG_LEN];
    r = encryptGCM(key, nonce, (const uint8_t*)aadStr, aadLen,
                   (const uint8_t*)plainStr, plen, cipher, tag);
    if (r == CryptoResult::OK) ok("encryptGCM");
    else                       fail("encryptGCM");
    logHex("tag",    tag,    GCM_TAG_LEN);
    logHex("cipher", cipher, plen);

    log("decrypting and comparing to original");
    uint8_t recovered[sizeof(plainStr)] = {};
    r = decryptGCM(key, nonce, (const uint8_t*)aadStr, aadLen,
                   cipher, plen, tag, recovered);
    if (r == CryptoResult::OK && memcmp(recovered, plainStr, plen) == 0)
        ok("decryptGCM round-trip");
    else
        fail("decryptGCM round-trip");
    tprintf("  recovered: \"%.*s\"\n", (int)plen, (char*)recovered);

    log("trying decryption with all-0xFF key (expect ERR_AUTH)");
    uint8_t badKey[AES_KEY_LEN];
    memset(badKey, 0xFF, sizeof(badKey));
    r = decryptGCM(badKey, nonce, (const uint8_t*)aadStr, aadLen,
                   cipher, plen, tag, recovered);
    if (r == CryptoResult::ERR_AUTH) ok("decryptGCM rejects wrong key");
    else { char m[32]; snprintf(m,sizeof(m),"got result=%u",(uint8_t)r); fail("decryptGCM rejects wrong key",m); }

    log("flipping cipher[0] bit (expect ERR_AUTH)");
    uint8_t tampered[sizeof(plainStr)];
    memcpy(tampered, cipher, plen);
    tampered[0] ^= 0x01;
    r = decryptGCM(key, nonce, (const uint8_t*)aadStr, aadLen,
                   tampered, plen, tag, recovered);
    if (r == CryptoResult::ERR_AUTH) ok("decryptGCM rejects tampered ciphertext");
    else { char m[32]; snprintf(m,sizeof(m),"got result=%u",(uint8_t)r); fail("decryptGCM rejects tampered ciphertext",m); }

    log("wiping 64-byte buffer with secureClear");
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
    section("2. Password Generator");

    char buf[64];

    log("generating 20-char FULL_ASCII password");
    bool gen = generatePassword(buf, 20, CharProfile::FULL_ASCII);
    bool printable = true;
    for (int i = 0; i < 20; i++) if (buf[i] < 0x20 || buf[i] > 0x7E) { printable = false; break; }
    if (gen && strlen(buf) == 20 && printable) ok("FULL_ASCII length + charset");
    else                                       fail("FULL_ASCII length + charset");
    tprintf("  result: \"%s\"\n", buf);

    log("generating 24-char STANDARD password");
    gen = generatePassword(buf, 24, CharProfile::STANDARD);
    if (gen && strlen(buf) == 24) ok("STANDARD length");
    else                          fail("STANDARD length");
    tprintf("  result: \"%s\"\n", buf);

    log("generating 16-char HEX password");
    gen = generatePassword(buf, 16, CharProfile::HEXADECIMAL);
    bool allHex = true;
    for (int i = 0; i < 16; i++) {
        char c = buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) { allHex = false; break; }
    }
    if (gen && strlen(buf) == 16 && allHex) ok("HEX length + charset");
    else                                    fail("HEX length + charset");
    tprintf("  result: \"%s\"\n", buf);

    log("generating 32-char BASE64 password");
    gen = generatePassword(buf, 32, CharProfile::BASE64);
    bool allB64 = true;
    const char* b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 32; i++) if (!strchr(b64chars, buf[i])) { allB64 = false; break; }
    if (gen && strlen(buf) == 32 && allB64) ok("BASE64 length + charset");
    else                                    fail("BASE64 length + charset");
    tprintf("  result: \"%s\"\n", buf);

    char pbuf[PASSPHRASE_MAX_BUF];
    for (uint8_t wc = 1; wc <= 4; wc++) {
        tprintf("  ... generating %u-word passphrase\n", wc);
        gen = generatePassphrase(pbuf, sizeof(pbuf), wc, '-');
        char label[48];
        snprintf(label, sizeof(label), "passphrase %u word(s)", wc);
        if (gen && strlen(pbuf) >= wc) ok(label);
        else                           fail(label);
        tprintf("  result: \"%s\"\n", pbuf);
    }
}

// ============================================================
// 3. Vault
// ============================================================

static void testVault() {
    section("3. Vault");

    VaultState vs;
    generateSalt(vs.salt);
    logHex("salt", vs.salt, PBKDF2_SALT_LEN);

    const char    phrase[] = "vault-test-phrase";
    const uint8_t token[]  = { 0x01, 0x02, 0x03, 0x04 };
    log("deriving master key");
    CryptoResult cr = deriveKey(phrase, strlen(phrase), token, sizeof(token), vs.salt, vs.masterKey);
    if (cr != CryptoResult::OK) { fail("vault setup: deriveKey"); return; }
    logHex("masterKey", vs.masterKey, AES_KEY_LEN);
    vs.unlocked = true; vs.unlockedAt = millis();

    VaultRecord r1;
    generateUUID(r1.uuid);
    r1.type = RecordType::WEB_SERVICE; r1.domain = "example.com";
    r1.username = "alice"; r1.password = "p@ssw0rd!";
    r1.queryValue = "example"; r1.lastChanged = 1716000000UL;
    logHex("r1.uuid", r1.uuid, 16);
    tprintf("  r1: domain=%s username=%s password=%s\n",
            r1.domain.c_str(), r1.username.c_str(), r1.password.c_str());

    VaultRecord r2;
    generateUUID(r2.uuid);
    r2.type = RecordType::API_TOKEN; r2.domain = "api.service.io";
    r2.username = ""; r2.password = "tok_abc123XYZ";
    r2.queryValue = "service"; r2.lastChanged = 1716100000UL;
    tprintf("  r2: domain=%s username=(empty) password=%s\n",
            r2.domain.c_str(), r2.password.c_str());

    vs.records.push_back(r1);
    vs.records.push_back(r2);
    tprintf("  ... serializing %u records and writing /test_vault.bin\n", (unsigned)vs.records.size());

    bool saved = saveVault(vs, "/test_vault.bin");
    if (saved) ok("saveVault");
    else     { fail("saveVault"); return; }

    log("loading from /test_vault.bin with correct key");
    VaultState vs2;
    memcpy(vs2.masterKey, vs.masterKey, AES_KEY_LEN);
    memcpy(vs2.salt,      vs.salt,      PBKDF2_SALT_LEN);
    vs2.unlocked = true;

    bool loaded = loadVault(vs2, "/test_vault.bin");
    if (!loaded) { fail("loadVault"); return; }
    ok("loadVault");

    tprintf("  loaded %u record(s)\n", (unsigned)vs2.records.size());
    if (vs2.records.size() == 2) ok("record count after load");
    else { char m[24]; snprintf(m,sizeof(m),"got %u",(unsigned)vs2.records.size()); fail("record count after load",m); }

    if (!vs2.records.empty()) {
        auto& rec = vs2.records[0];
        tprintf("  rec[0]: domain=%s username=%s password=%s type=%u\n",
                rec.domain.c_str(), rec.username.c_str(), rec.password.c_str(), (uint8_t)rec.type);
        bool match = rec.domain   == "example.com"
                  && rec.username == "alice"
                  && rec.password == "p@ssw0rd!"
                  && rec.type     == RecordType::WEB_SERVICE;
        if (match) ok("record fields round-trip");
        else       fail("record fields round-trip");
    }

    log("rotating password on rec[0]: p@ssw0rd! -> newpass!");
    vs2.records[0].rotatePassword("newpass!", 1716050000UL);
    tprintf("  current=%s  history size=%u  history[0]=%s\n",
            vs2.records[0].password.c_str(),
            (unsigned)vs2.records[0].history.size(),
            vs2.records[0].history.empty() ? "(none)" : vs2.records[0].history[0].password.c_str());
    if (vs2.records[0].password == "newpass!"
        && vs2.records[0].history.size() == 1
        && vs2.records[0].history[0].password == "p@ssw0rd!")
        ok("rotatePassword pushes history");
    else
        fail("rotatePassword pushes history");

    log("calling lock() -- expect key zeroed, records cleared");
    vs2.lock();
    bool keyZero = true;
    for (int i = 0; i < AES_KEY_LEN; i++) if (vs2.masterKey[i]) { keyZero = false; break; }
    tprintf("  unlocked=%d  records=%u  keyAllZero=%d\n",
            (int)vs2.unlocked, (unsigned)vs2.records.size(), (int)keyZero);
    if (!vs2.unlocked && keyZero && vs2.records.empty())
        ok("lock() clears key and records");
    else
        fail("lock() clears key and records");

    log("attempting loadVault with all-0xFF key (expect failure)");
    VaultState vs3;
    memset(vs3.masterKey, 0xFF, AES_KEY_LEN);
    memcpy(vs3.salt, vs.salt, PBKDF2_SALT_LEN);
    vs3.unlocked = true;
    bool badLoad = loadVault(vs3, "/test_vault.bin");
    tprintf("  loadVault returned %d (want 0/false)\n", (int)badLoad);
    if (!badLoad) ok("loadVault rejects wrong key");
    else          fail("loadVault rejects wrong key");

    LittleFS.remove("/test_vault.bin");
    log("cleaned up /test_vault.bin");
}

// ============================================================
// 4. Sync
// ============================================================

static uint8_t sSyncPlain[(size_t)SYNC_MAX_FRAMES * SYNC_CHUNK_BYTES];

static void testSync() {
    section("4. Sync (QR Frame Protocol)");

    uint8_t rawKey[AES_KEY_LEN];
    for (int i = 0; i < AES_KEY_LEN; i++) rawKey[i] = (uint8_t)i;
    logHex("preshare key", rawKey, AES_KEY_LEN);

    VaultState vs;
    generateSalt(vs.salt);
    memcpy(vs.masterKey, rawKey, AES_KEY_LEN);
    vs.unlocked = true;

    VaultRecord r;
    generateUUID(r.uuid);
    r.type = RecordType::WEB_SERVICE; r.domain = "sync-test.example";
    r.username = "bob"; r.password = "SyncPass99!";
    r.queryValue = "synctest"; r.lastChanged = 1716200000UL;
    vs.records.push_back(r);

    uint8_t savedUuid[16];
    memcpy(savedUuid, r.uuid, 16);
    logHex("record uuid", savedUuid, 16);

    log("calling prepareSyncOutbound");
    if (!prepareSyncOutbound(vs, rawKey)) { fail("prepareSyncOutbound"); return; }
    ok("prepareSyncOutbound");

    uint8_t total = syncOutboundTotal();
    tprintf("  total frames: %u  (SYNC_MAX_FRAMES=%u)\n", total, SYNC_MAX_FRAMES);
    if (total >= 1) ok("syncOutboundTotal >= 1");
    else          { fail("syncOutboundTotal >= 1"); return; }

    log("encoding and parsing all frames into SyncReceiver");
    syncReceiver.reset();
    bool allParsed = true;
    char frameBuf[SYNC_FRAME_STR_MAX];
    for (uint8_t i = 0; i < total; i++) {
        syncOutboundGetFrame(i, frameBuf, sizeof(frameBuf));
        tprintf("  frame[%u]: %.60s%s\n", i, frameBuf, strlen(frameBuf) > 60 ? "..." : "");
        SyncFrame sf;
        if (!parseSyncFrame(frameBuf, &sf)) {
            tprintf("  parseSyncFrame failed on frame %u\n", i);
            allParsed = false; break;
        }
        syncReceiver.addFrame(sf);
    }
    if (allParsed) ok("parseSyncFrame all frames");
    else           fail("parseSyncFrame all frames");

    tprintf("  receiver complete: %d\n", (int)syncReceiver.isComplete());
    if (!syncReceiver.isComplete()) { fail("SyncReceiver::isComplete"); syncOutboundClear(); return; }
    ok("SyncReceiver::isComplete");

    log("decrypting received payload");
    size_t plainLen = 0;
    bool dec = syncReceiver.decrypt(rawKey, sSyncPlain, sizeof(sSyncPlain), &plainLen);
    tprintf("  decrypted bytes: %u\n", (unsigned)plainLen);
    if (dec && plainLen > 0) ok("SyncReceiver::decrypt");
    else                   { fail("SyncReceiver::decrypt"); syncOutboundClear(); return; }

    log("merging into empty vault");
    VaultState dst;
    uint8_t merged = mergeSyncRecords(dst, sSyncPlain, plainLen);
    tprintf("  merged=%u  dst.records=%u\n", merged, (unsigned)dst.records.size());
    if (merged == 1 && dst.records.size() == 1) ok("mergeSyncRecords count");
    else                                         fail("mergeSyncRecords count");

    if (!dst.records.empty()) {
        auto& rec = dst.records[0];
        tprintf("  merged[0]: domain=%s password=%s\n", rec.domain.c_str(), rec.password.c_str());
        bool ok2 = rec.domain   == "sync-test.example"
                && rec.password == "SyncPass99!"
                && memcmp(rec.uuid, savedUuid, 16) == 0;
        if (ok2) ok("mergeSyncRecords field integrity");
        else     fail("mergeSyncRecords field integrity");
    }

    log("merge policy: incoming newer than local (t=1716200000 > t=1716000000)");
    VaultState withOlder;
    VaultRecord older;
    memcpy(older.uuid, savedUuid, 16);
    older.type = RecordType::WEB_SERVICE; older.domain = "old-domain.example";
    older.password = "OldPass"; older.lastChanged = 1716000000UL;
    withOlder.records.push_back(older);
    mergeSyncRecords(withOlder, sSyncPlain, plainLen);
    bool newerWon = false;
    for (auto& rec : withOlder.records)
        if (memcmp(rec.uuid, savedUuid, 16) == 0 && rec.domain == "sync-test.example")
            { newerWon = true; break; }
    tprintf("  newerWon=%d  domain after merge: %s\n",
            (int)newerWon, withOlder.records.empty() ? "?" : withOlder.records[0].domain.c_str());
    if (newerWon) ok("merge: newer lastChanged wins");
    else          fail("merge: newer lastChanged wins");

    log("merge policy: local newer than incoming (t=1716999999 > t=1716200000)");
    VaultState withNewer;
    VaultRecord newer;
    memcpy(newer.uuid, savedUuid, 16);
    newer.type = RecordType::WEB_SERVICE; newer.domain = "local-updated.example";
    newer.password = "NewestPass"; newer.lastChanged = 1716999999UL;
    withNewer.records.push_back(newer);
    mergeSyncRecords(withNewer, sSyncPlain, plainLen);
    bool localKept = false;
    for (auto& rec : withNewer.records)
        if (memcmp(rec.uuid, savedUuid, 16) == 0 && rec.domain == "local-updated.example")
            { localKept = true; break; }
    tprintf("  localKept=%d  domain after merge: %s\n",
            (int)localKept, withNewer.records.empty() ? "?" : withNewer.records[0].domain.c_str());
    if (localKept) ok("merge: local newer record preserved");
    else           fail("merge: local newer record preserved");

    log("decoding 64-char hex key string");
    const char* hexKey = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F";
    uint8_t decoded[AES_KEY_LEN];
    bool keyOk = syncDecodeKey(hexKey, decoded);
    logHex("decoded", decoded, AES_KEY_LEN);
    if (keyOk && memcmp(decoded, rawKey, AES_KEY_LEN) == 0) ok("syncDecodeKey");
    else                                                     fail("syncDecodeKey");

    log("passing invalid hex to syncDecodeKey (expect false)");
    bool keyBad = syncDecodeKey("ZZZZ", decoded);
    tprintf("  syncDecodeKey(\"ZZZZ\") returned %d (want 0/false)\n", (int)keyBad);
    if (!keyBad) ok("syncDecodeKey rejects invalid hex");
    else         fail("syncDecodeKey rejects invalid hex");

    syncOutboundClear();
    secureClear(sSyncPlain, sizeof(sSyncPlain));
}

// ============================================================
// 5. Backup
// ============================================================

static void testBackup() {
    section("5. Backup (DMZ Partition)");

    VaultState vs;
    generateSalt(vs.salt);
    logHex("salt", vs.salt, PBKDF2_SALT_LEN);

    const char    phrase[] = "backup-test-phrase";
    const uint8_t token[]  = { 0xBB, 0xCC, 0xDD, 0xEE };
    log("deriving key for backup vault");
    CryptoResult cr = deriveKey(phrase, strlen(phrase), token, sizeof(token), vs.salt, vs.masterKey);
    if (cr != CryptoResult::OK) { fail("backup setup: deriveKey"); return; }
    vs.unlocked = true; vs.unlockedAt = millis();

    VaultRecord r;
    generateUUID(r.uuid);
    r.type = RecordType::WEB_SERVICE; r.domain = "backup-test.example";
    r.username = "testuser"; r.password = "BackupPass123!";
    r.queryValue = "backuptest"; r.lastChanged = 1716300000UL;
    vs.records.push_back(r);
    tprintf("  source record: domain=%s password=%s\n", r.domain.c_str(), r.password.c_str());

    uint8_t savedUuid[16];
    memcpy(savedUuid, r.uuid, 16);
    logHex("record uuid", savedUuid, 16);

    log("calling backupExport -- auto-generates BACKUP_KEY, writes /backup.bin");
    bool exported = backupExport(vs);
    tprintf("  backupExport returned %d  records after export: %u\n",
            (int)exported, (unsigned)vs.records.size());
    if (exported) ok("backupExport");
    else        { fail("backupExport"); return; }

    bool hasKey = false;
    for (auto& rec : vs.records)
        if (rec.type == RecordType::BACKUP_KEY) { hasKey = true; break; }
    if (hasKey) ok("backupExport auto-generates BACKUP_KEY");
    else        fail("backupExport auto-generates BACKUP_KEY");

    log("removing source record to simulate need for restore");
    vs.records.erase(vs.records.begin());
    tprintf("  records remaining before import: %u\n", (unsigned)vs.records.size());

    log("calling backupImport");
    int result = backupImport(vs);
    tprintf("  backupImport returned %d  records after import: %u\n",
            result, (unsigned)vs.records.size());
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

    log("testing backupImport on vault with no BACKUP_KEY (expect -4)");
    VaultState empty;
    generateSalt(empty.salt);
    empty.unlocked = true;  // must be unlocked or it returns -1 (locked guard) before checking key
    int r2 = backupImport(empty);
    tprintf("  backupImport(no key) returned %d (want -4)\n", r2);
    if (r2 == -4) ok("backupImport returns -4 (no BACKUP_KEY)");
    else {
        char msg[32]; snprintf(msg, sizeof(msg), "got %d, want -4", r2);
        fail("backupImport returns -4 (no BACKUP_KEY)", msg);
    }
}

// ============================================================
// 6. Scan button
// ============================================================

static void testScanButton() {
    section("6. Scan Button (GPIO D10)");

    scanBtnBegin();
    tprintf("  Press the scan button within 5 seconds...\n");
    tprintf("  (leave unwired to skip)\n");

    uint32_t deadline = millis() + 5000;
    uint8_t  presses  = 0;
    bool     prev     = false;

    while (millis() < deadline) {
        bool p = scanBtnPressed();
        if (p && !prev) {
            presses++;
            tprintf("  press detected! total=%u  millis=%lu\n", presses, (unsigned long)millis());
        }
        prev = p;
        delay(10);
    }

    if (presses > 0) ok("scan button press detected");
    else             tprintf("  [SKIP] no press (hardware not yet connected)\n");
}

// ============================================================
// HID type-out — drains gHidBuf line-by-line via keyboard
// ============================================================

static void typeHidBuffer() {
    if (gHidBufLen == 0) return;

    uint32_t estSec = (uint32_t)(gHidBufLen * 8) / 1000;
    Serial.printf("\n  Buffer: %u chars (~%u sec to type at 8 ms/key)\n",
                  (unsigned)gHidBufLen, estSec);
    Serial.println("  Open a text editor on your PC and give it focus.");

    for (int cd = 5; cd > 0; cd--) {
        Serial.printf("  Starting in %d...\r", cd);
        delay(1000);
    }
    Serial.println("\n  Typing...");

    char     lineBuf[512];
    size_t   linePos  = 0;
    uint32_t linesDone = 0;

    for (size_t i = 0; i <= gHidBufLen; i++) {
        char c = (i < gHidBufLen) ? gHidBuf[i] : '\n';  // force final flush
        if (c == '\n') {
            lineBuf[linePos] = '\0';
            // typePassword rejects empty strings — send a space for blank lines
            HidResult hr = typePassword(linePos > 0 ? lineBuf : " ", true);
            if (hr == HidResult::NOT_READY) {
                Serial.println("\n  HID: host not ready -- aborting type-out.");
                break;
            }
            linePos = 0;
            linesDone++;
            if (linesDone % 20 == 0)
                Serial.printf("  ... %u lines typed\r", linesDone);
        } else if (c != '\r') {
            if (linePos < sizeof(lineBuf) - 1)
                lineBuf[linePos++] = c;
        }
    }

    gHidBufLen = 0;
    Serial.printf("\n  Done -- %u lines sent via HID keyboard.\n", linesDone);
}

// ============================================================
// Runners
// ============================================================

static void printSummary() {
    tprintf("\n=====================\n");
    tprintf("Results: %u passed, %u failed\n", gPassed, gFailed);
    if (gFailed == 0) tprintf("All tests passed!\n");
    else              tprintf("%u test(s) need attention.\n", gFailed);
}

static void printPrompt() {
    Serial.println("\nType a number (1-6), 'run', or 'h' (HID run).");
    Serial.print("> ");
}

static void runAll() {
    gPassed = 0; gFailed = 0;
    testCrypto();
    testPassgen();
    testVault();
    testSync();
    testBackup();
    testScanButton();
    printSummary();
    printPrompt();
}

static void runSection(int n) {
    gPassed = 0; gFailed = 0;
    switch (n) {
        case 1: testCrypto();      break;
        case 2: testPassgen();     break;
        case 3: testVault();       break;
        case 4: testSync();        break;
        case 5: testBackup();      break;
        case 6: testScanButton();  break;
        default: Serial.println("  unknown section"); break;
    }
    tprintf("\nSection results: %u passed, %u failed\n", gPassed, gFailed);
    Serial.print("> ");
}

static void runAllHid() {
    gHidBufLen = 0;
    gHidMode   = true;
    gPassed    = 0;
    gFailed    = 0;

    // All output goes to Serial (live) AND accumulates in gHidBuf
    testCrypto();
    testPassgen();
    testVault();
    testSync();
    testBackup();
    testScanButton();
    printSummary();

    gHidMode = false;  // stop buffering; type-out uses Serial only for status

    typeHidBuffer();   // replay full log via HID keyboard into a text editor

    Serial.println("\nHID off -- back to serial mode.");
    printPrompt();
}

// ============================================================
// Serial line reader
// ============================================================

static char    sLine[64];
static uint8_t sLineLen = 0;

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

static void processLine(const char* line) {
    while (*line == ' ') line++;
    if (line[0] == '\0') { Serial.print("> "); return; }

    char* endptr;
    long n = strtol(line, &endptr, 10);
    if (*endptr == '\0' && n >= 1 && n <= 6) { runSection((int)n); return; }

    if (strcasecmp(line, "run") == 0)                              { runAll();    return; }
    if (strcasecmp(line, "h") == 0 || strcasecmp(line, "hidrun") == 0) { runAllHid(); return; }

    Serial.printf("  unknown: '%s'\n", line);
    Serial.println("  Commands: run  1  2  3  4  5  6  h");
    Serial.print("> ");
}

// ============================================================
// Entry points
// ============================================================

void setup() {
    WiFi.mode(WIFI_OFF);
    btStop();

    hidBegin();   // registers HID with TinyUSB; triggers USB re-enumeration as CDC+HID
    delay(2000);  // wait for Windows to reconnect the composite device

    Serial.begin(115200);
    // Wait up to 5 s for host to open the CDC port, then proceed anyway.
    uint32_t _t = millis();
    while (!Serial && (millis() - _t) < 5000) delay(10);
    delay(500);

    Serial.println("\n\nPhotonPass Test Suite");
    Serial.println("=====================");
    Serial.printf("Build: %s %s\n", __DATE__, __TIME__);

    if (!LittleFS.begin(true)) {
        Serial.println("FATAL: LittleFS.begin() failed");
        while (true) delay(1000);
    }
    Serial.println("LittleFS: OK");

    Serial.println();
    Serial.println("  1      Crypto");
    Serial.println("  2      Password Generator");
    Serial.println("  3      Vault");
    Serial.println("  4      Sync");
    Serial.println("  5      Backup");
    Serial.println("  6      Scan Button");
    Serial.println("  run    run all (serial output)");
    Serial.println("  h      run all, then type full log via HID keyboard");
    Serial.println();
    Serial.print("> ");
}

void loop() {
    if (pollLine())
        processLine(sLine);
}
