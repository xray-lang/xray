/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xinstance_core.c - VM-neutral instance layout helpers
 */

#include "xinstance.h"
#include "../core/xr_runtime_core.h"
#include "../mem/xalloc_unified.h"
#include "../mem/xheap.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"

#include <string.h>

XrInstance *xr_instance_new_core(XrRuntimeCore *core, XrCoroutine *coro, XrClass *cls) {
    XR_DCHECK(cls != NULL, "Class must not be NULL");

    const char *class_name = cls->name ? cls->name : "<unnamed>";

    size_t size = xr_instance_size(cls);
    XrInstance *inst = NULL;
    if (coro) {
        inst = (XrInstance *) xr_alloc(coro, size, XR_TINSTANCE);
    } else if (core) {
        inst = (XrInstance *) xr_fixed_heap_alloc(&core->fixed_heap, size, XR_TINSTANCE);
    }

    if (!inst) {
        xr_log_warning("instance", "failed to allocate instance of class %s", class_name);
        return NULL;
    }

    xr_obj_header_init_type(&inst->hdr, XR_TINSTANCE);
    xr_instance_init_inplace(inst, cls);

    if (cls->flags & XR_CLASS_CYCLE_CANDIDATE) {
        XR_OBJ_SET_FLAG(&inst->hdr, XR_OBJ_CYCLE_CANDIDATE);
        inst->hdr._rsv = XR_CYCLE_NOT_IN_ROOTS;
    }

    XrNativeBodyDesc *desc = cls->native_body;
    if (desc && desc->init) {
        void *body = xr_instance_native_body(inst);
        XR_DCHECK(body != NULL, "native body pointer must not be NULL");
        desc->init(inst, body);
    }

    return inst;
}

XrInstance *xr_instance_new(XrayIsolate *X, XrClass *cls) {
    return xr_instance_new_core(xr_isolate_get_runtime_core(X), xr_current_coro(X), cls);
}

void xr_instance_init_inplace(XrInstance *inst, XrClass *cls) {
    if (!inst || !cls)
        return;

    inst->klass = cls;

    uint32_t slot_count;
    if (cls->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        slot_count = cls->in_object_capacity;
    } else {
        slot_count = xr_class_instance_field_count(cls);
    }

    if (cls->field_default_values && !(cls->flags & XR_CLASS_DYNAMIC_LAYOUT)) {
        for (uint32_t i = 0; i < slot_count; i++) {
            inst->fields[i] = cls->field_default_values[i];
        }
    } else {
        for (uint32_t i = 0; i < slot_count; i++) {
            inst->fields[i] = xr_null();
        }
    }
}

size_t xr_instance_size(XrClass *cls) {
    XR_DCHECK(cls != NULL, "instance_size: NULL cls");

    uint32_t slot_count;
    if (cls->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        slot_count = cls->in_object_capacity;
    } else {
        slot_count = xr_class_instance_field_count(cls);
    }

    size_t size = sizeof(XrInstance) + sizeof(XrValue) * slot_count;
    XrNativeBodyDesc *desc = cls->native_body;
    if (desc) {
        size_t align = desc->body_align ? (size_t) desc->body_align : sizeof(void *);
        size = (size + align - 1) & ~(align - 1);
        size += desc->body_size;
    }
    return size;
}

void xr_instance_free(XrInstance *inst) {
    (void) inst;
}

XrInstance *xr_instance_clone(XrayIsolate *X, XrInstance *src) {
    XR_DCHECK(src != NULL, "instance_clone: NULL src");
    XrClass *cls = src->klass;
    XR_DCHECK(cls != NULL, "instance_clone: NULL klass");

    uint32_t field_count = xr_class_instance_field_count(cls);
    size_t size = xr_instance_size(cls);
    XrInstance *dst = NULL;
    XrCoroutine *coro = xr_current_coro(X);
    if (coro) {
        dst = (XrInstance *) xr_alloc(coro, size, XR_TINSTANCE);
    } else {
        dst = (XrInstance *) xr_fixed_heap_alloc(xr_isolate_get_fixed_heap(X), size, XR_TINSTANCE);
    }
    if (!dst)
        return NULL;

    xr_obj_header_init_type(&dst->hdr, XR_TINSTANCE);
    xr_instance_init_inplace(dst, cls);
    if (cls->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        uint16_t cap = cls->in_object_capacity;
        if (cap > 0) {
            uint32_t in_object_count = field_count < cap - 1 ? field_count : cap - 1;
            for (uint32_t i = 0; i < in_object_count; i++) {
                dst->fields[i] = src->fields[i];
                xr_rc_retain_value(dst->fields[i]);
            }
            if (field_count > cap - 1) {
                XrValue *src_overflow = (XrValue *) src->fields[cap - 1].ptr;
                if (src_overflow) {
                    uint32_t overflow_count = field_count - (cap - 1);
                    xr_instance_set_dynamic_field(X, dst, cap - 1, xr_null());
                    for (uint32_t i = 0; i < overflow_count; i++) {
                        XrValue value = src_overflow[i];
                        xr_rc_retain_value(value);
                        xr_instance_set_dynamic_field(X, dst, (uint16_t) ((cap - 1) + i), value);
                    }
                }
            }
        }
    } else {
        memcpy(dst->fields, src->fields, sizeof(XrValue) * field_count);
        for (uint32_t i = 0; i < field_count; i++)
            xr_rc_retain_value(dst->fields[i]);
    }

    XrNativeBodyDesc *desc = cls->native_body;
    if (desc) {
        void *src_body = xr_instance_native_body(src);
        void *dst_body = xr_instance_native_body(dst);
        memcpy(dst_body, src_body, desc->body_size);
    }
    return dst;
}
