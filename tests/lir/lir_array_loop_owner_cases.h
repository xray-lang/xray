/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_loop_owner_cases.h - Selected loop edges transfer one real Array owner
 */
static void array_loop_owners(void) {
    for (unsigned missing_drop = 0; missing_drop < 2; ++missing_drop) {
        XrLirModule *module = NULL;
        OK(xr_lir_array_module_create(NULL, &module));
        XrLirFunction function = {0};
        XrLirFunctionSpec spec = {.name = "loop", .name_length = 4, .result = XR_LIR_UNIT};
        OK(xr_lir_declare_function(module, &spec, &function));
        OK(xr_lir_define_function(module, function));
        XrLirBlock entry = {0}, loop = {0}, leave = {0};
        const XrLirType parameters[] = {XR_LIR_ARRAY, XR_LIR_I64};
        OK(xr_lir_add_block(module, function, NULL, 0, &entry));
        OK(xr_lir_add_block(module, function, parameters, 2, &loop));
        OK(xr_lir_add_block(module, function, parameters, 1, &leave));
        OK(xr_lir_set_entry(module, function, entry));
        XrLirValue initial[2] = {{0}}, carried[2] = {{0}}, final = {0};
        XrLirArrayShape shape = {XR_LIR_I64};
        OK(xr_lir_block_parameter(module, loop, 0, &carried[0]));
        OK(xr_lir_block_parameter(module, loop, 1, &carried[1]));
        OK(xr_lir_block_parameter(module, leave, 0, &final));
        OK(xr_lir_set_array_shape(module, carried[0], &shape));
        OK(xr_lir_set_array_shape(module, final, &shape));
        uint32_t constructor = 0;
        OK(xr_lir_array_operation(module, XR_LIR_I64, XR_LIR_ARRAY_OP_NEW, &constructor));
        XrLirInstruction instruction = {.op = XR_LIR_CALL_INTRINSIC, .type = XR_LIR_ARRAY,
            .intrinsic = constructor};
        OK(xr_lir_emit_value(module, entry, &instruction, &initial[0]));
        instruction = (XrLirInstruction){.op = XR_LIR_CONST, .type = XR_LIR_I64};
        OK(xr_lir_emit_value(module, entry, &instruction, &initial[1]));
        XrLirInstruction branch = {.op = XR_LIR_BR, .type = XR_LIR_UNIT, .edge_count = 1,
            .edges = {{loop, initial, 2}}};
        OK(xr_lir_set_terminator(module, entry, &branch));
        XrLirValue one = {0}, limit = {0}, next = {0}, condition = {0};
        instruction.bits = 1;
        OK(xr_lir_emit_value(module, loop, &instruction, &one));
        instruction.bits = 3;
        OK(xr_lir_emit_value(module, loop, &instruction, &limit));
        XrLirValue add[] = {carried[1], one};
        instruction = (XrLirInstruction){.op = XR_LIR_ADD, .type = XR_LIR_I64,
            .operands = add, .operand_count = 2};
        OK(xr_lir_emit_value(module, loop, &instruction, &next));
        XrLirValue compare[] = {next, limit};
        instruction = (XrLirInstruction){.op = XR_LIR_ICMP, .type = XR_LIR_I1, .predicate = XR_LIR_LT,
            .operands = compare, .operand_count = 2};
        OK(xr_lir_emit_value(module, loop, &instruction, &condition));
        XrLirValue back[] = {carried[0], next};
        branch = (XrLirInstruction){.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT,
            .operands = &condition, .operand_count = 1, .edge_count = 2,
            .edges = {{loop, back, 2}, {leave, carried, 1}}};
        OK(xr_lir_set_terminator(module, loop, &branch));
        if (!missing_drop) {
            instruction = (XrLirInstruction){.op = XR_LIR_ARRAY_DROP, .type = XR_LIR_UNIT,
                .operands = &final, .operand_count = 1};
            OK(xr_lir_emit_effect(module, leave, &instruction));
        }
        instruction = (XrLirInstruction){.op = XR_LIR_RET, .type = XR_LIR_UNIT};
        OK(xr_lir_set_terminator(module, leave, &instruction));
        XrLirDiagnostic diagnostic = {0};
        XrLirStatus status = xr_lir_module_finish(module, &diagnostic);
        CHECK(status == (missing_drop ? XR_LIR_VERIFY_ERROR : XR_LIR_OK));
        if (missing_drop) CHECK(strstr(diagnostic.message, "Managed owner requires release or result transfer"));
        xr_lir_module_destroy(module);
    }
}
