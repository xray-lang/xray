/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_transfer.inc.c - Typed SSA operations in the existing scope CFG
 *
 * KEY CONCEPT:
 *   Authentic homes carry the verified owning facts. Ordinary bank definitions,
 *   State provenance and lexical scopes still pass the bytecode CFG receiver.
 */
static bool rbc_value_flow_read(RbcStringAdmission *state, const XrRbcValueCode *code,
    uint32_t value, RbcValueEdgeState *output) {
    if (!rbc_value_id(code,value)) return false;
    const XrLirValueHome *home=rbc_value_home(code,value);
    if (rbc_value_slice(state->module,code,value)) {
        /* Only the authentic shared producer grants this logical loan. The
         * receiving CFG transports its natural type, not another lifetime. */
        *output=(RbcValueEdgeState){.value={0,RBC_SLICE_BORROWED}};
        return true;
    }
    if (rbc_value_cell(state->module,code,value,false)) {
        /* The sealed producer already checked lexical Cell availability.
         * This receiver transports its complete borrowed identity only. */
        *output=(RbcValueEdgeState){.value={0,RBC_BITS}}; return true;
    }
    if (rbc_value_function(state->module,code,value,false)) {
        RbcFunctionFlow *functions=rbc_function_liveness(state);
        if (!functions) return false;
        /* A legal outer BIND may outlive the source's logical scope while its
         * activation storage remains valid. Read this value's own capability. */
        RbcFunctionFlow live=functions[value];
        if (!live.storage || !rbc_scope_contains(rbc_function_scopes(state->module,state->function),
                live.scope,rbc_scope_current(state))) return false;
        *output=(RbcValueEdgeState){.value={rbc_value_signature(state->module,code,value),RBC_FUNCTION},.scope=live.storage};
        return true;
    }
    const XrLirHomeBinding *binding=rbc_value_binding(code,value);
    bool state_cap=rbc_value_binding_state_cap(code,value);
    if ((home && (home->owner_count || (home->type && code->locations[value].kind==XR_RBC_VALUE_NOWHERE))) ||
        (binding && !state_cap)) { *output=(RbcValueEdgeState){0}; return true; }
    XrRbcValueLocation location=code->locations[value];
    if (location.kind==XR_RBC_VALUE_AGGREGATE) {
        if (location.index>=state->function->aggregate_value_count || !state->aggregates[location.index]) return false;
        *output=(RbcValueEdgeState){.value={0,RBC_BITS}}; return true;
    }
    if (!rbc_value_location_valid(state->function,location)) return false;
    *output=(RbcValueEdgeState){0};
    if (location.kind==XR_RBC_VALUE_SPILL) output->value=state->spills[location.index];
    else {
        output->value=(RbcSpillState){state->origin[location.index],state->state[location.index]};
        const uint32_t *scopes=rbc_scope_state(state);
        if (scopes) output->scope=scopes[location.index+1];
    }
    return output->value.state!=RBC_EMPTY;
}
static bool rbc_value_flow_write(RbcStringAdmission *state, const XrRbcValueCode *code,
    uint32_t value, RbcValueEdgeState incoming) {
    if (!rbc_value_id(code,value)) return false;
    const XrLirValueHome *home=rbc_value_home(code,value);
    if (rbc_value_slice(state->module,code,value)) return incoming.value.state==RBC_SLICE_BORROWED;
    if (rbc_value_function(state->module,code,value,false)) {
        RbcFunctionFlow *functions=rbc_function_liveness(state);
        if (!functions || !incoming.scope || incoming.scope>3) return false;
        functions[value]=(RbcFunctionFlow){rbc_scope_current(state),(uint8_t)incoming.scope};
        return true;
    }
    const XrLirHomeBinding *binding=rbc_value_binding(code,value);
    bool state_cap=rbc_value_binding_state_cap(code,value);
    if ((home && (home->owner_count || (home->type && code->locations[value].kind==XR_RBC_VALUE_NOWHERE))) ||
        (binding && !state_cap)) return true;
    XrRbcValueLocation location=code->locations[value];
    if (location.kind==XR_RBC_VALUE_AGGREGATE) {
        if (location.index>=state->function->aggregate_value_count) return false;
        state->aggregates[location.index]=1; return true;
    }
    if (!rbc_value_location_valid(state->function,location)) return false;
    if (location.kind==XR_RBC_VALUE_SPILL) { state->spills[location.index]=incoming.value; return true; }
    uint32_t index=location.index;
    XrRbcRegisterKind kind=rbc_register_kind(state->function,index);
    bool accepted=incoming.value.state==RBC_BITS ? rbc_bits_destination(state,index) :
        incoming.value.state==RBC_STATE_REFERENCE ? kind==XR_RBC_STATE_REF :
        incoming.value.state==RBC_STATE_BORROWED ? kind==XR_RBC_STATE || kind==XR_RBC_CALL_WINDOW :
        rbc_function_state(incoming.value.state) ? kind==XR_RBC_FUNCTION || kind==XR_RBC_CALL_WINDOW : false;
    if (!accepted || state->state[index]==RBC_STRING_OWNED || state->state[index]==RBC_CELL_OWNED) return false;
    state->state[index]=incoming.value.state; state->origin[index]=incoming.value.origin;
    uint32_t *scopes=rbc_scope_state(state);
    if (scopes) scopes[index+1]=incoming.scope;
    return true;
}
static bool rbc_value_flow_bits(RbcStringAdmission *state, const XrRbcValueCode *code, uint32_t value) {
    RbcValueEdgeState incoming;
    return rbc_value_flow_read(state,code,value,&incoming) && incoming.value.state==RBC_BITS;
}
static bool rbc_value_flow_reference(RbcStringAdmission *state, const XrRbcValueCode *code,
    uint32_t value, XrtValueKind kind, bool write) {
    RbcValueEdgeState incoming;
    return rbc_value_flow_read(state,code,value,&incoming) && incoming.value.state==RBC_STATE_REFERENCE &&
        xr_rbc_state_kind(state->module,incoming.value.origin)==(uint32_t)kind &&
        (!write || (incoming.value.origin&1u));
}
static bool rbc_value_flow_binding(RbcStringAdmission *state,const XrRbcValueCode *code,uint32_t value,bool write) {
    const XrLirHomeBinding *binding=rbc_value_binding(code,value);
    if (!binding) return false;
    if (binding->kind==XR_LIR_HOME_BINDING_CELL_BORROW) binding=rbc_value_binding(code,binding->source);
    if (!binding) return false;
    if (binding->kind!=XR_LIR_HOME_BINDING_STATE_BORROW) return true;
    return rbc_value_flow_reference(state,code,binding->source,rbc_value_kind(state->module,code,value),write);
}
static bool rbc_value_array_transfer(RbcStringAdmission *state,const XrRbcValueCode *code,XrRbcValueAccess access) {
    const uint32_t *args=access.count ? code->arguments+access.first : NULL;
    if (xr_lir_array_binding_update((XrLirArrayOperation)access.index)) {
        if (!rbc_value_flow_binding(state,code,args[0],true)) return false;
        for (uint32_t p=1;p<access.count;++p) {
            RbcValueEdgeState input; if (!rbc_value_flow_read(state,code,args[p],&input)) return false;
        }
        return true;
    }
    for (uint32_t p=0;p<access.count;++p) {
        RbcValueEdgeState input; if (!rbc_value_flow_read(state,code,args[p],&input)) return false;
    }
    return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
}
static bool rbc_value_map_transfer(RbcStringAdmission *state,const XrRbcValueCode *code,XrRbcValueAccess access) {
    const uint32_t *args=access.count ? code->arguments+access.first : NULL;
    bool mutate=access.index==XR_LIR_MAP_OP_SET || access.index==XR_LIR_MAP_OP_REMOVE || access.index==XR_LIR_MAP_OP_CLEAR;
    uint32_t first=0;
    if (mutate) {
        if (!rbc_value_flow_binding(state,code,args[0],true)) return false;
        first=1;
    }
    for (uint32_t p=first;p<access.count;++p) {
        RbcValueEdgeState input;
        if (!rbc_value_flow_read(state,code,args[p],&input)) return false;
    }
    return !access.result || rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
}
static bool rbc_value_set_transfer(RbcStringAdmission *state,const XrRbcValueCode *code,XrRbcValueAccess access) {
    const uint32_t *args=access.count ? code->arguments+access.first : NULL;
    bool mutate=access.index==XR_LIR_SET_ADD || access.index==XR_LIR_SET_REMOVE || access.index==XR_LIR_SET_CLEAR;
    uint32_t first=0;
    if (mutate) {
        if (!rbc_value_flow_binding(state,code,args[0],true)) return false;
        first=1;
    }
    for (uint32_t p=first;p<access.count;++p) {
        RbcValueEdgeState input;
        if (!rbc_value_flow_read(state,code,args[p],&input)) return false;
    }
    return !access.result || rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
}
static bool rbc_value_transfer(RbcStringAdmission *state, uint32_t index) {
    const XrRbcValueCode *code=rbc_value_code(state->module,state->function);
    if (!code || !code->accesses || index>=code->access_count) return false;
    XrRbcValueAccess access=code->accesses[index];
    RbcValueEdgeState incoming={0};
    switch (access.operation) {
    case XR_RBC_VALUE_MAP_LITERAL:
        for (uint32_t p=0; p<access.count; ++p)
            if (!rbc_value_flow_read(state,code,code->arguments[access.first+p],&incoming)) return false;
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_CELL: {
        const XrLirHomeEvent *event=&code->homes->events[access.index-1];
        if (event->kind==XR_LIR_OWNERSHIP_RELEASE) return true;
        if (event->kind==XR_LIR_OWNERSHIP_CELL_VALUE || event->kind==XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT) {
            if (access.source && !rbc_value_flow_read(state,code,access.source,&incoming)) return false;
            if (access.other && !rbc_callable_scope_transfer(state,(XrRbcAggregateAccess){
                .operation=event->kind==XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT ? XR_RBC_FUNCTION_SCOPE_LEAVE : XR_RBC_FUNCTION_BIND,
                .destination=access.other})) return false;
            return !access.result || rbc_value_flow_write(state,code,access.result,incoming);
        }
        if (event->cell_operation==XR_LIR_CELL_OP_NEW)
            return rbc_value_flow_read(state,code,access.source,&incoming);
        if (!rbc_value_flow_read(state,code,access.source,&incoming)) return false;
        if (event->cell_operation==XR_LIR_CELL_OP_WRITE)
            return rbc_value_flow_read(state,code,access.other,&incoming);
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    }
    case XR_RBC_VALUE_STRING_APPEND_OWNED:
        return rbc_value_flow_read(state,code,access.source,&incoming) &&
            rbc_value_flow_read(state,code,access.other,&incoming) &&
            rbc_value_flow_write(state,code,access.result,incoming);
    case XR_RBC_VALUE_STRING_APPEND_BINDING:
        return rbc_value_flow_binding(state,code,access.source,true) &&
            rbc_value_flow_read(state,code,access.other,&incoming);
    case XR_RBC_VALUE_SLICE: {
        if (access.index==XR_LIR_SLICE_OP_CLEANUP) return true;
        const uint32_t *args=code->arguments+access.first;
        if (access.index==XR_LIR_SLICE_OP_NEW) {
            bool binding;
            if (access.source==XR_LIR_STATE_REF) {
                XrtValueKind kind=rbc_value_kind(state->module,code,access.result);
                binding=rbc_value_flow_reference(state,code,args[0],
                    kind==XRT_VALUE_SLICE_I64 ? XRT_VALUE_ARRAY_I64 : XRT_VALUE_ARRAY_U8,false);
            } else binding=rbc_value_flow_binding(state,code,args[0],false);
            if (!binding || (access.source!=XR_LIR_STRING_CELL &&
                (!rbc_value_flow_bits(state,code,args[1]) || !rbc_value_flow_bits(state,code,args[2])))) return false;
        } else {
            if (!rbc_value_flow_read(state,code,args[0],&incoming) || incoming.value.state!=RBC_SLICE_BORROWED) return false;
            for (uint32_t p=1;p<access.count;++p) if (!rbc_value_flow_bits(state,code,args[p])) return false;
        }
        if (access.index==XR_LIR_SLICE_OP_END) return true;
        bool construct=access.index==XR_LIR_SLICE_OP_NEW || access.index==XR_LIR_SLICE_OP_REBORROW || access.index==XR_LIR_SLICE_OP_BIND;
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){
            .value={0,construct ? RBC_SLICE_BORROWED : RBC_BITS}});
    }
    case XR_RBC_VALUE_FUNCTION_NEW: {
        const XrRbcFunction *callee=&state->module->functions[access.index];
        for (uint32_t p=0;p<access.count;++p) {
            if (!rbc_value_flow_read(state,code,code->arguments[access.first+p],&incoming)) return false;
            if (p==callee->capture_count ? incoming.value.state!=RBC_STATE_BORROWED :
                callee->captures[p].kind==XR_RBC_CALLABLE_BINDING ? incoming.value.state!=RBC_CELL_BORROWED :
                callee->captures[p].kind==XR_RBC_CALLABLE_FUNCTION ? incoming.value.state!=RBC_FUNCTION :
                incoming.value.state!=RBC_BITS) return false;
        }
        if (access.window) {
            size_t home=state->function->aggregate_value_count+access.window-1;
            if (state->aggregates[home]) return false;
            state->aggregates[home]=1;
        }
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.scope=access.window ? 3u : 1u});
    }
    case XR_RBC_VALUE_FUNCTION_ALIAS:
        if (!rbc_value_flow_read(state,code,access.source,&incoming)) return false;
        if (access.other && !rbc_callable_scope_transfer(state,(XrRbcAggregateAccess){
                .operation=XR_RBC_FUNCTION_BIND,.destination=access.other})) return false;
        if (!rbc_value_flow_write(state,code,access.result,incoming)) return false;
        if (access.other) rbc_function_liveness(state)[access.result].scope=access.other;
        return true;
    case XR_RBC_VALUE_FUNCTION_SCOPE_EXIT: {
        if (access.source && !rbc_value_flow_read(state,code,access.source,&incoming)) return false;
        if (!rbc_callable_scope_transfer(state,(XrRbcAggregateAccess){
                .operation=XR_RBC_FUNCTION_SCOPE_LEAVE,.destination=access.other})) return false;
        return !access.source || rbc_value_flow_write(state,code,access.result,incoming);
    }
    case XR_RBC_VALUE_FUNCTION_CALL:
        if (!rbc_value_flow_read(state,code,access.source,&incoming)) return false;
        for (uint32_t p=0;p<access.count;++p)
            if (!rbc_value_flow_read(state,code,code->arguments[access.first+p],&incoming)) return false;
        if (!access.result) return true;
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS},
            .scope=rbc_value_function(state->module,code,access.result,false) ? 2u : 0u});
    case XR_RBC_VALUE_DISPLAY:
        if (rbc_value_kind(state->module, code, access.source) == XRT_VALUE_TUPLE)
            return rbc_value_flow_read(state, code, access.source, &incoming);
        return rbc_value_flow_bits(state,code,access.source);
    case XR_RBC_VALUE_STRING_I64: case XR_RBC_VALUE_STRING_F64:
        return rbc_value_flow_bits(state,code,access.source);
    case XR_RBC_VALUE_STRING_EQUAL: case XR_RBC_VALUE_STRING_NOT_EQUAL:
        return rbc_value_flow_bits(state,code,access.source) &&
            rbc_value_flow_bits(state,code,access.other) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_STRING_LENGTH:
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_STRING_BYTE_LENGTH:
        return rbc_value_flow_read(state,code,access.source,&incoming) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_STRING_REPLACE:
        return rbc_value_flow_read(state,code,access.source,&incoming) &&
            rbc_value_flow_read(state,code,access.other,&incoming) &&
            rbc_value_flow_read(state,code,access.index,&incoming);
    case XR_RBC_VALUE_STRING_SLICE:
        return rbc_value_flow_read(state,code,access.source,&incoming) &&
            rbc_value_flow_bits(state,code,access.other) && rbc_value_flow_bits(state,code,access.index);
    case XR_RBC_VALUE_STRING_INDEX_OF:
        return rbc_value_flow_read(state,code,access.source,&incoming) && rbc_value_flow_read(state,code,access.other,&incoming) &&
            rbc_value_flow_bits(state,code,access.index) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_STRING_ORDER:
        return rbc_value_flow_read(state,code,access.source,&incoming) && rbc_value_flow_read(state,code,access.other,&incoming) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_STRING_LAST_INDEX_OF:
        return rbc_value_flow_read(state,code,access.source,&incoming) && rbc_value_flow_read(state,code,access.other,&incoming) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_STRING_TEST:
        return rbc_value_flow_read(state,code,access.source,&incoming) && rbc_value_flow_read(state,code,access.other,&incoming) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_HOST_FILE_OPEN_WRITE: case XR_RBC_VALUE_HOST_FILE_READ:
        return rbc_value_flow_read(state,code,access.source,&incoming) && rbc_value_flow_bits(state,code,access.other);
    case XR_RBC_VALUE_HOST_PATH_CLASS: case XR_RBC_VALUE_HOST_ENV_GET:
    case XR_RBC_VALUE_HOST_TEXT_RESULT:
        return rbc_value_flow_read(state, code, access.source, &incoming);
    case XR_RBC_VALUE_HOST_DIRECTORY:
        if (access.index == XR_NATIVE_CALLABLE_IO_DIR_NEXT)
            return rbc_value_flow_bits(state, code, access.source);
        return (access.index == XR_NATIVE_CALLABLE_IO_DIR_OPEN ?
                rbc_value_flow_read(state, code, access.source, &incoming) :
                rbc_value_flow_bits(state, code, access.source)) &&
            rbc_value_flow_write(state, code, access.result, (RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_HOST_DIRECTORY_CLEANUP_CLOSE:
        return rbc_value_flow_bits(state, code, access.source);
    case XR_RBC_VALUE_HOST_DIRECTORY_CLEANUP_POLL:
    case XR_RBC_VALUE_HOST_DIRECTORY_FRAME:
        return true;
    case XR_RBC_VALUE_HOST_DIRECTORY_NEXT_CHECKED:
    case XR_RBC_VALUE_CHECKED_EMPTY_ARRAY:
        if ((access.source && !rbc_value_flow_bits(state,code,access.source)) || access.window >= state->function->register_count ||
            !rbc_bits_destination(state,access.window)) return false;
        state->state[access.window] = RBC_BITS;
        state->origin[access.window] = index + 1;
        return true;
    case XR_RBC_VALUE_HOST_DIRECTORY_CLOSE_CHECKED:
        return rbc_value_flow_bits(state,code,access.source) &&
            (!access.other || rbc_value_flow_bits(state,code,access.other)) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_HOST_DIRECTORY_RETURN_GUARD:
        return (!access.source || rbc_value_flow_read(state,code,access.source,&incoming)) &&
            rbc_value_flow_bits(state,code,access.other);
    case XR_RBC_VALUE_HOST_DIRECTORY_FAIL:
        return !rbc_scope_current(state) && rbc_value_flow_bits(state,code,access.source) &&
            rbc_value_flow_bits(state,code,access.other);
    case XR_RBC_VALUE_MANAGED_FAILURE:
        return !rbc_scope_current(state) && rbc_value_flow_bits(state,code,access.source);
    case XR_RBC_VALUE_HOST_PATH_BOOL: case XR_RBC_VALUE_HOST_FILE_CLOSE:
        return rbc_value_flow_read(state,code,access.source,&incoming) &&
            (!access.other || rbc_value_flow_read(state,code,access.other,&incoming)) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_HOST_FILE_WRITE:
        return rbc_value_flow_read(state, code, access.source, &incoming) &&
            rbc_value_flow_read(state, code, access.other, &incoming) &&
            rbc_value_flow_bits(state, code, code->arguments[access.first]) &&
            rbc_value_flow_write(state, code, access.result, (RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_STATE_READ:
        return rbc_value_flow_reference(state,code,access.source,rbc_value_kind(state->module,code,access.result),false) &&
            (!rbc_value_function(state->module,code,access.result,false) ||
                rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.scope=1}));
    case XR_RBC_VALUE_STATE_WRITE:
        return rbc_value_flow_reference(state,code,access.source,rbc_value_kind(state->module,code,access.other),true);
    case XR_RBC_VALUE_STATE_GROUP_INITIALIZE:
        return rbc_value_flow_read(state,code,access.source,&incoming) && incoming.value.state==RBC_STATE_BORROWED &&
            rbc_value_flow_read(state,code,access.other,&incoming);
    case XR_RBC_VALUE_STATE_CONSTRUCT: case XR_RBC_VALUE_STATE_INITIALIZE:
        if (!rbc_value_flow_read(state,code,access.source,&incoming) || incoming.value.state!=RBC_STATE_BORROWED)
            return false;
        return access.operation==XR_RBC_VALUE_STATE_INITIALIZE || rbc_value_flow_write(state,code,access.result,
            (RbcValueEdgeState){.value={xr_rbc_state_shape(state->module,access.index),RBC_STATE_REFERENCE}});
    case XR_RBC_VALUE_CALL: case XR_RBC_VALUE_INVOKE: {
        const XrRbcValueCode *callee=xr_rbc_module_value_code(state->module,access.index);
        for (uint32_t p=0;p<access.count;++p) {
            uint32_t actual=code->arguments[access.first+p];
            if (!rbc_value_flow_read(state,code,actual,&incoming)) return false;
            if (callee && rbc_value_i64_ref(state->module,callee,callee->parameters[p]) &&
                !rbc_value_i64_ref(state->module,code,actual) && incoming.value.state!=RBC_CELL_BORROWED &&
                !rbc_value_flow_reference(state,code,actual,XRT_VALUE_I64,true)) return false;
        }
        if (access.operation==XR_RBC_VALUE_INVOKE) {
            if (access.window>=state->function->register_count ||
                !rbc_bits_destination(state,access.window)) return false;
            state->state[access.window]=RBC_BITS;
            state->origin[access.window]=index+1;
            return true;
        }
        if (!access.result) return true;
        if (rbc_value_slice(state->module,code,access.result)) {
            return rbc_value_slice_call(state->module,code,callee,access) &&
                rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_SLICE_BORROWED}});
        }
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS},
            .scope=rbc_value_function(state->module,code,access.result,false) ? 2u : 0u});
    }
    case XR_RBC_VALUE_INVOKE_RESULT: case XR_RBC_VALUE_INVOKE_ERROR:
    case XR_RBC_VALUE_CHECKED_RESULT: case XR_RBC_VALUE_CHECKED_ERROR: {
        XrRbcValueAccess call=code->accesses[access.index];
        if (call.window>=state->function->register_count || state->state[call.window]!=RBC_BITS ||
            state->origin[call.window]!=access.index+1) return false;
        state->state[call.window]=RBC_EMPTY;
        state->origin[call.window]=0;
        return !access.result || rbc_value_flow_write(state,code,access.result,
            (RbcValueEdgeState){.value={0,RBC_BITS}});
    }
    case XR_RBC_VALUE_UNCAUGHT_ERROR:
        return !rbc_scope_current(state) && rbc_value_flow_read(state,code,access.source,&incoming);
    case XR_RBC_VALUE_PANIC_ERROR:
        return rbc_value_flow_read(state,code,access.source,&incoming);
    case XR_RBC_VALUE_RETURN: case XR_RBC_VALUE_RETURN_ERROR: {
        if (rbc_scope_current(state)) return false;
        const XrLirValueHome *home=rbc_value_home(code,access.source);
        uint32_t result_type = rbc_result_value_type(code);
        if (access.operation==XR_RBC_VALUE_RETURN_ERROR) {
            if (!rbc_value_flow_read(state,code,access.source,&incoming)) return false;
        } else if (rbc_callable_home_type(xr_rbc_module_value_image(state->module), result_type)) {
            if (!rbc_value_exact_home(state->module, code, access.source, result_type, false)) return false;
        } else if (rbc_value_slice(state->module,code,access.source)) {
            if (!rbc_value_slice_return_valid(state->module,code,access) ||
                !rbc_value_flow_read(state,code,access.source,&incoming) || incoming.value.state!=RBC_SLICE_BORROWED) return false;
        } else if (rbc_value_function(state->module,code,access.source,false)) {
            if (!rbc_value_flow_read(state,code,access.source,&incoming) || incoming.scope==3) return false;
        } else if (rbc_value_inline(state->module,code,access.source,false)) {
            if (!home || home->type!=result_type) return false;
        } else if (state->function->aggregate_result_type) {
            if (!rbc_value_flow_bits(state,code,access.source)) return false;
        } else if (rbc_managed_kind(state->function->result_kind)) {
            if (!home || !home->owner_count) return false;
        } else if (state->function->result_kind!=XR_RBC_BITS ||
            (access.source && !rbc_value_flow_bits(state,code,access.source))) return false;
        for (uint32_t r=0;r<state->function->register_count;++r)
            if (state->state[r]==RBC_STRING_OWNED || state->state[r]==RBC_CELL_OWNED ||
                state->state[r]==RBC_VIEW_FRAME || state->state[r]==RBC_VIEW_CELL ||
                state->state[r]==RBC_VIEW_SLICE || state->state[r]==RBC_VIEW_RETURNED) return false;
        return true;
    }
    case XR_RBC_VALUE_EDGE:
        if (access.count>state->value_edge_capacity || (access.count && !state->value_edges)) return false;
        for (uint32_t p=0;p<access.count;++p)
            if (!rbc_value_flow_read(state,code,code->arguments[access.first+(size_t)p*2],&state->value_edges[p])) return false;
        for (uint32_t p=0;p<access.count;++p)
            if (!rbc_value_flow_write(state,code,code->arguments[access.first+(size_t)p*2+1],state->value_edges[p])) return false;
        return true;
    case XR_RBC_VALUE_RECORD_NEW: case XR_RBC_VALUE_CLASS_NEW:
        for (uint32_t p=0;p<access.count;++p)
            if (!rbc_value_flow_read(state,code,code->arguments[access.first+p],&incoming)) return false;
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_SUM_NEW:
        for (uint32_t p=0;p<access.count;++p)
            if (!rbc_value_flow_read(state,code,code->arguments[access.first+p],&incoming)) return false;
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_SUM_TAG: case XR_RBC_VALUE_SUM_GET:
        return rbc_value_flow_read(state,code,access.source,&incoming) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_BOOL_TRY_PARSE:
    case XR_RBC_VALUE_I64_TRY_PARSE:
    case XR_RBC_VALUE_F64_TRY_PARSE:
    case XR_RBC_VALUE_RECORD_FIELD: case XR_RBC_VALUE_CLASS_FIELD: case XR_RBC_VALUE_NULLABLE_SOME:
    case XR_RBC_VALUE_NULLABLE_IS_SOME: case XR_RBC_VALUE_NULLABLE_UNWRAP:
        return rbc_value_flow_read(state,code,access.source,&incoming) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_CLASS_SCALAR_WRITE: case XR_RBC_VALUE_CLASS_STRING_WRITE:
    case XR_RBC_VALUE_CLASS_ARRAY_WRITE: case XR_RBC_VALUE_CLASS_ARRAY_PUSH:
        return rbc_value_flow_read(state,code,access.source,&incoming) && rbc_value_flow_read(state,code,access.other,&incoming);
    case XR_RBC_VALUE_NULLABLE_NONE:
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_ARRAY: return rbc_value_array_transfer(state,code,access);
    case XR_RBC_VALUE_MAP: return rbc_value_map_transfer(state,code,access);
    case XR_RBC_VALUE_SET: return rbc_value_set_transfer(state,code,access);
    case XR_RBC_VALUE_BINDING_NEW: return rbc_value_flow_read(state,code,access.source,&incoming);
    case XR_RBC_VALUE_BINDING_READ: return rbc_value_flow_binding(state,code,access.source,false) &&
        rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_BINDING_WRITE: return rbc_value_flow_binding(state,code,access.source,true) &&
        rbc_value_flow_read(state,code,access.other,&incoming);
    case XR_RBC_VALUE_BINDING_DROP: return rbc_value_flow_binding(state,code,access.source,false);
    case XR_RBC_VALUE_STRING_LITERAL: case XR_RBC_VALUE_STRING_CONCAT:
    case XR_RBC_VALUE_COPY: case XR_RBC_VALUE_MOVE:
        return !rbc_value_function(state->module,code,access.result,false) ||
            (rbc_value_flow_read(state,code,access.source,&incoming) && rbc_value_flow_write(state,code,access.result,incoming));
    case XR_RBC_VALUE_CLASS_ADOPT:
        return rbc_value_flow_read(state,code,access.source,&incoming) &&
            rbc_value_flow_write(state,code,access.result,incoming);
    case XR_RBC_VALUE_CLASS_IS:
        return rbc_value_flow_read(state,code,access.source,&incoming) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_DROP:
        if (rbc_value_function(state->module,code,access.source,false))
            rbc_function_liveness(state)[access.source]=(RbcFunctionFlow){0};
        return true;
    case XR_RBC_VALUE_STRING_PRINT:
        return true;
    case XR_RBC_VALUE_ASSERT_CONDITION:
        return rbc_value_flow_bits(state,code,access.source) &&
            rbc_value_flow_read(state,code,access.other,&incoming);
    default: return false;
    }
}
