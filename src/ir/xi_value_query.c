/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_value_query.c - Backend-neutral IR value/type classification predicates
 */

#include "xi_value_query.h"
#include "../runtime/value/xtype.h"
#include <string.h>

XR_FUNC bool xi_type_is_channel(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_CHANNEL)
        return true;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (xi_type_is_channel(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

XR_FUNC bool xi_type_is_named_instance(const XrType *type, const char *name) {
    if (!type || !name)
        return false;
    if (type->kind == XR_KIND_INSTANCE)
        return type->instance.class_name && strcmp(type->instance.class_name, name) == 0;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (xi_type_is_named_instance(type->union_type.members[i], name))
                return true;
        }
    }
    return false;
}

XR_FUNC bool xi_type_is_task(const XrType *type) {
    return xi_type_is_named_instance(type, "Task");
}

/* Strip BOX/UNBOX/COPY identity wrappers so the test sees the carried type. */
static const XiValue *xi_value_unwrap_identity(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v)) &&
           v->nargs >= 1)
        v = v->args[0];
    return v;
}

XR_FUNC bool xi_value_type_is_channel(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_channel(v->type);
}

XR_FUNC bool xi_value_type_is_task(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_task(v->type);
}

XR_FUNC bool xi_value_type_is_work_queue(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_named_instance(v->type, "WorkQueue");
}

XR_FUNC bool xi_value_type_is_result_group(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_named_instance(v->type, "ResultGroup");
}

XR_FUNC bool xi_value_type_is_unknown(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return !v || !v->type || v->type->kind == XR_KIND_UNKNOWN;
}
