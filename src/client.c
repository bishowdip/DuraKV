/*
 * client.c -- AF_UNIX client for DuraKV (Phase 4).
 *
 * From a terminal it shows a friendly guided menu (with a Login option for
 * secure servers). With piped input it falls back to a raw request/response
 * loop (one line per request), so scripts and tests are unaffected.
 */
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>

#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_DIM   "\033[2m"
#define C_GREEN "\033[32m"
#define C_RED   "\033[31m"
#define C_CYAN  "\033[36m"
#define C_YEL   "\033[33m"

/* Holds the most recent server reply (NUL-terminated) for the caller to inspect. */
static char g_resp[PROTO_MAX_PAYLOAD];

/* One round-trip of the request/response protocol: frame the request, read the
 * framed reply, NUL-terminate it. Returns 0 on success, -1 if the connection
 * was lost. This is the single point every command goes through. */
static int rpc(int fd, const char *req)
{
    if (frame_write(fd, req, (uint32_t)strlen(req)) != 0) return -1;
    uint32_t rl = 0;
    if (frame_read(fd, g_resp, sizeof(g_resp) - 1, &rl) != 0) return -1;
    g_resp[rl] = '\0';
    return 0;
}

/* Print a prompt and read one line, stripping the trailing newline. Returns 0
 * on EOF (e.g. Ctrl-D) so callers can exit cleanly. */
static int ask(const char *prompt, char *buf, size_t cap)
{
    fputs(prompt, stdout); fflush(stdout);
    if (!fgets(buf, (int)cap, stdin)) return 0;
    buf[strcspn(buf, "\n")] = '\0';
    return 1;
}

/* ---- friendly menu (terminal) ----------------------------------------- */

/* Interactive front-end: a numbered menu that turns each choice into a protocol
 * command via rpc() and translates the raw reply into friendly, colour-coded
 * feedback (so a non-technical user never has to type wire commands). */
static void client_menu(int fd, const char *sock)
{
    char choice[64], key[1024], user[128], pass[128];
    static char val[1 << 16];

    printf("\n" C_CYAN C_BOLD "  DuraKV client" C_RESET
           C_DIM "  \xE2\x80\x94 connected to %s\n" C_RESET, sock);

    for (;;) {
        printf("\n" C_BOLD "  What would you like to do?" C_RESET "\n");
        printf("    " C_GREEN "1" C_RESET ") Set a value     "
               "    " C_GREEN "4" C_RESET ") Log in (secure servers)\n");
        printf("    " C_GREEN "2" C_RESET ") Get a value     "
               "    " C_GREEN "5" C_RESET ") Server stats\n");
        printf("    " C_GREEN "3" C_RESET ") Delete a key    "
               "    " C_GREEN "6" C_RESET ") Ping server\n");
        printf("    " C_GREEN "0" C_RESET ") Quit\n");

        if (!ask("\n  choose [0-6]: ", choice, sizeof(choice))) { printf("\n"); break; }
        if (!*choice) continue;

        int rc = 0;
        if (!strcmp(choice, "1") || !strcasecmp(choice, "set")) {
            if (!ask("    key:   ", key, sizeof(key)) || !*key) continue;
            if (!ask("    value: ", val, sizeof(val))) break;
            char req[1 << 16];
            snprintf(req, sizeof(req), "SET %s %s", key, val);
            rc = rpc(fd, req);
            if (!rc) {
                if (!strcmp(g_resp, "OK"))
                    printf(C_GREEN "    \xE2\x9C\x93 stored\n" C_RESET);
                else if (!strcmp(g_resp, "ERR perm"))
                    printf(C_RED "    \xE2\x9C\x97 permission denied\n" C_RESET);
                else if (!strcmp(g_resp, "ERR auth required"))
                    printf(C_YEL "    please log in first (option 4)\n" C_RESET);
                else printf("    %s\n", g_resp);
            }
        } else if (!strcmp(choice, "2") || !strcasecmp(choice, "get")) {
            if (!ask("    key: ", key, sizeof(key)) || !*key) continue;
            char req[1100];
            snprintf(req, sizeof(req), "GET %s", key);
            rc = rpc(fd, req);
            if (!rc) {
                if (!strncmp(g_resp, "OK ", 3))
                    printf(C_GREEN "    \xE2\x9C\x93 %s = \"%s\"\n" C_RESET, key, g_resp + 3);
                else if (!strcmp(g_resp, "ERR notfound"))
                    printf(C_YEL "    \xE2\x80\x94 \"%s\" not found\n" C_RESET, key);
                else if (!strcmp(g_resp, "ERR auth required"))
                    printf(C_YEL "    please log in first (option 4)\n" C_RESET);
                else printf("    %s\n", g_resp);
            }
        } else if (!strcmp(choice, "3") || !strcasecmp(choice, "del")) {
            if (!ask("    key to delete: ", key, sizeof(key)) || !*key) continue;
            char req[1100];
            snprintf(req, sizeof(req), "DEL %s", key);
            rc = rpc(fd, req);
            if (!rc) printf(!strcmp(g_resp, "OK")
                            ? C_GREEN "    \xE2\x9C\x93 deleted\n" C_RESET
                            : C_YEL  "    \xE2\x80\x94 %s\n" C_RESET, g_resp);
        } else if (!strcmp(choice, "4") || !strcasecmp(choice, "login")) {
            if (!ask("    username: ", user, sizeof(user)) || !*user) continue;
            if (!ask("    password: ", pass, sizeof(pass))) break;
            char req[300];
            snprintf(req, sizeof(req), "AUTH %s %s", user, pass);
            rc = rpc(fd, req);
            if (!rc) printf(!strncmp(g_resp, "OK", 2)
                            ? C_GREEN "    \xE2\x9C\x93 logged in as %s\n" C_RESET
                            : C_RED   "    \xE2\x9C\x97 login failed\n" C_RESET, user);
        } else if (!strcmp(choice, "5") || !strcasecmp(choice, "stats")) {
            rc = rpc(fd, "STATS");
            if (!rc) printf("    %s\n", g_resp);
        } else if (!strcmp(choice, "6") || !strcasecmp(choice, "ping")) {
            rc = rpc(fd, "PING");
            if (!rc) printf(!strcmp(g_resp, "PONG")
                            ? C_GREEN "    \xE2\x9C\x93 server is alive\n" C_RESET
                            : "    %s\n", g_resp);
        } else if (!strcmp(choice, "0") || !strcasecmp(choice, "quit") ||
                   !strcasecmp(choice, "q")) {
            rpc(fd, "QUIT");
            printf(C_CYAN "  goodbye.\n" C_RESET);
            break;
        } else {
            printf(C_RED "    please choose a number from 0 to 6\n" C_RESET);
            continue;
        }
        if (rc) { printf(C_RED "  lost connection to server.\n" C_RESET); break; }
    }
}

/* ---- raw loop (piped input) ------------------------------------------- */

/* Non-interactive front-end: read one wire command per line from stdin and echo
 * each reply. This keeps scripts and the automated tests (which pipe commands
 * in) working unchanged, independent of the menu. */
static void client_raw(int fd)
{
    char *line = NULL; size_t cap = 0;
    while (getline(&line, &cap, stdin) > 0) {
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) len--;
        if (!len) continue;
        line[len] = '\0';
        if (rpc(fd, line) != 0) { fprintf(stderr, "server closed\n"); break; }
        printf("%s\n", g_resp);
        fflush(stdout);
        if (!strncmp(line, "QUIT", 4)) break;
    }
    free(line);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <socket-path>\n", argv[0]); return 2; }
    signal(SIGPIPE, SIG_IGN);

    int fd = unix_connect(argv[1]);
    if (fd < 0) {
        fprintf(stderr, C_RED "could not connect to %s "
                "(is durakv-server running?)\n" C_RESET, argv[1]);
        return 1;
    }

    /* Choose the front-end by whether stdin is a real terminal: a person gets
     * the guided menu, a pipe/script gets the raw line protocol. */
    if (isatty(STDIN_FILENO)) client_menu(fd, argv[1]);
    else                      client_raw(fd);

    close(fd);
    return 0;
}
