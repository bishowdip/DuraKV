/*
 * crypto.c -- the cryptographic primitives for Task 3, thin wrappers over
 * libsodium so the rest of the code never touches raw crypto.
 *
 * Two building blocks:
 *
 *  - AEAD seal/open using XChaCha20-Poly1305. "AEAD" = Authenticated Encryption
 *    with Associated Data: it provides BOTH confidentiality (the plaintext is
 *    hidden) AND integrity (any tampering is detected, because decryption of a
 *    modified ciphertext fails the Poly1305 tag check). XChaCha20 is chosen over
 *    AES-GCM because (a) it needs no AES hardware to be fast and constant-time
 *    in software, and (b) its 192-bit (24-byte) nonce is large enough that
 *    random nonces effectively never collide -- so we can pick a fresh random
 *    nonce per message without a counter, which is what crypto_seal does.
 *
 *  - Password handling with Argon2id, a MEMORY-HARD key-derivation function.
 *    Being memory-hard means an attacker cannot cheaply parallelise guesses on
 *    GPUs/ASICs (each guess must allocate a large memory buffer), which is what
 *    makes it far stronger than a plain hash for passwords. Used both to derive
 *    the encryption-at-rest key from a master password (crypto_derive_key) and
 *    to store login passwords (crypto_password_hash/verify).
 *
 * See include/crypto.h for sizes and the sealed-blob layout.
 */
#include "crypto.h"

#include <sodium.h>
#include <string.h>

/* Initialise libsodium (seeds the RNG, selects CPU-optimised implementations).
 * Safe to call more than once. Returns -1 only on a genuine init failure. */
int crypto_init(void)
{
    return sodium_init() < 0 ? -1 : 0;     /* idempotent; -1 only on failure */
}

/* Generate a random salt using the OS CSPRNG. A per-database salt ensures two
 * databases with the same password derive different keys (defeats rainbow
 * tables and cross-database key reuse). */
void crypto_random_salt(uint8_t salt[CRYPTO_SALTBYTES])
{
    randombytes_buf(salt, CRYPTO_SALTBYTES);
}

/* Derive a symmetric encryption key from a password + salt via Argon2id.
 * Interactive cost parameters trade brute-force resistance against latency
 * suitable for an interactive login. Deterministic: same password+salt -> same
 * key, which is how the correct password unlocks an existing database. */
int crypto_derive_key(uint8_t key[CRYPTO_KEYBYTES], const char *password,
                      const uint8_t salt[CRYPTO_SALTBYTES])
{
    return crypto_pwhash(key, CRYPTO_KEYBYTES, password, strlen(password), salt,
                         crypto_pwhash_OPSLIMIT_INTERACTIVE,
                         crypto_pwhash_MEMLIMIT_INTERACTIVE,
                         crypto_pwhash_ALG_ARGON2ID13);
}

/* Encrypt+authenticate `plen` bytes into `out`, returning the total length.
 * Output layout is [24-byte nonce][ciphertext+16-byte tag]; the nonce is stored
 * in the clear (it is not secret, only unique) so crypto_open can recover it. */
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

/* Verify+decrypt a blob produced by crypto_seal. Returns the plaintext length,
 * or -1 if the input is too short, was tampered with, or the key is wrong --
 * the three are indistinguishable by design (the tag check just fails). */
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

/* Produce a self-describing Argon2id hash string (embeds algorithm, params and
 * salt) suitable for storing in a user record. */
int crypto_password_hash(char out[CRYPTO_PWHASH_STRBYTES], const char *password)
{
    return crypto_pwhash_str(out, password, strlen(password),
                             crypto_pwhash_OPSLIMIT_INTERACTIVE,
                             crypto_pwhash_MEMLIMIT_INTERACTIVE);
}

/* Check a password against a stored hash. libsodium's verify re-derives using
 * the parameters encoded in the hash string and compares in constant time, so
 * it leaks nothing timing-wise. Returns 0 on match, -1 otherwise. */
int crypto_password_verify(const char *hash_str, const char *password)
{
    return crypto_pwhash_str_verify(hash_str, password, strlen(password)) == 0 ? 0 : -1;
}
