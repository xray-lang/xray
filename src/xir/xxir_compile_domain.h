/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_compile_domain.h - Value storage on the original compiler resource owner
 *
 * KEY CONCEPT:
 *   Owned values retain the same finite ledger beyond their producing scope.
 */
#ifndef XXIR_COMPILE_DOMAIN_H
#define XXIR_COMPILE_DOMAIN_H
#include "xxir_value.h"
#include "../base/xcompile_resources.h"

/* Empty output is preserved on failure. The Domain and every allocation use
 * this exact ledger, including header, zero/copy and allocator work charges.
 * No quota is copied or reset. Existing Domain category bounds can be bound
 * separately; compiler bounds always apply, including when unbound locally.
 * The caller holds a live ledger reference during construction. Owned storage
 * retains it thereafter; final drop allocates nothing and refunds only live. */
XR_FUNC XrXirValueStatus xr_xir_compile_domain_new(XrCompileResources *resources,
    uint64_t byte_limit, XrXirDomain **output);
/* Borrow an aggregate's authentic Domain only on its exact compiler ledger.
 * The caller prepays full shallow header validation and keeps the Value live
 * for the entire borrow. The fixed query charge is 64 work units. The pointer
 * grants no member, construction, call or loan authority and must not escape
 * the synchronous observation or be dropped as an owning Domain reference.
 * Empty output is preserved on every failure. */
XR_FUNC XrXirValueStatus xr_xir_compile_value_domain_borrow(XrCompileResources *resources,
    const XrXirValue *value, const XrXirTypeArena *arena, XrXirDomain **output);
/* Inspect only a live constructed carrier's fixed authentic prefix. The query
 * charges 64 work before inspection, allocates nothing, and borrows its actual
 * immutable arena. The caller keeps the value alive and separately verifies
 * the complete type and value family before observing children. No member,
 * construction, call or loan permission follows from the returned metadata. */
XR_FUNC XrXirValueStatus xr_xir_compile_value_arena_borrow(XrCompileResources *resources,
    const XrXirValue *value, const XrXirTypeArena **output);
/* Observe one existing pure aggregate child into the receiver's exact compiler
 * Domain. The admission names the source's authentic arena, not the target
 * arena. The caller prepays complete shallow type/header and inline snapshot
 * costs; the operation bills its fixed observation and any actual allocation
 * to resources. Source ownership grants no access, call or loan authority.
 * Temporary retains are billed to that receiver while preserving the source's
 * authentic reference counter and lifetime. No unrelated descendant is
 * admitted and empty output survives failure. */
XR_FUNC XrXirValueStatus xr_xir_compile_value_observe(XrCompileResources *resources,
    const XrXirValue *value, uint64_t index, XrXirValueAdmission *admission,
    XrXirValue *output);
#endif // XXIR_COMPILE_DOMAIN_H
