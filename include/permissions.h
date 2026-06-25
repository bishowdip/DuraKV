/*
 * permissions.h -- POSIX-style rwx access control for key namespaces (Phase 5).
 *
 * OS/systems primitive: access control (owner / group / other, rwx).
 *
 * Keys live in namespaces (the prefix before ':', e.g. "users:alice"). Each
 * namespace has an owner, a group, and a 3-octal-digit mode exactly like a
 * Unix file: owner bits, group bits, other bits, each r=4 w=2 x=1. Every
 * read/write is checked against the caller's identity.
 */
#ifndef DURAKV_PERMISSIONS_H
#define DURAKV_PERMISSIONS_H

typedef struct PermTable PermTable;

PermTable *perm_create(void);
void       perm_destroy(PermTable *p);

/* Define (or update) a namespace's owner, group and mode (e.g. 0640). */
int perm_set(PermTable *p, const char *ns, const char *owner,
             const char *group, int mode);

/* May `user` (in `user_group`) perform op on `ns`? op is 'r', 'w', or 'x'.
 * Returns 1 if allowed, 0 if denied (unknown namespace => denied). */
int perm_check(PermTable *p, const char *ns, const char *user,
               const char *user_group, char op);

#endif /* DURAKV_PERMISSIONS_H */
