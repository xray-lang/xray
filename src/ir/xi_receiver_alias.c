/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_receiver_alias.c - see xi_receiver_alias.h
 */

#include "xi_receiver_alias.h"
#include "xi_own.h"
#include "../frontend/analyzer/xanalyzer_builtins.h"

#include <string.h>

/* Method calls keep the source member name in `aux`. A few members are lowered
 * to receiver-specialized XI_CALL_BUILTIN intrinsics instead, which replace it
 * with the intrinsic's own name; this table recovers the member the intrinsic
 * came from so the declaration stays the single source of truth. Intrinsics
 * that build a NEW container (array_copy_new, array_filled_new,
 * array_with_capacity) are deliberately absent, as is void-returning
 * array_clear. */
static const struct {
    const char *intrinsic;
    const char *member;
} k_builtin_member_names[] = {
    {"array_reserve", "reserve"},
    {"array_resize", "resize"},
};

static const char *builtin_source_member(const char *intrinsic) {
    if (!intrinsic)
        return NULL;
    for (size_t i = 0; i < sizeof(k_builtin_member_names) / sizeof(k_builtin_member_names[0]);
         i++) {
        if (strcmp(k_builtin_member_names[i].intrinsic, intrinsic) == 0)
            return k_builtin_member_names[i].member;
    }
    return NULL;
}

XR_FUNC bool xi_call_result_aliases_receiver(const XiValue *v) {
    if (!v || v->nargs < 1 || !v->args[0] || !v->aux)
        return false;
    /* Only an RC result can alias: a scalar or void result carries no
     * reference to confuse with the receiver's. */
    if (!xi_own_type_is_rc(v->type))
        return false;

    const char *member = NULL;
    switch (v->op) {
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            member = (const char *) v->aux;
            break;
        case XI_CALL_BUILTIN:
            member = builtin_source_member((const char *) v->aux);
            break;
        default:
            return false;
    }
    if (!member)
        return false;

    return xa_builtin_member_returns_receiver((XrType *) v->args[0]->type, member);
}
