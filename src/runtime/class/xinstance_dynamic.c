/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xinstance_dynamic.c - Dynamic-layout instance field storage
 */

#include "xinstance.h"

#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../gc/xalloc_unified.h"

XrValue xr_instance_get_dynamic_field(XrInstance *inst, uint16_t index) {
    XR_DCHECK(inst != NULL, "get_dynamic_field: NULL inst");
    XrClass *klass = inst->klass;
    XR_DCHECK(klass->flags & XR_CLASS_DYNAMIC_LAYOUT, "get_dynamic_field: not dynamic");
    uint16_t cap = klass->in_object_capacity;
    XR_DCHECK(cap > 0, "get_dynamic_field: zero capacity");

    if (index < cap - 1)
        return inst->fields[index];

    XrValue *overflow = (XrValue *) inst->fields[cap - 1].ptr;
    if (!overflow)
        return xr_null();
    uint16_t overflow_idx = index - (cap - 1);
    return overflow[overflow_idx];
}

bool xr_instance_set_dynamic_field_direct(XrInstance *inst, uint16_t index, XrValue value) {
    XR_DCHECK(inst != NULL, "set_dynamic_field: NULL inst");
    XrClass *klass = inst->klass;
    XR_DCHECK(klass->flags & XR_CLASS_DYNAMIC_LAYOUT, "set_dynamic_field: not dynamic");
    uint16_t cap = klass->in_object_capacity;
    XR_DCHECK(cap > 0, "set_dynamic_field: zero capacity");

    if (index < cap - 1) {
        xr_rc_release_value(xr_current_coro_gc(), inst->fields[index]);
        inst->fields[index] = value;
        return true;
    }

    uint16_t overflow_idx = index - (cap - 1);
    XrValue *overflow = (XrValue *) inst->fields[cap - 1].ptr;
    uint16_t overflow_cap = 0;
    if (overflow)
        overflow_cap = (uint16_t) ((int64_t *) overflow)[-1];

    if (overflow_idx >= overflow_cap) {
        uint16_t new_cap = overflow_cap == 0 ? 8 : overflow_cap * 2;
        while (new_cap <= overflow_idx)
            new_cap *= 2;

        size_t alloc_size = sizeof(int64_t) + sizeof(XrValue) * new_cap;
        int64_t *raw = (int64_t *) xr_malloc(alloc_size);
        if (!raw)
            return false;
        raw[0] = (int64_t) new_cap;
        XrValue *new_overflow = (XrValue *) (raw + 1);
        for (uint16_t i = 0; i < overflow_cap; i++)
            new_overflow[i] = overflow[i];
        for (uint16_t i = overflow_cap; i < new_cap; i++)
            new_overflow[i] = xr_null();
        if (overflow) {
            int64_t *old_raw = ((int64_t *) overflow) - 1;
            xr_free(old_raw);
        }
        overflow = new_overflow;
        inst->fields[cap - 1].ptr = (void *) overflow;
    }

    xr_rc_release_value(xr_current_coro_gc(), overflow[overflow_idx]);
    overflow[overflow_idx] = value;
    return true;
}

bool xr_instance_set_dynamic_field(XrayIsolate *X, XrInstance *inst, uint16_t index,
                                   XrValue value) {
    XR_DCHECK(X != NULL, "set_dynamic_field: NULL isolate");
    (void) X;
    return xr_instance_set_dynamic_field_direct(inst, index, value);
}
