/*
 * demo_encrypt.c -- encryption at rest (Task 3).
 *
 * Writes secrets to an encrypted store, then proves:
 *   1. neither data.db nor wal.log contains the plaintext on disk;
 *   2. reopening with the CORRECT password recovers the data;
 *   3. reopening with the WRONG password yields nothing (data inaccessible),
 *      and does not corrupt the store.
 */
#include "storage.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define DATA "/tmp/durakv_enc.db"
#define WAL  "/tmp/durakv_enc.log"

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n); if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, n, f); fclose(f);
    int found = 0; size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= got; i++)
        if (memcmp(buf + i, needle, nl) == 0) { found = 1; break; }
    free(buf);
    return found;
}

int main(void)
{
    unlink(DATA); unlink(WAL);
    const char *PASS   = "master-passphrase";
    const char *SECRET = "TOP-SECRET-BALANCE-999999";

    /* write into an encrypted store */
    DB *db = db_open_secure(DATA, WAL, 64, POLICY_LRU, PASS);
    assert(db);
    assert(db_set(db, "acct:balance", SECRET, (uint32_t)strlen(SECRET)) == DK_OK);
    assert(db_set(db, "acct:owner",   "bishowdip", 9) == DK_OK);
    db_close(db);

    printf("1. wrote secrets to an encrypted store\n");
    int in_db  = file_contains(DATA, SECRET);
    int in_wal = file_contains(WAL,  SECRET);
    printf("   plaintext secret in data.db?  %s\n", in_db  ? "YES (bad!)" : "no");
    printf("   plaintext secret in wal.log?  %s\n", in_wal ? "YES (bad!)" : "no");
    assert(!in_db && !in_wal);

    /* correct password recovers it */
    db = db_open_secure(DATA, WAL, 64, POLICY_LRU, PASS);
    assert(db);
    char buf[64]; uint32_t vl = 0;
    assert(db_get(db, "acct:balance", buf, sizeof(buf), &vl) == DK_OK);
    assert(vl == strlen(SECRET) && memcmp(buf, SECRET, vl) == 0);
    printf("2. correct password -> recovered: %.*s\n", (int)vl, buf);
    db_close(db);

    /* wrong password -> no access, no corruption */
    db = db_open_secure(DATA, WAL, 64, POLICY_LRU, "wrong-password");
    assert(db);
    int rc = db_get(db, "acct:balance", buf, sizeof(buf), &vl);
    printf("3. wrong password   -> get returns %s\n",
           rc == DK_OK ? "data?!" : "NOTFOUND (inaccessible)");
    assert(rc == DK_NOTFOUND);
    db_close(db);

    /* correct password STILL works (wrong-password open did not corrupt) */
    db = db_open_secure(DATA, WAL, 64, POLICY_LRU, PASS);
    assert(db);
    assert(db_get(db, "acct:balance", buf, sizeof(buf), &vl) == DK_OK && vl == strlen(SECRET));
    db_close(db);

    unlink(DATA); unlink(WAL);
    printf("\ndemo_encrypt: PASS (data + WAL encrypted at rest; wrong key locked out)\n");
    return 0;
}
