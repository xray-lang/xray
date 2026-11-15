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
    if ((home && (home->owner_count || (home->type && code->locations[value].kind==XR_RBC_VALUE_NOWHERE))) || rbc_value_binding(code,value)) { *output=(RbcValueEdgeState){0}; return true; }
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
    if ((home && (home->owner_count || (home->type && code->locations[value].kind==XR_RBC_VALUE_NOWHERE))) || rbc_value_binding(code,value)) return true;
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
        xr_rbc_state_kind(state->module,incoming.value.origin)==kind &&
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
    if (access.index==XR_LIR_ARRAY_OP_SET || access.index==XR_LIR_ARRAY_OP_PUSH) {
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
static bool rbc_value_transfer(RbcStringAdmission *state, uint32_t index) {
    const XrRbcValueCode *code=rbc_value_code(state->module,state->function);
    if (!code || !code->accesses || index>=code->access_count) return false;
    XrRbcValueAccess access=code->accesses[index];
    RbcValueEdgeState incoming={0};
    switch (access.operation) {
    case XR_RBC_VALUE_STRING_I64: case XR_RBC_VALUE_STRING_F64: case XR_RBC_VALUE_DISPLAY:
        return rbc_value_flow_bits(state,code,access.source);
    case XR_RBC_VALUE_STRING_LENGTH:
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_STATE_READ:
        return rbc_value_flow_reference(state,code,access.source,rbc_value_kind(state->module,code,access.result),false);
    case XR_RBC_VALUE_STATE_WRITE:
        return rbc_value_flow_reference(state,code,access.source,rbc_value_kind(state->module,code,access.other),true);
    case XR_RBC_VALUE_STATE_CONSTRUCT: case XR_RBC_VALUE_STATE_INITIALIZE:
        if (!rbc_value_flow_read(state,code,access.source,&incoming) || incoming.value.state!=RBC_STATE_BORROWED)
            return false;
        return access.operation==XR_RBC_VALUE_STATE_INITIALIZE || rbc_value_flow_write(state,code,access.result,
            (RbcValueEdgeState){.value={xr_rbc_state_shape(state->module,access.index),RBC_STATE_REFERENCE}});
    case XR_RBC_VALUE_CALL:
        for (uint32_t p=0;p<access.count;++p)
            if (!rbc_value_flow_read(state,code,code->arguments[access.first+p],&incoming)) return false;
        if (!access.result) return true;
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_RETURN: {
        if (rbc_scope_current(state)) return false;
        const XrLirValueHome *home=rbc_value_home(code,access.source);
        if (rbc_value_inline(state->module,code,access.source,false)) {
            if (!home || home->type!=code->homes->result_type) return false;
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
    case XR_RBC_VALUE_RECORD_NEW:
        for (uint32_t p=0;p<access.count;++p)
            if (!rbc_value_flow_read(state,code,code->arguments[access.first+p],&incoming)) return false;
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_RECORD_FIELD: case XR_RBC_VALUE_NULLABLE_SOME:
    case XR_RBC_VALUE_NULLABLE_IS_SOME: case XR_RBC_VALUE_NULLABLE_UNWRAP:
        return rbc_value_flow_read(state,code,access.source,&incoming) &&
            rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_NULLABLE_NONE:
        return rbc_value_flow_write(state,code,access.result,(RbcValueEdgeState){.value={0,RBC_BITS}});
    case XR_RBC_VALUE_ARRAY: return rbc_value_array_transfer(state,code,access);
    case XR_RBC_VALUE_MAP: return rbc_value_map_transfer(state,code,access);
    case XR_RBC_VALUE_BINDING_NEW: return true;
    case XR_RBC_VALUE_BINDING_READ: return rbc_value_flow_binding(state,code,access.source,false);
    case XR_RBC_VALUE_BINDING_WRITE: return rbc_value_flow_binding(state,code,access.source,true);
    case XR_RBC_VALUE_BINDING_DROP: return rbc_value_flow_binding(state,code,access.source,false);
    case XR_RBC_VALUE_STRING_LITERAL: case XR_RBC_VALUE_STRING_CONCAT:
    case XR_RBC_VALUE_COPY: case XR_RBC_VALUE_MOVE: case XR_RBC_VALUE_DROP: case XR_RBC_VALUE_STRING_PRINT:
        return true;
    default: return false;
    }
}
