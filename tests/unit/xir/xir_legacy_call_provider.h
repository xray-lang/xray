/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_legacy_call_provider.h - Frozen obsolete provider declarations for rejection
 *
 * KEY CONCEPT:
 *   This isolated test TU must keep the old return ABI. Production consumers
 *   reject its entry before invoking either function; no adapter exists.
 */
#ifndef XIR_LEGACY_CALL_PROVIDER_H
#define XIR_LEGACY_CALL_PROVIDER_H
#include <stddef.h>
#include <stdint.h>
#define XR_XIR_VALUE_ABI_VERSION 15u
#define XR_XIR_CALL_ABI_VERSION 19u
typedef enum XrXirType { XR_XIR_UNIT, XR_XIR_BOOL, XR_XIR_I64, XR_XIR_STRING, XR_XIR_ATOMIC_I64,
    XR_XIR_I8, XR_XIR_I16, XR_XIR_I32, XR_XIR_U8, XR_XIR_U16, XR_XIR_U32, XR_XIR_U64, XR_XIR_F32, XR_XIR_F64,
    XR_XIR_ERROR, XR_XIR_PANIC_INFO } XrXirType;

typedef struct XrXirValue {
    uint32_t type, reserved;
    int64_t payload;
} XrXirValue;

typedef struct XrXirFaultDetail {
    uint32_t code, reserved;
    int64_t index, length;
} XrXirFaultDetail;

typedef enum XrXirCallStatus {
    XR_XIR_CALL_READY, XR_XIR_CALL_RETURNED, XR_XIR_CALL_THROWN,
    XR_XIR_CALL_SUSPENDED, XR_XIR_CALL_CANCELLED, XR_XIR_CALL_LIMIT,
    XR_XIR_CALL_OOM, XR_XIR_CALL_BAD_ARGUMENT, XR_XIR_CALL_BAD_ABI,
    XR_XIR_CALL_BAD_STATE, XR_XIR_CALL_BUSY, XR_XIR_CALL_DIVIDE_BY_ZERO,
    XR_XIR_CALL_CONSUMED, XR_XIR_CALL_OUTPUT_ERROR, XR_XIR_CALL_NUMERIC_RANGE,
    XR_XIR_CALL_BOUNDS, XR_XIR_CALL_MATCH_FAILURE, XR_XIR_CALL_DEFER_ASYNC,
    XR_XIR_CALL_CANCEL_REQUESTED
} XrXirCallStatus;

typedef enum XrXirActionKind {
    XR_XIR_ACTION_CALL = 1, XR_XIR_ACTION_RETURN, XR_XIR_ACTION_THROW,
    XR_XIR_ACTION_SUSPEND, XR_XIR_ACTION_CONTINUE, XR_XIR_ACTION_FAULT,
    XR_XIR_ACTION_OUTPUT, XR_XIR_ACTION_WRITE_STREAM, XR_XIR_ACTION_LEAVE,
    XR_XIR_ACTION_EXIT_DONE
} XrXirActionKind;

typedef struct XrXirAction {
    XrXirActionKind kind;
    uint32_t callee;
    const XrXirValue *arguments;
    uint32_t argument_count;
    XrXirValue value;
    XrXirFaultDetail fault;
    uint32_t flags;
} XrXirAction;

typedef struct XrXirCallView XrXirCallView;

typedef XrXirAction (*XrXirResumeEntry)(XrXirCallView *view);

typedef void (*XrXirReleaseEntry)(XrXirCallView *view, XrXirCallStatus reason);

typedef struct XrXirCallEntry {
    uint32_t abi_version;
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result;
    uint32_t state_bytes;
    XrXirResumeEntry resume;
    XrXirReleaseEntry release;
    const void *environment;
    uint32_t flags, cleanup_owner;
} XrXirCallEntry;
_Static_assert(sizeof(XrXirValue) == 16 && sizeof(XrXirFaultDetail) == 24, "old value layout");
_Static_assert(sizeof(XrXirAction) == 72 && offsetof(XrXirAction, fault) == 40 && offsetof(XrXirAction, flags) == 64, "old return ABI");
_Static_assert(sizeof(XrXirCallEntry) == 64 && offsetof(XrXirCallEntry, resume) == 32, "old entry metadata");
#endif // XIR_LEGACY_CALL_PROVIDER_H
