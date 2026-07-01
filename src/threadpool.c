/*
 * threadpool.c -- bounded-buffer (producer-consumer) thread pool.
 *
 * This is the concurrency engine behind Task 1 (process/threading) and the
 * server's ability to serve many clients at once (Task 4): the accept loop is
 * the *producer* that enqueues one job per connection, and a fixed set of
 * worker threads are the *consumers* that run them.
 *
 * Synchronisation model -- a monitor built from one mutex + two condition
 * variables over a bounded ring buffer:
 *
 *   - `mtx`        serialises every access to the shared queue state
 *                  (queue[], head, tail, count, shutdown), so there is no data
 *                  race on the buffer itself.
 *   - `not_empty`  workers sleep on this while there is nothing to consume;
 *                  a producer signals it after enqueuing.
 *   - `not_full`   producers sleep on this while the buffer is full
 *                  (backpressure); a worker signals it after dequeuing.
 *
 * pthreads condition variables follow *Mesa* semantics: a signalled thread is
 * merely made runnable, it is not guaranteed to run next, so the predicate it
 * waited for may already be false again by the time it re-acquires the mutex.
 * Every wait therefore sits inside a `while (predicate)` loop, not an `if` --
 * this also makes spurious wakeups (permitted by POSIX) harmless.
 * See include/threadpool.h for the public contract.
 */
#define _POSIX_C_SOURCE 200809L    /* expose the POSIX pthreads/condvar API */
#include "threadpool.h"

#include <stdlib.h>
#include <pthread.h>

/* One unit of work: a function pointer plus its opaque argument. Copied by
 * value into/out of the queue so the producer need not keep it alive. */
typedef struct { job_fn fn; void *arg; } Job;

struct ThreadPool {
    pthread_t      *workers;     /* the N joinable worker threads             */
    int             nworkers;

    Job            *queue;       /* ring buffer of capacity `cap`             */
    int             cap;
    int             head, tail;  /* dequeue at head, enqueue at tail (mod cap)*/
    int             count;       /* jobs currently queued (0..cap)            */

    pthread_mutex_t mtx;         /* guards ALL fields below and the queue     */
    pthread_cond_t  not_empty;   /* signalled when a job is added             */
    pthread_cond_t  not_full;    /* signalled when a slot frees up            */

    int             shutdown;    /* set once; no new jobs accepted afterwards */
    unsigned long   completed;   /* running total of finished jobs (stats)    */
};

/*
 * Worker loop: block until a job is available, take exactly one, then run it
 * with the lock released so jobs execute concurrently. Exits only once the
 * pool is both shutting down AND fully drained, so no queued work is dropped.
 */
static void *worker_main(void *arg)
{
    ThreadPool *tp = arg;
    for (;;) {
        pthread_mutex_lock(&tp->mtx);
        /* Wait for work. Loop (not if) because of Mesa semantics + spurious
         * wakeups: re-test `count` every time we regain the mutex. */
        while (tp->count == 0 && !tp->shutdown)
            pthread_cond_wait(&tp->not_empty, &tp->mtx);
        /* Empty *and* closing => nothing left to do; retire this worker. */
        if (tp->count == 0 && tp->shutdown) {
            pthread_mutex_unlock(&tp->mtx);
            return NULL;
        }
        /* Dequeue one job by value while holding the lock. */
        Job j = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->cap;
        tp->count--;
        /* A slot just freed: wake one producer that may be blocked in submit. */
        pthread_cond_signal(&tp->not_full);
        pthread_mutex_unlock(&tp->mtx);

        /* Run OUTSIDE the lock -- this is what gives real parallelism. Holding
         * the mutex across j.fn() would serialise all workers into one. */
        j.fn(j.arg);

        pthread_mutex_lock(&tp->mtx);
        tp->completed++;                               /* stats, under lock */
        pthread_mutex_unlock(&tp->mtx);
    }
}

/*
 * Allocate the pool, initialise its synchronisation objects, and spawn the
 * worker threads. Arguments are clamped to a sane minimum of 1 so a caller
 * cannot create a pool that can never make progress.
 */
ThreadPool *threadpool_create(int nworkers, int queue_cap)
{
    if (nworkers < 1) nworkers = 1;
    if (queue_cap < 1) queue_cap = 1;

    ThreadPool *tp = calloc(1, sizeof(*tp));           /* zeroes head/tail/... */
    tp->nworkers = nworkers;
    tp->cap      = queue_cap;
    tp->queue    = calloc(queue_cap, sizeof(Job));
    pthread_mutex_init(&tp->mtx, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    pthread_cond_init(&tp->not_full, NULL);

    /* Workers start immediately and block on not_empty until work arrives. */
    tp->workers = calloc(nworkers, sizeof(pthread_t));
    for (int i = 0; i < nworkers; i++)
        pthread_create(&tp->workers[i], NULL, worker_main, tp);
    return tp;
}

/*
 * Producer side. Enqueue one job, blocking while the buffer is full so a fast
 * producer cannot outrun the workers and grow memory without bound
 * (backpressure). Returns 0 on success, or -1 if the pool is shutting down.
 */
int threadpool_submit(ThreadPool *tp, job_fn fn, void *arg)
{
    pthread_mutex_lock(&tp->mtx);
    /* Block while full; loop for the same Mesa/spurious-wakeup reason as above. */
    while (tp->count == tp->cap && !tp->shutdown)
        pthread_cond_wait(&tp->not_full, &tp->mtx);
    /* Refuse new work once shutdown has begun (else it could never be joined). */
    if (tp->shutdown) { pthread_mutex_unlock(&tp->mtx); return -1; }

    tp->queue[tp->tail] = (Job){ fn, arg };
    tp->tail = (tp->tail + 1) % tp->cap;
    tp->count++;
    /* Wake one sleeping worker to consume what we just produced. */
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mtx);
    return 0;
}

/*
 * Graceful shutdown: stop accepting jobs, let the workers finish everything
 * already queued, then join them and release all resources. Safe to call with
 * a NULL pool. Must be called from a single thread that owns the pool's
 * lifetime (typically the one that created it).
 */
void threadpool_shutdown(ThreadPool *tp)
{
    if (!tp) return;
    pthread_mutex_lock(&tp->mtx);
    tp->shutdown = 1;
    /* broadcast, not signal: EVERY blocked thread must re-evaluate its
     * predicate now that `shutdown` is set, otherwise idle workers or a
     * producer stuck on not_full would sleep forever and never be joined. */
    pthread_cond_broadcast(&tp->not_empty);            /* wake idle workers      */
    pthread_cond_broadcast(&tp->not_full);             /* wake blocked producers */
    pthread_mutex_unlock(&tp->mtx);

    /* Join before destroying: workers still touch mtx/queue until they exit. */
    for (int i = 0; i < tp->nworkers; i++)
        pthread_join(tp->workers[i], NULL);

    pthread_mutex_destroy(&tp->mtx);
    pthread_cond_destroy(&tp->not_empty);
    pthread_cond_destroy(&tp->not_full);
    free(tp->workers);
    free(tp->queue);
    free(tp);
}

/*
 * Read the completed-job counter. Taken under the lock because a plain read of
 * a value another thread writes is a data race (undefined behaviour) even for
 * a single word.
 */
unsigned long threadpool_completed(ThreadPool *tp)
{
    pthread_mutex_lock(&tp->mtx);
    unsigned long c = tp->completed;
    pthread_mutex_unlock(&tp->mtx);
    return c;
}
