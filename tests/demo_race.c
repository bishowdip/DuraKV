/*
 * demo_race.c -- a data race made visible, then fixed three ways.
 *
 * OS/systems primitive: race conditions, atomicity, why unsynchronised
 * read-modify-write loses updates.
 *
 * Many threads each add 1 to a shared counter NITER times. The correct total
 * is NTHREADS * NITER. We run it:
 *   1. unsynchronised   -> total is short (updates were lost)
 *   2. mutex-protected   -> total is exact
 *   3. C11 _Atomic       -> total is exact, lock-free
 *
 * The read-modify-write is deliberately split (read; yield; write) to widen
 * the race window so the loss is reliably observable.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#define NTHREADS 8
#define NITER    20000

static long            plain_counter;
static long            mutex_counter;
static atomic_long     atomic_counter;
static pthread_mutex_t counter_mtx = PTHREAD_MUTEX_INITIALIZER;

static void *race_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < NITER; i++) {
        long tmp = plain_counter;    /* read  */
        sched_yield();               /* widen the window -> guarantee loss */
        plain_counter = tmp + 1;     /* write */
    }
    return NULL;
}

static void *mutex_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < NITER; i++) {
        pthread_mutex_lock(&counter_mtx);
        mutex_counter++;
        pthread_mutex_unlock(&counter_mtx);
    }
    return NULL;
}

static void *atomic_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < NITER; i++)
        atomic_fetch_add(&atomic_counter, 1);
    return NULL;
}

static long run(void *(*fn)(void *))
{
    pthread_t t[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) pthread_create(&t[i], NULL, fn, NULL);
    for (int i = 0; i < NTHREADS; i++) pthread_join(t[i], NULL);
    return 0;
}

int main(void)
{
    const long expected = (long)NTHREADS * NITER;

    run(race_worker);
    run(mutex_worker);
    run(atomic_worker);
    long atomic_final = atomic_load(&atomic_counter);

    printf("expected total           : %ld\n", expected);
    printf("1. no lock      -> %-8ld  (lost %ld updates)\n",
           plain_counter, expected - plain_counter);
    printf("2. mutex        -> %-8ld  %s\n", mutex_counter,
           mutex_counter == expected ? "(correct)" : "(WRONG)");
    printf("3. C11 _Atomic  -> %-8ld  %s\n", atomic_final,
           atomic_final == expected ? "(correct, lock-free)" : "(WRONG)");

    /* the synchronised versions must be exact */
    assert(mutex_counter == expected);
    assert(atomic_final == expected);
    /* the racy version should have lost updates (it essentially always does) */
    if (plain_counter == expected)
        printf("note: unsynchronised run happened to be correct this time; "
               "re-run to observe the race.\n");

    printf("\ndemo_race: PASS\n");
    return 0;
}
