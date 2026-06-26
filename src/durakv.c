/*
 * durakv.c -- Phase 1 command-line front end for the durable KV store.
 *
 * Usage:
 *   durakv <data.db> <wal.log>                 interactive command loop
 *   durakv <data.db> <wal.log> stress N START  commit N sequential keys
 *
 * Interactive commands (one per line):
 *   set <key> <value...>      -> OK
 *   get <key>                 -> VALUE <value> | NOTFOUND
 *   del <key>                 -> OK | NOTFOUND
 *   list                      -> one key per line, then END
 *   checkpoint                -> OK
 *   quit                      -> exits
 *
 * In stress mode each committed key is printed as "COMMIT <key>" and flushed
 * *after* its WAL fsync, so anything the harness sees on stdout is guaranteed
 * durable -- exactly the contract crashtest.sh relies on.
 */
#define _POSIX_C_SOURCE 200809L
#include "storage.h"
#include "bufferpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Shared by stress worker threads: a printf lock keeps COMMIT lines whole. */
static pthread_mutex_t g_print_mtx = PTHREAD_MUTEX_INITIALIZER;

typedef struct { DB *db; long start, count, threads, tid; } StressArg;

static void *stress_worker(void *arg)
{
    StressArg *a = arg;
    char key[64], val[64];
    for (long i = a->start + a->tid; i < a->start + a->count; i += a->threads) {
        snprintf(key, sizeof(key), "key%ld", i);
        snprintf(val, sizeof(val), "val%ld", i);
        if (db_set(a->db, key, val, (uint32_t)strlen(val)) == DK_OK) {
            /* db_set already fsync'd the WAL, so this COMMIT is durable */
            pthread_mutex_lock(&g_print_mtx);
            printf("COMMIT %s\n", key);
            fflush(stdout);
            pthread_mutex_unlock(&g_print_mtx);
        }
    }
    return NULL;
}

/* Commit `count` keys starting at `start`, optionally across `threads` worker
 * threads to exercise the store under concurrent load. */
static void run_stress(DB *db, long count, long start, long threads)
{
    if (threads < 1) threads = 1;
    StressArg *args = calloc(threads, sizeof(*args));
    pthread_t *th   = calloc(threads, sizeof(*th));
    for (long t = 0; t < threads; t++) {
        args[t] = (StressArg){ db, start, count, threads, t };
        pthread_create(&th[t], NULL, stress_worker, &args[t]);
    }
    for (long t = 0; t < threads; t++) pthread_join(th[t], NULL);
    free(args); free(th);
}

static void run_interactive(DB *db)
{
    char line[1 << 20];          /* generous: keys + values on one line */
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        char *cmd = strtok(line, " ");
        if (!cmd) continue;

        if (strcmp(cmd, "set") == 0) {
            char *key = strtok(NULL, " ");
            char *val = strtok(NULL, "");          /* rest of line = value */
            if (!key || !val) { printf("ERR usage: set <key> <value>\n"); continue; }
            int rc = db_set(db, key, val, (uint32_t)strlen(val));
            printf(rc == DK_OK ? "OK\n" :
                   rc == DK_TOOBIG ? "ERR size\n" : "ERR io\n");
        } else if (strcmp(cmd, "get") == 0) {
            char *key = strtok(NULL, " ");
            if (!key) { printf("ERR usage: get <key>\n"); continue; }
            static char buf[1 << 20];
            uint32_t vlen = 0;
            int rc = db_get(db, key, buf, sizeof(buf), &vlen);
            if (rc == DK_OK) printf("VALUE %.*s\n", (int)vlen, buf);
            else             printf("NOTFOUND\n");
        } else if (strcmp(cmd, "del") == 0) {
            char *key = strtok(NULL, " ");
            if (!key) { printf("ERR usage: del <key>\n"); continue; }
            printf(db_del(db, key) == DK_OK ? "OK\n" : "NOTFOUND\n");
        } else if (strcmp(cmd, "list") == 0) {
            for (size_t b = 0; b < db->dir.nbuckets; b++)
                for (DirEntry *e = db->dir.buckets[b]; e; e = e->next)
                    printf("%s\n", e->key);
            printf("END\n");
        } else if (strcmp(cmd, "checkpoint") == 0) {
            db_checkpoint(db);
            printf("OK\n");
        } else if (strcmp(cmd, "stats") == 0) {
            BPStats s = bp_stats(db->bp);
            printf("policy=%s frames=%zu pages=%llu\n",
                   bp_policy_name(db->bp), bp_nframes(db->bp),
                   (unsigned long long)db->page_count);
            printf("accesses=%llu hits=%llu faults=%llu "
                   "evictions=%llu writebacks=%llu hit_ratio=%.1f%%\n",
                   (unsigned long long)s.accesses, (unsigned long long)s.hits,
                   (unsigned long long)s.faults,   (unsigned long long)s.evictions,
                   (unsigned long long)s.writebacks, 100.0 * bp_hit_ratio(db->bp));
        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        } else {
            printf("ERR unknown command: %s\n", cmd);
        }
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s <data.db> <wal.log> [stress N START [THREADS]]\n"
            "  env: DURAKV_FRAMES=<n>  DURAKV_POLICY=fifo|lru\n", argv[0]);
        return 2;
    }

    /* buffer pool is configurable via env: DURAKV_FRAMES, DURAKV_POLICY */
    const char *fenv = getenv("DURAKV_FRAMES");
    const char *penv = getenv("DURAKV_POLICY");
    size_t frames = fenv ? (size_t)strtoul(fenv, NULL, 10) : 64;
    if (frames == 0) frames = 64;
    PolicyKind policy = (penv && strcmp(penv, "fifo") == 0) ? POLICY_FIFO : POLICY_LRU;

    /* DURAKV_PASSWORD => encryption at rest */
    const char *pw = getenv("DURAKV_PASSWORD");
    DB *db = pw ? db_open_secure(argv[1], argv[2], frames, policy, pw)
                : db_open_ex(argv[1], argv[2], frames, policy);
    if (!db) { fprintf(stderr, "failed to open store\n"); return 1; }

    if (argc >= 5 && strcmp(argv[3], "stress") == 0) {
        long count   = strtol(argv[4], NULL, 10);
        long start   = argc >= 6 ? strtol(argv[5], NULL, 10) : 0;
        long threads = argc >= 7 ? strtol(argv[6], NULL, 10) : 1;
        run_stress(db, count, start, threads);
    } else {
        run_interactive(db);
    }

    db_close(db);
    return 0;
}
