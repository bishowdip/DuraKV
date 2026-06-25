/*
 * demo_crypto.c -- authenticated encryption and password hashing (Task 3).
 *
 * Shows the three security guarantees DuraKV relies on:
 *   1. Confidentiality -- the sealed bytes are ciphertext, not the plaintext.
 *   2. Integrity      -- flipping a single ciphertext byte makes decryption
 *                        FAIL (the Poly1305 tag catches tampering).
 *   3. Authentication -- passwords are stored as Argon2id hashes; the right
 *                        password verifies, a wrong one does not.
 */
#include "crypto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void hex(const char *label, const uint8_t *p, size_t n)
{
    printf("%s", label);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

int main(void)
{
    assert(crypto_init() == 0);

    /* ---- key from a password (Argon2id) ---- */
    uint8_t salt[CRYPTO_SALTBYTES], key[CRYPTO_KEYBYTES];
    crypto_random_salt(salt);
    assert(crypto_derive_key(key, "correct horse battery staple", salt) == 0);

    /* ---- 1 & 2: seal / open / tamper ---- */
    const char *secret = "balance=1000; owner=bishowdip";
    size_t plen = strlen(secret);

    uint8_t blob[256];
    size_t blen = crypto_seal(blob, (const uint8_t *)secret, plen, key);

    printf("plaintext : %s\n", secret);
    hex("sealed    : ", blob, blen);
    printf("            (^ ciphertext on disk -- no plaintext is visible)\n\n");

    uint8_t out[256];
    long n = crypto_open(out, blob, blen, key);
    assert(n == (long)plen && memcmp(out, secret, plen) == 0);
    printf("decrypted : %.*s   (round-trip OK)\n", (int)n, out);

    blob[blen - 1] ^= 0x01;                       /* tamper one byte */
    long bad = crypto_open(out, blob, blen, key);
    printf("tampered  : decrypt returns %ld  (%s)\n", bad,
           bad < 0 ? "REJECTED -- integrity protected" : "ACCEPTED?!");
    assert(bad == -1);

    /* ---- 3: Argon2id password auth ---- */
    char hash[CRYPTO_PWHASH_STRBYTES];
    assert(crypto_password_hash(hash, "hunter2") == 0);
    printf("\npassword hash (Argon2id): %.48s...\n", hash);
    printf("verify correct password : %s\n",
           crypto_password_verify(hash, "hunter2") == 0 ? "ACCEPTED" : "rejected");
    printf("verify wrong   password : %s\n",
           crypto_password_verify(hash, "letmein") == 0 ? "accepted?!" : "REJECTED");
    assert(crypto_password_verify(hash, "hunter2") == 0);
    assert(crypto_password_verify(hash, "letmein") != 0);

    printf("\ndemo_crypto: PASS\n");
    return 0;
}
