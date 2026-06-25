/*
 * demo_auth.c -- authentication + access control (Task 3).
 *
 * Part 1: register users with Argon2id-hashed passwords; the right password
 *         logs in, a wrong one (and an unknown user) is rejected.
 * Part 2: a namespace "users" owned by alice (group staff) with mode 0640
 *         (owner rw, group r, other ---). The rwx checks then allow/deny each
 *         caller exactly as the bits dictate -- the POSIX model applied to keys.
 */
#include "auth.h"
#include "permissions.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* ---- authentication ---- */
    AuthStore *as = auth_create();
    assert(as);
    assert(auth_register(as, "alice", "alice-secret", "staff") == 0);
    assert(auth_register(as, "bob",   "bob-secret",   "staff") == 0);
    assert(auth_register(as, "eve",   "eve-secret",   "guest") == 0);
    assert(auth_register(as, "alice", "dup", "staff") == -1);   /* no duplicates */

    printf("== authentication (Argon2id) ==\n");
    int s_ok  = auth_login(as, "alice", "alice-secret");
    int s_bad = auth_login(as, "alice", "wrong-password");
    int s_unk = auth_login(as, "mallory", "whatever");
    printf("alice + correct password : %s (session %d)\n", s_ok  ? "LOGIN" : "DENIED", s_ok);
    printf("alice + wrong   password : %s\n",              s_bad ? "LOGIN?!" : "DENIED");
    printf("unknown user             : %s\n",              s_unk ? "LOGIN?!" : "DENIED");
    assert(s_ok > 0 && s_bad == 0 && s_unk == 0);

    /* ---- permissions: namespace "users" = alice:staff 0640 ---- */
    PermTable *pt = perm_create();
    perm_set(pt, "users", "alice", "staff", 0640);   /* owner rw, group r, other - */

    printf("\n== permissions on namespace \"users\" (owner=alice group=staff mode=0640) ==\n");
    struct { const char *u, *g; char op; } t[] = {
        {"alice", "staff", 'w'}, {"alice", "staff", 'r'},
        {"bob",   "staff", 'r'}, {"bob",   "staff", 'w'},
        {"eve",   "guest", 'r'}, {"eve",   "guest", 'w'},
    };
    for (size_t i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        int ok = perm_check(pt, "users", t[i].u, t[i].g, t[i].op);
        printf("  %-5s (%-5s) wants %c -> %s\n", t[i].u, t[i].g, t[i].op,
               ok ? "ALLOW" : "deny");
    }

    /* owner rw; group read-only; others nothing */
    assert(perm_check(pt, "users", "alice", "staff", 'w') == 1);
    assert(perm_check(pt, "users", "alice", "staff", 'r') == 1);
    assert(perm_check(pt, "users", "bob",   "staff", 'r') == 1);
    assert(perm_check(pt, "users", "bob",   "staff", 'w') == 0);
    assert(perm_check(pt, "users", "eve",   "guest", 'r') == 0);
    assert(perm_check(pt, "users", "eve",   "guest", 'w') == 0);

    perm_destroy(pt);
    auth_destroy(as);
    printf("\ndemo_auth: PASS\n");
    return 0;
}
