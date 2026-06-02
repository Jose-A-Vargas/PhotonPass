#include "crypto.h"
#include "config.h"
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <stdlib.h>
#include <string.h>

CryptoResult deriveKey(
    const char*    passphrase,
    size_t         passphraseLen,
    const uint8_t* token,
    size_t         tokenLen,
    const uint8_t* salt,
    uint8_t*       outKey
) {
    // Concatenate device salt + QR token so both physical factors feed into the KDF.
    size_t   fullLen  = PBKDF2_SALT_LEN + tokenLen;
    uint8_t* fullSalt = (uint8_t*)malloc(fullLen);
    if (!fullSalt) return CryptoResult::ERR_DERIVE;

    memcpy(fullSalt,                  salt,  PBKDF2_SALT_LEN);
    memcpy(fullSalt + PBKDF2_SALT_LEN, token, tokenLen);

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    int ret = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    if (ret == 0) {
        ret = mbedtls_pkcs5_pbkdf2_hmac(
            &ctx,
            (const uint8_t*)passphrase, passphraseLen,
            fullSalt, fullLen,
            PBKDF2_ITERATIONS,
            AES_KEY_LEN,
            outKey
        );
    }

    mbedtls_md_free(&ctx);
    mbedtls_platform_zeroize(fullSalt, fullLen);
    free(fullSalt);

    return (ret == 0) ? CryptoResult::OK : CryptoResult::ERR_DERIVE;
}

CryptoResult encryptGCM(
    const uint8_t* key,
    const uint8_t* nonce,
    const uint8_t* aad,
    size_t         aadLen,
    const uint8_t* plaintext,
    size_t         plaintextLen,
    uint8_t*       outCipher,
    uint8_t*       outTag
) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, AES_KEY_LEN * 8);
    if (ret == 0) {
        ret = mbedtls_gcm_crypt_and_tag(
            &gcm, MBEDTLS_GCM_ENCRYPT,
            plaintextLen,
            nonce,   GCM_NONCE_LEN,
            aad,     aadLen,
            plaintext, outCipher,
            GCM_TAG_LEN, outTag
        );
    }

    mbedtls_gcm_free(&gcm);
    return (ret == 0) ? CryptoResult::OK : CryptoResult::ERR_ENCRYPT;
}

CryptoResult decryptGCM(
    const uint8_t* key,
    const uint8_t* nonce,
    const uint8_t* aad,
    size_t         aadLen,
    const uint8_t* cipher,
    size_t         cipherLen,
    const uint8_t* tag,
    uint8_t*       outPlain
) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, AES_KEY_LEN * 8);
    if (ret == 0) {
        ret = mbedtls_gcm_auth_decrypt(
            &gcm,
            cipherLen,
            nonce,  GCM_NONCE_LEN,
            aad,    aadLen,
            tag,    GCM_TAG_LEN,
            cipher, outPlain
        );
    }

    mbedtls_gcm_free(&gcm);

    if (ret == MBEDTLS_ERR_GCM_AUTH_FAILED) return CryptoResult::ERR_AUTH;
    return (ret == 0) ? CryptoResult::OK : CryptoResult::ERR_DECRYPT;
}

void secureClear(const void* buf, size_t len) {
    mbedtls_platform_zeroize(const_cast<void*>(buf), len);
}
