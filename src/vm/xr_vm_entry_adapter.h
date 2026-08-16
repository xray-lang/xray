/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_entry_adapter.h - Frozen typed entry adapter bindings
 *
 * KEY CONCEPT:
 *   A dynamic resolution freezes one generated adapter contract after the
 *   entry cell has acquired its generation pin. Persistent facts come only
 *   from the verified TargetPlan; process-local native pointers are copied
 *   into this runtime-only binding and are never serialized or hashed.
 */

#ifndef XR_VM_ENTRY_ADAPTER_H
#define XR_VM_ENTRY_ADAPTER_H

#include "../plan/target/xr_target_plan.h"
#include "../runtime/xr_entry_cell.h"

typedef struct XrVmEntryAdapterI64 {
    XrEntryCellExpectation expectation;
    XrEntryNativeI64 native_entry;
    void *native_context;
    uint32_t executor_kind;
    uint32_t frozen;
} XrVmEntryAdapterI64;

XR_FUNC bool xr_typed_entry_adapter_i64_freeze(
    const XrEntryCellExpectation *expectation,
    const XrEntryCallToken *token, XrVmEntryAdapterI64 *adapter,
    char *diagnostic, size_t diagnostic_size);
XR_FUNC bool xr_typed_entry_adapter_i64_matches_target(
    const XrVmEntryAdapterI64 *adapter,
    const XrTargetEntryExpectationRecord *target_expectation);
XR_FUNC XrEntryNativeStatus xr_typed_entry_adapter_i64_invoke_native(
    const XrVmEntryAdapterI64 *adapter, const int64_t *arguments,
    uint32_t argument_count, int64_t *result);

#endif  // XR_VM_ENTRY_ADAPTER_H
