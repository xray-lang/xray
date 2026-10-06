/* Complete enum properties are independent of the escaping variant set. */
typedef struct TaskErrorTypes {
    XrXirNominalDeclaration declarations[6]; XrXirNominalTable table;
    XrXirNominalField safe_fields[2], generic_field, bad_field, phantom_field;
    XrXirNominalVariant empty_variant, safe_variants[2], bad_variants[2], generic_variant;
    XrXirConstraint bound, caller_bound; XrXirGeneric generic;
    XrXirType generic_argument, scalar_argument, bad_argument, safe_projection[2], generic_projection,
        scalar_projection, bad_projection, phantom_projection;
    XrXirCallableParameter tuple_fields[2];
    XrXirTypeNode nodes[10]; XrXirTypes types; XrXirFunction function; XrXirModule module;
} TaskErrorTypes;
static void task_error_types(TaskErrorTypes *f) {
    *f = (TaskErrorTypes){0};
    f->safe_fields[0] = (XrXirNominalField){{"number", 6}, XR_XIR_I64, 0};
    f->safe_fields[1] = (XrXirNominalField){{"message", 7}, XR_XIR_STRING, 0};
    f->generic_field = (XrXirNominalField){{"value", 5}, (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, 0};
    f->bad_field = (XrXirNominalField){{"hidden", 6}, (XrXirType)258, 0};
    f->phantom_field = (XrXirNominalField){{"value", 5}, XR_XIR_I64, 0};
    f->empty_variant = (XrXirNominalVariant){{"Empty", 5}, 0, 0};
    f->safe_variants[0] = (XrXirNominalVariant){{"Number", 6}, 0, 1};
    f->safe_variants[1] = (XrXirNominalVariant){{"Message", 7}, 1, 1};
    f->bad_variants[0] = (XrXirNominalVariant){{"Empty", 5}, 0, 0};
    f->bad_variants[1] = (XrXirNominalVariant){{"Unsafe", 6}, 0, 1};
    f->generic_variant = (XrXirNominalVariant){{"Value", 5}, 0, 1};
    f->bound.markers = XR_XIR_CONSTRAINT_SENDABLE; f->caller_bound = f->bound;
    f->generic = (XrXirGeneric){.constraints = &f->caller_bound, .parameter_count = 1};
    f->declarations[0] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"EmptyError", 10},
        .kind = XR_XIR_NOMINAL_ENUM, .variants = &f->empty_variant, .variant_count = 1};
    f->declarations[1] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"ScalarError", 11},
        .kind = XR_XIR_NOMINAL_ENUM, .variants = f->safe_variants, .variant_count = 2,
        .fields = f->safe_fields, .field_count = 2};
    f->declarations[2] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"LocalClass", 10},
        .kind = XR_XIR_NOMINAL_CLASS, .flags = XR_XIR_NOMINAL_FINAL};
    f->declarations[3] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"UnsafeError", 11},
        .kind = XR_XIR_NOMINAL_ENUM, .variants = f->bad_variants, .variant_count = 2,
        .fields = &f->bad_field, .field_count = 1};
    f->declarations[4] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"GenericError", 12},
        .kind = XR_XIR_NOMINAL_ENUM, .variants = &f->generic_variant, .variant_count = 1,
        .fields = &f->generic_field, .field_count = 1, .constraints = &f->bound, .parameter_count = 1};
    f->declarations[5] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"PhantomError", 12},
        .kind = XR_XIR_NOMINAL_ENUM, .variants = &f->generic_variant, .variant_count = 1,
        .fields = &f->phantom_field, .field_count = 1, .constraints = &f->bound, .parameter_count = 1};
    f->table = (XrXirNominalTable){f->declarations, 6, NULL};
    f->safe_projection[0] = XR_XIR_I64; f->safe_projection[1] = XR_XIR_STRING;
    f->generic_argument = f->generic_projection = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f->scalar_argument = f->scalar_projection = f->phantom_projection = XR_XIR_I64;
    f->bad_argument = f->bad_projection = (XrXirType)258;
    for (uint32_t i = 0; i < 4; ++i)
        f->nodes[i] = (XrXirTypeNode){.kind = XR_XIR_TYPE_NOMINAL, .nominal = {.declaration = i}};
    f->nodes[1].nominal.fields = f->safe_projection; f->nodes[1].nominal.field_count = 2;
    f->nodes[3].nominal.fields = &f->bad_projection; f->nodes[3].nominal.field_count = 1;
    f->nodes[4] = (XrXirTypeNode){.kind = XR_XIR_TYPE_NOMINAL, .parameter_span = 1,
        .nominal = {4, &f->generic_argument, 1, &f->generic_projection, 1}};
    f->nodes[5] = (XrXirTypeNode){.kind = XR_XIR_TYPE_NOMINAL,
        .nominal = {4, &f->scalar_argument, 1, &f->scalar_projection, 1}};
    f->tuple_fields[0] = (XrXirCallableParameter){XR_XIR_UNIT, 0};
    f->tuple_fields[1] = (XrXirCallableParameter){(XrXirType)257, 0};
    f->nodes[6] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TUPLE, .parameters = f->tuple_fields, .parameter_count = 2};
    f->nodes[7] = (XrXirTypeNode){.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)262};
    f->nodes[8] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = (XrXirType)263};
    f->nodes[9] = (XrXirTypeNode){.kind = XR_XIR_TYPE_NOMINAL,
        .nominal = {5, &f->bad_argument, 1, &f->phantom_projection, 1}};
    f->types = (XrXirTypes){f->nodes, 10, &f->table, NULL};
    f->module = (XrXirModule){.functions = &f->function, .function_count = 1, .types = &f->types, .generics = &f->generic};
}
static XrXirStatus task_error_proof(const XrXirCompileContext *c) {
    TaskErrorTypes f; task_error_types(&f);
    XrXirProofContext proof = {&f.module, {XR_XIR_CONTEXT_FUNCTION, 0, 0}};
    XrXirStatus status = xr_xir_compile_types_structure_verify(c, &f.types);
    const uint32_t accepted[] = {256, 257, 260, 261, 264};
    for (uint32_t i = 0; status == XR_XIR_OK && i < sizeof(accepted) / sizeof(accepted[0]); ++i)
        status = xr_xir_compile_type_markers_prove(c, &proof, (XrXirType)accepted[i], XR_XIR_CONSTRAINT_SENDABLE);
    return status;
}
static void task_error_reject(void) {
    XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
    TaskErrorTypes f; task_error_types(&f);
    XrXirProofContext proof = {&f.module, {XR_XIR_CONTEXT_FUNCTION, 0, 0}};
    CHECK(xr_xir_compile_types_structure_verify(&c, &f.types) == XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)259, XR_XIR_CONSTRAINT_ERROR) == XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)259, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)265, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_TYPE);
    f.caller_bound.markers = XR_XIR_CONSTRAINT_ERROR;
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
        XR_XIR_CONSTRAINT_ERROR | XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)260, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_TYPE);
    f.caller_bound.markers |= XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)XR_XIR_TYPE_PARAMETER_BASE,
        XR_XIR_CONSTRAINT_ERROR | XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_OK);
    f.nodes[5].nominal.fields = NULL; f.nodes[5].nominal.field_count = 0;
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)261, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_TYPE);
    f.nodes[5].nominal.fields = &f.scalar_projection; f.nodes[5].nominal.field_count = 1;
    f.scalar_projection = XR_XIR_STRING;
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)261, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_TYPE);
    f.safe_variants[1].field_begin = 0;
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)257, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_STRUCTURE);
    owner_free(&c, baseline);
}
static XrXirStatus task_error_effects(const XrXirCompileContext *c) {
    TaskErrorTypes f; task_error_types(&f);
    /* Remove only the deliberately invalid phantom instance from this valid
     * synchronous module. The unsafe enum remains legal to construct/throw. */
    f.types.count = 4;
    XrXirInstruction unsafe_ops[2] = {{.op = XR_XIR_ENUM_NEW, .type = (XrXirType)259},
        {.op = XR_XIR_THROW, .args = {0}}};
    XrXirInstruction caller_ops[2] = {{.op = XR_XIR_CALL, .immediate = 0}, {.op = XR_XIR_RETURN}};
    XrXirInstruction catch_ops[4] = {{.op = XR_XIR_INVOKE, .targets = {1, 2}, .immediate = 0},
        {.op = XR_XIR_RETURN}, {.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR}, {.op = XR_XIR_RETURN}};
    XrXirInstruction safe_ops[3] = {{.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7},
        {.op = XR_XIR_ENUM_NEW, .type = (XrXirType)257, .args = {0, 1}}, {.op = XR_XIR_THROW, .args = {1}}};
    XrXirInstruction throw_parameter = {.op = XR_XIR_THROW};
    uint32_t operand = 0;
    XrXirBlock unsafe_block = {.count = 2}, safe_block = {.count = 3}, parameter_block = {.count = 1};
    XrXirBlock catch_blocks[3] = {{.count = 1}, {.first = 1, .count = 1}, {.first = 2, .count = 2}};
    XrXirType existential = XR_XIR_ERROR, symbolic = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirFunction functions[8] = {
        {.name = "unsafe", .name_length = 6, .blocks = &unsafe_block, .block_count = 1,
            .instructions = unsafe_ops, .instruction_count = 2},
        {.name = "caller", .name_length = 6, .blocks = &unsafe_block, .block_count = 1,
            .instructions = caller_ops, .instruction_count = 2},
        {.name = "caught", .name_length = 6, .blocks = catch_blocks, .block_count = 3,
            .instructions = catch_ops, .instruction_count = 4},
        {.name = "safe", .name_length = 4, .blocks = &safe_block, .block_count = 1,
            .instructions = safe_ops, .instruction_count = 3, .operands = &operand, .operand_count = 1},
        {.name = "unknown", .name_length = 7, .parameters = &existential, .parameter_count = 1,
            .blocks = &parameter_block, .block_count = 1, .instructions = &throw_parameter, .instruction_count = 1},
        {.name = "symbolic", .name_length = 8, .parameters = &symbolic, .parameter_count = 1,
            .blocks = &parameter_block, .block_count = 1, .instructions = &throw_parameter, .instruction_count = 1},
        {.name = "errorOnly", .name_length = 9, .parameters = &symbolic, .parameter_count = 1,
            .blocks = &parameter_block, .block_count = 1, .instructions = &throw_parameter, .instruction_count = 1}};
    XrXirConstraint complete = {.markers = XR_XIR_CONSTRAINT_ERROR | XR_XIR_CONSTRAINT_SENDABLE};
    XrXirConstraint error_only = {.markers = XR_XIR_CONSTRAINT_ERROR};
    XrXirGeneric generics[8] = {0};
    generics[5] = (XrXirGeneric){.constraints = &complete, .parameter_count = 1};
    generics[6] = (XrXirGeneric){.constraints = &error_only, .parameter_count = 1};
    XrXirInstruction initializer = {.op = XR_XIR_RETURN};
    functions[7] = (XrXirFunction){.name = "init", .name_length = 4, .blocks = &parameter_block,
        .block_count = 1, .instructions = &initializer, .instruction_count = 1};
    XrXirSourceModule source = {"m", 1, NULL, 0, 7};
    XrXirFunctionIdentity identities[8] = {0};
    XrXirDeclarations declarations = {.modules = &source, .module_count = 1, .functions = identities,
        .root_module = UINT32_MAX, .entry_function = UINT32_MAX};
    f.module = (XrXirModule){.stage = XR_XIR_BUILT, .functions = functions, .function_count = 8,
        .types = &f.types, .generics = generics, .declarations = &declarations, .linkage_kind = XR_XIR_LIBRARY};
    XrXirArtifact *checked = NULL; XrXirEffects *effects = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_check(c, &f.module, &checked, &diagnostic);
    if (status != XR_XIR_OK && fail_at == SIZE_MAX)
        fprintf(stderr, "error module status=%u function=%u instruction=%u\n", status, diagnostic.function, diagnostic.instruction);
    if (status == XR_XIR_OK) status = xr_xir_compile_effects_analyze(checked, &effects);
    if (status == XR_XIR_OK) {
        const XrXirStatus expected[] = {XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE, XR_XIR_OK,
            XR_XIR_OK, XR_XIR_BAD_TYPE, XR_XIR_OK, XR_XIR_BAD_TYPE};
        for (uint32_t i = 0; i < 7; ++i) CHECK(xr_xir_effects_task_errors(effects, i) == expected[i]);
        CHECK(xr_xir_effects_error(effects, 0, (XrXirType)259, 0));
        CHECK(!xr_xir_effects_error(effects, 0, (XrXirType)259, 1));
        CHECK(xr_xir_effects_function(effects, 2)->throws == XR_XIR_EFFECT_NONE);
    }
    xr_xir_compile_effects_free(effects); xr_xir_compile_artifact_free(checked); return status;
}
static void task_error_axes(void) {
    XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
    CHECK(task_error_proof(&c) == XR_XIR_OK && task_error_effects(&c) == XR_XIR_OK);
    XrCompileResourceStats cost = stats(&c); owner_free(&c, baseline);
    const uint64_t exact[] = {cost.allocated_bytes, cost.peak_bytes, cost.work};
    for (uint32_t axis = 0; axis < 3; ++axis) for (uint32_t minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = caps(); CHECK(exact[axis]);
        if (!axis) limits.allocated_bytes = exact[axis] - minus;
        else if (axis == 1) limits.live_bytes = exact[axis] - minus;
        else limits.work = exact[axis] - minus;
        c = owner_new(limits); baseline = stats(&c).live_bytes;
        XrXirStatus status = task_error_proof(&c);
        if (status == XR_XIR_OK) status = task_error_effects(&c);
        CHECK(status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(stats(&c).allocated_bytes <= limits.allocated_bytes &&
            stats(&c).peak_bytes <= limits.live_bytes && stats(&c).work <= limits.work);
        owner_free(&c, baseline);
    }
    printf("Task complete error proof same-ledger axes allocated/live/work=%llu/%llu/%llu exact/minus1 physical0\n",
        (unsigned long long)cost.allocated_bytes, (unsigned long long)cost.peak_bytes,
        (unsigned long long)cost.work);
}
