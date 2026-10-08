/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_atomic_types.c - Check Atomic construction, hostile wire and owned snapshots
 *
 * KEY CONCEPT:
 *   Borrowed descriptors and packets die before the owned artifact is inspected.
 *   Repeated type references use one interned descriptor; duplicate records reject.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "atomic_types_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_BOOL == 1 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 &&
    XR_XIR_I8 == 5 && XR_XIR_F32 == 12 && XR_XIR_F64 == 13 && XR_XIR_PANIC_INFO == 15 && XR_XIR_RUNE == 16 &&
    XR_XIR_TYPE_ARRAY == 2 && XR_XIR_TYPE_ATOMIC == 7 && XR_XIR_CONSTRUCTED_TYPE_BASE == 256 &&
    XR_XIR_TYPE_PARAMETER_BASE == 65536 && XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33,
    "Independent Atomic representation");
_Static_assert(sizeof(atomic_types_cases)/sizeof(atomic_types_cases[0]) == 30, "Complete descriptor matrix");

static void exact_owner(const XrXirArtifact *owner, const AtomicTypesCase *test) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->stage == XR_XIR_CHECKED && m->function_count == 2 && m->types && m->types->count == test->node_count);
    CHECK(!m->declarations && !m->types->nominals && !m->types->interfaces);
    const XrXirFunction *f = &m->functions[0];
    CHECK(f->name_length == 4 && !memcmp(f->name, "main", 4) && f->result == XR_XIR_I64);
    CHECK(f->block_count == 1 && f->instruction_count == 2 && f->instructions[0].immediate == 42);
    for (uint32_t i = 0; i < test->node_count; ++i) {
        const XrXirTypeNode *n = &m->types->nodes[i];
        CHECK(n->kind == test->nodes[i][0] && n->parameter_span == test->nodes[i][1]);
        CHECK((uint32_t)n->element == test->nodes[i][2]);
        CHECK(!n->parameters && !n->parameter_count && !n->flags && n->result == XR_XIR_UNIT);
        CHECK(!n->nominal.declaration && !n->nominal.arguments && !n->nominal.argument_count &&
            !n->nominal.fields && !n->nominal.field_count);
        CHECK(xr_xir_atomic_element(m->types, (XrXirType)(256 + i)) == n->element);
        CHECK(xr_xir_type_is_owned(m->types, (XrXirType)(256 + i)));
    }
    const XrXirFunction *handles = &m->functions[1];
    CHECK(handles->name_length == 7 && !memcmp(handles->name, "handles", 7));
    CHECK(handles->parameter_count == 3 && handles->parameters[0] == 256 && handles->parameters[1] == 257);
    CHECK(handles->parameters[2] == handles->parameters[0] && handles->result == XR_XIR_UNIT);
    CHECK(handles->block_count == 1 && handles->instruction_count == 1 && handles->instructions[0].op == XR_XIR_RETURN);
    CHECK(xr_xir_type_is_atomic(m->types, handles->parameters[0]) && xr_xir_type_is_atomic(m->types, handles->parameters[1]));
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
}

static XrXirStatus build_case(const XrXirCompileContext *context, const AtomicTypesCase *test,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    XrXirTypeNode nodes[3] = {0};
    for (unsigned i = 0; i < test->node_count; ++i) {
        nodes[i].kind = test->nodes[i][0]; nodes[i].parameter_span = test->nodes[i][1];
        nodes[i].element = (XrXirType)test->nodes[i][2];
    }
    XrXirType field = XR_XIR_I64;
    XrXirCallableParameter parameter = {XR_XIR_I64, 0};
    switch (test->inactive) {
    case 1: nodes[0].parameters = &parameter; break;
    case 2: nodes[0].parameter_count = 1; break;
    case 3: nodes[0].result = XR_XIR_I64; break;
    case 4: nodes[0].flags = 1; break;
    case 5: nodes[0].nominal.declaration = 1; break;
    case 6: nodes[0].nominal.arguments = &field; break;
    case 7: nodes[0].nominal.argument_count = 1; break;
    case 8: nodes[0].nominal.fields = &field; break;
    case 9: nodes[0].nominal.field_count = 1; break;
    default: CHECK(!test->inactive); break;
    }
    char name[] = "main";
    XrXirInstruction instructions[2] = {{.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 42},
        {.op = XR_XIR_RETURN}};
    XrXirBlock block = {.count = 2};
    char handles_name[] = "handles";
    XrXirType parameters[3] = {256, 257, 256};
    XrXirBlock handles_block = {.count = 1};
    XrXirFunction functions[2] = {{.name = name, .name_length = 4, .result = XR_XIR_I64,
        .blocks = &block, .block_count = 1, .instructions = instructions, .instruction_count = 2},
        {.name = handles_name, .name_length = 7, .parameters = parameters, .parameter_count = 3,
        .result = XR_XIR_UNIT, .blocks = &handles_block, .block_count = 1,
        .instructions = &instructions[1], .instruction_count = 1}};
    XrXirTypes types = {.nodes = nodes, .count = test->node_count};
    XrXirModule built = {.stage = XR_XIR_BUILT, .functions = functions, .function_count = 2, .types = &types};
    XrXirTypeNode before[3]; memcpy(before, nodes, sizeof(nodes));
    XrXirStatus status = xr_xir_compile_check(context, &built, output, diagnostic);
    CHECK(!memcmp(before, nodes, sizeof(nodes)) && !memcmp(name, "main", 4));
    memset(nodes, 0xa5, sizeof(nodes)); memset(instructions, 0xa5, sizeof(instructions));
    memset(name, 0xa5, sizeof(name)); memset(functions, 0xa5, sizeof(functions));
    memset(handles_name, 0xa5, sizeof(handles_name)); memset(parameters, 0xa5, sizeof(parameters));
    memset(&handles_block, 0xa5, sizeof(handles_block));
    memset(&types, 0xa5, sizeof(types)); memset(&block, 0xa5, sizeof(block));
    return status;
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL;
    CHECK(build_case(context, &atomic_types_cases[0], &owner, NULL) == XR_XIR_OK);
    exact_owner(owner, &atomic_types_cases[0]);
    unsigned mismatches = 0, transitions = 0;
    for (size_t c = 0; c < 30; ++c) for (unsigned wire = 0; wire < 2; ++wire) {
        const AtomicTypesCase *test = &atomic_types_cases[c];
        if (wire && !test->wire) continue;
        for (unsigned occupied = 0; occupied < 2; ++occupied) {
            size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
            XrXirArtifact *output = occupied ? owner : NULL, *before = output;
            XrXirDiagnostic diagnostic = {0}; XrXirStatus status;
            if (wire) {
                uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
                status = xr_xir_compile_checked_read(context, input, test->length, &output, &diagnostic);
                CHECK(!memcmp(input, test->bytes, test->length));
                memset(input, 0xa5, test->length); xr_free(input);
            } else status = build_case(context, test, &output, &diagnostic);
            if (status == XR_XIR_OK) {
                CHECK(output && output != owner); exact_owner(output, test); xr_xir_compile_artifact_free(output);
            } else CHECK(output == before && diagnostic.status == status);
            CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes);
            exact_owner(owner, &atomic_types_cases[0]);
            bool match = status == test->expected; mismatches += match ? 0u : 1u; ++transitions;
            printf("atomic-types case=%s wire=%u occupied=%u expected=%u actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
                test->name, wire, occupied, test->expected, status, match ? "PASS" : "FAIL");
        }
    }
    xr_xir_compile_artifact_free(owner); source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity && transitions == 102);
    printf("atomic-types transitions=%u mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        transitions, mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
