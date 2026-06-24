/*
 * demo_mqueue.c -- inter-process "message sharing" with a System V message
 * queue (Task 4 IPC).
 *
 * OS/systems primitive: message-queue IPC (msgget / msgsnd / msgrcv).
 *
 * Unlike a byte-stream socket, a message queue preserves message *boundaries*
 * and lets the receiver pick messages by TYPE. A parent and a forked child
 * share one kernel queue: the child sends, the parent receives. We also show
 * type-selective receive (pull the "priority" type 2 ahead of type 1).
 *
 * System V IPC is used (not POSIX mq_*) because it is available on both macOS
 * and Linux; macOS does not implement POSIX message queues.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#define MAXTEXT 128

typedef struct { long mtype; char mtext[MAXTEXT]; } Msg;

#define T_NORMAL   1
#define T_PRIORITY 2

int main(void)
{
    int qid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (qid < 0) { perror("msgget"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* ---- child: producer ---- */
        const char *body[] = { "hello", "from", "the", "child" };
        for (int i = 0; i < 4; i++) {
            Msg m; m.mtype = T_NORMAL;
            snprintf(m.mtext, MAXTEXT, "%s", body[i]);
            msgsnd(qid, &m, strlen(m.mtext) + 1, 0);
        }
        /* one high-priority message sent LAST but meant to be read FIRST */
        Msg p; p.mtype = T_PRIORITY;
        snprintf(p.mtext, MAXTEXT, "URGENT");
        msgsnd(qid, &p, strlen(p.mtext) + 1, 0);
        _exit(0);
    }

    /* ---- parent: consumer ---- */
    waitpid(pid, NULL, 0);                 /* let all 5 messages be queued */

    /* selective receive: pull the priority type first, even though it was
     * sent last -- a capability a plain pipe/stream cannot offer. */
    Msg m;
    ssize_t n = msgrcv(qid, &m, MAXTEXT, T_PRIORITY, 0);
    assert(n > 0);
    printf("priority first : [type %ld] %s\n", m.mtype, m.mtext);

    /* then drain the normal messages in FIFO order (type 1) */
    int count = 0;
    printf("normal in order:");
    while (msgrcv(qid, &m, MAXTEXT, T_NORMAL, IPC_NOWAIT) > 0) {
        printf(" %s", m.mtext);
        count++;
    }
    printf("\n");

    msgctl(qid, IPC_RMID, NULL);           /* remove the queue */

    assert(count == 4);
    printf("\ndemo_mqueue: PASS (1 priority + %d normal messages shared via SysV queue)\n",
           count);
    return 0;
}
