/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_key.h - Finite built-in key equivalence and hashing
 *
 * KEY CONCEPT:
 *   Key equivalence is reflexive even where ordinary floating equality is not.
 *   Kernel support grants no declaration-site Hashable or operation authority.
 */
#ifndef XXIR_KEY_H
#define XXIR_KEY_H
#include "xxir_value.h"
#include "xxir_types.h"
#include "xxir_nominal.h"
static inline bool xr_xir_key_builtin_type(XrXirType type) {
    return type == XR_XIR_BOOL || type == XR_XIR_STRING || type == XR_XIR_RUNE ||
        xr_xir_type_is_number(type);
}
/* The surrounding type owner has admitted complete nominal metadata. The
 * selected instance must still be an exact enum with no payload in any variant.
 * This shape query never creates a receiving identity between type arenas. */
static inline bool xir_key_admitted_type(const XrXirTypes *types,XrXirType type) {
    if(xr_xir_key_builtin_type(type))return true;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!node||node->kind!=XR_XIR_TYPE_NOMINAL||node->nominal.field_count||!types->nominals||
        node->nominal.declaration>=types->nominals->count)return false;
    const XrXirNominalTable *table=types->nominals;
    if(!!table->declarations==!!table->identities)return false;
    uint32_t kind,arity,fields,count;const XrXirNominalVariant *variants;
    XrXirLiteral module,name;
    if(table->declarations) {
        XrXirNominalDeclaration item=table->declarations[node->nominal.declaration];
        kind=item.kind;arity=item.parameter_count;fields=item.field_count;count=item.variant_count;
        variants=item.variants;module=item.module;name=item.name;
    } else {
        XrXirNominalIdentity item=table->identities[node->nominal.declaration];
        kind=item.kind;arity=item.arity;fields=item.field_count;count=item.variant_count;
        variants=item.variants;module=item.module;name=item.name;
    }
    if(kind!=XR_XIR_NOMINAL_ENUM||fields||!count||!variants||!module.length||!module.bytes||
        !name.length||!name.bytes||node->nominal.argument_count!=arity||
        (arity&&!node->nominal.arguments))return false;
    /* Structural admission proves that variant intervals partition all fields,
     * so zero total fields proves every variant empty. It also checks every
     * nominal argument; the native Unit exceptions both require payload fields.
     * This constant-time fact is valid only below that authentic type owner. */
    return true;
}
/* Raw profile inspection retains complete validation for callers which have
 * not yet established the structural owner's invariants. */
static inline bool xr_xir_key_type(const XrXirTypes *types,XrXirType type) {
    if(!xir_key_admitted_type(types,type))return false;
    if(xr_xir_key_builtin_type(type))return true;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    const XrXirNominalTable *table=types->nominals;
    const XrXirNominalVariant *variants=table->declarations?
        table->declarations[node->nominal.declaration].variants:table->identities[node->nominal.declaration].variants;
    uint32_t count=table->declarations?table->declarations[node->nominal.declaration].variant_count:
        table->identities[node->nominal.declaration].variant_count;
    for(uint32_t a=0;a<node->nominal.argument_count;++a)if(node->nominal.arguments[a]==XR_XIR_UNIT)return false;
    for(uint32_t v=0;v<count;++v)if(variants[v].field_begin||variants[v].field_count||
        !variants[v].name.length||!variants[v].name.bytes)return false;
    return true;
}
/* Borrowed keys retain no owner and allocate no storage. Empty enum keys
 * require the receiving arena's authenticated identity lineage, including real
 * nominal arguments; identical descriptor shapes or local ordinals do not join
 * domains. Their work includes the bounded receiving type lookup; key eligibility is
 * presealed in the arena and never rescans enum metadata at runtime.
 * Built-in keys require no arena.
 * All NaNs share one key; both signed zeros share one key. Strings use all bytes.
 * Hash work is one plus eight scalar bytes or the String byte length. Equality
 * work is one scalar/length decision plus all equal-length String bytes.
 * A current receiving admission Domain is required. Failures preserve output;
 * consumed work is not refunded. Unsupported families confer no false proof. */
XR_FUNC XrXirValueStatus xr_xir_key_hash(const XrXirValue *value,
    XrXirValueAdmission *admission, uint64_t *output);
XR_FUNC XrXirValueStatus xr_xir_key_equal(const XrXirValue *left,
    const XrXirValue *right, XrXirValueAdmission *admission, bool *output);
#endif // XXIR_KEY_H
