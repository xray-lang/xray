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

static bool proc_env_key_valid(const char *key) {
    if (!key || key[0] == '\0')
        return false;
    return strchr(key, '=') == NULL;
}

static bool proc_spawn_options_valid(const XrProcSpawnOptions *options) {
    if (!options || options->env_count == 0)
        return true;
    if (!options->env_keys || !options->env_values)
        return false;
    for (size_t i = 0; i < options->env_count; i++) {
        if (!proc_env_key_valid(options->env_keys[i]) || !options->env_values[i])
            return false;
    }
    return true;
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

XrProcId xr_proc_spawn_ex(const char *prog, const char *const argv[],
                          const XrProcSpawnOptions *options) {
    if (prog == NULL || argv == NULL || !proc_spawn_options_valid(options)) {
        return XR_PROC_INVALID;
    }
    int detached_pipe[2] = {-1, -1};
    bool detached = options && options->detached;
    if (detached && pipe(detached_pipe) != 0)
        return XR_PROC_INVALID;

    pid_t pid = fork();
    if (pid < 0) {
        if (detached) {
            close(detached_pipe[0]);
            close(detached_pipe[1]);
        }
        return XR_PROC_INVALID;
    }
    if (pid == 0) {
        if (detached) {
            close(detached_pipe[0]);
            if (setsid() < 0) {
                (void) proc_write_i64(detached_pipe[1], (int64_t) XR_PROC_INVALID);
                close(detached_pipe[1]);
                _exit(127);
            }
            pid_t grandchild = fork();
            if (grandchild < 0) {
                (void) proc_write_i64(detached_pipe[1], (int64_t) XR_PROC_INVALID);
                close(detached_pipe[1]);
                _exit(127);
            }
            if (grandchild > 0) {
                (void) proc_write_i64(detached_pipe[1], (int64_t) grandchild);
                close(detached_pipe[1]);
                _exit(0);
            }
            close(detached_pipe[1]);
        }
        if (options && options->new_process_group && setpgid(0, 0) != 0)
            _exit(127);
        // Child: exec, never returns on success.
        if (proc_dup_stdio(options) != 0) {
            _exit(127);
        }
        if (options && options->cwd && options->cwd[0] != '\0' && chdir(options->cwd) != 0) {
            _exit(127);
        }
        if (options && options->env_count > 0) {
            for (size_t i = 0; i < options->env_count; i++) {
                if (setenv(options->env_keys[i], options->env_values[i], 1) != 0)
                    _exit(127);
            }
        }
        execvp(prog, (char *const *) argv);
        // exec failed; emit a short message and exit with 127 so
        // the parent's wait observes the failure. 127 matches the
        // shell convention for "command not found".
        _exit(127);
    }
    if (options && options->new_process_group)
        (void) setpgid(pid, pid);
    if (detached) {
        close(detached_pipe[1]);
        int64_t detached_pid = (int64_t) XR_PROC_INVALID;
        bool ok = proc_read_i64(detached_pipe[0], &detached_pid);
        close(detached_pipe[0]);
        int status = 0;
        pid_t r;
        do {
            r = waitpid(pid, &status, 0);
        } while (r < 0 && errno == EINTR);
        if (!ok || r < 0 || detached_pid <= 0)
            return XR_PROC_INVALID;
        return (XrProcId) detached_pid;
    }
    return (XrProcId) pid;
}

XrProcId xr_proc_spawn(const char *prog, const char *const argv[]) {
    return xr_proc_spawn_ex(prog, argv, NULL);
}

int xr_proc_wait(XrProcId pid, int *exit_code) {
    if (pid <= 0) {
        if (exit_code) {
            *exit_code = -1;
        }
        return -1;
    }
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
    if (pid <= 0) {
        if (exit_code) {
            *exit_code = -1;
        }
        return XR_PROC_WAIT_ERROR;
    }

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
    if (pid <= 0 || signal <= 0) {
        return -1;
    }
    return kill((pid_t) pid, signal) == 0 ? 0 : -1;
}

int xr_proc_kill_tree(XrProcId pid, int signal) {
    if (pid <= 0 || signal <= 0)
        return -1;
    return kill((pid_t) -pid, signal) == 0 ? 0 : -1;
}

int64_t xr_proc_self_pid(void) {
    return (int64_t) getpid();
}

int xr_proc_self_exe_path(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return -1;
    }
#if defined(XR_OS_MACOS)
    char raw[PATH_MAX];
    uint32_t raw_size = (uint32_t) sizeof(raw);
    if (_NSGetExecutablePath(raw, &raw_size) != 0) {
        return -1;
    }
    char resolved[PATH_MAX];
    const char *src = realpath(raw, resolved) ? resolved : raw;
    size_t len = strlen(src);
    if (len + 1 > size) {
        return -1;
    }
    memcpy(buf, src, len + 1);
    return 0;
#elif defined(XR_OS_LINUX)
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    return 0;
#else
    /* BSDs vary (sysctl KERN_PROC_PATHNAME vs /proc); no in-tree
     * caller targets them yet, so report unsupported rather than
     * guess a wrong path. */
    return -1;
#endif
}

bool xr_proc_debugger_attached(void) {
#if defined(XR_OS_MACOS)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info;
    memset(&info, 0, sizeof(info));
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0) {
        return false;
    }
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#elif defined(XR_OS_LINUX)
    // /proc/self/status has a "TracerPid:\t<n>\n" line; non-zero
    // means a debugger is attached.
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) {
        return false;
    }
    char line[256];
    bool attached = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            const char *p = line + 10;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p && *p != '0') {
                attached = true;
            }
            break;
        }
    }
    fclose(f);
    return attached;
#else
    return false;
#endif
}
