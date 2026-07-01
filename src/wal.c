/*
 * wal.c -- the write-ahead log (WAL): the source of DuraKV's crash safety.
 *
 * The write-ahead PRINCIPLE: before any change is applied to the data file, a
 * record describing that change is appended here and forced to stable storage.
 * On COMMIT the WAL is fsync'd BEFORE the client is told "OK", so every
 * acknowledged write is already durable. Data pages themselves may still be
 * only in RAM/OS cache at that moment -- that is safe, because recovery can
 * reconstruct them from the log (see recovery.c). This is the write-ahead
 * invariant: log-then-data, never data-then-log.
 *
 * Record layout (little-endian), all framed by a length prefix:
 *   [u32 reclen] [u64 lsn][u64 prev_lsn][u64 txn_id][u8 type][u64 page_id]
 *   [u32 before_len][before...][u32 after_len][after...] [u32 crc]
 * Each record carries BOTH the before-image (for undo) and after-image (for
 * redo) of a page, which also lets recovery repair a torn page write.
 *
 * Two integrity mechanisms make a half-written tail (the normal result of a
 * crash mid-append) detectable and harmless: the length prefix bounds the read,
 * and a CRC32 over the body is verified on scan -- a partial or corrupt final
 * record fails one of these checks and is simply discarded, so recovery replays
 * only whole, intact records. See include/wal.h for the public API.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE   /* expose F_FULLFSYNC from <fcntl.h> */
#endif
#include "wal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* ---- CRC32 (IEEE 802.3, reflected, poly 0xEDB88420) -------------------- */
/* Detects torn/corrupt records. Bit-at-a-time (no lookup table) -- the WAL is
 * fsync-bound, so CRC speed is not the bottleneck. */
uint32_t wal_crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88420u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---- helpers ----------------------------------------------------------- */

static void put_u32(uint8_t **p, uint32_t v) { memcpy(*p, &v, 4); *p += 4; }
static void put_u64(uint8_t **p, uint64_t v) { memcpy(*p, &v, 8); *p += 8; }
static void put_u8 (uint8_t **p, uint8_t  v) { **p = v; *p += 1; }

/* ---- append ------------------------------------------------------------ */

/*
 * Append one record and return its LSN (log sequence number -- a monotonically
 * increasing id used to order records and to make redo idempotent). This only
 * writes; durability is a separate wal_fsync() so a transaction can batch many
 * appends behind a single flush at commit.
 */
uint64_t wal_append(DB *db, WalType type, uint64_t txn_id, uint64_t prev_lsn,
                    uint64_t page_id,
                    const uint8_t *before, uint32_t before_len,
                    const uint8_t *after,  uint32_t after_len)
{
    uint64_t lsn = db->next_lsn++;

    /* body = lsn|prev|txn|type|page|blen|before|alen|after|crc */
    size_t body = 8 + 8 + 8 + 1 + 8 + 4 + before_len + 4 + after_len + 4;
    uint8_t *buf = malloc(4 + body);
    uint8_t *p   = buf;

    put_u32(&p, (uint32_t)body);     /* reclen prefix (excludes itself)     */
    uint8_t *body_start = p;

    put_u64(&p, lsn);
    put_u64(&p, prev_lsn);
    put_u64(&p, txn_id);
    put_u8 (&p, (uint8_t)type);
    put_u64(&p, page_id);
    put_u32(&p, before_len);
    if (before_len) { memcpy(p, before, before_len); p += before_len; }
    put_u32(&p, after_len);
    if (after_len)  { memcpy(p, after,  after_len);  p += after_len;  }

    uint32_t crc = wal_crc32(body_start, (size_t)(p - body_start));
    put_u32(&p, crc);

    /* O_APPEND makes this write land atomically at end-of-file. */
    ssize_t n = write(db->wal_fd, buf, 4 + body);
    if (n != (ssize_t)(4 + body)) perror("wal write");
    free(buf);
    return lsn;
}

void wal_fsync(DB *db)
{
#if defined(__APPLE__)
    /* fsync() on macOS does not flush the drive's own cache; F_FULLFSYNC
     * does. Fall back to fsync() if the device rejects it. */
    if (fcntl(db->wal_fd, F_FULLFSYNC) == -1) fsync(db->wal_fd);
#else
    fdatasync(db->wal_fd);
#endif
}

/* ---- scan -------------------------------------------------------------- */

static int read_full(int fd, off_t off, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(fd, buf + got, len - got, off + (off_t)got);
        if (n <= 0) return -1;       /* EOF or error -> short/truncated */
        got += (size_t)n;
    }
    return 0;
}

/*
 * Read the whole log from the start into an array of parsed records. Stops at
 * the first sign of an incomplete/corrupt record -- a clean EOF, an impossibly
 * small length, a short read (torn tail), or a CRC mismatch -- and returns only
 * the records before it. This is precisely the crash-tolerance guarantee: a
 * record half-written when the power failed is never replayed. Caller frees the
 * result with wal_records_free().
 */
size_t wal_scan(DB *db, WalRecord **out)
{
    size_t cap = 64, n = 0;
    WalRecord *recs = malloc(cap * sizeof(*recs));
    off_t off = 0;

    for (;;) {
        uint8_t lenbuf[4];
        if (read_full(db->wal_fd, off, lenbuf, 4) != 0) break;   /* clean EOF */
        uint32_t body;
        memcpy(&body, lenbuf, 4);
        if (body < 8 + 8 + 8 + 1 + 8 + 4 + 4 + 4) break;         /* nonsense */

        uint8_t *bodybuf = malloc(body);
        if (read_full(db->wal_fd, off + 4, bodybuf, body) != 0) {
            free(bodybuf); break;                                /* torn tail */
        }

        /* verify crc over everything except the trailing 4-byte crc */
        uint32_t want;
        memcpy(&want, bodybuf + body - 4, 4);
        if (wal_crc32(bodybuf, body - 4) != want) { free(bodybuf); break; }

        uint8_t *p = bodybuf;
        WalRecord r; memset(&r, 0, sizeof(r));
        memcpy(&r.lsn, p, 8);       p += 8;
        memcpy(&r.prev_lsn, p, 8);  p += 8;
        memcpy(&r.txn_id, p, 8);    p += 8;
        r.type = *p;                p += 1;
        memcpy(&r.page_id, p, 8);   p += 8;
        memcpy(&r.before_len, p, 4); p += 4;
        if (r.before_len) { r.before = malloc(r.before_len);
                            memcpy(r.before, p, r.before_len); p += r.before_len; }
        memcpy(&r.after_len, p, 4);  p += 4;
        if (r.after_len)  { r.after = malloc(r.after_len);
                            memcpy(r.after, p, r.after_len);  p += r.after_len; }
        free(bodybuf);

        if (n == cap) { cap *= 2; recs = realloc(recs, cap * sizeof(*recs)); }
        recs[n++] = r;
        off += 4 + body;
    }

    *out = recs;
    return n;
}

void wal_records_free(WalRecord *recs, size_t n)
{
    for (size_t i = 0; i < n; i++) { free(recs[i].before); free(recs[i].after); }
    free(recs);
}
