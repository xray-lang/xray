/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void callable_owned_edge_binding(void) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    XrLirCallableType signature = {.result=XR_LIR_I64};
    XrLirType callable = 0;
    OK(xr_lir_callable_type(c.module, &signature, &callable));
    XrLirFunction target = {0}, root = {0};
    XrLirType capture_type = XR_LIR_I64;
    XrLirFunctionSpec spec = {.name="answer", .name_length=6,
        .parameters=&capture_type, .parameter_count=1, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &target));
    OK(xr_lir_set_closure_target(c.module, target, (XrLirClosureTarget){callable, 1, false}));
    OK(xr_lir_define_function(c.module, target));
    XrLirBlock block;
    OK(xr_lir_add_block(c.module, target, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, target, block));
    XrLirValue seed;
    OK(xr_lir_function_parameter(c.module, target, 0, &seed));
    ca_return(&c, block, seed);
    XrLirType condition_type = XR_LIR_I1;
    spec = (XrLirFunctionSpec){.name="root", .name_length=4,
        .parameters=&condition_type, .parameter_count=1, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &root));
    OK(xr_lir_define_function(c.module, root));
    OK(xr_lir_add_block(c.module, root, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, root, block));
    XrLirBlock receiving;
    OK(xr_lir_add_block(c.module, root, &callable, 1, &receiving));
    XrLirValue outer = callable_fixture_scope(&c, block, (XrLirValue){0});
    XrLirValue inner = callable_fixture_scope(&c, block, outer);
    seed = ca_constant(&c, block, 42);
    XrLirInstruction reference = {.op=XR_LIR_FUNCTION_REF, .type=callable,
        .callee=target, .bits=XR_LIR_CLOSURE_OWNED, .operands=&seed, .operand_count=1};
    XrLirValue value, frame, condition;
    OK(xr_lir_emit_value(c.module, block, &reference, &value));
    reference.bits = XR_LIR_CLOSURE_FRAME;
    OK(xr_lir_emit_value(c.module, block, &reference, &frame));
    OK(xr_lir_function_parameter(c.module, root, 0, &condition));
    XrLirInstruction branch = {.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT,
        .operands=&condition, .operand_count=1,
        .edge_count=2, .edges={{receiving, &value, 1}, {receiving, &frame, 1}}};
    OK(xr_lir_set_terminator(c.module, block, &branch));
    OK(xr_lir_block_parameter(c.module, receiving, 0, &value));
    XrLirValue operands[] = {outer, value};
    XrLirInstruction bind = {.op=XR_LIR_FUNCTION_BIND, .type=callable, .operands=operands, .operand_count=2};
    OK(xr_lir_emit_value(c.module, receiving, &bind, &value));
    (void)callable_fixture_leave(&c, receiving, inner, (XrLirValue){0}, XR_LIR_UNIT);
    XrLirValue result = ca_value(&c, receiving, XR_LIR_CALL_INDIRECT, XR_LIR_I64, &value, 1);
    (void)callable_fixture_leave(&c, receiving, outer, (XrLirValue){0}, XR_LIR_UNIT);
    ca_return(&c, receiving, result);
    XrLirFunction entry = {0};
    spec = (XrLirFunctionSpec){.name="entry", .name_length=5, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &entry));
    OK(xr_lir_define_function(c.module, entry));
    OK(xr_lir_add_block(c.module, entry, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, entry, block));
    XrLirValue argument = {0}, returned = {0};
    XrLirInstruction boolean = {.op=XR_LIR_CONST, .type=XR_LIR_I1, .bits=1};
    OK(xr_lir_emit_value(c.module, block, &boolean, &argument));
    XrLirInstruction invoke = {.op=XR_LIR_CALL, .type=XR_LIR_I64, .callee=root,
        .operands=&argument, .operand_count=1};
    OK(xr_lir_emit_value(c.module, block, &invoke, &returned));
    ca_return(&c, block, returned);
    OK(xr_lir_module_finish(c.module, NULL));
    callable_c_rejected(c.module, root, XR_LIR_C_BAD_ARGUMENT);
    callable_rbc_unavailable(c.module, entry);
    callable_c_rejected(c.module, entry, XR_LIR_C_UNSUPPORTED);
    xr_lir_module_destroy(c.module);
}

static void captured_cell_frame_extent(void) {
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        CArrayFixture c = {0};
        OK(xr_lir_module_create(NULL, &c.module));
        XrLirType cell = 0, callable = 0;
        OK(xr_lir_cell_type(c.module, (XrLirCellType){XR_LIR_I64, 0}, &cell));
        XrLirCallableType signature = {.result=XR_LIR_I64};
        OK(xr_lir_callable_type(c.module, &signature, &callable));
        XrLirFunction target = {0}, root = {0};
        XrLirFunctionSpec spec = {.name="read_cell", .name_length=9,
            .parameters=&cell, .parameter_count=1, .result=XR_LIR_I64};
        OK(xr_lir_declare_function(c.module, &spec, &target));
        OK(xr_lir_set_closure_target(c.module, target, (XrLirClosureTarget){callable, 1, false}));
        OK(xr_lir_define_function(c.module, target));
        XrLirBlock block;
        OK(xr_lir_add_block(c.module, target, NULL, 0, &block));
        OK(xr_lir_set_entry(c.module, target, block));
        XrLirValue captured;
        OK(xr_lir_function_parameter(c.module, target, 0, &captured));
        ca_return(&c, block, ca_value(&c, block, XR_LIR_CELL_READ, XR_LIR_I64, &captured, 1));
        spec = (XrLirFunctionSpec){.name="root", .name_length=4, .result=XR_LIR_I64};
        OK(xr_lir_declare_function(c.module, &spec, &root));
        OK(xr_lir_define_function(c.module, root));
        OK(xr_lir_add_block(c.module, root, NULL, 0, &block));
        OK(xr_lir_set_entry(c.module, root, block));
        XrLirValue outer = callable_fixture_scope(&c, block, (XrLirValue){0});
        XrLirValue inner = callable_fixture_scope(&c, block, outer);
        XrLirValue seed = ca_constant(&c, block, 40);
        XrLirValue binding = ca_value(&c, block, XR_LIR_CELL_NEW, cell, &seed, 1);
        XrLirInstruction reference = {.op=XR_LIR_FUNCTION_REF, .type=callable,
            .callee=target, .operands=&binding, .operand_count=1, .bits=XR_LIR_CLOSURE_OWNED};
        XrLirValue owned, frame, receiver;
        OK(xr_lir_emit_value(c.module, block, &reference, &owned));
        reference.bits = XR_LIR_CLOSURE_FRAME;
        OK(xr_lir_emit_value(c.module, block, &reference, &frame));
        if (mutation == 1) receiver = callable_fixture_leave(&c, block, inner, frame, callable);
        else {
            XrLirValue operands[] = {mutation ? inner : outer, frame};
            XrLirInstruction bind = {.op=XR_LIR_FUNCTION_BIND, .type=callable,
                .operands=operands, .operand_count=2};
            OK(xr_lir_emit_value(c.module, block, &bind, &receiver));
            if (!mutation) (void)callable_fixture_leave(&c, block, inner, (XrLirValue){0}, XR_LIR_UNIT);
        }
        XrLirValue result = ca_value(&c, block, XR_LIR_CALL_INDIRECT, XR_LIR_I64, &receiver, 1);
        if (mutation == 2) (void)callable_fixture_leave(&c, block, inner, (XrLirValue){0}, XR_LIR_UNIT);
        (void)callable_fixture_leave(&c, block, outer, (XrLirValue){0}, XR_LIR_UNIT);
        ca_return(&c, block, result);
        XrLirStatus status = xr_lir_module_finish(c.module, NULL);
        if (!mutation && status == XR_LIR_OK) {
            XrRbcProgram *program = NULL;
            XrRbcCompileStatus compiled = xr_rbc_compile(c.module, root, &program, NULL);
            fprintf(stderr, "Frame captures retired Cell: finish=%u RBC=%u\n", (unsigned)status, (unsigned)compiled);
            xr_rbc_program_destroy(program);
        }
        CHECK(status == (mutation == 2 ? XR_LIR_OK : XR_LIR_VERIFY_ERROR));
        if (mutation == 2) {
            callable_rbc_unavailable(c.module, root);
            callable_c_supported(c.module, root, true);
        }
        xr_lir_module_destroy(c.module);
    }
    puts("Frame BIND and result LEAVE cannot outlive their captured Cell owner");
}
