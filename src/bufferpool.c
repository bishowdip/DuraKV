/*
 * bufferpool.c -- an in-RAM page cache (Task 2: virtual-memory/paging).
 *
 * The buffer pool sits between the store and the disk file. A fixed number of
 * `nframes` frames each hold one PAGE_SIZE page. Callers ask for a page by id
 * via bp_pin(); if it is already resident that is a HIT, otherwise it is a page
 * FAULT and the pool loads it from disk -- evicting another page first if every
 * frame is occupied. This is the software analogue of an OS/MMU page cache, and
 * the hit/miss counters here are what test_bufferpool reports.
 *
 * Two design choices carry the durability and memory stories:
 *
 *  1. WRITE-BACK, not write-through. A modified (dirty) page is NOT written to
 *     disk immediately; it is flushed only on eviction, bp_flush_all(), or
 *     close. This is faster (many updates to a hot page cost one disk write)
 *     and, crucially, means dirty pages live only in this process's RAM -- so a
 *     kill -9 genuinely loses them, which is what makes the WAL redo pass a real
 *     recovery test rather than one masked by the OS page cache.
 *
 *  2. A pluggable I/O hook (bp_set_io). By default the pool reads/writes raw
 *     pages through `fd`. The storage engine installs callbacks so that every
 *     page passes through one encryption choke point (encryption-at-rest),
 *     keeping storage.c/recovery.c free of crypto code.
 *
 * PINNING prevents eviction of a page currently in use: bp_pin increments a pin
 * count and returns the frame's buffer; the page cannot be chosen as a victim
 * until the matching bp_unpin drops the count to 0. One mutex serialises the
 * whole table so concurrent threads can fault pages safely. See
 * include/bufferpool.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "bufferpool.h"
#include "storage.h"        /* PAGE_SIZE, page_init */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

/* One cache frame: metadata plus the page's bytes. */
typedef struct {
    uint64_t page_id;
    int      valid;     /* frame holds a page                       */
    int      dirty;     /* page modified since load (needs writeback)*/
    int      pin;       /* pin count (>0 => in use, not evictable)  */
    uint8_t  data[PAGE_SIZE];
} Frame;

struct BufferPool {
    int        fd;         /* backing file (raw path)                       */
    size_t     nframes;    /* fixed number of frames = the "memory" size    */
    Frame     *frames;
    Replacer  *repl;       /* eviction policy (FIFO/LRU), see replacement.c */
    BPStats    st;         /* accesses/hits/faults/evictions/writebacks     */
    pthread_mutex_t mtx;   /* protects frame table, replacer AND stats      */

    void        *io_ctx;   /* optional caller-supplied page I/O (else use fd) */
    bp_read_fn   io_read;  /* encryption-aware read hook, or NULL             */
    bp_write_fn  io_write; /* encryption-aware write hook, or NULL            */
};

BufferPool *bp_create(int fd, size_t nframes, PolicyKind policy)
{
    if (nframes == 0) nframes = 1;
    BufferPool *bp = calloc(1, sizeof(*bp));
    bp->fd      = fd;
    bp->nframes = nframes;
    bp->frames  = calloc(nframes, sizeof(Frame));
    bp->repl    = replacer_create(policy, nframes);
    pthread_mutex_init(&bp->mtx, NULL);
    return bp;
}

void bp_destroy(BufferPool *bp)
{
    if (!bp) return;
    pthread_mutex_destroy(&bp->mtx);
    replacer_destroy(bp->repl);
    free(bp->frames);
    free(bp);
}

/* Read a page from disk into a frame; pages past EOF come back empty/valid.
 * If a custom I/O callback is installed (the storage engine's encryption-aware
 * path), use it; otherwise read the raw page directly. */
static void load_from_disk(BufferPool *bp, uint64_t page_id, uint8_t *dst)
{
    if (bp->io_read) { bp->io_read(bp->io_ctx, page_id, dst); return; }
    ssize_t n = pread(bp->fd, dst, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    if (n < (ssize_t)PAGE_SIZE) page_init(dst, page_id);
}

/* Flush one frame to disk if (and only if) it holds unsaved changes. Clearing
 * the dirty bit afterwards makes repeated flushes cheap and idempotent. */
static void writeback(BufferPool *bp, Frame *f)
{
    if (!f->valid || !f->dirty) return;
    if (bp->io_write) {
        bp->io_write(bp->io_ctx, f->page_id, f->data);
    } else {
        ssize_t n = pwrite(bp->fd, f->data, PAGE_SIZE, (off_t)f->page_id * PAGE_SIZE);
        if (n != (ssize_t)PAGE_SIZE) perror("bufferpool writeback");
    }
    f->dirty = 0;
    bp->st.writebacks++;
}

/* Install the encryption-aware page I/O callbacks (the single choke point that
 * keeps sealing/unsealing out of the pool and the storage engine). */
void bp_set_io(BufferPool *bp, void *ctx, bp_read_fn rd, bp_write_fn wr)
{
    bp->io_ctx = ctx; bp->io_read = rd; bp->io_write = wr;
}

/* Linear scan for a resident copy of `page_id`. O(nframes); fine for the small
 * pools used here. A production system would index this with a hash map. */
static Frame *find_resident(BufferPool *bp, uint64_t page_id, size_t *idx)
{
    for (size_t i = 0; i < bp->nframes; i++)
        if (bp->frames[i].valid && bp->frames[i].page_id == page_id) {
            if (idx) *idx = i;
            return &bp->frames[i];
        }
    return NULL;
}

/*
 * Pin a page and return a pointer to its bytes. This is the pool's core: it
 * resolves a page id to a resident frame, faulting it in on a miss and evicting
 * a victim if the pool is full. The returned buffer stays valid until the
 * caller bp_unpin()s it. Returns NULL only if every frame is pinned (no victim
 * available).
 */
uint8_t *bp_pin(BufferPool *bp, uint64_t page_id)
{
    pthread_mutex_lock(&bp->mtx);
    bp->st.accesses++;

    size_t idx;
    Frame *f = find_resident(bp, page_id, &idx);
    if (f) {                                   /* ---- HIT: page already cached ---- */
        bp->st.hits++;
        f->pin++;
        replacer_note_access(bp->repl, idx);   /* LRU counts this as a recent use */
        pthread_mutex_unlock(&bp->mtx);
        return f->data;
    }

    /* ---- PAGE FAULT: the page is not resident, we must find a frame ---- */
    bp->st.faults++;

    /* Prefer an empty frame (the pool is still warming up). */
    size_t target = bp->nframes;
    for (size_t i = 0; i < bp->nframes; i++)
        if (!bp->frames[i].valid) { target = i; break; }

    if (target == bp->nframes) {               /* pool full -> must EVICT */
        /* Build the evictable set: resident AND unpinned. The replacement
         * policy then picks the victim among these (oldest/least-recent). */
        unsigned char *evictable = malloc(bp->nframes);
        for (size_t i = 0; i < bp->nframes; i++)
            evictable[i] = (bp->frames[i].valid && bp->frames[i].pin == 0);
        size_t victim;
        int ok = replacer_victim(bp->repl, evictable, &victim);
        free(evictable);
        if (!ok) { pthread_mutex_unlock(&bp->mtx); return NULL; }  /* all pinned */

        writeback(bp, &bp->frames[victim]);    /* persist victim if dirty */
        replacer_note_free(bp->repl, victim);
        bp->frames[victim].valid = 0;
        bp->st.evictions++;
        target = victim;
    }

    /* Load the requested page into the chosen frame and pin it. */
    Frame *nf = &bp->frames[target];
    load_from_disk(bp, page_id, nf->data);
    nf->page_id = page_id;
    nf->valid   = 1;
    nf->dirty   = 0;
    nf->pin     = 1;
    replacer_note_load(bp->repl, target);
    pthread_mutex_unlock(&bp->mtx);
    return nf->data;
}

/* Release a pin. Pass dirty=1 if the caller modified the page, so it will be
 * written back before eviction (write-back). The page only becomes eligible for
 * eviction once its pin count returns to 0. */
void bp_unpin(BufferPool *bp, uint64_t page_id, int dirty)
{
    pthread_mutex_lock(&bp->mtx);
    Frame *f = find_resident(bp, page_id, NULL);
    if (f) {
        if (dirty) f->dirty = 1;               /* sticky: never clear on unpin */
        if (f->pin > 0) f->pin--;
    }
    pthread_mutex_unlock(&bp->mtx);
}

/* Write every dirty frame to disk (used at checkpoint/close). */
void bp_flush_all(BufferPool *bp)
{
    pthread_mutex_lock(&bp->mtx);
    for (size_t i = 0; i < bp->nframes; i++)
        writeback(bp, &bp->frames[i]);
    pthread_mutex_unlock(&bp->mtx);
}

/* Snapshot the counters. The parameter is const for callers, but we must cast
 * it away to take the mutex (locking mutates the lock) -- reading shared stats
 * without the lock would be a data race. */
BPStats bp_stats(const BufferPool *bp)
{
    BufferPool *m = (BufferPool *)bp;
    pthread_mutex_lock(&m->mtx);
    BPStats s = bp->st;
    pthread_mutex_unlock(&m->mtx);
    return s;
}

double bp_hit_ratio(const BufferPool *bp)
{
    BPStats s = bp_stats(bp);
    return s.accesses ? (double)s.hits / (double)s.accesses : 0.0;
}

void bp_reset_stats(BufferPool *bp)
{
    pthread_mutex_lock(&bp->mtx);
    memset(&bp->st, 0, sizeof(bp->st));
    pthread_mutex_unlock(&bp->mtx);
}

const char *bp_policy_name(const BufferPool *bp) { return replacer_name(bp->repl); }
size_t      bp_nframes(const BufferPool *bp)     { return bp->nframes; }
