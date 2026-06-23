/*
 * wal.h -- Write-ahead log (DuraKV Phase 1).
 *
 * OS/systems primitive: durability via fsync(), write-ahead logging.
 *
 * The WAL is an append-only file of length-prefixed records:
 *
 *     [reclen u32][ body (reclen bytes, including trailing crc32) ]
 *
 * body = lsn u64 | prev_lsn u64 | txn_id u64 | type u8 | page_id u64
 *        | before_len u32 | before[] | after_len u32 | after[] | crc32 u32
 *
 * Phase 1 uses *page-level* logging: before/after images are full page
 * snapshots. This makes redo idempotent (compare page_lsn) and transparently
 * repairs torn page writes.
 *
 * The write-ahead invariant: a dirty data page is never written to data.db
 * until the WAL has been fsync()'d up to that page's last log record. We
 * satisfy it the simple way -- every transaction fsyncs the whole WAL (after
 * its COMMIT record) *before* any of its data pages are written back.
 */
#ifndef DURAKV_WAL_H
#define DURAKV_WAL_H

#include <stdint.h>
#include "storage.h"

typedef enum {
    WAL_BEGIN      = 1,
    WAL_UPDATE     = 2,
    WAL_COMMIT     = 3,
    WAL_ABORT      = 4,
    WAL_CHECKPOINT = 5
} WalType;

/* A decoded WAL record (as produced by the recovery scanner). */
typedef struct {
    uint64_t  lsn;
    uint64_t  prev_lsn;
    uint64_t  txn_id;
    uint8_t   type;
    uint64_t  page_id;
    uint8_t  *before;    /* malloc'd, before_len bytes (may be NULL) */
    uint32_t  before_len;
    uint8_t  *after;     /* malloc'd, after_len bytes  (may be NULL) */
    uint32_t  after_len;
} WalRecord;

/* Append one record; assigns and returns its LSN. before/after may be NULL. */
uint64_t wal_append(DB *db, WalType type, uint64_t txn_id, uint64_t prev_lsn,
                    uint64_t page_id,
                    const uint8_t *before, uint32_t before_len,
                    const uint8_t *after,  uint32_t after_len);

/* Force all appended records to stable storage (the durability point). */
void wal_fsync(DB *db);

/*
 * Read the entire WAL into an array of decoded records. Stops cleanly at the
 * first truncated or crc-invalid record (that is the crash point). Caller
 * frees with wal_records_free(). Returns record count, *out set to the array.
 */
size_t wal_scan(DB *db, WalRecord **out);
void   wal_records_free(WalRecord *recs, size_t n);

/* CRC32 (IEEE 802.3, poly 0xEDB88420) over buf[0..len). */
uint32_t wal_crc32(const uint8_t *buf, size_t len);

#endif /* DURAKV_WAL_H */
