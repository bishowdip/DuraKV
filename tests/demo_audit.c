/*
 * demo_audit.c -- tamper-evident audit logging (Task 3).
 *
 * Appends a few events, verifies the hash chain is intact, then edits one
 * byte of a past entry directly on disk and shows that audit_verify() detects
 * the break and points to the offending entry. This is the property that makes
 * the log trustworthy: you cannot quietly rewrite history.
 */
#include "audit.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH "/tmp/durakv_audit.log"

/* flip one character of the first occurrence of `needle` in the file */
static void tamper(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f); buf[sz] = '\0'; fclose(f);

    char *p = strstr(buf, needle);
    if (p) p[strlen(needle) - 1] ^= 0x1e;   /* change a byte, keep length */

    f = fopen(path, "wb");
    fwrite(buf, 1, sz, f); fclose(f);
    free(buf);
}

int main(void)
{
    unlink(PATH);

    Audit *a = audit_open(PATH);
    assert(a);
    audit_append(a, "alice", "SET", "balance", "OK");
    audit_append(a, "bob",   "GET", "balance", "DENIED");
    audit_append(a, "alice", "DEL", "session", "OK");
    audit_close(a);

    printf("appended 3 audit entries to %s\n\n", PATH);

    long bad = -999;
    int rc = audit_verify(PATH, &bad);
    printf("verify (untouched) : %s\n", rc == 0 ? "INTACT" : "broken");
    assert(rc == 0);

    printf("\ntampering with entry 1 (editing the 'balance' key on disk)...\n");
    tamper(PATH, "balance");

    rc = audit_verify(PATH, &bad);
    printf("verify (tampered)  : %s, first broken entry = #%ld\n",
           rc == 0 ? "INTACT?!" : "BROKEN", bad);
    assert(rc == -1 && bad == 1);

    unlink(PATH);
    printf("\ndemo_audit: PASS (tampering detected by the hash chain)\n");
    return 0;
}
