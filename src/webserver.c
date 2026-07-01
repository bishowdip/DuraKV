/*
 * webserver.c -- a pure-C HTTP bridge and process supervisor that lets the
 * DuraKV engine be driven from a web browser, with no terminal required.
 *
 * Why a bridge is needed: the graded client-server (Task 4) speaks a framed
 * protocol over an AF_UNIX (Unix-domain) socket -- deliberately NOT TCP/IP. A
 * web browser, however, can only speak HTTP over TCP. This program bridges the
 * two WITHOUT changing the engine:
 *
 *   Browser  --HTTP/TCP-->  webserver (this file)  --AF_UNIX framed-->  durakv-server
 *
 * So every action in the dashboard exercises the real store, the real buffer
 * pool and the real IPC path -- nothing is faked or re-implemented in the page.
 *
 * It is also a supervisor: it launches a genuine durakv-server as a child
 * process and can SIGKILL it on demand. That powers the "crash theatre" -- the
 * kill really is a kill -9 of a running process, and Restart re-opens the same
 * files so crash recovery (WAL redo/undo) brings the committed data back. This
 * is the headline demonstration of DuraKV's durability, made visible.
 *
 * Design notes: a single-threaded accept loop keeps the supervised child's PID
 * in one place (so kill/restart are race-free); the browser uses short polling
 * for live stats rather than a persistent stream, which keeps the server model
 * simple. Each HTTP connection handles one request then closes.
 *
 * NOT part of the graded IPC requirement -- this is an innovation layer on top
 * of the AF_UNIX server, which remains the assessed client-server application.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE      /* expose strcasestr on macOS */
#else
#define _GNU_SOURCE           /* expose strcasestr on glibc */
#endif
#include "protocol.h"
#include "crypto.h"        /* real AEAD + Argon2 primitives (Task 3) */
#include "auth.h"          /* real Argon2id login store              */
#include "audit.h"         /* real SHA-256 hash-chained audit log    */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_BUF   (PROTO_MAX_PAYLOAD + 4096)
#define DEFAULT_PORT 8080

/* ---- supervised durakv-server child ----------------------------------- */

static const char *g_sock  = "/tmp/durakv-web.sock";
static const char *g_data  = "webdemo.db";
static const char *g_wal   = "webdemo.wal";
static int         g_work  = 4;
static pid_t       g_child = -1;      /* the durakv-server we supervise */
static volatile sig_atomic_t g_stop = 0;

/* ---- security-demo state (uses the REAL security modules) -------------- */
static AuthStore  *g_auth = NULL;               /* Argon2id user store        */
static uint8_t     g_key[CRYPTO_KEYBYTES];      /* demo encryption key        */
static const char *g_auditpath = "web-audit.log";
static Audit      *g_audit = NULL;              /* hash-chained audit log     */

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Wait until the child's socket accepts a connection, or time out. */
static int wait_ready(int tries)
{
    for (int i = 0; i < tries; i++) {
        int fd = unix_connect(g_sock);
        if (fd >= 0) { close(fd); return 0; }
        struct timespec ts = { 0, 100 * 1000 * 1000 };  /* 100 ms */
        nanosleep(&ts, NULL);
    }
    return -1;
}

/* Launch a fresh durakv-server child on the AF_UNIX socket. */
static int spawn_child(void)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char workers[16];
        snprintf(workers, sizeof(workers), "%d", g_work);
        execl("./durakv-server", "durakv-server",
              g_sock, g_data, g_wal, workers, (char *)NULL);
        perror("exec durakv-server");   /* only reached if exec fails */
        _exit(127);
    }
    g_child = pid;
    return wait_ready(30);              /* up to ~3 s for it to come up */
}

/* SIGKILL the child (the real "kill -9") and reap it. */
static void kill_child(void)
{
    if (g_child > 0) {
        kill(g_child, SIGKILL);
        waitpid(g_child, NULL, 0);
        g_child = -1;
    }
}

/* Is the supervised server currently reachable? */
static int child_up(void)
{
    if (g_child <= 0) return 0;
    int fd = unix_connect(g_sock);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

/* ---- relay one wire command to the durakv-server ---------------------- */

/* Send `cmd` to the child over AF_UNIX and copy the reply into resp.
 * Returns 0 on success, -1 if the server is down/unreachable. */
static int relay(const char *cmd, char *resp, size_t rcap)
{
    int fd = unix_connect(g_sock);
    if (fd < 0) return -1;
    int rc = -1;
    if (frame_write(fd, cmd, (uint32_t)strlen(cmd)) == 0) {
        uint32_t n = 0;
        if (frame_read(fd, resp, (uint32_t)rcap - 1, &n) == 0) {
            resp[n] = '\0';
            rc = 0;
        }
    }
    close(fd);
    return rc;
}

/* ---- tiny HTTP helpers ------------------------------------------------- */

/* Escape a string into JSON-safe form (quotes, backslashes, control chars). */
static void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 7 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20)  { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c); }
        else                { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

/* Write a full HTTP response with the given status, content-type and body. */
static void http_send(int fd, const char *status, const char *ctype,
                      const char *body, size_t blen)
{
    char hdr[512];
    int h = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status, ctype, blen);
    io_write_all(fd, hdr, (size_t)h);
    if (blen) io_write_all(fd, body, blen);
}

static void http_json(int fd, const char *json)
{
    http_send(fd, "200 OK", "application/json", json, strlen(json));
}

/* Serve the dashboard file; fall back to a message if it is missing. */
static void serve_dashboard(int fd)
{
    FILE *f = fopen("web/dashboard.html", "rb");
    if (!f) {
        const char *msg = "<h1>DuraKV</h1><p>web/dashboard.html not found. "
                          "Run durakv-web from the project root.</p>";
        http_send(fd, "200 OK", "text/html", msg, strlen(msg));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    http_send(fd, "200 OK", "text/html", buf, got);
    free(buf);
}

/* ---- request routing --------------------------------------------------- */

/* Extract the body of a request that has already been fully read into buf. */
static const char *find_body(const char *buf)
{
    const char *p = strstr(buf, "\r\n\r\n");
    return p ? p + 4 : "";
}

/* ---- security demo handlers (drive the real crypto/auth/audit modules) - */

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2*i] = h[in[i] >> 4]; out[2*i+1] = h[in[i] & 15]; }
    out[2*n] = '\0';
}

/* Argon2id login: verify credentials against the real auth store. */
static void sec_login(int fd, const char *body)
{
    char user[64] = {0}, pass[128] = {0};
    sscanf(body, "%63s %127s", user, pass);
    int sid = auth_login(g_auth, user, pass);
    char out[128];
    snprintf(out, sizeof(out),
        "{\"ok\":%s,\"msg\":\"%s\"}",
        sid > 0 ? "true" : "false",
        sid > 0 ? "authenticated (Argon2id verified)" : "denied (bad user or password)");
    http_json(fd, out);
}

/* AEAD seal: encrypt the value, return the ciphertext (unreadable) and prove
 * both confidentiality (plaintext absent) and integrity (a 1-byte flip fails). */
static void sec_seal(int fd, const char *body)
{
    size_t plen = strlen(body);
    if (plen == 0) { http_json(fd, "{\"ok\":false,\"msg\":\"empty\"}"); return; }
    if (plen > 4096) plen = 4096;

    uint8_t sealed[4096 + CRYPTO_SEAL_OVERHEAD];
    size_t  slen = crypto_seal(sealed, (const uint8_t *)body, plen, g_key);

    char cipher_hex[2 * sizeof(sealed) + 1];
    to_hex(sealed, slen, cipher_hex);

    /* honest integrity demo: flip one ciphertext byte, decryption must fail */
    uint8_t tampered[sizeof(sealed)];
    memcpy(tampered, sealed, slen);
    tampered[slen - 1] ^= 0x01;
    uint8_t rec[4096];
    long ok_open  = crypto_open(rec, sealed,   slen, g_key);   /* clean: succeeds */
    long bad_open = crypto_open(rec, tampered, slen, g_key);   /* tampered: -1    */

    char pe[4096 * 6 + 8];
    json_escape(body, pe, sizeof(pe));
    char out[2 * sizeof(sealed) + 4096 * 6 + 256];
    snprintf(out, sizeof(out),
        "{\"ok\":true,\"plaintext\":\"%s\",\"cipher_hex\":\"%s\","
        "\"cipher_len\":%zu,\"clean_open_ok\":%s,\"tamper_detected\":%s}",
        pe, cipher_hex, slen,
        ok_open >= 0 ? "true" : "false",
        bad_open < 0 ? "true" : "false");
    http_json(fd, out);
}

/* Read a whole file into a malloc'd, NUL-terminated buffer (caller frees). */
static char *read_file(const char *path, long *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1);
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[got] = '\0';
    if (len_out) *len_out = (long)got;
    return b;
}

/* Append one entry to the real hash-chained audit log. */
static void sec_audit_append(int fd, const char *body)
{
    char user[64] = "alice", op[16] = "SET", key[64] = "account:1", res[16] = "OK";
    sscanf(body, "%63s %15s %63s %15s", user, op, key, res);
    audit_append(g_audit, user, op, key, res);
    http_json(fd, "{\"ok\":true}");
}

/* Return the raw log so the browser can show the chain (hashes included). */
static void sec_audit_log(int fd)
{
    long n = 0;
    char *raw = read_file(g_auditpath, &n);
    char esc[16384];
    json_escape(raw ? raw : "", esc, sizeof(esc));
    free(raw);
    char out[16384 + 64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"log\":\"%s\"}", esc);
    http_json(fd, out);
}

/* Verify the chain and report the first broken sequence number, if any. */
static void sec_audit_verify(int fd)
{
    long bad = -1;
    int rc = audit_verify(g_auditpath, &bad);
    char out[96];
    snprintf(out, sizeof(out), "{\"ok\":true,\"intact\":%s,\"bad_seq\":%ld}",
             rc == 0 ? "true" : "false", bad);
    http_json(fd, out);
}

/* Tamper: silently alter one hashed byte of the first entry (in place, same
 * length) so the recomputed hash no longer matches -- exactly what audit_verify
 * is designed to catch. */
static void sec_audit_tamper(int fd)
{
    long n = 0;
    char *raw = read_file(g_auditpath, &n);
    if (!raw || n == 0) { free(raw); http_json(fd, "{\"ok\":false,\"msg\":\"log empty\"}"); return; }

    /* offset of field 3 (user) on line 1 = char after the 2nd tab */
    long off = -1; int tabs = 0;
    for (long i = 0; raw[i] && raw[i] != '\n'; i++)
        if (raw[i] == '\t') { if (++tabs == 2) { off = i + 1; break; } }

    if (off < 0) { free(raw); http_json(fd, "{\"ok\":false,\"msg\":\"no entry\"}"); return; }

    FILE *f = fopen(g_auditpath, "rb+");
    if (f) { fseek(f, off, SEEK_SET); fputc(raw[off] ^ 0x01, f); fclose(f); }
    free(raw);
    http_json(fd, "{\"ok\":true,\"msg\":\"entry #1 secretly altered\"}");
}

/* Discard and recreate the audit log (fresh genesis). */
static void sec_audit_reset(int fd)
{
    if (g_audit) audit_close(g_audit);
    unlink(g_auditpath);
    g_audit = audit_open(g_auditpath);
    http_json(fd, "{\"ok\":true}");
}

static void handle_request(int fd, char *buf)
{
    char method[8] = {0}, path[256] = {0};
    sscanf(buf, "%7s %255s", method, path);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        serve_dashboard(fd);
        return;
    }

    if (strcmp(path, "/api/health") == 0) {
        char json[64];
        snprintf(json, sizeof(json), "{\"up\":%s}", child_up() ? "true" : "false");
        http_json(fd, json);
        return;
    }

    if (strcmp(path, "/api/kill") == 0) {
        kill_child();
        http_json(fd, "{\"ok\":true,\"killed\":true}");
        return;
    }

    if (strcmp(path, "/api/restart") == 0) {
        kill_child();
        int rc = spawn_child();
        char json[64];
        snprintf(json, sizeof(json), "{\"ok\":%s}", rc == 0 ? "true" : "false");
        http_json(fd, json);
        return;
    }

    /* ---- security demo endpoints ---- */
    if (strcmp(path, "/api/sec/login") == 0)        { sec_login(fd, find_body(buf)); return; }
    if (strcmp(path, "/api/sec/seal") == 0)         { sec_seal(fd, find_body(buf)); return; }
    if (strcmp(path, "/api/sec/audit/append") == 0) { sec_audit_append(fd, find_body(buf)); return; }
    if (strcmp(path, "/api/sec/audit/log") == 0)    { sec_audit_log(fd); return; }
    if (strcmp(path, "/api/sec/audit/verify") == 0) { sec_audit_verify(fd); return; }
    if (strcmp(path, "/api/sec/audit/tamper") == 0) { sec_audit_tamper(fd); return; }
    if (strcmp(path, "/api/sec/audit/reset") == 0)  { sec_audit_reset(fd); return; }

    if (strcmp(path, "/api/cmd") == 0) {
        const char *body = find_body(buf);
        char resp[PROTO_MAX_PAYLOAD];
        char esc[PROTO_MAX_PAYLOAD + 64];
        char out[PROTO_MAX_PAYLOAD + 128];
        if (relay(body, resp, sizeof(resp)) == 0) {
            json_escape(resp, esc, sizeof(esc));
            snprintf(out, sizeof(out), "{\"ok\":true,\"resp\":\"%s\"}", esc);
        } else {
            snprintf(out, sizeof(out),
                     "{\"ok\":false,\"resp\":\"server is down\"}");
        }
        http_json(fd, out);
        return;
    }

    http_send(fd, "404 Not Found", "text/plain", "not found", 9);
}

/* Read a whole HTTP request (headers + Content-Length body) into buf. */
static int read_request(int fd, char *buf, size_t cap)
{
    size_t got = 0;
    ssize_t n;
    /* read at least until the header terminator */
    while (got < cap - 1) {
        n = read(fd, buf + got, cap - 1 - got);
        if (n <= 0) return got ? 0 : -1;
        got += (size_t)n;
        buf[got] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (!hdr_end) continue;                 /* headers not complete yet */

        /* honour Content-Length so POST bodies arrive in full */
        size_t need = 0;
        char *cl = strcasestr(buf, "Content-Length:");
        if (cl) need = (size_t)strtoul(cl + 15, NULL, 10);
        size_t have_body = got - (size_t)(hdr_end + 4 - buf);
        if (have_body >= need) break;
    }
    return 0;
}

/* ---- accept loop ------------------------------------------------------- */

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    if (argc >= 2) port = atoi(argv[1]);
    if (argc >= 3) g_data = argv[2];
    if (argc >= 4) g_wal  = argv[3];

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);          /* auto-reap any stray children */
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Security demo: initialise the real security modules. A fixed salt keeps
     * the demo key deterministic across restarts; the audit log starts fresh. */
    if (crypto_init() != 0) { fprintf(stderr, "libsodium init failed\n"); return 1; }
    g_auth = auth_create();
    auth_register(g_auth, "alice", "alice-secret", "staff");
    auth_register(g_auth, "bob",   "bob-secret",   "staff");
    uint8_t salt[CRYPTO_SALTBYTES] = {0};
    crypto_derive_key(g_key, "DuraKV demo master key", salt);
    unlink(g_auditpath);                 /* fresh chain each run */
    g_audit = audit_open(g_auditpath);

    if (spawn_child() != 0) {
        fprintf(stderr, "could not start durakv-server (is it built? run 'make')\n");
        return 1;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* localhost only */
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind"); kill_child(); return 1;
    }
    listen(lfd, 16);

    fprintf(stderr,
        "\n  DuraKV web dashboard is ready.\n"
        "    open:   http://127.0.0.1:%d   (or http://localhost:%d)\n"
        "    engine: durakv-server (AF_UNIX %s), %d workers\n"
        "    files:  %s / %s\n"
        "    (press Ctrl-C to stop)\n\n",
        port, port, g_sock, g_work, g_data, g_wal);

    char *buf = malloc(HTTP_BUF);
    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; else break; }
        if (read_request(cfd, buf, HTTP_BUF) == 0)
            handle_request(cfd, buf);
        close(cfd);
    }

    free(buf);
    close(lfd);
    kill_child();
    unlink(g_sock);
    if (g_audit) audit_close(g_audit);
    if (g_auth)  auth_destroy(g_auth);
    fprintf(stderr, "\n  DuraKV web dashboard stopped.\n");
    return 0;
}
