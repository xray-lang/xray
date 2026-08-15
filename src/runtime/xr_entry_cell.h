/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_entry_cell.h - Runtime-owned typed entry indirection
 *
 * KEY CONCEPT:
 *   An entry cell is the only runtime authority that turns a resolved entry
 *   into executable code. Its ABI identity describes the callee signature,
 *   never a caller site. A binding holds a static-root generation pin while
 *   published, and every acquired call token holds one additional in-flight
 *   pin until an exactly-once release.
 *
 * ARTIFACT BOUNDARY:
 *   Native entry pointers enter only through the runtime registration value
 *   below. They are process-local data, are never hashed, and have no artifact
 *   representation. The stable native identity, verified plan identity, and
 *   generation identity are the persistent facts used to bind such a pointer.
 */

#ifndef XR_ENTRY_CELL_H
#define XR_ENTRY_CELL_H

#include "../../include/xray_runtime_generation.h"
#include "../base/xstable_id.h"
#include "../os/os_thread.h"
#include <stdatomic.h>

#define XR_ENTRY_ABI_SCHEMA_VERSION UINT32_C(1)

typedef enum XrEntryExecutorKind {
    XR_ENTRY_EXECUTOR_INVALID = 0,
    XR_ENTRY_EXECUTOR_TYPED_VM,
    XR_ENTRY_EXECUTOR_NATIVE_I64,
    XR_ENTRY_EXECUTOR_KIND_COUNT,
} XrEntryExecutorKind;

typedef enum XrEntryAdapterKind {
    XR_ENTRY_ADAPTER_INVALID = 0,
    XR_ENTRY_ADAPTER_IDENTITY,
    XR_ENTRY_ADAPTER_KIND_COUNT,
} XrEntryAdapterKind;

typedef enum XrEntryNativeStatus {
    XR_ENTRY_NATIVE_OK = 0,
    XR_ENTRY_NATIVE_ERROR,
    XR_ENTRY_NATIVE_CANCELLED,
    XR_ENTRY_NATIVE_STATUS_COUNT,
} XrEntryNativeStatus;

typedef XrEntryNativeStatus (*XrEntryNativeI64)(
    void *context, const int64_t *arguments, uint32_t argument_count,
    int64_t *result);

/* The first entry ABI is deliberately exact: signed i64 parameters and one
 * signed i64 result under one frozen target profile. Other value families need
 * a new governed schema rather than widening this record by convention. */
typedef struct XrEntryAbi {
    uint32_t schema_version;
    uint16_t parameter_count;
    uint8_t value_kind;
    uint8_t reserved8;
    uint16_t native_abi;
    uint16_t reserved16;
    uint64_t target_data_layout;
    XrFingerprint target_profile_fingerprint;
    XrFingerprint fingerprint;
} XrEntryAbi;

typedef struct XrEntryCellExpectation {
    XrEntryAbi abi;
    XrFingerprint adapter_fingerprint;
    XrFingerprint target_plan_fingerprint;
    XrFingerprint generation_fingerprint;
    XrFingerprint binding_fingerprint;
    uint32_t executor_kind;
    uint32_t adapter_kind;
} XrEntryCellExpectation;

typedef struct XrEntryCellRegistration {
    XrLoadedModuleGeneration *generation;
    const XrTargetPlan *verified_plan;
    uint32_t function;
    uint32_t executor_kind;
    XrStableId native_entry_identity;
    XrEntryNativeI64 native_entry;
    void *native_context;
} XrEntryCellRegistration;

typedef struct XrEntryCellBinding {
    XrLoadedModuleGeneration *generation;
    const XrTargetPlan *plan;
    XrEntryCellExpectation expectation;
    XrStableId native_entry_identity;
    XrEntryNativeI64 native_entry;
    void *native_context;
    uint32_t function;
    uint32_t reserved;
} XrEntryCellBinding;

typedef struct XrEntryCell {
    xr_mutex_t gate;
    XrEntryCellBinding binding;
    bool initialized;
} XrEntryCell;

typedef struct XrEntryCallToken {
    XrLoadedModuleGeneration *generation;
    const XrTargetPlan *plan;
    XrEntryNativeI64 native_entry;
    void *native_context;
    XrFingerprint plan_fingerprint;
    uint32_t function;
    uint32_t executor_kind;
    atomic_uint release_state;
} XrEntryCallToken;

typedef enum XrEntryInvokeStatus {
    XR_ENTRY_INVOKE_OK = 0,
    XR_ENTRY_INVOKE_INVALID_ARGUMENT,
    XR_ENTRY_INVOKE_AUTHORITY_ERROR,
    XR_ENTRY_INVOKE_VM_ERROR,
    XR_ENTRY_INVOKE_NATIVE_ERROR,
    XR_ENTRY_INVOKE_CANCELLED,
    XR_ENTRY_INVOKE_RELEASE_ERROR,
    XR_ENTRY_INVOKE_STATUS_COUNT,
} XrEntryInvokeStatus;

XR_FUNC bool xr_entry_cell_init(XrEntryCell *cell);
XR_FUNC bool xr_entry_cell_dispose(XrEntryCell *cell, char *diagnostic,
                                   size_t diagnostic_size);
XR_FUNC bool xr_entry_cell_bind(
    XrEntryCell *cell, const XrEntryCellRegistration *registration,
    XrEntryCellExpectation *expectation, char *diagnostic,
    size_t diagnostic_size);
XR_FUNC bool xr_entry_cell_clear(XrEntryCell *cell, char *diagnostic,
                                 size_t diagnostic_size);
XR_FUNC bool xr_entry_cell_acquire(
    XrEntryCell *cell, const XrEntryCellExpectation *expectation,
    XrEntryCallToken *token, char *diagnostic, size_t diagnostic_size);
XR_FUNC bool xr_entry_call_release(XrEntryCallToken *token, char *diagnostic,
                                   size_t diagnostic_size);
XR_FUNC XrEntryInvokeStatus xr_entry_cell_invoke_i64(
    XrEntryCell *cell, const XrEntryCellExpectation *expectation,
    const int64_t *arguments, uint32_t argument_count, int64_t *result,
    uint32_t *executor_status, char *diagnostic, size_t diagnostic_size);

#endif  // XR_ENTRY_CELL_H
