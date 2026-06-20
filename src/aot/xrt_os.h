/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_os.h - Freestanding AOT helpers for pure OS queries.
 */

#ifndef XRT_OS_H
#define XRT_OS_H

#include "xrt_arc.h"
#include "xrt_coll.h"
#include "xrt_value.h"
#include "../base/xplatform.h"
#ifndef _WIN32
#include <errno.h>
#endif
#include <signal.h>
#include <stdbool.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/wait.h>
extern char **environ;
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

#ifndef XRT_OS_PATH_MAX
#define XRT_OS_PATH_MAX 4096
#endif

static inline XrValue xrt_os_cstr_value(const char *s) {
    return s ? xrt_str_from_cstr(s) : XR_NULL_VAL;
}

static inline const char *xrt_os_platform_cstr(void) {
#if defined(XR_OS_WINDOWS) || defined(_WIN64)
    return "windows";
#elif defined(XR_OS_MACOS) && defined(__MACH__)
    return "darwin";
#elif defined(XR_OS_LINUX)
    return "linux";
#elif defined(XR_OS_BSD)
    return "freebsd";
#else
    return "unknown";
#endif
}

static inline const char *xrt_os_arch_cstr(void) {
#if defined(XR_ARCH_ARM64) || defined(_M_ARM64)
    return "arm64";
#elif defined(XR_ARCH_X86_64) || defined(_M_X64)
    return "x64";
#elif defined(XR_ARCH_X86) || defined(_M_IX86)
    return "x86";
#elif defined(XR_ARCH_ARM) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

static inline XrValue xrt_os_platform(void) {
    return xrt_str_from_cstr(xrt_os_platform_cstr());
}

static inline XrValue xrt_os_arch(void) {
    return xrt_str_from_cstr(xrt_os_arch_cstr());
}

static inline XrValue xrt_os_sep(void) {
#ifdef XR_OS_WINDOWS
    return xrt_str_from_cstr("\\");
#else
    return xrt_str_from_cstr("/");
#endif
}

static inline XrValue xrt_os_eol(void) {
#ifdef XR_OS_WINDOWS
    return xrt_str_from_cstr("\r\n");
#else
    return xrt_str_from_cstr("\n");
#endif
}

static inline XrValue xrt_os_str_slice_value(const char *s, size_t len) {
    if (!s)
        return XR_NULL_VAL;
    XrValue v = xrt_str_alloc(len);
    if (len)
        memcpy(xr_str_buf(v), s, len);
    return v;
}

static inline char *xrt_os_copy_cstr_arg(const char *data, int64_t len, char *stack,
                                         size_t stack_cap, char **owned_out) {
    *owned_out = NULL;
    if (!data || len < 0 || stack_cap == 0)
        return NULL;
    char *out = stack;
    if ((size_t) len >= stack_cap) {
        *owned_out = (char *) XRT_MALLOC((size_t) len + 1);
        if (!*owned_out)
            return NULL;
        out = *owned_out;
    }
    memcpy(out, data, (size_t) len);
    out[len] = '\0';
    return out;
}

static inline XrValue xrt_os_getenv(const char *name, int64_t len) {
    char stack_name[256];
    char *owned = NULL;
    char *key = xrt_os_copy_cstr_arg(name, len, stack_name, sizeof(stack_name), &owned);
    if (!key)
        return XR_NULL_VAL;
    const char *value = getenv(key);
    if (owned)
        XRT_FREE(owned);
    return xrt_os_cstr_value(value);
}

static inline XrValue xrt_os_setenv(const char *name, int64_t name_len, const char *value,
                                    int64_t value_len) {
    char stack_name[256];
    char stack_value[512];
    char *owned_name = NULL;
    char *owned_value = NULL;
    char *key = xrt_os_copy_cstr_arg(name, name_len, stack_name, sizeof(stack_name), &owned_name);
    char *val =
        xrt_os_copy_cstr_arg(value, value_len, stack_value, sizeof(stack_value), &owned_value);
    bool ok = false;
    if (key && val) {
#ifdef _WIN32
        ok = _putenv_s(key, val) == 0;
#else
        ok = setenv(key, val, 1) == 0;
#endif
    }
    if (owned_name)
        XRT_FREE(owned_name);
    if (owned_value)
        XRT_FREE(owned_value);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_os_unsetenv(const char *name, int64_t len) {
    char stack_name[256];
    char *owned = NULL;
    char *key = xrt_os_copy_cstr_arg(name, len, stack_name, sizeof(stack_name), &owned);
    bool ok = false;
    if (key) {
#ifdef _WIN32
        ok = _putenv_s(key, "") == 0;
#else
        ok = unsetenv(key) == 0;
#endif
    }
    if (owned)
        XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline void xrt_os_environ_set_entry(xrt_map_t *map, const char *entry) {
    const char *eq = entry ? strchr(entry, '=') : NULL;
    if (!eq || eq == entry)
        return;
    XrValue key = xrt_os_str_slice_value(entry, (size_t) (eq - entry));
    const char *value = eq + 1;
    xrt_map_set(map, key, xrt_os_str_slice_value(value, strlen(value)));
}

static inline XrValue xrt_os_environ(void) {
    XrValue map_value = xrt_map_new(64);
    xrt_map_t *map = (xrt_map_t *) map_value.ptr;
#ifdef _WIN32
    LPCH env_block = GetEnvironmentStringsA();
    if (!env_block)
        return map_value;
    for (const char *p = env_block; *p; p += strlen(p) + 1)
        xrt_os_environ_set_entry(map, p);
    FreeEnvironmentStringsA(env_block);
#else
    for (char **env = environ; env && *env; env++)
        xrt_os_environ_set_entry(map, *env);
#endif
    return map_value;
}

static inline XrValue xrt_os_getpid(void) {
#ifdef _WIN32
    return XR_FROM_INT((int64_t) _getpid());
#else
    return XR_FROM_INT((int64_t) getpid());
#endif
}

static inline XrValue xrt_os_uid(void) {
#ifdef _WIN32
    return XR_FROM_INT(0);
#else
    return XR_FROM_INT((int64_t) getuid());
#endif
}

static inline XrValue xrt_os_gid(void) {
#ifdef _WIN32
    return XR_FROM_INT(0);
#else
    return XR_FROM_INT((int64_t) getgid());
#endif
}

static inline XrValue xrt_os_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return XR_FROM_INT((int64_t) si.dwNumberOfProcessors);
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return XR_FROM_INT((int64_t) (n > 0 ? n : 1));
#endif
}

static inline XrValue xrt_os_getcwd(void) {
    char buf[XRT_OS_PATH_MAX];
#ifdef _WIN32
    DWORD n = GetCurrentDirectoryA((DWORD) sizeof(buf), buf);
    if (n == 0 || n >= (DWORD) sizeof(buf))
        return XR_NULL_VAL;
#else
    if (!getcwd(buf, sizeof(buf)))
        return XR_NULL_VAL;
#endif
    return xrt_os_cstr_value(buf);
}

static inline XrValue xrt_os_chdir(const char *path, int64_t len) {
    char stack_path[XRT_OS_PATH_MAX];
    char *owned = NULL;
    char *dir = xrt_os_copy_cstr_arg(path, len, stack_path, sizeof(stack_path), &owned);
    bool ok = false;
    if (dir) {
#ifdef _WIN32
        ok = SetCurrentDirectoryA(dir) != 0;
#else
        ok = chdir(dir) == 0;
#endif
    }
    if (owned)
        XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_os_hostname(void) {
    char buf[256];
#ifdef _WIN32
    DWORD n = (DWORD) sizeof(buf);
    if (!GetComputerNameA(buf, &n))
        return XR_NULL_VAL;
    buf[sizeof(buf) - 1] = '\0';
#else
    if (gethostname(buf, sizeof(buf)) != 0)
        return XR_NULL_VAL;
    buf[sizeof(buf) - 1] = '\0';
#endif
    return xrt_os_cstr_value(buf);
}

static inline XrValue xrt_os_tmpdir(void) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir)
        tmpdir = getenv("TMP");
    if (!tmpdir)
        tmpdir = getenv("TEMP");
    if (!tmpdir) {
#ifdef _WIN32
        tmpdir = "C:\\Windows\\Temp";
#else
        tmpdir = "/tmp";
#endif
    }
    return xrt_os_cstr_value(tmpdir);
}

static inline XrValue xrt_os_username(void) {
#ifdef _WIN32
    const char *user = getenv("USERNAME");
    if (!user)
        user = getenv("USER");
    if (!user)
        user = getenv("LOGNAME");
    return xrt_os_cstr_value(user);
#else
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name)
        return xrt_os_cstr_value(pw->pw_name);
    const char *user = getenv("USER");
    if (!user)
        user = getenv("LOGNAME");
    return xrt_os_cstr_value(user);
#endif
}

static inline XrValue xrt_os_homedir(void) {
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home)
        home = getenv("USERPROFILE");
    return xrt_os_cstr_value(home);
#else
    if (home)
        return xrt_os_cstr_value(home);
    struct passwd *pw = getpwuid(getuid());
    return pw ? xrt_os_cstr_value(pw->pw_dir) : XR_NULL_VAL;
#endif
}

static inline XrValue xrt_os_ppid(void) {
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return XR_FROM_INT(0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = GetCurrentProcessId();
    int64_t ppid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                ppid = (int64_t) pe.th32ParentProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return XR_FROM_INT(ppid);
#else
    return XR_FROM_INT((int64_t) getppid());
#endif
}

static inline XrValue xrt_os_kill_signal(XrValue pid_value, XrValue sig_value) {
    if (!XR_IS_INT(pid_value) || !XR_IS_INT(sig_value))
        return XR_FROM_BOOL(false);
#ifdef _WIN32
    return XR_FROM_BOOL(false);
#else
    return XR_FROM_BOOL(kill((pid_t) XR_TO_INT(pid_value), (int) XR_TO_INT(sig_value)) == 0);
#endif
}

static inline XrValue xrt_os_kill(XrValue pid_value) {
    return xrt_os_kill_signal(pid_value, XR_FROM_INT(SIGTERM));
}

static inline XrValue xrt_os_total_memory(void) {
#ifdef _WIN32
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex))
        return XR_FROM_INT((int64_t) statex.ullTotalPhys);
    return XR_FROM_INT(0);
#elif defined(__APPLE__)
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0)
        return XR_FROM_INT(memsize);
    return XR_FROM_INT(0);
#elif defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return XR_FROM_INT((int64_t) si.totalram * (int64_t) si.mem_unit);
    return XR_FROM_INT(0);
#else
    return XR_FROM_INT(0);
#endif
}

static inline XrValue xrt_os_free_memory(void) {
#ifdef _WIN32
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex))
        return XR_FROM_INT((int64_t) statex.ullAvailPhys);
    return XR_FROM_INT(0);
#elif defined(__APPLE__)
    vm_statistics64_data_t vm_stat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t) &vm_stat, &count) ==
        KERN_SUCCESS)
        return XR_FROM_INT((int64_t) (vm_stat.free_count + vm_stat.inactive_count) *
                           (int64_t) vm_page_size);
    return XR_FROM_INT(0);
#elif defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return XR_FROM_INT((int64_t) si.freeram * (int64_t) si.mem_unit);
    return XR_FROM_INT(0);
#else
    return XR_FROM_INT(0);
#endif
}

static inline XrValue xrt_os_uptime(void) {
#ifdef _WIN32
    return XR_FROM_FLOAT((double) GetTickCount64() / 1000.0);
#elif defined(__APPLE__)
    struct timeval boottime;
    size_t len = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &len, NULL, 0) == 0) {
        time_t now = time(NULL);
        return XR_FROM_FLOAT((double) (now - boottime.tv_sec));
    }
    return XR_FROM_FLOAT(0.0);
#elif defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return XR_FROM_FLOAT((double) si.uptime);
    return XR_FROM_FLOAT(0.0);
#else
    return XR_FROM_FLOAT(0.0);
#endif
}

static inline XrValue xrt_os_loadavg(void) {
    double avg[3] = {0.0, 0.0, 0.0};
#ifdef _WIN32
    /* Windows has no load average equivalent in the VM stdlib today. */
#elif defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        avg[0] = (double) si.loads[0] / 65536.0;
        avg[1] = (double) si.loads[1] / 65536.0;
        avg[2] = (double) si.loads[2] / 65536.0;
    }
#else
    if (getloadavg(avg, 3) < 0) {
        avg[0] = 0.0;
        avg[1] = 0.0;
        avg[2] = 0.0;
    }
#endif
    XrValue arr = xrt_array_new(3);
    xrt_array_push(arr, XR_FROM_FLOAT(avg[0]));
    xrt_array_push(arr, XR_FROM_FLOAT(avg[1]));
    xrt_array_push(arr, XR_FROM_FLOAT(avg[2]));
    return arr;
}

static inline XrValue xrt_os_clock(void) {
    return XR_FROM_FLOAT((double) clock() / (double) CLOCKS_PER_SEC);
}

static inline XrValue xrt_os_sleep(XrValue ms_value) {
    if (!XR_IS_INT(ms_value))
        return XR_NULL_VAL;

    int64_t ms = XR_TO_INT(ms_value);
    if (ms <= 0)
        return XR_NULL_VAL;

#ifdef _WIN32
    uint64_t remaining = (uint64_t) ms;
    while (remaining > 0) {
        DWORD chunk = remaining > 0xffffffffULL ? 0xffffffffUL : (DWORD) remaining;
        Sleep(chunk);
        remaining -= (uint64_t) chunk;
    }
#else
    struct timespec req;
    req.tv_sec = (time_t) (ms / 1000);
    req.tv_nsec = (long) ((ms % 1000) * 1000000L);
    struct timespec rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
        req = rem;
#endif
    return XR_NULL_VAL;
}

static inline bool xrt_os_exec_buffer_init(char **buf, size_t *len, size_t *cap) {
    *len = 0;
    *cap = 4096;
    *buf = (char *) XRT_MALLOC(*cap);
    if (!*buf) {
        *cap = 0;
        return false;
    }
    (*buf)[0] = '\0';
    return true;
}

static inline bool xrt_os_exec_buffer_append(char **buf, size_t *len, size_t *cap, const char *data,
                                             size_t n) {
    if (!buf || !len || !cap || !data)
        return false;
    if (*len + n + 1 > *cap) {
        size_t new_cap = *cap ? *cap : 4096;
        while (*len + n + 1 > new_cap) {
            size_t next = new_cap * 2;
            if (next <= new_cap)
                return false;
            new_cap = next;
        }
        char *grown = (char *) XRT_REALLOC(*buf, new_cap);
        if (!grown)
            return false;
        *buf = grown;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static inline XrValue xrt_os_exec_result(const char *stdout_buf, const char *stderr_buf,
                                         int64_t exit_code) {
    static const char *const fields[] = {"stdout", "stderr", "exitCode"};
    XrValue obj = xrt_json_new_named(3, fields);
    xrt_json_set_field(obj, 0, xrt_os_cstr_value(stdout_buf ? stdout_buf : ""));
    xrt_json_set_field(obj, 1, xrt_os_cstr_value(stderr_buf ? stderr_buf : ""));
    xrt_json_set_field(obj, 2, XR_FROM_INT(exit_code));
    return obj;
}

#ifndef _WIN32
typedef struct {
    int fd;
    char *buf;
    size_t len;
    size_t cap;
    bool open;
} xrt_os_exec_pipe_t;

static inline bool xrt_os_exec_pipe_init(xrt_os_exec_pipe_t *pipe, int fd) {
    pipe->fd = fd;
    pipe->buf = NULL;
    pipe->len = 0;
    pipe->cap = 0;
    pipe->open = true;
    if (!xrt_os_exec_buffer_init(&pipe->buf, &pipe->len, &pipe->cap)) {
        close(fd);
        pipe->open = false;
        return false;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return true;
}

static inline void xrt_os_exec_pipe_close(xrt_os_exec_pipe_t *pipe) {
    if (pipe->open) {
        close(pipe->fd);
        pipe->open = false;
    }
}

static inline void xrt_os_exec_pipe_free(xrt_os_exec_pipe_t *pipe) {
    xrt_os_exec_pipe_close(pipe);
    if (pipe->buf)
        XRT_FREE(pipe->buf);
    pipe->buf = NULL;
    pipe->len = 0;
    pipe->cap = 0;
}

static inline bool xrt_os_exec_pipe_drain(xrt_os_exec_pipe_t *pipe) {
    char tmp[4096];
    for (;;) {
        ssize_t n = read(pipe->fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (!xrt_os_exec_buffer_append(&pipe->buf, &pipe->len, &pipe->cap, tmp, (size_t) n))
                return false;
            continue;
        }
        if (n == 0) {
            xrt_os_exec_pipe_close(pipe);
            return true;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        xrt_os_exec_pipe_close(pipe);
        return false;
    }
}

static inline bool xrt_os_read_exec_pipes(int stdout_fd, int stderr_fd, char **stdout_buf,
                                          char **stderr_buf) {
    xrt_os_exec_pipe_t pipes[2];
    if (!xrt_os_exec_pipe_init(&pipes[0], stdout_fd)) {
        close(stderr_fd);
        return false;
    }
    if (!xrt_os_exec_pipe_init(&pipes[1], stderr_fd)) {
        xrt_os_exec_pipe_free(&pipes[0]);
        return false;
    }

    while (pipes[0].open || pipes[1].open) {
        struct pollfd pfds[2];
        nfds_t nfds = 0;
        for (int i = 0; i < 2; i++) {
            if (!pipes[i].open)
                continue;
            pfds[nfds].fd = pipes[i].fd;
            pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
            pfds[nfds].revents = 0;
            nfds++;
        }

        int rc;
        do {
            rc = poll(pfds, nfds, -1);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0)
            goto fail;

        nfds_t pos = 0;
        for (int i = 0; i < 2; i++) {
            if (!pipes[i].open)
                continue;
            short revents = pfds[pos++].revents;
            if (revents & (POLLIN | POLLHUP | POLLERR)) {
                if (!xrt_os_exec_pipe_drain(&pipes[i]))
                    goto fail;
            }
        }
    }

    *stdout_buf = pipes[0].buf;
    *stderr_buf = pipes[1].buf;
    return true;

fail:
    xrt_os_exec_pipe_free(&pipes[0]);
    xrt_os_exec_pipe_free(&pipes[1]);
    return false;
}
#endif

static inline XrValue xrt_os_exec(const char *cmd_data, int64_t cmd_len) {
    char stack_cmd[1024];
    char *owned_cmd = NULL;
    char *cmd = xrt_os_copy_cstr_arg(cmd_data, cmd_len, stack_cmd, sizeof(stack_cmd), &owned_cmd);
    if (!cmd || cmd[0] == '\0') {
        if (owned_cmd)
            XRT_FREE(owned_cmd);
        return XR_NULL_VAL;
    }

#ifdef _WIN32
    FILE *fp = _popen(cmd, "r");
    if (owned_cmd)
        XRT_FREE(owned_cmd);
    if (!fp)
        return XR_NULL_VAL;

    char *output = NULL;
    size_t len = 0;
    size_t cap = 0;
    if (!xrt_os_exec_buffer_init(&output, &len, &cap)) {
        _pclose(fp);
        return XR_NULL_VAL;
    }

    char tmp[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
        if (!xrt_os_exec_buffer_append(&output, &len, &cap, tmp, n)) {
            ok = false;
            break;
        }
    }

    int raw_status = _pclose(fp);
    if (!ok) {
        XRT_FREE(output);
        return XR_NULL_VAL;
    }
    int64_t exit_code = raw_status < 0 ? -1 : (raw_status & 0xFF);
    XrValue result = xrt_os_exec_result(output, "", exit_code);
    XRT_FREE(output);
    return result;
#else
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0) {
        if (owned_cmd)
            XRT_FREE(owned_cmd);
        return XR_NULL_VAL;
    }
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        if (owned_cmd)
            XRT_FREE(owned_cmd);
        return XR_NULL_VAL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        if (owned_cmd)
            XRT_FREE(owned_cmd);
        return XR_NULL_VAL;
    }

    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    if (owned_cmd)
        XRT_FREE(owned_cmd);

    char *stdout_buf = NULL;
    char *stderr_buf = NULL;
    bool read_ok = xrt_os_read_exec_pipes(stdout_pipe[0], stderr_pipe[0], &stdout_buf, &stderr_buf);

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (!read_ok) {
        if (stdout_buf)
            XRT_FREE(stdout_buf);
        if (stderr_buf)
            XRT_FREE(stderr_buf);
        return XR_NULL_VAL;
    }

    int64_t exit_code = (waited >= 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    XrValue result = xrt_os_exec_result(stdout_buf, stderr_buf, exit_code);
    XRT_FREE(stdout_buf);
    XRT_FREE(stderr_buf);
    return result;
#endif
}

#endif  // XRT_OS_H
