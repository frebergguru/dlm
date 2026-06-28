/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm/compat — positioned/atomic file I/O shims. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "compat/dlm_io.h"

#include <string.h>

#if defined(_WIN32)

#include <windows.h>
#include <io.h>

long long dlm_pwrite(int fd, const void *buf, size_t n, int64_t off)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)(off & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)((off >> 32) & 0xFFFFFFFF);
    DWORD wrote = 0;
    if (!WriteFile(h, buf, (DWORD)n, &wrote, &ov)) return -1;
    return (long long)wrote;
}

int dlm_ftruncate(int fd, int64_t len)
{
    return _chsize_s(fd, len) == 0 ? 0 : -1;
}

int dlm_fsync(int fd)
{
    return _commit(fd) == 0 ? 0 : -1;
}

int dlm_rename_replace(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 0;
    return -1;
}

#else /* POSIX */

#include <unistd.h>
#include <stdio.h>

long long dlm_pwrite(int fd, const void *buf, size_t n, int64_t off)
{
    return (long long)pwrite(fd, buf, n, (off_t)off);
}

int dlm_ftruncate(int fd, int64_t len)
{
    return ftruncate(fd, (off_t)len);
}

int dlm_fsync(int fd)
{
    return fsync(fd);
}

int dlm_rename_replace(const char *from, const char *to)
{
    return rename(from, to);
}

#endif
