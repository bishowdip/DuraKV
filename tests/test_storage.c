/*
 * test_storage.c -- record round-trips, update/delete semantics, multi-page
 * growth, and persistence across a clean close+reopen.
 */
#include "storage.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define DATA "/tmp/durakv_storage.db"
#define WAL  "/tmp/durakv_storage.log"

static int get_eq(DB *db, const char *k, const char *expect)
{
    char buf[4096]; uint32_t vl = 0;
    int rc = db_get(db, k, buf, sizeof(buf), &vl);
    if (expect == NULL) return rc == DK_NOTFOUND;
    return rc == DK_OK && vl == strlen(expect) && memcmp(buf, expect, vl) == 0;
}

int main(void)
{
    unlink(DATA); unlink(WAL);

    DB *db = db_open(DATA, WAL);
    assert(db);

    /* basic set/get */
    assert(db_set(db, "alpha", "one", 3) == DK_OK);
    assert(db_set(db, "beta",  "two", 3) == DK_OK);
    assert(get_eq(db, "alpha", "one"));
    assert(get_eq(db, "beta",  "two"));
    assert(get_eq(db, "missing", NULL));

    /* update overwrites */
    assert(db_set(db, "alpha", "ONE-UPDATED", 11) == DK_OK);
    assert(get_eq(db, "alpha", "ONE-UPDATED"));

    /* delete */
    assert(db_del(db, "beta") == DK_OK);
    assert(get_eq(db, "beta", NULL));
    assert(db_del(db, "beta") == DK_NOTFOUND);

    /* enough keys to force allocation of several pages */
    for (int i = 0; i < 2000; i++) {
        char k[32], v[64];
        snprintf(k, sizeof(k), "k%d", i);
        snprintf(v, sizeof(v), "value-number-%d-padded-out-a-bit", i);
        assert(db_set(db, k, v, (uint32_t)strlen(v)) == DK_OK);
    }
    assert(db->page_count > 2);          /* genuinely multi-page now */

    db_close(db);

    /* reopen: directory must be rebuilt from disk, all data intact */
    db = db_open(DATA, WAL);
    assert(db);
    assert(get_eq(db, "alpha", "ONE-UPDATED"));
    assert(get_eq(db, "beta", NULL));
    for (int i = 0; i < 2000; i++) {
        char k[32], v[64];
        snprintf(k, sizeof(k), "k%d", i);
        snprintf(v, sizeof(v), "value-number-%d-padded-out-a-bit", i);
        assert(get_eq(db, k, v));
    }
    db_close(db);

    printf("test_storage: PASS\n");
    return 0;
}
