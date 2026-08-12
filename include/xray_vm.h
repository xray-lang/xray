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
#include "xray_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XrVMRuntime XrVMRuntime;

typedef struct XrBytecodeModule {
    const char *path;
    /* NULL with bytecode_size zero denotes an external module occupying its
     * compile-time topological slot. */
    const uint8_t *bytecode;
    size_t bytecode_size;
} XrBytecodeModule;

typedef struct XrBytecodeBundle {
    /* Complete compile-time topological order, including external slots. */
    const XrBytecodeModule *modules;
    size_t module_count;
    size_t entry_index;
} XrBytecodeBundle;

typedef struct {
    size_t initial_heap_size;
    size_t max_heap_size;

    bool trace_execution;
    bool dump_bytecode;
    bool dump_ic_feedback;

    int scheduler_workers;  // 0=auto; consumed only when EntryPlan reaches a scheduler

    void *userdata;

    const char *script_file;
    int script_argc;
    char **script_argv;
} XrVMConfig;

XRAY_API void xray_vm_config_init(XrVMConfig *config);

XRAY_API XrVMRuntime *xray_vm_new(const XrVMConfig *config);
XRAY_API XrVMRuntime *xray_vm_new_full(const XrVMConfig *config);
XRAY_API void xray_vm_delete(XrVMRuntime *vm);

XRAY_API int xray_vm_eval_bundle(XrVMRuntime *vm, const XrBytecodeBundle *bundle);

XRAY_API void xray_vm_set_script_info(XrVMRuntime *vm, const char *script_file, int argc,
                                      char **argv);

XRAY_API void xray_vm_multicore_init(XrVMRuntime *vm, int num_workers);
XRAY_API void xray_vm_multicore_destroy(XrVMRuntime *vm);
#ifdef __cplusplus
}
#endif

#endif  // XRAY_VM_H
