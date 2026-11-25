/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_execute.inc.c - Actual typed values in stable activation homes
 *
 * KEY CONCEPT:
 *   Activations borrow code and normalized formal places. Their frames own
 *   every real reference; movable banks contain only scalar or POD transport.
 */
#include "runtime/native/xnative_string_backing_ops.h"
#include "runtime/native/xnative_array_ops.h"
#include "runtime/native/xnative_host_environment.h"
#include "runtime/native/xnative_map_backing.h"
#include "runtime/native/xnative_set_backing.h"
#include "runtime/native/xnative_values.h"
#include "xray_native_array_local.h"
#include "xray_native_values.h"

typedef struct RbcValueActivation RbcValueActivation;
typedef struct RbcValueOrigin { RbcValueActivation *activation; uint32_t value; } RbcValueOrigin;
typedef struct RbcValuePayload {
    uint8_t *address;
    const XrLirHomeBytes *spans;
    size_t span_count, size;
    XrPodType packed;
    bool lanes;
} RbcValuePayload;
typedef struct RbcValuePublication {
    RbcValuePayload destination, clear;
    const uint8_t *snapshot;
} RbcValuePublication;
typedef struct RbcMapParameter {
    const uint8_t *address; /* Actual owner field, never a cached backing. */
    const XrtManagedSlot *slot;
    const bool *busy;
} RbcMapParameter;
typedef union RbcParameterStorage {
    RbcMapParameter map;
    XrtFunction function;
    XrtManagedHeader *cell;
} RbcParameterStorage;
XR_STATIC_ASSERT(sizeof(RbcParameterStorage) == sizeof(XrtFunction),
    "Map parameter addresses reuse the existing capture storage");
typedef enum RbcArrayRegionPhase {
    RBC_ARRAY_REGION_COLD, RBC_ARRAY_REGION_ELIGIBLE, RBC_ARRAY_REGION_ACTIVE, RBC_ARRAY_REGION_BYPASS
} RbcArrayRegionPhase;
typedef struct RbcArrayRegionState {
    XrtArrayI64RegionCache cache;
    RbcArrayRegionPhase phase;
    bool completed_set;
} RbcArrayRegionState;
struct RbcValueActivation {
    RbcValueActivation *previous;
    const XrRbcValueCode *code;
    const XrtValueImage *image;
    uint32_t function;
    size_t depth, bytes;
    XrtManagedFrame *frame;
    uint8_t *payload;
    XrtManagedFrameLocations frame_locations;
    RbcValueOrigin *parameters;
    const uint8_t **borrowed_parameters;
    RbcParameterStorage *parameter_storage;
    XrtPlaceEdge *edges;
    RbcValuePublication *publications;
    uint8_t *snapshots;
    size_t edge_capacity, publication_capacity, snapshot_capacity, publication_count;
    bool owned_code; /* Only a genuine immutable code owner supplies sealed plans. */
    bool backings_ready; /* Set only after this execution's complete backing setup succeeds. */
    bool receiving_call;
    uint32_t invoke_error, invoke_status;
    const XrLirHomeCall *receiving_slice_call;
    RbcArrayRegionState *array_regions;
    uint32_t active_array_region;
    uint32_t pending_array_read_fold; /* Reset by the exact entry edge and consumed at its first latch. */
    uint32_t blocked_array_push; /* A declined capacity window stays ordinary until its next entry. */
};
struct XrRbcValueRuntime {
    XrtBillingOwner *billing;
    XrtManagedRoot *root;
    RbcValueActivation *top;
    XrtValueImagePlans *plans;
    XrtCaptureType **environments;
    uint32_t environment_count;
};
static void rbc_value_environment_set_drop(XrRbcValueRuntime *runtime) {
    for (uint32_t n=0;n<runtime->environment_count;++n) xrt_capture_type_drop(&runtime->environments[n]);
    if (runtime->environments) xrt_managed_metadata_free(runtime->billing,runtime->environments,
        (size_t)runtime->environment_count*sizeof(*runtime->environments));
    runtime->environments=NULL; runtime->environment_count=0;
    xrt_value_image_drop(&runtime->plans);
}
static XrtManagedStatus rbc_value_plan_set(const XrRbcModule *module, XrRbcStack *stack) {
    const XrtValueImage *image = xr_rbc_module_value_image(module);
    if (stack->layout)
        return module->state_spec && module->state_spec->values == image ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
    if (stack->values->plans) return XRT_MANAGED_OK;
    /* A sealed value image can be owned by code with no callable entries. */
    const XrRbcCodeOwner *code = xr_rbc_module_code_owner(module);
    if (!code || !image || !image->type_count) return XRT_MANAGED_ARGUMENT;
    return xrt_value_image_bind(stack->values->billing,image,
        (XrtManagedLease){(void *)code,xr_rbc_code_owner_retain,xr_rbc_code_owner_release},&stack->values->plans);
}
static XrtManagedStatus rbc_value_environments(const XrRbcModule *module, XrRbcStack *stack) {
    XrRbcValueRuntime *runtime=stack->values;
    if (runtime->environments) return XRT_MANAGED_OK;
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    const XrRbcCodeOwner *code=xr_rbc_code_owner_borrow((XrtManagedLease){module->code_lease.owner,
        module->code_lease.retain,module->code_lease.release});
    if (!code || !image || !image->type_count) return XRT_MANAGED_ARGUMENT;
    size_t count=image->type_count;
    if (count>SIZE_MAX/sizeof(*runtime->environments)) return XRT_MANAGED_OVERFLOW;
    XrtManagedStatus status = rbc_value_plan_set(module,stack);
    void *storage=NULL;
    if (status==XRT_MANAGED_OK) status=xrt_managed_metadata_allocate(runtime->billing,
        (size_t)image->type_count*sizeof(*runtime->environments),&storage);
    if (status==XRT_MANAGED_OK) {
        runtime->environments=storage; runtime->environment_count=image->type_count;
        for (uint32_t n=0;n<image->type_count;++n) runtime->environments[n]=NULL;
    }
    for (uint32_t n=0;status==XRT_MANAGED_OK && n<image->type_count;++n) {
        if (image->types[n].kind!=XRT_VALUE_ENVIRONMENT && image->types[n].kind!=XRT_VALUE_CELL) continue;
        const XrtValuePlan *plan=NULL;
        status=stack->layout ? xrt_module_layout_plan(stack->layout,n+1,&plan) : xrt_value_image_get(runtime->plans,n+1,&plan);
        if (status==XRT_MANAGED_OK) status=xrt_capture_type_new(runtime->billing,plan,&runtime->environments[n]);
    }
    if (status!=XRT_MANAGED_OK) rbc_value_environment_set_drop(runtime);
    return status;
}
static bool rbc_value_extent(size_t *bytes, size_t count, size_t stride) {
    if (*bytes > SIZE_MAX - 7u) return false;
    *bytes = (*bytes + 7u) & ~(size_t)7u;
    if (count && stride > (SIZE_MAX - *bytes) / count) return false;
    *bytes += count * stride;
    return true;
}
static XrtManagedStatus rbc_value_runtime(XrRbcStack *stack, uint32_t source) {
    if (stack->values) return XRT_MANAGED_OK;
    XrtBillingOwner *billing = NULL;
    XrtManagedStatus status = xrt_native_values_prepare(xrt_value_root_current(source),&billing);
    if (status != XRT_MANAGED_OK) return status;
    if (!xrt_billing_owner_retain(billing)) return XRT_MANAGED_REFCOUNT;
    void *memory = NULL;
    status = xrt_managed_metadata_allocate(billing,sizeof(XrRbcValueRuntime),&memory);
    if (status != XRT_MANAGED_OK) { xrt_billing_owner_drop(billing); return status; }
    XrRbcValueRuntime *runtime = memory;
    *runtime = (XrRbcValueRuntime){.billing=billing};
    stack->values = runtime;
    return XRT_MANAGED_OK;
}
static bool rbc_value_capacities(const XrRbcValueCode *code, const XrtValueImage *image,
    size_t *leaves, size_t *rows, size_t *snapshots) {
    *leaves = 0; *rows = 1; *snapshots = 8;
    const XrLirValueHomes *homes = code->homes;
    if (homes) for (size_t v = 1; v <= (size_t)homes->value_count; ++v) {
        const XrLirValueHome *home = &homes->values[v];
        if (home->owner_count > *leaves) *leaves = home->owner_count;
        if (home->size > *snapshots) *snapshots = home->size;
    }
    for (size_t n = 0; n < code->access_count; ++n) {
        XrRbcValueAccess access = code->accesses[n];
        if (access.operation == XR_RBC_VALUE_CLASS_NEW) {
            const XrLirValueHome *home=rbc_value_home(code,access.result);
            if (!image || !image->types || !home || !home->type || home->type>image->type_count) return false;
            const XrtValueImageType *type=&image->types[home->type-1];
            if (type->kind!=XRT_VALUE_CLASS || !type->element || type->element>=home->type) return false;
            size_t bytes=image->types[type->element-1].size;
            if (bytes>*snapshots) *snapshots=bytes;
        }
        if (access.operation == XR_RBC_VALUE_MAP_LITERAL) {
            size_t count = access.count;
            if (count > SIZE_MAX / sizeof(XrtMapLiteralInput)) return false;
            size_t bytes = count * sizeof(XrtMapLiteralInput);
            if (bytes > *snapshots) *snapshots = bytes;
        }
        if (access.operation==XR_RBC_VALUE_FUNCTION_NEW) {
            size_t bytes=0;
            for (uint32_t p=0;p<access.count;++p) {
                const XrLirValueHome *home=rbc_value_home(code,code->arguments[access.first+p]);
                size_t size=home && home->type ? home->size : 8;
                if (bytes>SIZE_MAX-size) return false;
                bytes+=size;
            }
            if (bytes>*snapshots) *snapshots=bytes;
        }
        if ((access.operation==XR_RBC_VALUE_ARRAY && access.index==XR_LIR_ARRAY_OP_NEW) ||
            (access.operation==XR_RBC_VALUE_MAP && access.index==XR_LIR_MAP_OP_NEW)) {
            size_t count=access.count, stride=8;
            const XrLirValueHome *home=rbc_value_home(code,access.result);
            if (access.operation==XR_RBC_VALUE_ARRAY && image && image->types &&
                home && home->type && home->type<=image->type_count) {
                const XrtValueImageType *array=&image->types[home->type-1];
                if (array->kind==XRT_VALUE_ARRAY_INLINE) {
                    if (!array->element || array->element>=home->type) return false;
                    stride=image->types[array->element-1].size;
                }
            }
            if (!stride || count>SIZE_MAX/stride) return false;
            if (count*stride>*snapshots) *snapshots=count*stride;
        }
        if (access.operation==XR_RBC_VALUE_CALL || access.operation==XR_RBC_VALUE_INVOKE ||
            access.operation==XR_RBC_VALUE_FUNCTION_CALL) {
            size_t bytes=0;
            for (uint32_t p=0;p<access.count;++p) {
                const XrLirValueHome *home=rbc_value_home(code,code->arguments[access.first+p]);
                size_t size=home && home->owner_count ? 0 : home && home->type ? home->size : 8;
                if (bytes>SIZE_MAX-size) return false;
                bytes+=size;
            }
            if (access.count>*rows) *rows=access.count;
            if (bytes>*snapshots) *snapshots=bytes;
            continue;
        }
        if (access.operation != XR_RBC_VALUE_EDGE) continue;
        size_t count = 0, bytes = 0;
        for (uint32_t p = 0; p < access.count; ++p) {
            uint32_t target = code->arguments[access.first + (size_t)p * 2 + 1];
            const XrLirValueHome *home = rbc_value_home(code,target);
            size_t size = home && home->type ? home->size : 8;
            size_t owners=home ? home->owner_count : 0;
            if (home && home->view_workspace) {
                if (owners==SIZE_MAX) return false;
                ++owners;
            }
            if (bytes > SIZE_MAX - size || count > SIZE_MAX - owners) return false;
            bytes += size; count += owners;
        }
        if (count > *leaves) *leaves = count;
        if (access.count > *rows) *rows = access.count;
        if (bytes > *snapshots) *snapshots = bytes;
    }
    return true;
}
static XrRbcStatus xr_rbc_values_begin(const XrRbcModule *module, XrRbcStack *stack,
    uint32_t function, uint32_t source) {
    const XrRbcValueCode *code = xr_rbc_module_value_code(module,function);
    if (!code) return XR_RBC_OK;
    if (!stack->depth || function >= module->function_count || stack->frames[stack->depth - 1].function != function)
        return XR_RBC_INVALID;
    XrtManagedStatus status = rbc_value_runtime(stack,source);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    XrRbcValueRuntime *runtime = stack->values;
    if (runtime->top && runtime->top->depth == stack->depth) return XR_RBC_INVALID;
    size_t leaves, rows, snapshots, bytes = sizeof(RbcValueActivation);
    /* A borrowed raw description can still use the ordinary typed receiver.
     * Only the exact immutable executable owner supplies the closed window. */
    size_t regions=xr_rbc_module_code_owner(module) && code->array_region_runtime ?
        code->array_regions->region_count : 0;
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    /* An inline record home keeps its declared alignment, so the one payload
     * region reserves the padding its own rounding may consume. */
    size_t payload_alignment = code->homes && code->homes->frame.payload_alignment ?
        code->homes->frame.payload_alignment : 1;
    if (payload_alignment & (payload_alignment - 1))
        xrt_native_managed_failure(XRT_MANAGED_OVERFLOW,source);
    size_t payload_bytes = code->homes && !code->homes->owned_frame_needed ?
        code->homes->frame.payload_size : 0;
    if (payload_bytes && !rbc_value_extent(&payload_bytes,payload_alignment - 1,1))
        xrt_native_managed_failure(XRT_MANAGED_OVERFLOW,source);
    if (!rbc_value_capacities(code,image,&leaves,&rows,&snapshots) ||
        !rbc_value_extent(&bytes,code->parameter_count,sizeof(RbcValueOrigin)) ||
        !rbc_value_extent(&bytes,code->parameter_count,sizeof(const uint8_t *)) ||
        !rbc_value_extent(&bytes,code->parameter_count,sizeof(RbcParameterStorage)) ||
        !rbc_value_extent(&bytes,leaves,sizeof(XrtPlaceEdge)) ||
        !rbc_value_extent(&bytes,rows,sizeof(RbcValuePublication)) || !rbc_value_extent(&bytes,snapshots,1) ||
        !rbc_value_extent(&bytes,regions,sizeof(RbcArrayRegionState)) ||
        !rbc_value_extent(&bytes,payload_bytes,1))
        xrt_native_managed_failure(XRT_MANAGED_OVERFLOW,source);
    void *memory = NULL;
    status = xrt_managed_metadata_allocate(runtime->billing,bytes,&memory);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    RbcValueActivation *activation = memory;
    *activation = (RbcValueActivation){.previous=runtime->top,.code=code,.image=image,
        .function=function,.depth=stack->depth,
        .bytes=bytes,.edge_capacity=leaves,.publication_capacity=rows,.snapshot_capacity=snapshots,
        .owned_code=xr_rbc_module_code_owner(module)!=NULL};
    size_t offset = sizeof(*activation);
#define RBC_VALUE_PART(field,n,t) do { offset=(offset+7u)&~(size_t)7u; \
    activation->field=(t *)((uint8_t *)memory+offset); offset+=(n)*sizeof(t); } while(0)
    RBC_VALUE_PART(parameters,code->parameter_count,RbcValueOrigin);
    RBC_VALUE_PART(borrowed_parameters,code->parameter_count,const uint8_t *);
    RBC_VALUE_PART(parameter_storage,code->parameter_count,RbcParameterStorage);
    RBC_VALUE_PART(edges,leaves,XrtPlaceEdge);
    RBC_VALUE_PART(publications,rows,RbcValuePublication);
    RBC_VALUE_PART(snapshots,snapshots,uint8_t);
    if (regions) {
        RBC_VALUE_PART(array_regions,regions,RbcArrayRegionState);
        memset(activation->array_regions,0,regions*sizeof(RbcArrayRegionState));
    }
    if (code->homes && !code->homes->owned_frame_needed && code->homes->frame.payload_size) {
        /* The one inline payload region starts on its own alignment, not the
         * eight-byte part alignment used by the metadata around it. */
        offset = (offset + payload_alignment - 1) & ~(payload_alignment - 1);
        activation->payload = (uint8_t *)memory + offset;
        offset += code->homes->frame.payload_size;
        memset(activation->payload,0,code->homes->frame.payload_size);
    }
#undef RBC_VALUE_PART
    for (uint32_t p = 0; p < code->parameter_count; ++p) {
        activation->parameters[p]=(RbcValueOrigin){0}; activation->borrowed_parameters[p]=NULL;
        activation->parameter_storage[p]=(RbcParameterStorage){.map={0}};
    }
    runtime->top = activation;
    const XrRbcFunction *physical=&module->functions[function];
    if ((physical->aggregate_size || physical->environment_size) && !xr_rbc_aggregate_reserve(stack,&stack->frames[stack->depth-1],physical))
        return XR_RBC_OUT_OF_MEMORY;
    if (code->homes && code->homes->owned_frame_needed) {
        runtime->root = xrt_native_managed_root(source);
        status = xrt_managed_frame_enter_locations(runtime->root,&code->homes->frame,&activation->frame_locations);
        if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
        activation->frame = activation->frame_locations.frame;
        activation->payload = activation->frame_locations.payload;
        if (code->string_binding_setup) xrt_native_string_binding_prepare(source);
    }
    return XR_RBC_OK;
}
static void xr_rbc_values_free(XrRbcStack *stack) {
    XrRbcValueRuntime *runtime = stack->values;
    if (!runtime) return;
    stack->values = NULL;
    XrtBillingOwner *billing = runtime->billing;
    while (runtime->top) {
        RbcValueActivation *activation = runtime->top;
        runtime->top = activation->previous;
        xrt_managed_metadata_free(billing,activation,activation->bytes);
    }
    rbc_value_environment_set_drop(runtime);
    xrt_managed_metadata_free(billing,runtime,sizeof(*runtime));
    xrt_billing_owner_drop(billing);
}
static RbcValueOrigin rbc_value_origin(RbcValueActivation *activation, uint32_t value) {
    while (activation) {
        const XrLirHomeBinding *binding=rbc_value_binding(activation->code,value);
        if (binding && binding->kind==XR_LIR_HOME_BINDING_CELL_BORROW) { value=binding->source; continue; }
        uint32_t parameter=binding && binding->kind==XR_LIR_HOME_BINDING_PARAMETER ? binding->parameter : 0;
        const XrLirValueHome *home=rbc_value_home(activation->code,value);
        if (home && home->borrow_source) { value=home->borrow_source; continue; }
        if (!parameter && home && home->parameter &&
            (home->owner_count || activation->code->locations[value].kind==XR_RBC_VALUE_NOWHERE)) parameter=home->parameter;
        if (!parameter) return (RbcValueOrigin){activation,value};
        if (parameter>activation->code->parameter_count) return (RbcValueOrigin){0};
        if (activation->borrowed_parameters[parameter-1]) return (RbcValueOrigin){activation,value};
        RbcValueOrigin origin=activation->parameters[parameter-1];
        activation=origin.activation; value=origin.value;
    }
    return (RbcValueOrigin){0};
}
static XrRbcRegister *rbc_value_register(XrRbcStack *stack, RbcValueActivation *activation, uint32_t value) {
    if (!activation || !rbc_value_id(activation->code,value)) return NULL;
    XrRbcFrame *frame = &stack->frames[activation->depth - 1];
    XrRbcValueLocation location = activation->code->locations[value];
    return location.kind == XR_RBC_VALUE_REGISTER ? &stack->slots[frame->base + location.index] :
        location.kind == XR_RBC_VALUE_SPILL ? &stack->spills[frame->spill_base + location.index] : NULL;
}
static XrtManagedStatus rbc_value_place(RbcValueOrigin origin, bool temporary, size_t leaf, XrtPlace *out) {
    if (!origin.activation) return XRT_MANAGED_ARGUMENT;
    const XrLirValueHome *home=rbc_value_home(origin.activation->code,origin.value);
    const XrtValueImage *image=origin.activation->image;
    if (!home || !image || !home->type || home->type>image->type_count) return XRT_MANAGED_ARGUMENT;
    XrtValueKind kind=image->types[home->type-1].kind;
    XrtPlaceNull nulls = kind == XRT_VALUE_STRING || xrt_value_kind_is_array(kind) ||
        kind == XRT_VALUE_MAP_STRING_I64 || kind == XRT_VALUE_SET_I64 || kind == XRT_VALUE_CLASS ?
        XRT_PLACE_NONNULL : XRT_PLACE_NULLABLE;
    return xr_rbc_value_home_place((XrRbcHomeLocation){origin.activation->code->homes,
        origin.activation->frame,origin.value,temporary},leaf,nulls,out);
}
/* The real Frame view already binds the sealed home to its payload. Reading
 * a committed local Header calls no provider and cannot run guest code. The
 * root's busy flag and the actual slot's pending state still forbid reads. */
static XrtManagedStatus rbc_value_borrow_header(RbcValueOrigin origin, const XrLirValueHome *home,
    const XrtManagedHeader **output) {
    RbcValueActivation *activation = origin.activation;
    if (home && activation->owned_code && activation->frame && !home->parameter &&
        home->owner_count == 1 && home->first_owner < activation->frame_locations.slot_count) {
        const XrtManagedFrameLocations *locations = &activation->frame_locations;
        const XrtManagedSlot *slot = &locations->slots[home->first_owner];
        XR_DCHECK(locations->frame == activation->frame && locations->payload == activation->payload &&
            slot->owner_offset == home->offset && slot->owner_offset != SIZE_MAX,
            "the owned Header borrow uses its real Frame payload");
        if (*locations->busy || slot->pending.value || slot->pending.allocation_bytes)
            return XRT_MANAGED_ARGUMENT;
        XrtManagedHeader *header = *(XrtManagedHeader **)(activation->payload + slot->owner_offset);
        if (!header) return XRT_MANAGED_ARGUMENT;
        *output = header;
        return XRT_MANAGED_OK;
    }
    XrtPlace place;
    XrtManagedStatus status = rbc_value_place(origin, false, 0, &place);
    if (status != XRT_MANAGED_OK) return status;
    place.nulls = XRT_PLACE_NONNULL;
    return xrt_place_borrow(place, output);
}
static XrtManagedStatus rbc_value_read_literal_cache(RbcValueActivation *activation,
    const XrLirValueHome *home, const XrtManagedHeader **out) {
    const XrtManagedFrameLocations *locations = &activation->frame_locations;
    if (!activation->owned_code || !activation->frame ||
        home->literal_cache_owner >= locations->slot_count) return XRT_MANAGED_ARGUMENT;
    const XrtManagedSlot *slot = &locations->slots[home->literal_cache_owner];
    XR_DCHECK(locations->frame == activation->frame && locations->payload == activation->payload &&
        slot->owner_offset == home->literal_cache_offset && slot->owner_offset != SIZE_MAX,
        "the admitted String cache uses its actual Frame slot");
    if (*locations->busy || slot->pending.value || slot->pending.allocation_bytes)
        return XRT_MANAGED_ARGUMENT;
    *out = *(XrtManagedHeader **)(activation->payload + slot->owner_offset);
    return XRT_MANAGED_OK;
}
static XrtManagedStatus rbc_value_string_borrow(RbcValueActivation *activation, uint32_t value,
    const XrtStringBacking **out) {
    RbcValueOrigin origin = rbc_value_origin(activation,value);
    const XrLirValueHome *home=origin.activation ? rbc_value_home(origin.activation->code,origin.value) : NULL;
    if (home && home->parameter && home->parameter<=origin.activation->code->parameter_count &&
        origin.activation->borrowed_parameters[home->parameter-1]) {
        const XrtStringBacking *borrowed=NULL;
        memcpy(&borrowed,origin.activation->borrowed_parameters[home->parameter-1],sizeof(borrowed));
        if (!borrowed) return XRT_MANAGED_ARGUMENT;
        *out=borrowed; return XRT_MANAGED_OK;
    }
    const XrtManagedHeader *header = NULL;
    XrtManagedStatus status = home && home->literal_cache_borrow ?
        rbc_value_read_literal_cache(origin.activation, home, &header) :
        rbc_value_borrow_header(origin, home, &header);
    if (status == XRT_MANAGED_OK && !header) return XRT_MANAGED_ARGUMENT;
    if (status == XRT_MANAGED_OK) *out=(const XrtStringBacking *)header;
    return status;
}
static XrtManagedStatus rbc_value_map_borrow(const XrRbcModule *module,RbcValueActivation *activation,
    uint32_t value,const XrtMapBacking **output) {
    RbcValueOrigin origin=rbc_value_origin(activation,value);
    const XrLirValueHome *home=origin.activation ? rbc_value_home(origin.activation->code,origin.value) : NULL;
    if (home && home->parameter &&
        home->parameter<=origin.activation->code->parameter_count &&
        origin.activation->borrowed_parameters[home->parameter-1] &&
        xr_rbc_module_code_owner(module) &&
        rbc_value_map(module,origin.activation->code,origin.value,false)) {
        const XrtMapBacking *map=NULL;
        memcpy(&map,origin.activation->borrowed_parameters[home->parameter-1],sizeof(map));
        if (!map) return XRT_MANAGED_ARGUMENT;
        *output=map; return XRT_MANAGED_OK;
    }
    const XrtManagedHeader *header=NULL;
    XrtManagedStatus status=rbc_value_borrow_header(origin,home,&header);
    if (status==XRT_MANAGED_OK) *output=(const XrtMapBacking *)header;
    return status;
}
/* Call admission has already proved the typed actual. The caller stays
 * alive while its stable managed Frame or the invocation's Env is borrowed. */
static RbcMapParameter rbc_map_parameter_source(const XrRbcModule *module, RbcValueOrigin origin) {
    RbcValueActivation *owner = origin.activation;
    if (!owner || !owner->owned_code || !rbc_value_map(module, owner->code, origin.value, false))
        return (RbcMapParameter){0};
    uint32_t formal = rbc_map_formal(owner->code, origin.value);
    if (formal != UINT32_MAX)
        return owner->parameter_storage[formal].map; /* Normalized Env capture. */

    const XrtManagedFrameLocations *locations = &owner->frame_locations;
    uint32_t slot = rbc_local_borrow_slot(owner->code->homes, origin.value);
    if (!slot || !locations->frame || slot > locations->slot_count) return (RbcMapParameter){0};
    const XrtManagedSlot *actual = &locations->slots[slot - 1];
    if (actual->owner_offset == SIZE_MAX) return (RbcMapParameter){0};
    return (RbcMapParameter){.address=(const uint8_t *)locations->payload + actual->owner_offset,
        .slot=actual, .busy=locations->busy};
}

static XrtManagedStatus rbc_value_set_borrow(const XrRbcModule *module,RbcValueActivation *activation,
    uint32_t value,const XrtSetBacking **output) {
    RbcValueOrigin origin=rbc_value_origin(activation,value);
    const XrLirValueHome *home=origin.activation ? rbc_value_home(origin.activation->code,origin.value) : NULL;
    if (!origin.activation || !rbc_value_set(module,origin.activation->code,origin.value,false)) return XRT_MANAGED_ARGUMENT;
    if (home->parameter && home->parameter<=origin.activation->code->parameter_count &&
        origin.activation->borrowed_parameters[home->parameter-1]) {
        const XrtSetBacking *set=NULL;
        memcpy(&set,origin.activation->borrowed_parameters[home->parameter-1],sizeof(set));
        if (!set) return XRT_MANAGED_ARGUMENT;
        *output=set; return XRT_MANAGED_OK;
    }
    const XrtManagedHeader *header=NULL;
    XrtManagedStatus status=rbc_value_borrow_header(origin,home,&header);
    if (status==XRT_MANAGED_OK) *output=(const XrtSetBacking *)header;
    return status;
}
static XrtManagedStatus rbc_value_payload(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueOrigin origin, RbcValuePayload *output) {
    if (!origin.activation) return XRT_MANAGED_ARGUMENT;
    RbcValueActivation *activation=origin.activation;
    const XrLirValueHome *home=rbc_value_home(activation->code,origin.value);
    if (!home || !home->type) {
        XrRbcRegister *scalar=rbc_value_register(stack,activation,origin.value);
        static const XrLirHomeBytes span={0,8};
        if (!scalar) return XRT_MANAGED_ARGUMENT;
        *output=(RbcValuePayload){.address=(uint8_t *)&scalar->bits,.spans=&span,.span_count=1,.size=8};
        return XRT_MANAGED_OK;
    }
    *output=(RbcValuePayload){.size=home->size,.spans=home->byte_count ? activation->code->homes->bytes+home->first_bytes : NULL,
        .span_count=home->byte_count};
    if (home->parameter && home->parameter<=activation->code->parameter_count &&
        activation->borrowed_parameters[home->parameter-1]) {
        output->address=(uint8_t *)activation->borrowed_parameters[home->parameter-1];
        return XRT_MANAGED_OK;
    }
    if (home->owner_count || activation->code->locations[origin.value].kind==XR_RBC_VALUE_NOWHERE) {
        if (!activation->payload || home->offset==SIZE_MAX) return XRT_MANAGED_ARGUMENT;
        output->address=activation->payload+home->offset;
    } else {
        XrRbcValueLocation location=activation->code->locations[origin.value];
        if (location.kind != XR_RBC_VALUE_AGGREGATE) return XRT_MANAGED_ARGUMENT;
        const XrRbcFunction *function=&module->functions[activation->function];
        XrRbcAggregateValue value=function->aggregate_values[location.index];
        output->packed=module->aggregate_types[value.type];
        output->lanes=output->packed.kind==XR_POD_FIXED_U8 && output->packed.count<=XR_RBC_AGGREGATE_INLINE_SCALARS;
        output->address=xr_rbc_aggregate_value(stack,&stack->frames[activation->depth-1],value);
    }
    return XRT_MANAGED_OK;
}
static XrtManagedStatus rbc_value_cell_borrow(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,uint32_t value,XrtManagedHeader **output) {
    RbcValuePayload payload;
    XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(activation,value),&payload);
    if (status!=XRT_MANAGED_OK) return status;
    if (payload.size!=sizeof(*output) || payload.lanes) return XRT_MANAGED_ARGUMENT;
    memcpy(output,payload.address,sizeof(*output));
    return *output ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
}
#include "xrbc_value_slice_boundary.inc.c"
static void rbc_value_publication(void *context) {
    RbcValueActivation *activation=context;
    for (size_t p=0;p<activation->publication_count;++p) {
        RbcValuePayload *clear=&activation->publications[p].clear;
        for (size_t b=0;b<clear->span_count;++b)
            memset(clear->address+clear->spans[b].offset,0,clear->spans[b].size);
    }
    for (size_t p=0;p<activation->publication_count;++p) {
        RbcValuePublication *row=&activation->publications[p];
        if (row->destination.lanes) xr_rbc_aggregate_unpack(row->destination.packed,row->snapshot,row->destination.address);
        else for (size_t b=0;b<row->destination.span_count;++b) {
            XrLirHomeBytes span=row->destination.spans[b];
            memcpy(row->destination.address+span.offset,row->snapshot+span.offset,span.size);
        }
    }
}
static XrtManagedStatus rbc_value_snapshot(RbcValueActivation *scratch, RbcValuePayload destination,
    RbcValuePayload source, bool clear, size_t *bytes) {
    if (scratch->publication_count>=scratch->publication_capacity || destination.size!=source.size ||
        *bytes>scratch->snapshot_capacity || source.size>scratch->snapshot_capacity-*bytes) return XRT_MANAGED_ARGUMENT;
    uint8_t *snapshot=scratch->snapshots+*bytes;
    if (source.lanes) xr_rbc_aggregate_pack(source.packed,source.address,snapshot);
    else if (source.size) memcpy(snapshot,source.address,source.size);
    scratch->publications[scratch->publication_count++]=(RbcValuePublication){destination,
        clear ? source : (RbcValuePayload){0},snapshot};
    *bytes+=source.size;
    return XRT_MANAGED_OK;
}
typedef struct RbcValueEdgeBuilder {
    const XrRbcModule *module;
    XrRbcStack *stack;
    RbcValueActivation *activation;
    size_t owners,bytes;
    /* Logical scratch bounds include owner-only pairs; actual publications
     * contain only rows that still need their non-owner bytes transported. */
    size_t rows;
} RbcValueEdgeBuilder;
static XrtManagedStatus rbc_value_edge_pair(RbcValueEdgeBuilder *builder, uint32_t source, uint32_t target,
    XrLirOwnershipTransferMode transfer,const XrLirHomeTransfer *loan) {
    RbcValueActivation *activation=builder->activation;
    const XrRbcValueCode *code=activation->code;
    RbcValueOrigin from=rbc_value_origin(activation,source),to={activation,target};
    const XrLirValueHome *home=rbc_value_home(code,target);
    const XrLirValueHome *source_home=rbc_value_home(code,source);
    if (transfer!=XR_LIR_OWNERSHIP_TRANSFER_NONE && transfer!=XR_LIR_OWNERSHIP_TRANSFER_BORROW && transfer!=XR_LIR_OWNERSHIP_TRANSFER_COPY &&
        transfer!=XR_LIR_OWNERSHIP_TRANSFER_MOVE) return XRT_MANAGED_ARGUMENT;
    if (loan && loan->target_workspace) {
        const XrLirHomeWorkspace *source_workspace=rbc_value_slice_workspace(code,loan->source_workspace);
        const XrLirHomeWorkspace *target_workspace=rbc_value_slice_workspace(code,loan->target_workspace);
        const XrLirHomeWorkspace *stage=loan->source_storage==XR_LIR_SLICE_STORAGE_BORROWED_FORMAL ?
            rbc_value_slice_stage(code,*loan) : NULL;
        if ((!source_workspace && !stage) || !target_workspace || loan->hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_COPY ||
            builder->owners>=activation->edge_capacity || (!stage && from.activation!=activation)) return XRT_MANAGED_ARGUMENT;
        activation->edges[builder->owners++]=(XrtPlaceEdge){
            .destination={.frame=activation->frame,.slot=target_workspace->first_owner,.nulls=XRT_PLACE_NULLABLE},
            .source={.frame=activation->frame,.slot=stage ? stage->first_owner : source_workspace->first_owner,.nulls=XRT_PLACE_NONNULL},
            .temporary={.frame=activation->frame,.slot=target_workspace->temporary_first_owner,.nulls=XRT_PLACE_NULLABLE},
            .transfer=stage ? XRT_PLACE_TRANSFER_MOVE : XRT_PLACE_TRANSFER_COPY};
    }
    if (home && home->owner_count) {
        const XrLirHomeWorkspace *stage=loan && loan->borrowed_owner_workspace!=SIZE_MAX ?
            rbc_value_borrow_stage(code,*loan) : NULL;
        if (!source_home || (source_home->parameter && !stage) || builder->owners>activation->edge_capacity ||
            home->owner_count>activation->edge_capacity-builder->owners) return XRT_MANAGED_ARGUMENT;
        for (size_t leaf=0;leaf<home->owner_count;++leaf) {
            XrtPlaceEdge *edge=&activation->edges[builder->owners++];
            if (transfer!=XR_LIR_OWNERSHIP_TRANSFER_COPY && transfer!=XR_LIR_OWNERSHIP_TRANSFER_MOVE)
                return XRT_MANAGED_ARGUMENT;
            edge->transfer=!stage && transfer==XR_LIR_OWNERSHIP_TRANSFER_COPY ? XRT_PLACE_TRANSFER_COPY : XRT_PLACE_TRANSFER_MOVE;
            XrtManagedStatus status=rbc_value_place(to,false,leaf,&edge->destination);
            if (status==XRT_MANAGED_OK) {
                if (stage) edge->source=(XrtPlace){.frame=activation->frame,.slot=stage->first_owner,.nulls=XRT_PLACE_NONNULL};
                else status=rbc_value_place(from,false,leaf,&edge->source);
            }
            if (status==XRT_MANAGED_OK) status=rbc_value_place(to,true,leaf,&edge->temporary);
            if (status!=XRT_MANAGED_OK) return status;
        }
    }
    if (home && source_home && from.activation==activation && from.value==source && activation->payload &&
        home->owner_count==1 && source_home->owner_count==1 && home->type==source_home->type &&
        home->size==sizeof(XrtManagedHeader *) && source_home->size==home->size &&
        !home->byte_count && !source_home->byte_count && !home->parameter && !source_home->parameter &&
        home->transport==XR_LIR_CALLABLE_TRANSPORT_NONE && source_home->transport==XR_LIR_CALLABLE_TRANSPORT_NONE &&
        !home->borrow_source && !source_home->borrow_source && !home->view_workspace && !source_home->view_workspace &&
        loan && loan->borrowed_owner_workspace==SIZE_MAX && !loan->source_workspace && !loan->target_workspace &&
        loan->hold_transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE && loan->source_storage==XR_LIR_SLICE_STORAGE_NONE) {
        /* The owner transaction transports the complete representation. Keep
         * logical scratch bounds without materializing an empty byte publication. */
        if (builder->rows>=activation->publication_capacity || builder->bytes>activation->snapshot_capacity ||
            home->size>activation->snapshot_capacity-builder->bytes) return XRT_MANAGED_ARGUMENT;
        ++builder->rows; builder->bytes+=home->size;
        return XRT_MANAGED_OK;
    }
    RbcValuePayload destination,source_payload;
    XrtManagedStatus status=rbc_value_payload(builder->module,builder->stack,to,&destination);
    if (status==XRT_MANAGED_OK) status=rbc_value_payload(builder->module,builder->stack,from,&source_payload);
    if (status==XRT_MANAGED_OK && builder->rows>=activation->publication_capacity) return XRT_MANAGED_ARGUMENT;
    if (status==XRT_MANAGED_OK) status=rbc_value_snapshot(activation,destination,source_payload,
        home && home->owner_count && transfer==XR_LIR_OWNERSHIP_TRANSFER_MOVE,&builder->bytes);
    if (status==XRT_MANAGED_OK) ++builder->rows;
    return status;
}
static void rbc_value_edge_unstage(RbcValueActivation *activation,const XrLirHomeEdge *edge,size_t prepared) {
    while (prepared) {
        XrLirHomeTransfer transfer=activation->code->homes->transfers[edge->first+--prepared];
        if (transfer.borrowed_owner_workspace==SIZE_MAX) continue;
        const XrLirHomeWorkspace *stage=rbc_value_borrow_stage(activation->code,transfer);
        XR_CHECK(stage && xrt_managed_slot_drop(activation->frame,stage->first_owner)==XRT_MANAGED_OK,
            "an edge retires only borrowed owners it staged in real homes");
    }
}
static XrtManagedStatus rbc_value_edge_stage(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,const XrLirHomeEdge *edge,size_t *prepared) {
    for (size_t p=0;p<edge->count;++p) {
        XrLirHomeTransfer transfer=activation->code->homes->transfers[edge->first+p];
        if (transfer.borrowed_owner_workspace!=SIZE_MAX) {
            const XrLirHomeWorkspace *stage=rbc_value_borrow_stage(activation->code,transfer);
            if (!stage) return XRT_MANAGED_ARGUMENT;
            XrtManagedHeader *previous=NULL;
            XrtManagedStatus status=xrt_managed_slot_borrow(activation->frame,stage->first_owner,&previous);
            if (status!=XRT_MANAGED_OK || previous) return status!=XRT_MANAGED_OK ? status : XRT_MANAGED_ARGUMENT;
            XrtManagedHeader *cell=NULL;
            status=rbc_value_borrow_stage_owner(module,stack,activation,transfer,&cell);
            if (status!=XRT_MANAGED_OK) return status;
            if (!xrt_managed_retain(cell)) return XRT_MANAGED_REFCOUNT;
            status=xrt_managed_slot_replace(activation->frame,stage->first_owner,&cell);
            xrt_managed_drop(cell);
            if (status!=XRT_MANAGED_OK) return status;
        }
        *prepared=p+1;
    }
    return XRT_MANAGED_OK;
}
/* Called only after checking the exact ordinary move. Once the capacity
 * helper changes bytes, the caller publishes that move without a fallback. */
static XR_NOINLINE uint64_t rbc_value_append_window(XrRbcStack *stack, RbcValueActivation *activation,
    const XrLirHomeEdge *edge, const uint32_t *pair, uint64_t bits) {
    const XrRbcValueCode *code = activation->code;
    const XrLirValueHomes *homes = code->homes;
    XR_DCHECK(edge->string_window && edge->string_window <= homes->string_window_count,
        "the sealed latch names its own String window");
    const XrLirStringWindow *window = &homes->string_windows[edge->string_window - 1];
    XR_DCHECK(window->latch_empty_first <= homes->string_window_slot_count &&
        window->empty_count <= homes->string_window_slot_count - window->latch_empty_first &&
        window->latch_owner == homes->values[homes->transfers[edge->first].source].first_owner &&
        window->latch_target_owner == homes->values[homes->transfers[edge->first].target].first_owner,
        "the capacity window starts and ends at the actual selected owner slots");
    if (pair[0] != window->next_index || pair[1] != window->latch_index || bits > INT64_MAX) return bits;
    uint64_t limit_bits = window->limit_bits;
    if (!window->constant_limit) {
        XrRbcRegister *limit = rbc_value_register(stack, activation, window->limit);
        if (!limit) return bits;
        limit_bits = limit->bits;
    }
    if (limit_bits > INT64_MAX || bits >= limit_bits) return bits;
    uint64_t remaining = limit_bits - bits;
    size_t count = remaining > SIZE_MAX ? SIZE_MAX : (size_t)remaining;
    return bits + xrt_native_string_append_window(activation->frame, window->latch_owner,
        window->suffix_owner, count, homes->string_window_slots + window->latch_empty_first, window->empty_count);
}

/* This edge moves one complete Header and otherwise transports raw scalar
 * bits. Snapshot all changed destinations before publishing any of them. */
static XR_NOINLINE bool rbc_value_snapshot_edge_scalars(XrRbcStack *stack, RbcValueActivation *activation,
    XrRbcValueAccess access, uint32_t owner_target, size_t *count) {
    size_t scalars = access.count - 1;
    if (scalars > activation->publication_capacity ||
        scalars > activation->snapshot_capacity / sizeof(uint64_t)) return false;
    const uint32_t *pairs = activation->code->arguments + access.first;
    for (uint32_t p = 0; p < access.count; ++p, pairs += 2) {
        if (pairs[1] == owner_target) continue;
        RbcValueOrigin origin = rbc_value_origin(activation, pairs[0]);
        XrRbcRegister *from = rbc_value_register(stack, origin.activation, origin.value);
        XrRbcRegister *to = rbc_value_register(stack, activation, pairs[1]);
        if (!from || !to) return false;
        if (from == to) continue;
        activation->publications[*count].destination.address = (uint8_t *)&to->bits;
        memcpy(activation->snapshots + *count * sizeof(uint64_t), &from->bits, sizeof(uint64_t));
        ++*count;
    }
    return true;
}

/* The first iteration has already taken the ordinary path. A failed guard
 * leaves subsequent iterations there too, including the original failing GET.
 * Its complete latch publication put the owner and scalars in their header homes. */
static XR_NOINLINE void rbc_value_array_read_fold(XrRbcStack *stack,
    RbcValueActivation *activation, uint32_t region) {
    const XrRbcValueCode *code=activation->code;
    const XrLirArrayRegionBindings *bindings=code->array_regions;
    const XrLirArrayRegionBinding *row=&bindings->regions[region-1];
    const XrLirArraySetRegionRange *range=&row->region.range;
    const XrRbcArrayReadFold *fold=&code->array_read_folds[region-1];
    XrRbcRegister *iv=rbc_value_register(stack,activation,range->iv);
    XrRbcRegister *accumulator=rbc_value_register(stack,activation,fold->accumulator);
    if (!iv || !accumulator || iv==accumulator) return;
    uint64_t start=iv->bits, limit=fold->limit_bits;
    if (fold->limit) {
        XrRbcRegister *bound=rbc_value_register(stack,activation,fold->limit);
        if (!bound) return;
        limit=bound->bits;
    }
    if (start>INT64_MAX || limit>INT64_MAX ||
        (range->kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT && start>=limit))
        return;
    const XrtArrayI64RegionContext context={activation->frame,activation->payload,
        &activation->frame_locations,&code->homes->frame};
    const XrtArrayI64RegionSpec spec={bindings->slots+row->first_slot,row->slot_count,row->canonical_slot-row->first_slot};
    XrtArrayI64RegionCache cache={0};
    if (!xrt_array_i64_region_try_begin(&context,&spec,true,&cache)) return;
    if (range->kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN) limit=cache.length;
    if (start>=limit || limit>cache.length) return;
    uint64_t sum=accumulator->bits;
    for (size_t n=(size_t)start;n<(size_t)limit;++n) {
        uint64_t item;
        memcpy(&item,cache.elements+n*sizeof(item),sizeof(item));
        sum+=item;
    }
    iv->bits=limit;
    accumulator->bits=sum;
}

static bool rbc_value_edge_move_header(XrRbcStack *stack, RbcValueActivation *activation,
    XrRbcValueAccess access, const XrLirHomeEdge *edge) {
    if (!access.count || !activation->frame) return false;
    const XrRbcValueCode *code = activation->code;
    const XrLirValueHomes *homes = code->homes;
    XrLirHomeTransfer transfer = homes->transfers[edge->first];
    const XrLirValueHome *from = &homes->values[transfer.source], *to = &homes->values[transfer.target];
    XrRbcRegister *scalar = NULL;
    const uint32_t *pair = NULL;
    uint64_t bits = 0;
    if (access.count == 2) {
        pair = code->arguments + access.first;
        if (pair[1] == transfer.target) pair += 2;
        RbcValueOrigin origin = rbc_value_origin(activation, pair[0]);
        if (origin.activation != activation || origin.value != pair[0]) return false;
        XrRbcRegister *input = rbc_value_register(stack, activation, pair[0]);
        scalar = rbc_value_register(stack, activation, pair[1]);
        if (!input || !scalar) return false;
        memcpy(&bits, input, sizeof(bits));
    }
    const XrtManagedFrameLocations *locations = &activation->frame_locations;
    const XrtManagedSlot *slots = locations->slots;
    XR_DCHECK(locations->frame == activation->frame && locations->payload == activation->payload &&
        locations->slot_count == homes->frame.slot_count &&
        slots[from->first_owner].owner_offset == from->offset && slots[to->first_owner].owner_offset == to->offset &&
        slots[to->temporary_first_owner].owner_offset == to->temporary_offset,
        "the admitted move uses the actual Frame fields of its common plan");
    if (*locations->busy) return false;
    const XrtManagedPending *input = &slots[from->first_owner].pending;
    const XrtManagedPending *output = &slots[to->first_owner].pending;
    const XrtManagedPending *scratch = &slots[to->temporary_first_owner].pending;
    if (input->value || input->allocation_bytes || output->value || output->allocation_bytes ||
        scratch->value || scratch->allocation_bytes) return false;
    XrtManagedHeader **source = (XrtManagedHeader **)(activation->payload + from->offset);
    XrtManagedHeader **destination = (XrtManagedHeader **)(activation->payload + to->offset);
    XrtManagedHeader **temporary = (XrtManagedHeader **)(activation->payload + to->temporary_offset);
    /* One executor owns this Frame. There is no external observation, failure
     * or callback between checking the actual busy state and publishing both
     * destinations. The general transaction retires any occupied result. */
    if (!*source || *destination || *temporary) return false;
    size_t moves = 0;
    if (access.count > 2 && !rbc_value_snapshot_edge_scalars(stack, activation, access, transfer.target, &moves))
        return false;
    if (pair && edge->string_window) bits = rbc_value_append_window(stack, activation, edge, pair, bits);
    *destination = *source;
    *source = NULL;
    if (scalar) scalar->bits = bits;
    for (size_t n = 0; n < moves; ++n)
        memcpy(activation->publications[n].destination.address,
            activation->snapshots + n * sizeof(uint64_t), sizeof(uint64_t));
    return true;
}
static XrtManagedStatus rbc_value_edge(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, XrRbcValueAccess access) {
    const XrRbcValueCode *code=activation->code;
    RbcValueEdgeBuilder builder={.module=module,.stack=stack,.activation=activation};
    activation->publication_count=0;
    const XrLirHomeEdge *selected=NULL; size_t prepared=0;
    XrtManagedStatus status=XRT_MANAGED_OK;
    if (access.index!=UINT32_MAX) {
        selected=&code->homes->edges[access.index];
        bool fold_array=false;
        if (access.other) {
            if (code->array_regions->edges[access.index].kind==XR_LIR_ARRAY_REGION_EDGE_ENTRY)
                activation->pending_array_read_fold=access.other;
            else if (activation->pending_array_read_fold==access.other) {
                activation->pending_array_read_fold=0;
                fold_array=true;
            }
        }
        if (activation->owned_code && selected->direct_header_move &&
            rbc_value_edge_move_header(stack, activation, access, selected)) {
            if (fold_array) rbc_value_array_read_fold(stack,activation,access.other);
            return XRT_MANAGED_OK;
        }
        status=rbc_value_edge_stage(module,stack,activation,selected,&prepared);
        for (size_t p=0;status==XRT_MANAGED_OK && p<selected->count;++p) {
            XrLirHomeTransfer transfer=code->homes->transfers[selected->first+p];
            status=rbc_value_edge_pair(&builder,transfer.source,transfer.target,transfer.transfer_mode,&transfer);
        }
    }
    for (uint32_t p=0;status==XRT_MANAGED_OK && p<access.count;++p) {
        uint32_t source=code->arguments[access.first+(size_t)p*2],target=code->arguments[access.first+(size_t)p*2+1];
        const XrLirValueHome *home=rbc_value_home(code,target);
        if (home && home->type) continue;
        status=rbc_value_edge_pair(&builder,source,target,XR_LIR_OWNERSHIP_TRANSFER_BORROW,NULL);
    }
    const XrtPlacePublication publication={rbc_value_publication,activation};
    if (status==XRT_MANAGED_OK) status=xrt_place_edge_transfer(activation->edges,builder.owners,&publication);
    if (selected) rbc_value_edge_unstage(activation,selected,prepared);
    return status;
}

static XrtManagedStatus rbc_value_inline_replace(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueOrigin destination,const void *borrowed_source) {
    if (!destination.activation) return XRT_MANAGED_ARGUMENT;
    RbcValueActivation *activation=destination.activation;
    const XrLirValueHome *home=rbc_value_home(activation->code,destination.value);
    if (!home || !home->type || home->parameter || home->type>activation->image->type_count) return XRT_MANAGED_ARGUMENT;
    const XrtValueImageType *type=&activation->image->types[home->type-1];
    RbcValuePayload payload;
    XrtManagedStatus status=rbc_value_payload(module,stack,destination,&payload);
    if (status!=XRT_MANAGED_OK) return status;
    XrtInlineValueSpec spec={type->size,type->alignment,home->owner_count ? activation->image->owned_offsets+type->first_owned : NULL,home->owner_count};
    XrtInlineValueHome storage={.value=payload.address};
    if (home->owner_count) {
        if (!activation->payload || home->temporary_offset==SIZE_MAX) return XRT_MANAGED_ARGUMENT;
        storage.frame=activation->frame; storage.temporary=activation->payload+home->temporary_offset;
        storage.first_owner=home->first_owner; storage.temporary_first_owner=home->temporary_first_owner;
    }
    return xrt_inline_value_replace(&storage,&spec,borrowed_source);
}
static XrtManagedStatus rbc_value_replace(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *scratch, RbcValueOrigin destination, RbcValueOrigin source, bool copy) {
    const XrLirValueHome *home=rbc_value_home(destination.activation->code,destination.value);
    const XrLirValueHome *from=rbc_value_home(source.activation->code,source.value);
    if (!home || !from || home->type!=from->type || home->owner_count>scratch->edge_capacity) return XRT_MANAGED_ARGUMENT;
    scratch->publication_count=0;
    RbcValuePayload output,input; size_t bytes=0;
    XrtManagedStatus status=rbc_value_payload(module,stack,destination,&output);
    if (status==XRT_MANAGED_OK) status=rbc_value_payload(module,stack,source,&input);
    if (status==XRT_MANAGED_OK) status=rbc_value_snapshot(scratch,output,input,!copy,&bytes);
    if (status!=XRT_MANAGED_OK) return status;
    const XrtPlacePublication publication={rbc_value_publication,scratch};
    if (copy && rbc_value_inline(module,destination.activation->code,destination.value,true))
        return rbc_value_inline_replace(module,stack,destination,input.address);
    if (copy && from->parameter &&
        from->parameter<=source.activation->code->parameter_count &&
        source.activation->borrowed_parameters[from->parameter-1] &&
        xr_rbc_module_code_owner(module) &&
        rbc_value_map(module,source.activation->code,source.value,false) &&
        rbc_value_map(module,destination.activation->code,destination.value,true))
        return rbc_value_inline_replace(module,stack,destination,input.address);
    if (copy) {
        if (home->owner_count!=1) return XRT_MANAGED_ARGUMENT;
        XrtPlace to,from_place;
        status=rbc_value_place(destination,false,0,&to);
        if (status==XRT_MANAGED_OK) status=rbc_value_place(source,false,0,&from_place);
        return status==XRT_MANAGED_OK ? xrt_place_copy_replace(to,from_place,&publication) : status;
    }
    for (size_t leaf=0;leaf<home->owner_count;++leaf) {
        XrtPlaceEdge *edge=&scratch->edges[leaf];
        edge->transfer=XRT_PLACE_TRANSFER_MOVE;
        status=rbc_value_place(destination,false,leaf,&edge->destination);
        if (status==XRT_MANAGED_OK) status=rbc_value_place(source,false,leaf,&edge->source);
        if (status==XRT_MANAGED_OK) status=rbc_value_place(destination,true,leaf,&edge->temporary);
        if (status!=XRT_MANAGED_OK) return status;
    }
    return xrt_place_edge_transfer(scratch->edges,home->owner_count,&publication);
}
static XrtManagedStatus rbc_value_literal_factory(const XrRbcModule *module, RbcValueActivation *activation,
    XrRbcValueAccess access, uint32_t source, const XrLirValueHome *home) {
    XrtPlace cache={.frame=activation->frame,.slot=home->literal_cache_owner,.nulls=XRT_PLACE_NULLABLE};
    const XrtManagedHeader *cached=NULL;
    XrtManagedStatus status = home->literal_cache_borrow ?
        rbc_value_read_literal_cache(activation, home, &cached) : xrt_place_borrow(cache, &cached);
    if (status != XRT_MANAGED_OK) return status;
    if (!cached) {
        const XrtBackingTypes *types=NULL;
        status=xrt_native_values_backings(xrt_value_root_current(source),&types);
        if (status!=XRT_MANAGED_OK) return status;
        XrRbcLiteral literal=module->literals[access.index];
        status=xrt_string_backing_create(activation->frame,home->literal_cache_owner,types,literal.bytes,literal.length,0);
    }
    if (status!=XRT_MANAGED_OK || home->literal_cache_borrow) return status;
    XrtPlace destination;
    status=rbc_value_place((RbcValueOrigin){activation,access.result},false,0,&destination);
    cache.nulls=XRT_PLACE_NONNULL;
    return status==XRT_MANAGED_OK ? xrt_place_copy_replace(destination,cache,NULL) : status;
}
static XrtManagedStatus rbc_value_factory(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, XrRbcValueAccess access, uint32_t source) {
    const XrLirValueHome *home=rbc_value_home(activation->code,access.result);
    if (access.operation==XR_RBC_VALUE_STRING_LITERAL)
        return rbc_value_literal_factory(module,activation,access,source,home);
    const XrtBackingTypes *types=NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source),&types);
    if (status!=XRT_MANAGED_OK) return status;
    const XrtStringBacking *left=NULL,*right=NULL;
    if (access.operation==XR_RBC_VALUE_STRING_CONCAT) {
        status=rbc_value_string_borrow(activation,access.source,&left);
        if (status==XRT_MANAGED_OK) status=rbc_value_string_borrow(activation,access.other,&right);
        if (status==XRT_MANAGED_OK) status=xrt_string_backing_concat_create(activation->frame,
            home->temporary_first_owner,types,left,right,0);
    } else {
        XrRbcRegister *input=rbc_value_register(stack,activation,access.source);
        if (!input) return XRT_MANAGED_ARGUMENT;
        if (access.operation==XR_RBC_VALUE_STRING_I64) {
            int64_t integer; memcpy(&integer,&input->bits,sizeof(integer));
            status=xrt_string_backing_i64_create(activation->frame,home->temporary_first_owner,types,integer);
        } else status=xrt_string_backing_f64_create(activation->frame,home->temporary_first_owner,types,input->bits);
    }
    if (status!=XRT_MANAGED_OK) return status;
    XrtPlace destination,temporary;
    RbcValueOrigin result={activation,access.result};
    status=rbc_value_place(result,false,0,&destination);
    if (status==XRT_MANAGED_OK) status=rbc_value_place(result,true,0,&temporary);
    return status==XRT_MANAGED_OK ? xrt_place_move_replace(destination,temporary,NULL) : status;
}
static XrtManagedStatus rbc_value_string_append(RbcValueActivation *activation,
    XrRbcValueAccess access,uint32_t source) {
    const XrtBackingTypes *types=NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source),&types);
    const XrtStringBacking *suffix=NULL;
    if (status!=XRT_MANAGED_OK) return status;
    status=rbc_value_string_borrow(activation,access.other,&suffix);
    if (status!=XRT_MANAGED_OK) return status;
    const XrLirValueHome *from=rbc_value_home(activation->code,access.source),
        *to=rbc_value_home(activation->code,access.result);
    XrtStringAppendOwned update={.source_slot=from->first_owner,.result_slot=to->first_owner,
        .temporary_slot=to->temporary_first_owner,.suffix=suffix};
    return xrt_string_backing_append_owned(activation->frame,types,&update);
}
static XrtManagedStatus rbc_value_function_state(const XrRbcModule *,XrRbcStack *,RbcValueActivation *,XrRbcValueAccess);
static XrtManagedStatus rbc_value_state(const XrRbcModule *module,XrRbcStack *stack,RbcValueActivation *activation,
    XrRbcValueAccess access) {
    XrRbcRegister *operand=rbc_value_register(stack,activation,access.source);
    if (!operand) return XRT_MANAGED_ARGUMENT;
    if (access.operation==XR_RBC_VALUE_STATE_GROUP_INITIALIZE) {
        RbcValuePayload payload;
        XrtManagedStatus status = rbc_value_payload(module,stack,rbc_value_origin(activation,access.other),&payload);
        if (status != XRT_MANAGED_OK) return status;
        const XrLirValueHome *home = rbc_value_home(activation->code,access.other);
        const XrtValueImageType *type = &activation->image->types[home->type - 1];
        return xrt_module_initialize_i64_group(operand->state,access.index,(uint32_t)type->field_count,
            payload.address,payload.size);
    }
    if (access.operation==XR_RBC_VALUE_STATE_READ || access.operation==XR_RBC_VALUE_STATE_WRITE) {
        uint32_t value=access.operation==XR_RBC_VALUE_STATE_READ ? access.result : access.other;
        const XrLirValueHome *home=rbc_value_home(activation->code,value);
        if (!operand->state_ref || !home || !home->type || home->type>activation->image->type_count ||
            operand->state_ref->kind!=activation->image->types[home->type-1].kind ||
            operand->state_ref->type!=home->type) return XRT_MANAGED_ARGUMENT;
    }
    uint32_t inline_value_id=access.operation==XR_RBC_VALUE_STATE_READ ? access.result : access.other;
    if (rbc_value_function(module,activation->code,inline_value_id,false))
        return rbc_value_function_state(module,stack,activation,access);
    if (access.operation==XR_RBC_VALUE_STATE_READ && rbc_value_inline(module,activation->code,access.result,true)) {
        const void *borrow=NULL;
        XrtManagedStatus status=xrt_module_borrow(operand->state_ref->reference,&borrow);
        return status==XRT_MANAGED_OK ? rbc_value_inline_replace(module,stack,(RbcValueOrigin){activation,access.result},borrow) : status;
    }
    if (access.operation==XR_RBC_VALUE_STATE_READ) {
        XrtExternalOwner external;
        XrtManagedStatus status=xrt_module_external_owner(operand->state_ref->reference,0,&external);
        XrtPlace destination;
        if (status==XRT_MANAGED_OK) status=rbc_value_place((RbcValueOrigin){activation,access.result},false,0,&destination);
        return status==XRT_MANAGED_OK ? xrt_place_copy_replace(destination,
            (XrtPlace){.external=&external,.nulls=XRT_PLACE_NONNULL},NULL) : status;
    }
    RbcValuePayload inline_input={0};
    bool inline_value=rbc_value_inline(module,activation->code,access.other,false);
    XrtManagedStatus status=XRT_MANAGED_OK;
    XrtPlace place; const XrtManagedHeader *header=NULL;
    if (inline_value) status=rbc_value_payload(module,stack,rbc_value_origin(activation,access.other),&inline_input);
    else if (rbc_value_map(module,activation->code,access.other,false)) {
        const XrtMapBacking *map=NULL;
        status=rbc_value_map_borrow(module,activation,access.other,&map);
        header=(const XrtManagedHeader *)map;
    } else {
        status=rbc_value_place(rbc_value_origin(activation,access.other),false,0,&place);
        if (status==XRT_MANAGED_OK) status=xrt_place_borrow(place,&header);
    }
    if (status!=XRT_MANAGED_OK) return status;
    XrtManagedHeader *borrow=(XrtManagedHeader *)header;
    const void *value=inline_value ? (const void *)inline_input.address : (const void *)&borrow;
    if (access.operation==XR_RBC_VALUE_STATE_WRITE) return xrt_module_replace(operand->state_ref->reference,value);
    if (access.operation==XR_RBC_VALUE_STATE_INITIALIZE) return xrt_module_initialize(operand->state,access.index,value);
    XrtModuleRef *created=NULL;
    status=xrt_module_construct(operand->state,access.index,value,&created);
    if (status==XRT_MANAGED_OK) {
        XrtModuleAccess *cache=&stack->module_accesses[(size_t)access.index*2];
        status=xrt_module_reference_cache(created,cache);
        XrRbcRegister *result=rbc_value_register(stack,activation,access.result);
        if (status==XRT_MANAGED_OK && !result) status=XRT_MANAGED_ARGUMENT;
        if (status==XRT_MANAGED_OK) result->state_ref=cache;
    }
    return status;
}
#include "xrbc_value_callable_execute.inc.c"
static XrRbcStatus rbc_value_call(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *caller, XrRbcValueAccess access, size_t pc, uint32_t source) {
    size_t parent=stack->depth-1;
    XrRbcFrame *frame=&stack->frames[parent];
    bool indirect=access.operation==XR_RBC_VALUE_FUNCTION_CALL;
    bool invoke=access.operation==XR_RBC_VALUE_INVOKE;
    /* Keep setup evidence across known guest calls, but do not carry it
     * through a callable boundary whose external lifetime is not assumed. */
    if (indirect) for (RbcValueActivation *a=caller;a;a=a->previous) a->backings_ready=false;
    RbcFunctionCall invoked={0};
    if (indirect) {
        XrtFunction binding;
        XrtManagedStatus status=rbc_value_function_read(module,stack,caller,access.source,&binding);
        if (status==XRT_MANAGED_OK) status=rbc_value_function_target(module,stack,&binding,access.index,&invoked);
        if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
        if (invoked.entry->kind==XR_RBC_CALLABLE_ENTRY_ASSERT_CONDITION) {
            const uint32_t *arguments=caller->code->arguments+access.first;
            XrRbcRegister *condition=rbc_value_register(stack,caller,arguments[0]);
            const XrtStringBacking *message=NULL;
            status=rbc_value_string_borrow(caller,arguments[1],&message);
            if (status==XRT_MANAGED_OK && (!condition || condition->bits>1)) status=XRT_MANAGED_ARGUMENT;
            if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
            xrt_assert_condition((uint8_t)condition->bits,message,source);
            stack->frames[parent].pc=pc;
            return XR_RBC_OK;
        }
        access.index=invoked.entry->physical_function;
    }
    const XrRbcFunction *callee=&module->functions[access.index];
    const XrRbcValueCode *receiving=xr_rbc_module_value_code(module,access.index);
    uint32_t captures=indirect ? callee->capture_count : 0;
    for (uint32_t p=0;receiving && p<access.count;++p) {
        uint32_t to=receiving->parameters[captures+p];
        if (!rbc_value_function(module,receiving,to,false)) continue;
        XrtFunction binding; RbcFunctionCall argument;
        XrtManagedStatus status=rbc_value_function_read(module,stack,caller,caller->code->arguments[access.first+p],&binding);
        if (status==XRT_MANAGED_OK) status=rbc_value_function_target(module,stack,&binding,
            rbc_value_signature(module,receiving,to),&argument);
        bool escaping=false;
        if (indirect) escaping=module->callable_types[callee->callable-1].parameters[p].escaping;
        else if (module->function_callable_parameters) {
            XrRbcFunctionCallableParameters parameters=module->function_callable_parameters[access.index];
            for (uint32_t n=0;n<parameters.count;++n)
                if (parameters.rows[n].parameter==p) escaping=parameters.rows[n].contract.escaping;
        }
        if (status==XRT_MANAGED_OK && escaping && !binding.owner) status=XRT_MANAGED_ARGUMENT;
        if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    }
    size_t window=indirect ? module->functions[frame->function].register_count : (size_t)access.window+1;
    if (frame->base>SIZE_MAX-window) return XR_RBC_OUT_OF_MEMORY;
    size_t base=frame->base+window;
    if (!xr_rbc_call_reserve(stack,base,callee)) return XR_RBC_OUT_OF_MEMORY;
    /* Reserve may move either bank. Snapshot every non-owner argument before
     * entry initialization or overlapping call-window writes touch a source. */
    size_t captured=0;
    for (uint32_t p=0;p<access.count;++p) {
        uint32_t value=caller->code->arguments[access.first+p];
        const XrLirValueHome *home=rbc_value_home(caller->code,value);
        caller->publications[p]=(RbcValuePublication){0};
        if ((home && home->owner_count) || (rbc_value_binding(caller->code,value) &&
                !rbc_value_binding_state_cap(caller->code,value)) || rbc_value_inline(module,caller->code,value,false) ||
            rbc_value_slice(module,caller->code,value)) continue;
        RbcValuePayload payload;
        XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(caller,value),&payload);
        if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
        if (captured>caller->snapshot_capacity || payload.size>caller->snapshot_capacity-captured) return XR_RBC_INVALID;
        uint8_t *snapshot=caller->snapshots+captured;
        if (payload.lanes) xr_rbc_aggregate_pack(payload.packed,payload.address,snapshot);
        else if (payload.size) memcpy(snapshot,payload.address,payload.size);
        caller->publications[p].snapshot=snapshot;
        caller->publications[p].destination.size=payload.size;
        captured+=payload.size;
    }
    frame=&stack->frames[parent]; frame->pc=pc;
    size_t cell_base=stack->cell_count,spill_base=stack->spill_count;
    stack->frames[stack->depth++]=(XrRbcFrame){access.index,0,base,receiving ? access.result : access.window,
        cell_base,0,spill_base,0};
    xr_rbc_cells_begin(stack,cell_base,callee->cell_count);
    xr_rbc_entry_clear(stack->slots+base,&stack->entries[access.index]);
    stack->cell_count+=callee->cell_count; stack->spill_count+=callee->spill_count;
    if (callee->aggregate_size && !xr_rbc_aggregate_reserve(stack,&stack->frames[parent+1],callee)) return XR_RBC_OUT_OF_MEMORY;
    XrRbcStatus result=xr_rbc_values_begin(module,stack,access.index,source);
    if (result!=XR_RBC_OK) return result;
    RbcValueActivation *child=receiving ? stack->values->top : NULL;
    if (child) {
        child->receiving_call=true;
        if (invoke) {
            child->invoke_error=access.other;
            child->invoke_status=access.window;
        } else if (!indirect && access.other) {
            child->receiving_slice_call=rbc_value_slice_call(module,caller->code,receiving,access);
            if (!child->receiving_slice_call) return XR_RBC_INVALID;
        }
    }
    if (indirect) {
        if (!child || child->code->parameter_count!=captures+access.count+callee->state_capture) return XR_RBC_INVALID;
        XrtManagedStatus status=rbc_value_function_captures(module,stack,child,&invoked);
        if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    }
    for (uint32_t p=0;p<access.count;++p) {
        uint32_t from=caller->code->arguments[access.first+p];
        if (receiving) {
            uint32_t parameter=captures+p;
            uint32_t to=receiving->parameters[parameter];
            const XrLirValueHome *formal=rbc_value_home(receiving,to);
            const XrLirHomeBinding *binding=rbc_value_binding(receiving,to);
            if (!indirect && rbc_value_i64_ref(module,receiving,to)) {
                RbcValueOrigin origin=rbc_value_origin(caller,from);
                if (!origin.activation || binding->parameter!=parameter+1) return XR_RBC_INVALID;
                child->parameters[parameter]=origin; continue;
            }
            if (binding && !rbc_value_binding_state_cap(receiving,to)) {
                const XrLirHomeBinding *actual=rbc_value_binding(caller->code,from);
                if (!actual || actual->type!=binding->type || binding->parameter!=parameter+1) return XR_RBC_INVALID;
                child->parameters[parameter]=rbc_value_origin(caller,from); continue;
            }
            if (formal && formal->type) {
                if (!rbc_value_formal_accepts(module,receiving,to,caller->code,from) ||
                    formal->parameter!=parameter+1) return XR_RBC_INVALID;
                if (formal->owner_count || rbc_value_inline(module,receiving,to,false) || rbc_value_slice(module,receiving,to)) {
                    RbcValueOrigin origin = rbc_value_origin(caller, from);
                    child->parameters[parameter] = origin;
                    if (receiving->map_formal_reads && child->owned_code && formal->owner_count &&
                        child->image->types[formal->type - 1].kind == XRT_VALUE_MAP_STRING_I64)
                        child->parameter_storage[parameter].map = rbc_map_parameter_source(module, origin);
                    continue;
                }
                /* POD formal locations have their own ordinary by-value bank. */
                XrRbcValueLocation location=receiving->locations[to];
                if (location.kind!=XR_RBC_VALUE_AGGREGATE) return XR_RBC_INVALID;
                const XrRbcAggregateValue value=callee->aggregate_values[location.index];
                uint8_t *output=xr_rbc_aggregate_value(stack,&stack->frames[parent+1],value);
                xr_rbc_aggregate_unpack(module->aggregate_types[value.type],caller->publications[p].snapshot,output);
                continue;
            }
            XrRbcRegister *output=rbc_value_register(stack,child,to);
            if (!output || caller->publications[p].destination.size!=8) return XR_RBC_INVALID;
            memcpy(&output->bits,caller->publications[p].snapshot,8);
        } else {
            if (caller->publications[p].destination.size!=8) return XR_RBC_INVALID;
            memcpy(&stack->slots[base+p].bits,caller->publications[p].snapshot,8);
        }
    }
    return xr_rbc_callable_arguments_valid(module,callee,stack->slots+base) ? XR_RBC_OK : XR_RBC_INVALID;
}
static XrRbcStatus rbc_value_return(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, XrRbcValueAccess access, uint32_t source) {
    size_t depth=stack->depth;
    XrRbcFrame frame=stack->frames[depth-1];
    bool failed=access.operation==XR_RBC_VALUE_RETURN_ERROR;
    if (failed && (!activation->invoke_error || depth<2)) return XR_RBC_INVALID;
    if (failed) frame.destination=activation->invoke_error;
    XrRbcStatus result=xr_rbc_closure_scope_return(module,stack,NULL,0,source);
    if (result!=XR_RBC_OK) return result;
    const XrLirValueHome *home=rbc_value_home(activation->code,access.source);
    XrtManagedStatus status=XRT_MANAGED_OK;
    if (rbc_value_slice(module,activation->code,access.source)) {
        status=rbc_value_slice_return(module,stack,activation,access);
    } else if (rbc_value_function(module,activation->code,access.source,false)) {
        status=rbc_value_function_return(module,stack,activation,access);
    } else if (rbc_value_cell(module,activation->code,access.source,false)) {
        const XrLirHomeEvent *event=rbc_value_event(activation->code,access.index,XR_LIR_OWNERSHIP_RETURN_TRANSFER);
        RbcValueActivation *caller=activation->previous;
        if (!event || depth<=1 || !activation->receiving_call || !caller || caller->depth!=depth-1 ||
            (event->transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_COPY && event->transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_MOVE))
            return XR_RBC_INVALID;
        status=rbc_value_replace(module,stack,activation,(RbcValueOrigin){caller,frame.destination},
            rbc_value_origin(activation,access.source),event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY);
    } else if (home && (home->owner_count || rbc_value_inline(module,activation->code,access.source,false))) {
        RbcValueOrigin from=rbc_value_origin(activation,access.source);
        if (depth>1) {
            if (!activation->receiving_call) return XR_RBC_INVALID;
            RbcValueActivation *caller=activation->previous;
            if (!caller || caller->depth!=depth-1) return XR_RBC_INVALID;
            status=rbc_value_replace(module,stack,activation,(RbcValueOrigin){caller,frame.destination},from,home->parameter!=0);
        } else {
            if (rbc_value_inline(module,activation->code,access.source,false)) {
                if (!stack->aggregate_result || !rbc_inline_tuple_result(activation->image,home->type) ||
                    home->type != activation->code->homes->result_type) return XR_RBC_INVALID;
                RbcValuePayload payload;
                status = rbc_value_payload(module,stack,from,&payload);
                if (status == XRT_MANAGED_OK) {
                    memcpy(stack->aggregate_result,payload.address,payload.size);
                    stack->slots[0] = (XrRbcRegister){0};
                }
            } else {
                if (home->parameter || home->owner_count!=1 || !stack->value_result || *stack->value_result)
                    return XR_RBC_INVALID;
                status=xrt_managed_slot_take(activation->frame,home->first_owner,stack->value_result);
            }
        }
    } else if (home && home->type) {
        RbcValuePayload input;
        status=rbc_value_payload(module,stack,(RbcValueOrigin){activation,access.source},&input);
        if (status!=XRT_MANAGED_OK) return XR_RBC_INVALID;
        XrRbcAggregateLayout physical;
        if (!xr_rbc_aggregate_layout(input.packed,&physical)) return XR_RBC_INVALID;
        if (depth>1) {
            uint8_t *output=NULL;
            if (activation->receiving_call) {
                RbcValueActivation *caller=activation->previous; RbcValuePayload destination;
                if (!caller || caller->depth!=depth-1 || rbc_value_payload(module,stack,
                    (RbcValueOrigin){caller,frame.destination},&destination)!=XRT_MANAGED_OK || destination.size!=input.size)
                    return XR_RBC_INVALID;
                output=destination.address;
            } else {
                const XrRbcFrame *caller=&stack->frames[depth-2];
                const XrRbcFunction *receiver=&module->functions[caller->function];
                if (frame.destination>=receiver->aggregate_value_count) return XR_RBC_INVALID;
                output=xr_rbc_aggregate_value(stack,caller,receiver->aggregate_values[frame.destination]);
            }
            memmove(output,input.address,physical.size);
        } else {
            if (!stack->aggregate_result) return XR_RBC_INVALID;
            xr_rbc_aggregate_pack(input.packed,input.address,stack->aggregate_result);
            stack->slots[0]=(XrRbcRegister){0};
        }
    } else {
        uint64_t bits=0;
        if (access.source) {
            XrRbcRegister *input=rbc_value_register(stack,activation,access.source);
            if (!input) return XR_RBC_INVALID;
            bits=input->bits;
        }
        const XrRbcFunction *function=&module->functions[frame.function];
        if (function->callable && !xr_rbc_callable_scalar_valid(
                module->callable_types[function->callable-1].result,bits)) return XR_RBC_INVALID;
        if (depth>1) {
            if (activation->receiving_call) {
                RbcValueActivation *caller=activation->previous;
                XrRbcRegister *output=caller ? rbc_value_register(stack,caller,frame.destination) : NULL;
                if (!output && access.source) return XR_RBC_INVALID;
                if (output) output->bits=bits;
            } else stack->slots[stack->frames[depth-2].base+frame.destination].bits=bits;
        } else stack->slots[0].bits=bits;
    }
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    if (activation->frame) {
        status=xrt_managed_frame_leave(stack->values->root,activation->frame);
        if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    }
    if (activation->invoke_error) {
        size_t base=stack->frames[depth-2].base;
        stack->slots[base+activation->invoke_status].bits=failed ? 1u : 0u;
    }
    stack->values->top=activation->previous;
    xrt_managed_metadata_free(stack->values->billing,activation,activation->bytes);
    stack->cell_count=frame.cell_base; stack->spill_count=frame.spill_base; --stack->depth;
    return XR_RBC_OK;
}
static XrtManagedStatus rbc_value_publish_leaf(RbcValueActivation *activation,uint32_t result) {
    XrtPlace destination,temporary;
    XrtManagedStatus status=rbc_value_place((RbcValueOrigin){activation,result},false,0,&destination);
    if (status==XRT_MANAGED_OK) status=rbc_value_place((RbcValueOrigin){activation,result},true,0,&temporary);
    return status==XRT_MANAGED_OK ? xrt_place_move_replace(destination,temporary,NULL) : status;
}
static XrtManagedStatus rbc_value_host_file_write(XrRbcStack *stack, RbcValueActivation *activation,
    XrRbcValueAccess access, XrtBoundClass binding, const XrtManagedHeader *file, XrRbcRegister *output,
    uint32_t source) {
    XrNativeHostShape shape = xr_native_host_shape(access.index);
    const XrtManagedHeader *data = NULL;
    XrtManagedStatus status;
    if (shape == XR_NATIVE_HOST_FILE_WRITE_STRING) {
        const XrtStringBacking *text = NULL;
        status = rbc_value_string_borrow(activation, access.other, &text);
        data = (const XrtManagedHeader *)text;
    } else {
        RbcValueOrigin origin = rbc_value_origin(activation, access.other);
        const XrLirValueHome *home = origin.activation ? rbc_value_home(origin.activation->code, origin.value) : NULL;
        status = rbc_value_borrow_header(origin, home, &data);
    }
    if (status != XRT_MANAGED_OK) return status;
    XrRbcRegister *input = rbc_value_register(stack, activation, activation->code->arguments[access.first]);
    if (!input) return XRT_MANAGED_ARGUMENT;
    int64_t offset, count;
    memcpy(&offset, &input->bits, sizeof(offset));
    XrOsIoStatus host = xrt_host_file_write(binding, shape, file, data, offset, &count);
    if (host != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(host), source);
    output->bits = (uint64_t)count;
    return XRT_MANAGED_OK;
}

static XrtManagedStatus rbc_value_host_environment_get(RbcValueActivation *activation, XrRbcValueAccess access,
    uint32_t source) {
    const XrtStringBacking *name = NULL;
    XrtManagedStatus status = rbc_value_string_borrow(activation, access.source, &name);
    if (status != XRT_MANAGED_OK) return status;
    const XrLirValueHome *home = rbc_value_home(activation->code, access.result);
    const XrtHostNullableSlots output = {activation->frame, home->first_owner, home->temporary_first_owner,
        home->offset, home->temporary_offset + offsetof(XrtHostNullableOwner, owner)};
    XrtHostEnvironmentGet result = xrt_host_environment_get_publish(&output, (const XrtManagedHeader *)name, source);
    if (result.managed == XRT_MANAGED_OK && result.host != XR_OS_IO_OK)
        xrt_value_failure(xrt_host_io_failure(result.host), source);
    return result.managed;
}

static XrtHostFileStagedResult rbc_value_file_destination(RbcValueActivation *activation, uint32_t value) {
    const XrLirValueHome *home = rbc_value_home(activation->code, value);
    return (XrtHostFileStagedResult){
        {activation->frame, home->first_owner, home->temporary_first_owner, home->offset},
        home->temporary_offset + offsetof(XrtHostFileOptional, owner)};
}

static XrtManagedStatus rbc_value_host_file_read(XrRbcStack *stack, RbcValueActivation *activation,
    XrRbcValueAccess access, XrtBoundClass binding, const XrtManagedHeader *file, uint32_t source) {
    XrRbcRegister *maximum = rbc_value_register(stack, activation, access.other);
    if (!maximum || !xrt_class_backing_matches(file, binding.plan)) return XRT_MANAGED_ARGUMENT;
    int64_t max_bytes;
    memcpy(&max_bytes, &maximum->bits, sizeof(max_bytes));
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_native_values_backings(xrt_value_root_current(source), &types);
    if (status != XRT_MANAGED_OK) return status;
    XrtHostFileStagedResult output = rbc_value_file_destination(activation, access.result);
    XrtHostFileRead read = xrt_host_file_read_publish(&output, binding, file, types, max_bytes, source);
    if (read.managed == XRT_MANAGED_OK && read.host != XR_OS_IO_OK)
        xrt_value_failure(xrt_host_io_failure(read.host), source);
    return read.managed;
}

static XrtManagedStatus rbc_value_string_replace(XrRbcStack *stack, RbcValueActivation *activation,
    XrRbcValueAccess access, uint32_t source) {
    const XrtStringBacking *text = NULL, *search = NULL, *replacement = NULL;
    XrtManagedStatus status = rbc_value_string_borrow(activation, access.source, &text);
    if (status == XRT_MANAGED_OK) status = rbc_value_string_borrow(activation, access.other, &search);
    if (status == XRT_MANAGED_OK) status = rbc_value_string_borrow(activation, access.index, &replacement);
    if (status != XRT_MANAGED_OK) return status;
    const XrLirValueHome *home = rbc_value_home(activation->code, access.result);
    xrt_native_string_replace_create(activation->frame, home->temporary_first_owner, text, search,
        replacement, access.first != 0, source);
    return rbc_value_publish_leaf(activation, access.result);
}

static XrtManagedStatus rbc_value_string_slice(XrRbcStack *stack, RbcValueActivation *activation,
    XrRbcValueAccess access, uint32_t source) {
    XrRbcRegister *first = rbc_value_register(stack, activation, access.other);
    XrRbcRegister *last = rbc_value_register(stack, activation, access.index);
    if (!first || !last) return XRT_MANAGED_ARGUMENT;
    const XrtStringBacking *text = NULL;
    XrtManagedStatus status = rbc_value_string_borrow(activation, access.source, &text);
    if (status != XRT_MANAGED_OK) return status;
    int64_t start, end;
    memcpy(&start, &first->bits, sizeof(start));
    memcpy(&end, &last->bits, sizeof(end));
    const XrLirValueHome *home = rbc_value_home(activation->code, access.result);
    xrt_native_string_slice_create(activation->frame, home->temporary_first_owner, &text->managed, start, end, source);
    return rbc_value_publish_leaf(activation, access.result);
}

static XrtManagedStatus rbc_value_host_file(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, XrRbcValueAccess access, uint32_t source) {
    bool path_class = access.operation == XR_RBC_VALUE_HOST_PATH_CLASS;
    bool open = path_class || access.operation == XR_RBC_VALUE_HOST_FILE_OPEN_WRITE;
    const XrLirValueHome *home = rbc_value_home(activation->code, open ? access.result : access.source);
    const XrtValueImageType *type = &activation->image->types[home->type - 1];
    uint32_t class_type = open ? type->nullable_element : home->type;
    XrtBoundClass binding = {0};
    XrtManagedStatus status = rbc_value_plan_set(module, stack);
    if (status == XRT_MANAGED_OK) status = stack->layout ? xrt_module_layout_class(stack->layout, class_type, &binding) :
        xrt_value_image_class(stack->values->plans, class_type, &binding);
    if (status != XRT_MANAGED_OK) return status;
    if (open) {
        const XrtStringBacking *path = NULL;
        status = rbc_value_string_borrow(activation, access.source, &path);
        if (path_class) {
            if (status != XRT_MANAGED_OK) return status;
            if (!xrt_backing_string_matches((const XrtManagedHeader *)path)) return XRT_MANAGED_ARGUMENT;
            XrtHostFileStagedResult output = rbc_value_file_destination(activation, access.result);
            if (xr_native_host_shape(access.index) == XR_NATIVE_HOST_FILE_STAT) {
                const XrtHostNullableSlots slots = {output.location.frame, output.location.destination,
                    output.location.temporary, output.location.offset, output.temporary_owner_offset};
                XrtHostStatResult stat = xrt_host_stat_publish(&slots, binding, (const XrtManagedHeader *)path, source);
                /* Leave the guest with the query failure before close-only drain can run. */
                if (stat.host != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(stat.host), source);
                return stat.managed;
            }
            XrtHostFileOpen result = xrt_host_file_open_read_publish(&output, binding, path, source);
            if (result.managed == XRT_MANAGED_OK && result.host != XR_OS_IO_OK)
                xrt_value_failure(xrt_host_io_failure(result.host), source);
            return result.managed;
        }
        XrRbcRegister *append = rbc_value_register(stack, activation, access.other);
        if (!append || append->bits > 1) return XRT_MANAGED_ARGUMENT;
        if (status == XRT_MANAGED_OK) {
            XrtNativeFileResult output = {activation->frame, home->first_owner, home->temporary_first_owner, home->offset};
            xrt_host_file_open_publish(&output, binding, path, append->bits != 0, source);
        }
    } else {
        const XrtManagedHeader *file = NULL;
        status = rbc_value_borrow_header(rbc_value_origin(activation, access.source), home, &file);
        if (status != XRT_MANAGED_OK) return status;
        if (access.operation == XR_RBC_VALUE_HOST_FILE_READ)
            return rbc_value_host_file_read(stack, activation, access, binding, file, source);
        XrRbcRegister *output = rbc_value_register(stack, activation, access.result);
        if (!output) return XRT_MANAGED_ARGUMENT;
        if (access.operation == XR_RBC_VALUE_HOST_FILE_WRITE)
            return rbc_value_host_file_write(stack, activation, access, binding, file, output, source);
        bool closed;
        status = xrt_host_file_close(binding, file, &closed);
        if (status == XRT_MANAGED_OK) output->bits = closed;
    }
    return status;
}
#include "xrbc_value_inline_execute.inc.c"
#include "xrbc_value_array_execute.inc.c"
#include "xrbc_value_array_region.inc.c"
#include "xrbc_value_array_push.inc.c"
#include "xrbc_value_cell_execute.inc.c"
#include "xrbc_value_slice_execute.inc.c"
static uint32_t rbc_value_source(const XrRbcModule *module, const XrRbcStack *stack, size_t pc) {
    const XrRbcFunction *function = &module->functions[stack->frames[stack->depth - 1].function];
    return function->source_ids ? function->source_ids[pc - 1] : 0;
}

#include "xrbc_value_map_execute.inc.c"
#include "xrbc_value_set_execute.inc.c"
#include "xrbc_value_callable_factory.inc.c"
#include "xrbc_value_callable_state.inc.c"
static XrtManagedStatus rbc_value_display(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    RbcValuePayload payload;
    XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(activation,access.source),&payload);
    const XrtBackingTypes *types=NULL;
    if (status==XRT_MANAGED_OK) status=xrt_native_values_backings(xrt_value_root_current(source),&types);
    const XrLirValueHome *result=rbc_value_home(activation->code,access.result);
    if (status==XRT_MANAGED_OK) {
        const XrLirValueHome *home = rbc_value_home(activation->code, access.source);
        const XrtValueImage *image = activation->image;
        if (home && home->type && image->types[home->type - 1].kind == XRT_VALUE_TUPLE)
            status = xrt_string_backing_tuple_display_create(activation->frame,
                result->temporary_first_owner, types, image, home->type, payload.address);
        else status=xrt_string_backing_pod_display_create(activation->frame,result->temporary_first_owner,
            types,&activation->code->displays[access.index],payload.address);
    }
    return status==XRT_MANAGED_OK ? rbc_value_publish_leaf(activation,access.result) : status;
}
/* Class identity rows must be resolved through the same plan authority that
 * created the instance: a module-state layout or the program value image. */
typedef struct XrRbcValueClassResolver { XrRbcStack *stack; } XrRbcValueClassResolver;
static XrtManagedStatus rbc_value_class_binding(const XrRbcValueClassResolver *resolver,
    uint32_t id, XrtBoundClass *binding) {
    return resolver->stack->layout ?
        xrt_module_layout_class(resolver->stack->layout, id, binding) :
        xrt_value_image_class(resolver->stack->values->plans, id, binding);
}
static XR_NOINLINE XrRbcStatus xr_rbc_values_step(const XrRbcModule *module, XrRbcStack *stack,
    uint32_t index, size_t pc, uint32_t source) {
    RbcValueActivation *activation=stack->values ? stack->values->top : NULL;
    if ((!activation || activation->depth!=stack->depth) && stack->depth) {
        XrRbcStatus status=xr_rbc_values_begin(module,stack,(uint32_t)stack->frames[stack->depth-1].function,source);
        if (status!=XR_RBC_OK) return status;
        activation=stack->values ? stack->values->top : NULL;
    }
    if (!activation || activation->depth!=stack->depth || index>=activation->code->access_count) return XR_RBC_INVALID;
    XrRbcValueAccess access=activation->code->accesses[index];
    if (!source && activation->code->strict_utf8 && activation->invoke_error && stack->depth > 1) {
        const XrRbcFrame *caller = &stack->frames[stack->depth - 2];
        const XrRbcFunction *owner = &module->functions[caller->function];
        if (owner->source_ids && caller->pc && caller->pc <= owner->source_id_count)
            source = owner->source_ids[caller->pc - 1];
    }
    XrtManagedStatus region_status=XRT_MANAGED_OK;
    if (rbc_value_array_region_before(module,stack,activation,index,&region_status)) {
        if (region_status!=XRT_MANAGED_OK) xrt_native_managed_failure(region_status,source);
        if (access.operation==XR_RBC_VALUE_EDGE && access.window)
            rbc_value_array_push_after_edge(stack,activation,access);
        stack->frames[stack->depth-1].pc=pc;
        return XR_RBC_OK;
    }
    if (rbc_value_binding_forward(activation, access)) {
        stack->frames[stack->depth-1].pc=pc+1;
        return XR_RBC_OK;
    }
    if (access.operation==XR_RBC_VALUE_CALL || access.operation==XR_RBC_VALUE_FUNCTION_CALL ||
        access.operation==XR_RBC_VALUE_INVOKE)
        return rbc_value_call(module,stack,activation,access,pc,source);
    if (access.operation==XR_RBC_VALUE_RETURN || access.operation==XR_RBC_VALUE_RETURN_ERROR)
        return rbc_value_return(module,stack,activation,access,source);
    if (access.operation==XR_RBC_VALUE_INVOKE_RESULT || access.operation==XR_RBC_VALUE_INVOKE_ERROR) {
        stack->frames[stack->depth-1].pc=pc;
        return XR_RBC_OK;
    }
    if (access.operation==XR_RBC_VALUE_PANIC_ERROR || access.operation==XR_RBC_VALUE_UNCAUGHT_ERROR) {
        if (access.operation==XR_RBC_VALUE_UNCAUGHT_ERROR && stack->depth!=1) return XR_RBC_INVALID;
        RbcValuePayload value={0};
        XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(activation,access.source),&value);
        if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
        const XrRbcLiteral *metadata=&module->literals[access.index];
        if (access.operation==XR_RBC_VALUE_UNCAUGHT_ERROR)
            xrt_uncaught_error((const uint8_t *)metadata->bytes,metadata->length,value.address,value.size);
        xrt_try_assert_panic((const uint8_t *)metadata->bytes,metadata->length,value.address,value.size,source);
    }
    XrtManagedStatus status=XRT_MANAGED_OK;
    if (access.operation==XR_RBC_VALUE_CELL) status=rbc_value_cell_step(module,stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_STRING_APPEND_BINDING)
        status=rbc_value_string_binding_append(module,stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_STRING_APPEND_OWNED) status=rbc_value_string_append(activation,access,source);
    else if (access.operation==XR_RBC_VALUE_SLICE) status=rbc_value_slice_step(module,stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_FUNCTION_NEW) status=rbc_value_function_new(module,stack,activation,access);
    else if (access.operation==XR_RBC_VALUE_FUNCTION_ALIAS || access.operation==XR_RBC_VALUE_FUNCTION_SCOPE_EXIT)
        status=rbc_value_function_alias(module,stack,activation,access,source);
    else if (access.operation<=XR_RBC_VALUE_STRING_CONCAT) status=rbc_value_factory(module,stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_STRING_SLICE) status=rbc_value_string_slice(stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_STRING_REPLACE) status=rbc_value_string_replace(stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_COPY || access.operation==XR_RBC_VALUE_MOVE)
        status=rbc_value_replace(module,stack,activation,(RbcValueOrigin){activation,access.result},
            rbc_value_origin(activation,access.source),access.operation==XR_RBC_VALUE_COPY);
    else if (access.operation==XR_RBC_VALUE_CLASS_ADOPT) {
        /* Retaining retag: the destination acquires a new reference to the same
         * nominal handle, so later DROP on either side stays balanced. */
        const XrLirValueHome *home=rbc_value_home(activation->code,access.source);
        const XrtManagedHeader *header=NULL;
        status=rbc_value_borrow_header(rbc_value_origin(activation,access.source),home,&header);
        if (status==XRT_MANAGED_OK && !header) status=XRT_MANAGED_ARGUMENT;
        if (status==XRT_MANAGED_OK)
            status=rbc_value_inline_replace(module,stack,(RbcValueOrigin){activation,access.result},&header);
    } else if (access.operation==XR_RBC_VALUE_CLASS_IS) {
        const XrLirValueHome *home=rbc_value_home(activation->code,access.source);
        const XrtManagedHeader *header=NULL;
        status=rbc_value_borrow_header(rbc_value_origin(activation,access.source),home,&header);
        if (status==XRT_MANAGED_OK) {
            /* Resolve through the same plan authority that constructed the
             * instance. Find the dynamic class row from the live Ops identity,
             * then walk UP its ancestor chain: the target matches when it is the
             * value's class or an ancestor of it. The reverse walk (target's own
             * ancestors) would invert the predicate. */
            XrRbcValueClassResolver resolve={stack};
            XrtBoundClass target_binding={0};
            status=rbc_value_class_binding(&resolve,access.index,&target_binding);
            uint32_t current=0;
            for (uint32_t id=1; status==XRT_MANAGED_OK && id<=activation->image->type_count; ++id) {
                if (activation->image->types[id-1].kind!=XRT_VALUE_CLASS) continue;
                XrtBoundClass row={0};
                status=rbc_value_class_binding(&resolve,id,&row);
                if (status!=XRT_MANAGED_OK) break;
                if (row.ops==header->ops) {current=id;break;}
            }
            bool matched=false;
            for (; status==XRT_MANAGED_OK && current; ) {
                XrtBoundClass row={0};
                status=rbc_value_class_binding(&resolve,current,&row);
                if (status!=XRT_MANAGED_OK) break;
                if (row.ops==target_binding.ops) {matched=true;break;}
                current=current<=activation->image->type_count ? activation->image->types[current-1].class_parent : 0;
            }
            XrRbcRegister *output=status==XRT_MANAGED_OK ? rbc_value_register(stack,activation,access.result) : NULL;
            if (status==XRT_MANAGED_OK && !output) status=XRT_MANAGED_ARGUMENT;
            if (status==XRT_MANAGED_OK) output->bits=matched ? 1u : 0u;
        }
    }
    else if (access.operation==XR_RBC_VALUE_DROP) {
        const XrLirValueHome *home=rbc_value_home(activation->code,access.source);
        status=rbc_value_inline(module,activation->code,access.source,false) ?
            rbc_value_inline_replace(module,stack,(RbcValueOrigin){activation,access.source},NULL) :
            xrt_managed_slot_drop(activation->frame,home->first_owner);
    } else if (access.operation==XR_RBC_VALUE_ASSERT_CONDITION) {
        const XrtStringBacking *message=NULL;
        status=rbc_value_string_borrow(activation,access.other,&message);
        XrRbcRegister *condition=rbc_value_register(stack,activation,access.source);
        if (status==XRT_MANAGED_OK && (!condition || condition->bits>1)) status=XRT_MANAGED_ARGUMENT;
        if (status==XRT_MANAGED_OK) xrt_assert_condition((uint8_t)condition->bits,message,source);
    } else if (access.operation==XR_RBC_VALUE_HOST_FILE_OPEN_WRITE || access.operation==XR_RBC_VALUE_HOST_FILE_CLOSE ||
        access.operation==XR_RBC_VALUE_HOST_FILE_WRITE || access.operation==XR_RBC_VALUE_HOST_PATH_CLASS ||
        access.operation==XR_RBC_VALUE_HOST_FILE_READ) {
        status = rbc_value_host_file(module, stack, activation, access, source);
    } else if (access.operation==XR_RBC_VALUE_HOST_ENV_GET) {
        status = rbc_value_host_environment_get(activation, access, source);
    } else if (access.operation==XR_RBC_VALUE_HOST_PATH_BOOL) {
        const XrtStringBacking *path = NULL, *destination = NULL;
        status = rbc_value_string_borrow(activation, access.source, &path);
        if (status == XRT_MANAGED_OK && access.other)
            status = rbc_value_string_borrow(activation, access.other, &destination);
        XrRbcRegister *output = rbc_value_register(stack, activation, access.result);
        if (!output) status = XRT_MANAGED_ARGUMENT;
        if (status == XRT_MANAGED_OK)
            output->bits = access.other ? xrt_native_host_path_pair_bool(access.index, path, destination, source) :
                xrt_native_host_path_bool(access.index, path, source);
    } else if (access.operation==XR_RBC_VALUE_I64_TRY_PARSE || access.operation==XR_RBC_VALUE_F64_TRY_PARSE) {
        const XrtStringBacking *string = NULL;
        RbcValuePayload destination;
        status = rbc_value_string_borrow(activation, access.source, &string);
        if (status == XRT_MANAGED_OK)
            status = rbc_value_payload(module, stack, (RbcValueOrigin){activation, access.result}, &destination);
        if (status == XRT_MANAGED_OK) {
            int64_t parsed_i64 = 0;
            double parsed_f64 = 0.0;
            const void *parsed = &parsed_i64;
            size_t parsed_size = sizeof(parsed_i64);
            bool some;
            if (access.operation==XR_RBC_VALUE_I64_TRY_PARSE) {
                some = xrt_native_string_try_parse_i64(string, &parsed_i64);
            } else {
                some = xrt_native_string_try_parse_f64(string, &parsed_f64);
                parsed = &parsed_f64;
                parsed_size = sizeof(parsed_f64);
            }
            memset(destination.address, 0, destination.size);
            if (some) {
                memcpy(destination.address + destination.packed.physical.alignment, parsed, parsed_size);
                destination.address[0] = 1;
            }
        }
    } else if (access.operation==XR_RBC_VALUE_STRING_PRINT || access.operation==XR_RBC_VALUE_STRING_LENGTH ||
        access.operation==XR_RBC_VALUE_STRING_BYTE_LENGTH) {
        const XrtStringBacking *string=NULL;
        status=rbc_value_string_borrow(activation,access.source,&string);
        if (status==XRT_MANAGED_OK && access.operation==XR_RBC_VALUE_STRING_PRINT)
            xrt_print_string(string->bytes,string->length,source);
        else if (status==XRT_MANAGED_OK) {
            XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
            size_t length = access.operation == XR_RBC_VALUE_STRING_BYTE_LENGTH ? string->length : string->runes;
            if (!output) status=XRT_MANAGED_ARGUMENT;
            else if (length>INT64_MAX) status=XRT_MANAGED_OVERFLOW;
            else output->bits=(uint64_t)length;
        }
    } else if (access.operation==XR_RBC_VALUE_STRING_INDEX_OF) {
        const XrtStringBacking *text=NULL,*search=NULL;
        status=rbc_value_string_borrow(activation,access.source,&text);
        if (status==XRT_MANAGED_OK) status=rbc_value_string_borrow(activation,access.other,&search);
        if (status==XRT_MANAGED_OK) {
            XrRbcRegister *start=rbc_value_register(stack,activation,access.index);
            XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
            if (!start || !output) status=XRT_MANAGED_ARGUMENT;
            else {
                int64_t from;
                memcpy(&from,&start->bits,sizeof(from));
                output->bits=(uint64_t)xrt_native_string_index_of(text,search,from,source);
            }
        }
    } else if (access.operation==XR_RBC_VALUE_STRING_ORDER) {
        const XrtStringBacking *left=NULL,*right=NULL;
        status=rbc_value_string_borrow(activation,access.source,&left);
        if (status==XRT_MANAGED_OK) status=rbc_value_string_borrow(activation,access.other,&right);
        if (status==XRT_MANAGED_OK) {
            XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
            if (!output) status=XRT_MANAGED_ARGUMENT;
            else {
                int32_t order=xrt_native_string_order(left,right,source);
                bool result=access.index==XR_LIR_LT ? order<0 : access.index==XR_LIR_LE ? order<=0 :
                    access.index==XR_LIR_GT ? order>0 : order>=0;
                output->bits=result ? 1u : 0u;
            }
        }
    } else if (access.operation==XR_RBC_VALUE_STRING_LAST_INDEX_OF) {
        const XrtStringBacking *text=NULL,*search=NULL;
        status=rbc_value_string_borrow(activation,access.source,&text);
        if (status==XRT_MANAGED_OK) status=rbc_value_string_borrow(activation,access.other,&search);
        if (status==XRT_MANAGED_OK) {
            XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
            if (!output) status=XRT_MANAGED_ARGUMENT;
            else output->bits=(uint64_t)xrt_native_string_last_index_of(text,search,source);
        }
    } else if (access.operation==XR_RBC_VALUE_STRING_TEST) {
        const XrtStringBacking *text=NULL,*pattern=NULL;
        status=rbc_value_string_borrow(activation,access.source,&text);
        if (status==XRT_MANAGED_OK) status=rbc_value_string_borrow(activation,access.other,&pattern);
        if (status==XRT_MANAGED_OK) {
            XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
            if (!output) status=XRT_MANAGED_ARGUMENT;
            else output->bits=xrt_native_string_test(text,pattern,access.index,source) ? 1u : 0u;
        }
    } else if (access.operation==XR_RBC_VALUE_STRING_EQUAL || access.operation==XR_RBC_VALUE_STRING_NOT_EQUAL) {
        const XrtStringBacking *left=NULL,*right=NULL;
        status=rbc_value_string_borrow(activation,access.source,&left);
        if (status==XRT_MANAGED_OK) status=rbc_value_string_borrow(activation,access.other,&right);
        if (status==XRT_MANAGED_OK) {
            XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
            if (!output) status=XRT_MANAGED_ARGUMENT;
            else {
                bool equal=left->length==right->length &&
                    (!left->length || !memcmp(left->bytes,right->bytes,left->length));
                output->bits=access.operation==XR_RBC_VALUE_STRING_NOT_EQUAL ? !equal : equal;
            }
        }
    } else if (access.operation>=XR_RBC_VALUE_STATE_READ && access.operation<=XR_RBC_VALUE_STATE_GROUP_INITIALIZE)
        status=rbc_value_state(module,stack,activation,access);
    else if (access.operation==XR_RBC_VALUE_EDGE) {
        status=rbc_value_edge(module,stack,activation,access);
        if (status==XRT_MANAGED_OK && access.window) rbc_value_array_push_after_edge(stack,activation,access);
    }
    else if (access.operation>=XR_RBC_VALUE_ARRAY && access.operation<=XR_RBC_VALUE_BINDING_DROP)
        status=rbc_value_array_step(module,stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_MAP) status=rbc_value_map_step(module,stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_SET) status=rbc_value_set_step(module,stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_MAP_LITERAL) status=rbc_value_map_literal(module,stack,activation,access,source);
    else if ((access.operation>=XR_RBC_VALUE_RECORD_NEW && access.operation<=XR_RBC_VALUE_NULLABLE_UNWRAP) ||
        access.operation==XR_RBC_VALUE_SUM_NEW || access.operation==XR_RBC_VALUE_SUM_TAG ||
        access.operation==XR_RBC_VALUE_SUM_GET || access.operation==XR_RBC_VALUE_CLASS_SCALAR_WRITE ||
        access.operation==XR_RBC_VALUE_CLASS_ARRAY_WRITE || access.operation==XR_RBC_VALUE_CLASS_ARRAY_PUSH)
        status=rbc_value_inline_step(module,stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_DISPLAY) status=rbc_value_display(module,stack,activation,access,source);
    else return XR_RBC_INVALID;
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    if (activation->array_regions) rbc_value_array_region_after(activation,index);
    stack->frames[stack->depth-1].pc=pc;
    return XR_RBC_OK;
}

/* A verified local edit has neither a forwarding word nor an Array role.
 * Its cold transaction can replace the backing, but cannot change the VM
 * cursor, activation, or register bank. */
static XR_NOINLINE XrRbcStatus xr_rbc_map_edit_step(const XrRbcModule *module,
    XrRbcStack *stack, uint32_t index, size_t pc) {
    RbcValueActivation *activation = stack->values ? stack->values->top : NULL;
    if (!activation || activation->depth != stack->depth)
        return xr_rbc_values_step(module, stack, index, pc, rbc_value_source(module, stack, pc));
    if (!activation->owned_code || index >= activation->code->access_count) return XR_RBC_INVALID;
    if (activation->active_array_region)
        rbc_value_array_region_forget(activation, activation->active_array_region, RBC_ARRAY_REGION_BYPASS);
    XrRbcValueAccess access = activation->code->accesses[index];
    if (activation->backings_ready) {
        const uint32_t *args = activation->code->arguments + access.first;
        bool applied = false;
        XrtManagedStatus status = rbc_value_map_edit_local(stack, activation, access, args, &applied);
        if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, rbc_value_source(module, stack, pc));
        if (applied) return XR_RBC_OK;
    }
    uint32_t source = rbc_value_source(module, stack, pc);
    XrtManagedStatus status = rbc_value_map_step(module, stack, activation, access, source);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source);
    return XR_RBC_OK;
}

/* A borrowed literal only materializes its Frame keeper. The ordinary
 * factory still owns first use and failure, including the source and cleanup. */
static XR_NOINLINE XrRbcStatus xr_rbc_string_literal_borrow_step(const XrRbcModule *module,
    XrRbcStack *stack, uint32_t index, size_t pc) {
    RbcValueActivation *activation = stack->values ? stack->values->top : NULL;
    if (activation && activation->depth == stack->depth && activation->owned_code &&
        !activation->active_array_region && index < activation->code->access_count) {
        const XrRbcValueAccess *access = &activation->code->accesses[index];
        const XrLirValueHome *home = &activation->code->homes->values[access->result];
        const XrtManagedHeader *cached = NULL;
        if (rbc_value_read_literal_cache(activation, home, &cached) == XRT_MANAGED_OK && cached)
            return XR_RBC_OK;
    }
    return xr_rbc_values_step(module, stack, index, pc, rbc_value_source(module, stack, pc));
}
