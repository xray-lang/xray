/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static XrLirModule *edge_spill_fixture(XrLirFunction *root) {
    CArrayFixture fixture = {.element=XR_LIR_I64};
    OK(xr_lir_array_module_create(NULL, &fixture.module));
    XrLirModule *m = fixture.module;
    XrLirType type = XR_LIR_I64, carried[] = {XR_LIR_ARRAY, XR_LIR_ARRAY, XR_LIR_STRING, XR_LIR_STRING, XR_LIR_I64};
    *root = test_function(m, "typed_edge_spill_cycle", &type, 1, type);
    XrLirBlock entry = test_block(m, *root, NULL, 0), loop = test_block(m, *root, carried, 5);
    XrLirBlock body = test_block(m, *root, carried, 5), done = test_block(m, *root, carried, 5);
    OK(xr_lir_set_entry(m, *root, entry));
    XrLirValue limit = {0}, parameters[5] = {{0}}, body_values[5] = {{0}}, outputs[5] = {{0}}, initial[5] = {{0}};
    OK(xr_lir_function_parameter(m, *root, 0, &limit));
    for (uint32_t p = 0; p < 5; ++p) {
        OK(xr_lir_block_parameter(m, loop, p, &parameters[p]));
        OK(xr_lir_block_parameter(m, body, p, &body_values[p]));
        OK(xr_lir_block_parameter(m, done, p, &outputs[p]));
        if (p < 2) {
            OK(xr_lir_set_array_shape(m, parameters[p], &(XrLirArrayShape){XR_LIR_I64}));
            OK(xr_lir_set_array_shape(m, body_values[p], &(XrLirArrayShape){XR_LIR_I64}));
            OK(xr_lir_set_array_shape(m, outputs[p], &(XrLirArrayShape){XR_LIR_I64}));
        }
    }
    for (uint32_t n = 0; n < 302; ++n) {
        XrLirLiteral literal = {0}; XrLirValue text = {0};
        OK(xr_lir_add_literal(m, n == 301 ? "LONG" : "FIRST", n == 301 ? 4 : 5, &literal));
        XrLirInstruction op = {.op=XR_LIR_STRING_LITERAL, .type=XR_LIR_STRING, .bits=literal.id};
        OK(xr_lir_emit_value(m, entry, &op, &text));
        if (n < 300) {
            op = (XrLirInstruction){.op=XR_LIR_STRING_DROP, .type=XR_LIR_UNIT, .operands=&text, .operand_count=1};
            OK(xr_lir_emit_effect(m, entry, &op));
        } else initial[n - 298] = text;
    }
    for (uint32_t p = 0; p < 2; ++p) {
        XrLirValue value = ca_constant(&fixture, entry, p ? 11 : 7);
        initial[p] = ca_intrinsic(&fixture, entry, XR_LIR_ARRAY_OP_NEW, XR_LIR_ARRAY, &value, 1);
    }
    initial[4] = test_constant(m, entry, type, 0);
    XrLirValue one = test_constant(m, entry, type, 1);
    test_branch(m, entry, loop, initial, 5);
    XrLirValue condition = test_binary(m, loop, XR_LIR_ICMP, parameters[4], limit, XR_LIR_LT);
    XrLirInstruction branch = {.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT, .operands=&condition, .operand_count=1,
        .edges={{body, parameters, 5}, {done, parameters, 5}}, .edge_count=2};
    OK(xr_lir_set_terminator(m, loop, &branch));
    XrLirValue next = test_binary(m, body, XR_LIR_ADD, body_values[4], one, XR_LIR_PREDICATE_NONE);
    XrLirValue swapped[] = {body_values[1], body_values[0], body_values[3], body_values[2], next};
    test_branch(m, body, loop, swapped, 5);
    XrLirValue read[] = {outputs[0], ca_constant(&fixture, done, 0)};
    XrLirValue number = ca_intrinsic(&fixture, done, XR_LIR_ARRAY_OP_GET, XR_LIR_I64, read, 2), length = {0};
    XrLirInstruction op = {.op=XR_LIR_STRING_LEN, .type=XR_LIR_I64, .operands=&outputs[2], .operand_count=1};
    OK(xr_lir_emit_value(m, done, &op, &length));
    for (uint32_t p = 0; p < 4; ++p) {
        op = (XrLirInstruction){.op=p < 2 ? XR_LIR_ARRAY_DROP : XR_LIR_STRING_DROP,
            .type=XR_LIR_UNIT, .operands=&outputs[p], .operand_count=1};
        OK(xr_lir_emit_effect(m, done, &op));
    }
    XrLirValue hundred = test_constant(m, done, type, 100);
    XrLirValue scaled = test_binary(m, done, XR_LIR_MUL, number, hundred, XR_LIR_PREDICATE_NONE);
    test_return(m, done, test_binary(m, done, XR_LIR_ADD, scaled, length, XR_LIR_PREDICATE_NONE));
    XrLirDiagnostic diagnostic = {0};
    XrLirStatus status = xr_lir_module_finish(m, &diagnostic);
    if (status != XR_LIR_OK) fprintf(stderr, "edge fixture %u f%u b%u i%u %s\n",
        status, diagnostic.function, diagnostic.block, diagnostic.instruction, diagnostic.message);
    OK(status);
    return m;
}

static void typed_edge_spill_cycles(void) {
    XrLirFunction root = {0};
    XrLirModule *module = edge_spill_fixture(&root);
    XrRbcProgram *program = NULL;
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    const XrRbcFunction *function = &program->module.functions[0];
    CHECK(function->reference_spill_count && function->spill_count > function->reference_spill_count);
    bool string = false, array = false;
    for (uint32_t n = 0; n < function->reference_spill_count; ++n) {
        string |= function->reference_spill_kinds[n] == XR_RBC_STRING;
        array |= function->reference_spill_kinds[n] == XR_RBC_ARRAY_I64;
    }
    CHECK(string && array);
    for (uint64_t n = 0; n < 4; ++n) {
        uint64_t bits = 0;
        CHECK(execute(program, &n, 1, &bits) == XR_RBC_OK && bits == (n % 2 ? 1104u : 705u));
    }
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    for (size_t point = 1; point <= total; ++point) {
        attempts = 0; fail_at = point; program = NULL;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !live_allocations);
    }
    fail_at = SIZE_MAX;
    xr_lir_module_destroy(module);
    printf("Typed edge String/Array snapshots, loop swaps and %zu compiler failures PASS\n", total);
}
