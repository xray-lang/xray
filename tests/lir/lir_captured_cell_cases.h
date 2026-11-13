/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void captured_cell_frame_raw(const XrRbcProgram *program) {
    const XrRbcModule *original = xr_rbc_program_view(program);
    CHECK(original->function_count == 2);
    const XrRbcFunction *root = &original->functions[0];
    XrRbcAggregateAccess *accesses = xr_malloc(root->aggregate_access_count * sizeof(*accesses));
    CHECK(accesses);
    for (unsigned mutation = 0; mutation < 2; ++mutation) {
        memcpy(accesses, root->aggregate_accesses, root->aggregate_access_count * sizeof(*accesses));
        XrRbcFunction functions[2];
        memcpy(functions, original->functions, sizeof(functions));
        functions[0].aggregate_accesses = accesses;
        XrRbcAggregateAccess bind = {0};
        for (size_t n = 0; n < root->aggregate_access_count; ++n)
            if (accesses[n].operation == XR_RBC_FUNCTION_BIND) {
                bind = accesses[n];
                if (!mutation) accesses[n].destination = 1;
            }
        CHECK(bind.destination == 2);
        if (mutation)
            for (size_t n = 0; n < root->aggregate_access_count; ++n)
                if (accesses[n].operation == XR_RBC_FUNCTION_SCOPE_LEAVE && accesses[n].destination == 2) {
                    accesses[n].count = 1;
                    accesses[n].source = bind.result.index;
                    accesses[n].result = bind.result;
                    accesses[n].slot = bind.slot;
                }
        XrRbcModule module = *original;
        module.functions = functions;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0,
            xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
    }
    xr_free(accesses);
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
            XrRbcProgram *program = NULL;
            CHECK(xr_rbc_compile(c.module, root, &program, NULL) == XR_RBC_COMPILE_OK);
            captured_cell_frame_raw(program);
            XrRbcExecution *execution = NULL;
            CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
            xr_rbc_program_destroy(program);
            xr_lir_module_destroy(c.module);
            c.module = NULL;
            XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
            uint64_t bits = 0;
            CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK &&
                xr_rbc_outcome_bits(outcome, &bits) && bits == 40);
            xr_rbc_execution_free(&execution);
            xr_rbc_outcome_free(&outcome);
        }
        xr_lir_module_destroy(c.module);
    }
    puts("Frame BIND and result LEAVE cannot outlive their captured Cell owner");
}

static void captured_cell_scope_join(void) {
    XrLirModule *lir = NULL;
    XrLirFunction root;
    callable_factory_fixture(&lir, &root, false);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(lir, root, &program, NULL) == XR_RBC_COMPILE_OK);
    const XrRbcModule *original = xr_rbc_program_view(program);
    CHECK(original->function_count == 3);
    const XrXirTypes *old = xr_xir_compile_type_arena_types(original->closure_types);
    CHECK(old->count == 5);
    XrXirTypeNode nodes[6];
    memcpy(nodes, old->nodes, 5 * sizeof(*nodes));
    nodes[5] = (XrXirTypeNode){.kind=XR_XIR_TYPE_CELL, .element=XR_XIR_I64};
    XrXirTypes types = {.nodes=nodes, .count=6};
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_type_arena_new(&context, &types, &arena) == XR_XIR_VALUE_OK);
    xr_compile_resources_release(context.resources);
    uint32_t parents[] = {0, 1}, callables[5] = {0};
    XrRbcFunctionScopes scopes[3] = {{parents, 2}, {0}, {0}};
    XrRbcRegisterKind kinds[] = {XR_RBC_BITS, XR_RBC_CAPTURED_I64_CELL,
        XR_RBC_CAPTURED_I64_CELL, XR_RBC_BITS, XR_RBC_BITS};
    XrRbcAggregateAccess accesses[] = {
        {.operation=XR_RBC_FUNCTION_SCOPE_ENTER, .destination=1},
        {.operation=XR_RBC_CAPTURED_CELL_NEW, .result={1, false}, .source=3, .slot=261},
        {.operation=XR_RBC_FUNCTION_SCOPE_ENTER, .destination=2, .source=1},
        {.operation=XR_RBC_CAPTURED_CELL_NEW, .result={2, false}, .source=3, .slot=261},
        {.operation=XR_RBC_FUNCTION_SCOPE_LEAVE, .destination=2},
        {.operation=XR_RBC_CAPTURED_CELL_READ, .source=2, .result={4, false}, .slot=261},
        {.operation=XR_RBC_FUNCTION_SCOPE_LEAVE, .destination=1},
        {.operation=XR_RBC_FUNCTION_SCOPE_RETURN, .source=4, .count=1}
    };
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        XrRbcOperands instructions[] = {
            {.op=XR_RBC_LOADI, .a=3, .immediate=40},
            {.op=XR_RBC_AGGREGATE_ACCESS, .immediate=0},
            {.op=XR_RBC_AGGREGATE_ACCESS, .immediate=1},
            {.op=XR_RBC_AGGREGATE_ACCESS, .immediate=2},
            {.op=XR_RBC_MOV, .a=2, .b=1},
            {.op=XR_RBC_EQ, .a=0, .b=3},
            {.op=XR_RBC_JMP},
            {.op=XR_RBC_AGGREGATE_ACCESS, .immediate=3},
            {.op=XR_RBC_AGGREGATE_ACCESS, .immediate=4},
            {.op=XR_RBC_AGGREGATE_ACCESS, .immediate=5},
            {.op=XR_RBC_AGGREGATE_ACCESS, .immediate=6},
            {.op=XR_RBC_AGGREGATE_ACCESS, .immediate=7}
        };
        if (mutation == 1) {
            XrRbcOperands temporary = instructions[4];
            instructions[4] = instructions[7];
            instructions[7] = temporary;
        }
        if (mutation == 2) {
            XrRbcOperands temporary = instructions[8];
            instructions[8] = instructions[9];
            instructions[9] = temporary;
        }
        uint32_t code[12];
        for (uint32_t n = 0; n < 12; ++n) CHECK(xr_rbc_encode(&code[n], instructions[n]));
        CHECK(xr_rbc_jump(&code[6], 6, 8, 12));
        XrRbcFunction functions[3];
        memcpy(functions, original->functions, sizeof(functions));
        functions[0] = (XrRbcFunction){.code=code, .code_count=12,
            .register_count=5, .parameter_count=1, .register_kinds=kinds, .register_kind_count=5,
            .register_callables=callables, .register_callable_count=5,
            .aggregate_accesses=accesses, .aggregate_access_count=8};
        XrRbcModule module = *original;
        module.functions = functions;
        module.closure_types = arena;
        module.function_scopes = scopes;
        module.function_scope_count = 3;
        XrRbcExecution *execution = NULL;
        uint64_t argument = 0;
        XrRbcStatus status = xr_rbc_module_prepare(&module, 0, &argument, 1,
            xr_rbc_program_locations(program), &execution);
        if (status != (mutation == 2 ? XR_RBC_OK : XR_RBC_INVALID))
            fprintf(stderr, "captured Cell scope join mutation=%u status=%u\n", mutation, (unsigned)status);
        CHECK(status == (mutation == 2 ? XR_RBC_OK : XR_RBC_INVALID));
        CHECK((execution != NULL) == (mutation == 2));
        if (execution) {
            XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
            uint64_t result = 0;
            CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK &&
                xr_rbc_outcome_bits(outcome, &result) && result == 40);
            xr_rbc_outcome_free(&outcome);
        }
        xr_rbc_execution_free(&execution);
    }
    xr_xir_compile_type_arena_drop(arena);
    xr_rbc_program_destroy(program);
    xr_lir_module_destroy(lir);
    puts("captured Cell joins preserve the shortest lexical owner on both predecessor orders");
}
