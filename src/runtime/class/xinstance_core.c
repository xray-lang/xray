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
#include "../core/xr_exec_context.h"
#include "../core/xr_runtime_core.h"
#include "../mem/xalloc_unified.h"
#include "../mem/xheap.h"
#include "../object/xarray.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"
#include "../value/xstruct_layout.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"

#include <string.h>

XrInstance *xr_instance_new(XrVMRuntime *X, XrClass *cls) {
    XR_DCHECK(cls != NULL, "Class must not be NULL");
    XR_DCHECK(X != NULL, "Runtime must not be NULL");

    const char *class_name = cls->name ? cls->name : "<unnamed>";

    size_t size = xr_instance_size(cls);
    XrAllocationContext *alloc = xr_alloc_context_current();
    XrRuntimeCore *core = xr_isolate_get_runtime_core(X);
    XrInstance *inst = alloc && alloc->core == core
                           ? (XrInstance *) xr_alloc_context_new_object(alloc, size, XR_TINSTANCE)
                           : NULL;

    if (!inst) {
        xr_log_warning("instance", "failed to allocate instance of class %s", class_name);
        return NULL;
    }

    xr_obj_header_init_type(&inst->hdr, XR_TINSTANCE);
    xr_instance_init_inplace(inst, cls);

    if (cls->flags & XR_CLASS_CYCLE_CANDIDATE) {
    }

    return inst;
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

    XrNativeBodyDesc *desc = cls->native_body;
    if (desc) {
        void *body = xr_instance_native_body(inst);
        XR_DCHECK(body != NULL, "native body pointer must not be NULL");
        if (desc->init) {
            desc->init(inst, body);
        } else {
            memset(body, 0, desc->body_size);
        }
    }
}

XrValue xr_instance_get_field_by_index(XrInstance *inst, int index) {
    XR_DCHECK(inst != NULL, "Instance must not be NULL");
    XrClass *klass = xr_instance_get_class(inst);
    XR_DCHECK_BOUNDS(index, klass->field_count, "field index out of bounds");
    return inst->fields[index];
}

void xr_instance_set_field_by_index(XrInstance *inst, int index, XrValue value) {
    XR_DCHECK(inst != NULL, "Instance must not be NULL");
    XrClass *klass = xr_instance_get_class(inst);
    XR_DCHECK_BOUNDS(index, klass->field_count, "field index out of bounds");
    xr_rc_release_value(xr_current_coro_heap(), inst->fields[index]);
    inst->fields[index] = value;
}

static bool xr_instance_write_decoded_native_field(uint8_t *dst,
                                                   const XrAggregateFieldLayout *field,
                                                   XrValue value);

static bool xr_instance_write_decoded_array(uint8_t *dst, const XrAggregateFieldLayout *field,
                                            XrValue value) {
    if (!dst || !field || field->elem_count == 0)
        return false;
    if (XR_IS_ARRAY_REF(value)) {
        memcpy(dst, value.ptr, field->size);
        return true;
    }
    if (!XR_IS_ARRAY(value))
        return false;
    uint8_t elem_size = xr_native_type_size(xr_target_data_layout_host(), field->elem_native_type);
    if (elem_size == 0)
        return false;
    XrArray *array = XR_TO_ARRAY(value);
    int count = array->length < field->elem_count ? array->length : field->elem_count;
    XrAggregateFieldLayout elem = {.native_type = field->elem_native_type};
    for (int i = 0; i < count; i++) {
        if (!xr_instance_write_decoded_native_field(dst + (size_t) i * elem_size, &elem,
                                                    xr_array_get(array, i)))
            return false;
    }
    if (count < field->elem_count)
        memset(dst + (size_t) count * elem_size, 0,
               (size_t) (field->elem_count - count) * elem_size);
    return true;
}

static bool xr_instance_write_decoded_native_field(uint8_t *dst,
                                                   const XrAggregateFieldLayout *field,
                                                   XrValue value) {
    if (!dst || !field || field->is_flexible)
        return false;
#define XR_STORE_NATIVE(type_, expression_)                                                        \
    do {                                                                                           \
        type_ native_value = (expression_);                                                        \
        memcpy(dst, &native_value, sizeof(native_value));                                          \
        return true;                                                                               \
    } while (0)
    switch (field->native_type) {
        case XR_NATIVE_I64:
            XR_STORE_NATIVE(int64_t, XR_TO_INT(value));
        case XR_NATIVE_U64:
            XR_STORE_NATIVE(uint64_t, (uint64_t) XR_TO_INT(value));
        case XR_NATIVE_ISIZE:
            XR_STORE_NATIVE(ptrdiff_t, (ptrdiff_t) XR_TO_INT(value));
        case XR_NATIVE_USIZE:
            XR_STORE_NATIVE(size_t, (size_t) XR_TO_INT(value));
        case XR_NATIVE_I32:
            XR_STORE_NATIVE(int32_t, (int32_t) XR_TO_INT(value));
        case XR_NATIVE_U32:
            XR_STORE_NATIVE(uint32_t, (uint32_t) XR_TO_INT(value));
        case XR_NATIVE_I16:
            XR_STORE_NATIVE(int16_t, (int16_t) XR_TO_INT(value));
        case XR_NATIVE_U16:
            XR_STORE_NATIVE(uint16_t, (uint16_t) XR_TO_INT(value));
        case XR_NATIVE_I8:
            XR_STORE_NATIVE(int8_t, (int8_t) XR_TO_INT(value));
        case XR_NATIVE_U8:
            XR_STORE_NATIVE(uint8_t, (uint8_t) XR_TO_INT(value));
        case XR_NATIVE_F64:
            XR_STORE_NATIVE(double, XR_TO_FLOAT(value));
        case XR_NATIVE_F32:
            XR_STORE_NATIVE(float, (float) XR_TO_FLOAT(value));
        case XR_NATIVE_BOOL:
            XR_STORE_NATIVE(uint8_t, XR_TO_BOOL(value) ? 1u : 0u);
        case XR_NATIVE_STRING:
            XR_STORE_NATIVE(XrString *, XR_IS_NULL(value) ? NULL : XR_TO_STRING(value));
        case XR_NATIVE_ARRAY_REF:
        case XR_NATIVE_MAP_REF:
        case XR_NATIVE_SET_REF:
        case XR_NATIVE_VALUE:
            memcpy(dst, &value, sizeof(value));
            return true;
        case XR_NATIVE_NESTED_AGGREGATE: {
            if (!XR_IS_INSTANCE(value))
                return false;
            XrInstance *nested = XR_TO_INSTANCE(value);
            void *body = nested && nested->klass ? xr_instance_native_body(nested) : NULL;
            if (!body || !nested->klass->struct_layout ||
                nested->klass->struct_layout->total_size != field->size)
                return false;
            memcpy(dst, body, field->size);
            return true;
        }
        case XR_NATIVE_ARRAY:
            return xr_instance_write_decoded_array(dst, field, value);
        default:
            return false;
    }
#undef XR_STORE_NATIVE
}

/* Installed by the VM; NULL in backends that never materialize struct
 * instances with native bodies. */
static XrInstanceStructFieldReadHook g_struct_field_read_hook;

void xr_instance_set_struct_field_read_hook(XrInstanceStructFieldReadHook hook) {
    g_struct_field_read_hook = hook;
}

XrValue xr_instance_load_field(struct XrVMRuntime *X, XrInstance *inst, int index) {
    if (inst && inst->klass && inst->klass->struct_layout && g_struct_field_read_hook) {
        XrValue out;
        if (g_struct_field_read_hook(X, inst, index, &out))
            return out;
    }
    return xr_instance_get_field_fast(inst, index);
}

bool xr_instance_set_decoded_field(XrInstance *inst, int index, XrValue value) {
    if (!inst || !inst->klass || index < 0 || index >= xr_class_instance_field_count(inst->klass))
        return false;
    XrAggregateLayout *layout = inst->klass->struct_layout;
    if (!layout) {
        xr_instance_set_field_by_index(inst, index, value);
        return true;
    }
    if (index >= layout->field_count)
        return false;
    uint8_t *body = (uint8_t *) xr_instance_native_body(inst);
    XrAggregateFieldLayout *field = &layout->fields[index];
    if (!body || !xr_instance_write_decoded_native_field(body + field->offset, field, value))
        return false;
    xr_rc_release_value(xr_current_coro_heap(), inst->fields[index]);
    inst->fields[index] = value;
    return true;
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

XrInstance *xr_instance_clone(XrVMRuntime *X, XrInstance *src) {
    XR_DCHECK(src != NULL, "instance_clone: NULL src");
    XrClass *cls = src->klass;
    XR_DCHECK(cls != NULL, "instance_clone: NULL klass");

    uint32_t field_count = xr_class_instance_field_count(cls);
    size_t size = xr_instance_size(cls);
    XrAllocationContext *alloc = xr_alloc_context_current();
    XrRuntimeCore *core = xr_isolate_get_runtime_core(X);
    XrInstance *dst = alloc && alloc->core == core
                          ? (XrInstance *) xr_alloc_context_new_object(alloc, size, XR_TINSTANCE)
                          : NULL;
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
