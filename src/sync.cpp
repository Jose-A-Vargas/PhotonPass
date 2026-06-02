#include "sync.h"
#include "crypto.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <esp_random.h>

// ============================================================
// Hex encoding helpers (uppercase, QR alphanumeric-safe)
// ============================================================

static const char HEX_CHARS[] = "0123456789ABCDEF";

static void toHex(const uint8_t* src, size_t len, char* dst) {
    for (size_t i = 0; i < len; i++) {
        dst[i * 2]     = HEX_CHARS[src[i] >> 4];
        dst[i * 2 + 1] = HEX_CHARS[src[i] & 0x0F];
    }
}

static int8_t hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool fromHex(const char* src, size_t hexLen, uint8_t* dst) {
    if (hexLen & 1) return false;
    for (size_t i = 0; i < hexLen; i += 2) {
        int8_t hi = hexNibble(src[i]);
        int8_t lo = hexNibble(src[i + 1]);
        if (hi < 0 || lo < 0) return false;
        dst[i / 2] = ((uint8_t)hi << 4) | (uint8_t)lo;
    }
    return true;
}

// ============================================================
// Frame parsing / building
// ============================================================

bool parseSyncFrame(const char* raw, SyncFrame* out) {
    if (!raw || !out) return false;
    if (strncmp(raw, SYNC_PREFIX, SYNC_PREFIX_LEN) != 0) return false;

    const char* p = raw + SYNC_PREFIX_LEN;

    // Session ID: 8 uppercase hex chars
    char sessHex[9] = {};
    for (int i = 0; i < 8; i++) {
        if (!*p || hexNibble(*p) < 0) return false;
        sessHex[i] = *p++;
    }
    uint8_t sessBytes[4];
    if (!fromHex(sessHex, 8, sessBytes)) return false;
    out->sessionId = ((uint32_t)sessBytes[0] << 24) | ((uint32_t)sessBytes[1] << 16)
                   | ((uint32_t)sessBytes[2] <<  8) |  (uint32_t)sessBytes[3];

    if (*p++ != ':') return false;

    // Frame index: NNN
    char idxStr[4] = {};
    for (int i = 0; i < 3 && *p >= '0' && *p <= '9'; i++) idxStr[i] = *p++;
    if (*p++ != '/') return false;

    // Total frames: TTT
    char totStr[4] = {};
    for (int i = 0; i < 3 && *p >= '0' && *p <= '9'; i++) totStr[i] = *p++;
    if (*p++ != ':') return false;

    int idx = atoi(idxStr);
    int tot = atoi(totStr);
    if (idx < 0 || tot <= 0 || idx >= tot || tot > SYNC_MAX_FRAMES) return false;

    out->frameIdx    = (uint8_t)idx;
    out->totalFrames = (uint8_t)tot;

    // Hex payload
    size_t hexLen = strlen(p);
    if (hexLen == 0 || (hexLen & 1) || hexLen / 2 > SYNC_CHUNK_BYTES) return false;
    if (!fromHex(p, hexLen, out->payload)) return false;
    out->payloadLen = (uint8_t)(hexLen / 2);

    return true;
}

void buildSyncFrameString(const SyncFrame& f, char* buf, size_t bufLen) {
    char hexBuf[SYNC_CHUNK_BYTES * 2 + 1];
    toHex(f.payload, f.payloadLen, hexBuf);
    hexBuf[f.payloadLen * 2] = '\0';

    snprintf(buf, bufLen, SYNC_PREFIX "%08X:%03u/%03u:%s",
             (unsigned)f.sessionId,
             (unsigned)f.frameIdx,
             (unsigned)f.totalFrames,
             hexBuf);
}

// ============================================================
// SyncReceiver
// ============================================================

SyncReceiver syncReceiver;

void SyncReceiver::reset() {
    secureClear(_buf, sizeof(_buf));
    memset(_slots,     0, sizeof(_slots));
    memset(_frameLens, 0, sizeof(_frameLens));
    _sessionId   = 0;
    _totalFrames = 0;
    _received    = 0;
    _started     = false;
}

bool SyncReceiver::addFrame(const SyncFrame& f) {
    if (!_started) {
        _sessionId   = f.sessionId;
        _totalFrames = f.totalFrames;
        _started     = true;
    } else if (f.sessionId != _sessionId || f.totalFrames != _totalFrames) {
        // New session — reset and start fresh
        reset();
        _sessionId   = f.sessionId;
        _totalFrames = f.totalFrames;
        _started     = true;
    }

    if (f.frameIdx >= _totalFrames || f.frameIdx >= SYNC_MAX_FRAMES) return false;
    if (_slots[f.frameIdx]) return isComplete();  // duplicate, already have it

    size_t offset = (size_t)f.frameIdx * SYNC_CHUNK_BYTES;
    memcpy(_buf + offset, f.payload, f.payloadLen);
    _frameLens[f.frameIdx] = f.payloadLen;
    _slots[f.frameIdx]     = true;
    _received++;

    return isComplete();
}

bool SyncReceiver::isComplete() const {
    return _started && (_received == _totalFrames);
}

bool SyncReceiver::decrypt(const uint8_t* preshareKey,
                            uint8_t* outBuf, size_t bufLen, size_t* outLen) {
    if (!isComplete()) return false;

    // Compute total assembled bytes
    size_t totalBytes = 0;
    for (uint8_t i = 0; i < _totalFrames; i++) totalBytes += _frameLens[i];

    if (totalBytes < (size_t)(GCM_NONCE_LEN + GCM_TAG_LEN + 1)) return false;

    // Reassemble frames contiguously into a temp buffer (heap to keep stack safe)
    uint8_t* assembled = (uint8_t*)malloc(totalBytes);
    if (!assembled) return false;

    size_t off = 0;
    for (uint8_t i = 0; i < _totalFrames; i++) {
        memcpy(assembled + off, _buf + (size_t)i * SYNC_CHUNK_BYTES, _frameLens[i]);
        off += _frameLens[i];
    }

    // Layout: [nonce(12)] [tag(16)] [ciphertext]
    const uint8_t* nonce      = assembled;
    const uint8_t* tag        = assembled + GCM_NONCE_LEN;
    const uint8_t* ciphertext = assembled + GCM_NONCE_LEN + GCM_TAG_LEN;
    size_t         cipherLen  = totalBytes - GCM_NONCE_LEN - GCM_TAG_LEN;

    if (bufLen < cipherLen) { secureClear(assembled, totalBytes); free(assembled); return false; }

    CryptoResult res = decryptGCM(
        preshareKey, nonce,
        nullptr, 0,          // no AAD for sync frames
        ciphertext, cipherLen,
        tag,
        outBuf
    );

    secureClear(assembled, totalBytes);
    free(assembled);

    if (res != CryptoResult::OK) return false;
    *outLen = cipherLen;
    return true;
}

// ============================================================
// Outbound static context
// ============================================================

static uint8_t*  _outPayload    = nullptr;
static size_t    _outPayloadLen = 0;
static uint8_t   _outTotal      = 0;
static uint32_t  _outSessionId  = 0;

bool prepareSyncOutbound(const VaultState& vault, const uint8_t* preshareKey) {
    syncOutboundClear();

    if (vault.records.empty()) return false;

    auto plain = serializeRecords(vault.records);
    if (plain.empty()) return false;

    // Check that the payload will fit within SYNC_MAX_FRAMES frames
    size_t payloadLen = (size_t)GCM_NONCE_LEN + GCM_TAG_LEN + plain.size();
    uint32_t frameCount = (uint32_t)((payloadLen + SYNC_CHUNK_BYTES - 1) / SYNC_CHUNK_BYTES);
    if (frameCount > SYNC_MAX_FRAMES) {
        secureClear(plain.data(), plain.size());
        return false;
    }

    _outPayload = (uint8_t*)malloc(payloadLen);
    if (!_outPayload) {
        secureClear(plain.data(), plain.size());
        return false;
    }

    uint8_t* nonce      = _outPayload;
    uint8_t* tag        = _outPayload + GCM_NONCE_LEN;
    uint8_t* ciphertext = _outPayload + GCM_NONCE_LEN + GCM_TAG_LEN;

    generateNonce(nonce);

    CryptoResult res = encryptGCM(
        preshareKey, nonce,
        nullptr, 0,           // no AAD
        plain.data(), plain.size(),
        ciphertext, tag
    );

    secureClear(plain.data(), plain.size());

    if (res != CryptoResult::OK) {
        free(_outPayload);
        _outPayload = nullptr;
        return false;
    }

    _outPayloadLen = payloadLen;
    _outTotal      = (uint8_t)frameCount;
    _outSessionId  = esp_random();
    return true;
}

uint8_t syncOutboundTotal() { return _outTotal; }

void syncOutboundGetFrame(uint8_t idx, char* buf, size_t bufLen) {
    if (!_outPayload || idx >= _outTotal || !buf) {
        if (buf) buf[0] = '\0';
        return;
    }

    size_t offset   = (size_t)idx * SYNC_CHUNK_BYTES;
    size_t chunkLen = _outPayloadLen - offset;
    if (chunkLen > SYNC_CHUNK_BYTES) chunkLen = SYNC_CHUNK_BYTES;

    SyncFrame f;
    f.sessionId   = _outSessionId;
    f.frameIdx    = idx;
    f.totalFrames = _outTotal;
    memcpy(f.payload, _outPayload + offset, chunkLen);
    f.payloadLen  = (uint8_t)chunkLen;

    buildSyncFrameString(f, buf, bufLen);
}

void syncOutboundClear() {
    if (_outPayload) {
        secureClear(_outPayload, _outPayloadLen);
        free(_outPayload);
        _outPayload = nullptr;
    }
    _outPayloadLen = 0;
    _outTotal      = 0;
    _outSessionId  = 0;
}

// ============================================================
// Hex key helper
// ============================================================

bool syncDecodeKey(const char* hexStr, uint8_t* outKey) {
    if (!hexStr || strlen(hexStr) != 64) return false;
    return fromHex(hexStr, 64, outKey);
}

// ============================================================
// Merge
// ============================================================

uint8_t mergeSyncRecords(VaultState& vault, const uint8_t* data, size_t len) {
    std::vector<VaultRecord> incoming;
    if (!deserializeRecords(data, len, incoming)) return 0;

    uint8_t changed = 0;

    for (auto& in : incoming) {
        bool found = false;
        for (auto& ex : vault.records) {
            if (memcmp(ex.uuid, in.uuid, 16) != 0) continue;
            found = true;
            if (in.lastChanged > ex.lastChanged) {
                // Zero old plaintext before overwriting with newer record
                secureClear(ex.password.data(), ex.password.size());
                for (auto& h : ex.history)
                    secureClear(h.password.data(), h.password.size());
                ex = std::move(in);
                changed++;
            }
            break;
        }
        if (!found && vault.records.size() < VAULT_MAX_RECORDS) {
            vault.records.push_back(std::move(in));
            changed++;
        }
    }

    // Wipe plaintext passwords from the temporary incoming vector
    for (auto& r : incoming) {
        secureClear(r.password.data(), r.password.size());
        for (auto& h : r.history)
            secureClear(h.password.data(), h.password.size());
    }

    return changed;
}
