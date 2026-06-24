/*
 * protocol.h -- wire framing and local-socket helpers (DuraKV Phase 4).
 *
 * OS/systems primitive: protocol design, message framing, socket lifecycle.
 *
 * Transport is a Unix domain socket (AF_UNIX) -- a local IPC endpoint, NOT
 * TCP/IP. Every message is length-prefixed:
 *
 *     [ uint32 big-endian length N ][ N bytes of payload ]
 *
 * Length-prefixing removes partial-read ambiguity: the reader knows exactly
 * how many bytes a message contains and loops until it has them all. The
 * payload itself is a human-readable command/response line (SET/GET/...).
 */
#ifndef DURAKV_PROTOCOL_H
#define DURAKV_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define PROTO_MAX_PAYLOAD (1u << 20)   /* 1 MiB cap on any single frame */

/* Loop until exactly n bytes are transferred. Return 0 on success, -1 on
 * error or premature EOF. */
int io_read_all(int fd, void *buf, size_t n);
int io_write_all(int fd, const void *buf, size_t n);

/* Write one length-prefixed frame. Return 0 on success, -1 on error. */
int frame_write(int fd, const void *payload, uint32_t len);

/* Read one frame into buf (capacity cap), setting *len. Returns:
 *    0  success
 *   -1  peer closed / I/O error
 *   -2  framed length exceeds cap or PROTO_MAX_PAYLOAD (protocol violation) */
int frame_read(int fd, void *buf, uint32_t cap, uint32_t *len);

/* AF_UNIX endpoint helpers. Return a connected/listening fd, or -1 on error. */
int unix_listen(const char *path, int backlog);
int unix_connect(const char *path);

#endif /* DURAKV_PROTOCOL_H */
