/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_stdlib_provider_bindings_gen.inc.c - Typed host operation bindings
 *
 * Generated from explicit provider declarations. Do not edit.
 */

#include "execution/xr_byte_storage_provider.h"
#include "execution/xr_random_provider.h"
#include "os/os_pipe.h"
#include "os/os_time.h"
#include "shared/xr_os_core.h"
#include "shared/xr_time_offset.h"

/* clang-format off */

_Static_assert(_Generic(&xr_random_provider_fill,
    XrProviderCallStatus (*)(void *, const XrProviderValuePack *, XrProviderValuePack *): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_0(
    void *context, const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    (void)context;
    return xr_random_provider_fill(context, arguments, result);
}

_Static_assert(_Generic(&xr_time_monotonic_ns,
    uint64_t (*)(void): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_1(
    void *context, int64_t *result_out) {
    (void)context;
    if (!result_out) return XR_PROVIDER_CALL_FAILED;
    uint64_t raw = xr_time_monotonic_ns();
    *result_out = raw <= INT64_MAX ? (int64_t)raw :
                  -INT64_C(1) - (int64_t)(UINT64_MAX - raw);
    return XR_PROVIDER_CALL_OK;
}

_Static_assert(_Generic(&xr_time_realtime_ns,
    uint64_t (*)(void): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_2(
    void *context, int64_t *result_out) {
    (void)context;
    if (!result_out) return XR_PROVIDER_CALL_FAILED;
    uint64_t raw = xr_time_realtime_ns();
    *result_out = raw <= INT64_MAX ? (int64_t)raw :
                  -INT64_C(1) - (int64_t)(UINT64_MAX - raw);
    return XR_PROVIDER_CALL_OK;
}

_Static_assert(_Generic(&xr_time_utc_offset_at,
    bool (*)(int64_t, int64_t *): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_3(
    void *context, int64_t argument, int64_t *result_out) {
    (void)context;
    return xr_time_utc_offset_at(argument, result_out) ?
           XR_PROVIDER_CALL_OK : XR_PROVIDER_CALL_FAILED;
}

_Static_assert(_Generic(&xr_time_process_cpu_ns,
    uint64_t (*)(void): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_4(
    void *context, int64_t *result_out) {
    (void)context;
    if (!result_out) return XR_PROVIDER_CALL_FAILED;
    uint64_t raw = xr_time_process_cpu_ns();
    *result_out = raw <= INT64_MAX ? (int64_t)raw :
                  -INT64_C(1) - (int64_t)(UINT64_MAX - raw);
    return XR_PROVIDER_CALL_OK;
}

_Static_assert(_Generic(&xr_os_core_getpid,
    int64_t (*)(void): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_5(
    void *context, int64_t *result_out) {
    (void)context;
    if (!result_out) return XR_PROVIDER_CALL_FAILED;
    *result_out = xr_os_core_getpid();
    return XR_PROVIDER_CALL_OK;
}

_Static_assert(_Generic(&xr_pipe_close,
    int (*)(XrPipeHandle): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_6(
    void *context, int64_t argument, bool *result_out) {
    (void)context;
    if (!result_out) return XR_PROVIDER_CALL_FAILED;
    XrPipeHandle handle = (XrPipeHandle)argument;
    if ((int64_t)handle != argument) {
        *result_out = false;
        return XR_PROVIDER_CALL_OK;
    }
    *result_out = xr_pipe_close(handle) == 0;
    return XR_PROVIDER_CALL_OK;
}

_Static_assert(_Generic(&xr_pipe_create,
    int (*)(XrPipe *, const XrPipeOptions *): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_7(
    void *context, bool *present_out, int64_t *first_out, int64_t *second_out) {
    (void)context;
    if (!present_out || !first_out || !second_out)
        return XR_PROVIDER_CALL_FAILED;
    XrPipe pipe = {XR_PIPE_INVALID, XR_PIPE_INVALID};
    bool present = xr_pipe_create(&pipe, NULL) == 0;
    *present_out = present;
    *first_out = present ? (int64_t)pipe.read : 0;
    *second_out = present ? (int64_t)pipe.write : 0;
    return XR_PROVIDER_CALL_OK;
}

_Static_assert(_Generic(&xr_byte_storage_allocate,
    XrProviderCallStatus (*)(void *, const XrProviderValuePack *, XrProviderValuePack *): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_8(
    void *context, const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    (void)context;
    return xr_byte_storage_allocate(context, arguments, result);
}

_Static_assert(_Generic(&xr_byte_storage_allocate_zeroed,
    XrProviderCallStatus (*)(void *, const XrProviderValuePack *, XrProviderValuePack *): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_9(
    void *context, const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    (void)context;
    return xr_byte_storage_allocate_zeroed(context, arguments, result);
}

_Static_assert(_Generic(&xr_byte_storage_length,
    XrProviderCallStatus (*)(void *, const XrProviderValuePack *, XrProviderValuePack *): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_10(
    void *context, const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    (void)context;
    return xr_byte_storage_length(context, arguments, result);
}

_Static_assert(_Generic(&xr_byte_storage_allocate_aligned,
    XrProviderCallStatus (*)(void *, const XrProviderValuePack *, XrProviderValuePack *): 1, default: 0),
    "Provider host signature does not match its explicit adapter");
static XrProviderCallStatus xr_stdlib_provider_call_11(
    void *context, const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    (void)context;
    return xr_byte_storage_allocate_aligned(context, arguments, result);
}

static const XrProviderOperationBinding xr_stdlib_provider_bindings[] = {
    {
        .operation_id = { { 0xb9, 0x12, 0xce, 0x85, 0x4b, 0xac, 0x31, 0x0d, 0x1c, 0x15, 0x1a, 0x36, 0x61, 0xf6, 0xc7, 0xdb } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_TYPED,
        .entry.typed = xr_stdlib_provider_call_0,
    },
    {
        .operation_id = { { 0x48, 0x83, 0x2a, 0x90, 0x67, 0x12, 0x89, 0x24, 0x47, 0xd7, 0x3f, 0xa2, 0x84, 0xa0, 0x74, 0x0c } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY,
        .entry.i64_nullary = xr_stdlib_provider_call_1,
    },
    {
        .operation_id = { { 0xa8, 0x46, 0xda, 0xbb, 0x8b, 0x04, 0x1d, 0x87, 0x50, 0x24, 0xea, 0x11, 0x4e, 0xce, 0xec, 0x5b } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY,
        .entry.i64_nullary = xr_stdlib_provider_call_2,
    },
    {
        .operation_id = { { 0xa9, 0x64, 0xbc, 0x65, 0x4c, 0x1c, 0xfa, 0xec, 0xdd, 0x3c, 0xf2, 0x76, 0xaf, 0xae, 0xb3, 0x9a } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_UNARY,
        .entry.i64_unary = xr_stdlib_provider_call_3,
    },
    {
        .operation_id = { { 0xfb, 0x7d, 0x08, 0x64, 0x85, 0xbe, 0x51, 0x21, 0xd5, 0xf2, 0xe4, 0x23, 0xd8, 0xfb, 0xcf, 0xd9 } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY,
        .entry.i64_nullary = xr_stdlib_provider_call_4,
    },
    {
        .operation_id = { { 0x01, 0x2b, 0x0f, 0x73, 0x60, 0xee, 0xe2, 0x31, 0xd5, 0x81, 0x34, 0x46, 0x89, 0xc6, 0x20, 0x9f } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY,
        .entry.i64_nullary = xr_stdlib_provider_call_5,
    },
    {
        .operation_id = { { 0x8e, 0x76, 0x5f, 0x2e, 0xe5, 0x84, 0x9b, 0xc3, 0x9d, 0xfc, 0xf0, 0xd3, 0xb1, 0x2b, 0x5e, 0x0d } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY,
        .entry.bool_i64_unary = xr_stdlib_provider_call_6,
    },
    {
        .operation_id = { { 0xc1, 0x87, 0xd1, 0x40, 0x01, 0x17, 0x9d, 0xc6, 0x1f, 0x90, 0xde, 0x19, 0xe1, 0x20, 0x4b, 0x28 } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_OPTIONAL_I64_PAIR_NULLARY,
        .entry.optional_i64_pair_nullary = xr_stdlib_provider_call_7,
    },
    {
        .operation_id = { { 0x52, 0x80, 0xb2, 0xe3, 0xe8, 0xc2, 0xb1, 0x0d, 0x17, 0x92, 0x5f, 0x92, 0x10, 0x58, 0xb2, 0xf4 } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_TYPED,
        .entry.typed = xr_stdlib_provider_call_8,
    },
    {
        .operation_id = { { 0x75, 0xea, 0x81, 0x97, 0xf7, 0x81, 0x6e, 0x73, 0xff, 0x59, 0xc0, 0x80, 0xad, 0xbf, 0x46, 0x49 } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_TYPED,
        .entry.typed = xr_stdlib_provider_call_9,
    },
    {
        .operation_id = { { 0x79, 0xff, 0x36, 0x06, 0x80, 0xc2, 0xb1, 0x0f, 0x0a, 0xba, 0x91, 0x62, 0x44, 0xa5, 0xdb, 0x54 } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_TYPED,
        .entry.typed = xr_stdlib_provider_call_10,
    },
    {
        .operation_id = { { 0x99, 0x72, 0x62, 0x2e, 0xff, 0xa8, 0xe0, 0xdd, 0x4f, 0x95, 0xbe, 0xf9, 0x76, 0x64, 0x41, 0x05 } },
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_TYPED,
        .entry.typed = xr_stdlib_provider_call_11,
    },
};
#define XR_STDLIB_PROVIDER_BINDING_COUNT 12u

/* clang-format on */
