/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_fact.h - Pure queries against an explicit XIR compilation target
 */
#ifndef XR_TARGET_FACT_H
#define XR_TARGET_FACT_H

#include "xr_target_query_registry_gen.h"
#include <stdbool.h>

typedef struct XrXirTarget {
    uint32_t os, architecture, abi_version;
} XrXirTarget;

typedef struct XrTargetFact {
    uint16_t query_id, expected_value;
} XrTargetFact;

static inline bool xr_target_os_machine_valid(uint32_t os, uint32_t architecture) {
    if (os != XR_TARGET_QUERY_VALUE_OS_WINDOWS && os != XR_TARGET_QUERY_VALUE_OS_LINUX &&
        os != XR_TARGET_QUERY_VALUE_OS_MACOS) return false;
    if (architecture == XR_TARGET_QUERY_VALUE_ARCH_X86_64 ||
        architecture == XR_TARGET_QUERY_VALUE_ARCH_AARCH64) return true;
    return os == XR_TARGET_QUERY_VALUE_OS_LINUX &&
        (architecture == XR_TARGET_QUERY_VALUE_ARCH_POWERPC64 ||
         architecture == XR_TARGET_QUERY_VALUE_ARCH_LOONGARCH64);
}

/* The caller supplies the admitted compilation target. A bad query or value
 * preserves the output. Native ABI queries are distinct from the XIR value ABI. */
static inline bool xr_target_fact_matches(const XrXirTarget *target, XrTargetFact fact, bool *matches) {
    if (!target || !matches || !target->abi_version ||
        !xr_target_os_machine_valid(target->os, target->architecture)) return false;
    const XrTargetQueryEnumDesc *query = xr_target_query_enum_by_query_id(fact.query_id);
    if (!query || !xr_target_query_enum_value_valid(query->core_type_id, fact.expected_value)) return false;
    uint32_t actual;
    switch (fact.query_id) {
    case XR_TARGET_QUERY_OPERATING_SYSTEM: actual = target->os; break;
    case XR_TARGET_QUERY_ARCHITECTURE: actual = target->architecture; break;
    default: return false;
    }
    *matches = actual == fact.expected_value;
    return true;
}
#endif // XR_TARGET_FACT_H
