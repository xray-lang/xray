/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_callable_fixture.h - Real higher-order calls through typed function values
 */
static void callable_fixture_capture(XrLirModule **output, XrLirFunction *root, unsigned capture) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    XrLirCallableParameter parameter = {.type=XR_LIR_I64};
    XrLirCallableType shape = {.parameters=&parameter, .parameter_count=1, .result=XR_LIR_I64};
    XrLirType callable = 0;
    OK(xr_lir_callable_type(c.module, &shape, &callable));
    XrLirFunction plus = {0}, twice = {0};
    XrLirType scalars[] = {XR_LIR_I64, XR_LIR_I64};
    if (capture == 2) scalars[0] = XR_LIR_I64_CELL;
    XrLirFunctionSpec spec = {.name="plus_one", .name_length=8,
        .parameters=scalars, .parameter_count=capture ? 2u : 1u, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &plus));
    OK(xr_lir_set_closure_target(c.module, plus,
        (XrLirClosureTarget){.callable=callable, .capture_count=capture ? 1u : 0u}));
    OK(xr_lir_define_function(c.module, plus));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(c.module, plus, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, plus, block));
    XrLirValue input = {0};
    OK(xr_lir_function_parameter(c.module, plus, capture ? 1u : 0u, &input));
    XrLirValue increment = {0};
    if (capture) {
        OK(xr_lir_function_parameter(c.module, plus, 0, &increment));
        if (capture == 2) increment = ca_value(&c, block, XR_LIR_CELL_READ, XR_LIR_I64, &increment, 1);
    }
    else increment = ca_constant(&c, block, 1);
    ca_return(&c, block, ca_binary(&c, block, XR_LIR_ADD, input, increment));
    XrLirType parameters[] = {callable, XR_LIR_I64};
    spec = (XrLirFunctionSpec){.name="twice", .name_length=5,
        .parameters=parameters, .parameter_count=2, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &twice));
    OK(xr_lir_define_function(c.module, twice));
    OK(xr_lir_add_block(c.module, twice, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, twice, block));
    XrLirValue callback = {0};
    OK(xr_lir_function_parameter(c.module, twice, 0, &callback));
    OK(xr_lir_function_parameter(c.module, twice, 1, &input));
    XrLirValue arguments[] = {callback, input};
    XrLirValue first = ca_value(&c, block, XR_LIR_CALL_INDIRECT, XR_LIR_I64, arguments, 2);
    arguments[1] = first;
    ca_return(&c, block, ca_value(&c, block, XR_LIR_CALL_INDIRECT, XR_LIR_I64, arguments, 2));
    spec = (XrLirFunctionSpec){.name="root", .name_length=4, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, root));
    OK(xr_lir_define_function(c.module, *root));
    OK(xr_lir_add_block(c.module, *root, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, *root, block));
    XrLirInstruction reference = {.op=XR_LIR_FUNCTION_REF, .type=callable, .callee=plus};
    XrLirValue captured = {0};
    if (capture) {
        captured = ca_constant(&c, block, 1);
        if (capture == 2) captured = ca_value(&c, block, XR_LIR_CELL_NEW, XR_LIR_I64_CELL, &captured, 1);
        reference.operands = &captured;
        reference.operand_count = 1;
    }
    OK(xr_lir_emit_value(c.module, block, &reference, &callback));
    XrLirValue actuals[] = {callback, ca_constant(&c, block, 40)};
    XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_I64, .callee=twice,
        .operands=actuals, .operand_count=2};
    XrLirValue result = {0};
    OK(xr_lir_emit_value(c.module, block, &call, &result));
    ca_return(&c, block, result);
    OK(xr_lir_module_finish(c.module, NULL));
    *output = c.module;
}

static void callable_fixture(XrLirModule **output, XrLirFunction *root) {
    callable_fixture_capture(output, root, false);
}
