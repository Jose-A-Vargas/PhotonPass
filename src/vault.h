#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include "config.h"
#include "crypto.h"

// ============================================================
// vault.bin binary header — unencrypted, 72 bytes.
// The first offsetof(VaultHeader, tag) bytes are fed to GCM as
// AAD, so any tampering with magic/version/salt/nonce is caught.
// ============================================================
#pragma pack(push, 1)
struct VaultHeader {
    uint8_t  magic[4];               // VAULT_MAGIC_0..3 = "PPV1"
    uint8_t  version;                // VAULT_VERSION
    uint8_t  flags;                  // reserved, must be 0
    uint16_t recordCount;
    uint8_t  salt[PBKDF2_SALT_LEN];  // random, generated once at vault creation
    uint8_t  nonce[GCM_NONCE_LEN];   // fresh random on every save
    uint8_t  tag[GCM_TAG_LEN];       // GCM auth tag (covers ciphertext + AAD above)
    uint32_t payloadLen;             // byte length of encrypted payload
};
#pragma pack(pop)

static_assert(sizeof(VaultHeader) == 72, "VaultHeader layout changed");

// ============================================================
// Record types
// ============================================================
enum class RecordType : uint8_t {
    WEB_SERVICE  = 0x01,
    PC_ACCOUNT   = 0x02,
    API_TOKEN    = 0x03,
    ENCRYPT_KEY  = 0x04,
    PRESHARE_KEY = 0x05,
    BACKUP_KEY   = 0x06,
    OTHER        = 0xFF,
};

// ============================================================
// Per-record binary layout (within encrypted payload):
//
//  16  uuid
//   1  type
//   2  domain_len  + domain bytes
//   2  username_len + username bytes
//   2  password_len + password bytes
//   2  queryValue_len + queryValue bytes
//   4  lastChanged (unix timestamp, big-endian)
//   1  historyCount
//   [historyCount times]:
//     2  pw_len + pw bytes
//     4  changedAt (unix timestamp, big-endian)
// ============================================================

struct HistoryEntry {
    std::string password;
    uint32_t    changedAt;
};

struct VaultRecord {
    uint8_t              uuid[16];
    RecordType           type;
    std::string          domain;
    std::string          username;
    std::string          password;
    std::string          queryValue;
    uint32_t             lastChanged;
    std::vector<HistoryEntry> history;

    uint8_t maxHistory() const {
        return (type == RecordType::PC_ACCOUNT) ? VAULT_HISTORY_PC : VAULT_HISTORY_WEB;
    }

    // Pushes current password into history before updating to newPassword.
    void rotatePassword(const std::string& newPassword, uint32_t now) {
        if (!password.empty()) {
            history.insert(history.begin(), { password, lastChanged });
            if ((uint8_t)history.size() > maxHistory())
                history.resize(maxHistory());
        }
        password    = newPassword;
        lastChanged = now;
    }
};

// ============================================================
// In-RAM vault state — master key + decrypted records live here.
// Nothing from this struct ever touches non-volatile storage.
// ============================================================
struct VaultState {
    uint8_t  masterKey[AES_KEY_LEN];  // zeroed by lock()
    uint8_t  salt[PBKDF2_SALT_LEN];   // copied from header on load
    std::vector<VaultRecord> records;
    bool     unlocked    = false;
    uint32_t unlockedAt  = 0;         // millis() timestamp

    // Wipe all sensitive data and mark locked.
    void lock() {
        secureClear(masterKey, AES_KEY_LEN);
        for (auto& r : records) {
            secureClear(r.password.data(), r.password.size());
            for (auto& h : r.history)
                secureClear(h.password.data(), h.password.size());
        }
        records.clear();
        unlocked   = false;
        unlockedAt = 0;
    }
};

// ============================================================
// Serialization
// ============================================================
std::vector<uint8_t> serializeRecords(const std::vector<VaultRecord>& records);
bool deserializeRecords(const uint8_t* buf, size_t len, std::vector<VaultRecord>& out);

// ============================================================
// LittleFS persistence
// ============================================================
// Encrypts state.records with state.masterKey and writes vault.bin.
bool saveVault(const VaultState& state, const char* path = VAULT_FILE_PATH);

// Reads vault.bin and decrypts into state.records using state.masterKey.
// Caller must have placed the derived key in state.masterKey before calling.
bool loadVault(VaultState& state, const char* path = VAULT_FILE_PATH);

// ============================================================
// TRNG helpers (esp_random)
// ============================================================
void generateUUID(uint8_t* uuid);          // RFC 4122 v4
void generateSalt(uint8_t* salt);          // PBKDF2_SALT_LEN bytes
void generateNonce(uint8_t* nonce);        // GCM_NONCE_LEN bytes
