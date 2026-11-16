/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_values.c - Stable fixed-value owners across synchronous recovery
 *
 * KEY CONCEPT:
 *   Publish each context owner before the next fallible operation. The enclosing
 *   execution drains the same storage on normal return and longjmp recovery.
 */
#include "xnative_values.h"
#include "xray_native_values.h"
#include "xray_native_runtime.h"
#include "xnative_string_backing.h"
#include "xnative_string_backing_ops.h"
#include "xnative_array_ops.h"
#include "xnative_map_backing.h"
#include "xnative_status.h"
#include "xnative_capture_backing.h"
#include "xnative_module_state.h"
#include "base/xchecks.h"
#include <string.h>

typedef struct NativeEnvironmentImage {
    struct NativeEnvironmentImage *next;
    const XrtValueImage *source;
    XrtManagedLease code;
    XrtValueImagePlans *plans;
    bool code_owned;
} NativeEnvironmentImage;
typedef struct NativeEnvironmentType {
    struct NativeEnvironmentType *next;
    const XrtValuePlan *plan;
    XrtCaptureType *capture;
} NativeEnvironmentType;
struct XrtNativeValues {
    XrtBillingOwner *billing;
    XrtManagedRoot *root;
    XrtBackingTypes backings;
    NativeEnvironmentImage *images;
    NativeEnvironmentType *environments;
};
static XR_THREAD_LOCAL XrtValueBinding value_binding;
XR_FUNC XrtValueBinding xrt_value_root_bind(XrtValueBinding binding) {
    XrtValueBinding previous = value_binding;
    value_binding = binding;
    return previous;
}
XR_FUNC XrtValueRoot *xrt_value_root_active(void) { return value_binding.root; }
XR_FUNC XrtValueRoot *xrt_value_root_current(uint32_t source_id) {
    XR_CHECK(value_binding.root && source_id <= value_binding.sources,
        "value operations require an active execution and its source id");
    return value_binding.root;
}
XR_FUNC void xrt_value_root_drain(XrtValueRoot *owner) {
    if (!owner || owner->draining) return;
    uint64_t limit = owner->byte_limit;
    owner->draining = true;
    xrt_native_values_drain(owner);
    *owner = (XrtValueRoot){.byte_limit=limit};
}

XRAY_API_NORET void xrt_native_managed_failure(XrtManagedStatus status, uint32_t source_id) {
    XR_CHECK(status != XRT_MANAGED_OK, "managed failure preserves a real failing kernel result");
    xrt_value_failure(xrt_managed_value_status(status), source_id);
}

XR_FUNC XrtManagedStatus xrt_native_values_prepare(XrtValueRoot *owner, XrtBillingOwner **output) {
    if (!owner || owner->draining || !output || *output) return XRT_MANAGED_ARGUMENT;
    if (!owner->managed) {
        XrtBillingOwner *billing = NULL;
        size_t limit = SIZE_MAX;
        if (owner->byte_limit) {
#if SIZE_MAX < UINT64_MAX
            if (owner->byte_limit > SIZE_MAX) return XRT_MANAGED_LIMIT;
#endif
            limit = (size_t)owner->byte_limit;
        }
        XrtManagedStatus status = xrt_billing_owner_new(NULL, limit, &billing);
        if (status != XRT_MANAGED_OK) return status;
        void *memory = NULL;
        status = xrt_managed_metadata_allocate(billing, sizeof(struct XrtNativeValues), &memory);
        if (status != XRT_MANAGED_OK) {
            xrt_billing_owner_drop(billing);
            return status;
        }
        memset(memory, 0, sizeof(struct XrtNativeValues));
        struct XrtNativeValues *values = memory;
        values->billing = billing;
        owner->managed = values;
    }
    *output = owner->managed->billing;
    return XRT_MANAGED_OK;
}

XR_FUNC bool xrt_native_values_prepared(const XrtValueRoot *owner) {
    return owner && xrt_value_root_storage_empty(owner) &&
        (!owner->managed || (!owner->managed->root && !owner->managed->backings.string &&
            !owner->managed->images && !owner->managed->environments));
}
XR_FUNC XrtManagedStatus xrt_native_values_backings(XrtValueRoot *owner, const XrtBackingTypes **output) {
    if (!output || *output) return XRT_MANAGED_ARGUMENT;
    XrtBillingOwner *billing = NULL;
    XrtManagedStatus status = xrt_native_values_prepare(owner,&billing);
    if (status == XRT_MANAGED_OK && !owner->managed->backings.string)
        status = xrt_backing_types_init(billing,&owner->managed->backings);
    if (status == XRT_MANAGED_OK) *output = &owner->managed->backings;
    return status;
}

static struct XrtNativeValues *native_values(uint32_t source_id) {
    XrtValueRoot *owner = xrt_value_root_current(source_id);
    XrtBillingOwner *billing = NULL;
    XrtManagedStatus status = xrt_native_values_prepare(owner, &billing);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    struct XrtNativeValues *values = owner->managed;
    if (!values->root) {
        status = xrt_managed_root_new(billing, &values->root);
        if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    }
    return values;
}

XRAY_API XrtManagedRoot *xrt_native_managed_root(uint32_t source_id) {
    return native_values(source_id)->root;
}

static XrtManagedStatus native_capture_plan(struct XrtNativeValues *values,
    const XrtValueImage *source, XrtModuleState *state, XrtManagedLease code,
    uint32_t type_id, const XrtValuePlan **output) {
    if (state) return xrt_module_state_plan(state,type_id,output);
    if (!source || (code.context ? !code.retain || !code.drop : code.retain || code.drop)) return XRT_MANAGED_ARGUMENT;
    NativeEnvironmentImage *image = values->images;
    while (image && (image->source != source || image->code.context != code.context ||
        image->code.retain != code.retain || image->code.drop != code.drop)) image = image->next;
    if (!image) {
        void *memory = NULL;
        XrtManagedStatus status = xrt_managed_metadata_allocate(values->billing,sizeof(*image),&memory);
        if (status != XRT_MANAGED_OK) return status;
        image = memory;
        *image = (NativeEnvironmentImage){.next=values->images,.source=source,.code=code};
        values->images = image;
        /* A Cell-only image need not leave any Code lease in its plans. Keep
         * the cache's raw identity alive independently until this node retires. */
        if (code.context) {
            if (!code.retain(code.context)) return XRT_MANAGED_REFCOUNT;
            image->code_owned = true;
        }
        status = xrt_value_image_bind(values->billing,source,code,&image->plans);
        if (status != XRT_MANAGED_OK) return status;
    }
    return xrt_value_image_get(image->plans,type_id,output);
}
static XrtManagedStatus native_environment_type(struct XrtNativeValues *values,
    const XrtValuePlan *plan, const XrtCaptureType **output) {
    NativeEnvironmentType *type = values->environments;
    while (type && type->plan != plan) type = type->next;
    if (!type) {
        void *memory = NULL;
        XrtManagedStatus status = xrt_managed_metadata_allocate(values->billing,sizeof(*type),&memory);
        if (status != XRT_MANAGED_OK) return status;
        type = memory;
        *type = (NativeEnvironmentType){.next=values->environments,.plan=plan};
        values->environments = type;
        status = xrt_capture_type_new(values->billing,plan,&type->capture);
        if (status != XRT_MANAGED_OK) return status;
    }
    *output = type->capture;
    return XRT_MANAGED_OK;
}
XRAY_API XrtFunction xrt_native_environment_create(XrtManagedFrame *frame, size_t slot,
    const XrtNativeEnvironmentInput *input, uint32_t source_id) {
    if (!input || !input->entry || !input->type)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT,source_id);
    struct XrtNativeValues *values = native_values(source_id);
    const XrtValuePlan *plan = NULL;
    XrtManagedStatus status = native_capture_plan(values,input->image,input->state,input->code,input->type,&plan);
    XrtValuePlanSpec shape = {0};
    if (status == XRT_MANAGED_OK) status = xrt_value_plan_view(plan,&shape);
    if (status == XRT_MANAGED_OK && shape.kind != XRT_VALUE_ENVIRONMENT) status = XRT_MANAGED_ARGUMENT;
    const XrtCaptureType *type = NULL;
    if (status == XRT_MANAGED_OK) status = native_environment_type(values,plan,&type);
    if (status == XRT_MANAGED_OK) status = xrt_environment_create(frame,slot,type,input->captures);
    XrtManagedHeader *owner = NULL;
    if (status == XRT_MANAGED_OK) status = xrt_managed_slot_borrow(frame,slot,&owner);
    XrtEnvironmentBorrow view = {0};
    if (status == XRT_MANAGED_OK) status = xrt_environment_borrow(type,owner,&view);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
    return (XrtFunction){input->entry,(uintptr_t)view.payload,owner};
}
XRAY_API void xrt_native_cell_create(XrtManagedFrame *frame, size_t slot,
    const XrtNativeCellInput *input, uint32_t source_id) {
    if (!input || !input->type) xrt_native_managed_failure(XRT_MANAGED_ARGUMENT,source_id);
    const XrtManagedHeader *previous = NULL;
    XrtManagedStatus status = xrt_place_borrow(
        (XrtPlace){.frame=frame,.slot=slot,.nulls=XRT_PLACE_NULLABLE},&previous);
    if (status != XRT_MANAGED_OK || previous)
        xrt_native_managed_failure(status != XRT_MANAGED_OK ? status : XRT_MANAGED_ARGUMENT,source_id);
    struct XrtNativeValues *values = native_values(source_id);
    const XrtValuePlan *plan = NULL;
    status = native_capture_plan(values,input->image,input->state,input->code,input->type,&plan);
    XrtValuePlanSpec shape = {0}, payload = {0};
    if (status == XRT_MANAGED_OK) status = xrt_value_plan_view(plan,&shape);
    if (status == XRT_MANAGED_OK && shape.kind != XRT_VALUE_CELL) status = XRT_MANAGED_ARGUMENT;
    if (status == XRT_MANAGED_OK) status = xrt_value_plan_view(shape.element,&payload);
    if (status == XRT_MANAGED_OK && payload.kind != XRT_VALUE_I64 && payload.kind != XRT_VALUE_MAP_STRING_I64)
        status = XRT_MANAGED_ARGUMENT;
    const XrtCaptureType *type = NULL;
    if (status == XRT_MANAGED_OK) status = native_environment_type(values,plan,&type);
    if (status == XRT_MANAGED_OK) status = payload.kind == XRT_VALUE_I64 ?
        xrt_i64_cell_create(frame,slot,type,input->initial.i64) :
        xrt_map_cell_create(frame,slot,type,(const XrtMapBacking *)input->initial.map);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API int64_t xrt_native_cell_i64_read(const XrtManagedHeader *cell, uint32_t source_id) {
    int64_t value = 0;
    XrtManagedStatus status = xrt_i64_cell_read((const XrtI64Cell *)cell,&value);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
    return value;
}
XRAY_API void xrt_native_cell_i64_write(XrtManagedHeader *cell, int64_t value, uint32_t source_id) {
    XrtManagedStatus status = xrt_i64_cell_write_exclusive((XrtI64Cell *)cell,value);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_cell_map_read_copy(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *cell, uint32_t source_id) {
    XrtManagedStatus status = xrt_map_cell_read_copy(frame,slot,(const XrtMapCell *)cell);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_cell_map_write(XrtManagedHeader *cell, const XrtManagedHeader *map, uint32_t source_id) {
    XrtManagedStatus status = xrt_map_cell_write_exclusive((XrtMapCell *)cell,(const XrtMapBacking *)map);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_cell_map_update(XrtManagedHeader *cell, XrtManagedFrame *frame, size_t workspace,
    const XrtMapUpdate *update, bool *removed, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status == XRT_MANAGED_OK)
        status = xrt_map_cell_update_exclusive((XrtMapCell *)cell,frame,workspace,types,update,removed);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API XrtExternalOwner xrt_native_cell_map_binding(XrtManagedHeader *cell, uint32_t source_id) {
    XrtExternalOwner binding = {0};
    XrtManagedStatus status = xrt_map_cell_external((XrtMapCell *)cell,&binding);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
    return binding;
}

XRAY_API void xrt_native_string_create(XrtManagedFrame *frame, size_t slot,
    const char *bytes, size_t length, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status == XRT_MANAGED_OK)
        status = xrt_string_backing_create(frame, slot, types, bytes, length, 0);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

XRAY_API void xrt_native_string_i64_create(XrtManagedFrame *frame, size_t slot,
    int64_t value, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status==XRT_MANAGED_OK) status=xrt_string_backing_i64_create(frame,slot,types,value);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_string_f64_create(XrtManagedFrame *frame, size_t slot,
    uint64_t bits, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK) status = xrt_string_backing_f64_create(frame, slot, types, bits);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

XRAY_API void xrt_native_string_concat_create(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *left, const XrtManagedHeader *right, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status == XRT_MANAGED_OK)
        status = xrt_string_backing_concat_create(frame, slot, types,
            (const XrtStringBacking *)left, (const XrtStringBacking *)right, 0);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API void xrt_native_string_append_owned(XrtManagedFrame *frame,
    const XrtStringAppendOwned *input, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK) status = xrt_string_backing_append_owned(frame, types, input);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

XRAY_API void xrt_native_array_create(XrtManagedFrame *frame, size_t slot,
    const XrtArrayInput *input, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = input ? xrt_native_values_backings(xrt_value_root_current(source_id),&types) : XRT_MANAGED_ARGUMENT;
    if (status == XRT_MANAGED_OK) {
        switch (input->kind) {
        case XRT_BACKING_ARRAY_I64: status=xrt_array_backing_i64_create(frame,slot,types,input->values,input->count,input->count); break;
        case XRT_BACKING_ARRAY_STRING: status=xrt_array_backing_string_create(frame,slot,types,input->values,input->count,input->count); break;
        case XRT_BACKING_ARRAY_U8: status=xrt_array_backing_u8_create(frame,slot,types,input->values,input->count,input->count); break;
        case XRT_BACKING_ARRAY_U64: status=xrt_array_backing_u64_create(frame,slot,types,input->values,input->count,input->count); break;
        default: status=XRT_MANAGED_ARGUMENT; break;
        }
    }
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_array_repeat(XrtManagedFrame *frame, size_t slot,
    const XrtArrayItem *item, int64_t length, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status==XRT_MANAGED_OK) status=xrt_array_backing_repeat_create(frame,slot,types,item,length);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_array_get(const XrtManagedHeader *array, XrtBackingArrayKind kind,
    int64_t index, XrtArrayItem *output, uint32_t source_id) {
    XrtManagedStatus status=xrt_array_backing_get((const XrtArrayBacking *)array,kind,index,output);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_array_display(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *array, XrtBackingArrayKind kind, uint32_t source_id) {
    const XrtBackingTypes *types=NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status==XRT_MANAGED_OK) status=xrt_array_backing_display_create(frame,slot,types,(const XrtArrayBacking *)array,kind);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_array_update_place(XrtPlace binding, XrtBackingArrayKind kind,
    XrtManagedFrame *frame, size_t workspace, const XrtArrayUpdate *update, uint32_t source_id) {
    const XrtBackingTypes *types=NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status==XRT_MANAGED_OK) status=xrt_array_update_place(binding,kind,frame,workspace,types,update);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}

XRAY_API void xrt_native_map_create(XrtManagedFrame *frame, size_t slot,
    const XrtMapInput *input, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = input ? xrt_native_values_backings(xrt_value_root_current(source_id), &types) : XRT_MANAGED_ARGUMENT;
    if (status == XRT_MANAGED_OK)
        status = xrt_map_backing_create(frame, slot, types, input->keys, input->values, input->count);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API bool xrt_native_map_get(const XrtManagedHeader *map, const XrtStringBacking *key,
    int64_t *value, uint32_t source_id) {
    bool found = false;
    XrtManagedStatus status = xrt_map_backing_get((const XrtMapBacking *)map, key, value, &found);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    return found;
}
XRAY_API int64_t xrt_native_map_index(const XrtManagedHeader *map, const XrtStringBacking *key, uint32_t source_id) {
    int64_t value = 0;
    if (!xrt_native_map_get(map, key, &value, source_id)) xrt_typed_panic(431, source_id);
    return value;
}
XRAY_API void xrt_native_map_update_place(XrtPlace binding, XrtManagedFrame *frame, size_t workspace,
    const XrtMapUpdate *update, bool *removed, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK) status = xrt_map_update_place(binding, frame, workspace, types, update, removed);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API void xrt_native_pod_display_create(XrtManagedFrame *frame, size_t slot,
    const XrtPodDisplay *description, const void *bytes, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK) status = xrt_string_backing_pod_display_create(frame, slot, types, description, bytes);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

XR_FUNC void xrt_native_values_drain(XrtValueRoot *owner) {
    struct XrtNativeValues *values = owner->managed;
    if (!values) return;
    XrtManagedStatus status = xrt_managed_root_destroy(&values->root);
    XR_CHECK(status == XRT_MANAGED_OK, "execution retirement runs outside every managed mutation");
    while (values->environments) {
        NativeEnvironmentType *type = values->environments;
        values->environments = type->next;
        xrt_capture_type_drop(&type->capture);
        xrt_managed_metadata_free(values->billing,type,sizeof(*type));
    }
    while (values->images) {
        NativeEnvironmentImage *image = values->images;
        values->images = image->next;
        XrtManagedLease code = image->code;
        bool code_owned = image->code_owned;
        xrt_value_image_drop(&image->plans);
        xrt_managed_metadata_free(values->billing,image,sizeof(*image));
        if (code_owned) code.drop(code.context);
    }
    xrt_backing_types_drop(&values->backings);
    XrtBillingOwner *billing = values->billing;
    owner->managed = NULL;
    xrt_managed_metadata_free(billing, values, sizeof(*values));
    xrt_billing_owner_drop(billing);
}
