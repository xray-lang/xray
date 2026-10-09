/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_metadata_probe.h - Runtime metadata allocation qualification
 *
 * KEY CONCEPT:
 *   Real VM initialization forwards metadata operations and observes their owned allocations.
 */
#ifndef XR_VM_METADATA_PROBE_H
#define XR_VM_METADATA_PROBE_H
#include "base/xmalloc.h"
#include "runtime/symbol/xsymbol_table.h"
#include "runtime/class/xtype_registry.h"
#include "runtime/class/xclass_system.h"
XR_FUNC void *xr_vm_metadata_test_malloc(size_t bytes, const char *source);
XR_FUNC void *xr_vm_metadata_test_calloc(size_t count, size_t bytes);
XR_FUNC void *xr_vm_metadata_test_realloc(void *memory, size_t bytes);
XR_FUNC void xr_vm_metadata_test_free(void *memory);
XR_FUNC XrSymbolTable *xr_vm_metadata_test_symbol_create(void);
XR_FUNC bool xr_vm_metadata_test_symbol_builtins(XrSymbolTable *table);
XR_FUNC void xr_vm_metadata_test_registry_init(XrVMRuntime *runtime);
XR_FUNC void xr_vm_metadata_test_core_init(XrVMRuntime *runtime);
#ifdef XR_VM_METADATA_ALLOCATION_PROBE_OBJECT
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) xr_vm_metadata_test_malloc((bytes), __FILE__)
#define xr_calloc(count, bytes) xr_vm_metadata_test_calloc((count), (bytes))
#define xr_realloc(memory, bytes) xr_vm_metadata_test_realloc((memory), (bytes))
#define xr_free(memory) xr_vm_metadata_test_free(memory)
#endif
#ifdef XR_VM_METADATA_CALL_PROBE_OBJECT
#define xr_symbol_table_create() xr_vm_metadata_test_symbol_create()
#define xr_symbol_table_init_builtins(table) xr_vm_metadata_test_symbol_builtins(table)
#define xr_registry_init(runtime) xr_vm_metadata_test_registry_init(runtime)
#define xr_core_init(runtime) xr_vm_metadata_test_core_init(runtime)
#endif
#endif // XR_VM_METADATA_PROBE_H
