/*
 * protocol.c -- the wire protocol and socket helpers for Task 4 (client-server
 * IPC over AF_UNIX Unix-domain sockets).
 *
 * A SOCK_STREAM socket is a BYTE STREAM with no message boundaries: one write()
 * may be split across several read()s, or several writes coalesced into one
 * read. So we cannot assume "one read == one message". Two mechanisms fix this:
 *
 *  1. Exact-length I/O (io_read_all/io_write_all) loops until the requested
 *     number of bytes has actually been transferred, transparently handling
 *     partial transfers and retrying on EINTR (interrupted syscalls).
 *
 *  2. Length-prefixed framing (frame_write/frame_read): every message is
 *     preceded by a 4-byte big-endian length, so the reader knows exactly how
 *     many payload bytes to expect. Big-endian is "network byte order", the
 *     conventional on-the-wire choice. A declared length over the cap or the
 *     protocol maximum is rejected as a protocol violation (input validation --
 *     it stops a malicious/oversized frame from overrunning the buffer).
 *
 * The remaining helpers wrap the AF_UNIX socket lifecycle (listen/connect on a
 * filesystem path). Unix-domain sockets are used instead of TCP/IP because this
 * is LOCAL IPC: no network stack, lower overhead, and access is governed by
 * filesystem permissions on the socket path. See include/protocol.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ---- exact-length I/O (handles partial read/write) --------------------- */

/* Read exactly n bytes into buf, looping over partial reads. Returns 0 on
 * success, or -1 if the peer closed the connection or an error occurred. */
int io_read_all(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return -1;                 /* peer closed */
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 0;
}

/* Write exactly n bytes from buf, looping over partial writes. Returns 0 on
 * success, -1 on error. */
int io_write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, p + put, n - put);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        put += (size_t)w;
    }
    return 0;
}

/* ---- big-endian length prefix ------------------------------------------ */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* Send one framed message: the 4-byte length header followed by the payload. */
int frame_write(int fd, const void *payload, uint32_t len)
{
    uint8_t hdr[4];
    put_be32(hdr, len);
    if (io_write_all(fd, hdr, 4) != 0) return -1;
    if (len && io_write_all(fd, payload, len) != 0) return -1;
    return 0;
}

/* Receive one framed message into buf. Returns 0 on success (payload length in
 * *len), -1 on connection error/close, or -2 if the advertised length exceeds
 * the caller's buffer or the protocol maximum (rejected without reading it). */
int frame_read(int fd, void *buf, uint32_t cap, uint32_t *len)
{
    uint8_t hdr[4];
    if (io_read_all(fd, hdr, 4) != 0) return -1;
    uint32_t n = get_be32(hdr);
    if (n > cap || n > PROTO_MAX_PAYLOAD) return -2;   /* protocol violation */
    if (n && io_read_all(fd, buf, n) != 0) return -1;
    *len = n;
    return 0;
}

/* ---- AF_UNIX endpoint helpers ------------------------------------------ */

static int fill_addr(struct sockaddr_un *a, const char *path)
{
    if (strlen(path) >= sizeof(a->sun_path)) { errno = ENAMETOOLONG; return -1; }
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strncpy(a->sun_path, path, sizeof(a->sun_path) - 1);
    return 0;
}

/* Create a listening AF_UNIX socket bound to `path`. Returns the socket fd, or
 * -1 on error. */
int unix_listen(const char *path, int backlog)
{
    struct sockaddr_un addr;
    if (fill_addr(&addr, path) != 0) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    unlink(path);                              /* remove any stale socket node */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    if (listen(fd, backlog) != 0) { close(fd); return -1; }
    return fd;
}

/* Connect to an AF_UNIX server listening at `path`. Returns a connected fd, or
 * -1 on error. */
int unix_connect(const char *path)
{
    struct sockaddr_un addr;
    if (fill_addr(&addr, path) != 0) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}
