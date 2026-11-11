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
#include "vm/register/xrbc_lir.h"
#include "base/xmalloc.h"
#include "lir_array_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)
#include "lir_array_c_fixture.h"

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
    XrLirIntrinsic extra = {1, &scalar, 1, scalar, true};
    XrLirRegistry registry = {&extra, 1};
    XrLirModule *m = NULL;
    OK(xr_lir_array_module_create(&registry, &m));
    for (uint32_t family = 0; family < 2; ++family) {
        XrLirType element = family ? XR_LIR_STRING : XR_LIR_I64;
        for (uint32_t op = XR_LIR_ARRAY_OP_NEW; op <= XR_LIR_ARRAY_OP_PUSH; ++op) {
            uint32_t id = 0;
            OK(xr_lir_array_operation(m, element, (XrLirArrayOperation)op, &id));
            CHECK(id && id != extra.id);
            CHECK(id == m->array_operations[family][op - 1]);
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
    xr_lir_module_destroy(copy);
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
    XrLirIntrinsic entry = {17, &scalar, 1, scalar, true};
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
    CHECK(xr_rbc_compile(m, entry, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED);
    CHECK(!program);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=m, .entry=entry, .resources=resources,
        .translation_unit="array-frontier.c"};
    XrLirCOutput output = {0};
    CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_OK);
    CHECK(output.bytes && output.length && strstr(output.bytes, "xrt_array_i64_new(NULL, UINT32_C(0)"));
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

static void array_state_shapes(void) {
    XrLirModule *m = NULL;
    OK(array_fixture_registry(XR_LIR_I64, &m));
    XrLirStateModule module = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_ARRAY, .mutable=true, .size=8, .alignment=8,
        .array_element=XR_LIR_I64};
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
    XrLirValue state, selected, number, array, reference, read;
    OK(xr_lir_function_parameter(m, f, 0, &state));
    XrLirBlock entry, join;
    OK(xr_lir_add_block(m, f, NULL, 0, &entry));
    OK(xr_lir_add_block(m, f, &ref_type, 1, &join));
    OK(xr_lir_set_entry(m, f, entry));
    OK(xr_lir_block_parameter(m, join, 0, &selected));
    XrLirStateReference shape = {.payload=XR_LIR_ARRAY, .mutable=true, .array_element=XR_LIR_I64};
    OK(xr_lir_set_state_reference(m, selected, &shape));
    XrLirInstruction op = {.op=XR_LIR_MODULE_BEGIN, .type=XR_LIR_UNIT, .operands=&state, .operand_count=1};
    OK(xr_lir_emit_effect(m, entry, &op));
    op = (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_I64, .bits=9};
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
    op = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&number, .operand_count=1};
    OK(xr_lir_set_terminator(m, join, &op));
    OK(xr_lir_module_finish(m, NULL));
    XrLirArrayShape array_shape;
    OK(xr_lir_array_shape_view(m, read, &array_shape));
    CHECK(array_shape.element == XR_LIR_I64);
    XrLirModule *copy = NULL, *inlined = NULL;
    OK(xr_lir_rebuild(m, &copy));
    xr_lir_module_destroy(m);
    OK(xr_lir_inline(copy, &inlined));
    xr_lir_module_destroy(copy);
    OK(xr_lir_state_reference_view(inlined, (XrLirValue){inlined, 1, selected.id}, &shape));
    CHECK(shape.array_element == XR_LIR_I64 && shape.payload == XR_LIR_ARRAY);
    inlined->functions[0].values[selected.id - 1].state_reference.array_element = XR_LIR_STRING;
    CHECK(xr_lir_verify(inlined, NULL) == XR_LIR_VERIFY_ERROR);
    xr_lir_module_destroy(inlined);
}

static void array_c_output(const char *name, const char *path) {
    XrLirModule *m = NULL;
    XrLirFunction root = {0};
    array_c_fixture(name, &m, &root);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=m, .entry=root, .resources=resources, .translation_unit=path};
    XrLirCOutput output = {0};
    XrLirCDiagnostic diagnostic = {0};
    XrLirCStatus status = xr_lir_emit_c(&request, &output, &diagnostic);
    if (status != XR_LIR_C_OK) fprintf(stderr, "C emission status=%u %s\n", (unsigned)status, diagnostic.message);
    CHECK(status == XR_LIR_C_OK && output.bytes);
    if (!strcmp(name, "state-layout")) {
        CHECK(strstr(output.bytes, "XRT_MODULE_ARRAY_I64") && strstr(output.bytes, "XRT_MODULE_ARRAY_STRING"));
        XrRbcProgram *program = NULL;
        CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED && !program);
    }
    FILE *file = fopen(path, "wb");
    CHECK(file && fwrite(output.bytes, 1, output.length, file) == output.length && !fclose(file));
    xr_lir_c_output_free(&output);
    xr_lir_module_destroy(m); xr_compile_resources_release(resources);
}

int main(int argc, char **argv) {
    if (argc == 3) { array_c_output(argv[1], argv[2]); return 0; }
    CHECK(argc == 1);
    array_catalog();
    array_receiving_owners(XR_LIR_I64);
    array_receiving_owners(XR_LIR_STRING);
    array_corruptions();
    array_missing_release();
    array_shape_rejections();
    array_cell_loop();
    array_state_shapes();
    puts("Closed Array shapes, dynamic constructors and receiving owners passed");
    return 0;
}
