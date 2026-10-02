/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_unix.c - POSIX implementation of os_proc.h.
 */

#include "../os_proc.h"
#include "../../base/xmalloc.h"
#include <fcntl.h>
#include <stdatomic.h>

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(XR_OS_MACOS)
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#endif

#define PROC_GROUP_CAPACITY 64
static atomic_int proc_groups[PROC_GROUP_CAPACITY];
static int proc_group_slot(XrProcId pid) {
    for (int i = 0; i < PROC_GROUP_CAPACITY; ++i)
        if (atomic_load_explicit(&proc_groups[i], memory_order_acquire) == (pid_t)pid) return i;
    return -1;
}
static int proc_group_reserve(void) {
    for (int i = 0; i < PROC_GROUP_CAPACITY; ++i) {
        int empty = 0;
        if (atomic_compare_exchange_strong(&proc_groups[i], &empty, -1)) return i;
    }
    return -1;
}

static XrOsProcStatus proc_system_alloc(void *context, size_t bytes, void **out) {
    (void) context; void *p = xr_malloc(bytes);
    if (!p) return XR_PROC_OUT_OF_MEMORY;
    *out = p; return XR_PROC_OK;
}
static void proc_system_free(void *context, void *p) { (void) context; xr_free(p); }
static XrOsProcStatus proc_system_work(void *context, uint64_t n) {
    (void) context; (void) n; return XR_PROC_OK;
}
XR_FUNC XrProcMemory xr_proc_system_memory(void) {
    XrProcMemory m = {NULL, proc_system_alloc, proc_system_free, proc_system_work}; return m;
}
XR_FUNC XrOsProcStatus xr_proc_last_error(void) {
    if (errno == ENOMEM) return XR_PROC_OUT_OF_MEMORY;
    if (errno == ENOENT || errno == ENOTDIR) return XR_PROC_UNRESOLVED;
    if (errno == EINVAL || errno == EILSEQ) return XR_PROC_INVALID_ARGUMENT;
    return XR_PROC_IO;
}

static XrOsProcStatus proc_spawn_options_valid(const XrProcSpawnOptions *o) {
    if (o->env_count && (!o->env_keys || !o->env_values)) return XR_PROC_INVALID_ARGUMENT;
    for (size_t i = 0; i < o->env_count; ++i) {
        const char *key = o->env_keys[i];
        if (!key || !*key || !o->env_values[i]) return XR_PROC_INVALID_ARGUMENT;
        for (size_t j = 0;; ++j) {
            XrOsProcStatus s = o->memory.work(o->memory.context, 1); if (s != XR_PROC_OK) return s;
            if (!key[j]) break;
            if (key[j] == '=') return XR_PROC_INVALID_ARGUMENT;
        }
    }
    return XR_PROC_OK;
}

static int proc_dup_stdio(const XrProcSpawnOptions *options) {
    if (!options)
        return 0;
    if (options->has_stdin && dup2((int) options->stdin_read, STDIN_FILENO) < 0)
        return -1;
    if (options->has_stdout && dup2((int) options->stdout_write, STDOUT_FILENO) < 0)
        return -1;
    if (options->has_stderr && dup2((int) options->stderr_write, STDERR_FILENO) < 0)
        return -1;
    return 0;
}

static bool proc_write_i64(int fd, int64_t value) {
    const char *p = (const char *) &value;
    size_t left = sizeof(value);
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        p += n;
        left -= (size_t) n;
    }
    return true;
}

static bool proc_read_i64(int fd, int64_t *out) {
    if (!out)
        return false;
    char *p = (char *) out;
    size_t left = sizeof(*out);
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        p += n;
        left -= (size_t) n;
    }
    return true;
}

XR_FUNC XrOsProcStatus xr_proc_spawn(const char *prog, const char *const argv[],
    const XrProcSpawnOptions *o, XrProcId *output) {
    if (!prog || !*prog || !argv || !argv[0] || !o || !output || *output != XR_PROC_INVALID ||
        !o->memory.alloc || !o->memory.free || !o->memory.work ||
        (o->detached && o->new_process_group)) return XR_PROC_INVALID_ARGUMENT;
    XrProcMemory m = o->memory;
    XrOsProcStatus status = proc_spawn_options_valid(o);
    if (status != XR_PROC_OK) return status;
    char **environment = NULL; size_t count = 0;
    int report[2] = {-1, -1}, detached_pipe[2] = {-1, -1}; pid_t pid = -1; int group_slot = -1;
    if (o->new_process_group) {
        group_slot = proc_group_reserve();
        if (group_slot < 0) return XR_PROC_BUDGET;
    }
    if (o->complete_environment) {
        if (o->env_count >= SIZE_MAX / sizeof(char *)) { status = XR_PROC_BUDGET; goto done; }
        status = m.alloc(m.context, (o->env_count + 1) * sizeof(char *), (void **)&environment);
        if (status != XR_PROC_OK) goto done;
        for (; count < o->env_count; ++count) {
            size_t k = 0, v = 0;
            for (;;) { status = m.work(m.context, 1); if (status != XR_PROC_OK) goto done; if (!o->env_keys[count][k]) break; ++k; }
            for (;;) { status = m.work(m.context, 1); if (status != XR_PROC_OK) goto done; if (!o->env_values[count][v]) break; ++v; }
            if (k > SIZE_MAX - v - 2) { status = XR_PROC_BUDGET; goto done; }
            char *pair = NULL; status = m.alloc(m.context, k + v + 2, (void **)&pair);
            if (status != XR_PROC_OK) goto done;
            status = m.work(m.context, k + v + 2);
            if (status != XR_PROC_OK) { m.free(m.context, pair); goto done; }
            memcpy(pair, o->env_keys[count], k); pair[k] = '=';
            memcpy(pair + k + 1, o->env_values[count], v + 1); environment[count] = pair;
        }
        environment[count] = NULL;
    }
    status = m.work(m.context, 1); if (status != XR_PROC_OK) goto done;
    if (pipe(report) != 0) { status = xr_proc_last_error(); goto done; }
    status = m.work(m.context, 1); if (status != XR_PROC_OK) goto done;
    if (fcntl(report[1], F_SETFD, FD_CLOEXEC) < 0) { status = xr_proc_last_error(); goto done; }
    if (o->detached) {
        status = m.work(m.context, 1); if (status != XR_PROC_OK) goto done;
        if (pipe(detached_pipe) != 0) { status = xr_proc_last_error(); goto done; }
    }
    status = m.work(m.context, 1); if (status != XR_PROC_OK) goto done;
    pid = fork(); if (pid < 0) { status = xr_proc_last_error(); goto done; }
    if (!pid) {
        close(report[0]);
        if (o->detached) {
            close(detached_pipe[0]);
            if (setsid() < 0) goto child_failed;
            pid_t grandchild = fork(); if (grandchild < 0) goto child_failed;
            if (grandchild > 0) { (void)proc_write_i64(detached_pipe[1], grandchild); _exit(0); }
            close(detached_pipe[1]);
        }
        if (o->new_process_group && setpgid(0, 0) != 0) goto child_failed;
        if (proc_dup_stdio(o) != 0 || (o->cwd && *o->cwd && chdir(o->cwd) != 0)) goto child_failed;
        if (o->complete_environment) execve(prog, (char *const *)argv, environment);
        else {
            for (size_t i = 0; i < o->env_count; ++i) if (setenv(o->env_keys[i], o->env_values[i], 1) != 0) goto child_failed;
            execvp(prog, (char *const *)argv);
        }
    child_failed:
        (void)proc_write_i64(report[1], errno); _exit(127);
    }
    close(report[1]); report[1] = -1;
    if (o->new_process_group) (void)setpgid(pid, pid);
    int64_t failure = 0; ssize_t received;
    do { received = read(report[0], &failure, sizeof(failure)); } while (received < 0 && errno == EINTR);
    if (received != 0) {
        if (received > 0) errno = (int)failure;
        status = received > 0 && received != sizeof(failure) ? XR_PROC_IO : xr_proc_last_error();
        (void)kill(pid, SIGKILL);
        pid_t reaped; do { reaped = waitpid(pid, NULL, 0); } while (reaped < 0 && errno == EINTR);
        goto done;
    }
    if (o->detached) {
        close(detached_pipe[1]); detached_pipe[1] = -1;
        int64_t detached_pid = -1; bool ok = proc_read_i64(detached_pipe[0], &detached_pid);
        pid_t reaped; do { reaped = waitpid(pid, NULL, 0); } while (reaped < 0 && errno == EINTR);
        if (!ok || reaped < 0 || detached_pid <= 0) { status = XR_PROC_IO; goto done; }
        *output = detached_pid;
    } else {
        if (group_slot >= 0) atomic_store_explicit(&proc_groups[group_slot], pid, memory_order_release);
        *output = (XrProcId)pid;
    }
 done:
    if (status != XR_PROC_OK && group_slot >= 0) atomic_store(&proc_groups[group_slot], 0);
    for (int i = 0; i < 2; ++i) { if (report[i] >= 0) close(report[i]); if (detached_pipe[i] >= 0) close(detached_pipe[i]); }
    for (size_t i = 0; i < count; ++i) m.free(m.context, environment[i]);
    if (environment) m.free(m.context, environment);
    return status;
}

/* A group owner retains its unreaped leader until descendants are stopped. */
XR_FUNC int xr_proc_close(XrProcId pid) {
    if (pid <= 0 || (XrProcId)(pid_t)pid != pid) { errno = EINVAL; return -1; }
    int slot = proc_group_slot(pid); bool stopped = true;
    if (slot < 0) { errno = EINVAL; return -1; }
    /* A registered leader remains unreaped, so both identifiers still name
     * this owner while termination is sent. Release the slot only after reap. */
    if (slot >= 0 && kill((pid_t)-pid, SIGKILL) != 0 && errno != ESRCH) stopped = false;
    if (kill((pid_t)pid, SIGKILL) != 0 && errno != ESRCH) stopped = false;
    pid_t result;
    do { result = waitpid((pid_t)pid, NULL, stopped ? 0 : WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == 0 || (result < 0 && errno != ECHILD)) return -1;
    if (slot >= 0) atomic_store_explicit(&proc_groups[slot], 0, memory_order_release);
    return stopped ? 0 : -1;
}
static XrProcWaitResult proc_group_wait(XrProcId pid, bool block, int *exit_code) {
    siginfo_t info = {0}; int result;
    do { result = waitid(P_PID, (id_t)pid, &info, WEXITED | WNOWAIT | (block ? 0 : WNOHANG)); }
    while (result < 0 && errno == EINTR);
    if (result < 0) return XR_PROC_WAIT_ERROR;
    if (!info.si_pid) return XR_PROC_WAIT_RUNNING;
    if (exit_code) *exit_code = info.si_code == CLD_EXITED ? info.si_status : -1;
    return XR_PROC_WAIT_EXITED;
}

int xr_proc_wait(XrProcId pid, int *exit_code) {
    if (pid <= 0 || (XrProcId)(pid_t)pid != pid) {
        if (exit_code) {
            *exit_code = -1;
        }
        return -1;
    }
    if (proc_group_slot(pid) >= 0) return proc_group_wait(pid, true, exit_code) == XR_PROC_WAIT_EXITED ? 0 : -1;
    int status = 0;
    pid_t r;
    do {
        r = waitpid((pid_t) pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        if (exit_code) {
            *exit_code = -1;
        }
        return -1;
    }
    if (exit_code) {
        if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else {
            *exit_code = -1;
        }
    }
    return 0;
}

XrProcWaitResult xr_proc_try_wait(XrProcId pid, int *exit_code) {
    if (pid <= 0 || (XrProcId)(pid_t)pid != pid) {
        if (exit_code) {
            *exit_code = -1;
        }
        return XR_PROC_WAIT_ERROR;
    }

    if (proc_group_slot(pid) >= 0) return proc_group_wait(pid, false, exit_code);
    int status = 0;
    pid_t r;
    do {
        r = waitpid((pid_t) pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == 0) {
        return XR_PROC_WAIT_RUNNING;
    }
    if (r < 0) {
        if (exit_code) {
            *exit_code = -1;
        }
        return XR_PROC_WAIT_ERROR;
    }

    if (exit_code) {
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return XR_PROC_WAIT_EXITED;
}

int xr_proc_kill(XrProcId pid, int signal) {
    if (pid <= 0 || (XrProcId)(pid_t)pid != pid || signal <= 0) {
        return -1;
    }
    return kill((pid_t) pid, signal) == 0 ? 0 : -1;
}

int xr_proc_kill_tree(XrProcId pid, int signal) {
    if (pid <= 0 || (XrProcId)(pid_t)pid != pid || signal <= 0)
        return -1;
    if (proc_group_slot(pid) < 0) { errno = EINVAL; return -1; }
    return kill((pid_t) -pid, signal) == 0 ? 0 : -1;
}
