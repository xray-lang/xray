/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * semantic67_reader_candidate.c - Full named packet families and fail-closed history
 *
 * KEY CONCEPT:
 *   Current bytes retain their original semantic roles. Private writer sizing
 *   fixtures do not become reader positives, and old identities allocate nothing.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_interface.h"
#include "xir/xxir_nominal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "semantic67_named_cases.h"
#include "semantic67_unit_rejects.inc.c"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25 && XR_XIR_CHECKED_CONTRACT == 67 &&
    XR_XIR_OP_COUNT == 148, "current Checked identity and exact opcode count");
_Static_assert(XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33 &&
    XR_XIR_CONST_RUNE == 136 && XR_XIR_RUNE_TO_INTEGER == 137 &&
    XR_XIR_SLOT_GROUP_INIT == 145 && XR_XIR_GO == 146 && XR_XIR_TASK_AWAIT == 147,
    "independent opcode ordinals");

static void named_declaration_roles(const XrXirModule *m) {
    const XrXirDeclarations *d = m->declarations;
    for (uint32_t f = 0; f < m->function_count; ++f)
        for (uint32_t p = 0; p < m->functions[f].parameter_count; ++p)
            CHECK(m->functions[f].parameters[p] != XR_XIR_UNIT);
    if (!d) return;
    for (uint32_t f = 0; f < m->function_count; ++f) {
        CHECK(!d->functions[f].test_role && !d->functions[f].test_timeout_seconds);
        CHECK(!d->functions[f].nominal_owner && !d->functions[f].cleanup_owner);
        CHECK(!d->functions[f].member_access && !d->functions[f].method_kind && !d->functions[f].promises);
    }
    for (uint32_t owner = 0; owner < d->module_count; ++owner) {
        uint32_t init = d->modules[owner].initializer;
        CHECK(init < m->function_count && d->functions[init].module == owner);
        CHECK(m->functions[init].result == XR_XIR_UNIT && !m->functions[init].parameter_count);
        CHECK(!d->functions[init].exported && init != d->entry_function);
        CHECK(!m->generics || !m->generics[init].parameter_count);
    }
    if (m->linkage_kind == XR_XIR_LIBRARY) {
        CHECK(d->root_module == UINT32_MAX && d->entry_function == UINT32_MAX);
    } else {
        CHECK(d->root_module < d->module_count && d->entry_function < m->function_count);
        CHECK(m->functions[d->entry_function].result == XR_XIR_I64);
        CHECK(!m->functions[d->entry_function].parameter_count);
    }
}

static void named_method_roles(const XrXirModule *m) {
    CHECK(m->types && m->types->count == 1 && m->types->interfaces);
    CHECK(m->types->nodes[0].kind == XR_XIR_TYPE_CALLABLE && m->types->nodes[0].parameter_span == 2);
    CHECK(m->types->nodes[0].parameters[0].type == (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + 1));
    const XrXirInterfaceTable *table = m->types->interfaces;
    CHECK(table->count == 2);
    const XrXirInterfaceDeclaration *parent = &table->declarations[1];
    CHECK(parent->parameter_count == 1 && parent->method_count == 2);
    for (uint32_t method = 0; method < 2; ++method) {
        const XrXirInterfaceMethod *entry = &parent->methods[method];
        CHECK(!entry->receiver && entry->own_parameter_count == 1 && entry->constraints);
        CHECK(entry->constraints[0].markers == (method ? 0u : XR_XIR_CONSTRAINT_SENDABLE));
        CHECK(entry->constraints[0].interface_count == 1);
        CHECK(entry->constraints[0].interfaces[0].declaration == 0);
        CHECK(entry->constraints[0].interfaces[0].argument_count == 1);
        CHECK(entry->constraints[0].interfaces[0].arguments[0] == (XrXirType)XR_XIR_TYPE_PARAMETER_BASE);
    }
}

static void named_family_roles(const XrXirModule *m, size_t index) {
    named_declaration_roles(m);
    if (index == 5 || index == 6) {
        CHECK(m->function_count == 7 && m->generics && m->types && m->types->count == 1);
        CHECK(m->generics[2].parameter_count == 1 && m->generics[2].parameter_kinds);
        CHECK(m->generics[2].parameter_kinds[0] == XR_XIR_BINDER_RESULT_VARIABLE);
        CHECK(!m->generics[3].parameter_kinds && m->generics[3].constraints[0].markers == XR_XIR_CONSTRAINT_EQUAL);
        CHECK(m->types->nodes[0].result == (XrXirType)XR_XIR_TYPE_PARAMETER_BASE);
        CHECK(m->defaults && m->defaults->count == 3);
    }
    if (index == 7) {
        CHECK(!m->declarations && m->functions[0].result == XR_XIR_UNIT);
        CHECK(m->functions[0].parameter_count == 3 && m->types && m->types->count == 3);
        const XrXirType elements[3] = {XR_XIR_I64, XR_XIR_BOOL, XR_XIR_F64};
        for (uint32_t type = 0; type < 3; ++type) {
            CHECK(m->types->nodes[type].kind == XR_XIR_TYPE_ATOMIC);
            CHECK(m->types->nodes[type].element == elements[type]);
        }
    }
    if (index == 8 || index == 10) CHECK(m->functions[0].instructions[0].immediate == INT64_MIN);
    if (index == 11 || index == 14) named_method_roles(m);
    if (index >= 15 && index <= 21) {
        const size_t lengths[7] = {9, 9, 3, 0, 703, 1, 5};
        CHECK(m->linkage_kind == XR_XIR_LIBRARY && m->function_count == 3);
        CHECK(m->declarations->literals[0].length == lengths[index - 15]);
        CHECK(m->declarations->functions[2].exported && !m->declarations->functions[0].exported);
        CHECK(m->defaults && m->defaults->count == 1 && m->defaults->records[0].function == 0);
        if (index == 17) CHECK(!memcmp(m->declarations->literals[0].bytes, "a\0b", 3));
        if (index == 21) CHECK(m->declarations->literal_count == 3);
    }
    if (index == 22) {
        CHECK(m->declarations->module_count == 2 && m->declarations->entry_function == 2);
        CHECK(m->declarations->modules[0].dependency_count == 1 && m->declarations->modules[0].dependencies[0] == 1);
        CHECK(m->types && m->types->nominals && m->types->nominals->count == 1);
        CHECK(m->types->nominals->declarations[0].variant_count == 5);
        CHECK(m->types->nominals->declarations[0].native.native_id == 4);
    }
}

static void named_old_rejection(const XrXirCompileContext *context, const uint8_t *bytes, size_t size) {
    for (unsigned occupied = 0; occupied < 2; ++occupied) {
        XrXirArtifact *artifact = occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL;
        XrXirArtifact *expected = artifact;
        XrCompileResourceStats before = {0}, after = {0};
        CHECK(xr_compile_resources_stats(context->resources, &before) == XR_COMPILE_RESOURCE_OK);
        size_t attempts = source_program_compile_attempts;
        CHECK(xr_xir_compile_checked_read(context, bytes, size, &artifact, NULL) == XR_XIR_BAD_STRUCTURE);
        CHECK(artifact == expected && source_program_compile_attempts == attempts);
        CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK);
        CHECK(after.allocated_bytes == before.allocated_bytes && after.allocation_count == before.allocation_count);
        CHECK(after.live_bytes == before.live_bytes && after.peak_bytes == before.peak_bytes);
    }
}

static void named_packet(const Semantic67NamedVector *v) {
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    const XrCompileResourceLimits caps = {UINT64_C(64) * 1024 * 1024, UINT64_C(8) * 1024 * 1024, 128000000};
    CHECK(xr_compile_resources_new(&caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context.resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    if (v->reader_positive) {
        XrXirArtifact *artifact = NULL;
        XrXirCheckedPacket packet = {0};
        XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_checked_read(&context, v->current, v->current_size, &artifact, &diagnostic);
        if (status != XR_XIR_OK) fprintf(stderr, "%s status=%u function=%u block=%u instruction=%u reason=%u\n",
            v->name, status, diagnostic.function, diagnostic.block, diagnostic.instruction, diagnostic.reason);
        CHECK(status == XR_XIR_OK && artifact);
        const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
        named_family_roles(module, v->index);
        CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(artifact, &packet, NULL) == XR_XIR_OK);
        CHECK(packet.length == v->current_size && !memcmp(packet.bytes, v->current, v->current_size));
        xr_xir_compile_checked_packet_free(&packet);
        xr_xir_compile_artifact_free(artifact);
    }
    named_old_rejection(&context, v->previous65, v->previous65_size);
    named_old_rejection(&context, v->previous64, v->previous64_size);
    CHECK(xr_compile_resources_stats(context.resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes == baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
}

static void named_unit_rejection(const uint8_t *bytes, size_t size) {
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    const XrCompileResourceLimits caps = {UINT64_C(64) * 1024 * 1024, UINT64_C(8) * 1024 * 1024, 128000000};
    CHECK(xr_compile_resources_new(&caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    for (unsigned occupied = 0; occupied < 2; ++occupied) {
        XrXirArtifact *artifact = occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL;
        XrXirArtifact *expected = artifact;
        XrCompileResourceStats before = {0}, after = {0};
        CHECK(xr_compile_resources_stats(context.resources, &before) == XR_COMPILE_RESOURCE_OK);
        size_t attempts = source_program_compile_attempts;
        CHECK(xr_xir_compile_checked_read(&context, bytes, size, &artifact, NULL) == XR_XIR_BAD_TYPE);
        CHECK(artifact == expected && source_program_compile_attempts > attempts);
        CHECK(xr_compile_resources_stats(context.resources, &after) == XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes == before.live_bytes);
    }
    xr_compile_resources_release(context.resources);
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
}

int main(void) {
    size_t readers = 0, writers = 0;
    for (size_t i = 0; i < sizeof(semantic67_named_vectors) / sizeof(semantic67_named_vectors[0]); ++i) {
        const Semantic67NamedVector *v = &semantic67_named_vectors[i];
        fprintf(stderr, "index=%zu family=%s bytes=%zu reader=%u\n", v->index, v->name, v->current_size, (unsigned)v->reader_positive);
        named_packet(v);
        if (v->reader_positive) ++readers; else ++writers;
    }
    CHECK(readers == 21 && writers == 2);
    named_unit_rejection(named_rejected67_unit_formal, sizeof(named_rejected67_unit_formal));
    named_unit_rejection(named_rejected67_ordinary_unit_argument, sizeof(named_rejected67_ordinary_unit_argument));
    puts("21 full current67 reader/writer and decoded roles; 2 writer-only vectors retained; 46 authentic old65/64 empty+occupied zero-allocation rejects; 2 full current67 Unit type rejects; finite ledger physical0 PASS");
    return 0;
}
