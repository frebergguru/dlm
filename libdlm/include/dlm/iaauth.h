/* libdlm — Internet Archive authentication.
 *
 * Downloads work anonymously by default. The user may optionally sign in to
 * reach account-restricted items and higher rate limits, via either:
 *   - S3-like keys (access + secret from archive.org/account/s3.php), sent as
 *     "Authorization: LOW <access>:<secret>" — recommended, no password stored;
 *   - a session cookie (logged-in-user/logged-in-sig), pasted from a browser or
 *     obtained by password login.
 * Credentials live in $XDG_CONFIG_HOME/dlm/credentials (mode 0600), never in the
 * queue database.
 */
#ifndef DLM_IAAUTH_H
#define DLM_IAAUTH_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IA_AUTH_NONE = 0, /* anonymous */
    IA_AUTH_S3,
    IA_AUTH_COOKIE
} ia_auth_mode;

typedef struct {
    ia_auth_mode mode;
    char *access; /* S3 access key */
    char *secret; /* S3 secret key */
    char *cookie; /* full Cookie header value */
} ia_credentials;

/* Load saved credentials (mode IA_AUTH_NONE if none configured). Returns 0. */
int dlm_ia_load(ia_credentials *out);
void dlm_ia_credentials_free(ia_credentials *c);

/* Persist credentials (atomically, mode 0600). Return 0 on success. */
int dlm_ia_save_s3(const char *access, const char *secret);
int dlm_ia_save_cookie(const char *cookie);
int dlm_ia_logout(void); /* clear all stored credentials */

/* Human-readable description of the current auth mode (e.g. "signed in (S3)"). */
const char *dlm_ia_mode_str(ia_auth_mode m);

/* Build a NULL-terminated "Key: Value" header array for the given credentials,
 * or NULL when anonymous. Free with dlm_ia_free_headers(). */
char **dlm_ia_auth_headers(const ia_credentials *c);
void dlm_ia_free_headers(char **h);

/* Log in with email + password via the IA xauthn endpoint; on success stores
 * the session cookie and returns 0. On failure returns non-zero and, if err is
 * non-NULL, sets *err to a malloc'd message. */
int dlm_ia_login_password(const char *email, const char *password, char **err);

#ifdef __cplusplus
}
#endif

#endif /* DLM_IAAUTH_H */
