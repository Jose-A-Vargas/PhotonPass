#pragma once
#include <stdint.h>
#include <stddef.h>

enum class CryptoResult : uint8_t {
    OK          = 0,
    ERR_DERIVE  = 1,
    ERR_ENCRYPT = 2,
    ERR_DECRYPT = 3,
    ERR_AUTH    = 4,  // GCM tag mismatch — data tampered or wrong key
};

// Derive 256-bit AES key from passphrase + physical QR token via PBKDF2-HMAC-SHA256.
// fullSalt = vault header salt || token bytes, so both factors contribute to key material.
// outKey must be AES_KEY_LEN (32) bytes.
CryptoResult deriveKey(
    const char*    passphrase,
    size_t         passphraseLen,
    const uint8_t* token,
    size_t         tokenLen,
    const uint8_t* salt,        // PBKDF2_SALT_LEN bytes
    uint8_t*       outKey
);

// AES-256-GCM encrypt.
// aad bytes are authenticated but not encrypted (use for vault header).
// outCipher must be plaintextLen bytes; outTag must be GCM_TAG_LEN bytes.
CryptoResult encryptGCM(
    const uint8_t* key,
    const uint8_t* nonce,       // GCM_NONCE_LEN bytes
    const uint8_t* aad,
    size_t         aadLen,
    const uint8_t* plaintext,
    size_t         plaintextLen,
    uint8_t*       outCipher,
    uint8_t*       outTag
);

// AES-256-GCM decrypt + authenticate.
// Returns ERR_AUTH immediately on tag mismatch without exposing any plaintext.
CryptoResult decryptGCM(
    const uint8_t* key,
    const uint8_t* nonce,
    const uint8_t* aad,
    size_t         aadLen,
    const uint8_t* cipher,
    size_t         cipherLen,
    const uint8_t* tag,         // GCM_TAG_LEN bytes
    uint8_t*       outPlain
);

// Compiler-safe zero wipe (uses mbedtls_platform_zeroize internally).
void secureClear(const void* buf, size_t len);
