/*
 * server.h -- AF_UNIX request server over the storage engine (Phase 4).
 *
 * OS/systems primitive: sockets, connection management, concurrent clients.
 *
 * server_run() listens on a Unix domain socket and hands each accepted
 * connection to the thread pool, so many clients are served concurrently. It
 * returns when a SIGINT/SIGTERM asks it to stop, after draining workers.
 */
#ifndef DURAKV_SERVER_H
#define DURAKV_SERVER_H

#include "storage.h"

int server_run(const char *sock_path, DB *db, int nworkers);

#endif /* DURAKV_SERVER_H */
