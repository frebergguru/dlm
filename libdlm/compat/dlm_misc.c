/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm/compat — small platform shims. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "compat/dlm_misc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void (*g_term_cb)(void);

#if defined(_WIN32)

#include <windows.h>

void dlm_sleep_ms(unsigned ms) { Sleep(ms); }

int dlm_self_dir(char *buf, size_t n)
{
    DWORD r = GetModuleFileNameA(NULL, buf, (DWORD)n);
    if (r == 0 || r >= n) return -1;
    char *slash = strrchr(buf, '\\');
    char *fslash = strrchr(buf, '/');
    if (fslash && (!slash || fslash > slash)) slash = fslash;
    if (slash) *slash = '\0';
    return 0;
}

char *dlm_prompt_hidden(const char *prompt)
{
    if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    int restored = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode & ~(DWORD)ENABLE_ECHO_INPUT);
        restored = 1;
    }
    char line[1024];
    char *r = fgets(line, sizeof line, stdin);
    if (restored) SetConsoleMode(h, mode);
    fputc('\n', stderr);
    if (!r) return NULL;
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    char *out = malloc(len + 1);
    if (out) memcpy(out, line, len + 1);
    return out;
}

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    if (g_term_cb) g_term_cb();
    return TRUE;
}

void dlm_install_term_handler(void (*fn)(void))
{
    g_term_cb = fn;
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
}

#else /* POSIX */

#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <termios.h>

void dlm_sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int dlm_self_dir(char *buf, size_t n)
{
    ssize_t r = readlink("/proc/self/exe", buf, n - 1);
    if (r <= 0) return -1;
    buf[r] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash) *slash = '\0';
    return 0;
}

char *dlm_prompt_hidden(const char *prompt)
{
    if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    struct termios old, no;
    int have_tty = (tcgetattr(STDIN_FILENO, &old) == 0);
    if (have_tty) {
        no = old;
        no.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &no);
    }
    char line[1024];
    char *r = fgets(line, sizeof line, stdin);
    if (have_tty) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    fputc('\n', stderr);
    if (!r) return NULL;
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    char *out = malloc(len + 1);
    if (out) memcpy(out, line, len + 1);
    return out;
}

static void term_trampoline(int s) { (void)s; if (g_term_cb) g_term_cb(); }

void dlm_install_term_handler(void (*fn)(void))
{
    g_term_cb = fn;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = term_trampoline;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

#endif
