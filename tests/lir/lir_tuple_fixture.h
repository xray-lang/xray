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
    XrLirValue value = ca_value(&c, block, XR_LIR_TUPLE_NEW, tuple, values, 2), field = {0};
    XrLirInstruction get = {.op=XR_LIR_TUPLE_FIELD, .type=XR_LIR_I64,
        .operands=&value, .operand_count=1, .bits=1};
    OK(xr_lir_emit_value(c.module, block, &get, &field));
    ca_return(&c, block, field);
    OK(xr_lir_module_finish(c.module, NULL));
    return c.module;
}
