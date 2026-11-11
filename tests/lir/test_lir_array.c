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
    for (uint32_t scenario = 0; scenario < 11; ++scenario) {
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
    XrLirIntrinsic entry = {17, &scalar, 1, scalar, true};
    XrLirRegistry registry = {&entry, 1};
    OK(xr_lir_module_create(&registry, &m));
    XrLirType no_shape = XR_LIR_TYPE_INVALID;
    XrLirArrayIntrinsic invalid = {17, &no_shape, 1, XR_LIR_TYPE_INVALID, false};
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
    CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_UNSUPPORTED);
    CHECK(!output.bytes && !output.length);
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

int main(void) {
    array_receiving_owners(XR_LIR_I64);
    array_receiving_owners(XR_LIR_STRING);
    array_corruptions();
    array_missing_release();
    array_shape_rejections();
    array_cell_loop();
    puts("Closed Array shapes, dynamic constructors and receiving owners passed");
    return 0;
}
