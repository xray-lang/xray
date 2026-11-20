/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Execute a sealed append sequence inside existing private capacity.
 */
static XR_NOINLINE bool rbc_value_array_push_segment(XrRbcStack *stack,RbcValueActivation *activation,uint32_t region) {
    const XrRbcValueCode *code=activation->code;
    const XrLirArrayRegionBindings *bindings=code->array_push_windows;
    const XrLirArrayRegionBinding *row=&bindings->regions[region-1];
    const XrRbcArrayPushLimit *limit=&code->array_push_limits[region-1];
    XrRbcRegister *iv=rbc_value_register(stack,activation,row->region.push_capacity.iv);
    if (!iv || !activation->backings_ready) return false;
    uint64_t start=iv->bits,end=limit->bits;
    if (limit->value) {
        XrRbcRegister *bound=rbc_value_register(stack,activation,limit->value);
        if (!bound) return false;
        end=bound->bits;
    }
    if (start>INT64_MAX || end>INT64_MAX) return false;
    if (start>=end) return true;
    const XrtArrayI64RegionContext context={activation->frame,activation->payload,
        &activation->frame_locations,&code->homes->frame};
    const XrtArrayI64RegionSpec spec=xr_lir_array_region_guard_spec(bindings,row);
    XrtArrayI64RegionCache cache={0};
    if (!xrt_array_i64_region_try_begin(&context,&spec,true,&cache)) return false;
    XrtArrayBacking *array=(XrtArrayBacking *)cache.owner;
    size_t room=array->capacity-cache.length;
    uint64_t length_room=(uint64_t)INT64_MAX-cache.length,remaining=end-start;
    if (length_room<room) room=(size_t)length_room;
    size_t count=remaining<room ? (size_t)remaining : room;
    if (!count) return true;
    /* Every RHS is the original current induction value. No observer,
     * failure or allocation can run before the next ordinary header. */
    for (size_t n=0;n<count;++n) {
        int64_t item=(int64_t)(start+n);
        memcpy(cache.elements+(cache.length+n)*sizeof(item),&item,sizeof(item));
    }
    array->length=cache.length+count;
    iv->bits=start+count;
    return true;
}

static void rbc_value_array_push_after_edge(XrRbcStack *stack,RbcValueActivation *activation,XrRbcValueAccess access) {
    const XrLirArrayRegionEdge *edge=&activation->code->array_push_windows->edges[access.index];
    if (edge->kind==XR_LIR_ARRAY_REGION_EDGE_ENTRY) {
        activation->blocked_array_push=0;
    } else if (activation->blocked_array_push!=access.window &&
        !rbc_value_array_push_segment(stack,activation,access.window)) {
        activation->blocked_array_push=access.window;
    }
}
