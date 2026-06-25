/*
 * test_secure.c -- end-to-end test of the SECURE server: authentication gate,
 * namespace rwx permission enforcement, and an intact audit chain.
 *
 * The forked server runs with DURAKV_SECURE=1. Clients then prove:
 *   - data commands are refused before AUTH;
 *   - alice (owner of "alice", mode 0640) can read+write it;
 *   - bob (group staff) can READ alice's namespace but not WRITE it;
 *   - a wrong password is rejected;
 *   - the audit log of all this verifies cleanly (hash chain intact).
 */
#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "protocol.h"
#include "audit.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define SOCK "/tmp/durakv_sec.sock"
#define DATA "/tmp/durakv_sec.db"
#define WAL  "/tmp/durakv_sec.log"
#define AUD  "/tmp/durakv_sec.audit"

static int rpc(int fd, const char *req, char *resp, size_t cap)
{
    if (frame_write(fd, req, (uint32_t)strlen(req)) != 0) return -1;
    uint32_t rlen = 0;
    if (frame_read(fd, resp, (uint32_t)cap - 1, &rlen) != 0) return -1;
    resp[rlen] = '\0';
    return 0;
}

int main(void)
{
    unlink(DATA); unlink(WAL); unlink(SOCK); unlink(AUD);
    setenv("DURAKV_SECURE", "1", 1);
    setenv("DURAKV_AUDIT", AUD, 1);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        DB *db = db_open(DATA, WAL);
        if (!db) _exit(3);
        server_run(SOCK, db, 4);
        db_close(db);
        _exit(0);
    }
    for (int i = 0; i < 200 && access(SOCK, F_OK) != 0; i++) usleep(10000);
    assert(access(SOCK, F_OK) == 0);

    char r[256];

    /* alice: auth gate + owner rw + denied on bob's namespace */
    int fd = unix_connect(SOCK); assert(fd >= 0);
    assert(rpc(fd, "SET alice:x hi", r, sizeof(r)) == 0 && strcmp(r, "ERR auth required") == 0);
    assert(rpc(fd, "AUTH alice alice-secret", r, sizeof(r)) == 0 && strncmp(r, "OK", 2) == 0);
    assert(rpc(fd, "SET alice:bal 1000", r, sizeof(r)) == 0 && strcmp(r, "OK") == 0);
    assert(rpc(fd, "GET alice:bal", r, sizeof(r)) == 0 && strcmp(r, "OK 1000") == 0);
    assert(rpc(fd, "SET bob:y nope", r, sizeof(r)) == 0 && strcmp(r, "ERR perm") == 0);
    rpc(fd, "QUIT", r, sizeof(r)); close(fd);

    /* bob: group can READ alice's namespace but not WRITE it */
    fd = unix_connect(SOCK); assert(fd >= 0);
    assert(rpc(fd, "AUTH bob bob-secret", r, sizeof(r)) == 0 && strncmp(r, "OK", 2) == 0);
    assert(rpc(fd, "GET alice:bal", r, sizeof(r)) == 0 && strcmp(r, "OK 1000") == 0);
    assert(rpc(fd, "SET alice:bal 0", r, sizeof(r)) == 0 && strcmp(r, "ERR perm") == 0);
    rpc(fd, "QUIT", r, sizeof(r)); close(fd);

    /* wrong password is rejected */
    fd = unix_connect(SOCK); assert(fd >= 0);
    assert(rpc(fd, "AUTH alice wrong-pass", r, sizeof(r)) == 0 && strcmp(r, "ERR auth") == 0);
    rpc(fd, "QUIT", r, sizeof(r)); close(fd);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    /* the audit trail of everything above must verify cleanly */
    long bad = -1;
    assert(audit_verify(AUD, &bad) == 0);

    unlink(DATA); unlink(WAL); unlink(SOCK); unlink(AUD);
    printf("test_secure: PASS (auth gate, rwx allow/deny, audit chain intact)\n");
    return 0;
}
