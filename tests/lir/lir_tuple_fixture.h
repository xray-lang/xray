/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static XrLirModule *tuple_fixture(void) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    XrLirType tuple = 0;
    OK(xr_lir_tuple_i64_type(c.module, 2, &tuple));
    XrLirFunction root = {0};
    XrLirFunctionSpec spec = {.name="tuple_root", .name_length=10, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &root));
    OK(xr_lir_define_function(c.module, root));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(c.module, root, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, root, block));
    XrLirValue values[] = {ca_constant(&c, block, -17), ca_constant(&c, block, 42)};
    XrLirValue value = ca_value(&c, block, XR_LIR_RECORD_NEW, tuple, values, 2), field = {0};
    XrLirInstruction get = {.op=XR_LIR_RECORD_FIELD, .type=XR_LIR_I64,
        .operands=&value, .operand_count=1, .bits=1};
    OK(xr_lir_emit_value(c.module, block, &get, &field));
    ca_return(&c, block, field);
    OK(xr_lir_module_finish(c.module, NULL));
    return c.module;
}

static void tuple_frame_c_fixture(XrLirModule **module, XrLirFunction *root) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    XrLirType tuple = 0, callable = 0;
    OK(xr_lir_tuple_i64_type(c.module, 2, &tuple));
    OK(xr_lir_callable_type(c.module, &(XrLirCallableType){.result=XR_LIR_I64}, &callable));
    XrLirFunction leaf = {0};
    XrLirFunctionSpec spec = {.name="capture", .name_length=7, .parameters=&tuple,
        .parameter_count=1, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &leaf));
    OK(xr_lir_define_function(c.module, leaf));
    OK(xr_lir_set_closure_target(c.module, leaf, (XrLirClosureTarget){.callable=callable, .capture_count=1}));
    XrLirBlock body = {0};
    OK(xr_lir_add_block(c.module, leaf, NULL, 0, &body));
    OK(xr_lir_set_entry(c.module, leaf, body));
    XrLirValue captured = {0}, field = {0};
    OK(xr_lir_function_parameter(c.module, leaf, 0, &captured));
    XrLirInstruction project = {.op=XR_LIR_RECORD_FIELD, .type=XR_LIR_I64,
        .operands=&captured, .operand_count=1, .bits=1};
    OK(xr_lir_emit_value(c.module, body, &project, &field));
    ca_return(&c, body, field);

    spec = (XrLirFunctionSpec){.name="root", .name_length=4, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, root));
    OK(xr_lir_define_function(c.module, *root));
    XrLirBlock entry = {0};
    OK(xr_lir_add_block(c.module, *root, NULL, 0, &entry));
    OK(xr_lir_set_entry(c.module, *root, entry));
    XrLirValue fields[] = {ca_constant(&c, entry, -17), ca_constant(&c, entry, 42)};
    XrLirValue value = ca_value(&c, entry, XR_LIR_RECORD_NEW, tuple, fields, 2), closure = {0};
    XrLirInstruction capture = {.op=XR_LIR_FUNCTION_REF, .type=callable, .callee=leaf,
        .operands=&value, .operand_count=1, .bits=XR_LIR_CLOSURE_FRAME};
    OK(xr_lir_emit_value(c.module, entry, &capture, &closure));
    ca_return(&c, entry, ca_value(&c, entry, XR_LIR_CALL_INDIRECT, XR_LIR_I64, &closure, 1));
    OK(xr_lir_module_finish(c.module, NULL));
    *module = c.module;
}
