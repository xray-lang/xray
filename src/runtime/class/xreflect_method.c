/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_method.c - Method accessor (zero-copy)
 */

#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../gc/xgc.h"
#include "xreflect_internal.h"
#include "xreflect_registry.h"
#include "../xisolate_api.h"
#include "../xvm_call.h"
#include "../value/xchunk.h"
#include "../../runtime/xexec_frame.h"
#include "../object/xstring.h"
#include <ctype.h>
#include <string.h>

static inline XrMethodMetadata *get_method_metadata_internal(XrValue val) {
    if (!XR_IS_INSTANCE(val))
        return NULL;
    MethodWrapper *wrapper = (MethodWrapper *) XR_TO_INSTANCE(val);
    return &wrapper->metadata;
}

/* ========== Method Property Getters ========== */

XrValue xr_method_getName(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "method_getName: NULL isolate");
    (void) args;
    (void) nargs;

    XrMethodMetadata *method = get_method_metadata_internal(self);
    if (!method || !method->owner)
        return xr_null();

    XrMethod *m = &method->owner->methods[method->method_index];
    XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
    const char *name = xr_symbol_get_name_in_table(sym_table, m->symbol);

    if (!name)
        return xr_null();
    return xr_string_value(xr_string_intern(isolate, name, strlen(name), 0));
}

XrValue xr_method_getDeclaringType(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "method_getDeclaringType: NULL isolate");
    (void) args;
    (void) nargs;

    XrMethodMetadata *method = get_method_metadata_internal(self);
    if (!method || !method->owner)
        return xr_null();

    XrTypeMetadata meta = {.klass = method->owner};
    return xr_create_type_object(isolate, &meta);
}

/**
 * Method.isOverride: bool
 */
XrValue xr_method_getIsOverride(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;

    XrMethodMetadata *method = get_method_metadata_internal(self);
    if (!method || !method->owner)
        return xr_bool(false);

    XrMethod *m = &method->owner->methods[method->method_index];
    XrClass *super = method->owner->super;

    // Check if parent class has method with same symbol
    if (super && m->symbol >= 0) {
        XrMethod *parent_method = xr_class_lookup_method(super, m->symbol);
        if (parent_method && parent_method->type != XMETHOD_NONE) {
            return xr_bool(true);  // Method overrides parent
        }
    }

    return xr_bool(false);
}

XrValue xr_method_getIsGetter(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;

    XrMethodMetadata *method = get_method_metadata_internal(self);
    if (!method || !method->owner)
        return xr_bool(false);

    XrMethod *m = &method->owner->methods[method->method_index];
    return xr_bool(m->type == XMETHOD_GETTER);
}

XrValue xr_method_getIsSetter(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;

    XrMethodMetadata *method = get_method_metadata_internal(self);
    if (!method || !method->owner)
        return xr_bool(false);

    XrMethod *m = &method->owner->methods[method->method_index];
    return xr_bool(m->type == XMETHOD_SETTER);
}

XrValue xr_method_getIsOperator(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;

    XrMethodMetadata *method = get_method_metadata_internal(self);
    if (!method || !method->owner)
        return xr_bool(false);

    XrMethod *m = &method->owner->methods[method->method_index];
    return xr_bool(m->type == XMETHOD_OPERATOR);
}

XrValue xr_method_getReturnType(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "method_getReturnType: NULL isolate");
    (void) args;
    (void) nargs;

    XrMethodMetadata *method = get_method_metadata_internal(self);
    if (!method || !method->owner)
        return xr_null();

    XrMethod *m = &method->owner->methods[method->method_index];

    // Constructor returns the owner class type
    if (m->flags & XMETHOD_FLAG_CONSTRUCTOR) {
        const char *cls_name = method->owner->name;
        XrTypeMetadata *cls_meta = cls_name ? xr_registry_find_type(isolate, cls_name) : NULL;
        if (!cls_meta) {
            XrTypeMetadata *void_meta = xr_registry_find_type(isolate, "void");
            if (void_meta)
                return xr_create_type_object(isolate, void_meta);
        } else {
            return xr_create_type_object(isolate, cls_meta);
        }
    }

    // For closure methods, extract return type from proto
    if (m->type == XMETHOD_CLOSURE && m->as.closure && m->as.closure->proto) {
        const char *ret = m->as.closure->proto->return_type;
        if (ret && ret[0] != '\0') {
            XrTypeMetadata *ret_meta = xr_registry_find_type(isolate, ret);
            if (ret_meta)
                return xr_create_type_object(isolate, ret_meta);
            // Return type name as string if not in registry
            XrString *s = xr_string_new(isolate, ret, strlen(ret));
            if (s)
                return xr_string_value(s);
        }
    }

    // No type info available
    return xr_null();
}

XrValue xr_method_getParameterCount(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;

    XrMethodMetadata *method = get_method_metadata_internal(self);
    if (!method || !method->owner)
        return xr_int(0);

    XrMethod *m = &method->owner->methods[method->method_index];
    if (m->type == XMETHOD_CLOSURE && m->as.closure) {
        return xr_int(m->as.closure->proto->numparams);
    }

    return xr_int(0);
}

static const char *type_kind_to_name(XrTypeKind kind) {
    switch (kind) {
        case XR_KIND_INT:
            return "int";
        case XR_KIND_FLOAT:
            return "float";
        case XR_KIND_STRING:
            return "string";
        case XR_KIND_BOOL:
            return "bool";
        case XR_KIND_NULL:
            return "null";
        case XR_KIND_ARRAY:
            return "Array";
        case XR_KIND_MAP:
            return "Map";
        case XR_KIND_SET:
            return "Set";
        case XR_KIND_CHANNEL:
            return "Channel";
        case XR_KIND_JSON:
            return "Json";
        case XR_KIND_INSTANCE:
            return "object";
        case XR_KIND_FUNCTION:
            return "function";
        case XR_KIND_UNIT:
            return "void";
        default:
            return "unknown";
    }
}

XrValue xr_method_getParameters(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "method_getParameters: NULL isolate");
    (void) args;
    (void) nargs;

    XrMethodMetadata *method = get_method_metadata_internal(self);
    if (!method || !method->owner)
        return xr_null();

    XrMethod *m = &method->owner->methods[method->method_index];
    XrCoroutine *co = xr_current_coro(isolate);
    XrArray *params = xr_array_new(co);
    if (!params)
        return xr_null();

    if (m->type == XMETHOD_CLOSURE && m->as.closure && m->as.closure->proto) {
        XrProto *proto = m->as.closure->proto;
        int np = proto->numparams;
        for (int i = 0; i < np; i++) {
            // Derive type name from param_types if available. The metadata
            // is value-copied into ParameterWrapper inside xr_create_parameter_object,
            // so a short-lived stack record is sufficient and avoids leaking
            // a malloc'd descriptor that no one else would free.
            const char *tname = "unknown";
            if (proto->param_types && i < proto->param_types_count && proto->param_types[i]) {
                tname = type_kind_to_name(proto->param_types[i]->kind);
            }
            XrParameterMetadata pmeta = {.name = tname, .index = i};
            XrValue pval = xr_create_parameter_object(isolate, &pmeta);
            xr_array_push(params, pval);
        }
    }

    return xr_value_from_array(params);
}

/* ========== Method.invoke ========== */

XrValue xr_method_invoke(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "method_invoke: NULL isolate");
    if (nargs < 1) {
        xr_log_warning("reflect", "Method.invoke: requires at least 1 argument");
        return xr_null();
    }

    XrMethodMetadata *method = get_method_metadata_internal(self);
    XrValue instance = args[0];

    if (!method || !method->owner) {
        xr_log_warning("reflect", "Method.invoke: invalid method");
        return xr_null();
    }

    XrMethod *xr_method = &method->owner->methods[method->method_index];

    XrArray *arg_array = NULL;
    int arg_count = 0;

    if (nargs >= 2 && XR_IS_ARRAY(args[1])) {
        arg_array = XR_TO_ARRAY(args[1]);
        arg_count = arg_array->length;
    }

    if (arg_count + 1 > 256) {
        xr_log_warning("reflect", "Method.invoke: too many arguments (%d), max 255", arg_count);
        return xr_null();
    }

    XrValue call_args[256];
    int total_args = 0;

    if (arg_array && arg_count > 0) {
        XrValue *adata = (XrValue *) arg_array->data;
        for (int i = 0; i < arg_count; i++) {
            call_args[total_args++] = adata[i];
        }
    }

    switch (xr_method->type) {
        case XMETHOD_PRIMITIVE: {
            return xr_method->as.primitive(isolate, instance, call_args, total_args);
        }

        case XMETHOD_CLOSURE: {
            XrClosure *closure = xr_method->as.closure;
            if (!closure) {
                xr_log_warning("reflect", "Method.invoke: closure is null");
                return xr_null();
            }

            // Closure calling convention: args[0]=self
            XrValue cl_args[256];
            cl_args[0] = instance;
            for (int i = 0; i < total_args && i < 255; i++)
                cl_args[i + 1] = call_args[i];
            return xr_vm_call_closure(isolate, closure, cl_args, total_args + 1);
        }

        default: {
            xr_log_warning("reflect", "Method.invoke: unsupported method type");
            return xr_null();
        }
    }
}

XrValue xr_method_invokeStatic(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "method_invokeStatic: NULL isolate");

    XrMethodMetadata *method = get_method_metadata_internal(self);
    if (!method || !method->owner) {
        xr_log_warning("reflect", "Method.invokeStatic: invalid method");
        return xr_null();
    }

    XrMethod *xr_method = &method->owner->methods[method->method_index];

    if (!xr_method_is_static(xr_method)) {
        xr_log_warning("reflect", "Method.invokeStatic: method is not static");
        return xr_null();
    }

    XrArray *arg_array = NULL;
    int arg_count = 0;

    if (nargs >= 1 && XR_IS_ARRAY(args[0])) {
        arg_array = XR_TO_ARRAY(args[0]);
        arg_count = arg_array->length;
    }

    if (arg_count > 256) {
        xr_log_warning("reflect", "Method.invokeStatic: too many arguments (%d), max 256",
                       arg_count);
        return xr_null();
    }

    XrValue call_args[256];
    int total_args = 0;

    if (arg_array && arg_count > 0) {
        XrValue *adata = (XrValue *) arg_array->data;
        for (int i = 0; i < arg_count; i++) {
            call_args[total_args++] = adata[i];
        }
    }

    switch (xr_method->type) {
        case XMETHOD_PRIMITIVE: {
            return xr_method->as.primitive(isolate, xr_null(), call_args, total_args);
        }

        case XMETHOD_CLOSURE: {
            XrClosure *closure = xr_method->as.closure;
            if (!closure) {
                xr_log_warning("reflect", "Method.invokeStatic: closure is null");
                return xr_null();
            }

            return xr_vm_call_closure(isolate, closure, call_args, total_args);
        }

        default: {
            xr_log_warning("reflect", "Method.invokeStatic: unsupported method type");
            return xr_null();
        }
    }
}
