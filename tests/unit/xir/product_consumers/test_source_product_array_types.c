/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_array_types.c - Check Array construction, hostile wire and owned snapshots
 *
 * KEY CONCEPT:
 *   Borrowed descriptors and packets die before the owned artifact is inspected.
 *   Legacy semantic disagreements remain strict failures after complete cleanup.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "array_types_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 && XR_XIR_PANIC_INFO == 15 &&
    XR_XIR_TYPE_ARRAY == 2 && XR_XIR_TYPE_CELL == 3 && XR_XIR_CONSTRUCTED_TYPE_BASE == 256 &&
    XR_XIR_TYPE_PARAMETER_BASE == 65536 && XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33,
    "Independent Array representation");
_Static_assert(sizeof(array_types_cases)/sizeof(array_types_cases[0]) == 25, "Complete descriptor matrix");

static void exact_owner(const XrXirArtifact *owner, const ArrayTypesCase *test) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->stage == XR_XIR_CHECKED && m->function_count == 1 && m->types && m->types->count == 3);
    CHECK(!m->declarations && !m->types->nominals && !m->types->interfaces);
    const XrXirFunction *f = &m->functions[0];
    CHECK(f->name_length == 4 && !memcmp(f->name, "main", 4) && f->result == XR_XIR_I64);
    CHECK(f->block_count == 1 && f->instruction_count == 2 && f->instructions[0].immediate == 42);
    for (uint32_t i = 0; i < 3; ++i) {
        const XrXirTypeNode *n = &m->types->nodes[i];
        CHECK(n->kind == test->nodes[i][0] && n->parameter_span == test->nodes[i][1]);
        CHECK((uint32_t)n->element == test->nodes[i][2]);
        CHECK(!n->parameters && !n->parameter_count && !n->flags && n->result == XR_XIR_UNIT);
        CHECK(!n->nominal.declaration && !n->nominal.arguments && !n->nominal.argument_count &&
            !n->nominal.fields && !n->nominal.field_count);
        CHECK(xr_xir_array_element(m->types, (XrXirType)(256 + i)) == n->element);
        CHECK(xr_xir_type_is_owned(m->types, (XrXirType)(256 + i)));
    }
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
}

static XrXirStatus build_case(const XrXirCompileContext *context, const ArrayTypesCase *test,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    XrXirTypeNode nodes[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
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
    XrXirFunction function = {.name = name, .name_length = 4, .result = XR_XIR_I64,
        .blocks = &block, .block_count = 1, .instructions = instructions, .instruction_count = 2};
    XrXirTypes types = {.nodes = nodes, .count = 3};
    XrXirModule built = {.stage = XR_XIR_BUILT, .functions = &function, .function_count = 1, .types = &types};
    XrXirTypeNode before[3]; memcpy(before, nodes, sizeof(nodes));
    XrXirStatus status = xr_xir_compile_check(context, &built, output, diagnostic);
    CHECK(!memcmp(before, nodes, sizeof(nodes)) && !memcmp(name, "main", 4));
    memset(nodes, 0xa5, sizeof(nodes)); memset(instructions, 0xa5, sizeof(instructions));
    memset(name, 0xa5, sizeof(name)); memset(&function, 0xa5, sizeof(function));
    memset(&types, 0xa5, sizeof(types)); memset(&block, 0xa5, sizeof(block));
    return status;
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL;
    CHECK(build_case(context, &array_types_cases[0], &owner, NULL) == XR_XIR_OK);
    exact_owner(owner, &array_types_cases[0]);
    unsigned mismatches = 0, transitions = 0;
    for (size_t c = 0; c < 25; ++c) for (unsigned wire = 0; wire < 2; ++wire) {
        const ArrayTypesCase *test = &array_types_cases[c];
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
            exact_owner(owner, &array_types_cases[0]);
            bool match = status == test->expected; mismatches += match ? 0u : 1u; ++transitions;
            printf("array-types case=%s wire=%u occupied=%u expected=%u actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
                test->name, wire, occupied, test->expected, status, match ? "PASS" : "FAIL");
        }
    }
    xr_xir_compile_artifact_free(owner); source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity && transitions == 82);
    printf("array-types transitions=%u mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        transitions, mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
