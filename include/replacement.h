/*
 * replacement.h -- Pluggable page-replacement policies (DuraKV Phase 2).
 *
 * OS/systems primitive: page-replacement algorithms (FIFO, LRU).
 *
 * The buffer pool delegates victim selection to a Replacer. Each policy is a
 * small vtable of function pointers, so adding CLOCK or LRU-K later is just
 * another Policy instance -- nothing in the buffer pool changes.
 *
 * Both shipped policies are driven by a per-frame "stamp" (a logical clock):
 *   - FIFO stamps a frame only when a page is loaded into it; the victim is
 *     the oldest stamp. Accesses are ignored -- so FIFO can suffer Belady's
 *     anomaly (more frames -> more faults; see tests/test_belady.c).
 *   - LRU additionally re-stamps a frame on every access; the victim is the
 *     least-recently-used. LRU is a stack algorithm and cannot suffer Belady.
 */
#ifndef DURAKV_REPLACEMENT_H
#define DURAKV_REPLACEMENT_H

#include <stddef.h>

typedef enum {
    POLICY_FIFO = 0,
    POLICY_LRU  = 1
} PolicyKind;

typedef struct Replacer Replacer;

Replacer   *replacer_create(PolicyKind kind, size_t nframes);
void        replacer_destroy(Replacer *r);
const char *replacer_name(const Replacer *r);

/* lifecycle hooks the buffer pool calls */
void replacer_note_load(Replacer *r, size_t frame);    /* page loaded into frame */
void replacer_note_access(Replacer *r, size_t frame);  /* page referenced (a hit) */
void replacer_note_free(Replacer *r, size_t frame);    /* frame emptied */

/* Pick a victim among frames whose evictable[f] != 0. Returns 1 and sets
 * *out on success, or 0 if every frame is pinned/empty. */
int  replacer_victim(Replacer *r, const unsigned char *evictable, size_t *out);

#endif /* DURAKV_REPLACEMENT_H */
