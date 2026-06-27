/*
 * server.c -- AF_UNIX server: parse, validate, dispatch commands to the store,
 * one worker per connection. See include/server.h.
 *
 * Security is opt-in via the DURAKV_SECURE environment variable. When enabled,
 * a connection must AUTH before issuing data commands, every SET/GET/DEL is
 * checked against the caller's rwx permission on the key's namespace, and every
 * attempt is written to the hash-chained audit log. When disabled the server
 * runs "open" (used by the plain IPC test).
 */
#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "protocol.h"
#include "threadpool.h"
#include "bufferpool.h"
#include "auth.h"
#include "permissions.h"
#include "audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>

#define MAX_KEY 256

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ---- security context (shared by all connections) ---------------------- */

typedef struct {
    AuthStore      *auth;
    PermTable      *perm;
    Audit          *audit;
    pthread_mutex_t audit_mtx;
} SecCtx;

static SecCtx *sec_create(void)
{
    SecCtx *s = calloc(1, sizeof(*s));
    s->auth = auth_create();
    s->perm = perm_create();
    const char *apath = getenv("DURAKV_AUDIT");
    s->audit = audit_open(apath ? apath : "audit.log");
    pthread_mutex_init(&s->audit_mtx, NULL);

    /* seeded demo policy: two staff users and three namespaces */
    auth_register(s->auth, "alice", "alice-secret", "staff");
    auth_register(s->auth, "bob",   "bob-secret",   "staff");
    perm_set(s->perm, "alice",  "alice", "staff", 0640); /* alice rw, staff r  */
    perm_set(s->perm, "bob",    "bob",   "staff", 0640); /* bob rw,   staff r  */
    perm_set(s->perm, "shared", "alice", "staff", 0660); /* alice+staff rw     */
    return s;
}

static void sec_destroy(SecCtx *s)
{
    if (!s) return;
    auth_destroy(s->auth);
    perm_destroy(s->perm);
    audit_close(s->audit);
    pthread_mutex_destroy(&s->audit_mtx);
    free(s);
}

static void sec_audit(SecCtx *s, const char *user, const char *op,
                      const char *key, const char *result)
{
    if (!s->audit) return;
    pthread_mutex_lock(&s->audit_mtx);
    audit_append(s->audit, user, op, key, result);
    pthread_mutex_unlock(&s->audit_mtx);
}

/* namespace = the prefix before ':' in a key, else "default" */
static void extract_ns(const char *key, char *ns, size_t cap)
{
    const char *colon = strchr(key, ':');
    if (colon) {
        size_t n = (size_t)(colon - key);
        if (n >= cap) n = cap - 1;
        memcpy(ns, key, n); ns[n] = '\0';
    } else {
        snprintf(ns, cap, "default");
    }
}

/* ---- per-connection session state ------------------------------------- */

typedef struct {
    int     fd;
    DB     *db;
    SecCtx *sec;            /* NULL => open mode (no auth/perm/audit) */
    int     authed;
    char    user[64];
    char    group[64];
} Conn;

/* ---- command dispatch -------------------------------------------------- */

static size_t handle_command(Conn *c, const char *payload, uint32_t plen,
                             char *resp, size_t respcap)
{
    char line[512 + 16];
    char *big = NULL, *buf = line;
    if (plen + 1 > sizeof(line)) { big = malloc(plen + 1); buf = big; }
    memcpy(buf, payload, plen);
    buf[plen] = '\0';

    DB *db = c->db; SecCtx *sec = c->sec;
    char *save = NULL;
    char *cmd = strtok_r(buf, " ", &save);
    int n = 0;

    if (!cmd) { n = snprintf(resp, respcap, "ERR empty"); goto done; }

    if (strcmp(cmd, "PING") == 0) { n = snprintf(resp, respcap, "PONG"); goto done; }
    if (strcmp(cmd, "QUIT") == 0) { n = snprintf(resp, respcap, "BYE");  goto done; }

    if (strcmp(cmd, "AUTH") == 0) {
        char *u = strtok_r(NULL, " ", &save);
        char *p = strtok_r(NULL, " ", &save);
        if (!sec)        n = snprintf(resp, respcap, "ERR auth not enabled");
        else if (!u || !p) n = snprintf(resp, respcap, "ERR usage AUTH <user> <pass>");
        else if (auth_login(sec->auth, u, p) > 0) {
            c->authed = 1;
            snprintf(c->user, sizeof(c->user), "%s", u);
            const char *g = auth_group_of(sec->auth, u);
            snprintf(c->group, sizeof(c->group), "%s", g ? g : "");
            n = snprintf(resp, respcap, "OK authenticated as %s", u);
        } else {
            n = snprintf(resp, respcap, "ERR auth");
        }
        goto done;
    }

    /* everything below touches data; in secure mode it needs a session */
    int is_data = (strcmp(cmd, "SET") == 0 || strcmp(cmd, "GET") == 0 ||
                   strcmp(cmd, "DEL") == 0 || strcmp(cmd, "STATS") == 0);
    if (sec && is_data && !c->authed) {
        n = snprintf(resp, respcap, "ERR auth required"); goto done;
    }

    if (strcmp(cmd, "STATS") == 0) {
        BPStats s = bp_stats(db->bp);
        n = snprintf(resp, respcap,
                     "policy=%s frames=%zu accesses=%llu hits=%llu hit_ratio=%.1f%%",
                     bp_policy_name(db->bp), bp_nframes(db->bp),
                     (unsigned long long)s.accesses, (unsigned long long)s.hits,
                     100.0 * bp_hit_ratio(db->bp));
        goto done;
    }

    if (strcmp(cmd, "SET") == 0 || strcmp(cmd, "GET") == 0 || strcmp(cmd, "DEL") == 0) {
        char *key = strtok_r(NULL, " ", &save);
        char *val = strtok_r(NULL, "", &save);           /* SET only */
        char op   = (cmd[0] == 'G') ? 'r' : 'w';

        if (!key) { n = snprintf(resp, respcap, "ERR usage %s <key> ...", cmd); goto done; }
        if (strlen(key) > MAX_KEY) { n = snprintf(resp, respcap, "ERR key too long"); goto done; }

        if (sec) {                                        /* permission gate */
            char ns[64]; extract_ns(key, ns, sizeof(ns));
            if (!perm_check(sec->perm, ns, c->user, c->group, op)) {
                sec_audit(sec, c->user, cmd, key, "DENIED");
                n = snprintf(resp, respcap, "ERR perm"); goto done;
            }
        }

        const char *result = "OK";
        if (cmd[0] == 'S') {                              /* SET */
            if (!val) { n = snprintf(resp, respcap, "ERR usage SET <key> <value>"); goto done; }
            int rc = db_set(db, key, val, (uint32_t)strlen(val));
            result = (rc == DK_OK) ? "OK" : (rc == DK_TOOBIG ? "ERR-size" : "ERR-io");
            n = snprintf(resp, respcap, "%s", rc == DK_OK ? "OK" :
                         rc == DK_TOOBIG ? "ERR value too large" : "ERR io");
        } else if (cmd[0] == 'G') {                       /* GET */
            static __thread char vbuf[PROTO_MAX_PAYLOAD];
            uint32_t vl = 0;
            int rc = db_get(db, key, vbuf, sizeof(vbuf), &vl);
            if (rc == DK_OK) { n = snprintf(resp, respcap, "OK %.*s", (int)vl, vbuf); result = "OK"; }
            else             { n = snprintf(resp, respcap, "ERR notfound"); result = "NOTFOUND"; }
        } else {                                          /* DEL */
            int rc = db_del(db, key);
            n = snprintf(resp, respcap, "%s", rc == DK_OK ? "OK" : "ERR notfound");
            result = (rc == DK_OK) ? "OK" : "NOTFOUND";
        }
        if (sec) sec_audit(sec, c->user, cmd, key, result);
        goto done;
    }

    n = snprintf(resp, respcap, "ERR unknown command");

done:
    free(big);
    return n < 0 ? 0 : (size_t)n;
}

/* ---- per-connection session (runs on a worker thread) ------------------ */

static void serve_connection(void *arg)
{
    Conn *c = arg;
    static __thread char req[PROTO_MAX_PAYLOAD];
    static __thread char resp[PROTO_MAX_PAYLOAD];

    for (;;) {
        uint32_t len = 0;
        int rc = frame_read(c->fd, req, sizeof(req), &len);
        if (rc == -2) { frame_write(c->fd, "ERR frame too large", 19); break; }
        if (rc != 0) break;

        size_t rlen = handle_command(c, req, len, resp, sizeof(resp));
        if (frame_write(c->fd, resp, (uint32_t)rlen) != 0) break;
        if (len >= 4 && strncmp(req, "QUIT", 4) == 0) break;
    }
    close(c->fd);
    free(c);
}

/* ---- accept loop ------------------------------------------------------- */

int server_run(const char *sock_path, DB *db, int nworkers)
{
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    SecCtx *sec = getenv("DURAKV_SECURE") ? sec_create() : NULL;

    int lfd = unix_listen(sock_path, 64);
    if (lfd < 0) { perror("unix_listen"); sec_destroy(sec); return -1; }

    ThreadPool *tp = threadpool_create(nworkers, 128);
    fprintf(stderr,
            "\n  DuraKV server is ready.\n"
            "    socket : %s\n"
            "    workers: %d\n"
            "    mode   : %s\n"
            "    connect with:  ./durakv-client %s\n"
            "    (press Ctrl-C to stop)\n\n",
            sock_path, nworkers, sec ? "SECURE (login required)" : "open", sock_path);

    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (g_stop) break;
            continue;
        }
        Conn *c = calloc(1, sizeof(*c));
        c->fd = cfd; c->db = db; c->sec = sec;
        threadpool_submit(tp, serve_connection, c);
    }

    fprintf(stderr, "\n  DuraKV server stopped.\n");
    close(lfd);
    unlink(sock_path);
    threadpool_shutdown(tp);
    sec_destroy(sec);
    return 0;
}

#ifndef DURAKV_SERVER_NO_MAIN
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <socket-path> [data.db] [wal.log] [workers]\n"
            "  env: DURAKV_FRAMES=<n>  DURAKV_POLICY=fifo|lru  DURAKV_SECURE=1\n", argv[0]);
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

    const char *pw = getenv("DURAKV_PASSWORD");
    DB *db = pw ? db_open_secure(data, wal, frames ? frames : 64, pol, pw)
                : db_open_ex(data, wal, frames ? frames : 64, pol);
    if (!db) { fprintf(stderr, "failed to open store\n"); return 1; }

    int rc = server_run(sock, db, workers);
    db_close(db);
    return rc == 0 ? 0 : 1;
}
#endif
