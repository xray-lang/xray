/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_entry_abi.h - Canonical dynamic-entry ABI identity
 */

#ifndef XR_TARGET_ENTRY_ABI_H
#define XR_TARGET_ENTRY_ABI_H

#include "xr_target_plan.h"

#define XR_TARGET_ENTRY_ABI_SCHEMA_VERSION UINT32_C(1)

typedef struct XrTargetEntryAbiFacts {
    uint32_t schema_version;
    uint16_t parameter_count;
    uint16_t native_abi;
    uint8_t value_kind;
    uint8_t reserved8[3];
    uint64_t target_data_layout;
    XrFingerprint target_profile_fingerprint;
} XrTargetEntryAbiFacts;

XR_FUNC bool xr_target_entry_abi_fingerprint(
    const XrTargetEntryAbiFacts *facts, XrFingerprint *fingerprint);
XR_FUNC bool xr_target_entry_identity_adapter_fingerprint(
    const XrFingerprint *entry_abi_fingerprint, XrFingerprint *fingerprint);

#endif  // XR_TARGET_ENTRY_ABI_H
