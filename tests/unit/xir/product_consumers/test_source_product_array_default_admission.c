/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_array_default_admission.c - Typed Array default expansion and owners
 *
 * KEY CONCEPT:
 *   Repetition consumes an i64 count and an exactly typed zero-fill value.
 *   Borrowed input death precedes public revalidation and independent packet comparison.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "array_default_cases.inc.c"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_BOOL == 1 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 &&
    XR_XIR_U8 == 8 && XR_XIR_TYPE_ARRAY == 2 && XR_XIR_CONST_INT == 2 && XR_XIR_ARRAY_REPEAT == 133 &&
    XR_XIR_ARRAY_LEN == 77 && XR_XIR_RETURN == 33, "Independent Array default identities");
_Static_assert(sizeof(array_default_cases)/sizeof(array_default_cases[0]) == 10, "Complete default admission matrix");

static void exact_owner(const XrXirArtifact *owner) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->stage == XR_XIR_CHECKED && m->function_count == 1 && m->types && m->types->count == 1);
    CHECK(!m->declarations && !m->generics && !m->types->nominals && !m->types->interfaces);
    const XrXirTypeNode *node = &m->types->nodes[0];
    CHECK(node->kind == XR_XIR_TYPE_ARRAY && node->element == XR_XIR_U8 && !node->parameter_span &&
        !node->parameters && !node->parameter_count && !node->result && !node->flags &&
        !node->nominal.declaration && !node->nominal.arguments && !node->nominal.argument_count &&
        !node->nominal.fields && !node->nominal.field_count);
    CHECK(xr_xir_type_is_owned(m->types, 256) && xr_xir_array_element(m->types, 256) == XR_XIR_U8);
    const XrXirFunction *f = &m->functions[0];
    CHECK(f->name_length == 13 && !memcmp(f->name, "default_count", 13));
    CHECK(f->parameter_count == 1 && f->parameters[0] == XR_XIR_I64 && f->result == XR_XIR_I64);
    CHECK(f->block_count == 1 && !f->blocks[0].first && f->blocks[0].count == 4 &&
        !f->blocks[0].panic && !f->blocks[0].frontier && f->instruction_count == 4 &&
        !f->operands && !f->operand_count);
    const XrXirInstruction expected[4] = {{.op = XR_XIR_CONST_INT, .type = XR_XIR_U8},
        {.op = XR_XIR_ARRAY_REPEAT, .type = 256, .args = {0, 1}},
        {.op = XR_XIR_ARRAY_LEN, .type = XR_XIR_I64, .args = {2, 0}},
        {.op = XR_XIR_RETURN, .args = {3, 0}}};
    for (unsigned i = 0; i < 4; ++i) {
        const XrXirInstruction *actual = &f->instructions[i], *want = &expected[i];
        CHECK(actual->op == want->op && actual->type == want->type && actual->args[0] == want->args[0] &&
            actual->args[1] == want->args[1] && !actual->targets[0] && !actual->targets[1] &&
            !actual->immediate && !actual->type_arguments[0] && !actual->type_arguments[1]);
    }
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == array_default_cases[0].length && !memcmp(packet.bytes, array_default_cases[0].bytes, packet.length));
    xr_xir_compile_checked_packet_free(&packet);
}

static XrXirStatus build_case(const XrXirCompileContext *context, unsigned mutation,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    char name[] = "default_count"; XrXirType parameter = mutation == 1 ? XR_XIR_BOOL : XR_XIR_I64;
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_ARRAY, .element = mutation == 2 ? XR_XIR_STRING : XR_XIR_U8};
    XrXirTypes types = {.nodes = &node, .count = 1}; XrXirBlock block = {.count = 4};
    XrXirInstruction instructions[4] = {{.op = XR_XIR_CONST_INT, .type = XR_XIR_U8},
        {.op = XR_XIR_ARRAY_REPEAT, .type = mutation == 3 ? XR_XIR_I64 : 256, .args = {0, 1}},
        {.op = XR_XIR_ARRAY_LEN, .type = mutation == 9 ? XR_XIR_BOOL : XR_XIR_I64, .args = {2, 0}},
        {.op = XR_XIR_RETURN, .args = {3, 0}}};
    switch (mutation) {
    case 0: case 1: case 2: case 3: case 9: break;
    case 4: instructions[1].args[0] = UINT32_MAX; break;
    case 5: instructions[1].args[1] = UINT32_MAX; break;
    case 6: instructions[1].immediate = 1; break;
    case 7: instructions[1].targets[0] = 1; break;
    case 8: instructions[1].type_arguments[0] = 1; break;
    default: CHECK(false); break;
    }
    XrXirFunction function = {.name = name, .name_length = 13, .parameters = &parameter,
        .parameter_count = 1, .result = XR_XIR_I64, .blocks = &block, .block_count = 1,
        .instructions = instructions, .instruction_count = 4};
    XrXirModule built = {.stage = XR_XIR_BUILT, .functions = &function, .function_count = 1, .types = &types};
    XrXirInstruction saved[4]; memcpy(saved, instructions, sizeof(saved));
    XrXirTypeNode saved_node = node; XrXirTypes saved_types = types; XrXirBlock saved_block = block;
    XrXirFunction saved_function = function; XrXirModule saved_module = built; XrXirType saved_parameter = parameter;
    XrXirStatus status = xr_xir_compile_check(context, &built, output, diagnostic);
    CHECK(!memcmp(saved, instructions, sizeof(saved)) && !memcmp(&saved_node, &node, sizeof(node)) &&
        !memcmp(&saved_types, &types, sizeof(types)) && !memcmp(&saved_block, &block, sizeof(block)) &&
        !memcmp(&saved_function, &function, sizeof(function)) && !memcmp(&saved_module, &built, sizeof(built)) &&
        saved_parameter == parameter && !memcmp(name, "default_count", sizeof(name)));
    memset(instructions, 0xa5, sizeof(instructions)); memset(name, 0xa5, sizeof(name));
    memset(&node, 0xa5, sizeof(node)); memset(&types, 0xa5, sizeof(types)); memset(&block, 0xa5, sizeof(block));
    memset(&function, 0xa5, sizeof(function)); memset(&built, 0xa5, sizeof(built)); parameter = XR_XIR_UNIT;
    return status;
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL; CHECK(build_case(context, 0, &owner, NULL) == XR_XIR_OK); exact_owner(owner);
    unsigned transitions = 0, mismatches = 0;
    for (size_t c = 0; c < 10; ++c) for (unsigned wire = 0; wire < 2; ++wire) for (unsigned occupied = 0; occupied < 2; ++occupied) {
        const ArrayDefaultCase *test = &array_default_cases[c];
        size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
        XrXirArtifact *output = occupied ? owner : NULL, *before = output; XrXirDiagnostic diagnostic = {0}; XrXirStatus status;
        if (wire) {
            uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
            status = xr_xir_compile_checked_read(context, input, test->length, &output, &diagnostic);
            CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
        } else status = build_case(context, test->mutation, &output, &diagnostic);
        if (status == XR_XIR_OK) { CHECK(output && output != owner); exact_owner(output); xr_xir_compile_artifact_free(output); }
        else CHECK(output == before && diagnostic.status == status);
        CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes); exact_owner(owner);
        bool match = status == test->expected; mismatches += match ? 0u : 1u; ++transitions;
        printf("array-default case=%s wire=%u occupied=%u expected=%u actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
            test->name, wire, occupied, test->expected, status, match ? "PASS" : "FAIL");
    }
    xr_xir_compile_artifact_free(owner); source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity && transitions == 40);
    printf("array-default instructions=4 transitions=%u mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        transitions, mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
