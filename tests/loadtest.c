/*
 * loadtest.c -- many concurrent clients driving the real store through the
 * thread pool, proving the storage engine is correct under concurrent load.
 *
 * Each "client" is a job that performs NOPS SET operations on its own key
 * namespace. Workers run them in parallel. Afterwards the main thread reads
 * every key back and checks the value -- if any update were lost or any page
 * corrupted by a race, this fails. Per-client durations are reported to show
 * the work was spread fairly across workers.
 */
#define _POSIX_C_SOURCE 200809L
#include "storage.h"
#include "threadpool.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define DATA    "/tmp/durakv_load.db"
#define WAL     "/tmp/durakv_load.log"
#define NCLIENT 16
#define NOPS    120     /* commits are F_FULLFSYNC-bound, so keep this modest */
#define WORKERS 4

typedef struct {
    DB    *db;
    int    cid;
    double elapsed_ms;
} Client;

static double now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void client_job(void *arg)
{
    Client *c = arg;
    double t0 = now_ms();
    char k[48], v[48];
    for (int i = 0; i < NOPS; i++) {
        snprintf(k, sizeof(k), "c%d_k%d", c->cid, i);
        snprintf(v, sizeof(v), "c%d_v%d", c->cid, i);
        int rc = db_set(c->db, k, v, (uint32_t)strlen(v));
        assert(rc == DK_OK);
    }
    c->elapsed_ms = now_ms() - t0;
}

int main(void)
{
    unlink(DATA); unlink(WAL);
    DB *db = db_open(DATA, WAL);
    assert(db);

    Client clients[NCLIENT];
    for (int i = 0; i < NCLIENT; i++) { clients[i].db = db; clients[i].cid = i; }

    ThreadPool *tp = threadpool_create(WORKERS, 8);

    double t0 = now_ms();
    for (int i = 0; i < NCLIENT; i++)
        threadpool_submit(tp, client_job, &clients[i]);
    threadpool_shutdown(tp);          /* drains + joins */
    double wall = now_ms() - t0;

    /* every key from every client must be present and correct */
    for (int c = 0; c < NCLIENT; c++) {
        for (int i = 0; i < NOPS; i++) {
            char k[48], expect[48], buf[64]; uint32_t vl = 0;
            snprintf(k, sizeof(k), "c%d_k%d", c, i);
            snprintf(expect, sizeof(expect), "c%d_v%d", c, i);
            int rc = db_get(db, k, buf, sizeof(buf), &vl);
            assert(rc == DK_OK);
            assert(vl == strlen(expect) && memcmp(buf, expect, vl) == 0);
        }
    }

    double mn = 1e18, mx = 0, sum = 0;
    for (int i = 0; i < NCLIENT; i++) {
        double e = clients[i].elapsed_ms;
        if (e < mn) mn = e; if (e > mx) mx = e; sum += e;
    }

    int total_ops = NCLIENT * NOPS;
    printf("clients=%d ops/client=%d workers=%d  (%d ops total)\n",
           NCLIENT, NOPS, WORKERS, total_ops);
    printf("wall=%.1f ms  throughput=%.0f ops/s\n",
           wall, total_ops / (wall / 1000.0));
    printf("per-client latency: min=%.1f ms  avg=%.1f ms  max=%.1f ms\n",
           mn, sum / NCLIENT, mx);

    db_close(db);
    unlink(DATA); unlink(WAL);
    printf("\nloadtest: PASS (%d concurrent ops, all values verified)\n", total_ops);
    return 0;
}
