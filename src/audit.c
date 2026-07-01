/*
 * audit.c -- a tamper-EVIDENT audit log (Task 3), recording every security-
 * relevant action (who did what to which key, and whether it was allowed).
 *
 * The log is append-only, but append-only alone does not stop someone with file
 * access from editing or deleting a past line. The defence is a HASH CHAIN
 * (the same idea a blockchain uses): each entry stores
 *      hash = SHA-256( previous_entry_hash || this_entry's_fields )
 * so every entry cryptographically commits to the entire history before it.
 * Altering, reordering, or removing any past entry changes its hash, which no
 * longer matches the `prev` recorded in the next entry -- so audit_verify()
 * detects the break and reports the exact sequence number where it occurs. The
 * chain starts from a fixed all-zero "genesis" hash.
 *
 * This makes the log tamper-EVIDENT, not tamper-PROOF: an attacker who can
 * rewrite the whole file could recompute every hash forward. Defeating that
 * needs an external anchor (e.g. periodically signing or off-siting the tip) --
 * noted as a limitation, not implemented here. See include/audit.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "audit.h"

#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HASHLEN crypto_hash_sha256_BYTES   /* 32 */

struct Audit {
    FILE   *fp;
    long    seq;                  /* sequence number of the last entry      */
    uint8_t prev[HASHLEN];        /* hash of the last entry (chain tip)      */
};

static void to_hex(char *dst, const uint8_t *src, size_t n)
{
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { dst[2*i] = h[src[i] >> 4]; dst[2*i+1] = h[src[i] & 15]; }
    dst[2*n] = '\0';
}

/* hash = SHA-256( prev_hash || "seq|ts|user|op|key|result" ) */
static void chain_hash(uint8_t out[HASHLEN], const uint8_t prev[HASHLEN],
                       const char *fields)
{
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, prev, HASHLEN);
    crypto_hash_sha256_update(&st, (const uint8_t *)fields, strlen(fields));
    crypto_hash_sha256_final(&st, out);
}

/* Open (or create) an audit log for appending. If the file already exists, the
 * chain tip -- the last sequence number and last hash -- is recovered by
 * scanning it, so new entries continue the existing chain rather than starting
 * a fresh one. */
Audit *audit_open(const char *path)
{
    if (sodium_init() < 0) return NULL;

    Audit *a = calloc(1, sizeof(*a));
    a->seq = 0;
    memset(a->prev, 0, HASHLEN);          /* genesis */

    /* recover the chain tip (last seq + last hash) from any existing log */
    FILE *r = fopen(path, "r");
    if (r) {
        char *line = NULL; size_t cap = 0; ssize_t n;
        while ((n = getline(&line, &cap, r)) > 0) {
            char *fields[8] = {0}, *save = NULL;
            char *tok = strtok_r(line, "\t", &save);
            int i = 0;
            while (tok && i < 8) { fields[i++] = tok; tok = strtok_r(NULL, "\t", &save); }
            if (i == 8) {
                a->seq = atol(fields[0]);
                char *hx = fields[7];
                for (int b = 0; b < (int)HASHLEN; b++)
                    sscanf(hx + 2*b, "%2hhx", &a->prev[b]);
            }
        }
        free(line);
        fclose(r);
    }

    a->fp = fopen(path, "a");
    if (!a->fp) { free(a); return NULL; }
    return a;
}

void audit_close(Audit *a)
{
    if (!a) return;
    if (a->fp) fclose(a->fp);
    free(a);
}

/* Append one entry, linking it into the hash chain and flushing to disk so the
 * record survives a crash immediately after the action. Returns 0 on success. */
int audit_append(Audit *a, const char *user, const char *op,
                 const char *key, const char *result)
{
    long seq = a->seq + 1;
    long ts  = (long)time(NULL);

    char fields[512];
    snprintf(fields, sizeof(fields), "%ld|%ld|%s|%s|%s|%s",
             seq, ts, user, op, key, result);

    uint8_t hash[HASHLEN];
    chain_hash(hash, a->prev, fields);

    char prev_hex[2*HASHLEN+1], hash_hex[2*HASHLEN+1];
    to_hex(prev_hex, a->prev, HASHLEN);
    to_hex(hash_hex, hash, HASHLEN);

    if (fprintf(a->fp, "%ld\t%ld\t%s\t%s\t%s\t%s\t%s\t%s\n",
                seq, ts, user, op, key, result, prev_hex, hash_hex) < 0) return -1;
    fflush(a->fp);

    a->seq = seq;
    memcpy(a->prev, hash, HASHLEN);
    return 0;
}

/* Re-walk the whole log from genesis, recomputing each entry's hash and
 * checking it against the stored `prev`/hash fields. Returns 0 if the chain is
 * intact, or -1 and sets *bad_seq to the sequence number of the first broken
 * entry -- pinpointing exactly where tampering occurred. */
int audit_verify(const char *path, long *bad_seq)
{
    FILE *r = fopen(path, "r");
    if (!r) { if (bad_seq) *bad_seq = -1; return -1; }

    uint8_t prev[HASHLEN];
    memset(prev, 0, HASHLEN);              /* genesis */
    char *line = NULL; size_t cap = 0; ssize_t n;
    long count = 0;
    int rc = 0;

    while ((n = getline(&line, &cap, r)) > 0) {
        if (line[n-1] == '\n') line[n-1] = '\0';
        /* split off the two hex fields, keep the rest for re-hashing */
        char *copy = strdup(line);
        char *fields[8] = {0}, *save = NULL;
        char *tok = strtok_r(copy, "\t", &save);
        int i = 0;
        while (tok && i < 8) { fields[i++] = tok; tok = strtok_r(NULL, "\t", &save); }
        if (i != 8) { rc = -1; if (bad_seq) *bad_seq = count; free(copy); break; }

        long seq = atol(fields[0]);
        char recomputed_fields[512];
        snprintf(recomputed_fields, sizeof(recomputed_fields), "%s|%s|%s|%s|%s|%s",
                 fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]);

        uint8_t want[HASHLEN]; char want_hex[2*HASHLEN+1];
        chain_hash(want, prev, recomputed_fields);
        to_hex(want_hex, want, HASHLEN);

        /* stored prev must match running prev, and stored hash must match ours */
        char prev_hex[2*HASHLEN+1];
        to_hex(prev_hex, prev, HASHLEN);
        if (strcmp(fields[6], prev_hex) != 0 || strcmp(fields[7], want_hex) != 0) {
            rc = -1; if (bad_seq) *bad_seq = seq; free(copy); break;
        }
        memcpy(prev, want, HASHLEN);       /* advance the chain */
        count++;
        free(copy);
    }

    free(line);
    fclose(r);
    return rc;
}
