/*
 * bufferpool.h -- In-RAM page cache over the data file (DuraKV Phase 2).
 *
 * OS/systems primitive: paging / virtual-memory buffer management.
 *
 * A fixed set of frames caches pages from data.db. Callers pin a page (a page
 * fault loads it, possibly evicting a victim chosen by the replacement policy)
 * and unpin it when done, declaring whether they dirtied it. Dirty frames are
 * written back lazily -- on eviction, or in bulk via bp_flush_all().
 *
 * Write-back + the WAL together honour the write-ahead invariant: because the
 * storage engine only dirties a page *after* its commit has been fsync'd to
 * the WAL, any page the pool later flushes already has a durable log record.
 * (And because dirty pages live only in this process's RAM, a kill -9 really
 * does lose them -- so recovery's redo pass is exercised for real.)
 */
#ifndef DURAKV_BUFFERPOOL_H
#define DURAKV_BUFFERPOOL_H

#include <stdint.h>
#include <stddef.h>
#include "replacement.h"

typedef struct BufferPool BufferPool;

typedef struct {
    uint64_t accesses;     /* total bp_pin calls           */
    uint64_t hits;         /* page already resident         */
    uint64_t faults;       /* page had to be loaded         */
    uint64_t evictions;    /* a resident page was replaced  */
    uint64_t writebacks;   /* a dirty victim was flushed    */
} BPStats;

BufferPool *bp_create(int fd, size_t nframes, PolicyKind policy);
void        bp_destroy(BufferPool *bp);   /* frees frames; does NOT fsync fd */

/* Optional: route page I/O through caller-supplied callbacks instead of the
 * pool's own pread/pwrite. The storage engine uses this so that a single,
 * encryption-aware code path performs every data-file transfer. Each callback
 * transfers one logical PAGE_SIZE page; return 0 on success. */
typedef int (*bp_read_fn)(void *ctx, uint64_t page_id, uint8_t *out);
typedef int (*bp_write_fn)(void *ctx, uint64_t page_id, const uint8_t *in);
void bp_set_io(BufferPool *bp, void *ctx, bp_read_fn rd, bp_write_fn wr);

/* Pin page_id into a frame and return its PAGE_SIZE buffer (NULL if every
 * frame is pinned). Modify it in place, then bp_unpin with dirty=1. */
uint8_t    *bp_pin(BufferPool *bp, uint64_t page_id);
void        bp_unpin(BufferPool *bp, uint64_t page_id, int dirty);

void        bp_flush_all(BufferPool *bp);  /* write every dirty frame to fd */

BPStats     bp_stats(const BufferPool *bp);
double      bp_hit_ratio(const BufferPool *bp);
void        bp_reset_stats(BufferPool *bp);
const char *bp_policy_name(const BufferPool *bp);
size_t      bp_nframes(const BufferPool *bp);

#endif /* DURAKV_BUFFERPOOL_H */
