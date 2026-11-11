/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_unsigned_frontier_cases.h - Exact shapes cannot fall into an implemented family
 */
static void array_unsigned_frontier(void) {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        XrLirType element = scenario & 1 ? XR_LIR_U64 : XR_LIR_U8;
        bool stateful = scenario >= 2;
        XrLirModule *m = NULL;
        OK(xr_lir_array_module_create(NULL, &m));
        XrLirStateModule state_module = {0};
        XrLirStateSlot slot = {.payload=XR_LIR_ARRAY, .mutable=true, .size=8, .alignment=8,
            .array_element=element};
        XrLirStateLayout layout = {.modules=&state_module, .module_count=1,
            .slots=&slot, .slot_count=1, .payload_size=8, .payload_alignment=8};
        if (stateful) OK(xr_lir_set_state_layout(m, &layout));
        XrLirType state_type = XR_LIR_STATE;
        XrLirFunction f = {0};
        XrLirFunctionSpec spec = {.name="unsigned_frontier", .name_length=17, .result=XR_LIR_I64,
            .parameters=stateful ? &state_type : NULL, .parameter_count=stateful ? 1u : 0u};
        OK(xr_lir_declare_function(m, &spec, &f));
        if (stateful) OK(xr_lir_set_state_entry(m, f));
        OK(xr_lir_define_function(m, f));
        XrLirBlock block = {0};
        OK(xr_lir_add_block(m, f, NULL, 0, &block));
        OK(xr_lir_set_entry(m, f, block));
        XrLirValue value = {0};
        if (stateful) {
            XrLirValue state = {0};
            OK(xr_lir_function_parameter(m, f, 0, &state));
            XrLirInstruction reference = {.op=XR_LIR_STATE_REFERENCE, .type=XR_LIR_STATE_REF,
                .operands=&state, .operand_count=1};
            OK(xr_lir_emit_value(m, block, &reference, &value));
        } else {
            uint32_t id = 0;
            OK(xr_lir_array_operation(m, element, XR_LIR_ARRAY_OP_NEW, &id));
            XrLirInstruction create = {.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_ARRAY, .intrinsic=id};
            OK(xr_lir_emit_value(m, block, &create, &value));
            XrLirInstruction drop = {.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT,
                .operands=&value, .operand_count=1};
            OK(xr_lir_emit_effect(m, block, &drop));
        }
        XrLirInstruction zero = {.op=XR_LIR_CONST, .type=XR_LIR_I64};
        OK(xr_lir_emit_value(m, block, &zero, &value));
        XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&value, .operand_count=1};
        OK(xr_lir_set_terminator(m, block, &ret));
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = NULL;
        CHECK(xr_rbc_compile(m, f, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED && !program);
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrLirCRequest request = {.module=m, .entry=f, .resources=resources,
            .translation_unit="unsigned-frontier.c"};
        XrLirCOutput output = {0};
        CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_UNSUPPORTED);
        CHECK(!output.bytes && !output.length && !output.entry_result);
        xr_compile_resources_release(resources);
        xr_lir_module_destroy(m);
    }
}

static void array_unsigned_spans(void) {
    for (unsigned family = 0; family < 2; ++family) {
        XrLirType element = family ? XR_LIR_U64 : XR_LIR_U8;
        XrLirModule *m = NULL;
        OK(xr_lir_array_module_create(NULL, &m));
        XrLirFunction f = {0};
        XrLirFunctionSpec spec = {.name="unsigned_span", .name_length=13, .result=XR_LIR_ARRAY};
        OK(xr_lir_declare_function(m, &spec, &f));
        XrLirArrayShape shape = {element};
        OK(xr_lir_set_array_result(m, f, &shape));
        OK(xr_lir_define_function(m, f));
        XrLirBlock block = {0};
        OK(xr_lir_add_block(m, f, NULL, 0, &block));
        OK(xr_lir_set_entry(m, f, block));
        XrLirValue values[300], array = {0};
        for (uint32_t i = 0; i < 300; ++i) {
            uint64_t bits = family ? UINT64_MAX - i : i & 255u;
            if (!i) bits = 0;
            XrLirInstruction value = {.op=XR_LIR_CONST, .type=element, .bits=bits};
            OK(xr_lir_emit_value(m, block, &value, &values[i]));
        }
        uint32_t id = 0;
        OK(xr_lir_array_operation(m, element, XR_LIR_ARRAY_OP_NEW, &id));
        XrLirInstruction create = {.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_ARRAY,
            .intrinsic=id, .operands=values, .operand_count=300};
        OK(xr_lir_emit_value(m, block, &create, &array));
        XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&array, .operand_count=1};
        OK(xr_lir_set_terminator(m, block, &ret));
        OK(xr_lir_module_finish(m, NULL));
        XrLirModule *copy = NULL;
        OK(xr_lir_rebuild(m, &copy));
        xr_lir_module_destroy(m);
        XrLirInstructionView value = {0}, constructor = {0};
        XrLirBlock received = {copy, f.id, block.id};
        OK(xr_lir_instruction_view(copy, received, family ? 1 : 255, &value));
        CHECK(value.type == element && value.bits == (family ? UINT64_MAX - 1 : 255));
        OK(xr_lir_instruction_view(copy, received, 300, &constructor));
        CHECK(constructor.operand_count == 300 && constructor.intrinsic == id);
        XrLirType other = family ? XR_LIR_U8 : XR_LIR_U64;
        copy->functions[f.id - 1].values[values[0].id - 1].type = other;
        copy->functions[f.id - 1].blocks[block.id - 1].instructions[0].view.type = other;
        CHECK(xr_lir_verify(copy, NULL) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(copy);
    }
}
