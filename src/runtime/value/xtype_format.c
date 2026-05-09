/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_format.c - Type formatting, hashing, and immutability
 *
 * KEY CONCEPT:
 *   Converts XrType to human-readable string representation.
 *   Also provides immutability checks and const type construction.
 *   Split from xtype.c for maintainability.
 */

#include "xtype.h"
#include "xtype_internal.h"
#include "xtype_names.h"
#include "../../base/xchecks.h"
#include <string.h>
#include <stdio.h>

// Convert type to string representation
// Uses pool arena allocation — each call returns an arena-allocated string that
// lives until pool reset/destroy. Safe for arbitrary nesting depth (no rotating
// buffer overwrite). Simple types return static constants (zero allocation).
const char *xr_type_to_string(XrType *type) {
#define TYPE_STR_BUF_SIZE 256

    if (!type)
        return TYPE_NAME_UNKNOWN;

    // Simple types: return static constants (no allocation)
    if (XR_TYPE_IS_UNKNOWN(type))
        return TYPE_NAME_UNKNOWN;
    if (XR_TYPE_IS_NEVER(type))
        return TYPE_NAME_NEVER;
    if (XR_TYPE_IS_VOID(type))
        return TYPE_NAME_VOID;
    if (XR_TYPE_IS_INT(type) && !type->is_nullable)
        return TYPE_NAME_INT;
    if (XR_TYPE_IS_FLOAT(type) && !type->is_nullable)
        return TYPE_NAME_FLOAT;
    if (XR_TYPE_IS_STRING(type) && !type->is_nullable)
        return TYPE_NAME_STRING;
    if (XR_TYPE_IS_BOOL(type) && !type->is_nullable)
        return TYPE_NAME_BOOL;
    if (XR_TYPE_IS_NULL(type))
        return TYPE_NAME_NULL;

    // Complex types need pool for arena allocation
    XrTypePool *pool = xr_type_get_current_pool();
    XR_CHECK(pool != NULL, "Type pool not set - call xr_type_set_current_pool first");
    char buf[TYPE_STR_BUF_SIZE];

    if (XR_TYPE_IS_UNION(type)) {
        int pos = 0;
        for (int i = 0; i < type->union_type.member_count && pos < TYPE_STR_BUF_SIZE - 4; i++) {
            if (i > 0) {
                pos += snprintf(buf + pos, TYPE_STR_BUF_SIZE - pos, " | ");
            }
            pos += snprintf(buf + pos, TYPE_STR_BUF_SIZE - pos, "%s",
                            xr_type_to_string(type->union_type.members[i]));
        }
        if (type->is_nullable) {
            snprintf(buf + pos, TYPE_STR_BUF_SIZE - pos, " | null");
        }
        return xr_pool_strdup(pool, buf);
    }

    if (type->is_nullable) {
        XrType *base = xr_type_non_nullable(NULL, type);
        if (base) {
            snprintf(buf, TYPE_STR_BUF_SIZE, "%s?", xr_type_to_string(base));
            return xr_pool_strdup(pool, buf);
        }
        return "unknown?";
    }

    if (XR_TYPE_IS_ARRAY(type)) {
        const char *elem = type->container.element_type
                               ? xr_type_to_string(type->container.element_type)
                               : "unknown";
        snprintf(buf, TYPE_STR_BUF_SIZE, "Array<%s>", elem);
        return xr_pool_strdup(pool, buf);
    }

    if (XR_TYPE_IS_MAP(type)) {
        const char *key = type->map.key_type ? xr_type_to_string(type->map.key_type) : "unknown";
        const char *val =
            type->map.value_type ? xr_type_to_string(type->map.value_type) : "unknown";
        snprintf(buf, TYPE_STR_BUF_SIZE, "Map<%s, %s>", key, val);
        return xr_pool_strdup(pool, buf);
    }

    if (type->kind == XR_KIND_SET) {
        const char *elem = type->container.element_type
                               ? xr_type_to_string(type->container.element_type)
                               : "unknown";
        snprintf(buf, TYPE_STR_BUF_SIZE, "Set<%s>", elem);
        return xr_pool_strdup(pool, buf);
    }

    if (type->kind == XR_KIND_CHANNEL) {
        const char *elem = type->container.element_type
                               ? xr_type_to_string(type->container.element_type)
                               : "unknown";
        snprintf(buf, TYPE_STR_BUF_SIZE, "Channel<%s>", elem);
        return xr_pool_strdup(pool, buf);
    }

    if (type->kind == XR_KIND_FIXED_ARRAY) {
        const char *elem = type->fixed_array.element_type
                               ? xr_type_to_string(type->fixed_array.element_type)
                               : "unknown";
        snprintf(buf, TYPE_STR_BUF_SIZE, "[%d]%s", type->fixed_array.length, elem);
        return xr_pool_strdup(pool, buf);
    }

    if (type->kind == XR_KIND_JSON) {
        if (type->object.type_name) {
            return type->object.type_name;
        }
        if (type->object.field_count > 0 && type->object.field_names) {
            /* Count named (non-computed) fields */
            int named = 0;
            bool has_computed = false;
            for (int i = 0; i < type->object.field_count; i++) {
                if (type->object.field_names[i])
                    named++;
                else
                    has_computed = true;
            }
            /* All fields computed → fall through to plain "Json" */
            if (named > 0) {
                char *ptr = buf;
                size_t remaining = TYPE_STR_BUF_SIZE;
                int n = snprintf(ptr, remaining, "{");
                ptr += n;
                remaining -= n;

                int printed = 0;
                for (int i = 0; i < type->object.field_count && remaining > 2; i++) {
                    if (!type->object.field_names[i])
                        continue;
                    if (printed > 0) {
                        n = snprintf(ptr, remaining, ",");
                        ptr += n;
                        remaining -= n;
                    }
                    n = snprintf(ptr, remaining, "%s", type->object.field_names[i]);
                    ptr += n;
                    remaining -= n;
                    printed++;
                }
                if (has_computed && remaining > 5) {
                    n = snprintf(ptr, remaining, ",...");
                    ptr += n;
                    remaining -= n;
                }
                snprintf(ptr, remaining, "}");
                return xr_pool_strdup(pool, buf);
            }
        }
        return (type->kind == XR_KIND_JSON) ? TYPE_NAME_JSON : "{...}";
    }

    if (type->kind == XR_KIND_TYPE_PARAM) {
        return type->type_param.name ? type->type_param.name : "T";
    }
    if (xr_type_is_named_class(type, "Regex"))
        return "Regex";
    if (xr_type_is_named_class(type, "BigInt"))
        return "BigInt";
    if (xr_type_is_named_class(type, "StringBuilder"))
        return "StringBuilder";
    if (xr_type_is_named_class(type, "Exception"))
        return "Exception";
    if (type->kind == XR_KIND_ENUM) {
        return type->enum_type.enum_name ? type->enum_type.enum_name : "Enum";
    }

    if (XR_TYPE_IS_INSTANCE(type) || XR_TYPE_IS_CLASS(type)) {
        if (!type->instance.class_name)
            return "Object";

        if (type->instance.type_arg_count > 0 && type->instance.type_args) {
            char *ptr = buf;
            size_t remaining = TYPE_STR_BUF_SIZE;
            int n = snprintf(ptr, remaining, "%s<", type->instance.class_name);
            ptr += n;
            remaining -= n;

            for (int i = 0; i < type->instance.type_arg_count && remaining > 2; i++) {
                if (i > 0) {
                    n = snprintf(ptr, remaining, ", ");
                    ptr += n;
                    remaining -= n;
                }
                const char *arg_str = type->instance.type_args[i]
                                          ? xr_type_to_string(type->instance.type_args[i])
                                          : "unknown";
                n = snprintf(ptr, remaining, "%s", arg_str);
                ptr += n;
                remaining -= n;
            }
            snprintf(ptr, remaining, ">");
            return xr_pool_strdup(pool, buf);
        }

        return type->instance.class_name;
    }

    if (XR_TYPE_IS_FUNCTION(type)) {
        char *ptr = buf;
        size_t remaining = TYPE_STR_BUF_SIZE;
        int n = snprintf(ptr, remaining, "fn(");
        ptr += n;
        remaining -= n;

        for (int i = 0; i < type->function.param_count && remaining > 2; i++) {
            if (i > 0) {
                n = snprintf(ptr, remaining, ", ");
                ptr += n;
                remaining -= n;
            }
            const char *param_str = type->function.param_types[i]
                                        ? xr_type_to_string(type->function.param_types[i])
                                        : "unknown";
            n = snprintf(ptr, remaining, "%s", param_str);
            ptr += n;
            remaining -= n;
        }

        const char *ret_str =
            type->function.return_type ? xr_type_to_string(type->function.return_type) : "void";
        snprintf(ptr, remaining, "): %s", ret_str);
        return xr_pool_strdup(pool, buf);
    }

    if (XR_TYPE_IS_TUPLE(type)) {
        char *ptr = buf;
        size_t remaining = TYPE_STR_BUF_SIZE;
        int n = snprintf(ptr, remaining, "(");
        ptr += n;
        remaining -= n;

        for (int i = 0; i < type->tuple.element_count && remaining > 2; i++) {
            if (i > 0) {
                n = snprintf(ptr, remaining, ", ");
                ptr += n;
                remaining -= n;
            }
            const char *elem_str = type->tuple.element_types[i]
                                       ? xr_type_to_string(type->tuple.element_types[i])
                                       : "unknown";
            n = snprintf(ptr, remaining, "%s", elem_str);
            ptr += n;
            remaining -= n;
        }
        snprintf(ptr, remaining, ")");
        return xr_pool_strdup(pool, buf);
    }

    return "unknown";
}

bool xr_type_is_inherently_immutable(XrType *type) {
    if (!type)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_NULL:
        case XR_KIND_STRING:
        case XR_KIND_VOID:
            return true;
        default:
            return false;
    }
}

// Immutability
bool xr_type_is_const(XrType *type) {
    if (!type)
        return false;
    if (xr_type_is_inherently_immutable(type))
        return true;
    return type->is_const;
}

XrType *xr_type_make_const(XrayIsolate *X, XrType *base) {
    if (!base)
        return NULL;
    if (xr_type_is_inherently_immutable(base))
        return base;
    if (base->is_const)
        return base;

    XrType *copy = xr_type_copy(X, base);
    if (copy) {
        copy->is_const = true;
    }
    return copy;
}

XrType *xr_type_object_get_field(XrType *type, const char *field_name) {
    if (!type || !field_name)
        return NULL;
    if (type->kind != XR_KIND_JSON)
        return NULL;

    for (int i = 0; i < type->object.field_count; i++) {
        if (type->object.field_names && type->object.field_names[i] &&
            strcmp(type->object.field_names[i], field_name) == 0) {
            return type->object.field_types ? type->object.field_types[i]
                                            : xr_type_new_unknown(NULL);
        }
    }
    return NULL;
}
