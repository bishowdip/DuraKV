/*
 * permissions.c -- owner/group/other rwx checks for namespaces.
 * See include/permissions.h.
 */
#include "permissions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char ns[64];
    char owner[64];
    char group[64];
    int  mode;          /* 3 octal digits: owner|group|other, each rwx */
} NsPerm;

struct PermTable {
    NsPerm *e;
    size_t  n, cap;
};

PermTable *perm_create(void) { return calloc(1, sizeof(PermTable)); }

void perm_destroy(PermTable *p)
{
    if (!p) return;
    free(p->e);
    free(p);
}

static NsPerm *find_ns(PermTable *p, const char *ns)
{
    for (size_t i = 0; i < p->n; i++)
        if (strcmp(p->e[i].ns, ns) == 0) return &p->e[i];
    return NULL;
}

int perm_set(PermTable *p, const char *ns, const char *owner,
             const char *group, int mode)
{
    NsPerm *e = find_ns(p, ns);
    if (!e) {
        if (p->n == p->cap) {
            p->cap = p->cap ? p->cap * 2 : 8;
            p->e = realloc(p->e, p->cap * sizeof(NsPerm));
        }
        e = &p->e[p->n++];
        snprintf(e->ns, sizeof(e->ns), "%s", ns);
    }
    snprintf(e->owner, sizeof(e->owner), "%s", owner);
    snprintf(e->group, sizeof(e->group), "%s", group);
    e->mode = mode;
    return 0;
}

int perm_check(PermTable *p, const char *ns, const char *user,
               const char *user_group, char op)
{
    NsPerm *e = find_ns(p, ns);
    if (!e) return 0;                                 /* no namespace -> deny */

    /* select the relevant permission triad: owner, then group, then other */
    int triad;
    if (strcmp(user, e->owner) == 0)                  triad = (e->mode >> 6) & 7;
    else if (user_group && strcmp(user_group, e->group) == 0) triad = (e->mode >> 3) & 7;
    else                                              triad =  e->mode       & 7;

    int need = (op == 'r') ? 4 : (op == 'w') ? 2 : 1; /* r=4 w=2 x=1 */
    return (triad & need) ? 1 : 0;
}
