/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os.c - Operating system interface module implementation
 *
 * KEY CONCEPT:
 *   Native C implementation of OS operations: environment variables,
 *   process control, and platform detection.
 */

#include "os.h"
#include "../common.h"
#include "../../src/base/xplatform.h"
#include "../../src/base/xmalloc.h"
#include "../../src/base/xchecks.h"
#include "../../src/coro/xyieldable.h"  // xr_yield_for_timeout
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../../src/os/os_fs.h"

#ifdef XR_OS_WINDOWS
#include <process.h>
#include <winsock2.h>
#else
#include <unistd.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

#ifdef XR_OS_MACOS
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif

#ifdef XR_OS_LINUX
#include <sys/sysinfo.h>
#endif

// External: environ variable
extern char **environ;

/* ========== External Declarations ========== */

struct XrCoroutine;
extern struct XrCoroutine *xr_current_coro(XrayIsolate *X);
extern XrMap *xr_map_new(struct XrCoroutine *coro);
extern void xr_map_set(XrMap *map, XrValue key, XrValue value);
extern XrValue xr_value_from_map(XrMap *map);
extern XrArray *xr_array_new(struct XrCoroutine *coro);
extern void xr_array_push(XrArray *arr, XrValue value);
extern XrValue xr_value_from_array(XrArray *arr);

/* ========== Windows Compatibility ========== */

#ifdef XR_OS_WINDOWS
#define os_setenv_impl(name, value) _putenv_s(name, value)
#define os_unsetenv_impl(name) _putenv_s(name, "")
#define os_getpid_impl() _getpid()
#else
#define os_setenv_impl(name, value) setenv(name, value, 1)
#define os_unsetenv_impl(name) unsetenv(name)
#define os_getpid_impl() getpid()
#endif

/* ========== Environment Variables ========== */

// getenv(name) - Get environment variable
static XrValue os_getenv(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *name = xrs_string_arg(args[0], NULL);
    if (!name)
        return xr_null();

    const char *value = getenv(name);
    if (!value)
        return xr_null();

    return xrs_string_value_c(X, value);
}

// setenv(name, value) - Set environment variable
static XrValue os_setenv(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);

    const char *name = xrs_string_arg(args[0], NULL);
    const char *value = xrs_string_arg(args[1], NULL);
    if (!name || !value)
        return xr_bool(false);

    int result = os_setenv_impl(name, value);
    return xr_bool(result == 0);
}

// unsetenv(name) - Delete environment variable
static XrValue os_unsetenv(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);

    const char *name = xrs_string_arg(args[0], NULL);
    if (!name)
        return xr_bool(false);

    int result = os_unsetenv_impl(name);
    return xr_bool(result == 0);
}

// environ() - Get all environment variables
static XrValue os_environ(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrMap *map = xr_map_new(xr_current_coro(X));
    if (!map)
        return xr_null();

    for (char **env = environ; *env != NULL; env++) {
        char *eq = strchr(*env, '=');
        if (!eq)
            continue;

        size_t name_len = eq - *env;
        const char *value = eq + 1;

        // Directly intern with length — no temporary allocation needed
        XrString *key_str = xr_string_intern(X, *env, name_len, 0);
        XrValue key = xr_string_value(key_str);
        XrValue val = xrs_string_value_c(X, value);
        xr_map_set(map, key, val);
    }

    return xr_value_from_map(map);
}

/* ========== Process Control ========== */

// exit(code) - Exit program
static XrValue os_exit(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;

    int code = 0;
    if (argc >= 1 && XR_IS_INT(args[0])) {
        code = (int) XR_TO_INT(args[0]);
    }

    exit(code);
    return xr_null();  // Never reached
}

// getpid() - Get process ID
static XrValue os_getpid(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
    return xr_int(os_getpid_impl());
}

// getcwd() - Get current working directory
static XrValue os_getcwd(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    char buf[PATH_MAX];
    if (xr_fs_getcwd(buf, sizeof(buf)) == NULL) {
        return xr_null();
    }
    return xrs_string_value_c(X, buf);
}

// chdir(path) - Change working directory
static XrValue os_chdir(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);

    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_chdir(path) == 0);
}

// hostname() - Get hostname
static XrValue os_hostname(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    char buf[256];
#ifdef XR_OS_WINDOWS
    // gethostname() on Windows lives in Winsock and requires WSAStartup()
    // to have been called; otherwise it returns WSANOTINITIALISED. We
    // initialise on demand (idempotent via WSACleanup pair), so this works
    // for embedded callers that have not spun up the networking stack yet.
    WSADATA wsa;
    int wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    int rc = gethostname(buf, sizeof(buf));
    if (wsa_ok)
        WSACleanup();
    if (rc != 0)
        return xr_null();
#else
    if (gethostname(buf, sizeof(buf)) != 0) {
        return xr_null();
    }
#endif
    return xrs_string_value_c(X, buf);
}

// tmpdir() - Get temporary directory
static XrValue os_tmpdir(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    // Try to get from environment variables
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir)
        tmpdir = getenv("TMP");
    if (!tmpdir)
        tmpdir = getenv("TEMP");
    if (!tmpdir) {
#ifdef XR_OS_WINDOWS
        tmpdir = "C:\\Windows\\Temp";
#else
        tmpdir = "/tmp";
#endif
    }

    return xrs_string_value_c(X, tmpdir);
}

/* ========== User Information (P1) ========== */

// username() - Get current user name
static XrValue os_username(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

#ifdef XR_OS_WINDOWS
    char buf[256];
    DWORD size = sizeof(buf);
    if (GetUserNameA(buf, &size))
        return xrs_string_value_c(X, buf);
    return xr_null();
#else
    struct passwd *pw = getpwuid(getuid());
    if (!pw)
        return xr_null();
    return xrs_string_value_c(X, pw->pw_name);
#endif
}

// homedir() - Get user home directory
static XrValue os_homedir(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    const char *home = getenv("HOME");
#ifdef XR_OS_WINDOWS
    if (!home)
        home = getenv("USERPROFILE");
    if (home)
        return xrs_string_value_c(X, home);
    return xr_null();
#else
    if (home)
        return xrs_string_value_c(X, home);
    struct passwd *pw = getpwuid(getuid());
    if (!pw)
        return xr_null();
    return xrs_string_value_c(X, pw->pw_dir);
#endif
}

// uid() - Get user ID
static XrValue os_uid(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
#ifdef XR_OS_WINDOWS
    return xr_int(0);
#else
    return xr_int(getuid());
#endif
}

// gid() - Get group ID
static XrValue os_gid(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
#ifdef XR_OS_WINDOWS
    return xr_int(0);
#else
    return xr_int(getgid());
#endif
}

/* ========== System Information (P2) ========== */

// cpuCount() - Get number of CPU cores
static XrValue os_cpuCount(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;

#ifdef XR_OS_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return xr_int(si.dwNumberOfProcessors);
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return xr_int(n > 0 ? n : 1);
#endif
}

// totalMemory() - Get total system memory in bytes
static XrValue os_totalMemory(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;

#ifdef XR_OS_MACOS
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
    return xr_int(memsize);
#elif defined(XR_OS_LINUX)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return xr_int((int64_t) si.totalram * si.mem_unit);
    return xr_int(0);
#else
    return xr_int(0);
#endif
}

// freeMemory() - Get available system memory in bytes
static XrValue os_freeMemory(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;

#ifdef XR_OS_MACOS
    vm_statistics64_data_t vm_stat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t) &vm_stat, &count) ==
        KERN_SUCCESS) {
        int64_t free_bytes = (int64_t) (vm_stat.free_count + vm_stat.inactive_count) * vm_page_size;
        return xr_int(free_bytes);
    }
    return xr_int(0);
#elif defined(XR_OS_LINUX)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return xr_int((int64_t) si.freeram * si.mem_unit);
    return xr_int(0);
#else
    return xr_int(0);
#endif
}

// uptime() - Get system uptime in seconds
static XrValue os_uptime(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;

#ifdef XR_OS_MACOS
    struct timeval boottime;
    size_t len = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &len, NULL, 0) == 0) {
        time_t now = time(NULL);
        return xr_float((double) (now - boottime.tv_sec));
    }
    return xr_float(0.0);
#elif defined(XR_OS_LINUX)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return xr_float((double) si.uptime);
    return xr_float(0.0);
#else
    return xr_float(0.0);
#endif
}

// loadavg() - Get system load averages (1, 5, 15 min)
static XrValue os_loadavg(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr)
        return xr_null();

#ifndef XR_OS_WINDOWS
    double avg[3] = {0};
    getloadavg(avg, 3);
    xr_array_push(arr, xr_float(avg[0]));
    xr_array_push(arr, xr_float(avg[1]));
    xr_array_push(arr, xr_float(avg[2]));
#else
    xr_array_push(arr, xr_float(0.0));
    xr_array_push(arr, xr_float(0.0));
    xr_array_push(arr, xr_float(0.0));
#endif

    return xr_value_from_array(arr);
}

/* ========== Process & Signal (P3) ========== */

// ppid() - Get parent process ID
static XrValue os_ppid(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
#ifdef XR_OS_WINDOWS
    return xr_int(0);
#else
    return xr_int(getppid());
#endif
}

// kill(pid, signal) - Send signal to process
static XrValue os_kill(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    if (!XR_IS_INT(args[0]))
        return xr_bool(false);

    int pid = (int) XR_TO_INT(args[0]);
    int sig = SIGTERM;  // default signal
    if (argc >= 2 && XR_IS_INT(args[1])) {
        sig = (int) XR_TO_INT(args[1]);
    }

#ifdef XR_OS_WINDOWS
    return xr_bool(false);
#else
    return xr_bool(kill(pid, sig) == 0);
#endif
}

// Continuation for os.sleep — timer fired, return null.
static XrCFuncResult os_sleep_done(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void) X;
    (void) status;
    (void) ctx;
    *result = xr_null();
    return XR_CFUNC_DONE;
}

// sleep(ms) - Coroutine-friendly sleep for milliseconds.
// Yields the coroutine via the timer wheel so the worker thread can
// service other coroutines during the wait.
static XrCFuncResult os_sleep(XrayIsolate *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 1 || !XR_IS_INT(args[0])) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    int64_t ms = XR_TO_INT(args[0]);
    if (ms <= 0) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    return xr_yield_for_timeout(X, ms, os_sleep_done, NULL, result);
}

// clock() - Get process CPU time in seconds
static XrValue os_clock(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
    return xr_float((double) clock() / CLOCKS_PER_SEC);
}

/* ========== Process Execution (P0) ========== */

#ifndef XR_OS_WINDOWS
// Read all data from a file descriptor into a heap-allocated string
static char *read_fd_to_string(int fd) {
    XR_DCHECK(fd >= 0, "read_fd_to_string: fd must be non-negative");
    size_t cap = 4096, len = 0;
    char *buf = (char *) xr_malloc(cap);
    if (!buf)
        return NULL;

    ssize_t n;
    while ((n = read(fd, buf + len, cap - len - 1)) > 0) {
        len += (size_t) n;
        if (len + 1 >= cap) {
            size_t new_cap = cap * 2;
            if (!XR_REALLOC(buf, new_cap)) {
                xr_free(buf);
                return NULL;
            }
            cap = new_cap;
        }
    }
    buf[len] = '\0';
    return buf;
}
#endif

// exec(cmd) - Execute shell command, return {stdout, stderr, exitCode}
static XrValue os_exec(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *cmd = xrs_string_arg(args[0], NULL);
    if (!cmd)
        return xr_null();
    XR_DCHECK(cmd[0] != '\0', "os_exec: command string must be non-empty");

#ifdef XR_OS_WINDOWS
    // Windows: simplified via _popen (stdout only)
    FILE *fp = _popen(cmd, "r");
    if (!fp)
        return xr_null();

    char buf[4096];
    size_t len = 0, cap = 4096;
    char *output = (char *) xr_malloc(cap);
    if (!output) {
        _pclose(fp);
        return xr_null();
    }

    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (len + n + 1 >= cap) {
            size_t new_cap = cap * 2;
            while (len + n + 1 >= new_cap)
                new_cap *= 2;
            if (!XR_REALLOC(output, new_cap)) {
                xr_free(output);
                _pclose(fp);
                return xr_null();
            }
            cap = new_cap;
        }
        memcpy(output + len, buf, n);
        len += n;
    }
    output[len] = '\0';
    // _pclose returns the same wait-style encoding as _cwait/_spawn, so the
    // exit code lives in the low-order byte only when the child terminated
    // normally. Treat negative values (close itself failed) as -1.
    int raw_status = _pclose(fp);
    int exit_code = raw_status;
    if (raw_status < 0) {
        exit_code = -1;
    } else {
        exit_code = raw_status & 0xFF;
    }

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, xrs_string_value_c(X, "stdout"), xrs_string_value_c(X, output));
    xr_map_set(map, xrs_string_value_c(X, "stderr"), xrs_string_value_c(X, ""));
    xr_map_set(map, xrs_string_value_c(X, "exitCode"), xr_int(exit_code));
    xr_free(output);
    return xr_value_from_map(map);
#else
    // Unix: fork + exec + pipe for both stdout and stderr
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0)
        return xr_null();
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return xr_null();
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return xr_null();
    }

    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    char *stdout_buf = read_fd_to_string(stdout_pipe[0]);
    char *stderr_buf = read_fd_to_string(stderr_pipe[0]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, xrs_string_value_c(X, "stdout"),
               xrs_string_value_c(X, stdout_buf ? stdout_buf : ""));
    xr_map_set(map, xrs_string_value_c(X, "stderr"),
               xrs_string_value_c(X, stderr_buf ? stderr_buf : ""));
    xr_map_set(map, xrs_string_value_c(X, "exitCode"), xr_int(exit_code));

    xr_free(stdout_buf);
    xr_free(stderr_buf);
    return xr_value_from_map(map);
#endif
}

/* ========== Platform Information ========== */

// Get operating system name
static const char *get_platform(void) {
#if defined(XR_OS_WINDOWS) || defined(_WIN64)
    return "windows";
#elif defined(XR_OS_MACOS) && defined(__MACH__)
    return "darwin";
#elif defined(XR_OS_LINUX)
    return "linux";
#elif defined(XR_OS_BSD)
    return "freebsd";
#elif defined(XR_OS_BSD)
    return "openbsd";
#else
    return "unknown";
#endif
}

// Get processor architecture
static const char *get_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module os

XR_DEFINE_BUILTIN(os_getenv, "getenv", "(name: string): string?", "Get environment variable")
XR_DEFINE_BUILTIN(os_setenv, "setenv", "(name: string, value: string): bool",
                  "Set environment variable")
XR_DEFINE_BUILTIN(os_unsetenv, "unsetenv", "(name: string): bool", "Unset environment variable")
XR_DEFINE_BUILTIN(os_environ, "environ", "(): Map<string, string>", "Get all environment variables")
XR_DEFINE_BUILTIN(os_exit, "exit", "(code?: int): void", "Exit process")
XR_DEFINE_BUILTIN(os_getpid, "getpid", "(): int", "Get process ID")
XR_DEFINE_BUILTIN(os_getcwd, "getcwd", "(): string", "Get current working directory")
XR_DEFINE_BUILTIN(os_chdir, "chdir", "(path: string): bool", "Change working directory")
XR_DEFINE_BUILTIN(os_hostname, "hostname", "(): string", "Get hostname")
XR_DEFINE_BUILTIN(os_tmpdir, "tmpdir", "(): string", "Get temporary directory path")

// User information
XR_DEFINE_BUILTIN(os_username, "username", "(): string?", "Get current user name")
XR_DEFINE_BUILTIN(os_homedir, "homedir", "(): string?", "Get user home directory")
XR_DEFINE_BUILTIN(os_uid, "uid", "(): int", "Get user ID")
XR_DEFINE_BUILTIN(os_gid, "gid", "(): int", "Get group ID")

// System information
XR_DEFINE_BUILTIN(os_cpuCount, "cpuCount", "(): int", "Get number of CPU cores")
XR_DEFINE_BUILTIN(os_totalMemory, "totalMemory", "(): int", "Get total system memory in bytes")
XR_DEFINE_BUILTIN(os_freeMemory, "freeMemory", "(): int", "Get available system memory in bytes")
XR_DEFINE_BUILTIN(os_uptime, "uptime", "(): float", "Get system uptime in seconds")
XR_DEFINE_BUILTIN(os_loadavg, "loadavg", "(): Array<float>",
                  "Get system load averages (1, 5, 15 min)")

// Process & signal
XR_DEFINE_BUILTIN(os_ppid, "ppid", "(): int", "Get parent process ID")
XR_DEFINE_BUILTIN(os_kill, "kill", "(pid: int, signal?: int): bool", "Send signal to process")
XR_DEFINE_BUILTIN(os_sleep, "sleep", "(ms: int): void", "Sleep for milliseconds")
XR_DEFINE_BUILTIN(os_clock, "clock", "(): float", "Get process CPU time in seconds")

// Process execution
XR_DEFINE_BUILTIN(os_exec, "exec", "(cmd: string): Map<string, any>?", "Execute shell command")

XrModule *xr_load_module_os(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_os: NULL isolate");

    // 1. Create native module
    XrModule *mod = xr_module_create_native(isolate, "os");
    if (!mod)
        return NULL;

    // 2. Add exported functions
    XRS_EXPORT(mod, isolate, "getenv", os_getenv);
    XRS_EXPORT(mod, isolate, "setenv", os_setenv);
    XRS_EXPORT(mod, isolate, "unsetenv", os_unsetenv);
    XRS_EXPORT(mod, isolate, "environ", os_environ);

    // Process control
    XRS_EXPORT(mod, isolate, "exit", os_exit);
    XRS_EXPORT(mod, isolate, "getpid", os_getpid);
    XRS_EXPORT(mod, isolate, "getcwd", os_getcwd);
    XRS_EXPORT(mod, isolate, "chdir", os_chdir);
    XRS_EXPORT(mod, isolate, "hostname", os_hostname);
    XRS_EXPORT(mod, isolate, "tmpdir", os_tmpdir);

    // User information
    XRS_EXPORT(mod, isolate, "username", os_username);
    XRS_EXPORT(mod, isolate, "homedir", os_homedir);
    XRS_EXPORT(mod, isolate, "uid", os_uid);
    XRS_EXPORT(mod, isolate, "gid", os_gid);

    // System information
    XRS_EXPORT(mod, isolate, "cpuCount", os_cpuCount);
    XRS_EXPORT(mod, isolate, "totalMemory", os_totalMemory);
    XRS_EXPORT(mod, isolate, "freeMemory", os_freeMemory);
    XRS_EXPORT(mod, isolate, "uptime", os_uptime);
    XRS_EXPORT(mod, isolate, "loadavg", os_loadavg);

    // Process & signal
    XRS_EXPORT(mod, isolate, "ppid", os_ppid);
    XRS_EXPORT(mod, isolate, "kill", os_kill);
    XRS_EXPORT_YIELDABLE(mod, isolate, "sleep", os_sleep);
    XRS_EXPORT(mod, isolate, "clock", os_clock);

    // Process execution
    XRS_EXPORT(mod, isolate, "exec", os_exec);

    // 3. Add constants
    xr_module_add_export(isolate, mod, "platform", xrs_string_value_c(isolate, get_platform()));
    xr_module_add_export(isolate, mod, "arch", xrs_string_value_c(isolate, get_arch()));

#ifdef XR_OS_WINDOWS
    xr_module_add_export(isolate, mod, "sep", xrs_string_value_c(isolate, "\\"));
    xr_module_add_export(isolate, mod, "eol", xrs_string_value_c(isolate, "\r\n"));
#else
    xr_module_add_export(isolate, mod, "sep", xrs_string_value_c(isolate, "/"));
    xr_module_add_export(isolate, mod, "eol", xrs_string_value_c(isolate, "\n"));
#endif

    // 4. Mark as loaded
    mod->loaded = true;
    return mod;
}
