/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_tuple_cases.h - Exact I64 tuples survive owner retirement and raw admission
 */
#include "lir_tuple_fixture.h"

static void tuple_reject_corrupt_access(const XrRbcProgram *program, XrRbcValueOperation operation,
    uint32_t invalid_index) {
    const XrRbcModule *owned = xr_rbc_program_view(program);
    const XrRbcFunction *entry = &owned->functions[0];
    const XrRbcValueCode *code = xr_rbc_module_value_code(owned, 0);
    CHECK(code && owned->borrowed_value_code_count == owned->function_count && owned->borrowed_value_image);
    const XrRbcValueCode **codes = xr_malloc(owned->function_count * sizeof(*codes));
    XrRbcValueAccess *accesses = xr_malloc(code->access_count * sizeof(*accesses));
    CHECK(codes && accesses);
    memcpy(codes, owned->borrowed_value_codes, owned->function_count * sizeof(*codes));
    memcpy(accesses, code->accesses, code->access_count * sizeof(*accesses));
    XrRbcValueCode changed = *code;
    changed.accesses = accesses;
    codes[0] = &changed;
    XrRbcModule raw = *owned;
    raw.code_lease = (XrRbcCodeLease){0};
    raw.borrowed_value_codes = codes;
    bool found = false;
    for (size_t pc = 0; pc < entry->code_count; ++pc) {
        uint32_t word = entry->code[pc], index = word >> 16;
        if ((word & 255u) != XR_RBC_VALUE_ACCESS || index >= code->access_count ||
            accesses[index].operation != operation) continue;
        CHECK(xr_rbc_value_instruction_valid(&raw, entry, pc));
        accesses[index].index = invalid_index;
        CHECK(!xr_rbc_value_instruction_valid(&raw, entry, pc));
        found = true;
        break;
    }
    CHECK(found);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&raw, 0, NULL, 0, xr_rbc_program_locations(program), &execution) ==
        XR_RBC_INVALID && !execution);
    xr_free(accesses);
    xr_free(codes);
}

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
        tuple_reject_corrupt_access(program, XR_RBC_VALUE_RECORD_FIELD, 2);
        XrRbcExecution *execution = NULL;
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

static void tuple_root_result(void) {
    for (uint32_t nullable = 0; nullable < 2; ++nullable) {
        CArrayFixture c = {0};
        OK(xr_lir_module_create(NULL, &c.module));
        XrLirType tuple = 0;
        OK(xr_lir_tuple_i64_type(c.module, 2, &tuple));
        XrLirType result = tuple;
        if (nullable) OK(xr_lir_nullable_type(c.module, tuple, &result));
        XrLirFunctionSpec spec = {.name="root", .name_length=4, .result=result};
        XrLirFunction root = {0};
        OK(xr_lir_declare_function(c.module, &spec, &root));
        OK(xr_lir_define_function(c.module, root));
        XrLirBlock block = {0};
        OK(xr_lir_add_block(c.module, root, NULL, 0, &block));
        OK(xr_lir_set_entry(c.module, root, block));
        XrLirValue fields[] = {ca_constant(&c, block, 42), ca_constant(&c, block, -17)};
        XrLirValue value = ca_value(&c, block, XR_LIR_RECORD_NEW, tuple, fields, 2);
        if (nullable) value = ca_value(&c, block, XR_LIR_NULLABLE_SOME, result, &value, 1);
        ca_return(&c, block, value);
        OK(xr_lir_module_finish(c.module, NULL));
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic diagnostic = {0};
        CHECK(xr_rbc_compile(c.module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
        xr_lir_module_destroy(c.module);
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        xr_rbc_execution_free(&execution);
        xr_rbc_program_destroy(program);
        const XrtValueImage *image = NULL;
        uint32_t type = 0;
        const uint8_t *bytes = NULL;
        CHECK(xr_rbc_outcome_inline(outcome, &image, &type, &bytes) && image && type && bytes);
        const XrtValueImageType *row = &image->types[type - 1];
        if (nullable) {
            CHECK(row->kind == XRT_VALUE_NULLABLE && bytes[row->nullable_tag] == 1 && row->nullable_element < type);
            bytes += row->nullable_payload;
            row = &image->types[row->nullable_element - 1];
        }
        CHECK(row->kind == XRT_VALUE_TUPLE && row->size == 2 * sizeof(int64_t) && row->field_count == 2);
        int64_t payload[2];
        for (uint32_t f = 0; f < 2; ++f)
            memcpy(&payload[f], bytes + image->slots[row->first_field + f].offset, sizeof(payload[f]));
        CHECK(payload[0] == 42 && payload[1] == -17);
        uint64_t untouched = 11;
        CHECK(!xr_rbc_outcome_bits(outcome, &untouched) && untouched == 11);
        xr_rbc_outcome_free(&outcome);
    }
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
    tuple_reject_corrupt_access(program, XR_RBC_VALUE_STATE_GROUP_INITIALIZE, 1);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == 99);
    xr_rbc_outcome_free(&outcome);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
}
