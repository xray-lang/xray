/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmethod.c - Method object implementation
 */

#include "xmethod.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../value/xvalue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Method Creation ========== */

XrMethod xr_method_from_closure(struct XrClosure* closure, uint8_t flags) {
    XR_DCHECK(closure != NULL, "method_from_closure: NULL closure");
    XrMethod method;
    method.type = XMETHOD_CLOSURE;
    method.as.closure = closure;
    method.flags = flags;
    method.op_type = 0;
    method.symbol = 0;
    return method;
}

XrMethod xr_method_from_primitive(XrCFunctionPtr primitive, uint8_t flags) {
    XR_DCHECK(primitive != NULL, "method_from_primitive: NULL primitive");
    XrMethod method;
    method.type = XMETHOD_PRIMITIVE;
    method.as.primitive = primitive;
    method.flags = flags;
    method.op_type = 0;
    method.symbol = 0;
    return method;
}

XrMethod xr_method_from_getter(struct XrClosure* getter_closure, bool is_private) {
    XrMethod method;
    method.type = XMETHOD_GETTER;
    method.as.closure = getter_closure;
    method.flags = is_private ? XMETHOD_FLAG_PRIVATE : 0;
    method.op_type = 0;
    method.symbol = 0;
    return method;
}

XrMethod xr_method_from_setter(struct XrClosure* setter_closure, bool is_private) {
    XrMethod method;
    method.type = XMETHOD_SETTER;
    method.as.closure = setter_closure;
    method.flags = is_private ? XMETHOD_FLAG_PRIVATE : 0;
    method.op_type = 0;
    method.symbol = 0;
    return method;
}

XrMethod xr_method_from_operator(struct XrClosure* operator_closure, 
                                  uint8_t op_type, bool is_private) {
    XR_DCHECK(operator_closure != NULL, "method_from_operator: NULL closure");
    XrMethod method;
    method.type = XMETHOD_OPERATOR;
    method.as.closure = operator_closure;
    method.flags = is_private ? XMETHOD_FLAG_PRIVATE : 0;
    method.op_type = op_type;
    method.symbol = 0;
    return method;
}

/* ========== Debug ========== */

void xr_method_print(const XrMethod *method) {
    if (!method) {
        printf("null method\n");
        return;
    }
    
    printf("Method{ type=%s, flags=0x%02x }\n",
           xr_method_type_name(method->type),
           method->flags);
}

const char* xr_method_type_name(XrMethodType type) {
    switch (type) {
        case XMETHOD_NONE:      return "None";
        case XMETHOD_CLOSURE:   return "Closure";
        case XMETHOD_PRIMITIVE: return "Primitive";
        case XMETHOD_GETTER:    return "Getter";
        case XMETHOD_SETTER:    return "Setter";
        case XMETHOD_OPERATOR:  return "Operator";
        default:                return "Unknown";
    }
}
