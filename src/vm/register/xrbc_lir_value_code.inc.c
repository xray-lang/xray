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
    xr_free(code);
}
static void rbc_lir_value_codes_free(XrRbcValueCode **codes, size_t count) {
    for (size_t f = 0; codes && f < count; ++f) rbc_lir_value_code_free(codes[f]);
    xr_free(codes);
}
static bool rbc_lir_value_call(const XrLirModule *module, const XrLirInstructionView *instruction) {
    if (instruction->op != XR_LIR_CALL) return false;
    XrLirFunctionView callee;
    if (xr_lir_function_view(module,(XrLirFunction){module,instruction->callee},&callee) != XR_LIR_OK ||
        callee.kind != XR_LIR_FUNCTION_DEFINITION) return false;
    if (callee.result == XR_LIR_STRING) return true;
    for (uint32_t p = 0; p < callee.parameter_count; ++p)
        if (callee.parameter_types[p] == XR_LIR_STRING) return true;
    return false;
}
static bool rbc_lir_value_string_slot(RbcLirCompiler *c, uint64_t slot) {
    const XrtModuleLayoutSpec *state = c->literals->program->module.state_spec;
    if (!state || slot >= state->slot_count) return false;
    uint32_t type = state->slots[slot].type;
    return type && type <= state->values->type_count && state->values->types[type - 1].kind == XRT_VALUE_STRING;
}
static bool rbc_lir_value_needed(RbcLirCompiler *c) {
    for (uint32_t v = 1; v <= c->view.value_count; ++v)
        if (rbc_lir_value_type(c,v) == XR_LIR_STRING) return true;
    if (c->view.result == XR_LIR_STRING) return true;
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
    if (!c->homes || c->homes->value_count != c->view.value_count)
        return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"typed function has no common receiving homes");
    size_t count = (size_t)c->view.value_count + 1;
    if (count > SIZE_MAX / sizeof(XrRbcValueLocation))
        return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"typed value locations exceed address space");
    XrRbcValueCode *code = xr_calloc(1,sizeof(*code));
    if (!code) return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot own typed bytecode description");
    c->value_code = code;
    code->homes = c->homes; code->value_count = c->view.value_count;
    if (c->homes->edge_count) {
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
        if (c->homes->values[v].owner_count) continue;
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
static bool rbc_lir_value_instruction(RbcLirCompiler *c, const XrLirInstructionView *i, bool *handled) {
    *handled = false;
    if (!c->typed_values) return true;
    XrRbcValueAccess access = {.result=i->result,.source=i->operand_count ? i->operands[0] : 0,
        .other=i->operand_count > 1 ? i->operands[1] : 0};
    switch (i->op) {
    case XR_LIR_STRING_LITERAL:
        access.operation = XR_RBC_VALUE_STRING_LITERAL;
        if (!rbc_lir_literal(c,(uint32_t)i->bits,&access.index)) return false;
        break;
    case XR_LIR_STRING_I64: access.operation = XR_RBC_VALUE_STRING_I64; break;
    case XR_LIR_STRING_F64: access.operation = XR_RBC_VALUE_STRING_F64; break;
    case XR_LIR_STRING_CONCAT: access.operation = XR_RBC_VALUE_STRING_CONCAT; break;
    case XR_LIR_STRING_RETAIN: access.operation = XR_RBC_VALUE_COPY; break;
    case XR_LIR_STRING_DROP: access.operation = XR_RBC_VALUE_DROP; break;
    case XR_LIR_STRING_PRINT: access.operation = XR_RBC_VALUE_STRING_PRINT; break;
    case XR_LIR_STRING_LEN: access.operation = XR_RBC_VALUE_STRING_LENGTH; break;
    case XR_LIR_STATE_READ: case XR_LIR_STATE_WRITE: {
        XrLirStateReference shape = {0};
        xr_lir_state_reference_view(c->input,(XrLirValue){c->input,c->function.id,access.source},&shape);
        if (shape.payload != XR_LIR_STRING) return true;
        access.operation = i->op == XR_LIR_STATE_READ ? XR_RBC_VALUE_STATE_READ : XR_RBC_VALUE_STATE_WRITE;
        break;
    }
    case XR_LIR_STATE_CONSTRUCT: case XR_LIR_STATE_INITIALIZE:
        if (!rbc_lir_value_string_slot(c,i->bits)) return true;
        access.operation = i->op == XR_LIR_STATE_CONSTRUCT ? XR_RBC_VALUE_STATE_CONSTRUCT : XR_RBC_VALUE_STATE_INITIALIZE;
        access.index = (uint32_t)i->bits; break;
    case XR_LIR_CALL:
        if (!rbc_lir_value_call(c->input,i)) return true;
        access.operation = XR_RBC_VALUE_CALL; access.source = access.other = 0;
        access.index = c->functions[i->callee - 1] - 1; access.count = i->operand_count;
        access.window = c->register_count - rbc_lir_scalar_arguments(c->input,i) - 1;
        if (!rbc_lir_value_arguments(c,i->operands,i->operand_count,&access.first)) return false;
        break;
    case XR_LIR_RET: access.operation = XR_RBC_VALUE_RETURN; break;
    case XR_LIR_CALL_INDIRECT:
        return rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"mixed typed indirect calls have no fixed invocation plan");
    default: return true;
    }
    *handled = true;
    return rbc_lir_value_append(c,access);
}
