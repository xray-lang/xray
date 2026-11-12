/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_display_alloc_cases.h - Complete display metadata failure and retirement
 */
static void display_compiler_failures(void) {
    for (uint32_t nested = 0; nested < 2; ++nested) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirType type = XR_LIR_I1;
        for (uint32_t depth = 0; nested && depth < 128; ++depth)
            OK(xr_lir_nullable_type(module, depth ? type : XR_LIR_I64, &type));
        OK(xr_lir_display_catalog_install(module, false, false));
        uint32_t intrinsic = 0;
        OK(xr_lir_display_operation(module, &intrinsic));
        XrLirFunction root = test_function(module, "display_length", NULL, 0, XR_LIR_I64);
        XrLirBlock block = test_block(module, root, NULL, 0);
        OK(xr_lir_set_entry(module, root, block));
        XrLirValue value = {0}, text = {0}, length = {0};
        XrLirInstruction instruction = {.op=nested ? XR_LIR_NULLABLE_NONE : XR_LIR_CONST,
            .type=type, .bits=nested ? 0 : 1};
        OK(xr_lir_emit_value(module, block, &instruction, &value));
        instruction = (XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_STRING,
            .intrinsic=intrinsic, .operands=&value, .operand_count=1};
        OK(xr_lir_emit_value(module, block, &instruction, &text));
        instruction = (XrLirInstruction){.op=XR_LIR_STRING_LEN, .type=XR_LIR_I64,
            .operands=&text, .operand_count=1};
        OK(xr_lir_emit_value(module, block, &instruction, &length));
        instruction = (XrLirInstruction){.op=XR_LIR_STRING_DROP, .type=XR_LIR_UNIT,
            .operands=&text, .operand_count=1};
        OK(xr_lir_emit_effect(module, block, &instruction));
        test_return(module, block, length);
        OK(xr_lir_module_finish(module, NULL));
        XrRbcProgram *program = NULL;
        attempts = 0; fail_at = SIZE_MAX;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
        size_t total = attempts;
        xr_rbc_program_destroy(program); program = NULL;
        CHECK(total && !live_allocations);
        for (size_t point = 1; point <= total; ++point) {
            attempts = 0; fail_at = point;
            CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
            CHECK(!program && !live_allocations);
        }
        fail_at = SIZE_MAX;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
        xr_lir_module_destroy(module);
        uint64_t bits = 0;
        CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OK && bits == 4);
        xr_rbc_program_destroy(program);
        CHECK(!live_allocations);
        printf("Inline display %s compiler allocation failure points: %zu\n", nested ? "nested" : "bool", total);
    }
}
