/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_sequence_admission.c - Exact String length admission
 *
 * KEY CONCEPT:
 *   Authenticated semantic refusals preserve real occupied owners and allocations.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "sequence_admission_cases.inc.c"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");
_Static_assert(XR_XIR_STRING == 3 && XR_XIR_I64 == 2 && XR_XIR_BOOL == 1 &&
    XR_XIR_STRING_LEN == 86 && XR_XIR_RETURN == 33, "Independent String length identities");
_Static_assert(sizeof(sequence_admission_cases)/sizeof(sequence_admission_cases[0]) == 10, "Complete admission cases");
static void exact_owner(const XrXirArtifact *owner) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->stage == XR_XIR_CHECKED && m->function_count == 1 && !m->types && !m->declarations &&
        !m->generics && !m->provenance && !m->defaults);
    const XrXirFunction *f = &m->functions[0];
    CHECK(f->name_length == 13 && !memcmp(f->name, "string_length", 13));
    CHECK(f->parameter_count == 1 && f->parameters[0] == XR_XIR_STRING && f->result == XR_XIR_I64 &&
        f->block_count == 1 && !f->blocks[0].first && f->blocks[0].count == 2 &&
        !f->blocks[0].panic && !f->blocks[0].frontier && f->instruction_count == 2 && !f->operand_count && !f->operands);
    const XrXirInstruction expected[2] = {{.op=XR_XIR_STRING_LEN,.type=XR_XIR_I64}, {.op=XR_XIR_RETURN,.args={1}}};
    CHECK(!memcmp(f->instructions, expected, sizeof(expected)));
    XrXirCheckedPacket packet = {0}; CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == sequence_admission_cases[0].length && !memcmp(packet.bytes, sequence_admission_cases[0].bytes, packet.length));
    xr_xir_compile_checked_packet_free(&packet);
}
static XrXirStatus build_case(const XrXirCompileContext *context, unsigned c,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    char name[] = "string_length"; XrXirType parameter = XR_XIR_STRING;
    XrXirInstruction ops[2] = {{.op=XR_XIR_STRING_LEN,.type=XR_XIR_I64}, {.op=XR_XIR_RETURN,.args={1}}};
    XrXirBlock block = {.count=2};
    XrXirFunction fn = {.name=name,.name_length=13,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
        .blocks=&block,.block_count=1,.instructions=ops,.instruction_count=2};
    switch (c) {
    case 0: break;
    case 1: parameter = XR_XIR_I64; break;
    case 2: fn.result = ops[0].type = XR_XIR_BOOL; break;
    case 3: ops[0].immediate = 1; break;
    case 4: ops[0].args[0] = UINT32_MAX; break;
    case 5: fn.result = XR_XIR_STRING; break;
    case 6: ops[0].args[1] = 1; break;
    case 7: ops[0].targets[0] = 1; break;
    case 8: ops[0].type_arguments[0] = 1; break;
    case 9: block.panic = 1; break;
    default: CHECK(false); break;
    }
    XrXirModule built = {.stage=XR_XIR_BUILT,.functions=&fn,.function_count=1}, saved_built = built;
    XrXirFunction saved_fn = fn; XrXirBlock saved_block = block; XrXirType saved_parameter = parameter;
    XrXirInstruction saved_ops[2]; memcpy(saved_ops, ops, sizeof(ops));
    XrXirStatus status = xr_xir_compile_check(context, &built, output, diagnostic);
    CHECK(!memcmp(&saved_built, &built, sizeof(built)) && !memcmp(&saved_fn, &fn, sizeof(fn)) &&
        !memcmp(&saved_block, &block, sizeof(block)) && saved_parameter == parameter &&
        !memcmp(saved_ops, ops, sizeof(ops)) && !memcmp(name, "string_length", sizeof(name)));
    memset(name, 0xa5, sizeof(name)); memset(ops, 0xa5, sizeof(ops)); memset(&block, 0xa5, sizeof(block));
    memset(&parameter, 0xa5, sizeof(parameter)); memset(&fn, 0xa5, sizeof(fn)); memset(&built, 0xa5, sizeof(built)); return status;
}
int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL; CHECK(build_case(context, 0, &owner, NULL) == XR_XIR_OK); exact_owner(owner);
    unsigned transitions = 0, mismatches = 0;
    for (unsigned c = 0; c < 10; ++c) for (unsigned wire = 0; wire < 2; ++wire) for (unsigned occupied = 0; occupied < 2; ++occupied) {
        const SequenceAdmissionCase *test = &sequence_admission_cases[c];
        size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
        XrXirArtifact *output = occupied ? owner : NULL, *before = output; XrXirDiagnostic diagnostic = {0}; XrXirStatus status;
        if (wire) {
            uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
            status = xr_xir_compile_checked_read(context, input, test->length, &output, &diagnostic);
            CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
        } else status = build_case(context, c, &output, &diagnostic);
        if (status == XR_XIR_OK) { CHECK(output && output != owner); exact_owner(output); xr_xir_compile_artifact_free(output); }
        else CHECK(output == before && diagnostic.status == status);
        CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes); exact_owner(owner);
        bool match = status == test->expected; mismatches += match ? 0u : 1u; ++transitions;
        printf("sequence-admission case=%s wire=%u occupied=%u expected=%u actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
            test->name, wire, occupied, test->expected, status, match ? "PASS" : "FAIL");
    }
    xr_xir_compile_artifact_free(owner); source_program_owners_free();
    CHECK(!source_program_compile_live && !source_program_compile_bytes && !source_program_compile_allocations && !source_program_compile_capacity && transitions == 40);
    printf("sequence-admission cases=10 transitions=%u mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        transitions, mismatches, mismatches ? "FAIL" : "PASS"); return mismatches ? 1 : 0;
}
