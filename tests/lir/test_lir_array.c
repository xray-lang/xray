/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_array.c - Closed Array shapes survive receiving-owner transforms
 */
#include "lir/xlir_internal.h"
#include "lir/xlir_inline.h"
#include "lir/xlir_rebuild.h"
#include "lir/xlir_sccp.h"
#include "lir/xlir_verify.h"
#include "lir/xlir_c.h"
#include "lir/xlir_string.h"
#include "vm/register/xrbc_lir.h"
#include "vm/register/xrbc_value_code.h"
#include "xray_native_array.h"
#include "base/xmalloc.h"
#include "lir_array_fixture.h"
#include "lir_array_repeat_fixture.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)
#include "lir_array_c_fixture.h"
#include "lir_array_unsigned_frontier_cases.h"
#include "lir_array_ref_cases.h"
#include "lir_constructed_frontier_cases.h"
#include "lir_fixed_bytecode_cases.h"
#include "lir_view_cases.h"
#include "lir_callable_frontier_cases.h"
#include "lir_callable_fixture.h"
#include "lir_callable_factory_fixture.h"
#include "lir_callable_execution_cases.h"
#include "lir_callable_factory_cases.h"
#include "lir_callable_composition_cases.h"
#include "lir_captured_cell_cases.h"
#include "lir_tuple_cases.h"
#include "lir_map_cases.h"
#include "lir_c_cell_fixture.h"
#include "lir_frame_scalar_cell_fixture.h"
#include "lir_set_binding_fixture.h"
#include "lir_set_header_edge_fixture.h"
#include "lir_set_block_cell_fixture.h"
#include "lir_c_map_literal_fixture.h"
#include "lir_map_borrow_fixture.h"
#include "lir_slice_boundary_root_fixture.h"
#include "lir_slice_state_read_fixture.h"
#include "lir_slice_cleanup_fixture.h"
#include "lir_slice_callback_loan_fixture.h"
#include "lir_string_fixture.h"
#include "lir_array_forwarding_bytecode_cases.h"
#include "lir_array_local_bytecode_cases.h"

static void array_shapes(const XrLirModule *m, XrLirType element) {
    OK(xr_lir_verify(m, NULL));
    XrLirArrayIntrinsic intrinsic;
    OK(xr_lir_array_intrinsic_view(m, 11, &intrinsic));
    CHECK(intrinsic.variadic && intrinsic.id == 11 && intrinsic.parameter_count == 1);
    CHECK(intrinsic.parameter_elements[0] == XR_LIR_TYPE_INVALID && intrinsic.result_element == element);
    for (uint32_t f = 1; f <= xr_lir_function_count(m); ++f) {
        XrLirFunction function = {m, f};
        XrLirFunctionView view;
        XrLirArrayShape shape;
        OK(xr_lir_function_view(m, function, &view));
        OK(xr_lir_array_result_view(m, function, &shape));
        CHECK(shape.element == (view.result == XR_LIR_ARRAY ? element : XR_LIR_TYPE_INVALID));
        for (uint32_t v = 1; v <= view.value_count; ++v) {
            XrLirValue value = {m, f, v};
            XrLirType type;
            OK(xr_lir_value_type(m, value, &type));
            if (type == XR_LIR_ARRAY || type == XR_LIR_ARRAY_CELL) {
                OK(xr_lir_array_shape_view(m, value, &shape));
                CHECK(shape.element == element);
            } else {
                shape.element = XR_LIR_STRING;
                CHECK(xr_lir_array_shape_view(m, value, &shape) == XR_LIR_INVALID_ARGUMENT);
                CHECK(shape.element == XR_LIR_TYPE_INVALID);
            }
        }
    }
    XrLirInstructionView constructor;
    OK(xr_lir_instruction_view(m, (XrLirBlock){m, 1, 1}, 2, &constructor));
    CHECK(constructor.op == XR_LIR_CALL_INTRINSIC && constructor.intrinsic == 11);
    CHECK(constructor.operand_count == 2 && constructor.operands[0] == 1 && constructor.operands[1] == 1);
    if(element==XR_LIR_I64) {
        XrRbcProgram *program=NULL;
        CHECK(xr_rbc_compile(m,(XrLirFunction){m,1},&program,NULL)==XR_RBC_COMPILE_OK);
        uint64_t argument=7,result=0;XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_program_prepare(program,&argument,1,&execution)==XR_RBC_OK);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
        CHECK(xr_rbc_outcome_bits(outcome,&result) && result==3);
        xr_rbc_execution_free(&execution);xr_rbc_outcome_free(&outcome);xr_rbc_program_destroy(program);
    }
}

static void array_receiving_owners(XrLirType element) {
    XrLirModule *source = NULL, *receiver = NULL;
    OK(array_fixture_create(element, &source));
    array_shapes(source, element);
    XrLirArrayIntrinsic before, after;
    OK(xr_lir_array_intrinsic_view(source, 13, &before));
    OK(xr_lir_rebuild(source, &receiver));
    OK(xr_lir_array_intrinsic_view(receiver, 13, &after));
    CHECK(before.parameter_elements != after.parameter_elements);
    xr_lir_module_destroy(source);
    source = receiver; receiver = NULL;
    array_shapes(source, element);
    OK(xr_lir_inline(source, &receiver));
    xr_lir_module_destroy(source);
    source = receiver; receiver = NULL;
    array_shapes(source, element);
    OK(xr_lir_sccp(source, &receiver));
    xr_lir_module_destroy(source);
    array_shapes(receiver, element);
    xr_lir_module_destroy(receiver);
}

static void array_corruptions(void) {
    for (uint32_t scenario = 0; scenario < 13; ++scenario) {
        XrLirModule *m = NULL;
        OK(array_fixture_create(XR_LIR_I64, &m));
        XrLirFunctionData *root = &m->functions[0];
        if (scenario == 0) root->values[root->blocks[0].instructions[2].view.result - 1].array_element = XR_LIR_STRING;
        if (scenario == 1) m->functions[1].values[0].array_element = XR_LIR_STRING;
        if (scenario == 2) root->values[root->blocks[0].instructions[5].view.result - 1].array_element = XR_LIR_STRING;
        if (scenario == 3) root->values[root->blocks[1].parameters[0] - 1].array_element = XR_LIR_STRING;
        if (scenario == 4) m->functions[1].array_result = XR_LIR_STRING;
        if (scenario == 5) root->values[0].array_element = XR_LIR_I64;
        if (scenario == 6) m->array_intrinsics[0].result_element = XR_LIR_STRING;
        if (scenario == 7) m->array_intrinsics[0].id = 0;
        if (scenario == 8) root->blocks[0].instructions[2].view.op = XR_LIR_CONST;
        if (scenario == 9) root->blocks[0].instructions[6].view.op = XR_LIR_ARRAY_CELL_DROP;
        if (scenario == 10) ((XrLirType *)m->array_intrinsics[2].parameter_elements)[0] = XR_LIR_STRING;
        if (scenario == 11) m->array_intrinsics[0].operation = XR_LIR_ARRAY_OP_GET;
        if (scenario == 12) m->array_operations[0][0] = 99;
        CHECK(xr_lir_verify(m, NULL) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(m);
    }
}

static void array_catalog(void) {
    XrLirType scalar = XR_LIR_I64;
    XrLirIntrinsic extra = {1, &scalar, 1, scalar, true, 0};
    XrLirRegistry registry = {&extra, 1};
    XrLirModule *m = NULL;
    OK(xr_lir_array_module_create(&registry, &m));
    const XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
    for (uint32_t family = 0; family < sizeof(elements) / sizeof(*elements); ++family) {
        XrLirType element = elements[family];
        for (uint32_t op = XR_LIR_ARRAY_OP_NEW; op <= XR_LIR_ARRAY_OP_DISPLAY; ++op) {
            uint32_t id = 0;
            OK(xr_lir_array_operation(m, element, (XrLirArrayOperation)op, &id));
            CHECK(id && id != extra.id);
            CHECK(id == m->array_operations[family][op - 1]);
            CHECK(id == (op == XR_LIR_ARRAY_OP_DISPLAY ? 34 + family : 2 + family * 6 + op - 1));
        }
        for (uint32_t op = XR_LIR_ARRAY_OP_SET; op <= XR_LIR_ARRAY_OP_PUSH; ++op) {
            uint32_t cell = 0, reference = 0;
            OK(xr_lir_array_mutator(m, element, XR_LIR_ARRAY_CELL, (XrLirArrayOperation)op, &cell));
            OK(xr_lir_array_mutator(m, element, XR_LIR_ARRAY_REF, (XrLirArrayOperation)op, &reference));
            CHECK(cell != reference && reference == m->array_ref_operations[family][op - XR_LIR_ARRAY_OP_SET]);
            const XrLirIntrinsic *entry = NULL;
            for (uint32_t i = 0; i < m->intrinsic_count; ++i)
                if (m->intrinsics[i].id == reference) entry = &m->intrinsics[i];
            CHECK(entry && entry->parameters[0] == XR_LIR_ARRAY_REF && entry->result == XR_LIR_UNIT);
        }
    }
    OK(xr_lir_module_finish(m, NULL));
    XrLirModule *copy = NULL;
    OK(xr_lir_rebuild(m, &copy));
    xr_lir_module_destroy(m);
    uint32_t id = 99;
    OK(xr_lir_array_operation(copy, XR_LIR_STRING, XR_LIR_ARRAY_OP_NEW, &id));
    XrLirArrayIntrinsic shape;
    OK(xr_lir_array_intrinsic_view(copy, id, &shape));
    CHECK(shape.variadic && shape.operation == XR_LIR_ARRAY_OP_NEW && shape.result_element == XR_LIR_STRING);
    CHECK(xr_lir_array_operation(copy, XR_LIR_F64, XR_LIR_ARRAY_OP_NEW, &id) == XR_LIR_INVALID_ARGUMENT && !id);
    OK(xr_lir_array_mutator(copy, XR_LIR_U64, XR_LIR_ARRAY_REF, XR_LIR_ARRAY_OP_PUSH, &id));
    OK(xr_lir_array_intrinsic_view(copy, id, &shape));
    CHECK(shape.operation == XR_LIR_ARRAY_OP_PUSH && shape.parameter_elements[0] == XR_LIR_U64);
    CHECK(xr_lir_array_mutator(copy, XR_LIR_U64, XR_LIR_STATE_REF, XR_LIR_ARRAY_OP_PUSH, &id) ==
        XR_LIR_INVALID_ARGUMENT && !id);
    CHECK(xr_lir_array_mutator(copy, XR_LIR_U8, XR_LIR_ARRAY_REF, XR_LIR_ARRAY_OP_GET, &id) ==
        XR_LIR_INVALID_ARGUMENT && !id);
    uint32_t saved = copy->array_ref_operations[0][0];
    copy->array_ref_operations[0][0] = copy->array_operations[0][XR_LIR_ARRAY_OP_SET - 1];
    CHECK(xr_lir_verify(copy, NULL) == XR_LIR_VERIFY_ERROR);
    copy->array_ref_operations[0][0] = saved;
    OK(xr_lir_verify(copy, NULL));
    xr_lir_module_destroy(copy);
}

static void array_repeat_owners(void) {
    const XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
    for (unsigned family = 0; family < sizeof(elements) / sizeof(*elements); ++family) {
        XrLirType element = elements[family];
        XrLirModule *m = NULL;
        OK(array_repeat_fixture(element, &m));
        uint32_t id = 0;
        OK(xr_lir_array_operation(m, element, XR_LIR_ARRAY_OP_REPEAT, &id));
        XrLirArrayIntrinsic before = {0};
        OK(xr_lir_array_intrinsic_view(m, id, &before));
        CHECK(!before.variadic && before.parameter_count == 2 && before.result_element == element);
        CHECK(!before.parameter_elements[0] && !before.parameter_elements[1]);
        XrLirModule *copy = NULL;
        OK(xr_lir_rebuild(m, &copy));
        XrLirArrayIntrinsic after = {0};
        OK(xr_lir_array_intrinsic_view(copy, id, &after));
        CHECK(before.parameter_elements != after.parameter_elements);
        xr_lir_module_destroy(m);
        m = NULL;
        OK(xr_lir_inline(copy, &m));
        xr_lir_module_destroy(copy);
        copy = NULL;
        OK(xr_lir_sccp(m, &copy));
        xr_lir_module_destroy(m);
        OK(xr_lir_verify(copy, NULL));
        XrLirArrayShape shape = {0};
        OK(xr_lir_array_result_view(copy, (XrLirFunction){copy, 1}, &shape));
        CHECK(shape.element == element);
        xr_lir_module_destroy(copy);
    }
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        XrLirModule *m = NULL;
        OK(array_repeat_fixture(XR_LIR_STRING, &m));
        uint32_t id = 0;
        OK(xr_lir_array_operation(m, XR_LIR_STRING, XR_LIR_ARRAY_OP_REPEAT, &id));
        uint32_t index = 0;
        while (index < m->intrinsic_count && m->intrinsics[index].id != id) ++index;
        CHECK(index < m->intrinsic_count);
        if (scenario == 0) m->array_intrinsics[index].variadic = true;
        if (scenario == 1) m->array_intrinsics[index].result_element = XR_LIR_I64;
        if (scenario == 2) ((XrLirType *)m->array_intrinsics[index].parameter_elements)[0] = XR_LIR_I64;
        if (scenario == 3) ((XrLirType *)m->intrinsics[index].parameters)[0] = XR_LIR_STRING;
        if (scenario == 4) m->functions[0].blocks[0].instructions[0].view.operand_count = 1;
        if (scenario == 5) m->array_intrinsics[index].operation = XR_LIR_ARRAY_OP_NEW;
        CHECK(xr_lir_verify(m, NULL) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(m);
    }
}

static void array_missing_release(void) {
    XrLirModule *m = NULL;
    OK(array_fixture_create(XR_LIR_I64, &m));
    XrLirBlockData *join = &m->functions[0].blocks[1];
    xr_free((void *)join->instructions[1].view.operands);
    memmove(join->instructions + 1, join->instructions + 2,
        (size_t)(join->instruction_count - 2) * sizeof(*join->instructions));
    --join->instruction_count;
    /* Removing only the drop preserves SSA structure and leaks the edge owner. */
    CHECK(xr_lir_verify(m, NULL) == XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(m);
}

static void array_shape_rejections(void) {
    XrLirModule *m = NULL;
    OK(array_fixture_registry(XR_LIR_I64, &m));
    XrLirType type = XR_LIR_ARRAY;
    XrLirFunction f;
    XrLirFunctionSpec spec = {.name="bad", .name_length=3, .parameters=&type,
        .parameter_count=1, .result=XR_LIR_UNIT};
    OK(xr_lir_declare_function(m, &spec, &f));
    XrLirValue parameter;
    OK(xr_lir_function_parameter(m, f, 0, &parameter));
    XrLirArrayShape shape = {XR_LIR_F64};
    CHECK(xr_lir_set_array_shape(m, parameter, &shape) == XR_LIR_INVALID_ARGUMENT);
    CHECK(xr_lir_module_finish(m, NULL) == XR_LIR_INVALID_ARGUMENT);
    xr_lir_module_destroy(m);
    m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    spec.result = XR_LIR_ARRAY_CELL;
    CHECK(xr_lir_declare_function(m, &spec, &f) == XR_LIR_INVALID_ARGUMENT);
    xr_lir_module_destroy(m);
    m = NULL;
    XrLirType scalar = XR_LIR_I64;
    XrLirIntrinsic entry = {17, &scalar, 1, scalar, true, 0};
    XrLirRegistry registry = {&entry, 1};
    OK(xr_lir_module_create(&registry, &m));
    XrLirType no_shape = XR_LIR_TYPE_INVALID;
    XrLirArrayIntrinsic invalid = {17, &no_shape, 1, XR_LIR_TYPE_INVALID, false, XR_LIR_ARRAY_OP_LEN};
    CHECK(xr_lir_set_array_intrinsic(m, &invalid) == XR_LIR_INVALID_ARGUMENT);
    xr_lir_module_destroy(m);
    m = NULL;
    entry.result = XR_LIR_ARRAY;
    OK(xr_lir_module_create(&registry, &m));
    CHECK(xr_lir_module_finish(m, NULL) == XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(m);
}

static void array_backend_frontier(const XrLirModule *m, XrLirFunction entry) {
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(m, entry, &program, NULL) == XR_RBC_COMPILE_OK && program);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    uint64_t result=0;
    CHECK(xr_rbc_outcome_bits(outcome,&result) && result==3);
    xr_rbc_outcome_free(&outcome);xr_rbc_execution_free(&execution);xr_rbc_program_destroy(program);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=m, .entry=entry, .resources=resources,
        .translation_unit="array-frontier.c"};
    XrLirCOutput output = {0};
    CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_OK);
    CHECK(output.bytes && output.length && strstr(output.bytes, "xrt_native_array_create(") &&
        strstr(output.bytes, "XRT_BACKING_ARRAY_I64"));
    xr_lir_c_output_free(&output);
    xr_compile_resources_release(resources);
}

static void array_cell_loop(void) {
    XrLirModule *m = NULL;
    OK(array_fixture_registry(XR_LIR_I64, &m));
    XrLirFunction f;
    XrLirFunctionSpec spec = {.name="loop", .name_length=4, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(m, &spec, &f));
    OK(xr_lir_define_function(m, f));
    XrLirBlock entry, loop, done;
    OK(xr_lir_add_block(m, f, NULL, 0, &entry));
    OK(xr_lir_add_block(m, f, NULL, 0, &loop));
    OK(xr_lir_add_block(m, f, NULL, 0, &done));
    OK(xr_lir_set_entry(m, f, entry));
    XrLirValue array, cell, read, condition, length;
    XrLirInstruction instruction = {.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_ARRAY, .intrinsic=11};
    OK(xr_lir_emit_value(m, entry, &instruction, &array));
    instruction = (XrLirInstruction){.op=XR_LIR_CELL_NEW, .type=XR_LIR_ARRAY_CELL,
        .operands=&array, .operand_count=1};
    OK(xr_lir_emit_value(m, entry, &instruction, &cell));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT,
        .operands=&array, .operand_count=1};
    OK(xr_lir_emit_effect(m, entry, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_BR, .type=XR_LIR_UNIT,
        .edge_count=1, .edges={{loop, NULL, 0}}};
    OK(xr_lir_set_terminator(m, entry, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_CELL_READ, .type=XR_LIR_ARRAY,
        .operands=&cell, .operand_count=1};
    OK(xr_lir_emit_value(m, loop, &instruction, &read));
    XrLirValue operands[] = {cell, read};
    instruction = (XrLirInstruction){.op=XR_LIR_CELL_WRITE, .type=XR_LIR_UNIT,
        .operands=operands, .operand_count=2};
    OK(xr_lir_emit_effect(m, loop, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT,
        .operands=&read, .operand_count=1};
    OK(xr_lir_emit_effect(m, loop, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_I1};
    OK(xr_lir_emit_value(m, loop, &instruction, &condition));
    instruction = (XrLirInstruction){.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT,
        .operands=&condition, .operand_count=1, .edge_count=2,
        .edges={{loop, NULL, 0}, {done, NULL, 0}}};
    OK(xr_lir_set_terminator(m, loop, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_ARRAY_CELL_DROP, .type=XR_LIR_UNIT,
        .operands=&cell, .operand_count=1};
    OK(xr_lir_emit_effect(m, done, &instruction));
    instruction = (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_I64, .bits=3};
    OK(xr_lir_emit_value(m, done, &instruction, &length));
    instruction = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT,
        .operands=&length, .operand_count=1};
    OK(xr_lir_set_terminator(m, done, &instruction));
    OK(xr_lir_module_finish(m, NULL));
    array_backend_frontier(m, f);
    XrLirModule *copy = NULL;
    OK(xr_lir_rebuild(m, &copy));
    xr_lir_module_destroy(m);
    OK(xr_lir_verify(copy, NULL));
    xr_lir_module_destroy(copy);
}

static void array_state_shapes(XrLirType element) {
    XrLirModule *m = NULL;
    OK(array_fixture_registry(element, &m));
    XrLirStateModule module = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_ARRAY, .mutable=true, .size=8, .alignment=8,
        .array_element=element};
    XrLirStateLayout layout = {.modules=&module, .module_count=1, .slots=&slot,
        .slot_count=1, .payload_size=8, .payload_alignment=8};
    OK(xr_lir_set_state_layout(m, &layout));
    XrLirType state_type = XR_LIR_STATE, ref_type = XR_LIR_STATE_REF;
    XrLirFunction f;
    XrLirFunctionSpec spec = {.name="state", .name_length=5, .parameters=&state_type,
        .parameter_count=1, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(m, &spec, &f));
    OK(xr_lir_set_state_entry(m, f));
    OK(xr_lir_define_function(m, f));
    XrLirValue state, selected, number, array, reference, read, answer;
    OK(xr_lir_function_parameter(m, f, 0, &state));
    XrLirBlock entry, join;
    OK(xr_lir_add_block(m, f, NULL, 0, &entry));
    OK(xr_lir_add_block(m, f, &ref_type, 1, &join));
    OK(xr_lir_set_entry(m, f, entry));
    OK(xr_lir_block_parameter(m, join, 0, &selected));
    XrLirStateReference shape = {.payload=XR_LIR_ARRAY, .mutable=true, .array_element=element};
    OK(xr_lir_set_state_reference(m, selected, &shape));
    XrLirInstruction op = {.op=XR_LIR_MODULE_BEGIN, .type=XR_LIR_UNIT, .operands=&state, .operand_count=1};
    OK(xr_lir_emit_effect(m, entry, &op));
    op = (XrLirInstruction){.op=XR_LIR_CONST, .type=element, .bits=9};
    OK(xr_lir_emit_value(m, entry, &op, &number));
    op = (XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_ARRAY, .intrinsic=11,
        .operands=&number, .operand_count=1};
    OK(xr_lir_emit_value(m, entry, &op, &array));
    XrLirValue arguments[] = {state, array};
    op = (XrLirInstruction){.op=XR_LIR_STATE_CONSTRUCT, .type=XR_LIR_STATE_REF,
        .operands=arguments, .operand_count=2};
    OK(xr_lir_emit_value(m, entry, &op, &reference));
    op = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT, .operands=&array, .operand_count=1};
    OK(xr_lir_emit_effect(m, entry, &op));
    op = (XrLirInstruction){.op=XR_LIR_STATE_PUBLISH, .type=XR_LIR_UNIT, .operands=&reference, .operand_count=1};
    OK(xr_lir_emit_effect(m, entry, &op));
    op = (XrLirInstruction){.op=XR_LIR_MODULE_READY, .type=XR_LIR_UNIT, .operands=&state, .operand_count=1};
    OK(xr_lir_emit_effect(m, entry, &op));
    op = (XrLirInstruction){.op=XR_LIR_BR, .type=XR_LIR_UNIT, .edge_count=1, .edges={{join, &reference, 1}}};
    OK(xr_lir_set_terminator(m, entry, &op));
    op = (XrLirInstruction){.op=XR_LIR_STATE_READ, .type=XR_LIR_ARRAY, .operands=&selected, .operand_count=1};
    OK(xr_lir_emit_value(m, join, &op, &read));
    arguments[0] = selected; arguments[1] = read;
    op = (XrLirInstruction){.op=XR_LIR_STATE_WRITE, .type=XR_LIR_UNIT, .operands=arguments, .operand_count=2};
    OK(xr_lir_emit_effect(m, join, &op));
    op = (XrLirInstruction){.op=XR_LIR_ARRAY_DROP, .type=XR_LIR_UNIT, .operands=&read, .operand_count=1};
    OK(xr_lir_emit_effect(m, join, &op));
    op = (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_I64, .bits=9};
    OK(xr_lir_emit_value(m, join, &op, &answer));
    op = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&answer, .operand_count=1};
    OK(xr_lir_set_terminator(m, join, &op));
    OK(xr_lir_module_finish(m, NULL));
    XrLirArrayShape array_shape;
    OK(xr_lir_array_shape_view(m, read, &array_shape));
    CHECK(array_shape.element == element);
    XrLirModule *copy = NULL, *inlined = NULL;
    OK(xr_lir_rebuild(m, &copy));
    xr_lir_module_destroy(m);
    OK(xr_lir_inline(copy, &inlined));
    xr_lir_module_destroy(copy);
    OK(xr_lir_state_reference_view(inlined, (XrLirValue){inlined, 1, selected.id}, &shape));
    CHECK(shape.array_element == element && shape.payload == XR_LIR_ARRAY);
    inlined->functions[0].values[selected.id - 1].state_reference.array_element =
        element == XR_LIR_U8 ? XR_LIR_U64 : XR_LIR_U8;
    CHECK(xr_lir_verify(inlined, NULL) == XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(inlined);
}

static XrLirValue cs_binding_role(CArrayFixture *c, XrLirBlock block, XrLirStringOperation role,
    XrLirType type, const XrLirValue *arguments, uint32_t count) {
    uint32_t intrinsic=0; OK(xr_lir_string_operation(c->module,role,&intrinsic));
    XrLirInstruction instruction={.op=XR_LIR_CALL_INTRINSIC,.type=type,.intrinsic=intrinsic,
        .operands=arguments,.operand_count=count,.source=ca_source()};
    XrLirValue result={0};
    if (type==XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module,block,&instruction));
    else OK(xr_lir_emit_value(c->module,block,&instruction,&result));
    return result;
}
static void cs_binding_fixture(XrLirModule **output, XrLirFunction *entry) {
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    OK(xr_lir_string_catalog_install(c.module,false,false,false));
    XrLirStateModule state_module={0};
    XrLirStateSlot slot={.payload=XR_LIR_STRING,.mutable=true,.size=sizeof(void *),.alignment=_Alignof(void *)};
    XrLirStateLayout layout={.modules=&state_module,.module_count=1,.slots=&slot,.slot_count=1,
        .payload_size=sizeof(void *),.payload_alignment=_Alignof(void *)};
    OK(xr_lir_set_state_layout(c.module,&layout));
    XrLirType state_type=XR_LIR_STATE,parameters[]={XR_LIR_STRING,XR_LIR_STATE};
    XrLirFunction root={0},helper={0},borrowed_only={0};
    XrLirFunctionSpec spec={.name="root",.name_length=4,.parameters=&state_type,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&root)); OK(xr_lir_set_state_entry(c.module,root));
    spec=(XrLirFunctionSpec){.name="bindings",.name_length=8,.parameters=parameters,.parameter_count=2,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&helper));
    XrLirType ref_type=XR_LIR_STRING_REF;
    spec=(XrLirFunctionSpec){.name="borrowed_only",.name_length=13,.parameters=&ref_type,.parameter_count=1,.result=XR_LIR_UNIT};
    OK(xr_lir_declare_function(c.module,&spec,&borrowed_only)); OK(xr_lir_define_function(c.module,borrowed_only));
    XrLirBlock empty={0}; OK(xr_lir_add_block(c.module,borrowed_only,NULL,0,&empty));
    OK(xr_lir_set_entry(c.module,borrowed_only,empty));
    OK(xr_lir_set_terminator(c.module,empty,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT}));
    OK(xr_lir_define_function(c.module,helper));
    XrLirBlock body={0}; OK(xr_lir_add_block(c.module,helper,NULL,0,&body)); OK(xr_lir_set_entry(c.module,helper,body));
    XrLirValue incoming={0},state={0};
    OK(xr_lir_function_parameter(c.module,helper,0,&incoming)); OK(xr_lir_function_parameter(c.module,helper,1,&state));
    XrLirValue cell=ca_value(&c,body,XR_LIR_CELL_NEW,XR_LIR_STRING_CELL,&incoming,1);
    XrLirValue pair[]={cell,incoming};
    XrLirInstruction write={.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=pair,.operand_count=2,.source=ca_source()};
    OK(xr_lir_emit_effect(c.module,body,&write));
    XrLirValue local_ref=cs_binding_role(&c,body,XR_LIR_STRING_OP_BORROW_CELL,XR_LIR_STRING_REF,&cell,1);
    XrLirInstruction forward={.op=XR_LIR_CALL,.type=XR_LIR_UNIT,.callee=borrowed_only,.operands=&local_ref,.operand_count=1};
    OK(xr_lir_emit_effect(c.module,body,&forward));
    pair[0]=local_ref; (void)cs_binding_role(&c,body,XR_LIR_STRING_OP_WRITE_REF,XR_LIR_UNIT,pair,2);
    pair[0]=cell; (void)cs_binding_role(&c,body,XR_LIR_STRING_OP_APPEND_CELL,XR_LIR_UNIT,pair,2);
    pair[0]=local_ref; (void)cs_binding_role(&c,body,XR_LIR_STRING_OP_APPEND_REF,XR_LIR_UNIT,pair,2);
    XrLirValue reference=ca_value(&c,body,XR_LIR_STATE_REFERENCE,XR_LIR_STATE_REF,&state,1);
    pair[0]=reference; write=(XrLirInstruction){.op=XR_LIR_STATE_WRITE,.type=XR_LIR_UNIT,.operands=pair,.operand_count=2,.source=ca_source()};
    OK(xr_lir_emit_effect(c.module,body,&write));
    (void)cs_binding_role(&c,body,XR_LIR_STRING_OP_APPEND_STATE,XR_LIR_UNIT,pair,2);
    XrLirValue state_ref=cs_binding_role(&c,body,XR_LIR_STRING_OP_BORROW_STATE,XR_LIR_STRING_REF,&reference,1);
    XrLirValue left=cs_binding_role(&c,body,XR_LIR_STRING_OP_READ_REF,XR_LIR_STRING,&local_ref,1);
    XrLirValue right=cs_binding_role(&c,body,XR_LIR_STRING_OP_READ_REF,XR_LIR_STRING,&state_ref,1);
    XrLirValue left_length=ca_value(&c,body,XR_LIR_STRING_LEN,XR_LIR_I64,&left,1);
    XrLirValue right_length=ca_value(&c,body,XR_LIR_STRING_LEN,XR_LIR_I64,&right,1);
    ca_effect(&c,body,XR_LIR_STRING_DROP,left); ca_effect(&c,body,XR_LIR_STRING_DROP,right);
    ca_effect(&c,body,XR_LIR_STRING_CELL_DROP,cell);
    ca_return(&c,body,ca_binary(&c,body,XR_LIR_ADD,left_length,right_length));
    OK(xr_lir_define_function(c.module,root)); OK(xr_lir_add_block(c.module,root,NULL,0,&body));
    OK(xr_lir_set_entry(c.module,root,body)); OK(xr_lir_function_parameter(c.module,root,0,&state));
    ca_effect(&c,body,XR_LIR_MODULE_BEGIN,state);
    XrLirValue seed=ca_text(&c,body,"seed",4),rhs=ca_text(&c,body,"xy",2),construct[]={state,seed};
    reference=ca_value(&c,body,XR_LIR_STATE_CONSTRUCT,XR_LIR_STATE_REF,construct,2);
    ca_effect(&c,body,XR_LIR_STATE_PUBLISH,reference); ca_effect(&c,body,XR_LIR_MODULE_READY,state);
    XrLirValue snapshot=ca_value(&c,body,XR_LIR_STATE_READ,XR_LIR_STRING,&reference,1),arguments[]={rhs,state},result={0};
    XrLirInstruction call={.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=helper,.operands=arguments,.operand_count=2};
    OK(xr_lir_emit_value(c.module,body,&call,&result));
    XrLirValue saved_length=ca_value(&c,body,XR_LIR_STRING_LEN,XR_LIR_I64,&snapshot,1);
    ca_effect(&c,body,XR_LIR_STRING_DROP,snapshot); ca_effect(&c,body,XR_LIR_STRING_DROP,seed); ca_effect(&c,body,XR_LIR_STRING_DROP,rhs);
    ca_return(&c,body,ca_binary(&c,body,XR_LIR_ADD,result,saved_length));
    OK(xr_lir_module_finish(c.module,NULL)); *output=c.module; *entry=root;
}

static void cs_binding_setup_scope(const char *text) {
    static const char setup[]="xrt_native_string_binding_prepare(";
    static const char frame[]="    if (!xrl_frame) {\n";
    size_t calls=0,guarded=0;
    for (const char *cursor=text; (cursor=strstr(cursor,setup))!=NULL; cursor+=sizeof(setup)-1) ++calls;
    for (const char *cursor=text; (cursor=strstr(cursor,frame))!=NULL;) {
        const char *end=strstr(cursor,"    }\n"); CHECK(end);
        const char *prepare=strstr(cursor,setup);
        if (prepare && prepare<end) {
            const char *enter=strstr(cursor,"xrt_managed_frame_enter_payload(xrl_root, &xrl_frame_");
            if (!enter || enter>=end) enter=strstr(cursor,"xrt_managed_frame_enter_locations(xrl_root, &xrl_frame_");
            const char *failure=strstr(cursor,"if (status != XRT_MANAGED_OK) xrt_native_managed_failure(");
            CHECK(enter && failure && enter<failure && failure<prepare);
            ++guarded;
        }
        cursor=end+6;
    }
    CHECK(calls && calls==guarded);
}
static void slice_c_raw_entry_rejection(XrLirType element) {
    SliceBoundaryFixture fixture = slice_boundary_fixture(element, false);
    OK(xr_lir_module_finish(fixture.module, NULL));
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=fixture.module, .entry=fixture.fanout, .resources=resources,
        .translation_unit="raw-slice-entry.c"};
    XrLirCOutput output = {0};
    CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_BAD_ARGUMENT && !output.bytes && !output.length);
    xr_compile_resources_release(resources); xr_lir_module_destroy(fixture.module);
}
static void slice_c_unfinished_result_rejection(XrLirType element) {
    XrLirModule *module = NULL; OK(xr_lir_module_create(NULL, &module));
    XrLirType result = XR_LIR_SLICE_I64;
    if (element != XR_LIR_TYPE_INVALID) OK(xr_lir_slice_type(module, element, &result));
    XrLirFunction entry = {0};
    XrLirFunctionSpec spec = {.name="unpublished", .name_length=11, .result=result};
    XrLirStatus declared = xr_lir_declare_function(module, &spec, &entry);
    if (element == XR_LIR_TYPE_INVALID) {
        CHECK(declared == XR_LIR_INVALID_ARGUMENT && !entry.module && !entry.id);
        xr_lir_module_destroy(module); return;
    }
    OK(declared); OK(xr_lir_define_function(module, entry));
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=module, .entry=entry, .resources=resources,
        .translation_unit="unpublished-slice-result.c"};
    XrLirCOutput output = {0};
    CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_BAD_ARGUMENT && !output.bytes && !output.length);
    xr_compile_resources_release(resources); xr_lir_module_destroy(module);
}
static void slice_c_reader_has_no_owners(const char *text) {
    const char *needle = "static int64_t xrl_f4(", *reader = strstr(text, needle);
    while (reader) {
        const char *line = strchr(reader, '\n'), *body = strstr(reader, ") {\n");
        if (line && body && body < line) { reader = body; break; }
        reader = strstr(reader + 1, needle);
    }
    CHECK(reader);
    const char *end = strstr(reader, "\n}\n"); CHECK(end);
    const char *frame = strstr(reader, "xrl_frame"), *retain = strstr(reader, "xrt_managed_retain(");
    CHECK((!frame || frame > end) && (!retain || retain > end));
}
static void scalar_edge_structure(const char *text,XrLirModule *module,XrLirFunction function,bool mixed) {
    const char *cursor=strstr(text,"bb1: ;\n"); CHECK(cursor);
    CHECK((strstr(text,"XrlScalars_") != NULL)==mixed);
    CHECK((strstr(text,"xrl_phi_") == NULL)==mixed);
    const uint32_t blocks[]={1,2,2},edges[]={1,1,2},targets[]={2,2,3};
    for (size_t e=0;e<3;++e) {
        char marker[192];
        snprintf(marker,sizeof(marker),"XrtManagedStatus status = xrl_edge_%" PRIu32 "_%" PRIu32 "_%" PRIu32
            "(xrl_frame, xrl_payload, %s",function.id,blocks[e],edges[e],mixed?"&scalar_publication":"NULL");
        const char *call=strstr(cursor,marker); CHECK(call);
        const char *failure=strstr(call,"if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, 0);\n");
        CHECK(failure);
        snprintf(marker,sizeof(marker),"    goto bb%" PRIu32 ";\n",targets[e]);
        const char *jump=strstr(failure,marker); CHECK(jump);
        if (!mixed) {
            XrLirBlockView target;
            OK(xr_lir_block_view(module,(XrLirBlock){module,function.id,targets[e]},&target));
            const char *failure_end=strchr(failure,'\n'); CHECK(failure_end && failure_end<jump);
            const char *previous=cursor;
            for (uint32_t a=1;a<target.parameter_count;++a) {
                snprintf(marker,sizeof(marker),"    int64_t xrl_phi_%" PRIu32 " = ",a);
                const char *snapshot=strstr(cursor,marker); CHECK(snapshot && snapshot>=previous && snapshot<call);
                previous=snapshot+strlen(marker);
                snprintf(marker,sizeof(marker),"    v%" PRIu32 " = xrl_phi_%" PRIu32 ";\n",target.parameters[a],a);
                const char *publish=strstr(failure_end,marker); CHECK(publish && publish<jump);
                const char *early=strstr(cursor,marker); CHECK(early==publish);
            }
            snprintf(marker,sizeof(marker),"static XrtManagedStatus xrl_edge_%" PRIu32 "_%" PRIu32 "_%" PRIu32 "(",
                function.id,blocks[e],edges[e]);
            const char *declaration=strstr(text,marker); CHECK(declaration);
            const char *end=strchr(declaration,'\n'),*locations=strstr(declaration,"const XrtManagedFrameLocations *locations");
            CHECK(end);
            const char *arguments=locations && locations<end ? "xrl_payload, NULL, &xrl_frame_locations" : "xrl_payload, NULL";
            const char *null_argument=strstr(call,arguments),*close=strstr(call,");\n");
            CHECK(null_argument && close && null_argument+strlen(arguments)==close && close<failure);
        }
        cursor=strchr(jump,'\n')+1;
    }
}
static XrLirSliceCleanupMode cleanup_proof_mode(SliceCleanupFixture c) {
    XrLirOwnershipPlan *plan=NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){c.module,NULL,0},&plan,NULL));
    bool found=false;uint32_t formal=0;XrLirSliceCleanupMode mode=XR_LIR_SLICE_CLEANUP_CLEAR;
    for (uint32_t n=0;n<xr_lir_ownership_fact_count(plan);++n) {
        XrLirOwnershipFact fact;OK(xr_lir_ownership_fact_at(plan,n,&fact));
        if (fact.view_operation==XR_LIR_SLICE_OP_FORMAL_CLEANUP) {
            CHECK(fact.function==c.reader.id && fact.view_storage==XR_LIR_SLICE_STORAGE_BORROWED_FORMAL &&
                fact.view_cleanup==XR_LIR_SLICE_CLEANUP_CLEAR);++formal;
        }
        if (fact.kind==XR_LIR_OWNERSHIP_VIEW_END && fact.view_operation==XR_LIR_SLICE_OP_CLEANUP &&
            fact.function==c.worker.id && fact.block==c.cleanup.id &&
            fact.instruction==c.cleanup_instruction && fact.value==c.target.id) {
            CHECK(!found);found=true;mode=fact.view_cleanup;
        }
    }
    CHECK(found==(c.cleanup.id!=0) && formal==(c.reader.id?2u:0u));xr_lir_ownership_plan_destroy(plan);return mode;
}
static void slice_cleanup_proofs(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8};
    for (size_t e=0;e<2;++e) for (unsigned shape=0;shape<SLICE_CLEANUP_CASE_COUNT;++shape) {
        SliceCleanupFixture c=slice_cleanup_fixture(elements[e],(SliceCleanupCase)shape,false);
        OK(xr_lir_module_finish(c.module,NULL));
        CHECK(!c.module->functions[c.root.id-1].block_cleanup_rank);
        CHECK((c.module->functions[c.worker.id-1].block_cleanup_rank!=NULL)==(shape!=SLICE_CLEANUP_STRICT_END));
        CHECK(cleanup_proof_mode(c)==(c.unconstructed?XR_LIR_SLICE_CLEANUP_UNCONSTRUCTED:XR_LIR_SLICE_CLEANUP_CLEAR));
        if (shape==SLICE_CLEANUP_REACHABLE) {
            const uint32_t *rank=c.module->functions[c.worker.id-1].block_cleanup_rank;
            CHECK(rank[c.definition.id-1]>rank[c.cleanup.id-1]);
        }
        if (shape==SLICE_CLEANUP_LOOP) CHECK(c.module->functions[c.worker.id-1].block_multiplicity[c.definition.id-1]);
        if (shape==SLICE_CLEANUP_BRANCH) {
            XrLirFunctionData *f=&c.module->functions[c.worker.id-1];
            CHECK(f->block_cleanup_rank[c.definition.id-1]<f->block_cleanup_rank[c.cleanup.id-1]);
            uint32_t saved_instruction=f->values[c.target.id-1].instruction;
            f->values[c.target.id-1].instruction=UINT32_MAX;
            XrLirOwnershipPlan *invalid=NULL;
            CHECK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){c.module,NULL,0},&invalid,NULL)!=XR_LIR_OK && !invalid);
            f->values[c.target.id-1].instruction=saved_instruction;
            XrLirBlockData *block=&f->blocks[c.definition.id-1];
            XrLirEdgeView *edge=&block->instructions[block->instruction_count-1].view.edges[0],saved=*edge;
            edge->target=c.cleanup.id;edge->argument_count=0;edge->arguments=NULL;
            /* A valid new path contradicts the old immutable cache. Public
             * collection must use its freshly verified component order. */
            OK(xr_lir_verify(c.module,NULL));
            CHECK(cleanup_proof_mode(c)==XR_LIR_SLICE_CLEANUP_CLEAR);
            *edge=saved;
        }
        xr_lir_module_destroy(c.module);
    }
}
static void array_c_output(const char *name, const char *path) {
    XrLirModule *m = NULL;
    XrLirFunction root = {0};
    if (!strncmp(name,"slice-callback-",15)) {
        SliceCallbackLoanFixture c=slice_callback_loan_fixture(strstr(name,"-u8")?XR_LIR_U8:XR_LIR_I64,
            strstr(name,"-indirect-")?SLICE_CALLBACK_LOAN_INDIRECT:SLICE_CALLBACK_LOAN_DIRECT);
        m=c.module;root=c.root;OK(xr_lir_module_finish(m,NULL));
    }
    else if (!strncmp(name,"slice-proof-",12)) {
        SliceCleanupCase shape=strstr(name,"-bind-")?SLICE_CLEANUP_BIND:
            strstr(name,"-call-before-")?SLICE_CLEANUP_CALL_BEFORE:
            strstr(name,"-call-after-")?SLICE_CLEANUP_CALL_AFTER:
            strstr(name,"-loop-")?SLICE_CLEANUP_LOOP:SLICE_CLEANUP_BRANCH;
        SliceCleanupFixture c=slice_cleanup_fixture(strstr(name,"-u8")?XR_LIR_U8:XR_LIR_I64,
            shape,strstr(name,"-live-")!=NULL);
        m=c.module;root=c.root;OK(xr_lir_module_finish(m,NULL));
    }
    else if (!strcmp(name, "function-reference")) callable_fixture(&m, &root);
    else if (!strcmp(name, "function-frame")) callable_fixture_capture(&m, &root, true);
    else if (!strcmp(name, "function-owned")) callable_fixture_capture(&m, &root, 3);
    else if (!strcmp(name, "function-owned-return")) callable_factory_fixture(&m, &root, false);
    else if (!strcmp(name, "function-scoped-owned-return")) callable_factory_fixture_mode(&m, &root, false, true);
    else if (!strcmp(name,"callable-weaken")) callable_direct_weak_fixture(&m,&root);
    else if (!strcmp(name,"callable-nonentry-frame") || !strcmp(name,"callable-loop-before-frame")) {
        XrLirValue reference = {0};
        callable_construction_fixture(!strcmp(name,"callable-loop-before-frame") ? 1u : 0u,&m,&root,&reference);
    }
    else if (!strcmp(name,"set-i64-local") || !strcmp(name,"set-i64-state")) {
        SetLirFixture f=set_lir_fixture(!strcmp(name,"set-i64-state"));
        m=f.module;root=f.entry;OK(xr_lir_module_finish(m,NULL));
    }
    else if (!strcmp(name,"set-header-copy") || !strcmp(name,"set-header-duplicate")) {
        SetLirFixture f=set_header_edge_fixture(!strcmp(name,"set-header-duplicate")?1u:0u);
        m=f.module;root=f.entry;OK(xr_lir_module_finish(m,NULL));
    }
    else if (!strcmp(name,"set-block-cell-loop")) {
        SetLirFixture f=set_block_cell_fixture(0);m=f.module;root=f.entry;OK(xr_lir_module_finish(m,NULL));
    }
    else if (!strncmp(name,"set-formal-",11)) {
        unsigned mode=!strcmp(name,"set-formal-cell")?0u:!strcmp(name,"set-formal-ref")?1u:2u;
        SetLirFixture f=set_binding_formal_fixture(mode);m=f.module;root=f.entry;OK(xr_lir_module_finish(m,NULL));
    }
    else if (!strcmp(name,"frame-scalar-cells")) {
        FrameScalarCellFixture f=frame_scalar_cell_fixture(FRAME_SCALAR_CELL_VALID);
        m=f.module;root=f.root;OK(xr_lir_module_finish(m,NULL));
    }
    else if (!strcmp(name,"cell-i64-frame")) callable_cell_c_fixture(&m,&root,false,false);
    else if (!strcmp(name,"cell-i64-owned")) callable_cell_c_fixture(&m,&root,false,true);
    else if (!strcmp(name,"cell-map-frame")) callable_cell_c_fixture(&m,&root,true,false);
    else if (!strcmp(name,"cell-map-owned")) callable_cell_c_fixture(&m,&root,true,true);
    else if (!strncmp(name,"map-literal-",12)) {
        unsigned mode = !strcmp(name,"map-literal-spread") ? 1u : !strcmp(name,"map-literal-empty") ? 2u :
            !strcmp(name,"map-literal-duplicate") ? 3u : !strcmp(name,"map-literal-unknown-duplicate") ? 4u :
            !strcmp(name,"map-literal-spread-wins") ? 5u : 0u;
        c_map_literal_fixture(mode,&m,&root);
    }
    else if (!strncmp(name,"map-formal-",11)) {
        unsigned operation=!strcmp(name,"map-formal-write") ? 1u : !strcmp(name,"map-formal-ref") ? 2u :
            !strcmp(name,"map-formal-state") ? 3u : !strcmp(name,"map-formal-construct") ? 4u :
            !strcmp(name,"map-formal-initialize") ? 5u : 0u;
        XrLirFunction helper={0}; m=map_binding_formal_fixture(operation,false,!strcmp(name,"map-formal-closure"),&root,&helper);
    }
    else if (!strcmp(name,"string-bindings")) cs_binding_fixture(&m,&root);
    else if (!strcmp(name,"string-ref-bindings")) {
        OK(string_ref_fixture(&m)); OK(xr_lir_function_at(m,0,&root));
    }
    else if (!strcmp(name,"slice-boundary-i64") || !strcmp(name,"slice-boundary-u8")) {
        SliceBoundaryFixture fixture = slice_boundary_root_fixture(
            !strcmp(name,"slice-boundary-u8") ? XR_LIR_U8 : XR_LIR_I64, &root);
        m = fixture.module; OK(xr_lir_module_finish(m, NULL));
    }
    else if (!strncmp(name,"slice-state-",12)) {
        XrLirType element = strstr(name,"-u8") ? XR_LIR_U8 : XR_LIR_I64;
        bool mutable = strstr(name,"-mutable-") != NULL;
        SliceStateReadFixture fixture = strstr(name,"-initializing-") ?
            slice_state_read_fixture_phase(element,mutable,SLICE_STATE_INITIALIZING) :
            slice_state_read_fixture(element,mutable,strstr(name,"-unready-") == NULL);
        m = fixture.module; root = fixture.root; OK(xr_lir_module_finish(m, NULL));
    }
    else array_c_fixture(name, &m, &root);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=m, .entry=root, .resources=resources, .translation_unit=path ? path : "string-bindings.c"};
    XrLirCOutput output = {0};
    XrLirCDiagnostic diagnostic = {0};
    XrLirCStatus status = xr_lir_emit_c(&request, &output, &diagnostic);
    if (status != XR_LIR_C_OK) fprintf(stderr, "C emission status=%u %s\n", (unsigned)status, diagnostic.message);
    CHECK(status == XR_LIR_C_OK && output.bytes);
    if (!strcmp(name,"set-header-copy") || !strcmp(name,"set-header-duplicate")) {
        const char *pin="if (!xrt_managed_retain(incoming))",*cursor=output.bytes,*first=NULL;
        size_t count=0;
        while ((cursor=strstr(cursor,pin))!=NULL) {if (!first) first=cursor;++count;cursor+=strlen(pin);}
        CHECK(count==(!strcmp(name,"set-header-duplicate")?2u:1u));
        CHECK(first && strstr(output.bytes,"borrowed_0"));
        const char *publication=strstr(first,"status = xrt_place_edge_transfer(");CHECK(publication);
        if (count==2) {const char *second=strstr(first+strlen(pin),pin);CHECK(second && second<publication);}
        CHECK(strstr(publication,"xrl_borrow_failed:") && strstr(publication,"xrt_managed_slot_drop(frame,"));
    }
    if (!strcmp(name, "state-layout")) {
        XrRbcProgram *program = NULL;
        CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_OK && program);
        xr_rbc_program_destroy(program);
    }
    if (!strcmp(name,"string-bindings") || !strcmp(name,"string-ref-bindings")) {
        CHECK(strstr(output.bytes,"xrt_native_string_binding_prepare(") && strstr(output.bytes,"xrt_native_string_append_place(") &&
            strstr(output.bytes,"XrtPlace"));
        cs_binding_setup_scope(output.bytes);
    }
    if (!strcmp(name,"string-bindings")) {
        CHECK(strstr(output.bytes,"xrt_place_move_replace("));
        const char *borrowed=strstr(output.bytes,"static void xrl_f3(XrtPlace p0) {\n"); CHECK(borrowed);
        const char *end=strstr(borrowed,"\n}\n"),*frame=strstr(borrowed,"xrl_frame"); CHECK(end);
        CHECK(!frame || frame>end);
        const char *prepare=strstr(borrowed,"xrt_native_string_binding_prepare("); CHECK(!prepare || prepare>end);
    }
    if (!strcmp(name,"i64") || !strcmp(name,"string"))
        CHECK(!strstr(output.bytes,"xrt_native_string_binding_prepare("));
    if (!strncmp(name,"map-formal-",11)) {
        CHECK(!strstr(output.bytes,"xrt_native_string_binding_prepare("));
        if (strcmp(name,"map-formal-construct") && strcmp(name,"map-formal-initialize"))
            CHECK(strstr(output.bytes,"xrt_managed_slot_adopt(") && strstr(output.bytes,"xrt_place_move_replace("));
    }
    if (!strncmp(name,"slice-boundary-",15)) {
        slice_c_reader_has_no_owners(output.bytes);
        CHECK(strstr(output.bytes,"xrt_array_slice_clear(xrl_frame, (unsigned char *)xrl_payload + ") &&
            !strstr(output.bytes,"xrt_array_slice_clear(&(XrtInlineValueHome)"));
    }
    if (!strncmp(name,"slice-proof-",12)) {
        size_t clear=0;const char *at=output.bytes;
        while ((at=strstr(at,"xrt_array_slice_clear("))!=NULL) {++clear;++at;}
        CHECK(clear==((strstr(name,"-loop-") || strstr(name,"-bind-") || strstr(name,"-call-"))?2u:1u));
    }
    if (!strncmp(name,"scalar-edge-",12))
        scalar_edge_structure(output.bytes,m,root,strcmp(name,"scalar-edge-swap")!=0);
    if (path) {
        FILE *file = fopen(path, "wb");
        CHECK(file && fwrite(output.bytes, 1, output.length, file) == output.length && !fclose(file));
    }
    xr_lir_c_output_free(&output);
    xr_lir_module_destroy(m); xr_compile_resources_release(resources);
}

typedef struct ArrayVmResult { XrtManagedHeader *array; bool string; int64_t value; XrLirType element; } ArrayVmResult;
static void array_vm_result(void *context) {
    ArrayVmResult *result = context;
    XrtArrayItem item = {0};
    XrtBackingArrayKind kind = result->string ? XRT_BACKING_ARRAY_STRING :
        result->element == XR_LIR_U8 ? XRT_BACKING_ARRAY_U8 :
        result->element == XR_LIR_U64 ? XRT_BACKING_ARRAY_U64 : XRT_BACKING_ARRAY_I64;
    xrt_native_array_get(result->array, kind, 0, &item, 0);
    result->value = (int64_t)((const XrtArrayBacking *)result->array)->length;
    if (result->string) result->value += (int64_t)item.value.string->runes;
    else if (result->element == XR_LIR_U8) result->value += item.value.u8;
    else if (result->element == XR_LIR_U64) {
        int64_t value; memcpy(&value, &item.value.u64, sizeof(value)); result->value += value;
    } else result->value += item.value.i64;
}
static void array_vm_fixture(const char *name,int64_t expected) {
    XrLirModule *m=NULL;XrLirFunction root={0};
    array_c_fixture(name,&m,&root);
    XrRbcProgram *program=NULL;XrRbcCompileDiagnostic diagnostic={0};
    XrRbcCompileStatus status=xr_rbc_compile(m,root,&program,&diagnostic);
    if(status!=XR_RBC_COMPILE_OK)fprintf(stderr,"Array VM %s compile=%u %s\n",name,(unsigned)status,diagnostic.message);
    CHECK(status==XR_RBC_COMPILE_OK && program);
    xr_lir_module_destroy(m);
    if(!strncmp(name,"array-spills",12) || !strncmp(name,"unsigned-spills",15)) {
        const XrRbcModule *view=xr_rbc_program_view(program);
        const XrRbcValueCode *code = xr_rbc_module_value_code(view, 0);
        CHECK(code && code->homes->owned_frame_needed && code->homes->frame.slot_count >= 300);
    }
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);xr_rbc_program_destroy(program);
    if(!strncmp(name,"repeat-negative",15)) {
        XrXirValueStatus failure=XR_XIR_VALUE_OK;uint32_t source=0;const XrtLocationOwner *locations=NULL;
        CHECK(xr_rbc_outcome_value_failure(outcome,&failure,&source,&locations));
        const XrtSourceLocation *point=xrt_locations_get(locations,source);
        CHECK(failure==XR_XIR_VALUE_BAD_ARGUMENT && point && point->line==3 && point->column==1);
    } else if(!strncmp(name,"state-uninitialized",19)) {
        XrXirValueStatus failure=XR_XIR_VALUE_OK;uint32_t source=0;const XrtLocationOwner *locations=NULL;
        CHECK(xr_rbc_outcome_value_failure(outcome,&failure,&source,&locations));
        const XrtSourceLocation *point=xrt_locations_get(locations,source);
        CHECK(failure==XR_XIR_VALUE_BAD_ARGUMENT && point && point->line==(!strcmp(name,"state-uninitialized-read")?37u:41u));
    } else if(!strncmp(name,"return-",7)) {
        ArrayVmResult result={.string=strstr(name,"string")!=NULL,
            .element=strstr(name,"u64")?XR_LIR_U64:strstr(name,"u8")?XR_LIR_U8:XR_LIR_I64};
        CHECK(xr_rbc_outcome_take_array(outcome,&result.array));
        xr_rbc_outcome_free(&outcome);
        XrtSourceMap map={0};xrt_run_standalone(&map,array_vm_result,&result);
        CHECK(result.value==expected);xrt_managed_drop(result.array);
    } else {
        uint64_t result=0;
        CHECK(xr_rbc_outcome_bits(outcome,&result) && result==(uint64_t)expected);
    }
    xr_rbc_outcome_free(&outcome);
}

static void frame_scalar_cell_admission(void) {
    for (unsigned kind=FRAME_SCALAR_CELL_VALID;kind<=FRAME_SCALAR_CELL_UNINITIALIZED;++kind) {
        FrameScalarCellFixture f=frame_scalar_cell_fixture((FrameScalarCellCase)kind);
        bool repeat=true;
        CHECK(xr_lir_block_may_repeat(f.module,f.left,&repeat)==XR_LIR_INVALID_STATE && repeat);
        XrLirDiagnostic diagnostic={0};XrLirStatus status=xr_lir_module_finish(f.module,&diagnostic);
        if (kind!=FRAME_SCALAR_CELL_VALID) {
            CHECK(status==XR_LIR_VERIFY_ERROR);
            CHECK(strstr(diagnostic.message,kind==FRAME_SCALAR_CELL_LOOP?"may repeat":"does not dominate"));
            CHECK(xr_lir_block_may_repeat(f.module,f.left,&repeat)!=XR_LIR_OK && repeat);
        } else {
            OK(status);XrLirBlock blocks[]={f.entry,f.left,f.right,f.join};
            for (size_t b=0;b<sizeof(blocks)/sizeof(*blocks);++b) {
                repeat=true;OK(xr_lir_block_may_repeat(f.module,blocks[b],&repeat));CHECK(!repeat);
            }
            XrLirModule *other=NULL;OK(xr_lir_module_create(NULL,&other));repeat=true;
            CHECK(xr_lir_block_may_repeat(other,f.left,&repeat)==XR_LIR_INVALID_ARGUMENT && repeat);
            xr_lir_module_destroy(other);
            /* Independent verification must read the changed CFG, even when
             * this deliberately corrupted internal owner has an old cache. */
            XrLirFunctionData *function=&f.module->functions[f.choose.id-1];
            XrLirBlockData *left=&function->blocks[f.left.id-1];
            XrLirEdgeView *edge=&left->instructions[left->instruction_count-1].view.edges[0];
            XrLirEdgeView original=*edge;
            edge->target=f.left.id;edge->argument_count=0;edge->arguments=NULL;
            CHECK(xr_lir_verify(f.module,&diagnostic)==XR_LIR_VERIFY_ERROR);
            CHECK(strstr(diagnostic.message,"may repeat"));
            *edge=original;
        }
        xr_lir_module_destroy(f.module);
    }
    CArrayFixture c={0};OK(xr_lir_module_create(NULL,&c.module));XrLirFunction root={0};
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="cycle",.name_length=5,.result=XR_LIR_I64},&root));
    OK(xr_lir_define_function(c.module,root));XrLirBlock entry={0},loop={0},exit_block={0};
    OK(xr_lir_add_block(c.module,root,NULL,0,&entry));OK(xr_lir_set_entry(c.module,root,entry));
    OK(xr_lir_add_block(c.module,root,NULL,0,&loop));OK(xr_lir_add_block(c.module,root,NULL,0,&exit_block));
    OK(xr_lir_set_terminator(c.module,entry,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{loop,NULL,0}}}));XrLirValue flag={0};
    OK(xr_lir_emit_value(c.module,loop,&(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1},&flag));
    OK(xr_lir_set_terminator(c.module,loop,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&flag,.operand_count=1,.edge_count=2,.edges={{loop,NULL,0},{exit_block,NULL,0}}}));
    ca_return(&c,exit_block,ca_constant(&c,exit_block,42));OK(xr_lir_module_finish(c.module,NULL));bool repeat=false;
    OK(xr_lir_block_may_repeat(c.module,loop,&repeat));CHECK(repeat);xr_lir_module_destroy(c.module);
}

int main(int argc, char **argv) {
    if (argc == 3) { array_c_output(argv[1], argv[2]); return 0; }
    frame_scalar_cell_admission();array_c_output("frame-scalar-cells",NULL);
    static const char *const set_cases[]={"set-i64-local","set-i64-state","set-formal-cell","set-formal-ref","set-formal-state",
        "set-header-copy","set-header-duplicate","set-block-cell-loop"};
    for (size_t n=0;n<sizeof(set_cases)/sizeof(*set_cases);++n) array_c_output(set_cases[n],NULL);
    CHECK(argc == 1);
    array_c_output("string-bindings",NULL);
    array_c_output("string-ref-bindings",NULL);
    slice_c_raw_entry_rejection(XR_LIR_I64); slice_c_raw_entry_rejection(XR_LIR_U8);
    slice_c_unfinished_result_rejection(XR_LIR_TYPE_INVALID);
    slice_c_unfinished_result_rejection(XR_LIR_I64); slice_c_unfinished_result_rejection(XR_LIR_U8);
    array_c_output("slice-boundary-i64",NULL); array_c_output("slice-boundary-u8",NULL);
    static const char *const state_slices[]={"slice-state-const-i64","slice-state-const-u8",
        "slice-state-mutable-i64","slice-state-mutable-u8","slice-state-unready-const-i64",
        "slice-state-unready-const-u8","slice-state-unready-mutable-i64","slice-state-unready-mutable-u8",
        "slice-state-initializing-const-i64","slice-state-initializing-const-u8",
        "slice-state-initializing-mutable-i64","slice-state-initializing-mutable-u8"};
    for (size_t n=0;n<sizeof(state_slices)/sizeof(*state_slices);++n) array_c_output(state_slices[n],NULL);
    static const char *const map_formals[]={"map-formal-new","map-formal-closure","map-formal-write","map-formal-ref",
        "map-formal-state","map-formal-construct","map-formal-initialize"};
    for (size_t n=0;n<sizeof(map_formals)/sizeof(*map_formals);++n) array_c_output(map_formals[n],NULL);
    array_c_output("scalar-edge-swap",NULL);
    array_c_output("scalar-edge-mixed",NULL);
    array_c_output("scalar-edge-pod",NULL);
    slice_cleanup_proofs();
    static const char *const callback_products[]={"slice-callback-direct-i64","slice-callback-direct-u8",
        "slice-callback-indirect-i64","slice-callback-indirect-u8"};
    for (size_t n=0;n<sizeof(callback_products)/sizeof(*callback_products);++n) array_c_output(callback_products[n],NULL);
    static const char *const cleanup_products[]={"slice-proof-branch-i64","slice-proof-branch-u8",
        "slice-proof-live-i64","slice-proof-live-u8","slice-proof-bind-i64","slice-proof-bind-u8",
        "slice-proof-loop-i64","slice-proof-loop-u8","slice-proof-call-before-i64","slice-proof-call-before-u8",
        "slice-proof-call-after-i64","slice-proof-call-after-u8"};
    for (size_t n=0;n<sizeof(cleanup_products)/sizeof(*cleanup_products);++n) array_c_output(cleanup_products[n],NULL);
    map_receiving_owners();
    map_state_receiver();
    tuple_receiving_owners();
    tuple_group_admission();
    callable_owned_factory_cases();
    captured_cell_frame_extent();
    for (unsigned shape = 0; shape < 4; ++shape) {
        XrLirModule *cell_module = NULL; XrLirFunction cell_root = {0};
        callable_cell_c_fixture(&cell_module,&cell_root,(shape & 1) != 0,(shape & 2) != 0);
        callable_c_supported(cell_module,cell_root,(shape & 2) != 0); xr_lir_module_destroy(cell_module);
    }
    callable_owned_edge_binding();
    view_receiving_owners();
    callable_backend_frontier();
    callable_vm_execution();
    callable_composition_receiving();
    constructed_backend_frontiers();
    fixed_bytecode_execution();
    fixed_bytecode_selected_edges();
    fixed_bytecode_state();
    fixed_bytecode_calls();
    array_catalog();
    array_forwarding_bytecode_cases();
    array_local_bytecode_cases();
    array_reference_owners();
    array_unsigned_frontier();
    array_unsigned_spans();
    array_unsigned_scalar_boundaries();
    array_repeat_owners();
    array_receiving_owners(XR_LIR_I64);
    array_receiving_owners(XR_LIR_STRING);
    array_receiving_owners(XR_LIR_U8);
    array_receiving_owners(XR_LIR_U64);
    array_corruptions();
    array_missing_release();
    array_shape_rejections();
    array_cell_loop();
    array_state_shapes(XR_LIR_I64);
    array_state_shapes(XR_LIR_U8);
    array_state_shapes(XR_LIR_U64);
    const char *cases[]={"i64","string","large","state-i64","state-string","state-layout",
        "return-i64","return-string","array-spills","array-spills-string","phi-i64","phi-string",
        "state-uninitialized-read","state-uninitialized-write","repeat-i64","repeat-string",
        "repeat-empty-i64","repeat-empty-string","repeat-negative-i64","repeat-negative-string",
        "u8","u64","phi-u8","phi-u64","state-u8","state-u64","return-u8","return-u64",
        "unsigned-spills-u8","unsigned-spills-u64","scalar-edge-swap"};
    const int64_t expected[]={1202,511,1500,1202,511,9,13,7,44850,1500,1202,511,0,0,19,10,0,0,0,0,
        264,8,262,6,264,8,258,2,33586,-45150,2011};
    for(size_t c=0;c<sizeof(cases)/sizeof(*cases);++c)array_vm_fixture(cases[c],expected[c]);
    puts("Closed Array shapes, dynamic constructors and receiving owners passed");
    return 0;
}
