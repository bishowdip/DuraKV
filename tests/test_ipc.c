/*
 * test_ipc.c -- end-to-end test of the AF_UNIX server with CONCURRENT clients.
 *
 * A forked child runs the real server (server_run) over the storage engine.
 * The parent then opens many client connections from separate threads, each
 * doing SET/GET round-trips and verifying the framed responses. This exercises
 * the whole Phase 4 path: AF_UNIX sockets, length-prefixed framing, command
 * parsing, and concurrent service through the thread pool.
 */
#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

#define SOCK  "/tmp/durakv_ipc.sock"
#define DATA  "/tmp/durakv_ipc.db"
#define WAL   "/tmp/durakv_ipc.log"
#define NCLIENT 8
#define NOPS    50

/* one framed request -> one framed response (response NUL-terminated) */
static int rpc(int fd, const char *req, char *resp, size_t cap)
{
    if (frame_write(fd, req, (uint32_t)strlen(req)) != 0) return -1;
    uint32_t rlen = 0;
    if (frame_read(fd, resp, (uint32_t)cap - 1, &rlen) != 0) return -1;
    resp[rlen] = '\0';
    return 0;
}

static void *client_thread(void *arg)
{
    int id = (int)(long)arg;
    int fd = unix_connect(SOCK);
    assert(fd >= 0);

    char req[256], resp[256];
    for (int i = 0; i < NOPS; i++) {
        snprintf(req, sizeof(req), "SET c%d_k%d val_%d_%d", id, i, id, i);
        assert(rpc(fd, req, resp, sizeof(resp)) == 0);
        assert(strcmp(resp, "OK") == 0);

        snprintf(req, sizeof(req), "GET c%d_k%d", id, i);
        assert(rpc(fd, req, resp, sizeof(resp)) == 0);
        char expect[64]; snprintf(expect, sizeof(expect), "OK val_%d_%d", id, i);
        assert(strcmp(resp, expect) == 0);
    }
    rpc(fd, "QUIT", resp, sizeof(resp));
    close(fd);
    return NULL;
}

int main(void)
{
    unlink(DATA); unlink(WAL); unlink(SOCK);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {                         /* ---- server child ---- */
        DB *db = db_open(DATA, WAL);
        if (!db) _exit(3);
        server_run(SOCK, db, 4);
        db_close(db);
        _exit(0);
    }

    /* wait for the socket to appear (server is up) */
    for (int i = 0; i < 200 && access(SOCK, F_OK) != 0; i++) usleep(10000);
    assert(access(SOCK, F_OK) == 0);

    /* fire NCLIENT concurrent clients */
    pthread_t th[NCLIENT];
    for (int i = 0; i < NCLIENT; i++)
        pthread_create(&th[i], NULL, client_thread, (void *)(long)i);
    for (int i = 0; i < NCLIENT; i++)
        pthread_join(th[i], NULL);

    /* one more client confirms a key from another client persisted */
    int fd = unix_connect(SOCK);
    assert(fd >= 0);
    char resp[256];
    assert(rpc(fd, "GET c0_k0", resp, sizeof(resp)) == 0);
    assert(strcmp(resp, "OK val_0_0") == 0);
    rpc(fd, "QUIT", resp, sizeof(resp));
    close(fd);

    kill(pid, SIGTERM);                     /* ask the server to stop */
    waitpid(pid, NULL, 0);
    unlink(DATA); unlink(WAL); unlink(SOCK);

    printf("test_ipc: PASS (%d concurrent clients x %d ops over AF_UNIX)\n",
           NCLIENT, NOPS);
    return 0;
}
