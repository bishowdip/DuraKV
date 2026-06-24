/*
 * client.c -- interactive AF_UNIX client for DuraKV (Phase 4).
 *
 * Connects to the server's Unix domain socket and runs a request/response
 * loop: each stdin line is sent as one length-prefixed frame and the reply is
 * printed. Works interactively or with piped input (for scripts/tests).
 */
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <socket-path>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    int fd = unix_connect(argv[1]);
    if (fd < 0) { perror("connect"); return 1; }

    char *line = NULL;
    size_t cap = 0;
    static char resp[PROTO_MAX_PAYLOAD];

    while (getline(&line, &cap, stdin) > 0) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
        if (len == 0) continue;

        if (frame_write(fd, line, (uint32_t)len) != 0) {
            fprintf(stderr, "send failed (server gone?)\n");
            break;
        }
        uint32_t rlen = 0;
        int rc = frame_read(fd, resp, sizeof(resp), &rlen);
        if (rc != 0) { fprintf(stderr, "no response (server closed)\n"); break; }
        printf("%.*s\n", (int)rlen, resp);
        fflush(stdout);

        if (len >= 4 && strncmp(line, "QUIT", 4) == 0) break;
    }

    free(line);
    close(fd);
    return 0;
}
