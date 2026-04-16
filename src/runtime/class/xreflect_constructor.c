/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_constructor.c - Constructor class methods
 */

#include "xreflect_internal.h"
#include "../../base/xchecks.h"
#include "../xvm_call.h"

XrValue xr_constructor_newInstance(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "constructor_newInstance: NULL isolate");
    if (nargs < 1) {
        fprintf(stderr, "Constructor.newInstance: requires at least 1 argument\n");
        return xr_null();
    }
    
    XrMethodMetadata *ctor = xr_get_method_metadata(args[0]);
    
    if (!ctor || !ctor->owner) {
        fprintf(stderr, "Constructor.newInstance: invalid constructor\n");
        return xr_null();
    }
    
    XrayIsolate *X = isolate;
    XrClass *klass = ctor->owner;
    
    XrInstance *instance = xr_instance_new(X, klass);
    XrValue instance_val = xr_value_from_instance(instance);
    
    XrArray *arg_array = NULL;
    int arg_count = 0;
    
    if (nargs >= 2 && XR_IS_ARRAY(args[1])) {
        arg_array = XR_TO_ARRAY(args[1]);
        arg_count = arg_array->length;
    }
    
    if (arg_count + 1 > 256) {
        fprintf(stderr, "Constructor.newInstance: too many arguments (%d), max 255\n", arg_count);
        return xr_null();
    }
    
    XrValue call_args[256];
    int total_args = 0;
    call_args[total_args++] = instance_val;
    
    if (arg_array && arg_count > 0) {
        for (int i = 0; i < arg_count; i++) {
            call_args[total_args++] = ((XrValue*)arg_array->data)[i];
        }
    }
    
    XrMethod *method = &ctor->owner->methods[ctor->method_index];
    
    if (method->type == XMETHOD_PRIMITIVE) {
        XrCFunctionPtr func = method->as.primitive;
        func(isolate, call_args, total_args);
    } else if (method->type == XMETHOD_CLOSURE) {
        XrClosure *closure = method->as.closure;
        if (closure) {
            xr_vm_call_closure(isolate, closure, call_args, total_args);
        }
    }
    
    return instance_val;
}

/* ========== Type Property Getters ========== */

/**
 * Type.name getter
 */
