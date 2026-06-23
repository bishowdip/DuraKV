/*
 * demo_scheduler.c -- round-robin fairness made visible.
 *
 * Three clients submit very unequal bursts (a heavy client and two light
 * ones). A single FCFS queue would serve the whole heavy burst first,
 * starving the others. The round-robin dispatcher instead interleaves them:
 * the printed service order shows each client getting a turn, and no client
 * waits for the heavy one to fully drain.
 */
#define _POSIX_C_SOURCE 200809L
#include "scheduler.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    enum { NCLIENTS = 3 };
    const int load[NCLIENTS] = { 10, 2, 4 };   /* client 0 is the heavy one */

    Scheduler *s = scheduler_create(NCLIENTS);

    /* enqueue each client's burst; the request payload encodes a sequence no. */
    for (int c = 0; c < NCLIENTS; c++)
        for (int i = 0; i < load[c]; i++)
            scheduler_enqueue(s, c, (void *)(long)(i + 1));

    int total = 0;
    for (int c = 0; c < NCLIENTS; c++) total += load[c];

    printf("loads: client0=%d client1=%d client2=%d  (total=%d)\n",
           load[0], load[1], load[2], total);
    printf("round-robin service order:\n  ");

    int served[NCLIENTS] = {0};
    int light_done_at = -1;   /* service number by which clients 1 & 2 finish */
    int client, n = 0;
    void *req;
    while (scheduler_next(s, &client, &req)) {
        printf("c%d ", client);
        served[client]++;
        n++;
        if (n % 30 == 0) printf("\n  ");

        if (light_done_at < 0 &&
            served[1] == load[1] && served[2] == load[2])
            light_done_at = n;
    }
    printf("\n");

    /* every request was serviced exactly once per client */
    for (int c = 0; c < NCLIENTS; c++) assert(served[c] == load[c]);

    /* fairness: the light clients finished well before the heavy client's
     * burst was exhausted -- not after it (which is what FCFS would do). */
    printf("light clients (1,2) fully served by service #%d of %d; "
           "heavy client still had work queued -> no starvation.\n",
           light_done_at, total);
    assert(light_done_at < total);

    scheduler_destroy(s);
    printf("\ndemo_scheduler: PASS\n");
    return 0;
}
