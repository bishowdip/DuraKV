/*
 * recovery.c -- crash recovery, a simplified form of the ARIES algorithm.
 *
 * Run once at startup, this brings the data file to a consistent state after a
 * crash by replaying the WAL in three passes:
 *
 *   1. ANALYSIS -- scan forward from the last checkpoint to learn which
 *      transactions COMMITTED (winners) and which were still in flight
 *      (losers), and the highest LSN/txn ids seen.
 *   2. REDO -- re-apply the after-image of every committed update, so all
 *      acknowledged work is present even if its data page never reached disk.
 *   3. UNDO -- restore the before-image of every uncommitted update, so a
 *      partially-applied loser transaction leaves no trace ("atomicity").
 *
 * The crucial property is IDEMPOTENCE: redo compares each page's stored
 * page_lsn against the record's lsn and re-applies only when page_lsn < lsn.
 * A page already carrying that change (page_lsn >= lsn) is skipped. This makes
 * recovery safe to crash *during* and re-run from scratch -- exactly why the
 * crashtest can kill the process repeatedly and never lose or double-apply
 * data. Because each record holds a full page image, redo/undo also transparently
 * repair a torn (half-written) page. See include/recovery.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "recovery.h"
#include "wal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Is txn_id in the committed set? (linear scan -- fine for Phase 1.) */
static int committed(const uint64_t *set, size_t n, uint64_t txn)
{
    for (size_t i = 0; i < n; i++) if (set[i] == txn) return 1;
    return 0;
}

/* Recover a PAGE_SIZE plaintext image from a WAL image that may be sealed.
 * Returns 1 on success (img filled), 0 if it cannot be used (wrong key/corrupt
 * or unexpected length). */
static int wal_image(DB *db, const uint8_t *src, uint32_t len, uint8_t *img)
{
    if (db->codec) {
        long m = db->codec->open(db->codec->ctx, src, len, img);
        return m == (long)PAGE_SIZE;
    }
    if (len != PAGE_SIZE) return 0;
    memcpy(img, src, PAGE_SIZE);
    return 1;
}

int recovery_run(DB *db)
{
    WalRecord *recs;
    size_t n = wal_scan(db, &recs);

    uint64_t max_lsn = 0, max_txn = 0;

    if (n == 0) { wal_records_free(recs, n); return DK_OK; }

    /* Start from the last CHECKPOINT: pages were durable as of that point,
     * so earlier records need not be redone (this bounds recovery time). */
    size_t start = 0;
    for (size_t i = 0; i < n; i++)
        if (recs[i].type == WAL_CHECKPOINT) start = i + 1;

    /* ---- Analysis: collect committed txn ids from the active region ---- */
    uint64_t *commit_set = malloc(n * sizeof(uint64_t));
    size_t    ncommit = 0;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].lsn > max_lsn) max_lsn = recs[i].lsn;
        if (recs[i].txn_id > max_txn) max_txn = recs[i].txn_id;
        if (i >= start && recs[i].type == WAL_COMMIT)
            commit_set[ncommit++] = recs[i].txn_id;
    }

    uint8_t page[PAGE_SIZE];
    uint8_t img[PAGE_SIZE];

    /* ---- Redo: re-apply committed after-images, idempotently ----------- */
    for (size_t i = start; i < n; i++) {
        WalRecord *r = &recs[i];
        if (r->type != WAL_UPDATE) continue;
        if (!committed(commit_set, ncommit, r->txn_id)) continue;
        if (!wal_image(db, r->after, r->after_len, img)) continue;   /* wrong key */
        page_read(db, r->page_id, page);
        /* Idempotency check: only apply if the page has not already absorbed
         * this change. Skipping when page_lsn >= r->lsn is what lets recovery
         * be re-run any number of times without corrupting data. */
        if (page_get_lsn(page) < r->lsn) {
            memcpy(page, img, PAGE_SIZE);
            page_write(db, r->page_id, page);
        }
    }

    /* ---- Undo: roll back losers in reverse, restoring before-images ---- */
    for (size_t i = n; i-- > start; ) {
        WalRecord *r = &recs[i];
        if (r->type != WAL_UPDATE) continue;
        if (committed(commit_set, ncommit, r->txn_id)) continue;
        if (r->page_id >= db->page_count) continue;   /* never materialised */
        if (!wal_image(db, r->before, r->before_len, img)) continue;
        page_read(db, r->page_id, page);
        memcpy(page, img, PAGE_SIZE);
        page_write(db, r->page_id, page);
    }

    fsync(db->data_fd);          /* recovered state is now durable */

    db->next_lsn = max_lsn + 1;
    db->next_txn = max_txn + 1;

    free(commit_set);
    wal_records_free(recs, n);
    return DK_OK;
}
