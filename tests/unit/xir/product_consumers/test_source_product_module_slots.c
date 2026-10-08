/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_slots.c - Preserve module-slot metadata and rejection duties
 *
 * KEY CONCEPT:
 *   Slot type, mutability, ownership and dense ordinal expectations are strict.
 *   Unexpected admission is
 *   reported as failure after full owner cleanup, never blessed as a new oracle.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_declarations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "module_slot_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_BOOL == 1 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 &&
    XR_XIR_TYPE_TUPLE == 6 && XR_XIR_TYPE_CELL == 3 && XR_XIR_CONSTRUCTED_TYPE_BASE == 256, "Independent slot representation");
_Static_assert(sizeof(module_slot_cases)/sizeof(module_slot_cases[0]) == 22, "Complete byte obligation matrix");

static void exact_owner(const XrXirArtifact *owner, const ModuleSlotCase *test) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->function_count == 5 && m->declarations);
    const XrXirDeclarations *d = m->declarations;
    CHECK(d->module_count == 4 && d->slot_count == test->count && !d->literal_count);
    CHECK(d->root_module == 3 && d->entry_function == 4);
    for (uint32_t i = 0; i < 4; ++i) CHECK(d->modules[i].initializer == i);
    for (uint32_t i = 0; i < 5; ++i) CHECK(d->functions[i].module == (i == 4 ? 3u : i));
    for (uint32_t i = 0; i < test->count; ++i) {
        CHECK(d->slots[i].module == test->slots[i][0]);
        CHECK((uint32_t)d->slots[i].type == test->slots[i][1]);
        CHECK(d->slots[i].mutable == test->slots[i][2]);
    }
    if (test->type_kind) {
        CHECK(m->types && m->types->count == 1 && m->types->nodes[0].kind == test->type_kind);
        CHECK(m->types->nodes[0].parameter_span == 0);
        if (test->type_kind == XR_XIR_TYPE_TUPLE) {
            CHECK(m->types->nodes[0].parameter_count == 1 && m->types->nodes[0].parameters);
            CHECK(m->types->nodes[0].parameters[0].type == XR_XIR_I64 && !m->types->nodes[0].parameters[0].mode);
        }
    } else CHECK(!m->types);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL;
    CHECK(xr_xir_compile_checked_read(context, module_slot_cases[0].bytes,
        module_slot_cases[0].length, &owner, NULL) == XR_XIR_OK);
    exact_owner(owner, &module_slot_cases[0]);
    unsigned mismatches = 0;
    for (size_t c = 0; c < 22; ++c) for (unsigned occupied = 0; occupied < 2; ++occupied) {
        const ModuleSlotCase *test = &module_slot_cases[c];
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
        exact_owner(owner, &module_slot_cases[0]);
        bool match = status == test->expected;
        mismatches += match ? 0u : 1u;
        printf("module-slots case=%s occupied=%u expected=%u actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
            test->name, occupied, test->expected, status, match ? "PASS" : "FAIL");
    }
    xr_xir_compile_artifact_free(owner);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("module-slots reads=44 mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
