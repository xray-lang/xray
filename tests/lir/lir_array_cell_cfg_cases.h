/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_cell_cfg_cases.h - Physical Array Cell instances survive selected CFG edges
 */
static void array_cell_cfg(void) {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        XrLirModule *module = NULL;
        OK(xr_lir_array_module_create(NULL, &module));
        XrLirFunction function = {0};
        XrLirFunctionSpec spec = {.name = "cell_cfg", .name_length = 8, .result = XR_LIR_UNIT};
        OK(xr_lir_declare_function(module, &spec, &function));
        OK(xr_lir_define_function(module, function));
        XrLirBlock entry = {0}, body = {0}, left = {0}, right = {0}, join = {0};
        OK(xr_lir_add_block(module, function, NULL, 0, &entry));
        OK(xr_lir_add_block(module, function, NULL, 0, &body));
        OK(xr_lir_add_block(module, function, NULL, 0, &left));
        OK(xr_lir_add_block(module, function, NULL, 0, &right));
        OK(xr_lir_add_block(module, function, NULL, 0, &join));
        OK(xr_lir_set_entry(module, function, entry));
        bool outer = scenario == 1;
        bool loop = scenario == 1 || scenario == 2 || scenario == 4;
        uint32_t constructor = 0;
        OK(xr_lir_array_operation(module, XR_LIR_I64, XR_LIR_ARRAY_OP_NEW, &constructor));
        XrLirValue array = {0}, cell = {0}, condition = {0};
        XrLirInstruction make = {.op = XR_LIR_CALL_INTRINSIC, .type = XR_LIR_ARRAY,
            .intrinsic = constructor};
        XrLirBlock definition = outer ? entry : body;
        OK(xr_lir_emit_value(module, definition, &make, &array));
        make = (XrLirInstruction){.op = XR_LIR_CELL_NEW, .type = XR_LIR_ARRAY_CELL,
            .operands = &array, .operand_count = 1};
        OK(xr_lir_emit_value(module, definition, &make, &cell));
        XrLirInstruction drop = {.op = XR_LIR_ARRAY_DROP, .type = XR_LIR_UNIT,
            .operands = &array, .operand_count = 1};
        OK(xr_lir_emit_effect(module, definition, &drop));
        make = (XrLirInstruction){.op = XR_LIR_CONST, .type = XR_LIR_I1, .bits = 1};
        OK(xr_lir_emit_value(module, body, &make, &condition));
        XrLirInstruction branch = {.op = XR_LIR_BR, .type = XR_LIR_UNIT, .edge_count = 1,
            .edges = {{body, NULL, 0}}};
        OK(xr_lir_set_terminator(module, entry, &branch));
        XrLirInstruction choice = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT,
            .operands = &condition, .operand_count = 1, .edge_count = 2,
            .edges = {{left, NULL, 0}, {right, NULL, 0}}};
        OK(xr_lir_set_terminator(module, body, &choice));
        for (unsigned side = 0; side < 2; ++side) {
            XrLirBlock block = side ? right : left;
            XrLirValue read = {0};
            XrLirInstruction use = {.op = XR_LIR_CELL_READ, .type = XR_LIR_ARRAY,
                .operands = &cell, .operand_count = 1};
            OK(xr_lir_emit_value(module, block, &use, &read));
            drop.operands = &read;
            OK(xr_lir_emit_effect(module, block, &drop));
            bool release = side ? scenario != 3 && scenario != 5 : !outer && scenario != 4;
            if (release) {
                XrLirInstruction retire = {.op = XR_LIR_ARRAY_CELL_DROP, .type = XR_LIR_UNIT,
                    .operands = &cell, .operand_count = 1};
                OK(xr_lir_emit_effect(module, block, &retire));
            }
            branch.edges[0].target = !side && loop ? body : join;
            OK(xr_lir_set_terminator(module, block, &branch));
        }
        if (scenario == 5) {
            XrLirValue read = {0};
            make = (XrLirInstruction){.op = XR_LIR_CELL_READ, .type = XR_LIR_ARRAY,
                .operands = &cell, .operand_count = 1};
            OK(xr_lir_emit_value(module, join, &make, &read));
            drop.operands = &read;
            OK(xr_lir_emit_effect(module, join, &drop));
        }
        XrLirInstruction ret = {.op = XR_LIR_RET, .type = XR_LIR_UNIT};
        OK(xr_lir_set_terminator(module, join, &ret));
        XrLirDiagnostic diagnostic = {0};
        XrLirStatus status = xr_lir_module_finish(module, &diagnostic);
        CHECK(status == (scenario < 3 ? XR_LIR_OK : XR_LIR_VERIFY_ERROR));
        if (scenario == 3) CHECK(strstr(diagnostic.message, "release on each normal exit"));
        if (scenario == 4) CHECK(strstr(diagnostic.message, "reconstruction requires an empty lifetime"));
        if (scenario == 5) CHECK(strstr(diagnostic.message, "use follows release"));
        xr_lir_module_destroy(module);
    }
}
