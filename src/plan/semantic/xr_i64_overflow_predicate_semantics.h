/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_i64_overflow_predicate_semantics.h - Sealed i64 overflow-call semantics
 */

#ifndef XR_I64_OVERFLOW_PREDICATE_SEMANTICS_H
#define XR_I64_OVERFLOW_PREDICATE_SEMANTICS_H

#include "../../base/xdefs.h"
#include "../../base/xstable_id.h"
#include <stdbool.h>
#include <stdint.h>

#define XR_I64_OVERFLOW_PREDICATE_SEMANTICS_SCHEMA_VERSION UINT32_C(1)

typedef enum XrI64OverflowPredicateKind {
    XR_I64_OVERFLOW_PREDICATE_INVALID = 0,
    XR_I64_OVERFLOW_PREDICATE_ADD,
    XR_I64_OVERFLOW_PREDICATE_SUB,
    XR_I64_OVERFLOW_PREDICATE_MUL,
    XR_I64_OVERFLOW_PREDICATE_COUNT,
} XrI64OverflowPredicateKind;

/* These are durable dispatch-symbol IDs from xi_method_sym.def.  The semantic
 * owner freezes the values here so PSC, Xi, TargetPlan, VM and AOT never need
 * to recover them from selector text. */
enum {
    XR_I64_OVERFLOW_METHOD_SYMBOL_ADD = 225,
    XR_I64_OVERFLOW_METHOD_SYMBOL_SUB = 226,
    XR_I64_OVERFLOW_METHOD_SYMBOL_MUL = 227,
};

XR_FUNC bool xr_i64_overflow_predicate_method_symbol(XrI64OverflowPredicateKind kind,
                                                      uint32_t *symbol);
XR_FUNC bool xr_i64_overflow_predicate_kind_from_method_symbol(
    uint32_t symbol, XrI64OverflowPredicateKind *kind);
XR_FUNC bool xr_i64_overflow_predicate_builtin_identity(XrI64OverflowPredicateKind kind,
                                                        XrStableId *identity);
XR_FUNC bool xr_i64_overflow_predicate_kind_from_builtin_identity(
    XrStableId identity, XrI64OverflowPredicateKind *kind);
XR_FUNC bool xr_i64_overflow_predicate_policy(XrFingerprint *fingerprint);
XR_FUNC bool xr_i64_overflow_predicate_entry_signature(XrFingerprint *fingerprint);
XR_FUNC bool xr_i64_overflow_predicate_entry_effect(XrFingerprint *fingerprint);
XR_FUNC bool xr_i64_overflow_predicate_call_contract(XrI64OverflowPredicateKind kind,
                                                     XrFingerprint *fingerprint);

#endif  // XR_I64_OVERFLOW_PREDICATE_SEMANTICS_H
