/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_nominal_names.c - Preserve original name rejection duties
 *
 * KEY CONCEPT:
 *   Legacy byte and size expectations remain strict. Unexpected admission is
 *   reported as failure after full owner cleanup, never blessed as a new oracle.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_nominal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "nominal_name_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(sizeof(nominal_name_cases)/sizeof(nominal_name_cases[0]) == 18, "Complete byte obligation matrix");

static void exact_owner(const XrXirArtifact *owner, const NominalNameCase *test) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->types && m->types->nominals && m->types->nominals->count == 1);
    const XrXirNominalDeclaration *d = m->types->nominals->declarations;
    CHECK(d && d->kind == XR_XIR_NOMINAL_ENUM && d->field_count == 1 && d->variant_count == 2);
    CHECK(d->fields[0].type == XR_XIR_I64 && d->variants[0].field_count == 1 && !d->variants[1].field_count);
    CHECK(d->name.length == test->type_length && !memcmp(d->name.bytes, test->bytes+test->type_offset, test->type_length));
    CHECK(d->variants[0].name.length == test->variant_length &&
        !memcmp(d->variants[0].name.bytes, test->bytes+test->variant_offset, test->variant_length));
    CHECK(d->variants[1].name.length == 5 && !memcmp(d->variants[1].name.bytes, "Empty", 5));
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL;
    CHECK(xr_xir_compile_checked_read(context, nominal_name_cases[0].bytes,
        nominal_name_cases[0].length, &owner, NULL) == XR_XIR_OK);
    exact_owner(owner, &nominal_name_cases[0]);
    unsigned mismatches = 0;
    for (size_t c = 0; c < 18; ++c) for (unsigned occupied = 0; occupied < 2; ++occupied) {
        const NominalNameCase *test = &nominal_name_cases[c];
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
        exact_owner(owner, &nominal_name_cases[0]);
        bool match = status == test->expected;
        mismatches += match ? 0u : 1u;
        printf("nominal-name case=%s occupied=%u expected=%u actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
            test->name, occupied, test->expected, status, match ? "PASS" : "FAIL");
    }
    xr_xir_compile_artifact_free(owner);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("nominal-name reads=36 mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
