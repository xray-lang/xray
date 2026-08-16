/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_entry_adapter.c - Generated typed entry adapter execution
 */

#include "xr_vm_entry_adapter.h"
#include "../plan/target/xr_target_entry_abi.h"
#include <stdio.h>
#include <string.h>

typedef struct XrVmEntryAdapterContract {
    uint32_t target_kind;
    uint32_t runtime_kind;
    uint32_t value_kind;
} XrVmEntryAdapterContract;

static const XrVmEntryAdapterContract generated_adapter_contracts[] = {
#define XR_VM_ENTRY_ADAPTER(symbol, target_kind, runtime_kind, value_kind)       \
    {target_kind, runtime_kind, value_kind},
#include "../../xisa/target/vm_entry_adapters.def"
#undef XR_VM_ENTRY_ADAPTER
};

static bool fail(char *diagnostic, size_t diagnostic_size,
                 const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "XR_EXEC_5008: %s", detail);
    return false;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static const XrVmEntryAdapterContract *generated_contract(
    uint32_t target_kind, uint32_t runtime_kind, uint32_t value_kind) {
    const XrVmEntryAdapterContract *matched = NULL;
    for (size_t i = 0;
         i < sizeof(generated_adapter_contracts) /
                 sizeof(generated_adapter_contracts[0]);
         i++) {
        const XrVmEntryAdapterContract *candidate =
            &generated_adapter_contracts[i];
        if (candidate->target_kind != target_kind ||
            candidate->runtime_kind != runtime_kind ||
            candidate->value_kind != value_kind)
            continue;
        if (matched)
            return NULL;
        matched = candidate;
    }
    return matched;
}

static bool expectation_is_exact(
    const XrEntryCellExpectation *expectation) {
    if (!expectation ||
        expectation->abi.schema_version != XR_ENTRY_ABI_SCHEMA_VERSION ||
        expectation->abi.reserved8 != 0 || expectation->abi.reserved16 != 0 ||
        expectation->executor_kind <= XR_ENTRY_EXECUTOR_INVALID ||
        expectation->executor_kind >= XR_ENTRY_EXECUTOR_KIND_COUNT ||
        !generated_contract(XR_TARGET_ENTRY_ADAPTER_IDENTITY,
                            expectation->adapter_kind,
                            expectation->abi.value_kind) ||
        bytes_are_zero(expectation->target_plan_fingerprint.bytes,
                       sizeof(expectation->target_plan_fingerprint.bytes)) ||
        bytes_are_zero(expectation->generation_fingerprint.bytes,
                       sizeof(expectation->generation_fingerprint.bytes)) ||
        bytes_are_zero(expectation->binding_fingerprint.bytes,
                       sizeof(expectation->binding_fingerprint.bytes)))
        return false;
    XrTargetEntryAbiFacts facts = {
        .schema_version = expectation->abi.schema_version,
        .parameter_count = expectation->abi.parameter_count,
        .native_abi = expectation->abi.native_abi,
        .value_kind = expectation->abi.value_kind,
        .target_data_layout = expectation->abi.target_data_layout,
        .target_profile_fingerprint =
            expectation->abi.target_profile_fingerprint,
    };
    XrFingerprint abi = {{0}};
    XrFingerprint adapter = {{0}};
    return xr_target_entry_abi_fingerprint(&facts, &abi) &&
           xr_target_entry_identity_adapter_fingerprint(&abi, &adapter) &&
           xr_fingerprint_equal(abi, expectation->abi.fingerprint) &&
           xr_fingerprint_equal(adapter,
                                expectation->adapter_fingerprint);
}

bool xr_typed_entry_adapter_i64_freeze(
    const XrEntryCellExpectation *expectation,
    const XrEntryCallToken *token, XrVmEntryAdapterI64 *adapter,
    char *diagnostic, size_t diagnostic_size) {
    if (adapter)
        memset(adapter, 0, sizeof(*adapter));
    if (!expectation || !token || !adapter ||
        !expectation_is_exact(expectation) || !token->generation ||
        !token->plan || token->executor_kind != expectation->executor_kind ||
        !xr_fingerprint_equal(token->plan_fingerprint,
                              expectation->target_plan_fingerprint) ||
        !xr_target_plan_is_verified(token->plan) ||
        !xr_target_plan_fingerprint_is_intact(token->plan) ||
        !xr_fingerprint_equal(xr_target_plan_fingerprint(token->plan),
                              token->plan_fingerprint))
        return fail(diagnostic, diagnostic_size,
                    "entry adapter authority is not exact");
    bool native = token->executor_kind == XR_ENTRY_EXECUTOR_NATIVE_I64;
    if ((native && !token->native_entry) ||
        (!native && (token->native_entry || token->native_context)))
        return fail(diagnostic, diagnostic_size,
                    "entry executor does not match its adapter binding");
    *adapter = (XrVmEntryAdapterI64) {
        .expectation = *expectation,
        .native_entry = token->native_entry,
        .native_context = token->native_context,
        .executor_kind = token->executor_kind,
        .frozen = 1u,
    };
    return true;
}

bool xr_typed_entry_adapter_i64_matches_target(
    const XrVmEntryAdapterI64 *adapter,
    const XrTargetEntryExpectationRecord *target) {
    const XrEntryCellExpectation *actual =
        adapter ? &adapter->expectation : NULL;
    return adapter && adapter->frozen == 1u && target && actual &&
           expectation_is_exact(actual) && target->flags == 0 &&
           target->reserved32 == 0 &&
           target->abi_schema_version == actual->abi.schema_version &&
           target->parameter_count == actual->abi.parameter_count &&
           target->native_abi == actual->abi.native_abi &&
           target->value_kind == actual->abi.value_kind &&
           generated_contract(target->adapter_kind, actual->adapter_kind,
                              target->value_kind) &&
           target->target_data_layout == actual->abi.target_data_layout &&
           xr_fingerprint_equal(target->target_profile_fingerprint,
                                actual->abi.target_profile_fingerprint) &&
           xr_fingerprint_equal(target->entry_abi_fingerprint,
                                actual->abi.fingerprint) &&
           xr_fingerprint_equal(target->adapter_fingerprint,
                                actual->adapter_fingerprint) &&
           adapter->executor_kind == actual->executor_kind &&
           ((adapter->executor_kind == XR_ENTRY_EXECUTOR_TYPED_VM &&
             !adapter->native_entry && !adapter->native_context) ||
            (adapter->executor_kind == XR_ENTRY_EXECUTOR_NATIVE_I64 &&
             adapter->native_entry));
}

XrEntryNativeStatus xr_typed_entry_adapter_i64_invoke_native(
    const XrVmEntryAdapterI64 *adapter, const int64_t *arguments,
    uint32_t argument_count, int64_t *result) {
    if (!adapter || adapter->frozen != 1u || !result ||
        adapter->executor_kind != XR_ENTRY_EXECUTOR_NATIVE_I64 ||
        !adapter->native_entry ||
        argument_count != adapter->expectation.abi.parameter_count ||
        (argument_count && !arguments) ||
        !expectation_is_exact(&adapter->expectation))
        return XR_ENTRY_NATIVE_ERROR;
    return adapter->native_entry(adapter->native_context, arguments,
                                 argument_count, result);
}
