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
    HANDLE rd;  /* read end of the capture pipe, or NULL */
    HANDLE job; /* Job object holding the child and its descendants, or NULL */
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

/* Add h to the inherit list if it is a real, inheritable handle not already
 * listed (PROC_THREAD_ATTRIBUTE_HANDLE_LIST rejects non-inheritable handles). */
static void add_inherit(HANDLE *list, int *n, HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD fl = 0;
    if (!GetHandleInformation(h, &fl) || !(fl & HANDLE_FLAG_INHERIT)) return;
    for (int i = 0; i < *n; i++)
        if (list[i] == h) return;
    list[(*n)++] = h;
}

/* CreateProcessA that lets the child inherit ONLY the given handles (its std
 * handles), not every inheritable handle in this process (sockets, other
 * children's pipes, ...). Falls back to plain inheritance if the attribute
 * list cannot be set up or the restricted create fails. */
static BOOL create_process_restricted(char *cmd, DWORD flags, STARTUPINFOA *si,
                                      HANDLE *handles, int nh, PROCESS_INFORMATION *pi)
{
    if (nh == 0) /* nothing to inherit at all */
        return CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flags, NULL, NULL, si, pi);

    SIZE_T sz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = sz ? (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(sz) : NULL;
    if (al && InitializeProcThreadAttributeList(al, 1, 0, &sz)) {
        BOOL ok = FALSE;
        if (UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                      (SIZE_T)nh * sizeof(HANDLE), NULL, NULL)) {
            STARTUPINFOEXA six;
            memset(&six, 0, sizeof six);
            six.StartupInfo = *si;
            six.StartupInfo.cb = sizeof six;
            six.lpAttributeList = al;
            ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, flags | EXTENDED_STARTUPINFO_PRESENT,
                                NULL, NULL, &six.StartupInfo, pi);
        }
        DeleteProcThreadAttributeList(al);
        free(al);
        if (ok) return TRUE;
    } else {
        free(al);
    }
    /* fallback: previous behaviour (inherit all inheritable handles) */
    return CreateProcessA(NULL, cmd, NULL, NULL, TRUE, flags, NULL, NULL, si, pi);
}

/* A Job object that kills every process in it when terminated or when its last
 * handle closes, so a launcher's real child (PyInstaller onefile yt-dlp.exe,
 * yt-dlp's ffmpeg) dies with it. NULL if unavailable. */
static HANDLE make_kill_job(void)
{
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (!job) return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof li);
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

dlm_proc *dlm_proc_spawn(const char *const argv[], int capture)
{
    HANDLE rd = NULL, wr = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    if (capture != DLM_SPAWN_INHERIT) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) return NULL;
        /* only the child's write end may be inherited, never our read end */
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    HANDLE inh[3];
    int ninh = 0;
    if (capture != DLM_SPAWN_INHERIT) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = wr;
        si.hStdError = (capture == DLM_SPAWN_STDOUT_ERR) ? wr : GetStdHandle(STD_ERROR_HANDLE);
        add_inherit(inh, &ninh, si.hStdInput);
        add_inherit(inh, &ninh, si.hStdOutput);
        add_inherit(inh, &ninh, si.hStdError);
    } else {
        add_inherit(inh, &ninh, GetStdHandle(STD_INPUT_HANDLE));
        add_inherit(inh, &ninh, GetStdHandle(STD_OUTPUT_HANDLE));
        add_inherit(inh, &ninh, GetStdHandle(STD_ERROR_HANDLE));
    }
    char *cmd = build_cmdline(argv);
    if (!cmd) { if (rd) CloseHandle(rd); if (wr) CloseHandle(wr); return NULL; }
    HANDLE job = make_kill_job();
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    /* start suspended so it is inside the job before it can spawn anything */
    BOOL ok = create_process_restricted(cmd, job ? CREATE_SUSPENDED : 0, &si, inh, ninh, &pi);
    free(cmd);
    if (wr) CloseHandle(wr);
    if (!ok) { if (rd) CloseHandle(rd); if (job) CloseHandle(job); return NULL; }
    if (job) {
        if (!AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = NULL; }
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    dlm_proc *p = calloc(1, sizeof *p);
    if (!p) {
        if (job) CloseHandle(job); /* kills the child (KILL_ON_JOB_CLOSE) */
        CloseHandle(pi.hProcess);
        if (rd) CloseHandle(rd);
        return NULL;
    }
    p->proc = pi.hProcess;
    p->rd = rd;
    p->job = job;
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
    if (!p || !p->proc) return;
    /* the whole job: the child and every descendant still in it */
    if (p->job && TerminateJobObject(p->job, 1)) return;
    TerminateProcess(p->proc, 1);
}

void dlm_proc_kill(dlm_proc *p)
{
    dlm_proc_terminate(p); /* already forceful on Windows */
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
    if (p->job) CloseHandle(p->job); /* KILL_ON_JOB_CLOSE reaps any leftovers */
    free(p);
}

int dlm_proc_spawn_detached(const char *const argv[])
{
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    /* inheritable so the child can actually receive it as its std handles */
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = nul;
    si.hStdError = nul;
    char *cmd = build_cmdline(argv);
    if (!cmd) { if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul); return -1; }
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    HANDLE inh[1];
    int ninh = 0;
    add_inherit(inh, &ninh, nul);
    BOOL ok = create_process_restricted(cmd, DETACHED_PROCESS | CREATE_NO_WINDOW, &si,
                                        inh, ninh, &pi);
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
#include <time.h>
#include <sys/wait.h>

/* Grace period between SIGTERM and SIGKILL for a terminated child's group. */
#define DLM_PROC_KILL_GRACE_MS 3000

struct dlm_proc {
    pid_t pid;
    pid_t pgid;      /* own process group (capture mode), or 0 */
    int rfd;         /* read end of capture pipe, or -1 */
    long long term_ms; /* monotonic ms when terminate was requested, or 0 */
    int killed;      /* SIGKILL already sent */
};

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Signal the child's whole process group if it has one, else just the child. */
static void signal_proc(dlm_proc *p, int sig)
{
    if (p->pgid > 0 && kill(-p->pgid, sig) == 0) return;
    kill(p->pid, sig);
}

/* If terminate was requested and the grace period ran out, SIGKILL the group.
 * Returns the ms left in the grace period (0 once escalated / not pending). */
static long long escalate_if_due(dlm_proc *p)
{
    if (!p->term_ms || p->killed) return 0;
    long long left = p->term_ms + DLM_PROC_KILL_GRACE_MS - now_ms();
    if (left > 0) return left;
    signal_proc(p, SIGKILL);
    p->killed = 1;
    return 0;
}

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
            /* Own process group, so terminate reaches yt-dlp's ffmpeg children
             * too. (INHERIT stays in our group: the CLI runs it on a terminal
             * and Ctrl+C must reach it through the foreground group.) A
             * background group must not touch the terminal, so stdin comes
             * from /dev/null (otherwise a read/tcsetattr would stop it with
             * SIGTTIN/SIGTTOU). */
            setpgid(0, 0);
            int nfd = open("/dev/null", O_RDONLY);
            if (nfd >= 0) { dup2(nfd, STDIN_FILENO); if (nfd != STDIN_FILENO) close(nfd); }
            dup2(pfd[1], STDOUT_FILENO);
            if (capture == DLM_SPAWN_STDOUT_ERR) dup2(pfd[1], STDERR_FILENO);
            close(pfd[0]);
            close(pfd[1]);
        }
        /* Close every inherited descriptor above stdio so the child (yt-dlp,
         * ffmpeg, tar — third-party code) never receives the daemon's control
         * socket, live client connections, sqlite db, or other downloads'
         * .part fds. Cheap and robust regardless of per-fd CLOEXEC. */
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
        for (int fd = 3; fd < (int)maxfd; fd++) close(fd);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    pid_t pgid = 0;
    if (capture != DLM_SPAWN_INHERIT) {
        /* also from the parent, so the group exists before we return no matter
         * which side runs first (EACCES after exec is fine: child did it) */
        setpgid(pid, pid);
        pgid = pid;
    }
    if (pfd[1] >= 0) close(pfd[1]);
    dlm_proc *p = calloc(1, sizeof *p);
    if (!p) { if (pfd[0] >= 0) close(pfd[0]); return NULL; }
    p->pid = pid;
    p->pgid = pgid;
    p->rfd = pfd[0];
    return p;
}

long dlm_proc_read(dlm_proc *p, void *buf, size_t n, int timeout_ms)
{
    if (!p || p->rfd < 0) return -1;
    /* After terminate, never block past the grace period: a descendant that
     * ignored SIGTERM would otherwise hold the pipe open forever. */
    long long left = escalate_if_due(p);
    if (timeout_ms >= 0 || left > 0) {
        int wait_ms = timeout_ms;
        if (left > 0 && (wait_ms < 0 || left < wait_ms)) wait_ms = (int)left;
        struct pollfd pf = { p->rfd, POLLIN, 0 };
        int r = poll(&pf, 1, wait_ms);
        if (r < 0) return (errno == EINTR) ? -1 : 0;
        if (r == 0) {
            escalate_if_due(p);
            if (timeout_ms >= 0) return -1; /* timeout: caller re-checks cancel */
            /* blocking caller: group is now SIGKILLed, so EOF is imminent */
        }
    }
    ssize_t got = read(p->rfd, buf, n);
    if (got < 0) return (errno == EINTR) ? -1 : 0;
    return (long)got;
}

void dlm_proc_terminate(dlm_proc *p)
{
    if (!p) return;
    if (!p->term_ms) p->term_ms = now_ms();
    if (!p->term_ms) p->term_ms = 1;
    signal_proc(p, SIGTERM);
}

void dlm_proc_kill(dlm_proc *p)
{
    if (!p) return;
    if (!p->term_ms) p->term_ms = 1;
    signal_proc(p, SIGKILL);
    p->killed = 1;
}

int dlm_proc_wait(dlm_proc *p, int *exit_code)
{
    if (!p) return -1;
    int status = 0;
    pid_t r;
    if (p->term_ms && !p->killed) {
        /* terminated: give it the grace period, then SIGKILL the group */
        for (;;) {
            r = waitpid(p->pid, &status, WNOHANG);
            if (r == p->pid) break;
            if (r < 0 && errno != EINTR) return -1;
            if (escalate_if_due(p) == 0) { r = 0; break; }
            struct timespec ts = { 0, 50 * 1000000L };
            nanosleep(&ts, NULL);
        }
        if (r == 0) {
            do r = waitpid(p->pid, &status, 0); while (r < 0 && errno == EINTR);
            if (r < 0) return -1;
        }
    } else {
        do r = waitpid(p->pid, &status, 0); while (r < 0 && errno == EINTR);
        if (r < 0) return -1;
    }
    /* A terminated child is gone; make sure no member of its group outlives it
     * (e.g. an ffmpeg that ignored SIGTERM). */
    if (p->term_ms && p->pgid > 0 && !p->killed) {
        while (kill(-p->pgid, 0) == 0 && escalate_if_due(p) > 0) {
            struct timespec ts = { 0, 50 * 1000000L };
            nanosleep(&ts, NULL);
        }
    }
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
