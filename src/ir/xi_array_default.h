/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_array_default.h - Exact typed runtime-length scalar array construction
 */
#ifndef XI_ARRAY_DEFAULT_H
#define XI_ARRAY_DEFAULT_H
#include "xi.h"
#include "../runtime/value/xtype.h"

static inline bool xi_value_is_scalar_array_default(const XiValue *value) {
    if (!value || value->op != XI_ARRAY_NEW || !value->array_default_construct ||
        value->nargs != 1u || !value->args || !value->args[0] || !value->args[0]->type ||
        value->args[0]->type->is_nullable || value->args[0]->type->kind != XR_KIND_INT ||
        value->args[0]->type->scalar_rep != XR_NATIVE_I64 || !value->type ||
        value->type->is_nullable || value->type->kind != XR_KIND_ARRAY)
        return false;
    const XrType *element = value->type->container.element_type;
    return element && !element->is_nullable &&
        (element->kind == XR_KIND_INT || element->kind == XR_KIND_BOOL ||
         (element->kind == XR_KIND_FLOAT && element->scalar_rep == XR_NATIVE_F64));
}
#endif
