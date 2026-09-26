/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_string_slice.h - Exact builtin scalar string slice identity
 */
#ifndef XI_STRING_SLICE_H
#define XI_STRING_SLICE_H
#include "xi.h"
#include "../runtime/value/xtype.h"

static inline bool xi_value_is_string_slice(const XiValue *value) {
    if (!value || value->op != XI_CALL_METHOD ||
        value->aux_int != ((int64_t) XI_METHOD_SYMBOL_SLICE << 1) ||
        value->nargs != 3u || !value->args || !value->type ||
        value->type->is_nullable || value->type->kind != XR_KIND_STRING)
        return false;
    for (uint16_t i = 0u; i < 3u; ++i) {
        const XrType *type = value->args[i] ? value->args[i]->type : NULL;
        if (!type || type->is_nullable ||
            (i == 0u ? type->kind != XR_KIND_STRING :
             type->kind != XR_KIND_INT || type->scalar_rep != XR_NATIVE_I64))
            return false;
    }
    return true;
}
#endif
