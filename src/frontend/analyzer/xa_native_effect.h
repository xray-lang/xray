/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_native_effect.h - Native (extern "C") declaration effect axioms
 *
 * A bodyless extern declaration has no Xray body to infer from, so the only
 * admissible evidence about its effects is the audited `[native.symbol]`
 * contract in xray.toml.  This header is the single place that turns that
 * manifest contract into analyzer effect facts; every effect pass must ask
 * here instead of assuming an absent body means "no effect".  When no
 * complete contract covers the symbol, the answer is "unknown" and callers
 * must fail closed (LANGUAGE_SPEC 5.2.11).
 */

#ifndef XA_NATIVE_EFFECT_H
#define XA_NATIVE_EFFECT_H

#include "xa_effect_db.h"
#include "xa_provider_call.h"
#include "../../base/xdefs.h"
#include <stdbool.h>

struct XaAnalyzer;
struct XaSymbol;
struct AstNode;
struct XrType;

/* Declared effect axioms for one extern symbol. */
typedef struct XaNativeEffectAxioms {
    bool has_contract;           /* a complete [native.symbol.contract] was found */
    XaSemanticEffectSet effects; /* effects the contract declares as present */
    bool allocates;              /* allocation = "may" */
    bool suspends;               /* suspend = "may" */
} XaNativeEffectAxioms;

typedef enum XaProviderCallResolution {
    XA_PROVIDER_CALL_NOT_BOUND = 0,
    XA_PROVIDER_CALL_VERIFIED,
    XA_PROVIDER_CALL_INVALID,
} XaProviderCallResolution;

/* True when `symbol` is a bodyless extern "C" declaration, i.e. a symbol whose
 * effects can never be inferred from Xray source. */
XR_FUNC bool xa_native_effect_is_bodyless_extern(const struct XaSymbol *symbol);

/* Resolve the manifest contract axioms for `symbol`.  Returns axioms with
 * has_contract = false when the symbol is not covered by a complete contract;
 * in that case every effect conclusion about it is unknown. */
XR_FUNC XaNativeEffectAxioms xa_native_effect_axioms(struct XaAnalyzer *analyzer,
                                                     const struct XaSymbol *symbol);

/* Resolve a bodyless extern declaration to its manifest-bound provider
 * identity. Bound declarations with any ABI/type drift return INVALID and
 * never fall back to ordinary FFI lowering. */
XR_FUNC XaProviderCallResolution xa_native_provider_call_resolve(struct XaAnalyzer *analyzer,
                                                                 const struct XaSymbol *symbol,
                                                                 const struct XrType *function_type,
                                                                 XaProviderCallFact *out_fact);

#endif /* XA_NATIVE_EFFECT_H */
