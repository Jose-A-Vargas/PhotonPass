#include "backup.h"
#include "crypto.h"
#include <FFat.h>
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>

static bool sDmzMounted = false;

// ============================================================
// DMZ mount helpers
// ============================================================

bool dmzMount() {
    if (sDmzMounted) return true;
    sDmzMounted = FFat.begin(true, "/dmz", 10, "dmz");
    return sDmzMounted;
}

void dmzUnmount() {
    if (!sDmzMounted) return;
    FFat.end();
    sDmzMounted = false;
}

// ============================================================
// Backup file header (AAD boundary at offset 36)
// ============================================================

#pragma pack(push, 1)
struct BackupHeader {
    uint8_t  magic[4];
    uint8_t  version;
    uint8_t  flags;
    uint16_t recordCount;
    uint8_t  keyUuid[16];
    uint8_t  nonce[GCM_NONCE_LEN];
    // --- AAD boundary ---
    uint8_t  tag[GCM_TAG_LEN];
    uint32_t payloadLen;
};
#pragma pack(pop)

static_assert(offsetof(BackupHeader, tag) == 36, "BackupHeader AAD boundary moved");
static_assert(sizeof(BackupHeader)         == 56, "BackupHeader size changed");

// ============================================================
// Internal helpers
// ============================================================

static bool hexDecodeKey(const char* hex, uint8_t* outKey) {
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < AES_KEY_LEN; i++) {
        char hi = hex[2*i], lo = hex[2*i+1];
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int h = nib(hi), l = nib(lo);
        if (h < 0 || l < 0) return false;
        outKey[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

// Always generates a fresh key tied to a specific backup filename.
// username = bare filename (e.g. "FC980001.BKP") so import can match exactly.
static const VaultRecord* createBackupKey(VaultState& vault, const char* filename) {
    VaultRecord rec;
    generateUUID(rec.uuid);
    rec.type        = RecordType::BACKUP_KEY;
    rec.domain      = "PhotonPass Backup";
    rec.username    = filename;   // exact filename this key unlocks
    rec.lastChanged = 0;

    uint8_t rawKey[AES_KEY_LEN];
    for (int i = 0; i < AES_KEY_LEN; i += 4) {
        uint32_t rnd = esp_random();
        memcpy(rawKey + i, &rnd, 4);
    }
    char hexStr[65];
    for (int i = 0; i < AES_KEY_LEN; i++)
        snprintf(hexStr + 2*i, 3, "%02X", rawKey[i]);
    hexStr[64] = '\0';
    secureClear(rawKey, sizeof(rawKey));

    rec.password = std::string(hexStr, 64);
    secureClear(hexStr, sizeof(hexStr));

    vault.records.push_back(std::move(rec));
    if (!saveVault(vault)) { vault.records.pop_back(); return nullptr; }
    return &vault.records.back();
}

// ============================================================
// Chip identifier
// ============================================================

void backupChipId(char out[5]) {
    uint64_t mac = ESP.getEfuseMac();
    // MAC is stored LE in the uint64_t: byte0=bits[0..7] ... byte5=bits[40..47]
    uint8_t b4 = (uint8_t)((mac >> 32) & 0xFF);
    uint8_t b5 = (uint8_t)((mac >> 40) & 0xFF);
    snprintf(out, 5, "%02X%02X", b4, b5);
}

// ============================================================
// Export
// ============================================================

bool backupExport(VaultState& vault, char* outName, size_t outNameLen) {
    if (!vault.unlocked) return false;

    // Pick the filename first so it can be stored in the key record.
    if (!dmzMount()) return false;
    char chipId[5]; backupChipId(chipId);
    char path[BACKUP_FILENAME_MAX + 2];
    for (int n = 1; n <= 9999; n++) {
        snprintf(path, sizeof(path), "/%s%04d.BKP", chipId, n);
        if (!FFat.exists(path)) break;
    }
    dmzUnmount();  // unmount before saveVault (which writes main LittleFS)

    // Always generate a fresh key for this file — never reuse.
    const char* bareName = path + 1;  // strip leading '/'
    const VaultRecord* keyRec = createBackupKey(vault, bareName);
    if (!keyRec) return false;

    uint8_t backupKey[AES_KEY_LEN];
    if (!hexDecodeKey(keyRec->password.c_str(), backupKey)) return false;

    auto plain = serializeRecords(vault.records);

    BackupHeader hdr;
    hdr.magic[0] = BACKUP_MAGIC_0; hdr.magic[1] = BACKUP_MAGIC_1;
    hdr.magic[2] = BACKUP_MAGIC_2; hdr.magic[3] = BACKUP_MAGIC_3;
    hdr.version     = BACKUP_VERSION;
    hdr.flags       = 0;
    hdr.recordCount = (uint16_t)vault.records.size();
    memcpy(hdr.keyUuid, keyRec->uuid, 16);
    generateNonce(hdr.nonce);
    hdr.payloadLen = (uint32_t)plain.size();

    const size_t aadLen = offsetof(BackupHeader, tag);
    std::vector<uint8_t> cipher(plain.size());

    static const uint8_t kEmptyIn[1]  = {0};
    static       uint8_t kEmptyOut[1] = {0};
    const uint8_t* ptPtr = plain.empty() ? kEmptyIn  : plain.data();
    uint8_t*       ctPtr = cipher.empty() ? kEmptyOut : cipher.data();

    CryptoResult res = encryptGCM(
        backupKey, hdr.nonce,
        (const uint8_t*)&hdr, aadLen,
        ptPtr, plain.size(), ctPtr, hdr.tag);
    secureClear(backupKey, sizeof(backupKey));
    secureClear(plain.data(), plain.size());
    if (res != CryptoResult::OK) return false;

    if (!dmzMount()) return false;
    File f = FFat.open(path, FILE_WRITE);
    if (!f) { dmzUnmount(); return false; }
    f.write((const uint8_t*)&hdr, sizeof(hdr));
    if (!cipher.empty()) f.write(cipher.data(), cipher.size());
    f.close();
    dmzUnmount();

    if (outName && outNameLen > 0)
        strncpy(outName, bareName, outNameLen - 1);

    return true;
}

// ============================================================
// List
// ============================================================

int backupListFiles(void (*cb)(int idx, const char* name)) {
    if (!dmzMount()) return 0;

    File root = FFat.open("/");
    if (!root || !root.isDirectory()) { dmzUnmount(); return 0; }

    int count = 0;
    for (;;) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            const char* raw = f.name();
            // name() may return with or without leading '/'; strip it
            const char* name = (raw[0] == '/') ? raw + 1 : raw;
            size_t len = strlen(name);
            if (len > 4 && strcasecmp(name + len - 4, ".bkp") == 0) {
                if (cb) cb(count, name);
                count++;
            }
        }
        f.close();
    }
    root.close();
    dmzUnmount();
    return count;
}

// ============================================================
// Import
// ============================================================

int backupImport(VaultState& vault, const char* filename,
                 int* outFound, const char* hexKey) {
    if (outFound) *outFound = 0;
    if (!vault.unlocked) return -1;
    if (!dmzMount()) return -1;

    char path[BACKUP_FILENAME_MAX + 2];
    if (filename[0] != '/')
        snprintf(path, sizeof(path), "/%s", filename);
    else
        strncpy(path, filename, sizeof(path) - 1);

    File f = FFat.open(path, FILE_READ);
    if (!f) { dmzUnmount(); return -1; }

    BackupHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) {
        f.close(); dmzUnmount(); return -1;
    }

    if (hdr.magic[0] != BACKUP_MAGIC_0 || hdr.magic[1] != BACKUP_MAGIC_1 ||
        hdr.magic[2] != BACKUP_MAGIC_2 || hdr.magic[3] != BACKUP_MAGIC_3 ||
        hdr.version  != BACKUP_VERSION) {
        f.close(); dmzUnmount(); return -2;
    }

    if (hdr.payloadLen > 0x40000) { f.close(); dmzUnmount(); return -2; }

    std::vector<uint8_t> cipher(hdr.payloadLen);
    if (hdr.payloadLen > 0 &&
        f.read(cipher.data(), hdr.payloadLen) != (int)hdr.payloadLen) {
        f.close(); dmzUnmount(); return -1;
    }
    f.close();
    dmzUnmount();

    uint8_t backupKey[AES_KEY_LEN];
    if (hexKey) {
        if (!hexDecodeKey(hexKey, backupKey)) return -4;
    } else {
        // Match BACKUP_KEY whose username is exactly this filename.
        // Each export creates a unique key stored under the filename it protects.
        const char* bareName = (path[0] == '/') ? path + 1 : path;
        const VaultRecord* keyRec = nullptr;
        for (const auto& r : vault.records)
            if (r.type == RecordType::BACKUP_KEY &&
                strcasecmp(r.username.c_str(), bareName) == 0)
                { keyRec = &r; break; }
        if (!keyRec) return -4;  // prompts user for hex key in cmdRestore
        if (!hexDecodeKey(keyRec->password.c_str(), backupKey)) return -4;
    }

    const size_t aadLen = offsetof(BackupHeader, tag);
    std::vector<uint8_t> plain(hdr.payloadLen ? hdr.payloadLen : 1);

    static const uint8_t kEmptyIn[1]  = {0};
    static       uint8_t kEmptyOut[1] = {0};
    const uint8_t* ctPtr = cipher.empty() ? kEmptyIn  : cipher.data();
    uint8_t*       ptPtr = hdr.payloadLen  ? plain.data() : kEmptyOut;

    CryptoResult res = decryptGCM(
        backupKey, hdr.nonce,
        (const uint8_t*)&hdr, aadLen,
        ctPtr, hdr.payloadLen,
        hdr.tag, ptPtr);
    secureClear(backupKey, sizeof(backupKey));

    if (res != CryptoResult::OK) {
        secureClear(plain.data(), plain.size());
        return -3;
    }

    std::vector<VaultRecord> incoming;
    bool ok = hdr.payloadLen
              ? deserializeRecords(plain.data(), plain.size(), incoming)
              : true;
    secureClear(plain.data(), plain.size());
    if (!ok) return -2;

    if (outFound) *outFound = (int)incoming.size();

    int merged = 0;
    for (auto& inc : incoming) {
        bool found = false;
        for (auto& existing : vault.records) {
            if (memcmp(existing.uuid, inc.uuid, 16) == 0) {
                found = true;
                if (inc.lastChanged > existing.lastChanged) {
                    secureClear(existing.password.data(), existing.password.size());
                    existing = inc;
                    merged++;
                }
                secureClear(inc.password.data(), inc.password.size());
                break;
            }
        }
        if (!found && vault.records.size() < VAULT_MAX_RECORDS) {
            vault.records.push_back(inc);
            merged++;
            secureClear(inc.password.data(), inc.password.size());
        } else if (!found) {
            secureClear(inc.password.data(), inc.password.size());
        }
    }

    if (merged > 0) saveVault(vault);
    return merged;
}

// ============================================================
// Read / delete
// ============================================================

size_t backupReadFile(const char* filename, uint8_t* buf, size_t maxLen) {
    char path[BACKUP_FILENAME_MAX + 2];
    if (filename[0] != '/') snprintf(path, sizeof(path), "/%s", filename);
    else strncpy(path, filename, sizeof(path) - 1);

    if (!dmzMount()) return 0;
    File f = FFat.open(path, FILE_READ);
    if (!f) { dmzUnmount(); return 0; }
    size_t n = f.read(buf, maxLen);
    f.close();
    dmzUnmount();
    return n;
}

bool backupDeleteFile(const char* filename) {
    char path[BACKUP_FILENAME_MAX + 2];
    if (filename[0] != '/') snprintf(path, sizeof(path), "/%s", filename);
    else strncpy(path, filename, sizeof(path) - 1);

    if (!dmzMount()) return false;
    bool ok = FFat.remove(path);
    dmzUnmount();
    return ok;
}
