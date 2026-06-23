/*
 * test_belady.c -- demonstrates Belady's anomaly with the FIFO policy, and
 * confirms LRU (a stack algorithm) does not suffer it.
 *
 * The classic reference string 1 2 3 4 1 2 5 1 2 3 4 5 produces MORE page
 * faults with 4 frames than with 3 under FIFO -- the counter-intuitive result
 * that adding memory can hurt. We drive the *real* buffer pool over a backing
 * file so the fault counts come from the actual paging machinery, not a model.
 */
#define _POSIX_C_SOURCE 200809L
#include "bufferpool.h"
#include "storage.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define BACKING "/tmp/durakv_belady.db"

/* page ids referenced over time (the textbook Belady string) */
static const uint64_t REF[] = { 1,2,3,4,1,2,5,1,2,3,4,5 };
static const size_t   REFN  = sizeof(REF) / sizeof(REF[0]);

static uint64_t faults_for(PolicyKind policy, size_t nframes)
{
    int fd = open(BACKING, O_RDWR | O_CREAT, 0600);
    assert(fd >= 0);
    BufferPool *bp = bp_create(fd, nframes, policy);
    for (size_t i = 0; i < REFN; i++) {
        bp_pin(bp, REF[i]);
        bp_unpin(bp, REF[i], 0);
    }
    uint64_t faults = bp_stats(bp).faults;
    bp_destroy(bp);
    close(fd);
    return faults;
}

int main(void)
{
    /* the backing file just needs to be large enough to back page 5 */
    int fd = open(BACKING, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    ftruncate(fd, (off_t)6 * PAGE_SIZE);
    close(fd);

    uint64_t fifo3 = faults_for(POLICY_FIFO, 3);
    uint64_t fifo4 = faults_for(POLICY_FIFO, 4);
    uint64_t lru3  = faults_for(POLICY_LRU, 3);
    uint64_t lru4  = faults_for(POLICY_LRU, 4);

    printf("reference string:");
    for (size_t i = 0; i < REFN; i++) printf(" %llu", (unsigned long long)REF[i]);
    printf("\n\n");
    printf("policy   3 frames   4 frames\n");
    printf("FIFO     %-10llu %-10llu  <- more frames, MORE faults (Belady)\n",
           (unsigned long long)fifo3, (unsigned long long)fifo4);
    printf("LRU      %-10llu %-10llu  <- monotonic (stack algorithm)\n",
           (unsigned long long)lru3, (unsigned long long)lru4);

    /* the anomaly: FIFO with more frames faults more */
    assert(fifo4 > fifo3);
    /* LRU never faults more with more frames */
    assert(lru4 <= lru3);

    printf("\ntest_belady: PASS (FIFO %llu->%llu faults; LRU %llu->%llu)\n",
           (unsigned long long)fifo3, (unsigned long long)fifo4,
           (unsigned long long)lru3,  (unsigned long long)lru4);
    unlink(BACKING);
    return 0;
}
