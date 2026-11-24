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
#include "xnative_class_array.h"
#include "xnative_host_environment.h"
#include "xnative_host_file.h"
#include "xnative_host_stat.h"
#include "xnative_host_io.h"
#include "shared/xerror_codes.h"
#include "shared/xnative_host_leaf.h"
#include "xnative_map_backing.h"
#include "xnative_set_backing.h"
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
    const XrtModuleState *state;
    const XrtValueImage *state_image;
    XrOsFileMetadataOwner metadata;
    bool retired;
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
XRAY_API_NORET void xrt_native_managed_failure(XrtManagedStatus status, uint32_t source_id) {
    XR_CHECK(status != XRT_MANAGED_OK, "managed failure preserves a real failing kernel result");
    xrt_value_failure(xrt_managed_value_status(status), source_id);
}

static bool native_values_accepts_work(const XrtValueRoot *owner) {
    return xrt_value_root_storage_empty(owner) &&
        (!owner->managed || (!owner->managed->retired && !owner->managed->metadata.handle));
}

XR_FUNC XrtManagedStatus xrt_native_values_prepare(XrtValueRoot *owner, XrtBillingOwner **output) {
    if (!native_values_accepts_work(owner) || !output || *output) return XRT_MANAGED_ARGUMENT;
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

XR_FUNC XrtManagedStatus xrt_native_values_stat_prepare(XrtValueRoot *owner, XrtBillingOwner **billing,
    XrOsFileMetadataOwner **output) {
    if (!owner || owner->implicit || !output || *output) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = xrt_native_values_prepare(owner, billing);
    if (status == XRT_MANAGED_OK) *output = &owner->managed->metadata;
    return status;
}

XR_FUNC bool xrt_native_values_prepared(const XrtValueRoot *owner) {
    return native_values_accepts_work(owner) &&
        (!owner->managed || (!owner->managed->root && !owner->managed->backings.string &&
            !owner->managed->images && !owner->managed->environments));
}
XR_FUNC XrtManagedStatus xrt_native_values_backings(XrtValueRoot *owner, const XrtBackingTypes **output) {
    if (!native_values_accepts_work(owner) || !output || *output) return XRT_MANAGED_ARGUMENT;
    /* The String descriptor is published with the entire backing family. */
    if (owner->managed && owner->managed->backings.string) {
        *output = &owner->managed->backings;
        return XRT_MANAGED_OK;
    }
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

XR_FUNCDEF void xrt_native_values_use_state(XrtValueRoot *root, const XrtModuleState *state,
    const XrtValueImage *image) {
    XR_CHECK(root && root->managed && state && image && !root->managed->state,
        "a prepared native root borrows its one live State and image");
    root->managed->state = state;
    root->managed->state_image = image;
}
static XrtManagedStatus native_image_plans(struct XrtNativeValues *values,
    const XrtValueImage *source, XrtManagedLease code, const XrtValueImagePlans **output) {
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
    *output = image->plans;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus native_capture_plan(struct XrtNativeValues *values,
    const XrtValueImage *source, const XrtModuleState *state, XrtManagedLease code,
    uint32_t type_id, const XrtValuePlan **output) {
    if (state) return xrt_module_state_plan(state,type_id,output);
    const XrtValueImagePlans *plans = NULL;
    XrtManagedStatus status = native_image_plans(values, source, code, &plans);
    return status == XRT_MANAGED_OK ? xrt_value_image_get(plans, type_id, output) : status;
}
static XrtBoundInlineArray native_inline_array(const XrtValueImage *image, uint32_t type, uint32_t source) {
    struct XrtNativeValues *values = native_values(source);
    XrtBoundInlineArray binding = {0};
    XrtManagedStatus status;
    if (values->state && values->state_image == image)
        status = xrt_module_state_inline_array(values->state, type, &binding);
    else {
        const XrtValueImagePlans *plans = NULL;
        status = native_image_plans(values, image, (XrtManagedLease){0}, &plans);
        if (status == XRT_MANAGED_OK) status = xrt_value_image_inline_array(plans, type, &binding);
    }
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    return binding;
}
XRAY_API void xrt_native_array_inline_create(XrtManagedFrame *frame, size_t slot,
    const XrtValueImage *image, uint32_t type, const void *rows, size_t count, uint32_t source) {
    XrtBoundInlineArray binding = native_inline_array(image,type,source);
    XrtManagedStatus status = xrt_array_backing_inline_create(frame,slot,binding.ops,rows,count,count);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
}
XRAY_API const void *xrt_native_array_inline_at(const XrtManagedHeader *owner, const XrtValueImage *image,
    uint32_t type, int64_t index, uint32_t source) {
    XrtBoundInlineArray binding = native_inline_array(image,type,source);
    const void *row = NULL;
    XrtManagedStatus status = xrt_array_backing_inline_at((const XrtArrayBacking *)owner,binding.element,index,&row);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    return row;
}
static XrtBoundClass native_class(const XrtValueImage *image, uint32_t type, uint32_t source) {
    struct XrtNativeValues *values = native_values(source);
    XrtBoundClass binding = {0};
    XrtManagedStatus status;
    if (values->state && values->state_image == image)
        status = xrt_module_state_class(values->state, type, &binding);
    else {
        const XrtValueImagePlans *plans = NULL;
        status = native_image_plans(values, image, (XrtManagedLease){0}, &plans);
        if (status == XRT_MANAGED_OK) status = xrt_value_image_class(plans, type, &binding);
    }
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    return binding;
}

XRAY_API void xrt_native_host_file_open_write(uint32_t callable, const XrtNativeFileOpen *call, uint32_t source) {
    if (!call || xr_native_host_shape(callable) != XR_NATIVE_HOST_FILE_OPEN_WRITE)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    XrtBoundClass binding = native_class(call->image, call->type, source);
    xrt_host_file_open_publish(&call->output, binding, call->path, call->append, source);
}

static XrtBoundClass native_file_class(const XrtValueImage *image, uint32_t type, uint32_t source) {
    XrtBoundClass binding = native_class(image, type, source);
    XrtValuePlanSpec spec;
    if (xrt_value_plan_view(binding.plan, &spec) != XRT_MANAGED_OK ||
        spec.class_storage != XR_NATIVE_DECLARATION_FILE_STORAGE)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    return binding;
}

static const XrtValuePlan *native_host_optional_element(const XrtValueImage *image, uint32_t type, uint32_t source) {
    struct XrtNativeValues *values = native_values(source);
    const XrtModuleState *state = image == values->state_image ? values->state : NULL;
    const XrtValuePlan *plan = NULL;
    XrtManagedStatus status = native_capture_plan(values, image, state, (XrtManagedLease){0}, type, &plan);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source);
    XrtValuePlanSpec spec;
    const size_t owner = offsetof(XrtHostNullableOwner, owner);
    if (xrt_value_plan_view(plan, &spec) != XRT_MANAGED_OK || spec.kind != XRT_VALUE_NULLABLE ||
        spec.size != sizeof(XrtHostNullableOwner) || spec.alignment != _Alignof(XrtHostNullableOwner) ||
        spec.owned_count != 1 || spec.owned_offsets[0] != owner ||
        spec.nullable.tag_offset != offsetof(XrtHostNullableOwner, present) || spec.nullable.payload_offset != owner)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    return spec.nullable.element;
}

XRAY_API void xrt_native_host_file_open_read(uint32_t callable, const XrtNativeFileOpenRead *call, uint32_t source) {
    uint32_t native_class_id = xr_native_host_path_result_class(callable);
    if (!call || !native_class_id || !xrt_backing_string_matches(call->path))
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    XrtBoundClass binding = native_class_id == XR_NATIVE_DECLARATION_FILE_STORAGE ?
        native_file_class(call->image, call->type, source) : native_class(call->image, call->type, source);
    if (native_host_optional_element(call->image, call->result_type, source) != binding.plan)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    if (native_class_id == XR_NATIVE_DECLARATION_FILE_STAT) {
        const XrtHostNullableSlots output = {call->output.frame, call->output.destination, call->output.temporary,
            call->output.offset, call->temporary_owner_offset};
        XrtHostStatResult stat = xrt_host_stat_publish(&output, binding, call->path, source);
        if (stat.host != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(stat.host), source);
        if (stat.managed != XRT_MANAGED_OK) xrt_native_managed_failure(stat.managed, source);
        return;
    }
    const XrtHostFileStagedResult output = {call->output, call->temporary_owner_offset};
    XrtHostFileOpen opened = xrt_host_file_open_read_publish(&output, binding, (const XrtStringBacking *)call->path, source);
    if (opened.managed != XRT_MANAGED_OK) xrt_native_managed_failure(opened.managed, source);
    if (opened.host != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(opened.host), source);
}

XRAY_API void xrt_native_host_file_read(uint32_t callable, const XrtNativeFileRead *call, uint32_t source) {
    if (!call || xr_native_host_shape(callable) != XR_NATIVE_HOST_FILE_READ)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    XrtBoundClass binding = native_file_class(call->image, call->type, source);
    XrtValuePlanSpec element;
    if (!xrt_class_backing_matches(call->file, binding.plan) ||
        xrt_value_plan_view(native_host_optional_element(call->image, call->result_type, source), &element) != XRT_MANAGED_OK ||
        element.kind != XRT_VALUE_ARRAY_U8)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source), &types);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source);
    const XrtHostFileStagedResult output = {call->output, call->temporary_owner_offset};
    XrtHostFileRead read = xrt_host_file_read_publish(&output, binding, call->file, types, call->max_bytes, source);
    if (read.managed != XRT_MANAGED_OK) xrt_native_managed_failure(read.managed, source);
    if (read.host != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(read.host), source);
}

XRAY_API void xrt_native_host_environment_get(uint32_t callable, const XrtNativeEnvironmentGet *call, uint32_t source) {
    if (!call || xr_native_host_shape(callable) != XR_NATIVE_HOST_ENV_GET || !xrt_backing_string_matches(call->name))
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    XrtValuePlanSpec element;
    if (xrt_value_plan_view(native_host_optional_element(call->image, call->result_type, source), &element) != XRT_MANAGED_OK ||
        element.kind != XRT_VALUE_STRING)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    const XrtHostNullableSlots output = {call->frame, call->destination, call->temporary, call->offset,
        call->temporary_owner_offset};
    XrtHostEnvironmentGet result = xrt_host_environment_get_publish(&output, call->name, source);
    if (result.managed != XRT_MANAGED_OK) xrt_native_managed_failure(result.managed, source);
    if (result.host != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(result.host), source);
}

XRAY_API bool xrt_native_host_file_close(uint32_t callable, const XrtValueImage *image, uint32_t type,
    const XrtManagedHeader *owner, uint32_t source) {
    if (xr_native_host_shape(callable) != XR_NATIVE_HOST_FILE_CLOSE)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    bool closed;
    XrtManagedStatus status = xrt_host_file_close(native_class(image, type, source), owner, &closed);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source);
    return closed;
}

XRAY_API int64_t xrt_native_host_file_write(uint32_t callable, const XrtNativeFileWrite *call, uint32_t source) {
    if (!call) xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source);
    XrtBoundClass binding = native_class(call->image, call->type, source);
    int64_t count;
    XrOsIoStatus status = xrt_host_file_write(binding, xr_native_host_shape(callable),
        call->file, call->data, call->offset, &count);
    if (status != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(status), source);
    return count;
}
XRAY_API void xrt_native_class_create(XrtManagedFrame *frame, size_t slot, const XrtValueImage *image,
    uint32_t type, const void *row, uint32_t source) {
    XrtBoundClass binding = native_class(image,type,source);
    XrtManagedStatus status = xrt_class_create(frame,slot,binding.ops,row);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
}
XRAY_API const void *xrt_native_class_field(const XrtManagedHeader *owner, const XrtValueImage *image,
    uint32_t type, uint32_t field, uint32_t source) {
    XrtBoundClass binding = native_class(image,type,source);
    const XrtValuePlan *field_type = NULL;
    const void *bytes = NULL;
    XrtManagedStatus status = xrt_class_field_borrow(owner,binding.plan,field,&field_type,&bytes);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    return bytes;
}
/* A Class-identity union has no stored tag. The live instance header keeps its
 * immutable per-class Ops, which is the nominal identity of its dynamic class.
 * The test walks UP from that dynamic class: it succeeds when the target class
 * is the value's exact class or one of its Class ancestors. Walking the target's
 * own ancestors instead would answer "is an ancestor of the target", which is
 * the reverse question. Compiler type ids only name rows of this image. */
XRAY_API bool xrt_native_class_is(const XrtManagedHeader *owner, const XrtValueImage *image,
    uint32_t type, uint32_t source) {
    if (!owner || !image || !image->types || !type || type > image->type_count ||
        image->types[type - 1].kind != XRT_VALUE_CLASS) return false;
    XrtBoundClass target = native_class(image, type, source);
    if (!target.ops) return false;
    /* Locate the dynamic class row by its immutable Ops identity. */
    uint32_t current = 0;
    for (uint32_t id = 1; id <= image->type_count; ++id) {
        if (image->types[id - 1].kind != XRT_VALUE_CLASS) continue;
        XrtBoundClass row = native_class(image, id, source);
        if (row.ops == owner->ops) { current = id; break; }
    }
    for (; current; ) {
        XrtBoundClass row = native_class(image, current, source);
        if (row.ops == target.ops) return true;
        current = current <= image->type_count ? image->types[current - 1].class_parent : 0;
    }
    return false;
}
static XrtClassFieldAccess native_class_field(const XrtNativeClassField *field, uint32_t source) {
    if (!field) xrt_native_managed_failure(XRT_MANAGED_ARGUMENT,source);
    XrtBoundClass binding=native_class(field->image,field->type,source);
    return (XrtClassFieldAccess){field->instance,binding.plan,field->field};
}
XRAY_API void xrt_native_class_scalar_field_write(const XrtNativeClassField *field,
    const void *bytes, size_t size, uint32_t source) {
    XrtClassFieldAccess access=native_class_field(field,source);
    XrtManagedStatus status=xrt_class_scalar_field_write(access.instance,access.plan,access.index,bytes,size);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
}
XRAY_API void xrt_native_class_array_field_copy(const XrtNativeClassField *field,
    XrtManagedFrame *frame, size_t destination, uint32_t source) {
    XrtClassFieldAccess access=native_class_field(field,source);
    XrtManagedStatus status=xrt_class_array_field_copy(&access,frame,destination);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
}
XRAY_API void xrt_native_class_array_field_replace(const XrtNativeClassField *field,
    const XrtManagedHeader *incoming, uint32_t source) {
    XrtClassFieldAccess access=native_class_field(field,source);
    XrtManagedStatus status=xrt_class_array_field_replace(&access,incoming);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
}
XRAY_API void xrt_native_class_array_field_push(const XrtNativeClassField *field,
    XrtManagedFrame *frame, size_t workspace, int64_t item, uint32_t source) {
    XrtClassFieldAccess access=native_class_field(field,source);
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source),&types);
    if (status == XRT_MANAGED_OK)
        status = xrt_class_array_field_push(&access,frame,workspace,types,item);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
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
XRAY_API void xrt_native_string_slice_create(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *receiver, int64_t start, int64_t end, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK) {
        status = xrt_string_backing_slice_create(frame, slot, types, (const XrtStringBacking *)receiver, start, end);
        if (status == XRT_MANAGED_BOUNDS) xrt_typed_panic(XR_ERR_STRING_SLICE_RANGE, source_id);
    }
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API int64_t xrt_native_string_index_of(const XrtStringBacking *text, const XrtStringBacking *search,
    int64_t start, uint32_t source_id) {
    int64_t ordinal;
    XrtManagedStatus status = xrt_string_backing_index_of(text, search, start, &ordinal);
    if (status == XRT_MANAGED_BOUNDS) xrt_typed_panic(422, source_id);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    return ordinal;
}
XRAY_API bool xrt_native_string_test(const XrtStringBacking *text, const XrtStringBacking *pattern,
    uint32_t test, uint32_t source_id) {
    bool output = false;
    XrtManagedStatus status = xrt_string_backing_test(text, pattern, (XrtStringTest)test, &output);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    return output;
}
XRAY_API void xrt_native_string_append_owned(XrtManagedFrame *frame,
    const XrtStringAppendOwned *input, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK) status = xrt_string_backing_append_owned(frame, types, input);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API void xrt_native_string_binding_prepare(uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API void xrt_native_string_append_place(XrtPlace binding, XrtManagedFrame *frame, size_t workspace,
    const XrtStringBacking *suffix, uint32_t source_id) {
    XrtValueRoot *owner = xrt_value_root_current(source_id);
    const XrtBackingTypes *types = owner->managed && owner->managed->backings.string ?
        &owner->managed->backings : NULL;
    XrtManagedStatus status = types ? xrt_string_backing_append_place(binding, frame, workspace, types, suffix) :
        XRT_MANAGED_ARGUMENT;
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
XRAY_API void xrt_native_string_copy_bytes(XrtManagedFrame *frame, size_t slot,
    const XrtStringBacking *string, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status == XRT_MANAGED_OK) status = xrt_string_backing_copy_bytes_create(frame,slot,types,string);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API int64_t xrt_native_array_capacity(const XrtManagedHeader *array,
    XrtBackingArrayKind kind, uint32_t source_id) {
    int64_t capacity = 0;
    XrtManagedStatus status = xrt_array_backing_capacity(array,kind,&capacity);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
    return capacity;
}
XRAY_API void xrt_native_array_reserve(XrtManagedFrame *frame, size_t slot, const XrtManagedHeader *array,
    XrtBackingArrayKind kind, int64_t capacity, uint32_t source_id) {
    if (capacity < 0) xrt_typed_panic(422,source_id);
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status == XRT_MANAGED_OK) status = xrt_array_backing_reserve_create(frame,slot,types,kind,array,capacity);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_array_get(const XrtManagedHeader *array, XrtBackingArrayKind kind,
    int64_t index, XrtArrayItem *output, uint32_t source_id) {
    XrtManagedStatus status=xrt_array_backing_get((const XrtArrayBacking *)array,kind,index,output);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_array_string_read_copy(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *array, int64_t index, uint32_t source_id) {
    XrtArrayItem item = {0};
    xrt_native_array_get(array, XRT_BACKING_ARRAY_STRING, index, &item, source_id);
    XrtManagedHeader *owner = &item.value.string->managed;
    if (!xrt_managed_retain(owner)) xrt_native_managed_failure(XRT_MANAGED_REFCOUNT, source_id);
    XrtManagedStatus status = xrt_managed_slot_replace(frame, slot, &owner);
    if (status != XRT_MANAGED_OK) {
        xrt_managed_drop(owner);
        xrt_native_managed_failure(status, source_id);
    }
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
XRAY_API void xrt_native_array_push_local_i64(XrtPlace binding, size_t workspace, int64_t item, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK) status = xrt_array_push_local_i64(binding, workspace, types, item);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

#include "xnative_map_literal_value.inc.c"
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
typedef enum CollectionProjection {
    COLLECTION_MAP_KEYS=XRT_MAP_KEYS,
    COLLECTION_MAP_VALUES=XRT_MAP_VALUES,
    COLLECTION_MAP_DISPLAY=XRT_MAP_DISPLAY,
    COLLECTION_SET_DISPLAY,
} CollectionProjection;
static void native_collection_project(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *collection, CollectionProjection projection, uint32_t source_id) {
    XrtValueRoot *owner = xrt_value_root_current(source_id);
    XrtManagedSlotLocation guard = {0};
    XrtManagedStatus status = collection ? xrt_managed_slot_location(frame, slot, &guard) : XRT_MANAGED_ARGUMENT;
    if (status == XRT_MANAGED_OK) status = xrt_managed_domain_enter(guard);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);

    /* Even the lazy backing family may invoke an allocator callback. Pin the
     * source before either allocation so another root must detach it. */
    XrtManagedHeader *pin = (XrtManagedHeader *)collection;
    if (!xrt_managed_retain(pin)) {
        xrt_managed_domain_leave(guard);
        xrt_native_managed_failure(XRT_MANAGED_REFCOUNT, source_id);
    }
    const XrtBackingTypes *types = NULL;
    status = xrt_native_values_backings(owner, &types);
    xrt_managed_domain_leave(guard);
    if (status == XRT_MANAGED_OK) {
        switch (projection) {
        case COLLECTION_MAP_KEYS: case COLLECTION_MAP_VALUES:
            status = xrt_map_backing_snapshot_held(frame, slot, types,
                (const XrtMapBacking *)collection, projection == COLLECTION_MAP_KEYS);
            break;
        case COLLECTION_MAP_DISPLAY:
            status = xrt_map_backing_display_held(frame, slot, types, (const XrtMapBacking *)collection);
            break;
        case COLLECTION_SET_DISPLAY:
            status = xrt_set_backing_display_held(frame, slot, types, (const XrtSetBacking *)collection);
            break;
        }
    }
    XR_CHECK(xrt_managed_domain_enter(guard) == XRT_MANAGED_OK,
        "collection projection retirement uses the same live caller Root domain");
    xrt_managed_drop(pin);
    xrt_managed_domain_leave(guard);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API void xrt_native_map_project(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *map, XrtMapProjection projection, uint32_t source_id) {
    if (projection != XRT_MAP_KEYS && projection != XRT_MAP_VALUES && projection != XRT_MAP_DISPLAY)
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source_id);
    native_collection_project(frame, slot, map, (CollectionProjection)projection, source_id);
}
XRAY_API void xrt_native_set_display(XrtManagedFrame *frame, size_t slot,
    const XrtManagedHeader *set, uint32_t source_id) {
    native_collection_project(frame, slot, set, COLLECTION_SET_DISPLAY, source_id);
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
    bool applied = false;
    /* One real Frame already protects both slots during key retirement. */
    if (status == XRT_MANAGED_OK && frame && binding.frame == frame && !binding.external &&
        binding.nulls == XRT_PLACE_NONNULL)
        status = xrt_map_try_update_local(frame, binding.slot, workspace, update, &applied, removed);
    if (status == XRT_MANAGED_OK && !applied)
        status = xrt_map_update_place(binding, frame, workspace, types, update, removed);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API void xrt_native_set_create(XrtManagedFrame *frame, size_t slot,
    const XrtSetInput *input, uint32_t source_id) {
    const XrtBackingTypes *types=NULL;
    XrtManagedStatus status=input ? xrt_native_values_backings(xrt_value_root_current(source_id),&types) : XRT_MANAGED_ARGUMENT;
    if (status==XRT_MANAGED_OK) status=xrt_set_backing_create(frame,slot,types,input->values,input->count);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API bool xrt_native_set_contains(const XrtManagedHeader *set, int64_t key, uint32_t source_id) {
    bool found=false;
    XrtManagedStatus status=xrt_set_backing_contains((const XrtSetBacking *)set,key,&found);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
    return found;
}
XRAY_API void xrt_native_set_update_place(XrtPlace binding, XrtManagedFrame *frame, size_t workspace,
    const XrtSetUpdate *update, bool *removed, uint32_t source_id) {
    const XrtBackingTypes *types=NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source_id),&types);
    if (status==XRT_MANAGED_OK) status=xrt_set_update_place(binding,frame,workspace,types,update,removed);
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source_id);
}
XRAY_API void xrt_native_pod_display_create(XrtManagedFrame *frame, size_t slot,
    const XrtPodDisplay *description, const void *bytes, uint32_t source_id) {
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK) status = xrt_string_backing_pod_display_create(frame, slot, types, description, bytes);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}
XRAY_API void xrt_native_tuple_display_create(XrtManagedFrame *frame, size_t slot,
    const XrtValueImage *image, uint32_t type, const void *bytes, uint32_t source_id) {
    if (!image || !image->types || !type || type > image->type_count ||
        !(image->types[type - 1].flags & XRT_VALUE_INLINE_DISPLAY))
        xrt_native_managed_failure(XRT_MANAGED_ARGUMENT, source_id);
    struct XrtNativeValues *values = native_values(source_id);
    const XrtModuleState *state = image == values->state_image ? values->state : NULL;
    const XrtValuePlan *plan = NULL;
    XrtManagedStatus status = native_capture_plan(values, image, state,
        (XrtManagedLease){0}, type, &plan);
    const XrtBackingTypes *types = NULL;
    if (status == XRT_MANAGED_OK) status = xrt_native_values_backings(xrt_value_root_current(source_id), &types);
    if (status == XRT_MANAGED_OK)
        status = xrt_string_backing_tuple_display_create(frame, slot, types, image, type, bytes);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
}

static void native_values_retire_managed(struct XrtNativeValues *values) {
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
    values->state = NULL;
    values->state_image = NULL;
}

XR_FUNC XrOsIoStatus xrt_value_root_drain_with(XrtValueRoot *owner, XrtMetadataCloser close_metadata) {
    if (!owner || !owner->managed) return XR_OS_IO_OK;
    if (owner->draining || !close_metadata) return XR_OS_IO_BAD_ARGUMENT;
    owner->draining = true;
    struct XrtNativeValues *values = owner->managed;
    if (!values->retired) {
        native_values_retire_managed(values);
        values->retired = true;
    }
    if (values->metadata.handle) {
        intptr_t handle = values->metadata.handle;
        XrOsIoStatus status = close_metadata(&values->metadata);
        XR_CHECK(values->metadata.handle == (status == XR_OS_IO_OK ? 0 : handle),
            "metadata close consumes only on success and otherwise preserves the original handle");
        if (status != XR_OS_IO_OK) {
            owner->draining = false;
            return status;
        }
    }
    XrtBillingOwner *billing = values->billing;
    owner->managed = NULL;
    xrt_managed_metadata_free(billing, values, sizeof(*values));
    xrt_billing_owner_drop(billing);
    owner->draining = false;
    return XR_OS_IO_OK;
}

XR_FUNC XrOsIoStatus xrt_value_root_drain(XrtValueRoot *owner) {
    return xrt_value_root_drain_with(owner, xr_os_io_file_metadata_close);
}
