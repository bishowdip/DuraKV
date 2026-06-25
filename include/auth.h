/*
 * auth.h -- user store, password login, sessions (DuraKV Phase 5).
 *
 * OS/systems primitive: authentication.
 *
 * Each user record keeps an Argon2id password hash (never the password). Login
 * verifies with libsodium and, on success, issues a session id. A connection
 * holding a valid session is "authenticated"; commands before login are
 * rejected by the server.
 */
#ifndef DURAKV_AUTH_H
#define DURAKV_AUTH_H

typedef struct AuthStore AuthStore;

AuthStore *auth_create(void);
void       auth_destroy(AuthStore *s);

/* Register a user. Returns 0 on success, -1 if the user already exists. */
int auth_register(AuthStore *s, const char *user, const char *password,
                  const char *group);

/* Verify credentials. Returns a session id (> 0) on success, or 0 on failure. */
int auth_login(AuthStore *s, const char *user, const char *password);

/* The group a user belongs to, or NULL if unknown. */
const char *auth_group_of(AuthStore *s, const char *user);

#endif /* DURAKV_AUTH_H */
