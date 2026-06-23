/*
 * scheduler.h -- Round-robin dispatcher across per-client queues (Phase 3).
 *
 * OS/systems primitive: round-robin scheduling, fairness / anti-starvation.
 *
 * Instead of one FCFS queue (where one heavy client can monopolise service),
 * each client has its own queue. scheduler_next() services exactly one request
 * from the current client, then rotates to the next non-empty client. A client
 * that floods the system therefore cannot starve the others -- demonstrable
 * fairness, not a toy.
 */
#ifndef DURAKV_SCHEDULER_H
#define DURAKV_SCHEDULER_H

#include <stddef.h>

typedef struct Scheduler Scheduler;

Scheduler *scheduler_create(int nclients);
void       scheduler_destroy(Scheduler *s);

/* Append a request (an opaque pointer) to a client's queue. */
void scheduler_enqueue(Scheduler *s, int client_id, void *request);

/* Pop the next request in round-robin order. Returns 1 and fills *client_out
 * / *req_out on success, or 0 when every queue is empty. */
int  scheduler_next(Scheduler *s, int *client_out, void **req_out);

size_t scheduler_pending(const Scheduler *s);   /* total queued across clients */

#endif /* DURAKV_SCHEDULER_H */
