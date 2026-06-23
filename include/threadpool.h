/*
 * threadpool.h -- Fixed worker pool over a bounded job queue (DuraKV Phase 3).
 *
 * OS/systems primitive: threads + producer-consumer synchronisation.
 *
 * A fixed set of N worker threads pull jobs from a bounded ring buffer guarded
 * by one mutex and two condition variables (not_empty, not_full) -- the
 * textbook bounded-buffer pattern. Producers block when the queue is full
 * (backpressure); workers block when it is empty. Every wait re-checks its
 * predicate in a loop, so spurious wakeups are harmless.
 */
#ifndef DURAKV_THREADPOOL_H
#define DURAKV_THREADPOOL_H

#include <stddef.h>

typedef void (*job_fn)(void *arg);

typedef struct ThreadPool ThreadPool;

ThreadPool *threadpool_create(int nworkers, int queue_cap);

/* Enqueue a job. Blocks while the queue is full. Returns 0 on success, or -1
 * if the pool is shutting down. */
int  threadpool_submit(ThreadPool *tp, job_fn fn, void *arg);

/* Stop accepting jobs, let workers drain the queue, then join them all. */
void threadpool_shutdown(ThreadPool *tp);

/* Number of jobs completed so far (for stats/tests). */
unsigned long threadpool_completed(ThreadPool *tp);

#endif /* DURAKV_THREADPOOL_H */
