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

#include "../common.h"
#include "../../src/base/xplatform.h"
#include <stdlib.h>
#include <string.h>
#include "../../src/os/os_fs.h"
#include "../../src/shared/xr_os_core.h"

#include <signal.h>
#include <time.h>

#ifdef XR_OS_WINDOWS
#include <process.h>
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

#ifdef XR_OS_MACOS
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif

#ifdef XR_OS_LINUX
#include <sys/sysinfo.h>
#endif

// External: environ variable
#ifndef XR_OS_WINDOWS
extern char **environ;
#endif

/* ========== External Declarations ========== */

struct XrCoroutine;
extern struct XrCoroutine *xr_current_coro(XrVMRuntime *X);
extern XrArray *xr_array_new(struct XrCoroutine *coro);
extern void xr_array_push(XrArray *arr, XrValue value);
extern XrValue xr_value_from_array(XrArray *arr);

/* ========== Environment Variables ========== */

// getenv(name) - Get environment variable
static XrValue os_getenv(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *name = xrs_string_arg(args[0], NULL);
    if (!name)
        return xr_null();

    const char *value = NULL;
#ifdef XR_OS_WINDOWS
    static XR_THREAD_LOCAL char windows_value[32768];
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableA(name, windows_value, (DWORD) sizeof(windows_value));
    if (!(length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) &&
        length < sizeof(windows_value)) {
        windows_value[length] = '\0';
        value = windows_value;
    }
#else
    value = getenv(name);
#endif
    if (!value)
        return xr_null();

    return xrs_string_value_c(X, value);
}

// setenv(name, value) - Set environment variable
static XrValue os_setenv(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);

    const char *name = xrs_string_arg(args[0], NULL);
    const char *value = xrs_string_arg(args[1], NULL);
    if (!name || !value)
        return xr_bool(false);

    int result = -1;
#ifdef XR_OS_WINDOWS
    if (SetEnvironmentVariableA(name, value)) {
        /* Keep the CRT view synchronized when it can represent the value. */
        (void) _putenv_s(name, value);
        result = 0;
    }
#else
    result = setenv(name, value, 1);
#endif
    return xr_bool(result == 0);
}

// unsetenv(name) - Delete environment variable
static XrValue os_unsetenv(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);

    const char *name = xrs_string_arg(args[0], NULL);
    if (!name)
        return xr_bool(false);

    int result = -1;
#ifdef XR_OS_WINDOWS
    if (SetEnvironmentVariableA(name, NULL)) {
        (void) _putenv_s(name, "");
        result = 0;
    }
#else
    result = unsetenv(name);
#endif
    return xr_bool(result == 0);
}

/* Publish the host environment block as raw NAME=VALUE strings. Splitting the
 * entries is module policy and lives in os.xr. */
static XrValue os_environ_block(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr)
        return xr_null();

#ifdef XR_OS_WINDOWS
    LPCH env_block = GetEnvironmentStringsA();
    if (!env_block)
        return xr_value_from_array(arr);
    for (const char *p = env_block; *p; p += strlen(p) + 1)
        xr_array_push(arr, xrs_string_value_c(X, p));
    FreeEnvironmentStringsA(env_block);
#else
    for (char **env = environ; *env != NULL; env++)
        xr_array_push(arr, xrs_string_value_c(X, *env));
#endif

    return xr_value_from_array(arr);
}

/* ========== Process Control ========== */

// exit(code) - Exit program
static XrValue os_exit(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc != 1 || !XR_IS_INT(args[0]))
        return xr_null();
    int code = (int) XR_TO_INT(args[0]);
    exit(code);
    return xr_null();  // Never reached
}

// getpid() - Get process ID
static XrValue os_getpid(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
    return xr_int(xr_os_core_getpid());
}

// getcwd() - Get current working directory
static XrValue os_getcwd(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    char buf[XR_PATH_MAX];
    if (xr_fs_getcwd(buf, sizeof(buf)) == NULL) {
        return xr_null();
    }
    return xrs_string_value_c(X, buf);
}

// hostname() - Get hostname
static XrValue os_hostname(XrVMRuntime *X, XrValue *args, int argc) {
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
static XrValue os_system_username(XrVMRuntime *X, XrValue *args, int argc) {
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
/* The host's own answer only; the environment chain is the module's Xray body. */
static XrValue os_system_homedir(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
#ifdef XR_OS_WINDOWS
    return xr_null();
#else
    struct passwd *pw = getpwuid(getuid());
    return (pw && pw->pw_dir) ? xrs_string_value_c(X, pw->pw_dir) : xr_null();
#endif
}

static XrValue os_uid(XrVMRuntime *X, XrValue *args, int argc) {
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
static XrValue os_gid(XrVMRuntime *X, XrValue *args, int argc) {
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
static XrValue os_cpuCount(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;

#ifdef XR_OS_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return xr_int(si.dwNumberOfProcessors);
#else
    return xr_int((int64_t) sysconf(_SC_NPROCESSORS_ONLN));
#endif
}

// totalMemory() - Get total system memory in bytes
static XrValue os_totalMemory(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;

#ifdef XR_OS_WINDOWS
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex))
        return xr_int((int64_t) statex.ullTotalPhys);
    return xr_int(0);
#elif defined(XR_OS_MACOS)
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
    return xr_int(memsize);
#elif defined(XR_OS_LINUX)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return xr_int(xr_os_core_memory_bytes(si.totalram, si.mem_unit));
    return xr_int(0);
#else
    return xr_int(0);
#endif
}

// freeMemory() - Get available system memory in bytes
static XrValue os_freeMemory(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;

#ifdef XR_OS_WINDOWS
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex))
        return xr_int((int64_t) statex.ullAvailPhys);
    return xr_int(0);
#elif defined(XR_OS_MACOS)
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
        return xr_int(xr_os_core_memory_bytes(si.freeram, si.mem_unit));
    return xr_int(0);
#else
    return xr_int(0);
#endif
}

// uptime() - Get system uptime in seconds
static XrValue os_uptime(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;

#ifdef XR_OS_WINDOWS
    return xr_float((double) GetTickCount64() / 1000.0);
#elif defined(XR_OS_MACOS)
    struct timeval boottime;
    size_t len = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &len, NULL, 0) == 0) {
        return xr_float(
            xr_os_core_uptime_from_boot_seconds((int64_t) time(NULL), (int64_t) boottime.tv_sec));
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return xr_float(xr_os_core_seconds_from_nsec((int64_t) ts.tv_sec, (int64_t) ts.tv_nsec));
    return xr_float(0.0);
#elif defined(XR_OS_LINUX)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return xr_float((double) si.uptime);
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return xr_float(xr_os_core_seconds_from_nsec((int64_t) ts.tv_sec, (int64_t) ts.tv_nsec));
    return xr_float(0.0);
#else
    return xr_float(0.0);
#endif
}

// loadavg() - Get system load averages (1, 5, 15 min)
static XrValue os_loadavg(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr)
        return xr_null();

    double avg[3];
    xr_os_core_loadavg_zero(avg);
#ifndef XR_OS_WINDOWS
    double raw[3] = {0.0, 0.0, 0.0};
    getloadavg(raw, 3);
    xr_os_core_loadavg_set(avg, raw[0], raw[1], raw[2]);
#endif
    xr_array_push(arr, xr_float(avg[0]));
    xr_array_push(arr, xr_float(avg[1]));
    xr_array_push(arr, xr_float(avg[2]));

    return xr_value_from_array(arr);
}

/* ========== Process & Signal (P3) ========== */

// ppid() - Get parent process ID
static XrValue os_ppid(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
#ifdef XR_OS_WINDOWS
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return xr_int(0);
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
    return xr_int(ppid);
#else
    return xr_int(getppid());
#endif
}

// kill(pid, signal) - Send signal to process
static XrValue os_kill(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc != 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_bool(false);

    int pid = (int) XR_TO_INT(args[0]);
    int sig = (int) XR_TO_INT(args[1]);

#ifdef XR_OS_WINDOWS
    return xr_bool(false);
#else
    return xr_bool(kill(pid, sig) == 0);
#endif
}

/* ========== Platform Information ========== */

// Report host facts used by the public Xray wrappers. The ladders themselves
// are shared with the AOT runtime in xr_os_core.h so the two agree by
// construction.
static XrValue os_platform(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xrs_string_value_c(X, xr_os_core_platform());
}

static XrValue os_arch(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xrs_string_value_c(X, xr_os_core_arch());
}

/* ========== Module Loading ========== */

#define XR_STDLIB_VM_BIND_MODULE_OS 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_OS
