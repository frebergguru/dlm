/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm/compat — child process spawning with optional stdout capture. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "compat/dlm_proc.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include <windows.h>

struct dlm_proc {
    HANDLE proc;
    HANDLE rd; /* read end of the capture pipe, or NULL */
};

/* Build a single command line from argv using the MS C runtime quoting rules so
 * CreateProcess reconstructs the same argv the child's parser expects. */
static char *build_cmdline(const char *const argv[])
{
    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    for (int i = 0; argv[i]; i++) {
        const char *a = argv[i];
        int needq = (*a == '\0');
        for (const char *c = a; *c; c++)
            if (*c == ' ' || *c == '\t' || *c == '"') { needq = 1; break; }
        /* worst case: 2 quotes + every char doubled + space + NUL */
        size_t need = len + strlen(a) * 2 + 4;
        if (need > cap) { while (cap < need) cap *= 2; char *nb = realloc(out, cap); if (!nb) { free(out); return NULL; } out = nb; }
        if (i) out[len++] = ' ';
        if (needq) out[len++] = '"';
        for (const char *c = a; *c;) {
            unsigned slashes = 0;
            while (*c == '\\') { slashes++; c++; }
            if (*c == '"') {
                for (unsigned s = 0; s < slashes * 2 + 1; s++) out[len++] = '\\';
                out[len++] = '"';
                c++;
            } else if (*c == '\0') {
                for (unsigned s = 0; s < (needq ? slashes * 2 : slashes); s++) out[len++] = '\\';
            } else {
                for (unsigned s = 0; s < slashes; s++) out[len++] = '\\';
                out[len++] = *c++;
            }
        }
        if (needq) out[len++] = '"';
    }
    out[len] = '\0';
    return out;
}

dlm_proc *dlm_proc_spawn(const char *const argv[], int capture)
{
    HANDLE rd = NULL, wr = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    if (capture != DLM_SPAWN_INHERIT) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) return NULL;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (capture != DLM_SPAWN_INHERIT) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = wr;
        si.hStdError = (capture == DLM_SPAWN_STDOUT_ERR) ? wr : GetStdHandle(STD_ERROR_HANDLE);
    }
    char *cmd = build_cmdline(argv);
    if (!cmd) { if (rd) CloseHandle(rd); if (wr) CloseHandle(wr); return NULL; }
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(cmd);
    if (wr) CloseHandle(wr);
    if (!ok) { if (rd) CloseHandle(rd); return NULL; }
    CloseHandle(pi.hThread);
    dlm_proc *p = calloc(1, sizeof *p);
    if (!p) { CloseHandle(pi.hProcess); if (rd) CloseHandle(rd); return NULL; }
    p->proc = pi.hProcess;
    p->rd = rd;
    return p;
}

long dlm_proc_read(dlm_proc *p, void *buf, size_t n, int timeout_ms)
{
    if (!p || !p->rd) return -1;
    if (timeout_ms >= 0) {
        int waited = 0;
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(p->rd, NULL, 0, NULL, &avail, NULL)) return 0; /* broken => EOF */
            if (avail > 0) break;
            if (WaitForSingleObject(p->proc, 0) == WAIT_OBJECT_0) {
                if (!PeekNamedPipe(p->rd, NULL, 0, NULL, &avail, NULL) || avail == 0) return 0;
                break;
            }
            if (waited >= timeout_ms) return -1;
            Sleep(20);
            waited += 20;
        }
    }
    DWORD got = 0;
    if (!ReadFile(p->rd, buf, (DWORD)n, &got, NULL)) return 0;
    return (long)got;
}

void dlm_proc_terminate(dlm_proc *p)
{
    if (p && p->proc) TerminateProcess(p->proc, 1);
}

int dlm_proc_wait(dlm_proc *p, int *exit_code)
{
    if (!p || !p->proc) return -1;
    WaitForSingleObject(p->proc, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(p->proc, &code);
    if (exit_code) *exit_code = (int)code;
    return 0;
}

void dlm_proc_free(dlm_proc *p)
{
    if (!p) return;
    if (p->rd) CloseHandle(p->rd);
    if (p->proc) CloseHandle(p->proc);
    free(p);
}

int dlm_proc_spawn_detached(const char *const argv[])
{
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = nul;
    si.hStdError = nul;
    char *cmd = build_cmdline(argv);
    if (!cmd) { if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul); return -1; }
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                             DETACHED_PROCESS | CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) return -1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

#else /* POSIX */

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

struct dlm_proc {
    pid_t pid;
    int rfd; /* read end of capture pipe, or -1 */
};

dlm_proc *dlm_proc_spawn(const char *const argv[], int capture)
{
    int pfd[2] = { -1, -1 };
    if (capture != DLM_SPAWN_INHERIT && pipe(pfd) != 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); }
        return NULL;
    }
    if (pid == 0) {
        if (capture != DLM_SPAWN_INHERIT) {
            dup2(pfd[1], STDOUT_FILENO);
            if (capture == DLM_SPAWN_STDOUT_ERR) dup2(pfd[1], STDERR_FILENO);
            close(pfd[0]);
            close(pfd[1]);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (pfd[1] >= 0) close(pfd[1]);
    dlm_proc *p = calloc(1, sizeof *p);
    if (!p) { if (pfd[0] >= 0) close(pfd[0]); return NULL; }
    p->pid = pid;
    p->rfd = pfd[0];
    return p;
}

long dlm_proc_read(dlm_proc *p, void *buf, size_t n, int timeout_ms)
{
    if (!p || p->rfd < 0) return -1;
    if (timeout_ms >= 0) {
        struct pollfd pf = { p->rfd, POLLIN, 0 };
        int r = poll(&pf, 1, timeout_ms);
        if (r < 0) return (errno == EINTR) ? -1 : 0;
        if (r == 0) return -1; /* timeout: caller re-checks cancel */
    }
    ssize_t got = read(p->rfd, buf, n);
    if (got < 0) return (errno == EINTR) ? -1 : 0;
    return (long)got;
}

void dlm_proc_terminate(dlm_proc *p)
{
    if (p) kill(p->pid, SIGTERM);
}

int dlm_proc_wait(dlm_proc *p, int *exit_code)
{
    if (!p) return -1;
    int status = 0;
    if (waitpid(p->pid, &status, 0) < 0) return -1;
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

void dlm_proc_free(dlm_proc *p)
{
    if (!p) return;
    if (p->rfd >= 0) close(p->rfd);
    free(p);
}

int dlm_proc_spawn_detached(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    return 0;
}

#endif
