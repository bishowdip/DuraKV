/*
 * threadpool.c -- bounded-buffer thread pool. See include/threadpool.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "threadpool.h"

#include <stdlib.h>
#include <pthread.h>

typedef struct { job_fn fn; void *arg; } Job;

struct ThreadPool {
    pthread_t      *workers;
    int             nworkers;

    Job            *queue;       /* ring buffer of capacity `cap`  */
    int             cap;
    int             head, tail;  /* dequeue at head, enqueue at tail */
    int             count;       /* jobs currently queued           */

    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;

    int             shutdown;    /* set once; no new jobs accepted  */
    unsigned long   completed;
};

static void *worker_main(void *arg)
{
    ThreadPool *tp = arg;
    for (;;) {
        pthread_mutex_lock(&tp->mtx);
        while (tp->count == 0 && !tp->shutdown)        /* loop on predicate */
            pthread_cond_wait(&tp->not_empty, &tp->mtx);
        if (tp->count == 0 && tp->shutdown) {          /* drained + closing */
            pthread_mutex_unlock(&tp->mtx);
            return NULL;
        }
        Job j = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->cap;
        tp->count--;
        pthread_cond_signal(&tp->not_full);
        pthread_mutex_unlock(&tp->mtx);

        j.fn(j.arg);                                   /* run outside the lock */

        pthread_mutex_lock(&tp->mtx);
        tp->completed++;
        pthread_mutex_unlock(&tp->mtx);
    }
}

ThreadPool *threadpool_create(int nworkers, int queue_cap)
{
    if (nworkers < 1) nworkers = 1;
    if (queue_cap < 1) queue_cap = 1;

    ThreadPool *tp = calloc(1, sizeof(*tp));
    tp->nworkers = nworkers;
    tp->cap      = queue_cap;
    tp->queue    = calloc(queue_cap, sizeof(Job));
    pthread_mutex_init(&tp->mtx, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    pthread_cond_init(&tp->not_full, NULL);

    tp->workers = calloc(nworkers, sizeof(pthread_t));
    for (int i = 0; i < nworkers; i++)
        pthread_create(&tp->workers[i], NULL, worker_main, tp);
    return tp;
}

int threadpool_submit(ThreadPool *tp, job_fn fn, void *arg)
{
    pthread_mutex_lock(&tp->mtx);
    while (tp->count == tp->cap && !tp->shutdown)      /* backpressure */
        pthread_cond_wait(&tp->not_full, &tp->mtx);
    if (tp->shutdown) { pthread_mutex_unlock(&tp->mtx); return -1; }

    tp->queue[tp->tail] = (Job){ fn, arg };
    tp->tail = (tp->tail + 1) % tp->cap;
    tp->count++;
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mtx);
    return 0;
}

void threadpool_shutdown(ThreadPool *tp)
{
    if (!tp) return;
    pthread_mutex_lock(&tp->mtx);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->not_empty);            /* wake idle workers */
    pthread_cond_broadcast(&tp->not_full);             /* wake blocked producers */
    pthread_mutex_unlock(&tp->mtx);

    for (int i = 0; i < tp->nworkers; i++)
        pthread_join(tp->workers[i], NULL);

    pthread_mutex_destroy(&tp->mtx);
    pthread_cond_destroy(&tp->not_empty);
    pthread_cond_destroy(&tp->not_full);
    free(tp->workers);
    free(tp->queue);
    free(tp);
}

unsigned long threadpool_completed(ThreadPool *tp)
{
    pthread_mutex_lock(&tp->mtx);
    unsigned long c = tp->completed;
    pthread_mutex_unlock(&tp->mtx);
    return c;
}
