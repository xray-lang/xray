/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbigint_methods.c - BigInt method dispatch table.
 *
 * Method bodies are `static inline` in xbigint_methods.h so AOT
 * inlines them at the call site. Taking each address here forces
 * the compiler to emit one out-of-line copy that the VM dispatcher
 * reaches through xr_bigint_method_table[symbol].fn.
 */

#include "xbigint_methods.h"
#include "../class/xclass_builder.h"
#include "../class/xclass_system.h"
#include "../xisolate_api.h"
#include "../../base/xchecks.h"

void xr_bigint_register_class(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "bigint_register_class: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL, "bigint_register_class: NULL core");

    XrClassBuilder *b = xr_class_builder_new(isolate, "BigInt", core->objectClass);
    XR_CHECK(b != NULL, "bigint_register_class: builder alloc failed");

    xr_class_builder_add_method(b, "toString", xr_bigint_to_string_method, 0, 0);
    xr_class_builder_add_method(b, "abs", xr_bigint_abs_method, 0, 0);
    xr_class_builder_add_method(b, "sign", xr_bigint_sign_method, 0, 0);
    xr_class_builder_add_method(b, "isZero", xr_bigint_is_zero_method, 0, 0);
    xr_class_builder_add_method(b, "isNegative", xr_bigint_is_negative_method, 0, 0);
    xr_class_builder_add_method(b, "isPositive", xr_bigint_is_positive_method, 0, 0);
    xr_class_builder_add_method(b, "toInt", xr_bigint_to_int_method, 0, 0);
    xr_class_builder_add_method(b, "toFloat", xr_bigint_to_float_method, 0, 0);

    XrClass *cls = xr_class_builder_finalize(b);
    XR_CHECK(cls != NULL, "bigint_register_class: finalize failed");
    cls->flags |= XR_CLASS_BUILTIN | XR_CLASS_BIGINT;
    core->bigintClass = cls;
}
