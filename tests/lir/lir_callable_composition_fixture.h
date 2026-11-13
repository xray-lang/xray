/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void callable_composition_fixture(XrLirModule **output, XrLirFunction *root) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    XrLirCallableParameter parameter = {.type=XR_LIR_I64};
    XrLirCallableType shape = {.parameters=&parameter, .parameter_count=1, .result=XR_LIR_I64,
        .flags=XR_LIR_CALLABLE_NO_SUSPEND};
    XrLirType strong = 0, weak = 0;
    OK(xr_lir_callable_type(c.module, &shape, &strong));
    shape.flags = 0;
    OK(xr_lir_callable_type(c.module, &shape, &weak));
    XrLirType scalar = XR_LIR_I64;
    XrLirFunction leaf = {0}, wrapper = {0};
    XrLirFunctionSpec spec = {.name="leaf", .name_length=4,
        .parameters=&scalar, .parameter_count=1, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &leaf));
    OK(xr_lir_set_closure_target(c.module, leaf, (XrLirClosureTarget){.callable=strong}));
    OK(xr_lir_define_function(c.module, leaf));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(c.module, leaf, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, leaf, block));
    XrLirValue value = {0};
    OK(xr_lir_function_parameter(c.module, leaf, 0, &value));
    ca_return(&c, block, ca_binary(&c, block, XR_LIR_ADD, value, ca_constant(&c, block, 2)));
    XrLirType parameters[] = {weak, XR_LIR_I64};
    spec = (XrLirFunctionSpec){.name="wrapper", .name_length=7,
        .parameters=parameters, .parameter_count=2, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &wrapper));
    OK(xr_lir_set_closure_target(c.module, wrapper, (XrLirClosureTarget){.callable=weak, .capture_count=1}));
    OK(xr_lir_define_function(c.module, wrapper));
    OK(xr_lir_add_block(c.module, wrapper, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, wrapper, block));
    XrLirValue arguments[2] = {{0}};
    OK(xr_lir_function_parameter(c.module, wrapper, 0, &arguments[0]));
    OK(xr_lir_function_parameter(c.module, wrapper, 1, &arguments[1]));
    ca_return(&c, block, ca_value(&c, block, XR_LIR_CALL_INDIRECT, XR_LIR_I64, arguments, 2));
    spec = (XrLirFunctionSpec){.name="root", .name_length=4, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, root));
    OK(xr_lir_define_function(c.module, *root));
    OK(xr_lir_add_block(c.module, *root, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, *root, block));
    XrLirInstruction reference = {.op=XR_LIR_FUNCTION_REF, .type=strong, .callee=leaf};
    XrLirValue child = {0}, parent = {0};
    OK(xr_lir_emit_value(c.module, block, &reference, &child));
    child = ca_value(&c, block, XR_LIR_FUNCTION_WEAKEN, weak, &child, 1);
    reference = (XrLirInstruction){.op=XR_LIR_FUNCTION_REF, .type=weak, .callee=wrapper,
        .operands=&child, .operand_count=1, .bits=XR_LIR_CLOSURE_OWNED};
    OK(xr_lir_emit_value(c.module, block, &reference, &parent));
    arguments[0] = parent;
    arguments[1] = ca_constant(&c, block, 40);
    ca_return(&c, block, ca_value(&c, block, XR_LIR_CALL_INDIRECT, XR_LIR_I64, arguments, 2));
    OK(xr_lir_module_finish(c.module, NULL));
    *output = c.module;
}

