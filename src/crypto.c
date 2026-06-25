/*
 * crypto.c -- libsodium wrappers for AEAD and Argon2id. See include/crypto.h.
 */
#include "crypto.h"

#include <sodium.h>
#include <string.h>

int crypto_init(void)
{
    return sodium_init() < 0 ? -1 : 0;     /* idempotent; -1 only on failure */
}

void crypto_random_salt(uint8_t salt[CRYPTO_SALTBYTES])
{
    randombytes_buf(salt, CRYPTO_SALTBYTES);
}

int crypto_derive_key(uint8_t key[CRYPTO_KEYBYTES], const char *password,
                      const uint8_t salt[CRYPTO_SALTBYTES])
{
    /* Argon2id, interactive cost parameters (memory-hard KDF). */
    return crypto_pwhash(key, CRYPTO_KEYBYTES, password, strlen(password), salt,
                         crypto_pwhash_OPSLIMIT_INTERACTIVE,
                         crypto_pwhash_MEMLIMIT_INTERACTIVE,
                         crypto_pwhash_ALG_ARGON2ID13);
}

size_t crypto_seal(uint8_t *out, const uint8_t *plain, size_t plen,
                   const uint8_t key[CRYPTO_KEYBYTES])
{
    uint8_t *nonce = out;                          /* layout: [nonce][cipher] */
    uint8_t *cipher = out + CRYPTO_NONCEBYTES;
    randombytes_buf(nonce, CRYPTO_NONCEBYTES);     /* fresh nonce every time  */

    unsigned long long clen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        cipher, &clen, plain, plen, NULL, 0, NULL, nonce, key);
    return CRYPTO_NONCEBYTES + (size_t)clen;
}

long crypto_open(uint8_t *out, const uint8_t *in, size_t inlen,
                 const uint8_t key[CRYPTO_KEYBYTES])
{
    if (inlen < CRYPTO_SEAL_OVERHEAD) return -1;
    const uint8_t *nonce  = in;
    const uint8_t *cipher = in + CRYPTO_NONCEBYTES;
    size_t clen = inlen - CRYPTO_NONCEBYTES;

    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            out, &mlen, NULL, cipher, clen, NULL, 0, nonce, key) != 0)
        return -1;                                  /* tampered or wrong key */
    return (long)mlen;
}

int crypto_password_hash(char out[CRYPTO_PWHASH_STRBYTES], const char *password)
{
    return crypto_pwhash_str(out, password, strlen(password),
                             crypto_pwhash_OPSLIMIT_INTERACTIVE,
                             crypto_pwhash_MEMLIMIT_INTERACTIVE);
}

int crypto_password_verify(const char *hash_str, const char *password)
{
    return crypto_pwhash_str_verify(hash_str, password, strlen(password)) == 0 ? 0 : -1;
}
