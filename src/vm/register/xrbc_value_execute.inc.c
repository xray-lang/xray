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
#include "runtime/native/xnative_values.h"

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
struct RbcValueActivation {
    RbcValueActivation *previous;
    const XrRbcValueCode *code;
    const XrtValueImage *image;
    uint32_t function;
    size_t depth, bytes;
    XrtManagedFrame *frame;
    uint8_t *payload;
    RbcValueOrigin *parameters;
    XrtPlaceEdge *edges;
    RbcValuePublication *publications;
    uint8_t *snapshots;
    size_t edge_capacity, publication_capacity, snapshot_capacity, publication_count;
    bool receiving_call;
};
struct XrRbcValueRuntime {
    XrtBillingOwner *billing;
    XrtManagedRoot *root;
    RbcValueActivation *top;
};
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
static bool rbc_value_capacities(const XrRbcValueCode *code, size_t *leaves, size_t *rows, size_t *snapshots) {
    *leaves = 0; *rows = 1; *snapshots = 8;
    const XrLirValueHomes *homes = code->homes;
    if (homes) for (size_t v = 1; v <= (size_t)homes->value_count; ++v) {
        const XrLirValueHome *home = &homes->values[v];
        if (home->owner_count > *leaves) *leaves = home->owner_count;
        if (home->size > *snapshots) *snapshots = home->size;
    }
    for (size_t n = 0; n < code->access_count; ++n) {
        XrRbcValueAccess access = code->accesses[n];
        if (access.operation==XR_RBC_VALUE_CALL) {
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
            if (bytes > SIZE_MAX - size || (home && count > SIZE_MAX - home->owner_count)) return false;
            bytes += size; if (home) count += home->owner_count;
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
    if (!rbc_value_capacities(code,&leaves,&rows,&snapshots) ||
        !rbc_value_extent(&bytes,code->parameter_count,sizeof(RbcValueOrigin)) ||
        !rbc_value_extent(&bytes,leaves,sizeof(XrtPlaceEdge)) ||
        !rbc_value_extent(&bytes,rows,sizeof(RbcValuePublication)) || !rbc_value_extent(&bytes,snapshots,1))
        xrt_native_managed_failure(XRT_MANAGED_OVERFLOW,source);
    void *memory = NULL;
    status = xrt_managed_metadata_allocate(runtime->billing,bytes,&memory);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    RbcValueActivation *activation = memory;
    *activation = (RbcValueActivation){.previous=runtime->top,.code=code,.image=xr_rbc_module_value_image(module),
        .function=function,.depth=stack->depth,
        .bytes=bytes,.edge_capacity=leaves,.publication_capacity=rows,.snapshot_capacity=snapshots};
    size_t offset = sizeof(*activation);
#define RBC_VALUE_PART(field,n,t) do { offset=(offset+7u)&~(size_t)7u; \
    activation->field=(t *)((uint8_t *)memory+offset); offset+=(n)*sizeof(t); } while(0)
    RBC_VALUE_PART(parameters,code->parameter_count,RbcValueOrigin);
    RBC_VALUE_PART(edges,leaves,XrtPlaceEdge);
    RBC_VALUE_PART(publications,rows,RbcValuePublication);
    RBC_VALUE_PART(snapshots,snapshots,uint8_t);
#undef RBC_VALUE_PART
    for (uint32_t p = 0; p < code->parameter_count; ++p) activation->parameters[p]=(RbcValueOrigin){0};
    runtime->top = activation;
    const XrRbcFunction *physical=&module->functions[function];
    if (physical->aggregate_size && !xr_rbc_aggregate_reserve(stack,&stack->frames[stack->depth-1],physical))
        return XR_RBC_OUT_OF_MEMORY;
    if (code->homes && code->homes->owned_frame_needed) {
        runtime->root = xrt_native_managed_root(source);
        status = xr_rbc_value_homes_enter(code->homes,runtime->root,&activation->frame);
        void *payload = NULL; size_t size = 0;
        if (status == XRT_MANAGED_OK) status = xrt_managed_frame_payload(activation->frame,&payload,&size);
        if (status == XRT_MANAGED_OK && size != code->homes->frame.payload_size) status = XRT_MANAGED_ARGUMENT;
        if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
        activation->payload = payload;
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
    xrt_managed_metadata_free(billing,runtime,sizeof(*runtime));
    xrt_billing_owner_drop(billing);
}
static RbcValueOrigin rbc_value_origin(RbcValueActivation *activation, uint32_t value) {
    const XrLirValueHome *home = rbc_value_home(activation->code,value);
    if (home && home->parameter && home->owner_count) {
        if (home->parameter > activation->code->parameter_count) return (RbcValueOrigin){0};
        return activation->parameters[home->parameter - 1];
    }
    return (RbcValueOrigin){activation,value};
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
    XrtPlaceNull nulls=kind>=XRT_VALUE_STRING && kind<=XRT_VALUE_MAP_STRING_I64 ? XRT_PLACE_NONNULL : XRT_PLACE_NULLABLE;
    return xr_rbc_value_home_place((XrRbcHomeLocation){origin.activation->code->homes,
        origin.activation->frame,origin.value,temporary},leaf,nulls,out);
}
static XrtManagedStatus rbc_value_string_borrow(RbcValueActivation *activation, uint32_t value,
    const XrtStringBacking **out) {
    RbcValueOrigin origin = rbc_value_origin(activation,value);
    XrtPlace place;
    XrtManagedStatus status = rbc_value_place(origin,false,0,&place);
    const XrtManagedHeader *header = NULL;
    if (status == XRT_MANAGED_OK) { place.nulls=XRT_PLACE_NONNULL; status=xrt_place_borrow(place,&header); }
    if (status == XRT_MANAGED_OK) *out=(const XrtStringBacking *)header;
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
    if (home->owner_count) {
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
} RbcValueEdgeBuilder;
static XrtManagedStatus rbc_value_edge_pair(RbcValueEdgeBuilder *builder, uint32_t source, uint32_t target) {
    RbcValueActivation *activation=builder->activation;
    const XrRbcValueCode *code=activation->code;
    RbcValueOrigin from=rbc_value_origin(activation,source),to={activation,target};
    const XrLirValueHome *home=rbc_value_home(code,target);
    if (home && home->owner_count) {
        const XrLirValueHome *source_home=rbc_value_home(code,source);
        if (!source_home || source_home->parameter || builder->owners>activation->edge_capacity ||
            home->owner_count>activation->edge_capacity-builder->owners) return XRT_MANAGED_ARGUMENT;
        for (size_t leaf=0;leaf<home->owner_count;++leaf) {
            XrtPlaceEdge *edge=&activation->edges[builder->owners++];
            XrtManagedStatus status=rbc_value_place(to,false,leaf,&edge->destination);
            if (status==XRT_MANAGED_OK) status=rbc_value_place(from,false,leaf,&edge->source);
            if (status==XRT_MANAGED_OK) status=rbc_value_place(to,true,leaf,&edge->temporary);
            if (status!=XRT_MANAGED_OK) return status;
        }
    }
    RbcValuePayload destination,source_payload;
    XrtManagedStatus status=rbc_value_payload(builder->module,builder->stack,to,&destination);
    if (status==XRT_MANAGED_OK) status=rbc_value_payload(builder->module,builder->stack,from,&source_payload);
    if (status==XRT_MANAGED_OK) status=rbc_value_snapshot(activation,destination,source_payload,
        home && home->owner_count,&builder->bytes);
    return status;
}
static XrtManagedStatus rbc_value_edge(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, XrRbcValueAccess access) {
    const XrRbcValueCode *code=activation->code;
    RbcValueEdgeBuilder builder={module,stack,activation,0,0};
    activation->publication_count=0;
    if (access.index!=UINT32_MAX) {
        XrLirHomeEdge edge=code->homes->edges[access.index];
        for (size_t p=0;p<edge.count;++p) {
            XrLirHomeTransfer transfer=code->homes->transfers[edge.first+p];
            XrtManagedStatus status=rbc_value_edge_pair(&builder,transfer.source,transfer.target);
            if (status!=XRT_MANAGED_OK) return status;
        }
    }
    for (uint32_t p=0;p<access.count;++p) {
        uint32_t source=code->arguments[access.first+(size_t)p*2],target=code->arguments[access.first+(size_t)p*2+1];
        const XrLirValueHome *home=rbc_value_home(code,target);
        if (home && home->type) continue;
        XrtManagedStatus status=rbc_value_edge_pair(&builder,source,target);
        if (status!=XRT_MANAGED_OK) return status;
    }
    const XrtPlacePublication publication={rbc_value_publication,activation};
    return xrt_place_edge_move(activation->edges,builder.owners,&publication);
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
    if (copy) {
        if (home->owner_count!=1) return XRT_MANAGED_ARGUMENT;
        XrtPlace to,from_place;
        status=rbc_value_place(destination,false,0,&to);
        if (status==XRT_MANAGED_OK) status=rbc_value_place(source,false,0,&from_place);
        return status==XRT_MANAGED_OK ? xrt_place_copy_replace(to,from_place,&publication) : status;
    }
    for (size_t leaf=0;leaf<home->owner_count;++leaf) {
        XrtPlaceEdge *edge=&scratch->edges[leaf];
        status=rbc_value_place(destination,false,leaf,&edge->destination);
        if (status==XRT_MANAGED_OK) status=rbc_value_place(source,false,leaf,&edge->source);
        if (status==XRT_MANAGED_OK) status=rbc_value_place(destination,true,leaf,&edge->temporary);
        if (status!=XRT_MANAGED_OK) return status;
    }
    return xrt_place_edge_move(scratch->edges,home->owner_count,&publication);
}
static XrtManagedStatus rbc_value_factory(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, XrRbcValueAccess access, uint32_t source) {
    const XrLirValueHome *home=rbc_value_home(activation->code,access.result);
    const XrtBackingTypes *types=NULL;
    XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source),&types);
    if (status!=XRT_MANAGED_OK) return status;
    const XrtStringBacking *left=NULL,*right=NULL;
    if (access.operation==XR_RBC_VALUE_STRING_CONCAT) {
        status=rbc_value_string_borrow(activation,access.source,&left);
        if (status==XRT_MANAGED_OK) status=rbc_value_string_borrow(activation,access.other,&right);
        if (status==XRT_MANAGED_OK) status=xrt_string_backing_concat_create(activation->frame,
            home->temporary_first_owner,types,left,right,0);
    } else if (access.operation==XR_RBC_VALUE_STRING_LITERAL) {
        XrRbcLiteral literal=module->literals[access.index];
        status=xrt_string_backing_create(activation->frame,home->temporary_first_owner,types,literal.bytes,literal.length,0);
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
static XrtManagedStatus rbc_value_state(XrRbcStack *stack, RbcValueActivation *activation,
    XrRbcValueAccess access) {
    XrRbcRegister *operand=rbc_value_register(stack,activation,access.source);
    if (!operand) return XRT_MANAGED_ARGUMENT;
    if (access.operation==XR_RBC_VALUE_STATE_READ) {
        if (!operand->state_ref || operand->state_ref->kind!=XRT_VALUE_STRING) return XRT_MANAGED_ARGUMENT;
        XrtExternalOwner external;
        XrtManagedStatus status=xrt_module_external_owner(operand->state_ref->reference,0,&external);
        XrtPlace destination;
        if (status==XRT_MANAGED_OK) status=rbc_value_place((RbcValueOrigin){activation,access.result},false,0,&destination);
        return status==XRT_MANAGED_OK ? xrt_place_copy_replace(destination,
            (XrtPlace){.external=&external,.nulls=XRT_PLACE_NONNULL},NULL) : status;
    }
    const XrtStringBacking *string=NULL;
    XrtManagedStatus status=rbc_value_string_borrow(activation,access.other,&string);
    if (status!=XRT_MANAGED_OK) return status;
    XrtManagedHeader *borrow=(XrtManagedHeader *)string;
    if (access.operation==XR_RBC_VALUE_STATE_WRITE) {
        if (!operand->state_ref || operand->state_ref->kind!=XRT_VALUE_STRING) return XRT_MANAGED_ARGUMENT;
        return xrt_module_replace(operand->state_ref->reference,&borrow);
    }
    if (access.operation==XR_RBC_VALUE_STATE_INITIALIZE) return xrt_module_initialize(operand->state,access.index,&borrow);
    XrtModuleRef *created=NULL;
    status=xrt_module_construct(operand->state,access.index,&borrow,&created);
    if (status==XRT_MANAGED_OK) {
        XrtModuleAccess *cache=&stack->module_accesses[(size_t)access.index*2];
        status=xrt_module_reference_cache(created,cache);
        XrRbcRegister *result=rbc_value_register(stack,activation,access.result);
        if (status==XRT_MANAGED_OK && !result) status=XRT_MANAGED_ARGUMENT;
        if (status==XRT_MANAGED_OK) result->state_ref=cache;
    }
    return status;
}
static XrRbcStatus rbc_value_call(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *caller, XrRbcValueAccess access, size_t pc, uint32_t source) {
    size_t parent=stack->depth-1;
    XrRbcFrame *frame=&stack->frames[parent];
    const XrRbcFunction *callee=&module->functions[access.index];
    if (frame->base>SIZE_MAX-access.window-1) return XR_RBC_OUT_OF_MEMORY;
    size_t base=frame->base+access.window+1;
    if (!xr_rbc_call_reserve(stack,base,callee)) return XR_RBC_OUT_OF_MEMORY;
    /* Reserve may move either bank. Snapshot every non-owner argument before
     * entry initialization or overlapping call-window writes touch a source. */
    size_t captured=0;
    for (uint32_t p=0;p<access.count;++p) {
        uint32_t value=caller->code->arguments[access.first+p];
        const XrLirValueHome *home=rbc_value_home(caller->code,value);
        caller->publications[p]=(RbcValuePublication){0};
        if (home && home->owner_count) continue;
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
    const XrRbcValueCode *receiving=xr_rbc_module_value_code(module,access.index);
    size_t cell_base=stack->cell_count,spill_base=stack->spill_count;
    stack->frames[stack->depth++]=(XrRbcFrame){access.index,0,base,receiving ? access.result : access.window,
        cell_base,0,spill_base,0};
    xr_rbc_entry_clear(stack->slots+base,&stack->entries[access.index]);
    stack->cell_count+=callee->cell_count; stack->spill_count+=callee->spill_count;
    if (callee->aggregate_size && !xr_rbc_aggregate_reserve(stack,&stack->frames[parent+1],callee)) return XR_RBC_OUT_OF_MEMORY;
    XrRbcStatus result=xr_rbc_values_begin(module,stack,access.index,source);
    if (result!=XR_RBC_OK) return result;
    RbcValueActivation *child=receiving ? stack->values->top : NULL;
    if (child) child->receiving_call=true;
    for (uint32_t p=0;p<access.count;++p) {
        uint32_t from=caller->code->arguments[access.first+p];
        const XrLirValueHome *home=rbc_value_home(caller->code,from);
        if (receiving) {
            uint32_t to=receiving->parameters[p];
            const XrLirValueHome *formal=rbc_value_home(receiving,to);
            if (formal && formal->type) {
                if (!home || home->type!=formal->type || formal->parameter!=p+1) return XR_RBC_INVALID;
                if (formal->owner_count) { child->parameters[p]=rbc_value_origin(caller,from); continue; }
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
    return XR_RBC_OK;
}
static XrRbcStatus rbc_value_return(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, XrRbcValueAccess access, uint32_t source) {
    size_t depth=stack->depth;
    XrRbcFrame frame=stack->frames[depth-1];
    XrRbcStatus result=xr_rbc_closure_scope_return(module,stack,NULL,0,source);
    if (result!=XR_RBC_OK) return result;
    const XrLirValueHome *home=rbc_value_home(activation->code,access.source);
    XrtManagedStatus status=XRT_MANAGED_OK;
    if (home && home->owner_count) {
        RbcValueOrigin from=rbc_value_origin(activation,access.source);
        if (depth>1) {
            if (!activation->receiving_call) return XR_RBC_INVALID;
            RbcValueActivation *caller=activation->previous;
            if (!caller || caller->depth!=depth-1) return XR_RBC_INVALID;
            status=rbc_value_replace(module,stack,activation,(RbcValueOrigin){caller,frame.destination},from,home->parameter!=0);
        } else {
            if (home->parameter || home->owner_count!=1 || !stack->value_result || *stack->value_result)
                return XR_RBC_INVALID;
            status=xrt_managed_slot_take(activation->frame,home->first_owner,stack->value_result);
        }
    } else {
        uint64_t bits=0;
        if (access.source) {
            XrRbcRegister *input=rbc_value_register(stack,activation,access.source);
            if (!input) return XR_RBC_INVALID;
            bits=input->bits;
        }
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
    stack->values->top=activation->previous;
    xrt_managed_metadata_free(stack->values->billing,activation,activation->bytes);
    stack->cell_count=frame.cell_base; stack->spill_count=frame.spill_base; --stack->depth;
    return XR_RBC_OK;
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
    if (access.operation==XR_RBC_VALUE_CALL) return rbc_value_call(module,stack,activation,access,pc,source);
    if (access.operation==XR_RBC_VALUE_RETURN) return rbc_value_return(module,stack,activation,access,source);
    XrtManagedStatus status=XRT_MANAGED_OK;
    if (access.operation<=XR_RBC_VALUE_STRING_CONCAT) status=rbc_value_factory(module,stack,activation,access,source);
    else if (access.operation==XR_RBC_VALUE_COPY || access.operation==XR_RBC_VALUE_MOVE)
        status=rbc_value_replace(module,stack,activation,(RbcValueOrigin){activation,access.result},
            rbc_value_origin(activation,access.source),access.operation==XR_RBC_VALUE_COPY);
    else if (access.operation==XR_RBC_VALUE_DROP) {
        const XrLirValueHome *home=rbc_value_home(activation->code,access.source);
        status=xrt_managed_slot_drop(activation->frame,home->first_owner);
    } else if (access.operation==XR_RBC_VALUE_STRING_PRINT || access.operation==XR_RBC_VALUE_STRING_LENGTH) {
        const XrtStringBacking *string=NULL;
        status=rbc_value_string_borrow(activation,access.source,&string);
        if (status==XRT_MANAGED_OK && access.operation==XR_RBC_VALUE_STRING_PRINT)
            xrt_print_string(string->bytes,string->length,source);
        else if (status==XRT_MANAGED_OK) {
            XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
            if (!output) status=XRT_MANAGED_ARGUMENT;
            else if (string->runes>INT64_MAX) status=XRT_MANAGED_OVERFLOW;
            else output->bits=(uint64_t)string->runes;
        }
    } else if (access.operation>=XR_RBC_VALUE_STATE_READ && access.operation<=XR_RBC_VALUE_STATE_INITIALIZE)
        status=rbc_value_state(stack,activation,access);
    else if (access.operation==XR_RBC_VALUE_EDGE) status=rbc_value_edge(module,stack,activation,access);
    else return XR_RBC_INVALID;
    if (status!=XRT_MANAGED_OK) xrt_native_managed_failure(status,source);
    stack->frames[stack->depth-1].pc=pc;
    return XR_RBC_OK;
}
