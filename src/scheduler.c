/*
 * scheduler.c -- per-client FIFO queues serviced in round-robin order.
 * See include/scheduler.h. Thread-safe (one mutex) so the acceptor and the
 * dispatcher can run in different threads.
 */
#define _POSIX_C_SOURCE 200809L
#include "scheduler.h"

#include <stdlib.h>
#include <pthread.h>

/* singly-linked FIFO node */
typedef struct Node { void *req; struct Node *next; } Node;

typedef struct {
    Node *head, *tail;   /* this client's pending requests */
    size_t len;
} ClientQ;

struct Scheduler {
    ClientQ        *clients;
    int             nclients;
    int             cursor;     /* whose turn is next */
    size_t          pending;
    pthread_mutex_t mtx;
};

Scheduler *scheduler_create(int nclients)
{
    if (nclients < 1) nclients = 1;
    Scheduler *s = calloc(1, sizeof(*s));
    s->nclients = nclients;
    s->clients  = calloc(nclients, sizeof(ClientQ));
    s->cursor   = 0;
    pthread_mutex_init(&s->mtx, NULL);
    return s;
}

void scheduler_destroy(Scheduler *s)
{
    if (!s) return;
    for (int i = 0; i < s->nclients; i++) {
        Node *n = s->clients[i].head;
        while (n) { Node *nx = n->next; free(n); n = nx; }
    }
    pthread_mutex_destroy(&s->mtx);
    free(s->clients);
    free(s);
}

void scheduler_enqueue(Scheduler *s, int client_id, void *request)
{
    if (client_id < 0 || client_id >= s->nclients) return;
    Node *n = malloc(sizeof(*n));
    n->req = request; n->next = NULL;

    pthread_mutex_lock(&s->mtx);
    ClientQ *q = &s->clients[client_id];
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n;
    q->len++;
    s->pending++;
    pthread_mutex_unlock(&s->mtx);
}

int scheduler_next(Scheduler *s, int *client_out, void **req_out)
{
    pthread_mutex_lock(&s->mtx);
    if (s->pending == 0) { pthread_mutex_unlock(&s->mtx); return 0; }

    /* advance the cursor to the next client that actually has work */
    for (int step = 0; step < s->nclients; step++) {
        int c = (s->cursor + step) % s->nclients;
        ClientQ *q = &s->clients[c];
        if (q->len == 0) continue;

        Node *n = q->head;
        q->head = n->next;
        if (!q->head) q->tail = NULL;
        q->len--;
        s->pending--;
        s->cursor = (c + 1) % s->nclients;     /* rotate past the one we served */

        if (client_out) *client_out = c;
        if (req_out)    *req_out = n->req;
        free(n);
        pthread_mutex_unlock(&s->mtx);
        return 1;
    }
    pthread_mutex_unlock(&s->mtx);
    return 0;   /* unreachable: pending>0 implies some queue is non-empty */
}

size_t scheduler_pending(const Scheduler *s) { return s->pending; }
