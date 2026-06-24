/*
 * server.c -- AF_UNIX server: parse, validate, dispatch commands to the store,
 * one worker per connection. See include/server.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "protocol.h"
#include "threadpool.h"
#include "bufferpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>

#define MAX_KEY 256                 /* validation: key length cap */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ---- command dispatch (text in, text out) ------------------------------ *
 * Returns the response length written into resp (never exceeds respcap-1).  */
static size_t handle_command(DB *db, const char *payload, uint32_t plen,
                             char *resp, size_t respcap)
{
    /* copy to a NUL-terminated, tokenisable buffer */
    char line[512 + 16];
    char *big = NULL, *buf = line;
    if (plen + 1 > sizeof(line)) { big = malloc(plen + 1); buf = big; }
    memcpy(buf, payload, plen);
    buf[plen] = '\0';

    char *save = NULL;
    char *cmd = strtok_r(buf, " ", &save);
    int n = 0;

    if (!cmd) {
        n = snprintf(resp, respcap, "ERR empty");
    } else if (strcmp(cmd, "PING") == 0) {
        n = snprintf(resp, respcap, "PONG");
    } else if (strcmp(cmd, "SET") == 0) {
        char *key = strtok_r(NULL, " ", &save);
        char *val = strtok_r(NULL, "", &save);          /* rest = value */
        if (!key || !val)            n = snprintf(resp, respcap, "ERR usage SET <key> <value>");
        else if (strlen(key) > MAX_KEY) n = snprintf(resp, respcap, "ERR key too long");
        else {
            int rc = db_set(db, key, val, (uint32_t)strlen(val));
            n = snprintf(resp, respcap, "%s", rc == DK_OK ? "OK" :
                         rc == DK_TOOBIG ? "ERR value too large" : "ERR io");
        }
    } else if (strcmp(cmd, "GET") == 0) {
        char *key = strtok_r(NULL, " ", &save);
        if (!key) n = snprintf(resp, respcap, "ERR usage GET <key>");
        else {
            static __thread char vbuf[PROTO_MAX_PAYLOAD];
            uint32_t vl = 0;
            int rc = db_get(db, key, vbuf, sizeof(vbuf), &vl);
            if (rc == DK_OK) n = snprintf(resp, respcap, "OK %.*s", (int)vl, vbuf);
            else             n = snprintf(resp, respcap, "ERR notfound");
        }
    } else if (strcmp(cmd, "DEL") == 0) {
        char *key = strtok_r(NULL, " ", &save);
        if (!key) n = snprintf(resp, respcap, "ERR usage DEL <key>");
        else      n = snprintf(resp, respcap, "%s",
                               db_del(db, key) == DK_OK ? "OK" : "ERR notfound");
    } else if (strcmp(cmd, "STATS") == 0) {
        BPStats s = bp_stats(db->bp);
        n = snprintf(resp, respcap,
                     "policy=%s frames=%zu accesses=%llu hits=%llu faults=%llu "
                     "hit_ratio=%.1f%%", bp_policy_name(db->bp), bp_nframes(db->bp),
                     (unsigned long long)s.accesses, (unsigned long long)s.hits,
                     (unsigned long long)s.faults, 100.0 * bp_hit_ratio(db->bp));
    } else if (strcmp(cmd, "QUIT") == 0) {
        n = snprintf(resp, respcap, "BYE");
    } else {
        n = snprintf(resp, respcap, "ERR unknown command");
    }

    free(big);
    return n < 0 ? 0 : (size_t)n;
}

/* ---- per-connection session (runs on a worker thread) ------------------ */

typedef struct { int fd; DB *db; } Conn;

static int is_quit(const char *payload, uint32_t plen)
{
    return plen >= 4 && strncmp(payload, "QUIT", 4) == 0;
}

static void serve_connection(void *arg)
{
    Conn *c = arg;
    static __thread char req[PROTO_MAX_PAYLOAD];
    static __thread char resp[PROTO_MAX_PAYLOAD];

    for (;;) {
        uint32_t len = 0;
        int rc = frame_read(c->fd, req, sizeof(req), &len);
        if (rc == -2) {                          /* protocol violation */
            frame_write(c->fd, "ERR frame too large", 19);
            break;
        }
        if (rc != 0) break;                      /* peer closed / error */

        size_t rlen = handle_command(c->db, req, len, resp, sizeof(resp));
        if (frame_write(c->fd, resp, (uint32_t)rlen) != 0) break;
        if (is_quit(req, len)) break;
    }
    close(c->fd);
    free(c);
}

/* ---- accept loop ------------------------------------------------------- */

int server_run(const char *sock_path, DB *db, int nworkers)
{
    signal(SIGPIPE, SIG_IGN);                    /* writes to dead peers -> EPIPE */
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int lfd = unix_listen(sock_path, 64);
    if (lfd < 0) { perror("unix_listen"); return -1; }

    ThreadPool *tp = threadpool_create(nworkers, 128);
    fprintf(stderr, "durakv-server listening on %s (%d workers)\n", sock_path, nworkers);

    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;        /* interrupted by signal */
            if (g_stop) break;
            continue;
        }
        Conn *c = malloc(sizeof(*c));
        c->fd = cfd; c->db = db;
        threadpool_submit(tp, serve_connection, c);
    }

    fprintf(stderr, "durakv-server shutting down\n");
    close(lfd);
    unlink(sock_path);
    threadpool_shutdown(tp);                      /* drain in-flight clients */
    return 0;
}

#ifndef DURAKV_SERVER_NO_MAIN
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <socket-path> [data.db] [wal.log] [workers]\n"
            "  env: DURAKV_FRAMES=<n>  DURAKV_POLICY=fifo|lru\n", argv[0]);
        return 2;
    }
    const char *sock = argv[1];
    const char *data = argc >= 3 ? argv[2] : "data.db";
    const char *wal  = argc >= 4 ? argv[3] : "wal.log";
    int workers      = argc >= 5 ? atoi(argv[4]) : 4;

    const char *fenv = getenv("DURAKV_FRAMES");
    const char *penv = getenv("DURAKV_POLICY");
    size_t frames = fenv ? (size_t)strtoul(fenv, NULL, 10) : 64;
    PolicyKind pol = (penv && strcmp(penv, "fifo") == 0) ? POLICY_FIFO : POLICY_LRU;

    DB *db = db_open_ex(data, wal, frames ? frames : 64, pol);
    if (!db) { fprintf(stderr, "failed to open store\n"); return 1; }

    int rc = server_run(sock, db, workers);
    db_close(db);
    return rc == 0 ? 0 : 1;
}
#endif
