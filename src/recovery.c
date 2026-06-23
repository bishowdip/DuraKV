/*
 * recovery.c -- Simplified ARIES crash recovery: analysis, redo, undo.
 * See include/recovery.h for the algorithm description.
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

    /* ---- Redo: re-apply committed after-images, idempotently ----------- */
    for (size_t i = start; i < n; i++) {
        WalRecord *r = &recs[i];
        if (r->type != WAL_UPDATE) continue;
        if (!committed(commit_set, ncommit, r->txn_id)) continue;
        page_read(db, r->page_id, page);
        if (page_get_lsn(page) < r->lsn) {       /* the idempotency check */
            memcpy(page, r->after, r->after_len);
            page_write(db, r->page_id, page);
        }
    }

    /* ---- Undo: roll back losers in reverse, restoring before-images ---- */
    for (size_t i = n; i-- > start; ) {
        WalRecord *r = &recs[i];
        if (r->type != WAL_UPDATE) continue;
        if (committed(commit_set, ncommit, r->txn_id)) continue;
        if (r->page_id >= db->page_count) continue;   /* never materialised */
        page_read(db, r->page_id, page);
        memcpy(page, r->before, r->before_len);
        page_write(db, r->page_id, page);
    }

    fsync(db->data_fd);          /* recovered state is now durable */

    db->next_lsn = max_lsn + 1;
    db->next_txn = max_txn + 1;

    free(commit_set);
    wal_records_free(recs, n);
    return DK_OK;
}
