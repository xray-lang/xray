/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_graph.c - Preserve module ownership and graph rejection duties
 *
 * KEY CONCEPT:
 *   Module identities, initializer owners and dependency expectations are strict.
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
#include "module_graph_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(sizeof(module_graph_cases)/sizeof(module_graph_cases[0]) == 22, "Complete byte obligation matrix");

static void exact_owner(const XrXirArtifact *owner, const ModuleGraphCase *test) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->function_count == 5 && m->declarations);
    const XrXirDeclarations *d = m->declarations;
    CHECK(d->module_count == 4 && !d->slot_count && !d->literal_count);
    CHECK(d->root_module == test->root && d->entry_function == test->entry);
    for (size_t i = 0; i < 4; ++i) {
        const XrXirSourceModule *module = &d->modules[i];
        const ModuleGraphRow *expected = &test->modules[i];
        CHECK(module->name_length == expected->length &&
            !memcmp(module->name, test->bytes + expected->offset, expected->length));
        CHECK(module->initializer == expected->initializer && module->dependency_count == expected->count);
        for (uint32_t j = 0; j < expected->count; ++j)
            CHECK(module->dependencies[j] == expected->dependencies[j]);
    }
    for (size_t i = 0; i < 5; ++i) CHECK(d->functions[i].module == test->owners[i]);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL;
    CHECK(xr_xir_compile_checked_read(context, module_graph_cases[0].bytes,
        module_graph_cases[0].length, &owner, NULL) == XR_XIR_OK);
    exact_owner(owner, &module_graph_cases[0]);
    unsigned mismatches = 0;
    for (size_t c = 0; c < 22; ++c) for (unsigned occupied = 0; occupied < 2; ++occupied) {
        const ModuleGraphCase *test = &module_graph_cases[c];
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
        exact_owner(owner, &module_graph_cases[0]);
        bool match = status == test->expected;
        mismatches += match ? 0u : 1u;
        printf("module-graph case=%s occupied=%u expected=%u actual=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
            test->name, occupied, test->expected, status, match ? "PASS" : "FAIL");
    }
    xr_xir_compile_artifact_free(owner);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("module-graph reads=44 mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
