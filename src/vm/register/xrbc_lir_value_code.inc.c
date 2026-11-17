/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_value_code.inc.c - Bind typed operations to the common receiving homes
 */
static void rbc_lir_value_code_free(XrRbcValueCode *code) {
    if (!code) return;
    xr_free((void *)code->locations);
    xr_free((void *)code->parameters);
    xr_free((void *)code->accesses);
    xr_free((void *)code->arguments);
    xr_free((void *)code->displays);
    xr_free((void *)code->scope_values);
    xr_free(code);
}
static void rbc_lir_value_codes_free(XrRbcValueCode **codes, size_t count) {
    for (size_t f = 0; codes && f < count; ++f) rbc_lir_value_code_free(codes[f]);
    xr_free(codes);
}
static bool rbc_lir_value_call(RbcLirCompiler *c,const XrLirInstructionView *instruction) {
    if (instruction->op!=XR_LIR_CALL || !c->homes || c->value_call_cursor>=c->homes->call_count) return false;
    const XrLirHomeCall *row=&c->homes->calls[c->value_call_cursor];
    if (row->block!=c->diagnostic->block || row->instruction!=c->diagnostic->instruction) return false;
    ++c->value_call_cursor;
    bool typed=instruction->result && c->homes->values[instruction->result].type &&
        c->value_code->locations[instruction->result].kind==XR_RBC_VALUE_NOWHERE;
    for (size_t p=0;p<row->count;++p) {
        uint32_t value=c->homes->arguments[row->first+p];
        typed|=(c->homes->values[value].type && c->value_code->locations[value].kind==XR_RBC_VALUE_NOWHERE) ||
            (c->homes->bindings && c->homes->bindings[value].type &&
             !(c->homes->bindings[value].kind==XR_LIR_HOME_BINDING_STATE_BORROW && c->homes->bindings[value].source==value));
    }
    if (!typed) return false;
    XrLirFunctionView callee;
    return xr_lir_function_view(c->input,(XrLirFunction){c->input,instruction->callee},&callee)==XR_LIR_OK &&
        callee.kind==XR_LIR_FUNCTION_DEFINITION;
}
static bool rbc_lir_value_string_slot(RbcLirCompiler *c, uint64_t slot) {
    const XrtModuleLayoutSpec *state = c->literals->program->module.state_spec;
    if (!state || slot >= state->slot_count) return false;
    uint32_t type = state->slots[slot].type;
    XrLirStateLayout logical={0};
    bool inline_value=xr_lir_state_layout_view(c->input,&logical)==XR_LIR_OK && slot<logical.slot_count &&
        rbc_lir_inline_type(c->input,logical.slots[slot].payload);
    return type && type <= state->values->type_count && (state->values->types[type - 1].kind == XRT_VALUE_STRING ||
        state->values->types[type - 1].kind == XRT_VALUE_MAP_STRING_I64 ||
        state->values->types[type - 1].kind == XRT_VALUE_RECORD ||
        state->values->types[type - 1].kind == XRT_VALUE_FUNCTION ||
        inline_value ||
        (state->values->types[type - 1].kind >= XRT_VALUE_ARRAY_I64 && state->values->types[type - 1].kind <= XRT_VALUE_ARRAY_U64));
}
static bool rbc_lir_value_needed(RbcLirCompiler *c) {
    XrLirClosureTarget target={0};
    if (xr_lir_closure_target_view(c->input,c->function,&target)==XR_LIR_OK && target.callable) return true;
    for (uint32_t v = 1; v <= c->view.value_count; ++v)
        if (xr_lir_string_binding(c->input,(XrLirValue){c->input,c->function.id,v}) ||
            rbc_lir_captured_cell(c->input,rbc_lir_value_type(c,v)) || rbc_lir_slice_type(c->input,rbc_lir_value_type(c,v)) || rbc_lir_callable_type(c->input,rbc_lir_value_type(c,v)) || rbc_lir_inline_type(c->input,rbc_lir_value_type(c,v)) || rbc_lir_value_type(c,v) == XR_LIR_STRING || rbc_lir_value_type(c,v) == XR_LIR_ARRAY ||
            rbc_lir_value_type(c,v) == XR_LIR_ARRAY_CELL || rbc_lir_value_type(c,v) == XR_LIR_ARRAY_REF ||
            rbc_lir_value_type(c,v) == XR_LIR_MAP_STRING_I64 || rbc_lir_value_type(c,v) == XR_LIR_MAP_STRING_I64_CELL ||
            rbc_lir_value_type(c,v) == XR_LIR_MAP_STRING_I64_REF) return true;
    if (rbc_lir_captured_cell(c->input,c->view.result) || rbc_lir_callable_type(c->input,c->view.result) || rbc_lir_inline_type(c->input,c->view.result) || c->view.result == XR_LIR_STRING || c->view.result == XR_LIR_ARRAY || c->view.result == XR_LIR_MAP_STRING_I64) return true;
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block; XrLirBlockView view;
        xr_lir_block_at(c->input,c->function,b,&block); xr_lir_block_view(c->input,block,&view);
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView instruction; xr_lir_instruction_view(c->input,block,n,&instruction);
            if ((instruction.op == XR_LIR_STATE_CONSTRUCT || instruction.op == XR_LIR_STATE_INITIALIZE) &&
                rbc_lir_value_string_slot(c,instruction.bits)) return true;
        }
    }
    return false;
}
static bool rbc_lir_value_code_prepare(RbcLirCompiler *c) {
    if (!c->typed_values) return true;
    if (c->homes && c->homes->value_count != c->view.value_count)
        return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"typed function has no common receiving homes");
    size_t count = (size_t)c->view.value_count + 1;
    if (count > SIZE_MAX / sizeof(XrRbcValueLocation))
        return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"typed value locations exceed address space");
    XrRbcValueCode *code = xr_calloc(1,sizeof(*code));
    if (!code) return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot own typed bytecode description");
    c->value_code = code;
    code->homes = c->homes; code->value_count = c->view.value_count;
    if (c->view.scope_count) {
        uint32_t *scopes=xr_calloc(c->view.scope_count,sizeof(*scopes));
        if (!scopes) return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot own raw scope capabilities");
        code->scope_values=scopes; code->scope_count=c->view.scope_count;
        for (uint32_t v=1;v<=c->view.value_count;++v)
            if (c->scope_home[v]!=UINT32_MAX) scopes[c->scope_home[v]-1]=v;
    }
    if (c->homes && c->homes->edge_count) {
        if (c->homes->edge_count > UINT32_MAX ||
            (c->view.block_count && 2 * sizeof(*c->value_edges) > SIZE_MAX / c->view.block_count))
            return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"typed edge index exceeds address space");
        size_t edges = (size_t)c->view.block_count * 2;
        c->value_edges = xr_malloc(edges * sizeof(*c->value_edges));
        if (!c->value_edges) return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot index common selected edges");
        for (size_t n = 0; n < edges; ++n) c->value_edges[n] = UINT32_MAX;
        for (uint32_t n = 0; n < c->homes->edge_count; ++n) {
            const XrLirHomeEdge *edge = &c->homes->edges[n];
            if (!edge->block || edge->block > c->view.block_count || !edge->edge || edge->edge > 2)
                return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"common selected edge has no LIR position");
            c->value_edges[((size_t)edge->block - 1) * 2 + edge->edge - 1] = n;
        }
    }
    XrRbcValueLocation *locations = xr_calloc(count,sizeof(*locations));
    code->locations = locations;
    if (!locations) return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot own typed value locations");
    if (c->view.parameter_count) {
        uint32_t *parameters = xr_malloc((size_t)c->view.parameter_count * sizeof(*parameters));
        if (!parameters) return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot own typed logical parameters");
        code->parameters = parameters; code->parameter_count = c->view.parameter_count;
        memcpy(parameters,c->view.parameters,(size_t)code->parameter_count * sizeof(*parameters));
    }
    for (uint32_t v = 1; v <= c->view.value_count; ++v) {
        code->string_binding_setup |= c->homes && c->homes->owned_frame_needed &&
            xr_lir_string_binding(c->input,(XrLirValue){c->input,c->function.id,v});
        if (rbc_lir_common_value(c, v)) continue;
        if (c->aggregate_home && c->aggregate_home[v] != UINT32_MAX)
            locations[v] = (XrRbcValueLocation){XR_RBC_VALUE_AGGREGATE,c->aggregate_home[v]};
        else if (c->spill_home[v] != UINT32_MAX)
            locations[v] = (XrRbcValueLocation){XR_RBC_VALUE_SPILL,c->spill_home[v]};
        else if (c->slots[v] != UINT32_MAX)
            locations[v] = (XrRbcValueLocation){XR_RBC_VALUE_REGISTER,c->slots[v]};
    }
    return true;
}
static bool rbc_lir_value_arguments(RbcLirCompiler *c, const uint32_t *values, uint32_t count, uint32_t *first) {
    XrRbcValueCode *code = c->value_code;
    if (code->argument_count > UINT32_MAX - count)
        return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"typed arguments exceed indexed capacity");
    void *storage = (void *)code->arguments;
    if (!rbc_lir_grow(c,&storage,&c->value_argument_capacity,code->argument_count + count,sizeof(*code->arguments))) return false;
    code->arguments = storage; *first = (uint32_t)code->argument_count;
    if (count) memcpy((uint32_t *)storage + code->argument_count,values,(size_t)count * sizeof(*values));
    code->argument_count += count;
    return true;
}
static bool rbc_lir_value_append(RbcLirCompiler *c, XrRbcValueAccess access) {
    XrRbcValueCode *code = c->value_code;
    if (!code) return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"typed operation has no receiving code description");
    if (code->access_count > UINT16_MAX)
        return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"typed operation table exceeds bytecode encoding");
    void *storage = (void *)code->accesses;
    if (!rbc_lir_grow(c,&storage,&c->value_access_capacity,code->access_count + 1,sizeof(*code->accesses))) return false;
    code->accesses = storage;
    ((XrRbcValueAccess *)storage)[code->access_count] = access;
    return rbc_lir_emit(c,XR_RBC_VALUE_ACCESS,0,0,0,(int64_t)code->access_count++);
}
static bool rbc_lir_value_edge(RbcLirCompiler *c, XrLirEdgeView edge, uint32_t ordinal) {
    XrRbcValueAccess access = {.operation=XR_RBC_VALUE_EDGE,.index=UINT32_MAX,.count=edge.argument_count};
    if (c->value_edges) access.index = c->value_edges[((size_t)c->diagnostic->block - 1) * 2 + ordinal - 1];
    XrLirBlockView target;
    xr_lir_block_view(c->input,(XrLirBlock){c->input,c->function.id,edge.target},&target);
    if (edge.argument_count > UINT32_MAX / 2)
        return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"selected edge arguments exceed indexed capacity");
    uint32_t first = 0;
    if (!rbc_lir_value_arguments(c,NULL,0,&first)) return false;
    access.first = first;
    for (uint32_t p = 0; p < edge.argument_count; ++p) {
        uint32_t pair[] = {edge.arguments[p],target.parameters[p]};
        if (!rbc_lir_value_arguments(c,pair,2,&first)) return false;
    }
    return rbc_lir_value_append(c,access);
}
static bool rbc_lir_value_display(RbcLirCompiler *c,const XrLirInstructionView *i,XrRbcValueAccess *access) {
    XrLirType type=rbc_lir_value_type(c,i->operands[0]);
    XrLirTypeLayout layout={0};
    if (xr_lir_type_layout(c->input,type,&layout)!=XR_LIR_OK || layout.managed || layout.size>UINT32_MAX)
        return rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"display value has no exact inline scalar layout");
    XrtPodDisplay display={.physical={(uint32_t)layout.size,layout.alignment,layout.scalar_count}};
    while (type>=XR_LIR_TYPE_COUNT) {
        XrLirTypeView shape={0};
        if (xr_lir_type_view(c->input,type,&shape)!=XR_LIR_OK || shape.kind!=XR_LIR_TYPE_NULLABLE || shape.element>=type ||
            display.nullable_depth==UINT32_MAX)
            return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"display lost its closed Nullable scalar");
        ++display.nullable_depth; type=shape.element;
    }
    display.scalar=lir_pod_scalar(type);
    XrRbcValueCode *code=c->value_code;
    if (display.scalar>=XR_POD_SCALAR_COUNT || code->display_count>UINT32_MAX)
        return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"display description exceeds its scalar or index range");
    void *storage=(void *)code->displays;
    if (!rbc_lir_grow(c,&storage,&c->value_display_capacity,code->display_count+1,sizeof(display))) return false;
    code->displays=storage;
    access->index=(uint32_t)code->display_count;
    ((XrtPodDisplay *)storage)[code->display_count++]=display;
    access->operation=XR_RBC_VALUE_DISPLAY;
    return true;
}
#include "xrbc_lir_value_callable.inc.c"
#include "xrbc_lir_value_cell.inc.c"
static bool rbc_lir_slice_access(RbcLirCompiler *c, const XrLirInstructionView *instruction,
    XrLirSliceIntrinsic shape, XrRbcValueAccess *access) {
    *access=(XrRbcValueAccess){.operation=XR_RBC_VALUE_SLICE,.result=instruction->result,
        .source=shape.operation==XR_LIR_SLICE_OP_NEW && shape.binding_type==XR_LIR_STATE_REF ? XR_LIR_STATE_REF : 0,
        .index=(uint32_t)shape.operation,.count=instruction->operand_count};
    if (!rbc_lir_value_arguments(c,instruction->operands,instruction->operand_count,&access->first)) return false;
    bool construct=shape.operation==XR_LIR_SLICE_OP_NEW || shape.operation==XR_LIR_SLICE_OP_REBORROW ||
        shape.operation==XR_LIR_SLICE_OP_BIND;
    if (construct || shape.operation==XR_LIR_SLICE_OP_END) {
        uint32_t value=construct ? instruction->result : instruction->operands[0];
        access->window=rbc_lir_callable_event(c,construct ? XR_LIR_OWNERSHIP_VIEW_VALUE : XR_LIR_OWNERSHIP_VIEW_END,value);
        if (!access->window) return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"Slice operation lost its common loan event");
    }
    return true;
}
static bool rbc_lir_string_binding_operation(RbcLirCompiler *c,uint32_t role,XrRbcValueAccess *access) {
    if (role==XR_LIR_STRING_OP_READ_REF) access->operation=XR_RBC_VALUE_BINDING_READ;
    else if (role==XR_LIR_STRING_OP_WRITE_REF) access->operation=XR_RBC_VALUE_BINDING_WRITE;
    else if (role==XR_LIR_STRING_OP_APPEND_CELL || role==XR_LIR_STRING_OP_APPEND_STATE || role==XR_LIR_STRING_OP_APPEND_REF) {
        access->operation=XR_RBC_VALUE_STRING_APPEND_BINDING;
        access->index=rbc_lir_callable_event(c,XR_LIR_OWNERSHIP_STRING_OPERATION,access->source);
        if (!access->index) return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"String binding append lost its common event");
    } else return rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"String operation has no fixed binding receiver");
    return true;
}
static bool rbc_lir_fixed_binding(RbcLirCompiler *c,uint32_t value) {
    const XrLirHomeBinding *binding=c->homes && c->homes->bindings ? &c->homes->bindings[value] : NULL;
    const XrtValueImage *image=xr_lir_value_image_view(c->literals->program->value_image);
    return binding && image && binding->type && binding->type<=image->type_count &&
        (image->types[binding->type-1].kind==XRT_VALUE_STRING ||
         image->types[binding->type-1].kind==XRT_VALUE_MAP_STRING_I64);
}
static bool rbc_lir_array_local_metadata(RbcLirCompiler *c,XrRbcValueAccess *access,uint32_t receiver) {
    const XrLirHomeBinding *binding=c->homes->bindings ? &c->homes->bindings[receiver] : NULL;
    const XrtValueImage *image=xr_lir_value_image_view(c->literals->program->value_image);
    if (!binding || binding->kind!=XR_LIR_HOME_BINDING_LOCAL || !binding->type || !image ||
        binding->type>image->type_count || image->types[binding->type-1].kind!=XRT_VALUE_ARRAY_I64) return true;
    while (c->array_local_cursor<c->homes->event_count) {
        size_t n=c->array_local_cursor++;
        const XrLirHomeEvent *event=&c->homes->events[n];
        if (event->kind!=XR_LIR_OWNERSHIP_ARRAY_OPERATION) continue;
        if (event->block<c->diagnostic->block ||
            (event->block==c->diagnostic->block && event->instruction<c->diagnostic->instruction)) continue;
        if (event->block!=c->diagnostic->block || event->instruction!=c->diagnostic->instruction ||
            event->source!=receiver || event->operation!=access->index)
            return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"local Array update lost its actual common operation");
        if (n>=UINT32_MAX) return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"local Array event exceeds typed code");
        /* ARRAY otherwise leaves other zero. This is a common event ordinal,
         * not mutation authority inferred from the live reference count. */
        access->other=(uint32_t)n+1;
        return true;
    }
    return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"local Array update lost its common operation");
}
static uint32_t rbc_lir_binding_event(RbcLirCompiler *c,uint32_t kind,uint32_t receiver) {
    while (c->binding_event_cursor<c->homes->event_count) {
        size_t n=c->binding_event_cursor++;
        const XrLirHomeEvent *event=&c->homes->events[n];
        if (event->kind<XR_LIR_OWNERSHIP_BINDING_NEW || event->kind>XR_LIR_OWNERSHIP_BINDING_WRITE ||
            event->kind==XR_LIR_OWNERSHIP_BINDING_BORROW || !event->source || !rbc_lir_fixed_binding(c,event->source)) continue;
        return event->kind==kind && event->block==c->diagnostic->block &&
            event->instruction==c->diagnostic->instruction && event->source==receiver && n<UINT32_MAX ? (uint32_t)n+1 : 0;
    }
    return 0;
}
static bool rbc_lir_array_forwarding_metadata(RbcLirCompiler *c,XrRbcValueAccess *access) {
    bool create=access->operation==XR_RBC_VALUE_BINDING_NEW;
    if (!c->homes || (!create && access->operation!=XR_RBC_VALUE_BINDING_READ)) return true;
    uint32_t receiver=create ? access->result : access->source;
    uint32_t value=create ? access->source : access->result;
    /* Other event families can precede the ordered binding facts. Advance only
     * through forwarding candidates before comparing their real LIR positions. */
    while (c->array_forwarding_cursor<c->homes->event_count) {
        size_t n=c->array_forwarding_cursor;
        const XrLirHomeEvent *event=&c->homes->events[n];
        if (!event->forwarding_mode && !event->forwarding_drop) { ++c->array_forwarding_cursor; continue; }
        if (event->block>c->diagnostic->block || (event->block==c->diagnostic->block &&
                event->instruction>c->diagnostic->instruction)) break;
        ++c->array_forwarding_cursor;
        if (event->block!=c->diagnostic->block || event->instruction!=c->diagnostic->instruction) continue;
        XrLirInstructionView drop={0};
        const XrLirHomeBinding *binding=c->homes->bindings ? &c->homes->bindings[receiver] : NULL;
        const XrtValueImage *image=xr_lir_value_image_view(c->literals->program->value_image);
        if (event->kind!=(uint32_t)(create ? XR_LIR_OWNERSHIP_BINDING_NEW : XR_LIR_OWNERSHIP_BINDING_READ) ||
            event->source!=receiver || event->target!=value || event->forwarding_mode!=XR_LIR_OWNERSHIP_TRANSFER_MOVE ||
            event->instruction==UINT32_MAX || event->forwarding_drop!=event->instruction+1 ||
            !binding || binding->kind!=XR_LIR_HOME_BINDING_LOCAL || !binding->type || !image ||
            binding->type>image->type_count || image->types[binding->type-1].kind!=XRT_VALUE_ARRAY_I64 ||
            xr_lir_instruction_view(c->input,(XrLirBlock){c->input,c->function.id,c->diagnostic->block},
                c->diagnostic->instruction,&drop)!=XR_LIR_OK ||
            drop.op!=(create ? XR_LIR_ARRAY_DROP : XR_LIR_ARRAY_CELL_DROP) || drop.result ||
            drop.operand_count!=1 || drop.operands[0]!=access->source || n>=UINT32_MAX)
            return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"Array forwarding lost its actual adjacent binding and release");
        access->index=(uint32_t)n+1;
        return true;
    }
    return true;
}
static bool rbc_lir_binding_metadata(RbcLirCompiler *c,XrRbcValueAccess *access) {
    bool binding=access->operation>=XR_RBC_VALUE_BINDING_NEW && access->operation<=XR_RBC_VALUE_BINDING_DROP;
    uint32_t receiver=access->operation==XR_RBC_VALUE_BINDING_NEW ? access->result : access->source;
    if ((binding && rbc_lir_fixed_binding(c,receiver)) ||
        access->operation==XR_RBC_VALUE_STRING_APPEND_BINDING) {
        if (binding) {
            XrLirOwnershipFactKind kind=access->operation==XR_RBC_VALUE_BINDING_NEW ? XR_LIR_OWNERSHIP_BINDING_NEW :
                access->operation==XR_RBC_VALUE_BINDING_READ ? XR_LIR_OWNERSHIP_BINDING_READ :
                access->operation==XR_RBC_VALUE_BINDING_WRITE ? XR_LIR_OWNERSHIP_BINDING_WRITE : XR_LIR_OWNERSHIP_BINDING_RELEASE;
            access->index=rbc_lir_binding_event(c,kind,receiver);
            if (!access->index) return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"binding lost its common access event");
        }
        if (access->operation!=XR_RBC_VALUE_BINDING_READ && access->operation!=XR_RBC_VALUE_BINDING_DROP) {
            access->window=UINT32_MAX;
            uint32_t input=access->operation==XR_RBC_VALUE_BINDING_NEW ? access->source : access->other;
            if (access->operation==XR_RBC_VALUE_STRING_APPEND_BINDING || c->homes->values[input].parameter) {
                while (c->binding_workspace_cursor<c->homes->workspace_count) {
                    size_t w=c->binding_workspace_cursor++;
                    const XrLirHomeWorkspace *workspace=&c->homes->workspaces[w];
                    if (workspace->kind!=XR_LIR_HOME_WORKSPACE_VALUE || !workspace->value ||
                        !rbc_lir_fixed_binding(c,workspace->value)) continue;
                    if (workspace->block!=c->diagnostic->block || workspace->instruction!=c->diagnostic->instruction ||
                        workspace->value!=receiver || workspace->type!=c->homes->bindings[receiver].type)
                        return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"binding copy lost its exact pending workspace");
                    if (w>=UINT32_MAX) return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"binding workspace ordinal exceeds typed code");
                    access->window=(uint32_t)w; break;
                }
            }
            if ((access->operation==XR_RBC_VALUE_STRING_APPEND_BINDING || c->homes->values[input].parameter) && access->window==UINT32_MAX)
                return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"binding copy lost its pending workspace");
        }
    }
    return rbc_lir_array_forwarding_metadata(c,access);
}
static bool rbc_lir_value_instruction(RbcLirCompiler *c, const XrLirInstructionView *i, bool *handled) {
    *handled = false;
    if (!c->typed_values) return true;
    if (!rbc_lir_cell_access(c,i,handled)) return false;
    if (*handled) return true;
    XrRbcValueAccess access = {.result=i->result,.source=i->operand_count ? i->operands[0] : 0,
        .other=i->operand_count > 1 ? i->operands[1] : 0};
    switch (i->op) {
    case XR_LIR_MAP_LITERAL_NEW:
        if (!i->bits || i->bits > c->literals->program->module.map_literal_count)
            return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"Map literal lost its owned provenance");
        access = (XrRbcValueAccess){.operation=XR_RBC_VALUE_MAP_LITERAL,.result=i->result,
            .index=(uint32_t)i->bits,.count=i->operand_count,.window=UINT32_MAX};
        if (!rbc_lir_value_arguments(c,i->operands,i->operand_count,&access.first)) return false;
        break;
    case XR_LIR_SLICE_FORMAL_CLEANUP:
        /* The common verifier ends only this independent formal loan. */
        *handled=true; return true;
    case XR_LIR_SLICE_CLEANUP:
        if (!i->bits || i->bits>UINT32_MAX || !c->homes || !c->homes->values || i->bits>c->homes->value_count)
            return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"Slice cleanup lost its resource target");
        access=(XrRbcValueAccess){.operation=XR_RBC_VALUE_SLICE,.index=XR_LIR_SLICE_OP_CLEANUP,
            .window=rbc_lir_callable_event(c,XR_LIR_OWNERSHIP_VIEW_END,(uint32_t)i->bits)};
        if (!access.window)
            return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"Slice cleanup lost its common keeper event");
        {
            const XrLirHomeEvent *event=&c->homes->events[access.window-1];
            const XrLirValueHome *home=&c->homes->values[(uint32_t)i->bits];
            if (event->view_operation!=XR_LIR_SLICE_OP_CLEANUP || event->target ||
                event->view_storage!=XR_LIR_SLICE_STORAGE_LOCAL_KEEPER ||
                event->view_workspace!=home->view_workspace ||
                event->transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE ||
                event->hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE ||
                (uint32_t)event->view_cleanup>XR_LIR_SLICE_CLEANUP_UNCONSTRUCTED)
                return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"Slice cleanup disagrees with its common keeper event");
            /* The verified common plan still ends the loan.
             * Its private keeper cannot exist on this path, so emit no access
             * instead of publishing a runtime flag that could skip a clear. */
            if (event->view_cleanup==XR_LIR_SLICE_CLEANUP_UNCONSTRUCTED) {
                *handled=true; return true;
            }
        }
        break;
    case XR_LIR_FUNCTION_REF: case XR_LIR_FUNCTION_WEAKEN: case XR_LIR_FUNCTION_BIND:
    case XR_LIR_FUNCTION_SCOPE_LEAVE: case XR_LIR_CALL_INDIRECT:
        if (i->op==XR_LIR_FUNCTION_BIND && !rbc_lir_callable_type(c->input,i->type)) return true;
        if (i->op==XR_LIR_FUNCTION_SCOPE_LEAVE && !rbc_lir_callable_event(c,XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT,0)) return true;
        if (!rbc_lir_function_access(c,i,&access)) return false;
        *handled=true;
        return rbc_lir_value_append(c,access) &&
            (i->op!=XR_LIR_FUNCTION_SCOPE_LEAVE || rbc_lir_function_releases(c));
    case XR_LIR_STRING_LITERAL:
        access.operation = XR_RBC_VALUE_STRING_LITERAL;
        if (!rbc_lir_literal(c,(uint32_t)i->bits,&access.index)) return false;
        break;
    case XR_LIR_STRING_I64: access.operation = XR_RBC_VALUE_STRING_I64; break;
    case XR_LIR_STRING_F64: access.operation = XR_RBC_VALUE_STRING_F64; break;
    case XR_LIR_STRING_CONCAT: access.operation = XR_RBC_VALUE_STRING_CONCAT; break;
    case XR_LIR_VALUE_RETAIN:
    case XR_LIR_MAP_RETAIN: case XR_LIR_ARRAY_RETAIN:
    case XR_LIR_STRING_RETAIN: access.operation = XR_RBC_VALUE_COPY; break;
    case XR_LIR_VALUE_DROP:
    case XR_LIR_MAP_DROP: case XR_LIR_ARRAY_DROP:
    case XR_LIR_STRING_DROP: access.operation = XR_RBC_VALUE_DROP; break;
    case XR_LIR_STRING_PRINT: access.operation = XR_RBC_VALUE_STRING_PRINT; break;
    case XR_LIR_STRING_LEN: access.operation = XR_RBC_VALUE_STRING_LENGTH; break;
    case XR_LIR_MAP_BORROW: case XR_LIR_ARRAY_BORROW: *handled=true; return true;
    case XR_LIR_MAP_CELL_DROP: case XR_LIR_ARRAY_CELL_DROP: case XR_LIR_STRING_CELL_DROP:
        access.operation=XR_RBC_VALUE_BINDING_DROP; break;
    case XR_LIR_MAP_REF_READ: case XR_LIR_ARRAY_REF_READ: access.operation=XR_RBC_VALUE_BINDING_READ; break;
    case XR_LIR_MAP_REF_WRITE: case XR_LIR_ARRAY_REF_WRITE: access.operation=XR_RBC_VALUE_BINDING_WRITE; break;
    case XR_LIR_CELL_NEW: case XR_LIR_CELL_READ: case XR_LIR_CELL_WRITE: {
        uint32_t binding=i->op==XR_LIR_CELL_NEW ? i->result : access.source;
        if (!c->homes->bindings || !c->homes->bindings[binding].type) return true;
        access.operation=i->op==XR_LIR_CELL_NEW ? XR_RBC_VALUE_BINDING_NEW :
            i->op==XR_LIR_CELL_READ ? XR_RBC_VALUE_BINDING_READ : XR_RBC_VALUE_BINDING_WRITE;
        break;
    }
    case XR_LIR_CALL_INTRINSIC: {
        uint32_t string_role=xr_lir_string_role(c->input,i);
        if (string_role && string_role!=XR_LIR_STRING_OP_APPEND_OWNED) {
            if (string_role==XR_LIR_STRING_OP_BORROW_CELL || string_role==XR_LIR_STRING_OP_BORROW_STATE) {
                *handled=true; return true;
            }
            if (!rbc_lir_string_binding_operation(c,string_role,&access)) return false;
            break;
        }
        if (string_role==XR_LIR_STRING_OP_APPEND_OWNED) {
            access.operation=XR_RBC_VALUE_STRING_APPEND_OWNED;
            access.index=rbc_lir_callable_event(c,XR_LIR_OWNERSHIP_APPEND_TRANSFER,access.source);
            if (!access.index) return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"String append lost its common transfer event");
            break;
        }
        XrLirSliceIntrinsic slice={0};
        if (xr_lir_slice_intrinsic_view(c->input,i->intrinsic,&slice)==XR_LIR_OK && slice.id) {
            if (!rbc_lir_slice_access(c,i,slice,&access)) return false;
            break;
        }
        if (xr_lir_display_point(c->input,i)) {
            if (!rbc_lir_value_display(c,i,&access)) return false;
            break;
        }
        XrLirArrayIntrinsic shape={0};
        uint32_t map=rbc_lir_map_role(c->input,i);
        if (!map && (xr_lir_array_intrinsic_view(c->input,i->intrinsic,&shape)!=XR_LIR_OK || !shape.id)) return true;
        access.operation=map ? XR_RBC_VALUE_MAP : XR_RBC_VALUE_ARRAY;
        access.index=map ? map : (uint32_t)shape.operation;
        access.source=access.other=0; access.count=i->operand_count; access.window=UINT32_MAX;
        if (!rbc_lir_value_arguments(c,i->operands,i->operand_count,&access.first)) return false;
        if (map ? map==XR_LIR_MAP_OP_SET || map==XR_LIR_MAP_OP_REMOVE || map==XR_LIR_MAP_OP_CLEAR :
            shape.operation==XR_LIR_ARRAY_OP_SET || shape.operation==XR_LIR_ARRAY_OP_PUSH) {
            for (size_t w=0;w<c->homes->workspace_count;++w) {
                const XrLirHomeWorkspace *workspace=&c->homes->workspaces[w];
                if (workspace->block==c->diagnostic->block && workspace->instruction==c->diagnostic->instruction) {
                    if (w>UINT32_MAX) return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"pending workspace index exceeds bytecode description");
                    access.window=(uint32_t)w; break;
                }
            }
            if (access.window==UINT32_MAX) return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"container mutator lost its common pending workspace");
            if (!map && !rbc_lir_array_local_metadata(c,&access,i->operands[0])) return false;
        }
        break;
    }
    case XR_LIR_RECORD_NEW:
        access.operation=XR_RBC_VALUE_RECORD_NEW; access.source=access.other=0; access.count=i->operand_count;
        if (!rbc_lir_value_arguments(c,i->operands,i->operand_count,&access.first)) return false;
        break;
    case XR_LIR_RECORD_FIELD: access.operation=XR_RBC_VALUE_RECORD_FIELD; access.index=(uint32_t)i->bits; break;
    case XR_LIR_NULLABLE_NONE: case XR_LIR_NULLABLE_SOME:
    case XR_LIR_NULLABLE_IS_SOME: case XR_LIR_NULLABLE_UNWRAP: {
        XrLirType type=(i->op==XR_LIR_NULLABLE_NONE || i->op==XR_LIR_NULLABLE_SOME) ? i->type : rbc_lir_value_type(c,access.source);
        if (!rbc_lir_inline_type(c->input,type)) return true;
        access.operation=i->op==XR_LIR_NULLABLE_NONE ? XR_RBC_VALUE_NULLABLE_NONE :
            i->op==XR_LIR_NULLABLE_SOME ? XR_RBC_VALUE_NULLABLE_SOME :
            i->op==XR_LIR_NULLABLE_IS_SOME ? XR_RBC_VALUE_NULLABLE_IS_SOME : XR_RBC_VALUE_NULLABLE_UNWRAP;
        break;
    }
    case XR_LIR_STATE_READ: case XR_LIR_STATE_WRITE: {
        if (rbc_lir_fixed_binding(c,access.source)) {
            access.operation=i->op==XR_LIR_STATE_READ ? XR_RBC_VALUE_BINDING_READ : XR_RBC_VALUE_BINDING_WRITE;
            break;
        }
        XrLirStateReference shape = {0};
        xr_lir_state_reference_view(c->input,(XrLirValue){c->input,c->function.id,access.source},&shape);
        if (shape.payload != XR_LIR_STRING && shape.payload != XR_LIR_ARRAY && shape.payload != XR_LIR_MAP_STRING_I64 &&
            !rbc_lir_inline_type(c->input,shape.payload) && !rbc_lir_callable_type(c->input,shape.payload)) return true;
        access.operation = i->op == XR_LIR_STATE_READ ? XR_RBC_VALUE_STATE_READ : XR_RBC_VALUE_STATE_WRITE;
        break;
    }
    case XR_LIR_STATE_CONSTRUCT: case XR_LIR_STATE_INITIALIZE:
        if (!rbc_lir_value_string_slot(c,i->bits)) return true;
        access.operation = i->op == XR_LIR_STATE_CONSTRUCT ? XR_RBC_VALUE_STATE_CONSTRUCT : XR_RBC_VALUE_STATE_INITIALIZE;
        access.index = (uint32_t)i->bits; break;
    case XR_LIR_CALL:
        if (!rbc_lir_value_call(c,i)) return true;
        access.operation = XR_RBC_VALUE_CALL; access.source = access.other = 0;
        if (rbc_lir_slice_type(c->input,i->type)) {
            if (c->value_call_cursor>UINT32_MAX)
                return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"Slice call ordinal exceeds typed code");
            access.other=(uint32_t)c->value_call_cursor;
        }
        access.index = c->functions[i->callee - 1] - 1; access.count = i->operand_count;
        access.window = c->register_count - rbc_lir_scalar_arguments(c->input,i) - 1;
        if (!rbc_lir_value_arguments(c,i->operands,i->operand_count,&access.first)) return false;
        break;
    case XR_LIR_RET:
        access.operation = XR_RBC_VALUE_RETURN;
        if (i->operand_count && (rbc_lir_callable_type(c->input,rbc_lir_value_type(c,i->operands[0])) ||
                rbc_lir_captured_cell(c->input,rbc_lir_value_type(c,i->operands[0])) ||
                rbc_lir_slice_type(c->input,rbc_lir_value_type(c,i->operands[0]))))
            access.index=rbc_lir_callable_event(c,XR_LIR_OWNERSHIP_RETURN_TRANSFER,i->operands[0]);
        break;
    default: return true;
    }
    *handled = true;
    return rbc_lir_binding_metadata(c,&access) && rbc_lir_value_append(c,access);
}
