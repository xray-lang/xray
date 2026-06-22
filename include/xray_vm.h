/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_vm.h - Bytecode VM runtime public API
 */

#ifndef XRAY_VM_H
#define XRAY_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "xray_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XrVMRuntime XrVMRuntime;

typedef enum {
    XR_VM_BACKEND_BYTECODE,
} XrVMBackendType;

typedef struct {
    XrVMBackendType backend_type;

    size_t initial_heap_size;
    size_t max_heap_size;

    bool trace_execution;
    bool dump_bytecode;
    bool dump_ic_feedback;

    void *userdata;

    const char *script_file;
    int script_argc;
    char **script_argv;
} XrVMConfig;

XRAY_API void xray_vm_config_init(XrVMConfig *config);

XRAY_API XrVMRuntime *xray_vm_new(const XrVMConfig *config);
XRAY_API XrVMRuntime *xray_vm_new_runtime(const XrVMConfig *config);
XRAY_API XrVMRuntime *xray_vm_new_full(const XrVMConfig *config);
XRAY_API void xray_vm_delete(XrVMRuntime *vm);

XRAY_API int xray_vm_dostring(XrVMRuntime *vm, const char *source);
XRAY_API int xray_vm_dofile(XrVMRuntime *vm, const char *filename);
XRAY_API int xray_vm_dofile_debug(XrVMRuntime *vm, const char *filename, void **out_proto);

XRAY_API XrVMBackendType xray_vm_get_backend(XrVMRuntime *vm);

XRAY_API void xray_vm_set_userdata(XrVMRuntime *vm, void *userdata);
XRAY_API void *xray_vm_get_userdata(XrVMRuntime *vm);

XRAY_API void xray_vm_get_stats(XrVMRuntime *vm, size_t *bytes_allocated, int *cycle_count);
XRAY_API void xray_vm_collect_cycles(XrVMRuntime *vm);

XRAY_API void xray_vm_set_trace(XrVMRuntime *vm, bool enable);
XRAY_API void xray_vm_set_dump_bytecode(XrVMRuntime *vm, bool enable);

XRAY_API void xray_vm_set_script_info(XrVMRuntime *vm, const char *script_file, int argc,
                                      char **argv);

XRAY_API void xray_vm_set_stdout(XrVMRuntime *vm, FILE *stream);
XRAY_API void xray_vm_set_deadline_ms(XrVMRuntime *vm, int64_t timeout_ms);
XRAY_API bool xray_vm_timed_out(XrVMRuntime *vm);

XRAY_API void xray_vm_set_module_allowlist(XrVMRuntime *vm, const char *const *allowed,
                                           size_t count);

XRAY_API void xray_vm_multicore_init(XrVMRuntime *vm, int num_workers);
XRAY_API void xray_vm_multicore_destroy(XrVMRuntime *vm);
XRAY_API void xray_vm_coro_monitor_start(XrVMRuntime *vm, int watch_interval_ms, int http_port);

#ifdef __cplusplus
}
#endif

#endif  // XRAY_VM_H
