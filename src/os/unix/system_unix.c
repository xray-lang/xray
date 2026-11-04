/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * system_unix.c - Typed POSIX identities, signals and system information
 */
#include "../os_system.h"
#include "../../base/xio_policy.inc.h"
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(XR_OS_LINUX)
#include <sys/sysinfo.h>
#elif defined(XR_OS_MACOS)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#endif

static XrOsIoStatus system_unsigned_i64(uintmax_t value, int64_t *output) {
    if (value > INT64_MAX) return XR_OS_IO_OVERFLOW;
    *output = (int64_t)value;
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_os_io_uid(XrOsOptionalI64 *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    int64_t value = 0;
    XrOsIoStatus status = system_unsigned_i64((uintmax_t)getuid(), &value);
    if (status == XR_OS_IO_OK) *output = (XrOsOptionalI64){true, value};
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_gid(XrOsOptionalI64 *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    int64_t value = 0;
    XrOsIoStatus status = system_unsigned_i64((uintmax_t)getgid(), &value);
    if (status == XR_OS_IO_OK) *output = (XrOsOptionalI64){true, value};
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_system_close(XrOsSystemOwner *owner) {
    if (!owner) return XR_OS_IO_BAD_ARGUMENT;
    if (!owner->handle) return XR_OS_IO_OK;
#if defined(XR_OS_MACOS)
    if (owner->handle < 0 || (uintmax_t)owner->handle > UINT32_MAX) return XR_OS_IO_BAD_ARGUMENT;
    kern_return_t closed = mach_port_deallocate(mach_task_self(), (mach_port_t)owner->handle);
    if (closed != KERN_SUCCESS) return XR_OS_IO_IO;
    owner->handle = 0;
#else
    return XR_OS_IO_BAD_ARGUMENT;
#endif
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_os_io_ppid(XrOsSystemOwner *owner, void *context,
    bool (*current)(void *context), int64_t *output) {
    if (!owner || owner->handle || !output) return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(context)) return XR_OS_IO_CANCELLED;
    pid_t parent = getppid();
    if (parent < 0) return XR_OS_IO_IO;
    int64_t result = 0;
    XrOsIoStatus status = system_unsigned_i64((uintmax_t)parent, &result);
    if (current && !current(context)) return XR_OS_IO_CANCELLED;
    if (status == XR_OS_IO_OK) *output = result;
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_kill(XrOsSystemOwner *owner, int64_t pid,
    int64_t signal, void *context, bool (*current)(void *context), bool *output) {
    if (!owner || owner->handle || !output) return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(context)) return XR_OS_IO_CANCELLED;
    /* Supported POSIX targets use signed int process IDs. Range-check before
     * conversion; zero and negative group IDs retain kill(2) semantics. */
    _Static_assert(sizeof(pid_t) == sizeof(int), "POSIX process IDs use int width");
    if (pid < INT_MIN || pid > INT_MAX || signal < 0 || signal > INT_MAX) {
        *output = false;
        return XR_OS_IO_OK;
    }
    if (current && !current(context)) return XR_OS_IO_CANCELLED;
    errno = 0;
    bool result = kill((pid_t)pid, (int)signal) == 0;
    int error = errno;
    if (!result && error != EINVAL && error != ESRCH && error != EPERM && error != EACCES)
        return io_errno_status(error);
    *output = result;
    return XR_OS_IO_OK;
}

#if defined(XR_OS_LINUX)
static XrOsIoStatus system_memory(bool available, XrOsSystemOwner *owner, int64_t *output) {
    (void)owner;
    struct sysinfo information;
    if (sysinfo(&information) != 0) return io_errno_status(errno);
    uintmax_t units = available ? information.freeram : information.totalram;
    uintmax_t scale = information.mem_unit;
    if (!scale) return XR_OS_IO_IO;
    if (units > (uintmax_t)INT64_MAX / scale) return XR_OS_IO_OVERFLOW;
    return system_unsigned_i64(units * scale, output);
}
#elif defined(XR_OS_MACOS)
static XrOsIoStatus system_memory(bool available, XrOsSystemOwner *owner, int64_t *output) {
    if (!available) {
        uint64_t bytes = 0;
        size_t length = sizeof(bytes);
        if (sysctlbyname("hw.memsize", &bytes, &length, NULL, 0) != 0) return io_errno_status(errno);
        if (length != sizeof(bytes)) return XR_OS_IO_IO;
        return system_unsigned_i64(bytes, output);
    }
    vm_statistics64_data_t information;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    mach_port_t host = mach_host_self();
    if (!MACH_PORT_VALID(host)) return XR_OS_IO_IO;
    owner->handle = (intptr_t)host;
    kern_return_t queried = host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&information, &count);
    XrOsIoStatus closed = xr_os_io_system_close(owner);
    if (queried != KERN_SUCCESS)
        return queried == KERN_RESOURCE_SHORTAGE ? XR_OS_IO_OUT_OF_MEMORY : XR_OS_IO_IO;
    if (closed != XR_OS_IO_OK) return closed;
    if (count != HOST_VM_INFO64_COUNT) return XR_OS_IO_IO;
    uintmax_t free_pages = information.free_count, inactive_pages = information.inactive_count;
    if (free_pages > UINTMAX_MAX - inactive_pages) return XR_OS_IO_OVERFLOW;
    uintmax_t pages = free_pages + inactive_pages, scale = vm_page_size;
    if (!scale) return XR_OS_IO_IO;
    if (pages > (uintmax_t)INT64_MAX / scale) return XR_OS_IO_OVERFLOW;
    return system_unsigned_i64(pages * scale, output);
}
#else
static XrOsIoStatus system_memory(bool available, XrOsSystemOwner *owner, int64_t *output) {
    (void)available; (void)owner; (void)output;
    return XR_OS_IO_UNSUPPORTED;
}
#endif
XR_FUNC XrOsIoStatus xr_os_io_total_memory(int64_t *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    return system_memory(false, NULL, output);
}
XR_FUNC XrOsIoStatus xr_os_io_free_memory(XrOsSystemOwner *owner, int64_t *output) {
    if (!owner || owner->handle || !output) return XR_OS_IO_BAD_ARGUMENT;
    return system_memory(true, owner, output);
}
XR_FUNC XrOsIoStatus xr_os_io_uptime(double *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
#if defined(XR_OS_LINUX)
    struct sysinfo information;
    if (sysinfo(&information) != 0) return io_errno_status(errno);
    if (information.uptime < 0) return XR_OS_IO_IO;
    *output = (double)information.uptime;
    return XR_OS_IO_OK;
#elif defined(XR_OS_MACOS)
    struct timeval boot, now;
    size_t length = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &length, NULL, 0) != 0 || gettimeofday(&now, NULL) != 0)
        return io_errno_status(errno);
    if (length != sizeof(boot) || boot.tv_sec < 0 || boot.tv_usec < 0 || boot.tv_usec >= 1000000 ||
        now.tv_sec < boot.tv_sec || now.tv_usec < 0 || now.tv_usec >= 1000000) return XR_OS_IO_IO;
    double result = (double)(now.tv_sec - boot.tv_sec) + (double)(now.tv_usec - boot.tv_usec) / 1000000.0;
    if (result < 0.0) return XR_OS_IO_IO;
    *output = result;
    return XR_OS_IO_OK;
#else
    return XR_OS_IO_UNSUPPORTED;
#endif
}
XR_FUNC XrOsIoStatus xr_os_io_loadavg(XrOsLoadAverages *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    XrOsLoadAverages result = {true, {0.0, 0.0, 0.0}};
#if defined(XR_OS_LINUX)
    struct sysinfo information;
    if (sysinfo(&information) != 0) return io_errno_status(errno);
    for (size_t i = 0; i < 3; ++i) result.values[i] = (double)information.loads[i] / 65536.0;
#elif defined(XR_OS_MACOS)
    if (getloadavg(result.values, 3) != 3) return XR_OS_IO_IO;
    for (size_t i = 0; i < 3; ++i)
        if (!(result.values[i] >= 0.0) || result.values[i] > 1.7976931348623157e308) return XR_OS_IO_IO;
#else
    result.available = false;
#endif
    *output = result;
    return XR_OS_IO_OK;
}
