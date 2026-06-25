/*
 * crypto.h -- authenticated encryption + password hashing (DuraKV Phase 5).
 *
 * OS/systems primitive: authenticated encryption (AEAD), key derivation.
 *
 * Thin wrappers over libsodium. We use XChaCha20-Poly1305 (AEAD) rather than
 * AES-GCM because (a) it needs no hardware-AES instruction, and (b) its
 * 192-bit nonce makes a fresh random nonce per message collision-safe,
 * avoiding the catastrophic nonce-reuse footgun of GCM. Keys come from a
 * password via Argon2id (memory-hard). Never roll your own crypto.
 */
#ifndef DURAKV_CRYPTO_H
#define DURAKV_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#define CRYPTO_KEYBYTES    32   /* secret key                              */
#define CRYPTO_NONCEBYTES  24   /* XChaCha20 nonce (192-bit -> random-safe) */
#define CRYPTO_MACBYTES    16   /* Poly1305 authentication tag             */
#define CRYPTO_SALTBYTES   16   /* Argon2id salt                           */

/* Overhead a sealed blob adds over its plaintext (nonce + MAC). */
#define CRYPTO_SEAL_OVERHEAD (CRYPTO_NONCEBYTES + CRYPTO_MACBYTES)

/* Initialise libsodium. Returns 0 on success, -1 on failure. Call once. */
int  crypto_init(void);

/* Derive a key from a password and salt using Argon2id (interactive cost). */
int  crypto_derive_key(uint8_t key[CRYPTO_KEYBYTES], const char *password,
                       const uint8_t salt[CRYPTO_SALTBYTES]);
void crypto_random_salt(uint8_t salt[CRYPTO_SALTBYTES]);

/* AEAD seal: writes  [nonce || ciphertext+tag]  into out (which must have room
 * for plen + CRYPTO_SEAL_OVERHEAD bytes). Returns bytes written. A fresh random
 * nonce is generated each call. */
size_t crypto_seal(uint8_t *out, const uint8_t *plain, size_t plen,
                   const uint8_t key[CRYPTO_KEYBYTES]);

/* AEAD open: verifies and decrypts a blob produced by crypto_seal. Writes up to
 * inlen - CRYPTO_SEAL_OVERHEAD plaintext bytes into out. Returns the plaintext
 * length, or -1 if authentication fails (tampering / wrong key). */
long crypto_open(uint8_t *out, const uint8_t *in, size_t inlen,
                 const uint8_t key[CRYPTO_KEYBYTES]);

/* Argon2id password hashing for an auth store. The hash string is
 * self-describing (embeds salt + parameters); store it verbatim. */
#define CRYPTO_PWHASH_STRBYTES 128
int  crypto_password_hash(char out[CRYPTO_PWHASH_STRBYTES], const char *password);
int  crypto_password_verify(const char *hash_str, const char *password); /* 0 ok, -1 no */

#endif /* DURAKV_CRYPTO_H */
