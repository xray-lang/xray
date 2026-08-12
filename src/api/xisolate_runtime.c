/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_runtime.c - Runtime prelude enum registration
 *
 * Defines the shared VM prelude enum registry consumed by full isolate
 * construction without depending on the compiler frontend's source registry.
 */

#include "../runtime/xisolate_internal.h"
#include "../runtime/class/xenum.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/xisolate_api.h"
#include "../base/xglobal_indices.h"

static void isolate_bind_builtin(XrVMRuntime *isolate, int32_t index, XrValue value) {
    if (!isolate || index < 0 || index >= XR_USER_GLOBALS_START)
        return;
    isolate->vm.builtins[index] = value;
    xr_runtime_core_set_builtin(xr_isolate_get_runtime_core(isolate), index, value);
}

static XrEnumType *runtime_register_prelude_enum(XrVMRuntime *isolate, const char *name,
                                                 char **members, int member_count,
                                                 const int *payload_counts) {
    if (!isolate || !name || !members || member_count <= 0)
        return NULL;

    XrEnumType *type = xr_enum_type_new(isolate, "prelude", name, members, member_count);
    if (!type || !payload_counts)
        return type;

    (void) xr_enum_type_set_adt_payloads(type, payload_counts, member_count);
    return type;
}

void xr_isolate_register_runtime_prelude_enums(XrVMRuntime *isolate) {
    if (!isolate)
        return;

    char *ordering_members[] = {"Relaxed", "Acquire", "Release", "AcquireRelease", "SeqCst"};
    char *endian_members[] = {"Native", "LE", "BE"};
    char *recv_members[] = {"Value", "Empty", "Timeout", "Closed"};
    char *send_result_members[] = {"Sent", "Full", "Timeout", "Closed"};
    char *task_result_members[] = {"Success", "Failed", "Cancelled", "Timeout", "Pending"};
    char *task_status_members[] = {"Pending", "Running", "Success", "Failed", "Cancelled"};
    char *utf8_error_members[] = {"InvalidUtf8"};
    char *string_slice_error_members[] = {"InvalidByteRange"};
    char *compression_error_members[] = {"InvalidData"};
    char *crypto_error_members[] = {"InvalidLength"};
    const int recv_payloads[] = {1, 0, 0, 0};
    const int task_result_payloads[] = {1, 1, 0, 0, 0};

    XrEnumType *ordering =
        runtime_register_prelude_enum(isolate, "Ordering", ordering_members, 5, NULL);
    XrEnumType *endian = runtime_register_prelude_enum(isolate, "Endian", endian_members, 3, NULL);
    XrEnumType *recv =
        runtime_register_prelude_enum(isolate, "Recv", recv_members, 4, recv_payloads);
    XrEnumType *send_result =
        runtime_register_prelude_enum(isolate, "SendResult", send_result_members, 4, NULL);
    XrEnumType *task_result = runtime_register_prelude_enum(
        isolate, "TaskResult", task_result_members, 5, task_result_payloads);
    XrEnumType *task_status =
        runtime_register_prelude_enum(isolate, "TaskStatus", task_status_members, 5, NULL);
    XrEnumType *utf8_error =
        runtime_register_prelude_enum(isolate, "Utf8Error", utf8_error_members, 1, NULL);
    XrEnumType *string_slice_error = runtime_register_prelude_enum(
        isolate, "StringSliceError", string_slice_error_members, 1, NULL);
    XrEnumType *compression_error = runtime_register_prelude_enum(
        isolate, "CompressionError", compression_error_members, 1, NULL);
    XrEnumType *crypto_error =
        runtime_register_prelude_enum(isolate, "CryptoError", crypto_error_members, 1, NULL);

    if (ordering)
        isolate_bind_builtin(isolate, XR_GLOBAL_VAR_ORDERING, XR_FROM_PTR(ordering));
    if (endian)
        isolate_bind_builtin(isolate, XR_GLOBAL_VAR_ENDIAN, XR_FROM_PTR(endian));
    if (recv)
        isolate_bind_builtin(isolate, XR_GLOBAL_VAR_RECV, XR_FROM_PTR(recv));
    if (send_result)
        isolate_bind_builtin(isolate, XR_GLOBAL_VAR_SEND_RESULT, XR_FROM_PTR(send_result));
    if (task_result)
        isolate_bind_builtin(isolate, XR_GLOBAL_VAR_TASK_RESULT, XR_FROM_PTR(task_result));
    if (task_status)
        isolate_bind_builtin(isolate, XR_GLOBAL_VAR_TASK_STATUS, XR_FROM_PTR(task_status));
    if (utf8_error)
        isolate_bind_builtin(isolate, XR_GLOBAL_VAR_UTF8_ERROR, XR_FROM_PTR(utf8_error));
    if (string_slice_error)
        isolate_bind_builtin(isolate, XR_GLOBAL_VAR_STRING_SLICE_ERROR,
                             XR_FROM_PTR(string_slice_error));
    if (compression_error)
        isolate_bind_builtin(isolate, XR_GLOBAL_VAR_COMPRESSION_ERROR,
                             XR_FROM_PTR(compression_error));
    if (crypto_error)
        isolate_bind_builtin(isolate, XR_GLOBAL_VAR_CRYPTO_ERROR, XR_FROM_PTR(crypto_error));
}
