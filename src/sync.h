#pragma once
#include <stdint.h>
#include <stddef.h>
#include "vault.h"

// ============================================================
// PhotonPass device-to-device sync via sequential QR frame stream
//
// QR frame string format (all uppercase — alphanumeric QR mode):
//   PPSS:<sess8hex>:<idx3dec>/<tot3dec>:<hexbytes>
//
//   sess8hex  — 8-char hex session ID (4 random bytes)
//   idx3dec   — 0-based frame index, zero-padded to 3 digits
//   tot3dec   — total frame count, zero-padded to 3 digits
//   hexbytes  — hex-encoded binary payload chunk (up to 320 chars)
//
// Full binary payload (assembled across all frames):
//   [nonce 12 B][GCM tag 16 B][AES-256-GCM ciphertext]
//
// Max frame string: 22 header + 320 hex = 342 chars → fits QR v10 ECC_L.
// Max payload: 128 × 160 = 20 480 B → ~136 records at 150 B/record.
//
// Key convention: PRESHARE_KEY vault records store the 32-byte key
// as a 64-char uppercase hex string in the password field.
// ============================================================

#define SYNC_PREFIX          "PPSS:"
#define SYNC_PREFIX_LEN      5
#define SYNC_CHUNK_BYTES     160    // binary bytes per QR frame payload
#define SYNC_MAX_FRAMES      128    // max frames per session
#define SYNC_FRAME_STR_MAX   345    // 22 header + 320 hex + NUL + margin

// One parsed inbound frame
struct SyncFrame {
    uint32_t sessionId;
    uint8_t  frameIdx;      // 0-based
    uint8_t  totalFrames;   // 1-based count of frames in this session
    uint8_t  payload[SYNC_CHUNK_BYTES];
    uint8_t  payloadLen;    // actual binary bytes in this frame
};

// ============================================================
// Inbound frame accumulator
// Declared global in sync.cpp to avoid large stack allocation.
// ============================================================
class SyncReceiver {
public:
    void reset();

    // Add a parsed frame. Returns true when all frames have been received.
    bool addFrame(const SyncFrame& f);

    bool     isComplete() const;
    uint32_t sessionId()  const { return _sessionId; }

    // Decrypt the assembled payload using preshareKey (32 bytes, raw).
    // Writes plaintext to outBuf (caller-allocated, >= SYNC_MAX_FRAMES * SYNC_CHUNK_BYTES).
    // Sets *outLen to the plaintext byte count.
    bool decrypt(const uint8_t* preshareKey,
                 uint8_t* outBuf, size_t bufLen, size_t* outLen);

private:
    uint8_t  _buf[(size_t)SYNC_MAX_FRAMES * SYNC_CHUNK_BYTES];
    uint8_t  _frameLens[SYNC_MAX_FRAMES]; // actual payload length per frame
    bool     _slots[SYNC_MAX_FRAMES];
    uint32_t _sessionId;
    uint8_t  _totalFrames;
    uint8_t  _received;
    bool     _started;
};

extern SyncReceiver syncReceiver;

// ============================================================
// Frame parsing / building
// ============================================================

// Parse a QR scan string into a SyncFrame. Returns false if not PPSS: format.
bool parseSyncFrame(const char* raw, SyncFrame* out);

// Write the QR display string for a single frame into buf (>= SYNC_FRAME_STR_MAX).
void buildSyncFrameString(const SyncFrame& f, char* buf, size_t bufLen);

// ============================================================
// Outbound helpers (static context owned by sync.cpp)
// ============================================================

// Serialize all vault records, encrypt with preshareKey (32 bytes raw), and
// split into frames. Call syncOutboundClear() when done to wipe heap memory.
// Returns false on error or if the payload exceeds SYNC_MAX_FRAMES capacity.
bool prepareSyncOutbound(const VaultState& vault, const uint8_t* preshareKey);

uint8_t syncOutboundTotal();

// Write the QR string for frame idx (0-based) into buf (>= SYNC_FRAME_STR_MAX).
void syncOutboundGetFrame(uint8_t idx, char* buf, size_t bufLen);

// Wipe and free the outbound payload. Call after the sync stream has been shown.
void syncOutboundClear();

// ============================================================
// Hex key helper
// ============================================================

// Decode a 64-char uppercase hex string into a 32-byte AES key.
// Returns false if hexStr is not exactly 64 valid hex characters.
bool syncDecodeKey(const char* hexStr, uint8_t* outKey);

// ============================================================
// Merge
// ============================================================

// Deserialize plaintext sync payload and merge records into vault.
// Duplicate UUIDs: keep the record with the newer lastChanged timestamp.
// Returns the number of records added or updated.
uint8_t mergeSyncRecords(VaultState& vault, const uint8_t* data, size_t len);
