/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — platform compatibility layer.
 *
 * A thin shim that lets the POSIX-shaped engine, daemon and CLI build and run on
 * both Unix and Windows (MinGW-w64). Each source file includes this umbrella
 * header and uses the dlm_* wrappers instead of raw POSIX calls; on Unix the
 * wrappers are near-zero-cost pass-throughs so behaviour is unchanged.
 */
#ifndef DLM_COMPAT_H
#define DLM_COMPAT_H

#if defined(_WIN32)
#  define DLM_WIN 1
#else
#  define DLM_POSIX 1
#endif

/* Case-insensitive compare lives in <strings.h> on POSIX and as _stricmp on
 * Windows. Provide the POSIX names everywhere so call sites stay unchanged. */
#if defined(_WIN32)
#  include <string.h>
#  ifndef strcasecmp
#    define strcasecmp _stricmp
#  endif
#  ifndef strncasecmp
#    define strncasecmp _strnicmp
#  endif
#endif

#include "compat/dlm_paths.h"
#include "compat/dlm_misc.h"
#include "compat/dlm_io.h"
#include "compat/dlm_sock.h"
#include "compat/dlm_proc.h"

#endif /* DLM_COMPAT_H */
