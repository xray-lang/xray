/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_tuple_cases.h - Exact I64 tuples survive owner retirement and raw admission
 */
#include "lir_tuple_fixture.h"

static void tuple_receiving_owners(void) {
    XrLirModule *source = tuple_fixture(), *copies[3] = {0};
    OK(xr_lir_rebuild(source, &copies[0]));
    OK(xr_lir_inline(source, &copies[1]));
    OK(xr_lir_sccp(source, &copies[2]));
    xr_lir_module_destroy(source);
    for (unsigned n = 0; n < 3; ++n) {
        XrLirModule *module = copies[n];
        XrLirTypeView view;
        const XrLirRecordField *fields = NULL;
        uint32_t count = 0;
        OK(xr_lir_type_view(module, XR_LIR_TYPE_COUNT, &view));
        OK(xr_lir_aggregate_fields(module, XR_LIR_TYPE_COUNT, &fields, &count));
        CHECK(view.kind == XR_LIR_TYPE_TUPLE && !view.element && view.count == 2 && count == 2);
        CHECK(fields[0].type == XR_LIR_I64 && fields[1].type == XR_LIR_I64 && !fields[0].name && !fields[1].name);
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic diagnostic = {0};
        CHECK(xr_rbc_compile(module, (XrLirFunction){module, 1}, &program, &diagnostic) == XR_RBC_COMPILE_OK);
        xr_lir_module_destroy(module);
        const XrRbcModule *owned = xr_rbc_program_view(program);
        const XrtValueImage *image = xr_rbc_module_value_image(owned);
        const XrRbcValueCode *code = xr_rbc_module_value_code(owned, 0);
        CHECK(image && code && !owned->aggregate_type_count);
        bool projected = false;
        for (size_t a = 0; a < code->access_count; ++a) if (code->accesses[a].operation == XR_RBC_VALUE_RECORD_NEW) {
            uint32_t value = code->accesses[a].result;
            const XrtValueImageType *type = &image->types[code->homes->values[value].type - 1];
            CHECK(type->kind == XRT_VALUE_TUPLE && type->size == 16 && type->alignment == 8 &&
                type->field_count == 2 && !type->owned_count && image->slots[type->first_field + 1].offset == 8);
            projected = true;
        }
        CHECK(projected);
        XrRbcModule raw = *owned;
        XrRbcValueCode code_copy = *code;
        const XrRbcValueCode *codes[] = {&code_copy};
        raw.borrowed_value_codes = codes;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&raw, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
        CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        xr_rbc_execution_free(&execution);
        xr_rbc_program_destroy(program);
        uint64_t bits = 0;
        CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
        xr_rbc_outcome_free(&outcome);
    }
    puts("I64 Tuple receiving owners, physical layouts and sealed field projection passed");
}

static void tuple_group_admission(void) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    XrLirType tuple = 0;
    OK(xr_lir_tuple_i64_type(c.module, 2, &tuple));
    XrLirStateModule owner = {0};
    XrLirStateSlot slots[] = {
        {.payload=XR_LIR_I64, .mutable=true, .size=8, .alignment=8},
        {.payload=XR_LIR_I64, .mutable=true, .offset=8, .size=8, .alignment=8}};
    XrLirStateLayout layout = {.modules=&owner, .module_count=1, .slots=slots,
        .slot_count=2, .payload_size=16, .payload_alignment=8};
    OK(xr_lir_set_state_layout(c.module, &layout));
    XrLirType state_type = XR_LIR_STATE;
    XrLirFunction root = {0};
    XrLirFunctionSpec spec = {.name="group", .name_length=5, .parameters=&state_type,
        .parameter_count=1, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &root));
    OK(xr_lir_define_function(c.module, root));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(c.module, root, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, root, block));
    OK(xr_lir_set_state_entry(c.module, root));
    XrLirValue state = {0};
    OK(xr_lir_function_parameter(c.module, root, 0, &state));
    ca_effect(&c, block, XR_LIR_MODULE_BEGIN, state);
    XrLirValue values[] = {ca_constant(&c, block, 42), ca_constant(&c, block, 17)};
    XrLirValue value = ca_value(&c, block, XR_LIR_RECORD_NEW, tuple, values, 2), refs[2] = {{0}};
    for (uint32_t n = 0; n < 2; ++n) {
        XrLirValue args[] = {state, values[n]};
        XrLirInstruction create = {.op=XR_LIR_STATE_CONSTRUCT, .type=XR_LIR_STATE_REF,
            .operands=args, .operand_count=2, .bits=n};
        OK(xr_lir_emit_value(c.module, block, &create, &refs[n]));
    }
    XrLirValue write_args[] = {refs[0], ca_constant(&c, block, 99)};
    XrLirInstruction write = {.op=XR_LIR_STATE_WRITE, .type=XR_LIR_UNIT,
        .operands=write_args, .operand_count=2};
    OK(xr_lir_emit_effect(c.module, block, &write));
    XrLirValue args[] = {state, value};
    XrLirInstruction publish = {.op=XR_LIR_STATE_GROUP_INITIALIZE, .type=XR_LIR_UNIT,
        .operands=args, .operand_count=2};
    OK(xr_lir_emit_effect(c.module, block, &publish));
    ca_effect(&c, block, XR_LIR_MODULE_READY, state);
    ca_return(&c, block, ca_value(&c, block, XR_LIR_STATE_READ, XR_LIR_I64, refs, 1));
    OK(xr_lir_module_finish(c.module, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    CHECK(xr_rbc_compile(c.module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(c.module);
    const XrRbcModule *owned = xr_rbc_program_view(program);
    XrRbcModule bad = *owned;
    const XrRbcValueCode *code = xr_rbc_module_value_code(owned, 0);
    CHECK(code);
    size_t bytes = code->access_count * sizeof(*code->accesses);
    XrRbcValueAccess *accesses = xr_malloc(bytes);
    CHECK(accesses);
    memcpy(accesses, code->accesses, bytes);
    bool changed = false;
    for (size_t n = 0; n < code->access_count; ++n)
        if (accesses[n].operation == XR_RBC_VALUE_STATE_GROUP_INITIALIZE) { accesses[n].index = 1; changed = true; }
    CHECK(changed);
    XrRbcValueCode raw = *code;
    raw.accesses = accesses;
    const XrRbcValueCode *codes[] = {&raw};
    bad.borrowed_value_codes = codes;
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&bad, 0, NULL, 0, xr_rbc_program_locations(program), &execution) == XR_RBC_INVALID && !execution);
    xr_free(accesses);
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == 99);
    xr_rbc_outcome_free(&outcome);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
}
