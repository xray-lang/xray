/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_operation.c - Check slot operation authority, types and original positive duties
 *
 * KEY CONCEPT:
 *   Read, initialize and store preserve their distinct authority and type rules.
 *   Every expectation mismatch is reported as failure after full owner cleanup, never blessed as a new oracle.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_declarations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "module_operation_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_BOOL == 1 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 &&
    XR_XIR_TYPE_TUPLE == 6 && XR_XIR_TYPE_CELL == 3 && XR_XIR_CONSTRUCTED_TYPE_BASE == 256, "Independent slot representation");
_Static_assert(XR_XIR_SLOT_LOAD == 4 && XR_XIR_SLOT_INIT == 5 && XR_XIR_SLOT_STORE == 6, "Independent slot operations");
_Static_assert(sizeof(module_operation_cases)/sizeof(module_operation_cases[0]) == 20, "Complete byte obligation matrix");

static void exact_owner(const XrXirArtifact *owner, const ModuleOperationCase *test) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->function_count == 5 && m->declarations && !m->types);
    const XrXirDeclarations *d = m->declarations;
    CHECK(d->module_count == 4 && d->slot_count == 1 && !d->literal_count);
    CHECK(d->root_module == 3 && d->entry_function == 4);
    for (uint32_t i = 0; i < 4; ++i) CHECK(d->modules[i].initializer == i);
    CHECK(d->slots[0].module == test->owner && d->slots[0].mutable == test->mutable);
    CHECK((unsigned)d->slots[0].type == test->slot_type);
    const XrXirFunction *f = &m->functions[test->caller];
    CHECK(!f->parameter_count && f->block_count == 1 && f->instruction_count == 3);
    CHECK(f->result == (test->caller == 4 ? XR_XIR_I64 : XR_XIR_UNIT));
    CHECK((unsigned)f->instructions[0].type == test->value_type);
    CHECK(f->instructions[0].op == (test->value_type == 1 ? XR_XIR_CONST_BOOL : XR_XIR_CONST_INT));
    CHECK(f->instructions[0].immediate == (test->value_type == 1 ? 1 : 40));
    CHECK((unsigned)f->instructions[1].op == test->operation && (unsigned)f->instructions[1].type == test->result_type);
    CHECK(!f->instructions[1].args[0] && !f->instructions[1].immediate && f->instructions[2].op == XR_XIR_RETURN);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL;
    CHECK(xr_xir_compile_checked_read(context, module_operation_cases[0].bytes,
        module_operation_cases[0].length, &owner, NULL) == XR_XIR_OK);
    exact_owner(owner, &module_operation_cases[0]);
    unsigned mismatches = 0;
    for (size_t c = 0; c < 20; ++c) for (unsigned occupied = 0; occupied < 2; ++occupied) {
        const ModuleOperationCase *test = &module_operation_cases[c];
        uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
        size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
        XrXirArtifact *output = occupied ? owner : NULL, *before = output;
        XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_checked_read(context, input, test->length, &output, &diagnostic);
        CHECK(!memcmp(input, test->bytes, test->length));
        memset(input, 0xa5, test->length); xr_free(input);
        if (status == XR_XIR_OK) {
            CHECK(output && output != owner);
            exact_owner(output, test);
            xr_xir_compile_artifact_free(output);
        } else CHECK(output == before && diagnostic.status == status);
        CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes);
        exact_owner(owner, &module_operation_cases[0]);
        bool match = status == test->expected;
        mismatches += match ? 0u : 1u;
        printf("module-operation case=%s occupied=%u expected=%u actual=%u function=%u block=%u instruction=%u reason=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
            test->name, occupied, test->expected, status, diagnostic.function, diagnostic.block, diagnostic.instruction, diagnostic.reason, match ? "PASS" : "FAIL");
    }
    xr_xir_compile_artifact_free(owner);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("module-operation reads=40 mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
