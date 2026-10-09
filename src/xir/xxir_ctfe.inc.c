/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe.inc.c - Bounded compiler execution through the shared scalar step
 *
 * KEY CONCEPT:
 *   Admission inspects actual operations, not promises of effect absence.
 *   The frame and all variable work use the artifact's retained compiler ledger.
 */
static XrXirCtfeStatus ctfe_compile_status(XrXirStatus status) {
    if (status == XR_XIR_OK) return XR_XIR_CTFE_OK;
    if (status == XR_XIR_BUDGET) return XR_XIR_CTFE_BUDGET;
    if (status == XR_XIR_OUT_OF_MEMORY) return XR_XIR_CTFE_OUT_OF_MEMORY;
    return XR_XIR_CTFE_BAD_ARTIFACT;
}
static bool ctfe_scalar_type(XrXirType type) {
    return type == XR_XIR_BOOL || xr_xir_type_is_integer(type);
}
/* The count describes frame reads/writes in scalar_step; edge transfers charge
 * their actual predecessor scans and copies inside scalar_edge. */
static bool ctfe_opcode_work(XrXirOp op, uint64_t *work) {
    switch (op) {
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: *work = 1; return true;
    case XR_XIR_SCALAR_COPY: case XR_XIR_CONVERT_NUMBER: *work = 2; return true;
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
    case XR_XIR_DIV_INT: case XR_XIR_REM_INT: case XR_XIR_AND_INT:
    case XR_XIR_OR_INT: case XR_XIR_XOR_INT: case XR_XIR_SHL_INT:
    case XR_XIR_SHR_INT: case XR_XIR_EQ_INT: case XR_XIR_NE_INT:
    case XR_XIR_LT_INT: case XR_XIR_LE_INT: case XR_XIR_GT_INT:
    case XR_XIR_GE_INT: *work = 3; return true;
    case XR_XIR_JUMP: case XR_XIR_PHI: *work = 0; return true;
    case XR_XIR_BRANCH: case XR_XIR_RETURN: *work = 1; return true;
    default: return false;
    }
}
static XrXirCtfeStatus ctfe_admit(const XrXirArtifact *artifact, uint32_t selected) {
    const XrXirModule *module = &artifact->module;
    const XrXirCompileContext *context = &artifact->context;
    const XrXirFunction *body = &module->functions[selected];
    const XrXirFunctionIdentity *identity = &module->declarations->functions[selected];
    const XrXirFunctionLayout *layout = &artifact->layouts[selected];
    if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
    if (identity->cleanup_owner || identity->nominal_owner ||
        (module->generics && (module->generics[selected].parameter_count ||
        module->generics[selected].argument_count)) || !ctfe_scalar_type(body->result) ||
        layout->owned_count || layout->outgoing_count || layout->path_count)
        return XR_XIR_CTFE_NOT_LEAF;
    for (uint32_t m = 0; m < module->declarations->module_count; ++m) {
        if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
        if (module->declarations->modules[m].initializer == selected) return XR_XIR_CTFE_NOT_LEAF;
    }
    for (uint32_t p = 0; p < body->parameter_count; ++p) {
        if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
        if (!ctfe_scalar_type(body->parameters[p])) return XR_XIR_CTFE_NOT_LEAF;
    }
    for (uint32_t b = 0; b < body->block_count; ++b) {
        if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
        const XrXirBlock *block = &body->blocks[b];
        if (block->panic || block->frontier) return XR_XIR_CTFE_NOT_LEAF;
        const XrXirInstruction *last = &body->instructions[block->first + block->count - 1];
        uint32_t edges = last->op == XR_XIR_BRANCH ? 2 : last->op == XR_XIR_JUMP ? 1 : 0;
        for (uint32_t e = 0; e < edges; ++e) {
            if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
            if (last->targets[e] <= b) return XR_XIR_CTFE_NOT_LEAF;
        }
    }
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_CTFE_BUDGET;
        const XrXirInstruction *op = &body->instructions[i]; uint64_t work;
        if (!ctfe_opcode_work(op->op, &work)) return XR_XIR_CTFE_NOT_LEAF;
        bool terminal = op->op == XR_XIR_JUMP || op->op == XR_XIR_BRANCH || op->op == XR_XIR_RETURN;
        if ((!terminal && !ctfe_scalar_type(op->type)) || (terminal && op->type != XR_XIR_UNIT))
            return XR_XIR_CTFE_NOT_LEAF;
        if (op->op == XR_XIR_CONVERT_NUMBER &&
            (!xr_xir_type_is_integer(op->type) ||
            !xr_xir_type_is_integer(xr_xir_operand_type(body, op->args[0])))) return XR_XIR_CTFE_NOT_LEAF;
    }
    return XR_XIR_CTFE_OK;
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_leaf(const XrXirCtfeRequest *request,
    XrXirValue *output) {
    if (!request || !output || output->type || output->reserved || output->payload ||
        !request->artifact || (request->argument_count && !request->arguments) ||
        request->limits.steps > XR_XIR_CTFE_MAX_STEPS ||
        request->limits.frame_bytes > XR_XIR_CTFE_MAX_FRAME_BYTES ||
        request->limits.output_bytes > XR_XIR_CTFE_MAX_OUTPUT_BYTES) return XR_XIR_CTFE_BAD_ARGUMENT;
    const XrXirArtifact *artifact = request->artifact;
    XrXirCtfeStatus status = ctfe_compile_status(xr_xir_compile_artifact_verify(artifact, NULL));
    if (status != XR_XIR_CTFE_OK) return status;
    if (artifact->module.stage != XR_XIR_LOWERED || request->function >= artifact->module.function_count)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!xir_compile_work(&artifact->context, 1)) return XR_XIR_CTFE_BUDGET;
    const XrXirTarget *target = xr_xir_compile_artifact_target(artifact);
    if (!target || target->architecture != XR_XIR_ARCH_X86_64 ||
        target->abi_version != XR_XIR_VALUE_ABI_VERSION ||
        request->target.architecture != target->architecture ||
        request->target.abi_version != target->abi_version) return XR_XIR_CTFE_PROFILE;
    for (uint32_t i = 0; i < 32; ++i) {
        if (!xir_compile_work(&artifact->context, 1)) return XR_XIR_CTFE_BUDGET;
        if (request->checked_identity[i] != artifact->checked_identity[i]) return XR_XIR_CTFE_IDENTITY;
    }
    status = ctfe_admit(artifact, request->function);
    if (status != XR_XIR_CTFE_OK) return status;
    const XrXirFunction *body = &artifact->module.functions[request->function];
    const XrXirFunctionLayout *layout = &artifact->layouts[request->function];
    if (request->argument_count != body->parameter_count) return XR_XIR_CTFE_BAD_ARGUMENT;
    for (uint32_t p = 0; p < body->parameter_count; ++p) {
        if (!xir_compile_work(&artifact->context, 1)) return XR_XIR_CTFE_BUDGET;
        if (!xr_xir_value_argument(&request->arguments[p], NULL, body->parameters[p]))
            return XR_XIR_CTFE_BAD_ARGUMENT;
    }
    XrXirLayout storage = {0};
    status = ctfe_compile_status(xr_xir_compile_layout(&artifact->context, artifact->module.types,
        body->result, target, XR_XIR_LAYOUT_STORAGE, &storage));
    if (status != XR_XIR_CTFE_OK) return status;
    if (storage.size > request->limits.output_bytes) return XR_XIR_CTFE_OUTPUT_LIMIT;
    /* Target offsets come only from the verified layout. sizeof accounts for
     * physical interpreter bookkeeping, never target type or ABI selection. */
    size_t activation_bytes = sizeof(VmState) + layout->frame_bytes;
    if (activation_bytes > request->limits.frame_bytes) return XR_XIR_CTFE_FRAME_LIMIT;
    void *activation = NULL;
    status = ctfe_compile_status(xir_compile_resource_status(xr_compile_resources_calloc(
        artifact->context.resources, 1, activation_bytes, &activation)));
    if (status != XR_XIR_CTFE_OK) return status;
    VmState *state = activation; void *frame = state + 1;
    for (uint32_t p = 0; p < body->parameter_count; ++p) {
        if (!xir_compile_work(&artifact->context, 1)) { status = XR_XIR_CTFE_BUDGET; goto done; }
        xr_xir_scalar_store(frame, layout->offsets[p], request->arguments[p].payload);
    }
    {
        ScalarRun run = {&artifact->module, body, layout, frame, NULL, artifact->context.resources};
        uint64_t remaining = request->limits.steps;
        for (;;) {
            /* Every watchdog observation consumes work, including exhaustion. */
            if (!scalar_compile_work(&run, 1)) { status = XR_XIR_CTFE_BUDGET; break; }
            if (!remaining) { status = XR_XIR_CTFE_STEP_LIMIT; break; }
            --remaining;
            if (state->instruction >= body->instruction_count) { status = XR_XIR_CTFE_BAD_ARTIFACT; break; }
            uint64_t work = 0;
            if (!ctfe_opcode_work(body->instructions[state->instruction].op, &work)) {
                status = XR_XIR_CTFE_NOT_LEAF; break;
            }
            if (!scalar_compile_work(&run, work + 1)) { status = XR_XIR_CTFE_BUDGET; break; }
            XrXirAction action; XrXirRunStatus ran = scalar_step(&run, state, &action);
            if (ran != XR_XIR_RUN_OK) {
                status = ran == XR_XIR_RUN_STEP_LIMIT ? XR_XIR_CTFE_BUDGET :
                    ran == XR_XIR_RUN_DIVIDE_BY_ZERO ? XR_XIR_CTFE_DIVIDE_BY_ZERO :
                    ran == XR_XIR_RUN_NUMERIC_RANGE ? XR_XIR_CTFE_NUMERIC_RANGE : XR_XIR_CTFE_BAD_ARTIFACT;
                break;
            }
            if (action.kind == XR_XIR_ACTION_RETURN) {
                if (!scalar_compile_work(&run, sizeof(action.value))) { status = XR_XIR_CTFE_BUDGET; break; }
                *output = action.value; break;
            }
            if (action.kind != XR_XIR_ACTION_CONTINUE) { status = XR_XIR_CTFE_BAD_ARTIFACT; break; }
        }
    }
done:
    xr_compile_resources_free(activation);
    return status;
}
