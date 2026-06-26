/*
 * encryption.c -- encryption-at-rest codec for the storage engine (Phase 5).
 *
 * Builds a PageCodec (XChaCha20-Poly1305 via crypto.c) whose key is derived
 * from a password and the data file's KDF salt, then opens the DB with it.
 * This is the only file that links the storage engine to libsodium; storage.c
 * and recovery.c stay dependency-free and call through the codec's function
 * pointers. See include/storage.h (PageCodec) and include/crypto.h.
 */
#include "storage.h"
#include "crypto.h"

#include <sodium.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t key[CRYPTO_KEYBYTES]; } EncCtx;

static long enc_seal(void *ctx, const uint8_t *plain, size_t plen, uint8_t *out)
{
    return (long)crypto_seal(out, plain, plen, ((EncCtx *)ctx)->key);
}
static long enc_open(void *ctx, const uint8_t *in, size_t inlen, uint8_t *plain)
{
    return crypto_open(plain, in, inlen, ((EncCtx *)ctx)->key);
}
static void enc_free(void *ctx)
{
    if (ctx) { sodium_memzero(ctx, sizeof(EncCtx)); free(ctx); }
}

DB *db_open_secure(const char *data_path, const char *wal_path,
                   size_t nframes, PolicyKind policy, const char *password)
{
    if (crypto_init() != 0) return NULL;

    /* a new store gets a fresh random salt; an existing one reuses the salt
     * stored (plaintext) in its header so the same password derives the key. */
    int is_new = 0, encrypted = 0;
    uint8_t salt[DURAKV_SALT_LEN];
    storage_peek(data_path, &is_new, salt, &encrypted);
    if (is_new) crypto_random_salt(salt);

    EncCtx *ec = malloc(sizeof(EncCtx));
    if (!ec) return NULL;
    if (crypto_derive_key(ec->key, password, salt) != 0) { free(ec); return NULL; }

    PageCodec *codec = calloc(1, sizeof(PageCodec));
    codec->ctx      = ec;
    codec->seal     = enc_seal;
    codec->open     = enc_open;
    codec->overhead = CRYPTO_SEAL_OVERHEAD;
    codec->free_ctx = enc_free;
    memcpy(codec->salt, salt, DURAKV_SALT_LEN);

    DB *db = db_open_full(data_path, wal_path, nframes, policy, codec);
    if (!db) { enc_free(ec); free(codec); return NULL; }
    return db;            /* db owns codec; freed in db_close */
}
