/*
 * recovery.h -- Crash recovery (DuraKV Phase 1).
 *
 * OS/systems primitive: crash recovery (a simplified ARIES).
 *
 * On startup, recovery_run() performs three passes over the WAL:
 *   1. Analysis -- from the last CHECKPOINT, determine which transactions
 *      committed and which were in-flight (losers).
 *   2. Redo     -- re-apply each committed after-image, but only if
 *      page.page_lsn < record.lsn. This comparison makes redo idempotent,
 *      so crashing *during* recovery is itself safe.
 *   3. Undo     -- roll back losers by applying before-images in reverse.
 *
 * It also restores db->next_lsn and db->next_txn from the log.
 */
#ifndef DURAKV_RECOVERY_H
#define DURAKV_RECOVERY_H

#include "storage.h"

/* Run analysis/redo/undo against db's WAL and data file. Returns DK_OK. */
int recovery_run(DB *db);

#endif /* DURAKV_RECOVERY_H */
