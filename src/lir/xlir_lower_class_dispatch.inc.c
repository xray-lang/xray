/* Lower a verified method family to ordinary typed LIR calls. */
static bool lower_class_dispatch_return(Lower *c, XrLirBlock block, XrLirValue *arguments,
    XrLirType actual, uint32_t target, uint32_t count, XrLirType result, XrLirSource source) {
    XrLirFunction callee = c->functions[target];
    XrLirType expected = c->module->functions[callee.id - 1].parameter_types[0];
    XrLirValue receiver = arguments[0], returned = {0};
    if (actual != expected) {
        XrLirInstruction adopt = {.op=XR_LIR_CLASS_ADOPT,.type=expected,.operands=&receiver,
            .operand_count=1,.source=source};
        if (!lower_build(c, xr_lir_emit_value(c->module, block, &adopt, &arguments[0]))) return false;
    }
    XrLirInstruction call = {.op=XR_LIR_CALL,.type=result,.callee=callee,.operands=arguments,
        .operand_count=count + lower_hidden_state(c,target),.source=source};
    bool good = result == XR_LIR_UNIT ? lower_build(c, xr_lir_emit_effect(c->module, block, &call)) :
        lower_build(c, xr_lir_emit_value(c->module, block, &call, &returned));
    if (good && actual != expected) {
        XrLirInstruction drop = {.op=XR_LIR_VALUE_DROP,.type=XR_LIR_UNIT,.operands=&arguments[0],
            .operand_count=1,.source=source};
        good = lower_build(c, xr_lir_emit_effect(c->module, block, &drop));
    }
    arguments[0] = receiver;
    XrLirInstruction ret = {.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operand_count=returned.id ? 1u : 0u,
        .operands=&returned,.source=source};
    return good && lower_build(c, xr_lir_set_terminator(c->module, block, &ret));
}

static bool lower_class_dispatch(LowerFunction *f, const LowerClassCall *method,
    const XrXirInstruction *op, XrLirFunction *selected, uint32_t *hidden) {
    Lower *c = f->owner;
    const XrLirFunctionData *base = &c->module->functions[selected->id - 1];
    XrLirType receiver = lower_type(c, xr_xir_operand_type(f->source, method->receiver));
    if (!method->override_count && receiver == base->parameter_types[0]) return true;
    for (uint32_t i = 0; i < method->override_count; ++i)
        *hidden |= lower_hidden_state(c, method->overrides[i]);
    if (op->args[1] >= UINT32_MAX - *hidden) return lower_unsupported(c, "class dispatch parameter count");
    uint32_t parameters = op->args[1] + 1, count = parameters + *hidden;
    XrLirType *types = lower_alloc(c, count, sizeof(*types));
    XrLirValue *arguments = lower_alloc(c, count, sizeof(*arguments));
    bool good = types && arguments;
    XrLirType result = base->result;
    XrLirArrayShape array_result = {base->array_result};
    XrLirSource source = lower_source(c);
    XrLirBlock next = {0};
    if (good) {
        memcpy(types, base->parameter_types, parameters * sizeof(*types));
        types[0] = receiver;
        if (*hidden) types[parameters] = XR_LIR_STATE;
        char name[64];
        int length = snprintf(name, sizeof(name), "class_dispatch_%u_%u", c->function, c->instruction);
        XrLirFunctionSpec spec = {.name=name,.name_length=(size_t)length,.parameters=types,
            .parameter_count=count,.result=result,.kind=XR_LIR_FUNCTION_DEFINITION};
        *selected = (XrLirFunction){0};
        good = lower_build(c, xr_lir_declare_function(c->module, &spec, selected)) &&
            (result != XR_LIR_ARRAY || lower_build(c, xr_lir_set_array_result(c->module, *selected, &array_result))) &&
            lower_build(c, xr_lir_define_function(c->module, *selected)) &&
            lower_build(c, xr_lir_add_block(c->module, *selected, NULL, 0, &next)) &&
            lower_build(c, xr_lir_set_entry(c->module, *selected, next));
    }
    for (uint32_t p = 0; good && p < count; ++p)
        good = lower_build(c, xr_lir_function_parameter(c->module, *selected, p, &arguments[p]));
    for (uint32_t i = 0; good && i < method->override_count; ++i) {
        uint32_t target = method->overrides[i];
        XrLirType target_type = lower_type(c, c->source->functions[target].parameters[0]);
        XrLirBlock current = next, matched = {0};
        next = (XrLirBlock){0};
        XrLirValue condition = {0};
        XrLirInstruction test = {.op=XR_LIR_CLASS_IS,.type=XR_LIR_I1,.bits=target_type,
            .operands=arguments,.operand_count=1,.source=source};
        good = lower_build(c, xr_lir_add_block(c->module, *selected, NULL, 0, &matched)) &&
            lower_build(c, xr_lir_add_block(c->module, *selected, NULL, 0, &next)) &&
            lower_build(c, xr_lir_emit_value(c->module, current, &test, &condition));
        XrLirInstruction branch = {.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,
            .operand_count=1,.edge_count=2,.edges={{.target=matched},{.target=next}},.source=source};
        good = good && lower_build(c, xr_lir_set_terminator(c->module, current, &branch)) &&
            lower_class_dispatch_return(c, matched, arguments, receiver, target, parameters, result, source);
    }
    if (good)
        good = lower_class_dispatch_return(c, next, arguments, receiver, method->callee - 1, parameters, result, source);
    xr_free(arguments);
    xr_free(types);
    return good;
}
