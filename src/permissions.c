/*
 * permissions.c -- an access-control model on key namespaces, modelled directly
 * on the classic Unix owner/group/other rwx permission bits (Task 3).
 *
 * Every key belongs to a namespace (the prefix before ':'), and each namespace
 * has an owner, a group, and a 3-digit octal mode. A request is checked against
 * exactly ONE triad, chosen by identity: owner bits if the user owns the
 * namespace, else group bits if the user is in its group, else "other" bits.
 * This is the same first-match precedence the Unix kernel uses for files -- an
 * owner who lacks a bit is denied even if "other" would allow it.
 *
 * Mode encoding (per triad): r=4, w=2, x=1, so e.g. 0640 = owner rw-, group
 * r--, other ---. See include/permissions.h.
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

/* Decide whether `user` (in `user_group`) may perform op ('r'/'w'/'x') on `ns`.
 * Returns 1 to allow, 0 to deny. Fails closed: an unknown namespace is denied
 * rather than allowed. */
int perm_check(PermTable *p, const char *ns, const char *user,
               const char *user_group, char op)
{
    NsPerm *e = find_ns(p, ns);
    if (!e) return 0;                                 /* no namespace -> deny */

    /* Select the ONE applicable triad by first-match precedence, mirroring the
     * Unix kernel: owner (bits 6-8), else group (bits 3-5), else other (0-2). */
    int triad;
    if (strcmp(user, e->owner) == 0)                  triad = (e->mode >> 6) & 7;
    else if (user_group && strcmp(user_group, e->group) == 0) triad = (e->mode >> 3) & 7;
    else                                              triad =  e->mode       & 7;

    /* Map the requested op to its bit and test it in the chosen triad. */
    int need = (op == 'r') ? 4 : (op == 'w') ? 2 : 1; /* r=4 w=2 x=1 */
    return (triad & need) ? 1 : 0;
}
