/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_function_cases.h - Function result leases and revocable instance admission
 *
 * KEY CONCEPT:
 *   An escaped callable retains code while instance slots are physically released.
 */
#ifndef XIR_FUNCTION_CASES_H
#define XIR_FUNCTION_CASES_H
#include "xir/xxir_program.h"
#include <string.h>
typedef struct FunctionCaseFrame { uint32_t phase; XrXirValue owned; } FunctionCaseFrame;
static XrXirAction function_case_fault(XrXirCallStatus status) {
    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}};
}
static XrXirAction function_case_resume(XrXirCallView *view) {
    FunctionCaseFrame *frame = view->state;
    uint32_t entry = xr_xir_call_current_entry(view->activation);
    XrXirCallStatus status = XR_XIR_CALL_READY;
    if (entry == 0) {
        status = xr_xir_instance_literal(view, 0, &frame->owned);
        if (status == XR_XIR_CALL_READY) status = xr_xir_instance_slot_write(view, 0, &frame->owned, true);
    } else if (entry == 1) return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, {XR_XIR_I64, 0, 17}};
    else if (entry == 2) status = xr_xir_instance_function(view, (XrXirType) 256, 3, &frame->owned);
    else if (entry == 3 && !frame->phase++) {
        status = xr_xir_instance_slot_read(view, 0, &frame->owned);
        if (status == XR_XIR_CALL_READY) return (XrXirAction) {XR_XIR_ACTION_SUSPEND, 0, NULL, 0, {0}};
    } else if (entry == 4) {
        if (!frame->phase++) {
            uint32_t target = UINT32_MAX;
            status = xr_xir_instance_resolve_function(view, &view->arguments[0], &target);
            if (status == XR_XIR_CALL_READY) return (XrXirAction) {XR_XIR_ACTION_CALL, target, NULL, 0, {0}};
        } else return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, view->inbox.value};
    }
    if (status != XR_XIR_CALL_READY) return function_case_fault(status);
    return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, entry ? frame->owned : (XrXirValue) {0}};
}
static void function_case_cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason; xr_xir_value_drop(&((FunctionCaseFrame *) view->state)->owned);
}
static void function_case_release(void *owner) { ++*(unsigned *) owner; }
static XrXirStatus function_case_seal(unsigned *releases, XrXirProgram **program) {
    XrXirSourceModule module = {"root", 4, NULL, 0, 0};
    XrXirFunctionIdentity identities[] = {{0,0}, {0,1}, {0,1}, {0,0}, {0,1}};
    XrXirSlot slot = {0, XR_XIR_STRING, 1};
    XrXirLiteral literal = {"owned callback result", 21};
    XrXirDeclarations declarations = {&module, 1, identities, &slot, 1, &literal, 1, 0, 1};
    XrXirCallableSignature signature = {NULL, 0, XR_XIR_STRING, 0};
    XrXirCallableTypes types = {&signature, 1};
    XrXirType callback_type = (XrXirType) 256;
    XrXirCallEntry entries[5];
    for (unsigned i = 0; i < 5; ++i) entries[i] = (XrXirCallEntry) {XR_XIR_CALL_ABI_VERSION,
        i == 4 ? &callback_type : NULL, i == 4 ? 1u : 0u,
        i == 0 ? XR_XIR_UNIT : i == 1 ? XR_XIR_I64 : i == 2 ? callback_type : XR_XIR_STRING,
        sizeof(FunctionCaseFrame), function_case_resume, function_case_cleanup, NULL};
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        entries, 5, &declarations, {releases, function_case_release}, &types};
    return xr_xir_program_seal(&spec, 65536, program);
}
static bool function_case_run(bool cancel) {
    unsigned releases = 0;
    XrXirProgram *program = NULL;
    XrXirInstance *instance = NULL, *other = NULL;
    XrXirValue function = {0}, result = {0}, second = {0}, copy = {0};
    bool success = false;
    XrXirStatus sealed = function_case_seal(&releases, &program);
    if (sealed != XR_XIR_OK) { CHECK(sealed == XR_XIR_OUT_OF_MEMORY && !program && !releases); return false; }
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    if (status != XR_XIR_CALL_READY) goto failed;
    status = xr_xir_instance_new(program, &config, &other);
    if (status != XR_XIR_CALL_READY) goto failed;
    status = xr_xir_instance_start(instance, 2, NULL, 0);
    if (status != XR_XIR_CALL_READY) goto failed;
    status = xr_xir_instance_poll(instance).outcome.status;
    if (status != XR_XIR_CALL_RETURNED) goto failed;
    CHECK(xr_xir_instance_take_result(instance, &function) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_function_binding(&function) && xr_xir_function_binding(&function)->entry == 3);
    CHECK(xr_xir_instance_start(instance, 3, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_function(other, &function, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start(other, 4, &function, 1) == XR_XIR_CALL_BAD_ARGUMENT);
    status = xr_xir_instance_start_function(instance, &function, NULL, 0);
    if (status != XR_XIR_CALL_READY) goto failed;
    XrXirInstanceResult wait = xr_xir_instance_poll(instance);
    CHECK(wait.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_resume(instance, wait.epoch, wait.outcome.wake) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    status = xr_xir_instance_start(instance, 4, &function, 1);
    if (status != XR_XIR_CALL_READY) goto failed;
    wait = xr_xir_instance_poll(instance);
    if (wait.outcome.status != XR_XIR_CALL_SUSPENDED) { status = wait.outcome.status; goto failed; }
    if (cancel) {
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_CANCELLED);
    } else {
        CHECK(xr_xir_instance_resume(instance, wait.epoch, wait.outcome.wake) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance, &second) == XR_XIR_CALL_RETURNED);
    }
    CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start_function(instance, &function, NULL, 0) == XR_XIR_CALL_BAD_STATE);
    success = true;
    goto finish;
 failed:
    CHECK(status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT);
 finish:
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program);
    if (function.type) {
        CHECK(!releases && xr_xir_value_copy(&function, &copy) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_start_function(other, &function, NULL, 0) == XR_XIR_CALL_BAD_STATE);
    }
    CHECK(xr_xir_instance_free(other) == XR_XIR_CALL_READY);
    if (success) {
        const char *bytes; size_t length;
        CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 21 && !memcmp(bytes, "owned callback result", 21));
        if (!cancel) CHECK(xr_xir_string_view(&second, &bytes, &length) && length == 21);
    }
    xr_xir_value_drop(&result); xr_xir_value_drop(&second);
    xr_xir_value_drop(&function);
    if (copy.type) CHECK(!releases);
    xr_xir_value_drop(&copy); CHECK(releases == 1);
    return success;
}
#endif // XIR_FUNCTION_CASES_H
