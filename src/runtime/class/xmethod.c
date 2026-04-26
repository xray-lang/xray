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
#include <stdio.h>

/* ========== Debug ========== */

void xr_method_print(const XrMethod *method) {
    if (!method) {
        printf("null method\n");
        return;
    }

    printf("Method{ type=%s, flags=0x%02x }\n", xr_method_type_name(method->type), method->flags);
}

const char *xr_method_type_name(XrMethodType type) {
    switch (type) {
        case XMETHOD_NONE:
            return "None";
        case XMETHOD_CLOSURE:
            return "Closure";
        case XMETHOD_PRIMITIVE:
            return "Primitive";
        case XMETHOD_GETTER:
            return "Getter";
        case XMETHOD_SETTER:
            return "Setter";
        case XMETHOD_OPERATOR:
            return "Operator";
        default:
            return "Unknown";
    }
}
