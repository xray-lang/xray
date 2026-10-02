/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xio_policy.c - System and shared compiler-ledger I/O allocation policies
 */
#include "xio_policy.h"
#include "xcompile_resources.h"
#include "xmalloc.h"

static XrOsIoStatus system_allocate(void *context, size_t bytes, void **output) {
    if (!context || !output) return XR_OS_IO_BAD_ARGUMENT;
    void *memory = xr_malloc(bytes ? bytes : 1);
    if (!memory) return XR_OS_IO_OUT_OF_MEMORY;
    *output = memory;
    return XR_OS_IO_OK;
}
static void system_free(void *context, void *memory) {
    (void)context;
    xr_free(memory);
}
static XrOsIoStatus system_work(void *context, uint64_t units) {
    (void)units;
    return context ? XR_OS_IO_OK : XR_OS_IO_BAD_ARGUMENT;
}
XR_FUNC XrOsIoPolicy xr_os_io_system_policy(void) {
    static unsigned char context;
    return (XrOsIoPolicy){&context, system_allocate, system_free, system_work};
}

static XrOsIoStatus compile_status(XrCompileResourceStatus status) {
    switch (status) {
    case XR_COMPILE_RESOURCE_OK: return XR_OS_IO_OK;
    case XR_COMPILE_RESOURCE_BUDGET: return XR_OS_IO_BUDGET;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_OS_IO_OUT_OF_MEMORY;
    case XR_COMPILE_RESOURCE_BAD_ARGUMENT: return XR_OS_IO_BAD_ARGUMENT;
    }
    return XR_OS_IO_BAD_ARGUMENT;
}
static XrOsIoStatus compile_allocate(void *context, size_t bytes, void **output) {
    return compile_status(xr_compile_resources_alloc(context, bytes, output));
}
static void compile_free(void *context, void *memory) {
    (void)context;
    xr_compile_resources_free(memory);
}
static XrOsIoStatus compile_work(void *context, uint64_t units) {
    return compile_status(xr_compile_resources_work(context, units));
}
XR_FUNC XrOsIoPolicy xr_compile_io_policy(XrCompileResources *resources) {
    return (XrOsIoPolicy){resources, compile_allocate, compile_free, compile_work};
}
