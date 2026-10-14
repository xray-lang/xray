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
#endif // XXIR_COMPILE_DOMAIN_H
