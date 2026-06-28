/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm/compat — per-user directories and directory creation. */
#ifndef DLM_COMPAT_PATHS_H
#define DLM_COMPAT_PATHS_H

#include <stddef.h>

/* Fill buf with a per-user dlm directory (no trailing slash). The directory is
 * not created — call dlm_mkdir_p() on the result when you need it to exist.
 *   config:  $XDG_CONFIG_HOME/dlm  | %APPDATA%\dlm
 *   data:    $XDG_DATA_HOME/dlm    | %LOCALAPPDATA%\dlm
 *   cache:   $XDG_CACHE_HOME/dlm   | %LOCALAPPDATA%\dlm\cache
 *   runtime: $XDG_RUNTIME_DIR      | %LOCALAPPDATA%\dlm
 */
void dlm_config_dir(char *buf, size_t n);
void dlm_data_dir(char *buf, size_t n);
void dlm_cache_dir(char *buf, size_t n);
void dlm_runtime_dir(char *buf, size_t n);

/* The AF_UNIX socket the daemon binds and clients connect to. */
void dlm_socket_path(char *buf, size_t n);

/* The queue database path ($DLM_DB override, else <data>/queue.db). The parent
 * directory is created as a side effect. */
void dlm_db_path(char *buf, size_t n);

/* Recursively create a directory path (POSIX mode 0755). Returns 0 on success
 * or if it already exists, -1 on error. */
int dlm_mkdir_p(const char *path);

/* Create a single directory with the given POSIX mode (mode ignored on
 * Windows). Returns 0 on success or if it already exists, -1 otherwise. */
int dlm_mkdir_mode(const char *path, int mode);

#endif /* DLM_COMPAT_PATHS_H */
