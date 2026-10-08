/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_place.c - Check cross-block module places and ordinary value roles
 *
 * KEY CONCEPT:
 *   Module slot permissions survive dominated cross-block projection paths.
 *   Every expectation mismatch is reported as failure after full owner cleanup, never blessed as a new oracle.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_declarations.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "module_place_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_BOOL == 1 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 &&
    XR_XIR_TYPE_TUPLE == 6 && XR_XIR_TYPE_CELL == 3 && XR_XIR_CONSTRUCTED_TYPE_BASE == 256, "Independent slot representation");
_Static_assert(XR_XIR_SLOT_PLACE == 72 && XR_XIR_ARRAY_NEW == 73 && XR_XIR_INDEX_PLACE == 113 &&
    XR_XIR_PLACE_READ == 114 && XR_XIR_PLACE_WRITE == 115 && XR_XIR_PHI == 56 &&
    XR_XIR_COPY == 18 && XR_XIR_CALL == 28 && XR_XIR_JUMP == 31 && XR_XIR_BRANCH == 32 &&
    XR_XIR_RETURN == 33 && XR_XIR_TYPE_ARRAY == 2 && XR_XIR_BAD_DOMINANCE == 5, "Independent place representation");
_Static_assert(sizeof(module_place_cases)/sizeof(module_place_cases[0]) == 26, "Complete byte obligation matrix");

static void exact_owner(const XrXirArtifact *owner, const ModulePlaceCase *test) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->function_count == 7 && m->declarations && m->types);
    CHECK(m->types->count == 2 && xr_xir_array_element(m->types, 256) == XR_XIR_I64);
    CHECK(xr_xir_array_element(m->types, 257) == XR_XIR_BOOL);
    const XrXirDeclarations *d = m->declarations;
    CHECK(d->module_count == 4 && d->slot_count == 1 && !d->literal_count);
    CHECK(d->root_module == 3 && d->entry_function == 4);
    for (uint32_t i = 0; i < 4; ++i) CHECK(d->modules[i].initializer == i);
    CHECK(d->slots[0].module == test->owner && d->slots[0].mutable == test->mutable);
    CHECK((unsigned)d->slots[0].type == test->slot_type && d->functions[5].module == test->caller);
    const XrXirFunction *f = &m->functions[5];
    CHECK(!f->parameter_count && f->block_count == test->blocks && f->instruction_count == test->instructions);
    CHECK((unsigned)f->result == test->result);
    CHECK(m->functions[6].parameter_count == 1 && m->functions[6].parameters[0] == 256);
    unsigned places = 0;
    for (uint32_t i = 0; i < f->instruction_count; ++i) if (f->instructions[i].op == XR_XIR_SLOT_PLACE) {
        CHECK(f->instructions[i].type == 256 && !f->instructions[i].immediate); ++places;
    }
    CHECK(places == 1);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL;
    CHECK(xr_xir_compile_checked_read(context, module_place_cases[0].bytes,
        module_place_cases[0].length, &owner, NULL) == XR_XIR_OK);
    exact_owner(owner, &module_place_cases[0]);
    unsigned mismatches = 0;
    for (size_t c = 0; c < 26; ++c) for (unsigned occupied = 0; occupied < 2; ++occupied) {
        const ModulePlaceCase *test = &module_place_cases[c];
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
        exact_owner(owner, &module_place_cases[0]);
        bool match = status == test->expected;
        mismatches += match ? 0u : 1u;
        printf("module-place case=%s occupied=%u expected=%u actual=%u function=%u block=%u instruction=%u reason=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
            test->name, occupied, test->expected, status, diagnostic.function, diagnostic.block, diagnostic.instruction, diagnostic.reason, match ? "PASS" : "FAIL");
    }
    xr_xir_compile_artifact_free(owner);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("module-place reads=52 mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
