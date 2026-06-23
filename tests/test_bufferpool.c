/*
 * test_bufferpool.c -- buffer-pool correctness (page faults, eviction,
 * dirty write-back) plus a FIFO-vs-LRU hit-ratio report across workloads.
 */
#define _POSIX_C_SOURCE 200809L
#include "bufferpool.h"
#include "storage.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define BACKING "/tmp/durakv_bp.db"
#define NPAGES  64

static void mark(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint64_t unmark(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* ---- correctness: writes survive eviction (write-back works) ----------- */
static void test_correctness(void)
{
    int fd = open(BACKING, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)NPAGES * PAGE_SIZE) == 0);

    BufferPool *bp = bp_create(fd, 4, POLICY_LRU);   /* tiny: forces eviction */

    /* dirty 20 distinct pages through only 4 frames -> evictions + writeback */
    for (uint64_t pid = 1; pid <= 20; pid++) {
        uint8_t *f = bp_pin(bp, pid);
        assert(f);
        mark(f, pid * 1000 + 7);
        bp_unpin(bp, pid, 1);
    }
    bp_flush_all(bp);

    /* read them all back -- every value must be the one we wrote */
    for (uint64_t pid = 1; pid <= 20; pid++) {
        uint8_t *f = bp_pin(bp, pid);
        assert(unmark(f) == pid * 1000 + 7);
        bp_unpin(bp, pid, 0);
    }

    /* a re-pin of a resident page is a hit, not a fault */
    BPStats before = bp_stats(bp);
    bp_pin(bp, 20); bp_unpin(bp, 20, 0);   /* 20 was just accessed -> resident */
    BPStats after = bp_stats(bp);
    assert(after.hits == before.hits + 1);

    BPStats s = bp_stats(bp);
    assert(s.evictions > 0);
    assert(s.writebacks > 0);

    bp_destroy(bp);
    close(fd);
    printf("buffer pool correctness: PASS (evictions=%llu writebacks=%llu)\n",
           (unsigned long long)s.evictions, (unsigned long long)s.writebacks);
}

/* ---- a deterministic PRNG so the report is reproducible ---------------- */
static uint64_t rng_state = 0x1234567;
static uint64_t rng(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7;
                            rng_state ^= rng_state << 17; return rng_state; }

static double run(PolicyKind pol, size_t frames, const uint64_t *refs, size_t n)
{
    int fd = open(BACKING, O_RDWR | O_CREAT, 0600);
    BufferPool *bp = bp_create(fd, frames, pol);
    for (size_t i = 0; i < n; i++) { bp_pin(bp, refs[i]); bp_unpin(bp, refs[i], 0); }
    double r = bp_hit_ratio(bp);
    bp_destroy(bp);
    close(fd);
    return r;
}

static void report_row(const char *name, const uint64_t *refs, size_t n, size_t frames)
{
    printf("  %-22s %4zu     %6.1f%%   %6.1f%%\n",
           name, frames,
           100.0 * run(POLICY_FIFO, frames, refs, n),
           100.0 * run(POLICY_LRU,  frames, refs, n));
}

static void test_hit_ratio_report(void)
{
    static uint64_t refs[4000];
    size_t n;
    size_t frames = 16;

    printf("\nHit-ratio report (%zu frames):\n", frames);
    printf("  %-22s %-8s %-8s %-8s\n", "workload", "frames", "FIFO", "LRU");

    /* looping scan larger than the cache -> both thrash */
    n = 0;
    for (int pass = 0; pass < 8; pass++)
        for (uint64_t p = 1; p <= 24; p++) refs[n++] = p;
    report_row("loop(24) > cache", refs, n, frames);

    /* looping scan that fits -> high hit ratio after warm-up */
    n = 0;
    for (int pass = 0; pass < 8; pass++)
        for (uint64_t p = 1; p <= 12; p++) refs[n++] = p;
    report_row("loop(12) <= cache", refs, n, frames);

    /* skewed locality: 80% to a hot set of 8 pages, 20% cold over 1..48.
     * LRU should keep the hot set resident and beat FIFO. */
    n = 0;
    for (int i = 0; i < 2000; i++)
        refs[n++] = (rng() % 10 < 8) ? (1 + rng() % 8) : (9 + rng() % 40);
    report_row("hot-set 80/20", refs, n, frames);

    unlink(BACKING);
}

int main(void)
{
    test_correctness();
    test_hit_ratio_report();
    printf("\ntest_bufferpool: PASS\n");
    return 0;
}
