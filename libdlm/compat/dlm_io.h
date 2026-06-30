/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm/compat — positioned/atomic file I/O shims. */
#ifndef DLM_COMPAT_IO_H
#define DLM_COMPAT_IO_H

#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>

/* OR this into open() flags so binaries are never mangled by text-mode
 * translation on Windows (no-op on POSIX). */
#if defined(_WIN32)
#  ifndef O_BINARY
#    define O_BINARY 0x8000
#  endif
#  define DLM_O_BINARY O_BINARY
#else
#  define DLM_O_BINARY 0
#endif

/* Close-on-exec and no-follow-symlink open() flags, or 0 where unavailable, so
 * the same open() call is safe to write across platforms. */
#ifdef O_CLOEXEC
#  define DLM_O_CLOEXEC O_CLOEXEC
#else
#  define DLM_O_CLOEXEC 0
#endif
#ifdef O_NOFOLLOW
#  define DLM_O_NOFOLLOW O_NOFOLLOW
#else
#  define DLM_O_NOFOLLOW 0
#endif

/* pwrite(2): write n bytes at absolute offset without moving the shared file
 * pointer (the engine uses one fd per download from a single worker thread, so
 * the Windows seek+write emulation is race-free). Returns bytes written or -1. */
long long dlm_pwrite(int fd, const void *buf, size_t n, int64_t off);

/* Truncate/extend a file to len bytes. Returns 0 on success, -1 otherwise. */
int dlm_ftruncate(int fd, int64_t len);

/* Flush a file's data to disk. Returns 0 on success, -1 otherwise. */
int dlm_fsync(int fd);

/* Atomically rename `from` over `to`, replacing an existing `to`. POSIX rename()
 * already does this; Windows needs MoveFileEx(REPLACE_EXISTING). Returns 0 on
 * success, -1 otherwise. */
int dlm_rename_replace(const char *from, const char *to);

#endif /* DLM_COMPAT_IO_H */
