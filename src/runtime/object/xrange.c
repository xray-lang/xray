/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrange.c - Lightweight lazy Range implementation
 */

#include "xrange.h"
#include "xarray.h"
#include "../gc/xgc.h"
#include "../class/xclass.h"
#include "../class/xclass_builder.h"
#include "../class/xclass_system.h"
#include "../class/xinstance.h"
#include "../xisolate_api.h"
#include "../../base/xchecks.h"
#include "../../coro/xcoroutine.h"

/* ========== Internal helpers ========== */

static XrClass *range_class(XrayIsolate *X) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XR_DCHECK(core != NULL && core->rangeClass != NULL, "range: core->rangeClass not registered");
    return core->rangeClass;
}

/* ========== Type Check ========== */

bool xr_value_is_range(XrayIsolate *X, XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return xr_class_instanceof(inst->klass, range_class(X));
}

XrRange *xr_value_get_range_body(XrayIsolate *X, XrValue v) {
    if (!xr_value_is_range(X, v))
        return NULL;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return (XrRange *) xr_instance_native_body(inst);
}

/* ========== Creation ========== */

XrValue xr_range_new(XrayIsolate *X, int64_t start, int64_t end) {
    XR_DCHECK(X != NULL, "xr_range_new: NULL isolate");
    XrInstance *inst = xr_instance_new(X, range_class(X));
    if (!inst)
        return xr_null();
    XrRange *body = (XrRange *) xr_instance_native_body(inst);
    XR_DCHECK(body != NULL, "xr_range_new: native body NULL");
    body->start = start;
    body->end = end;
    body->step = 1;
    return XR_FROM_PTR(inst);
}

XrValue xr_range_new_with_step(XrayIsolate *X, int64_t start, int64_t end, int64_t step) {
    XR_DCHECK(X != NULL, "xr_range_new_with_step: NULL isolate");
    XR_CHECK(step != 0, "xr_range_new_with_step: step must not be zero");
    XrInstance *inst = xr_instance_new(X, range_class(X));
    if (!inst)
        return xr_null();
    XrRange *body = (XrRange *) xr_instance_native_body(inst);
    XR_DCHECK(body != NULL, "xr_range_new_with_step: native body NULL");
    body->start = start;
    body->end = end;
    body->step = step;
    return XR_FROM_PTR(inst);
}

/* ========== Conversion ========== */

XrValue xr_range_to_array(struct XrCoroutine *coro, XrRange *r) {
    XR_DCHECK(coro != NULL, "xr_range_to_array: coro must not be NULL");
    if (!r)
        return xr_null();

    int64_t len = xr_range_length(r);
    if (len <= 0) {
        XrArray *arr = xr_array_with_capacity(coro, 0);
        return xr_value_from_array(arr);
    }

    // Safety cap to prevent OOM (10M elements ~= 160MB for XrValue array)
    XR_CHECK(len <= 10000000, "range_to_array: range too large");

    XrArray *arr = xr_array_with_capacity(coro, (int32_t) len);
    if (!arr)
        return xr_null();

    XrValue *data = (XrValue *) arr->data;
    int64_t val = r->start;
    for (int64_t i = 0; i < len; i++) {
        data[i] = xr_int(val);
        val += r->step;
    }
    arr->length = (int32_t) len;

    return xr_value_from_array(arr);
}

/* ========== Primitive Methods ========== */

#include "xstring.h"
#include <inttypes.h>
#include <stdio.h>

static XrValue m_range_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrRange *rng = xr_value_get_range_body(iso, self);
    if (!rng)
        return xr_null();
    char buf[80];
    int n = (rng->step == 1)
                ? snprintf(buf, sizeof(buf), "%" PRId64 "..%" PRId64, rng->start, rng->end)
                : snprintf(buf, sizeof(buf), "%" PRId64 "..%" PRId64 ":%" PRId64, rng->start,
                           rng->end, rng->step);
    XrString *s = xr_string_intern(iso, buf, (size_t) n, 0);
    return xr_string_value(s);
}

static XrValue m_range_to_array(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrRange *rng = xr_value_get_range_body(iso, self);
    if (!rng)
        return xr_null();
    return xr_range_to_array(xr_current_coro(iso), rng);
}

static XrValue m_range_contains(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrRange *rng = xr_value_get_range_body(iso, self);
    if (!rng)
        return xr_bool(false);
    if (argc >= 1 && XR_IS_INT(args[0])) {
        return xr_bool(xr_range_contains(rng, XR_TO_INT(args[0])));
    }
    return xr_bool(false);
}

/* ========== Native Body Descriptor ========== */

static void range_body_init(XrInstance *inst, void *body) {
    (void) inst;
    XrRange *r = (XrRange *) body;
    r->start = 0;
    r->end = 0;
    r->step = 1;
}

static XrNativeBodyDesc g_range_body_desc = {
    .body_size = sizeof(XrRange),
    .body_align = _Alignof(int64_t),
    .copy_policy = XR_NATIVE_BODY_COPY_DEEP,
    .init = range_body_init,
    .destroy = NULL,
    .traverse = NULL,
    .deep_copy = NULL, /* memcpy suffices (pure values, no pointers) */
    .to_shared = NULL, /* memcpy suffices */
};

/* ========== Class Registration ========== */

void xr_register_range_class(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "register_range_class: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XR_DCHECK(core != NULL, "register_range_class: core not initialised");
    XR_DCHECK(core->objectClass != NULL, "register_range_class: Object not registered");
    XR_DCHECK(core->rangeClass == NULL, "register_range_class: already registered");

    XrClassBuilder *builder = xr_class_builder_new(X, "Range", core->objectClass);
    XR_CHECK(builder != NULL, "register_range_class: builder alloc failed");

    xr_class_builder_set_native_body(builder, &g_range_body_desc);

    xr_class_builder_add_method(builder, "toString", m_range_to_string, 0, 0);
    xr_class_builder_add_method(builder, "toArray", m_range_to_array, 0, 0);
    xr_class_builder_add_method(builder, "contains", m_range_contains, 1, 0);

    XrClass *cls = xr_class_builder_finalize(builder);
    XR_CHECK(cls != NULL, "register_range_class: finalize failed");
    cls->flags |= XR_CLASS_BUILTIN | XR_CLASS_HAS_NATIVE_BODY;
    cls->builtin_kind = XR_BK_RANGE;

    core->rangeClass = cls;
}
