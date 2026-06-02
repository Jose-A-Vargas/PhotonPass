#include "backup.h"
#include "crypto.h"
#include <LittleFS.h>
#include <esp_random.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>

// The DMZ LittleFS instance is independent of the vault LittleFS global.
// ESP32 Arduino allows multiple LITTLEFSFS objects, each mounting a
// separate flash partition by label.
static fs::LittleFSFS sDmzFS;
static bool       sDmzMounted = false;

// ============================================================
// DMZ mount helpers
// ============================================================

bool dmzMount() {
    if (sDmzMounted) return true;
    sDmzMounted = sDmzFS.begin(true, "/dmz", 10, "dmz");
    return sDmzMounted;
}

void dmzUnmount() {
    if (!sDmzMounted) return;
    sDmzFS.end();
    sDmzMounted = false;
}

// ============================================================
// Backup file header
// AAD = bytes [0, offsetof(BackupHeader, tag))  = first 36 bytes.
// ============================================================

#pragma pack(push, 1)
struct BackupHeader {
    uint8_t  magic[4];              // "PPBK"
    uint8_t  version;               // BACKUP_VERSION
    uint8_t  flags;                 // reserved, must be 0
    uint16_t recordCount;
    uint8_t  keyUuid[16];           // UUID of the BACKUP_KEY record used
    uint8_t  nonce[GCM_NONCE_LEN];  // fresh random on every export
    // --- AAD boundary at offset 36 ---
    uint8_t  tag[GCM_TAG_LEN];
    uint32_t payloadLen;
};
#pragma pack(pop)

static_assert(offsetof(BackupHeader, tag) == 36, "BackupHeader AAD boundary moved");
static_assert(sizeof(BackupHeader)         == 56, "BackupHeader size changed");

// ============================================================
// Hex key helpers (same convention as sync PRESHARE_KEY)
// ============================================================

static bool hexDecodeKey(const char* hex, uint8_t* outKey) {
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < AES_KEY_LEN; i++) {
        char hi = hex[2 * i], lo = hex[2 * i + 1];
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

// ============================================================
// Auto-generate a BACKUP_KEY record if none exists
// ============================================================

// Adds a new BACKUP_KEY record to vault.records, saves the vault,
// and returns a pointer to the new record.  Returns nullptr on save error.
static const VaultRecord* createBackupKey(VaultState& vault) {
    VaultRecord rec;
    generateUUID(rec.uuid);
    rec.type        = RecordType::BACKUP_KEY;
    rec.domain      = "PhotonPass Backup";
    rec.username    = "";
    rec.queryValue  = "";
    rec.lastChanged = 0;

    // 32 TRNG bytes → 64-char uppercase hex key
    uint8_t rawKey[AES_KEY_LEN];
    for (int i = 0; i < AES_KEY_LEN; i += 4) {
        uint32_t rnd = esp_random();
        memcpy(rawKey + i, &rnd, 4);
    }
    char hexStr[65];
    for (int i = 0; i < AES_KEY_LEN; i++)
        snprintf(hexStr + 2 * i, 3, "%02X", rawKey[i]);
    hexStr[64] = '\0';
    secureClear(rawKey, sizeof(rawKey));

    rec.password = std::string(hexStr, 64);
    secureClear(hexStr, sizeof(hexStr));

    vault.records.push_back(std::move(rec));

    if (!saveVault(vault)) {
        vault.records.pop_back();  // rollback
        return nullptr;
    }
    return &vault.records.back();
}

// ============================================================
// Export
// ============================================================

bool backupExport(VaultState& vault) {
    if (!vault.unlocked) return false;

    // Find or auto-create the BACKUP_KEY
    const VaultRecord* keyRec = nullptr;
    for (const auto& r : vault.records) {
        if (r.type == RecordType::BACKUP_KEY) { keyRec = &r; break; }
    }
    if (!keyRec) {
        keyRec = createBackupKey(vault);
        if (!keyRec) return false;
    }

    uint8_t backupKey[AES_KEY_LEN];
    if (!hexDecodeKey(keyRec->password.c_str(), backupKey)) return false;

    auto plain = serializeRecords(vault.records);

    BackupHeader hdr;
    hdr.magic[0]    = BACKUP_MAGIC_0;
    hdr.magic[1]    = BACKUP_MAGIC_1;
    hdr.magic[2]    = BACKUP_MAGIC_2;
    hdr.magic[3]    = BACKUP_MAGIC_3;
    hdr.version     = BACKUP_VERSION;
    hdr.flags       = 0;
    hdr.recordCount = (uint16_t)vault.records.size();
    memcpy(hdr.keyUuid, keyRec->uuid, 16);
    generateNonce(hdr.nonce);
    hdr.payloadLen  = (uint32_t)plain.size();

    const size_t aadLen = offsetof(BackupHeader, tag);

    std::vector<uint8_t> cipher(plain.size());
    CryptoResult res = encryptGCM(
        backupKey, hdr.nonce,
        (const uint8_t*)&hdr, aadLen,
        plain.data(), plain.size(),
        cipher.data(), hdr.tag
    );
    secureClear(backupKey, sizeof(backupKey));
    secureClear(plain.data(), plain.size());

    if (res != CryptoResult::OK) return false;

    if (!dmzMount()) return false;

    File f = sDmzFS.open(BACKUP_FILE_PATH, FILE_WRITE);
    if (!f) { dmzUnmount(); return false; }

    f.write((const uint8_t*)&hdr, sizeof(hdr));
    f.write(cipher.data(), cipher.size());
    f.close();

    dmzUnmount();
    return true;
}

// ============================================================
// Import
// ============================================================

int backupImport(VaultState& vault) {
    if (!vault.unlocked) return -1;

    if (!dmzMount()) return -1;

    File f = sDmzFS.open(BACKUP_FILE_PATH, FILE_READ);
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

    // Sanity-check payload size: 256 KB is far more than any realistic vault
    if (hdr.payloadLen == 0 || hdr.payloadLen > 0x40000) {
        f.close(); dmzUnmount(); return -2;
    }

    std::vector<uint8_t> cipher(hdr.payloadLen);
    if (f.read(cipher.data(), hdr.payloadLen) != (int)hdr.payloadLen) {
        f.close(); dmzUnmount(); return -1;
    }
    f.close();
    dmzUnmount();

    // Find matching BACKUP_KEY by UUID stored in the header
    const VaultRecord* keyRec = nullptr;
    for (const auto& r : vault.records) {
        if (r.type == RecordType::BACKUP_KEY &&
            memcmp(r.uuid, hdr.keyUuid, 16) == 0) {
            keyRec = &r;
            break;
        }
    }
    if (!keyRec) return -4;

    uint8_t backupKey[AES_KEY_LEN];
    if (!hexDecodeKey(keyRec->password.c_str(), backupKey)) return -4;

    const size_t aadLen = offsetof(BackupHeader, tag);
    std::vector<uint8_t> plain(hdr.payloadLen);

    CryptoResult res = decryptGCM(
        backupKey, hdr.nonce,
        (const uint8_t*)&hdr, aadLen,
        cipher.data(), cipher.size(),
        hdr.tag,
        plain.data()
    );
    secureClear(backupKey, sizeof(backupKey));

    if (res != CryptoResult::OK) {
        secureClear(plain.data(), plain.size());
        return -3;
    }

    std::vector<VaultRecord> incoming;
    bool ok = deserializeRecords(plain.data(), plain.size(), incoming);
    secureClear(plain.data(), plain.size());
    if (!ok) return -2;

    // Merge: same UUID → keep record with newer lastChanged
    int merged = 0;
    for (size_t k = 0; k < incoming.size(); k++) {
        VaultRecord& inc = incoming[k];
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
    incoming.clear();

    if (merged > 0) saveVault(vault);

    return merged;
}
