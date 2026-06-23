/*
 * demo_deadlock.c -- the Coffman circular-wait condition, and how a strict
 * lock ordering breaks it.
 *
 * OS/systems primitive: deadlock, the four Coffman conditions, lock hierarchy.
 *
 *   naive   : thread 1 locks A then B; thread 2 locks B then A. With a small
 *             stagger they each grab their first lock and then block forever
 *             on the second -> circular wait -> deadlock.
 *   ordered : both threads acquire locks in the SAME (ascending) order. There
 *             is no cycle, so the run completes. This is exactly the rule the
 *             storage engine follows for page latches: always lock pages in
 *             ascending page_id order.
 *
 * To keep the test suite from hanging, each scenario runs in a forked child
 * watched by a timeout: if the child does not finish, the parent declares a
 * deadlock and kills it.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>

static pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t B = PTHREAD_MUTEX_INITIALIZER;

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- naive: opposite acquisition orders ---- */
static void *naive_t1(void *a) { (void)a;
    pthread_mutex_lock(&A); msleep(100); pthread_mutex_lock(&B);
    pthread_mutex_unlock(&B); pthread_mutex_unlock(&A); return NULL; }
static void *naive_t2(void *a) { (void)a;
    pthread_mutex_lock(&B); msleep(100); pthread_mutex_lock(&A);
    pthread_mutex_unlock(&A); pthread_mutex_unlock(&B); return NULL; }

/* ---- ordered: both acquire A before B ---- */
static void *ord_t1(void *a) { (void)a;
    pthread_mutex_lock(&A); msleep(100); pthread_mutex_lock(&B);
    pthread_mutex_unlock(&B); pthread_mutex_unlock(&A); return NULL; }
static void *ord_t2(void *a) { (void)a;
    pthread_mutex_lock(&A); msleep(100); pthread_mutex_lock(&B);
    pthread_mutex_unlock(&B); pthread_mutex_unlock(&A); return NULL; }

/* Run a two-thread scenario in a child; return 1 if it deadlocked (had to be
 * killed by the watchdog), 0 if it completed within timeout_ms. */
static int run_scenario(void *(*f1)(void *), void *(*f2)(void *), int timeout_ms)
{
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t t1, t2;
        pthread_create(&t1, NULL, f1, NULL);
        pthread_create(&t2, NULL, f2, NULL);
        pthread_join(t1, NULL);
        pthread_join(t2, NULL);
        _exit(0);
    }
    int waited = 0;
    for (;;) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return 0;                 /* completed */
        if (waited >= timeout_ms) {             /* watchdog fires */
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return 1;                           /* deadlocked */
        }
        msleep(20);
        waited += 20;
    }
}

int main(void)
{
    int naive_dead   = run_scenario(naive_t1, naive_t2, 1500);
    printf("naive  (A->B vs B->A): %s\n",
           naive_dead ? "DEADLOCK (watchdog had to kill it)" : "completed");

    int ordered_dead = run_scenario(ord_t1, ord_t2, 1500);
    printf("ordered(A->B vs A->B): %s\n",
           ordered_dead ? "DEADLOCK" : "completed (lock ordering broke the cycle)");

    assert(naive_dead == 1);      /* the bug reproduces */
    assert(ordered_dead == 0);    /* the fix works */

    printf("\ndemo_deadlock: PASS\n");
    return 0;
}
