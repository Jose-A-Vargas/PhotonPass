#include "vault.h"
#include <LittleFS.h>
#include <esp_random.h>
#include <string.h>
#include <stddef.h>

// ============================================================
// Binary encode/decode helpers (big-endian for portability)
// ============================================================

static void writeU16(uint8_t* buf, size_t& off, uint16_t v) {
    buf[off++] = (v >> 8) & 0xFF;
    buf[off++] =  v       & 0xFF;
}

static void writeU32(uint8_t* buf, size_t& off, uint32_t v) {
    buf[off++] = (v >> 24) & 0xFF;
    buf[off++] = (v >> 16) & 0xFF;
    buf[off++] = (v >>  8) & 0xFF;
    buf[off++] =  v        & 0xFF;
}

static void writeBlob(uint8_t* buf, size_t& off, const void* src, size_t len) {
    memcpy(buf + off, src, len);
    off += len;
}

static void writeStr(uint8_t* buf, size_t& off, const std::string& s) {
    writeU16(buf, off, (uint16_t)s.size());
    writeBlob(buf, off, s.data(), s.size());
}

static uint16_t readU16(const uint8_t* buf, size_t& off, size_t cap) {
    if (off + 2 > cap) return 0;
    uint16_t v = ((uint16_t)buf[off] << 8) | buf[off + 1];
    off += 2;
    return v;
}

static uint32_t readU32(const uint8_t* buf, size_t& off, size_t cap) {
    if (off + 4 > cap) return 0;
    uint32_t v = ((uint32_t)buf[off]     << 24)
               | ((uint32_t)buf[off + 1] << 16)
               | ((uint32_t)buf[off + 2] <<  8)
               |  (uint32_t)buf[off + 3];
    off += 4;
    return v;
}

static std::string readStr(const uint8_t* buf, size_t& off, size_t cap) {
    uint16_t len = readU16(buf, off, cap);
    if (off + len > cap) return {};
    std::string s((const char*)(buf + off), len);
    off += len;
    return s;
}

// ============================================================
// Serialization
// ============================================================

static size_t recordSize(const VaultRecord& r) {
    size_t sz = 16 + 1;                      // uuid + type
    sz += 2 + r.domain.size();
    sz += 2 + r.username.size();
    sz += 2 + r.password.size();
    sz += 2 + r.queryValue.size();
    sz += 4 + 1;                             // lastChanged + historyCount
    for (const auto& h : r.history)
        sz += 2 + h.password.size() + 4;
    return sz;
}

std::vector<uint8_t> serializeRecords(const std::vector<VaultRecord>& records) {
    size_t total = 0;
    for (const auto& r : records) total += recordSize(r);

    std::vector<uint8_t> buf(total);
    size_t off = 0;

    for (const auto& r : records) {
        writeBlob(buf.data(), off, r.uuid, 16);
        buf[off++] = (uint8_t)r.type;
        writeStr(buf.data(), off, r.domain);
        writeStr(buf.data(), off, r.username);
        writeStr(buf.data(), off, r.password);
        writeStr(buf.data(), off, r.queryValue);
        writeU32(buf.data(), off, r.lastChanged);
        buf[off++] = (uint8_t)r.history.size();
        for (const auto& h : r.history) {
            writeStr(buf.data(), off, h.password);
            writeU32(buf.data(), off, h.changedAt);
        }
    }

    return buf;
}

bool deserializeRecords(const uint8_t* buf, size_t len, std::vector<VaultRecord>& out) {
    size_t off = 0;
    while (off < len) {
        if (off + 17 > len) return false;  // uuid(16) + type(1) minimum

        VaultRecord r;
        memcpy(r.uuid, buf + off, 16);
        off += 16;
        r.type       = (RecordType)buf[off++];
        r.domain     = readStr(buf, off, len);
        r.username   = readStr(buf, off, len);
        r.password   = readStr(buf, off, len);
        r.queryValue = readStr(buf, off, len);

        if (off + 5 > len) return false;   // lastChanged(4) + historyCount(1)
        r.lastChanged = readU32(buf, off, len);
        uint8_t hCount = buf[off++];

        for (uint8_t i = 0; i < hCount; i++) {
            HistoryEntry h;
            h.password  = readStr(buf, off, len);
            h.changedAt = readU32(buf, off, len);
            r.history.push_back(std::move(h));
        }
        out.push_back(std::move(r));
    }
    return true;
}

// ============================================================
// LittleFS persistence
// ============================================================

bool saveVault(const VaultState& state, const char* path) {
    if (!state.unlocked) return false;

    auto plaintext = serializeRecords(state.records);

    VaultHeader hdr;
    hdr.magic[0]    = VAULT_MAGIC_0;
    hdr.magic[1]    = VAULT_MAGIC_1;
    hdr.magic[2]    = VAULT_MAGIC_2;
    hdr.magic[3]    = VAULT_MAGIC_3;
    hdr.version     = VAULT_VERSION;
    hdr.flags       = 0;
    hdr.recordCount = (uint16_t)state.records.size();
    memcpy(hdr.salt, state.salt, PBKDF2_SALT_LEN);
    generateNonce(hdr.nonce);  // fresh nonce every write
    hdr.payloadLen  = (uint32_t)plaintext.size();

    // AAD = every header byte up to (not including) the tag field.
    // This authenticates magic, version, record count, salt, and nonce.
    const size_t aadLen = offsetof(VaultHeader, tag);

    std::vector<uint8_t> cipher(plaintext.size());

    // mbedtls rejects null pointers even when length is 0 (empty vault).
    // Use a non-null dummy so GCM still produces a valid auth tag over the AAD.
    static const uint8_t kEmptyIn[1]  = {0};
    static       uint8_t kEmptyOut[1] = {0};
    const uint8_t* ptPtr = plaintext.empty() ? kEmptyIn  : plaintext.data();
    uint8_t*       ctPtr = cipher.empty()    ? kEmptyOut : cipher.data();

    CryptoResult res = encryptGCM(
        state.masterKey,
        hdr.nonce,
        (const uint8_t*)&hdr, aadLen,
        ptPtr, plaintext.size(),
        ctPtr, hdr.tag
    );

    secureClear(plaintext.data(), plaintext.size());
    if (res != CryptoResult::OK) return false;

    File f = LittleFS.open(path, FILE_WRITE);
    if (!f) return false;
    f.write((const uint8_t*)&hdr, sizeof(hdr));
    f.write(cipher.data(), cipher.size());
    f.close();
    return true;
}

bool loadVault(VaultState& state, const char* path) {
    File f = LittleFS.open(path, FILE_READ);
    if (!f) return false;

    VaultHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) {
        f.close();
        return false;
    }

    if (hdr.magic[0] != VAULT_MAGIC_0 || hdr.magic[1] != VAULT_MAGIC_1 ||
        hdr.magic[2] != VAULT_MAGIC_2 || hdr.magic[3] != VAULT_MAGIC_3 ||
        hdr.version  != VAULT_VERSION) {
        f.close();
        return false;
    }

    std::vector<uint8_t> cipher(hdr.payloadLen);
    if (f.read(cipher.data(), hdr.payloadLen) != (int)hdr.payloadLen) {
        f.close();
        return false;
    }
    f.close();

    memcpy(state.salt, hdr.salt, PBKDF2_SALT_LEN);

    const size_t aadLen = offsetof(VaultHeader, tag);
    std::vector<uint8_t> plain(hdr.payloadLen);

    CryptoResult res = decryptGCM(
        state.masterKey,
        hdr.nonce,
        (const uint8_t*)&hdr, aadLen,
        cipher.data(),        cipher.size(),
        hdr.tag,
        plain.data()
    );

    if (res != CryptoResult::OK) {
        secureClear(plain.data(), plain.size());
        return false;
    }

    state.records.clear();
    bool ok = deserializeRecords(plain.data(), plain.size(), state.records);
    secureClear(plain.data(), plain.size());

    if (ok) {
        state.unlocked   = true;
        state.unlockedAt = millis();
    }
    return ok;
}

// ============================================================
// TRNG helpers
// ============================================================

void generateUUID(uint8_t* uuid) {
    for (int i = 0; i < 16; i += 4) {
        uint32_t rnd = esp_random();
        memcpy(uuid + i, &rnd, 4);
    }
    uuid[6] = (uuid[6] & 0x0F) | 0x40;  // version 4
    uuid[8] = (uuid[8] & 0x3F) | 0x80;  // RFC 4122 variant
}

void generateSalt(uint8_t* salt) {
    for (int i = 0; i < PBKDF2_SALT_LEN; i += 4) {
        uint32_t rnd = esp_random();
        memcpy(salt + i, &rnd, 4);
    }
}

void generateNonce(uint8_t* nonce) {
    // GCM_NONCE_LEN = 12, read 4 bytes at a time (3 iterations)
    for (int i = 0; i < GCM_NONCE_LEN; i += 4) {
        uint32_t rnd = esp_random();
        int remaining = GCM_NONCE_LEN - i;
        memcpy(nonce + i, &rnd, remaining < 4 ? remaining : 4);
    }
}
