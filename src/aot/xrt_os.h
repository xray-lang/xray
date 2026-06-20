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
#include "xrt_value.h"
#include <time.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#include <pwd.h>
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

static inline XrValue xrt_os_getenv(const char *name, int64_t len) {
    if (!name || len < 0)
        return XR_NULL_VAL;
    char stack_name[256];
    char *owned = NULL;
    char *key = stack_name;
    if ((size_t) len >= sizeof(stack_name)) {
        owned = (char *) XRT_MALLOC((size_t) len + 1);
        if (!owned)
            return XR_NULL_VAL;
        key = owned;
    }
    memcpy(key, name, (size_t) len);
    key[len] = '\0';
    const char *value = getenv(key);
    if (owned)
        XRT_FREE(owned);
    return xrt_os_cstr_value(value);
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

static inline XrValue xrt_os_clock(void) {
    return XR_FROM_FLOAT((double) clock() / (double) CLOCKS_PER_SEC);
}

#endif  // XRT_OS_H
