/* libdlm — file checksum verification (md5/sha1 via OpenSSL EVP). */
#define _POSIX_C_SOURCE 200809L
#include "dlm/verify.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <strings.h>

static int hash_file(const char *path, const EVP_MD *md, char *hex, size_t hexlen)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { fclose(fp); return -1; }
    if (EVP_DigestInit_ex(ctx, md, NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        fclose(fp);
        return -1;
    }
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
        EVP_DigestUpdate(ctx, buf, n);
    int read_err = ferror(fp); /* short count above may be EOF or an I/O error */
    fclose(fp);
    if (read_err) { EVP_MD_CTX_free(ctx); return -1; }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &dlen) != 1) {
        EVP_MD_CTX_free(ctx);
        return -1;
    }
    EVP_MD_CTX_free(ctx);

    if (hexlen < (size_t)dlen * 2 + 1) return -1;
    static const char hx[] = "0123456789abcdef";
    for (unsigned int i = 0; i < dlen; i++) {
        hex[i * 2] = hx[digest[i] >> 4];
        hex[i * 2 + 1] = hx[digest[i] & 15];
    }
    hex[dlen * 2] = '\0';
    return 0;
}

int dlm_verify_md5(const char *path, const char *expected_hex)
{
    char got[33];
    if (hash_file(path, EVP_md5(), got, sizeof got) != 0) return DLM_VERIFY_ERROR;
    return strcasecmp(got, expected_hex) == 0 ? DLM_VERIFY_OK : DLM_VERIFY_MISMATCH;
}

int dlm_verify_sha1(const char *path, const char *expected_hex)
{
    char got[41];
    if (hash_file(path, EVP_sha1(), got, sizeof got) != 0) return DLM_VERIFY_ERROR;
    return strcasecmp(got, expected_hex) == 0 ? DLM_VERIFY_OK : DLM_VERIFY_MISMATCH;
}
