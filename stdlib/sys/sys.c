/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * sys.c - Private ABI leaves for sys.xr
 */

#include "../common.h"
#include "../../src/coro/xsys_provider.h"
#include "../../src/vm/xvm.h"

static XrValue sys_cpu_count(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_cpu_count(isolate, args, argc);
}

static XrValue sys_thread_yield(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_thread_yield(isolate, args, argc);
}

static XrValue sys_sleep_ms(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_sleep_ms(isolate, args, argc);
}

static XrValue sys_pin_to_cpu(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_pin_to_cpu(isolate, args, argc);
}

static XrValue sys_thread_local_id(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_thread_local_id(isolate, args, argc);
}

static XrValue sys_thread_local_alive(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_thread_local_alive(isolate, args, argc);
}

static XrValue sys_on_signal(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_on_signal(isolate, args, argc);
}

static XrValue sys_dylib_open(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_dylib_open(isolate, args, argc);
}

static XrValue sys_dylib_symbol(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_dylib_symbol(isolate, args, argc);
}

static XrValue sys_dylib_close(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_dylib_close(isolate, args, argc);
}

static XrValue sys_dylib_last_error(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_dylib_last_error(isolate, args, argc);
}

static XrValue sys_process_spawn(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_process_spawn(isolate, args, argc);
}

static XrValue sys_process_try_wait(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_process_try_wait(isolate, args, argc);
}

static XrValue sys_process_kill(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_process_kill(isolate, args, argc);
}

static XrValue sys_pipe_open(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_pipe_open(isolate, args, argc);
}

static XrCFuncResult sys_pipe_read_yieldable(XrVMRuntime *isolate, XrValue *args, int argc,
                                             XrValue *result) {
    return xr_sys_provider_pipe_read(isolate, args, argc, result);
}

static XrCFuncResult sys_pipe_write_yieldable(XrVMRuntime *isolate, XrValue *args, int argc,
                                              XrValue *result) {
    return xr_sys_provider_pipe_write(isolate, args, argc, result);
}

static XrValue sys_pipe_close(XrVMRuntime *isolate, XrValue *args, int argc) {
    return xr_sys_provider_pipe_close(isolate, args, argc);
}

#define XR_STDLIB_VM_BIND_MODULE_SYS 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_SYS
