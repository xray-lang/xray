/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_record_fixture.h - Independent canonical snapshots across a real call
 */
static XrLirValue record_fixture_value(XrLirModule *m, XrLirBlock block,
    XrLirOp op, XrLirType type, const XrLirValue *args, uint32_t count, uint64_t bits, XrLirFunction callee) {
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op=op, .type=type, .operands=args, .operand_count=count, .bits=bits, .callee=callee};
    CHECK(xr_lir_emit_value(m, block, &instruction, &result) == XR_LIR_OK);
    return result;
}
static void record_fixture_drop(XrLirModule *m, XrLirBlock block, XrLirOp op, XrLirValue value) {
    XrLirInstruction instruction = {.op=op, .type=XR_LIR_UNIT, .operands=&value, .operand_count=1};
    CHECK(xr_lir_emit_effect(m, block, &instruction) == XR_LIR_OK);
}
static XrLirModule *record_fixture(XrLirFunction *root) {
    XrLirModule *m = NULL;
    CHECK(xr_lir_module_create(NULL, &m) == XR_LIR_OK);
    XrLirType optional, record;
    CHECK(xr_lir_nullable_type(m, XR_LIR_STRING, &optional) == XR_LIR_OK);
    XrLirRecordField fields[] = {{"name",4,optional}, {"score",5,XR_LIR_I64}};
    CHECK(xr_lir_record_type(m, fields, 2, &record) == XR_LIR_OK);
    XrLirFunction helper = {0};
    XrLirFunctionSpec spec = {.name="record_root", .name_length=11, .result=XR_LIR_I64};
    CHECK(xr_lir_declare_function(m, &spec, root) == XR_LIR_OK);
    spec = (XrLirFunctionSpec){.name="record_copy", .name_length=11, .parameters=&record,
        .parameter_count=1, .result=record};
    CHECK(xr_lir_declare_function(m, &spec, &helper) == XR_LIR_OK);
    CHECK(xr_lir_define_function(m, helper) == XR_LIR_OK);
    XrLirBlock body = {0};
    CHECK(xr_lir_add_block(m, helper, NULL, 0, &body) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(m, helper, body) == XR_LIR_OK);
    XrLirValue argument;
    CHECK(xr_lir_function_parameter(m, helper, 0, &argument) == XR_LIR_OK);
    XrLirValue copy = record_fixture_value(m, body, XR_LIR_VALUE_RETAIN, record, &argument, 1, 0, (XrLirFunction){0});
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&copy, .operand_count=1};
    CHECK(xr_lir_set_terminator(m, body, &ret) == XR_LIR_OK);
    CHECK(xr_lir_define_function(m, *root) == XR_LIR_OK);
    XrLirBlock block = {0};
    CHECK(xr_lir_add_block(m, *root, NULL, 0, &block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(m, *root, block) == XR_LIR_OK);
    XrLirLiteral literal = {0};
    CHECK(xr_lir_add_literal(m, "alice", 5, &literal) == XR_LIR_OK);
    XrLirValue text = record_fixture_value(m, block, XR_LIR_STRING_LITERAL, XR_LIR_STRING, NULL, 0, literal.id, (XrLirFunction){0});
    XrLirValue some = record_fixture_value(m, block, XR_LIR_NULLABLE_SOME, optional, &text, 1, 0, (XrLirFunction){0});
    record_fixture_drop(m, block, XR_LIR_STRING_DROP, text);
    XrLirValue args[] = {some, record_fixture_value(m, block, XR_LIR_CONST, XR_LIR_I64, NULL, 0, 95, (XrLirFunction){0})};
    XrLirValue initial = record_fixture_value(m, block, XR_LIR_RECORD_NEW, record, args, 2, 0, (XrLirFunction){0});
    record_fixture_drop(m, block, XR_LIR_VALUE_DROP, some);
    copy = record_fixture_value(m, block, XR_LIR_CALL, record, &initial, 1, 0, helper);
    record_fixture_drop(m, block, XR_LIR_VALUE_DROP, initial);
    XrLirValue score = record_fixture_value(m, block, XR_LIR_RECORD_FIELD, XR_LIR_I64, &copy, 1, 1, (XrLirFunction){0});
    some = record_fixture_value(m, block, XR_LIR_RECORD_FIELD, optional, &copy, 1, 0, (XrLirFunction){0});
    record_fixture_drop(m, block, XR_LIR_VALUE_DROP, copy);
    text = record_fixture_value(m, block, XR_LIR_NULLABLE_UNWRAP, XR_LIR_STRING, &some, 1, 0, (XrLirFunction){0});
    record_fixture_drop(m, block, XR_LIR_VALUE_DROP, some);
    XrLirValue length = record_fixture_value(m, block, XR_LIR_STRING_LEN, XR_LIR_I64, &text, 1, 0, (XrLirFunction){0});
    record_fixture_drop(m, block, XR_LIR_STRING_DROP, text);
    args[0] = length; args[1] = score;
    XrLirValue result = record_fixture_value(m, block, XR_LIR_ADD, XR_LIR_I64, args, 2, 0, (XrLirFunction){0});
    ret.operands = &result;
    CHECK(xr_lir_set_terminator(m, block, &ret) == XR_LIR_OK);
    XrLirDiagnostic diagnostic = {0};
    XrLirStatus status = xr_lir_module_finish(m, &diagnostic);
    if (status != XR_LIR_OK) fprintf(stderr, "record fixture f%u b%u i%u: %s\n",
        diagnostic.function, diagnostic.block, diagnostic.instruction, diagnostic.message);
    CHECK(status == XR_LIR_OK);
    return m;
}
