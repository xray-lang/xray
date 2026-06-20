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

#include "xrt_value.h"
#include <time.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

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
