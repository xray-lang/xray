/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_block_cell_cases.h - Temporary Array Cells retain block-local ownership
 */
#include "lir/xlir_array.h"

static void array_block_cells(void) {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        XrLirModule *module = NULL;
        OK(xr_lir_array_module_create(NULL, &module));
        XrLirFunction function = {0};
        XrLirFunctionSpec spec = {.name = "temporary", .name_length = 9, .result = XR_LIR_UNIT};
        OK(xr_lir_declare_function(module, &spec, &function));
        OK(xr_lir_define_function(module, function));
        XrLirBlock entry = {0}, body = {0}, leave = {0};
        OK(xr_lir_add_block(module, function, NULL, 0, &entry));
        OK(xr_lir_add_block(module, function, NULL, 0, &body));
        OK(xr_lir_add_block(module, function, NULL, 0, &leave));
        OK(xr_lir_set_entry(module, function, entry));
        XrLirInstruction branch = {.op = XR_LIR_BR, .type = XR_LIR_UNIT, .edge_count = 1,
            .edges = {{body, NULL, 0}}};
        OK(xr_lir_set_terminator(module, entry, &branch));
        uint32_t constructor = 0;
        OK(xr_lir_array_operation(module, XR_LIR_I64, XR_LIR_ARRAY_OP_NEW, &constructor));
        XrLirValue array = {0}, cell = {0};
        XrLirInstruction make = {.op = XR_LIR_CALL_INTRINSIC, .type = XR_LIR_ARRAY, .intrinsic = constructor};
        OK(xr_lir_emit_value(module, body, &make, &array));
        make = (XrLirInstruction){.op = XR_LIR_CELL_NEW, .type = XR_LIR_ARRAY_CELL,
            .operands = &array, .operand_count = 1};
        OK(xr_lir_emit_value(module, body, &make, &cell));
        XrLirInstruction drop = {.op = XR_LIR_ARRAY_DROP, .type = XR_LIR_UNIT,
            .operands = &array, .operand_count = 1};
        OK(xr_lir_emit_effect(module, body, &drop));
        drop.op = XR_LIR_ARRAY_CELL_DROP;
        drop.operands = &cell;
        if (scenario != 1 && scenario < 6) OK(xr_lir_emit_effect(module, body, &drop));
        if (scenario == 2) OK(xr_lir_emit_effect(module, body, &drop));
        if (scenario == 3 || scenario == 4) {
            XrLirValue read = {0};
            XrLirInstruction use = {.op = XR_LIR_CELL_READ, .type = XR_LIR_ARRAY,
                .operands = &cell, .operand_count = 1};
            XrLirBlock selected = scenario == 3 ? body : leave;
            OK(xr_lir_emit_value(module, selected, &use, &read));
            use = (XrLirInstruction){.op = XR_LIR_ARRAY_DROP, .type = XR_LIR_UNIT,
                .operands = &read, .operand_count = 1};
            OK(xr_lir_emit_effect(module, selected, &use));
        }
        if (scenario >= 5) {
            XrLirType parameter = XR_LIR_ARRAY_CELL;
            XrLirFunction leaf = {0};
            spec = (XrLirFunctionSpec){.name = "borrow", .name_length = 6, .result = XR_LIR_UNIT,
                .parameters = &parameter, .parameter_count = 1};
            OK(xr_lir_declare_function(module, &spec, &leaf));
            XrLirValue input = {0}; XrLirArrayShape shape = {XR_LIR_I64};
            OK(xr_lir_function_parameter(module, leaf, 0, &input));
            OK(xr_lir_set_array_shape(module, input, &shape));
            OK(xr_lir_define_function(module, leaf));
            XrLirBlock leaf_body = {0};
            OK(xr_lir_add_block(module, leaf, NULL, 0, &leaf_body));
            OK(xr_lir_set_entry(module, leaf, leaf_body));
            XrLirInstruction ret = {.op = XR_LIR_RET, .type = XR_LIR_UNIT};
            if (scenario == 7) {
                XrLirInstruction invalid_drop = {.op = XR_LIR_ARRAY_CELL_DROP, .type = XR_LIR_UNIT,
                    .operands = &input, .operand_count = 1};
                OK(xr_lir_emit_effect(module, leaf_body, &invalid_drop));
            }
            OK(xr_lir_set_terminator(module, leaf_body, &ret));
            XrLirInstruction call = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT, .callee = leaf,
                .operands = &cell, .operand_count = 1};
            OK(xr_lir_emit_effect(module, body, &call));
            if (scenario >= 6) OK(xr_lir_emit_effect(module, body, &drop));
        }
        branch.edges[0].target = leave;
        OK(xr_lir_set_terminator(module, body, &branch));
        XrLirInstruction ret = {.op = XR_LIR_RET, .type = XR_LIR_UNIT};
        OK(xr_lir_set_terminator(module, leave, &ret));
        XrLirDiagnostic diagnostic = {0};
        XrLirStatus status = xr_lir_module_finish(module, &diagnostic);
        CHECK(status == (scenario && scenario != 6 ? XR_LIR_VERIFY_ERROR : XR_LIR_OK));
        if (scenario == 1) CHECK(strstr(diagnostic.message, "release on each normal exit"));
        if (scenario == 2 || scenario == 3 || scenario == 5) CHECK(strstr(diagnostic.message, "use follows release"));
        if (scenario == 4) CHECK(strstr(diagnostic.message, "use follows release"));
        if (scenario == 7) CHECK(strstr(diagnostic.message, "borrow cannot be released"));
        xr_lir_module_destroy(module);
    }
}
