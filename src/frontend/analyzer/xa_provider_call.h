/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_provider_call.h - Verified source provider-call facts
 */

#ifndef XA_PROVIDER_CALL_H
#define XA_PROVIDER_CALL_H

#include "../../base/xstable_id.h"
#include "../../core/xr_core_spec_gen.h"
#include <stdint.h>

typedef enum XaProviderCallAbi {
    XA_PROVIDER_CALL_ABI_NONE = 0,
    XA_PROVIDER_CALL_ABI_I64_TO_I64 = 1,
} XaProviderCallAbi;

typedef struct XaProviderCallFact {
    uint32_t source_symbol_id;
    XrStableId contract_id;
    XrStableId operation_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
    uint8_t call_abi; /* XaProviderCallAbi */
    uint8_t complete;
} XaProviderCallFact;

typedef struct XaNodeProviderCallEntry {
    uint32_t node_id;
    XaProviderCallFact fact;
} XaNodeProviderCallEntry;

#define XA_PROVIDER_CALL_EFFECT_MASK                                                               \
    (XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL)
#define XA_PROVIDER_CALL_CAPABILITY_MASK XR_CORE_CAPABILITY_PROVIDER_BINDING

#endif  // XA_PROVIDER_CALL_H
