/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static XrLirValue callable_fixture_scope(CArrayFixture *c, XrLirBlock block, XrLirValue parent) {
    return ca_value(c, block, XR_LIR_FUNCTION_SCOPE_ENTER, XR_LIR_FUNCTION_SCOPE,
        parent.id ? &parent : NULL, parent.id ? 1u : 0u);
}
static XrLirValue callable_fixture_leave(CArrayFixture *c, XrLirBlock block,
    XrLirValue scope, XrLirValue value, XrLirType type) {
    XrLirValue operands[] = {scope, value}, result = {0};
    XrLirInstruction instruction = {.op=XR_LIR_FUNCTION_SCOPE_LEAVE, .type=type,
        .operands=operands, .operand_count=value.id ? 2u : 1u};
    if (value.id) OK(xr_lir_emit_value(c->module, block, &instruction, &result));
    else OK(xr_lir_emit_effect(c->module, block, &instruction));
    return result;
}
static void callable_factory_fixture_mode(XrLirModule **output, XrLirFunction *root, bool frame, bool scopes) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    XrLirCallableParameter parameter = {.type=XR_LIR_I64};
    XrLirCallableType signature = {.parameters=&parameter, .parameter_count=1, .result=XR_LIR_I64};
    XrLirType callable = 0;
    OK(xr_lir_callable_type(c.module, &signature, &callable));
    XrLirType integers[] = {XR_LIR_I64, XR_LIR_I64};
    XrLirFunction target = {0}, factory = {0};
    XrLirFunctionSpec spec = {.name="captured", .name_length=8, .parameters=integers,
        .parameter_count=2, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &target));
    OK(xr_lir_set_closure_target(c.module, target, (XrLirClosureTarget){callable, 1, false}));
    OK(xr_lir_define_function(c.module, target));
    XrLirBlock block;
    OK(xr_lir_add_block(c.module, target, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, target, block));
    XrLirValue seed, input;
    OK(xr_lir_function_parameter(c.module, target, 0, &seed));
    OK(xr_lir_function_parameter(c.module, target, 1, &input));
    ca_return(&c, block, ca_binary(&c, block, XR_LIR_ADD, seed, input));
    spec = (XrLirFunctionSpec){.name="factory", .name_length=7, .parameters=integers,
        .parameter_count=1, .result=callable};
    OK(xr_lir_declare_function(c.module, &spec, &factory));
    OK(xr_lir_define_function(c.module, factory));
    OK(xr_lir_add_block(c.module, factory, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, factory, block));
    OK(xr_lir_function_parameter(c.module, factory, 0, &seed));
    XrLirValue owner = {0}, temporary = {0};
    if (scopes) {
        owner = callable_fixture_scope(&c, block, (XrLirValue){0});
        temporary = callable_fixture_scope(&c, block, owner);
    }
    XrLirInstruction reference = {.op=XR_LIR_FUNCTION_REF, .type=callable, .callee=target,
        .operands=&seed, .operand_count=1, .bits=frame ? XR_LIR_CLOSURE_FRAME : XR_LIR_CLOSURE_OWNED};
    XrLirValue value;
    OK(xr_lir_emit_value(c.module, block, &reference, &value));
    if (scopes) {
        value = callable_fixture_leave(&c, block, temporary, value, callable);
        value = callable_fixture_leave(&c, block, owner, value, callable);
    }
    ca_return(&c, block, value);
    spec = (XrLirFunctionSpec){.name="root", .name_length=4, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, root));
    OK(xr_lir_define_function(c.module, *root));
    OK(xr_lir_add_block(c.module, *root, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, *root, block));
    if (scopes) {
        owner = callable_fixture_scope(&c, block, (XrLirValue){0});
        temporary = callable_fixture_scope(&c, block, owner);
    }
    XrLirValue first, second;
    seed = ca_constant(&c, block, 40);
    XrLirInstruction call = {.op=XR_LIR_CALL, .type=callable, .callee=factory,
        .operands=&seed, .operand_count=1};
    OK(xr_lir_emit_value(c.module, block, &call, &first));
    if (scopes) {
        XrLirValue operands[] = {owner, first};
        XrLirInstruction bind = {.op=XR_LIR_FUNCTION_BIND, .type=callable,
            .operands=operands, .operand_count=2};
        OK(xr_lir_emit_value(c.module, block, &bind, &first));
        (void)callable_fixture_leave(&c, block, temporary, (XrLirValue){0}, XR_LIR_UNIT);
        temporary = callable_fixture_scope(&c, block, owner);
    }
    seed = ca_constant(&c, block, 50);
    OK(xr_lir_emit_value(c.module, block, &call, &second));
    if (scopes) {
        XrLirValue operands[] = {owner, second};
        XrLirInstruction bind = {.op=XR_LIR_FUNCTION_BIND, .type=callable,
            .operands=operands, .operand_count=2, .bits=1};
        OK(xr_lir_emit_value(c.module, block, &bind, &second));
        (void)callable_fixture_leave(&c, block, temporary, (XrLirValue){0}, XR_LIR_UNIT);
    }
    XrLirValue arguments[] = {first, ca_constant(&c, block, 2)};
    XrLirValue a = ca_value(&c, block, XR_LIR_CALL_INDIRECT, XR_LIR_I64, arguments, 2);
    arguments[0] = second;
    XrLirValue b = ca_value(&c, block, XR_LIR_CALL_INDIRECT, XR_LIR_I64, arguments, 2);
    arguments[0] = first;
    arguments[1] = ca_constant(&c, block, 3);
    XrLirValue last = ca_value(&c, block, XR_LIR_CALL_INDIRECT, XR_LIR_I64, arguments, 2);
    XrLirValue total = ca_binary(&c, block, XR_LIR_ADD, ca_binary(&c, block, XR_LIR_ADD, a, b), last);
    if (scopes) (void)callable_fixture_leave(&c, block, owner, (XrLirValue){0}, XR_LIR_UNIT);
    ca_return(&c, block, total);
    OK(xr_lir_module_finish(c.module, NULL));
    *output = c.module;
}
static void callable_factory_fixture(XrLirModule **output, XrLirFunction *root, bool frame) {
    callable_factory_fixture_mode(output, root, frame, false);
}
