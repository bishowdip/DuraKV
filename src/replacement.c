/*
 * replacement.c -- FIFO and LRU victim selection behind a function-pointer
 * vtable. See include/replacement.h.
 */
#include "replacement.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct Policy {
    const char *name;
    void (*note_load)  (Replacer *, size_t);   /* page loaded into frame */
    void (*note_access)(Replacer *, size_t);   /* page referenced (a hit) */
} Policy;

struct Replacer {
    const Policy *pol;
    size_t        n;
    uint64_t      clock;      /* logical time, advanced on each stamp     */
    uint64_t     *stamp;      /* per-frame stamp; 0 means frame is empty   */
};

/* ---- FIFO: stamp on load only; accesses do not change order ----------- */
static void fifo_load(Replacer *r, size_t f)   { r->stamp[f] = ++r->clock; }
static void fifo_access(Replacer *r, size_t f) { (void)r; (void)f; }

/* ---- LRU: stamp on load and on every access --------------------------- */
static void lru_load(Replacer *r, size_t f)    { r->stamp[f] = ++r->clock; }
static void lru_access(Replacer *r, size_t f)  { r->stamp[f] = ++r->clock; }

static const Policy POLICIES[] = {
    [POLICY_FIFO] = { "FIFO", fifo_load, fifo_access },
    [POLICY_LRU]  = { "LRU",  lru_load,  lru_access  },
};

Replacer *replacer_create(PolicyKind kind, size_t nframes)
{
    Replacer *r = calloc(1, sizeof(*r));
    r->pol   = &POLICIES[kind];
    r->n     = nframes;
    r->clock = 0;
    r->stamp = calloc(nframes, sizeof(uint64_t));
    return r;
}

void replacer_destroy(Replacer *r)
{
    if (!r) return;
    free(r->stamp);
    free(r);
}

const char *replacer_name(const Replacer *r) { return r->pol->name; }

void replacer_note_load  (Replacer *r, size_t f) { r->pol->note_load(r, f); }
void replacer_note_access(Replacer *r, size_t f) { r->pol->note_access(r, f); }
void replacer_note_free  (Replacer *r, size_t f) { r->stamp[f] = 0; }

/* The victim is the evictable frame with the smallest (oldest) stamp.
 * Under FIFO that is the earliest-loaded page; under LRU the least-recently
 * used -- the only difference is whether accesses re-stamp the frame. */
int replacer_victim(Replacer *r, const unsigned char *evictable, size_t *out)
{
    int found = 0;
    uint64_t best = 0;
    for (size_t f = 0; f < r->n; f++) {
        if (!evictable[f] || r->stamp[f] == 0) continue;
        if (!found || r->stamp[f] < best) { best = r->stamp[f]; *out = f; found = 1; }
    }
    return found;
}
