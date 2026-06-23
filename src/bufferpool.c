/*
 * bufferpool.c -- frame table, page-fault handling, pin/unpin, dirty bits,
 * victim selection and write-back. See include/bufferpool.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "bufferpool.h"
#include "storage.h"        /* PAGE_SIZE, page_init */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

typedef struct {
    uint64_t page_id;
    int      valid;     /* frame holds a page                */
    int      dirty;     /* page modified since load           */
    int      pin;       /* pin count (>0 => not evictable)    */
    uint8_t  data[PAGE_SIZE];
} Frame;

struct BufferPool {
    int        fd;
    size_t     nframes;
    Frame     *frames;
    Replacer  *repl;
    BPStats    st;
    pthread_mutex_t mtx;   /* protects frame table, replacer, stats */
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

/* Read a page from disk into a frame; pages past EOF come back empty/valid. */
static void load_from_disk(BufferPool *bp, uint64_t page_id, uint8_t *dst)
{
    ssize_t n = pread(bp->fd, dst, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    if (n < (ssize_t)PAGE_SIZE) page_init(dst, page_id);
}

static void writeback(BufferPool *bp, Frame *f)
{
    if (!f->valid || !f->dirty) return;
    ssize_t n = pwrite(bp->fd, f->data, PAGE_SIZE, (off_t)f->page_id * PAGE_SIZE);
    if (n != (ssize_t)PAGE_SIZE) perror("bufferpool writeback");
    f->dirty = 0;
    bp->st.writebacks++;
}

static Frame *find_resident(BufferPool *bp, uint64_t page_id, size_t *idx)
{
    for (size_t i = 0; i < bp->nframes; i++)
        if (bp->frames[i].valid && bp->frames[i].page_id == page_id) {
            if (idx) *idx = i;
            return &bp->frames[i];
        }
    return NULL;
}

uint8_t *bp_pin(BufferPool *bp, uint64_t page_id)
{
    pthread_mutex_lock(&bp->mtx);
    bp->st.accesses++;

    size_t idx;
    Frame *f = find_resident(bp, page_id, &idx);
    if (f) {                                   /* ---- hit ---- */
        bp->st.hits++;
        f->pin++;
        replacer_note_access(bp->repl, idx);
        pthread_mutex_unlock(&bp->mtx);
        return f->data;
    }

    /* ---- page fault: find a frame to hold the page ---- */
    bp->st.faults++;

    /* prefer a free frame */
    size_t target = bp->nframes;
    for (size_t i = 0; i < bp->nframes; i++)
        if (!bp->frames[i].valid) { target = i; break; }

    if (target == bp->nframes) {               /* must evict */
        unsigned char *evictable = malloc(bp->nframes);
        for (size_t i = 0; i < bp->nframes; i++)
            evictable[i] = (bp->frames[i].valid && bp->frames[i].pin == 0);
        size_t victim;
        int ok = replacer_victim(bp->repl, evictable, &victim);
        free(evictable);
        if (!ok) { pthread_mutex_unlock(&bp->mtx); return NULL; }  /* all pinned */

        writeback(bp, &bp->frames[victim]);
        replacer_note_free(bp->repl, victim);
        bp->frames[victim].valid = 0;
        bp->st.evictions++;
        target = victim;
    }

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

void bp_unpin(BufferPool *bp, uint64_t page_id, int dirty)
{
    pthread_mutex_lock(&bp->mtx);
    Frame *f = find_resident(bp, page_id, NULL);
    if (f) {
        if (dirty) f->dirty = 1;
        if (f->pin > 0) f->pin--;
    }
    pthread_mutex_unlock(&bp->mtx);
}

void bp_flush_all(BufferPool *bp)
{
    pthread_mutex_lock(&bp->mtx);
    for (size_t i = 0; i < bp->nframes; i++)
        writeback(bp, &bp->frames[i]);
    pthread_mutex_unlock(&bp->mtx);
}

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
