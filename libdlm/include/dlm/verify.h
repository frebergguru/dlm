/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — file checksum verification. */
#ifndef DLM_VERIFY_H
#define DLM_VERIFY_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DLM_VERIFY_OK = 0,
    DLM_VERIFY_MISMATCH = 1,
    DLM_VERIFY_ERROR = -1 /* could not read/hash the file */
};

/* Compare a file's md5/sha1/sha256 against an expected lowercase/uppercase hex
 * string. */
int dlm_verify_md5(const char *path, const char *expected_hex);
int dlm_verify_sha1(const char *path, const char *expected_hex);
int dlm_verify_sha256(const char *path, const char *expected_hex);

#ifdef __cplusplus
}
#endif

#endif /* DLM_VERIFY_H */
