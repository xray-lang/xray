/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_ref_cases.h - Borrowed binding shapes and live-owner enforcement
 */
#include "lir_array_ref_fixture.h"

static void array_reference_readonly(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_array_module_create(NULL, &m));
    XrLirStateModule module = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_ARRAY, .size=8, .alignment=8,
        .array_element=XR_LIR_U8};
    XrLirStateLayout layout = {.modules=&module, .module_count=1, .slots=&slot,
        .slot_count=1, .payload_size=8, .payload_alignment=8};
    OK(xr_lir_set_state_layout(m, &layout));
    XrLirType state_type = XR_LIR_STATE;
    XrLirFunction function = {0};
    XrLirFunctionSpec spec = {.name="readonly", .name_length=8, .parameters=&state_type,
        .parameter_count=1, .result=XR_LIR_UNIT};
    OK(xr_lir_declare_function(m, &spec, &function));
    OK(xr_lir_set_state_entry(m, function));
    OK(xr_lir_define_function(m, function));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(m, function, NULL, 0, &block));
    OK(xr_lir_set_entry(m, function, block));
    XrLirValue state = {0}, reference = {0}, borrowed = {0};
    OK(xr_lir_function_parameter(m, function, 0, &state));
    XrLirInstruction op = {.op=XR_LIR_STATE_REFERENCE, .type=XR_LIR_STATE_REF,
        .operands=&state, .operand_count=1};
    OK(xr_lir_emit_value(m, block, &op, &reference));
    op = (XrLirInstruction){.op=XR_LIR_ARRAY_BORROW, .type=XR_LIR_ARRAY_REF,
        .operands=&reference, .operand_count=1};
    CHECK(xr_lir_emit_value(m, block, &op, &borrowed) == XR_LIR_VERIFY_ERROR);
    CHECK(!borrowed.id);
    xr_lir_module_destroy(m);
}

static void array_reference_owners(void) {
    array_reference_readonly();
    const XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
    for (size_t i = 0; i < sizeof(elements) / sizeof(*elements); ++i) {
        XrLirModule *input = NULL, *copy = NULL, *inlined = NULL;
        OK(array_ref_fixture(elements[i], &input));
        OK(xr_lir_rebuild(input, &copy));
        OK(xr_lir_inline(input, &inlined));
        xr_lir_module_destroy(input);
        OK(xr_lir_verify(copy, NULL));
        OK(xr_lir_verify(inlined, NULL));
        XrLirFunctionView signature = {0};
        OK(xr_lir_function_view(copy, (XrLirFunction){copy, 2}, &signature));
        XrLirValue formal = {copy, 2, signature.parameters[0]};
        XrLirArrayShape shape = {0};
        OK(xr_lir_array_shape_view(copy, formal, &shape));
        CHECK(shape.element == elements[i]);
        XrLirFunctionData *helper = &copy->functions[1];
        XrLirType saved = helper->values[formal.id - 1].array_element;
        helper->values[formal.id - 1].array_element = elements[(i + 1) % 4];
        CHECK(xr_lir_verify(copy, NULL) == XR_LIR_VERIFY_ERROR);
        helper->values[formal.id - 1].array_element = saved;
        XrLirBlockData *body = &copy->functions[0].blocks[0];
        CHECK(body->instructions[7].view.op == XR_LIR_CALL &&
            body->instructions[10].view.op == XR_LIR_ARRAY_CELL_DROP);
        XrLirInstructionData call = body->instructions[7];
        body->instructions[7] = body->instructions[10];
        body->instructions[10] = call;
        XrLirDiagnostic diagnostic = {0};
        CHECK(xr_lir_verify(copy, &diagnostic) == XR_LIR_VERIFY_ERROR);
        CHECK(strstr(diagnostic.message, "release") != NULL);
        body->instructions[10] = body->instructions[7];
        body->instructions[7] = call;
        OK(xr_lir_verify(copy, NULL));
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrLirCOutput output = {0};
        XrLirCRequest request = {.module=copy, .entry={copy, 1}, .resources=resources,
            .translation_unit="array-ref.c"};
        CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_OK);
        CHECK(output.bytes && output.length);
        xr_lir_c_output_free(&output);
        xr_compile_resources_release(resources);
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic compile_diagnostic = {0};
        XrRbcCompileStatus compiled = xr_rbc_compile(copy, (XrLirFunction){copy, 1}, &program, &compile_diagnostic);
        if (compiled != XR_RBC_COMPILE_OK) fprintf(stderr, "Array Ref bytecode: %s (%u/%u/%u)\n",
            compile_diagnostic.message, compile_diagnostic.function, compile_diagnostic.block, compile_diagnostic.instruction);
        CHECK(compiled == XR_RBC_COMPILE_OK);
        CHECK(program);
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        uint64_t result = UINT64_MAX;
        CHECK(xr_rbc_outcome_bits(outcome, &result) && result == 0);
        xr_rbc_outcome_free(&outcome);
        xr_rbc_execution_free(&execution);
        xr_rbc_program_destroy(program);
        xr_lir_module_destroy(copy);
        xr_lir_module_destroy(inlined);
    }
}
