/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xinstance_destroy.c - Instance object destroy cleanup
 */

#include "xinstance.h"

#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../gc/xcoro_heap.h"

void xr_obj_destroy_instance(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    if (!obj)
        return;
    XrInstance *inst = (XrInstance *) obj;
    XrClass *klass = inst->klass;
    if (!klass)
        return;

    if (klass->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        uint16_t cap = klass->in_object_capacity;
        if (cap > 0) {
            uint32_t field_count = xr_class_instance_field_count(klass);
            uint32_t in_object_count = field_count < cap - 1 ? field_count : cap - 1;
            for (uint32_t i = 0; i < in_object_count; i++)
                xr_rc_release_value(owner_heap, inst->fields[i]);
            if (field_count > cap - 1) {
                XrValue *overflow = (XrValue *) inst->fields[cap - 1].ptr;
                if (overflow) {
                    uint32_t overflow_count = field_count - (cap - 1);
                    for (uint32_t i = 0; i < overflow_count; i++)
                        xr_rc_release_value(owner_heap, overflow[i]);
                    int64_t *raw = ((int64_t *) overflow) - 1;
                    xr_free(raw);
                }
            }
        }
    } else {
        uint32_t field_count = xr_class_instance_field_count(klass);
        for (uint32_t i = 0; i < field_count; i++)
            xr_rc_release_value(owner_heap, inst->fields[i]);
    }

    XrNativeBodyDesc *desc = klass->native_body;
    if (desc && desc->destroy) {
        void *body = xr_instance_native_body(inst);
        XR_DCHECK(body != NULL, "destroy: native body NULL but desc present");
        desc->destroy(body);
    }
}
