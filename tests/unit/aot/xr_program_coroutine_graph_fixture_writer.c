/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_coroutine_graph_fixture_writer.c - Pure AOT coroutine graph fixtures
 */

#include "aot/program/xr_backend_ir.h"
#include "base/xsha256.h"
#include "program/xr_validated_program_internal.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_cleanup_graph_fixture.h"
#include "../program/xr_program_coroutine_branch_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct GraphCounts {
    uint32_t blocks;
    uint32_t yields;
    uint32_t providers;
    uint32_t conditionals;
    uint32_t self_edges;
    uint32_t drops;
} GraphCounts;

static bool exact_graph_counts(const XrValidatedProgram *program, bool cleanup,
                               GraphCounts *counts) {
    memset(counts, 0, sizeof(*counts));
    if (program->function_count != 1u || program->entry_function != 0u ||
        program->type_count != (cleanup ? 1u : 0u) ||
        program->provider_requirement_count != (cleanup ? 1u : 0u))
        return false;
    const XrValidatedFunction *function = &program->functions[0];
    if (function->parameter_count != 0u || function->coroutine_state_count != 2u ||
        function->coroutine_safepoint_count != 1u ||
        function->result_type_id != (cleanup ? XR_CORE_TYPE_VOID : XR_CORE_TYPE_I64))
        return false;
    counts->blocks = function->block_count;
    for (uint32_t b = 0u; b < function->block_count; ++b) {
        const XrValidatedBlock *block = &function->blocks[b];
        for (uint32_t i = 0u; i < block->instruction_count; ++i) {
            const XrValidatedInstruction *op = &block->instructions[i];
            counts->yields += op->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD;
            counts->providers += op->operation_id == XR_CORE_OP_CORE_PROVIDER_CALL;
            counts->conditionals += op->operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH;
            counts->drops += op->operation_id == XR_CORE_OP_CORE_OWNER_DROP;
            for (uint32_t s = 0u; s < op->successor_count; ++s)
                counts->self_edges += op->successors[s] == b;
        }
    }
    return counts->blocks == (cleanup ? 10u : 4u) && counts->yields == 1u &&
           counts->providers == (cleanup ? 2u : 0u) &&
           counts->conditionals == (cleanup ? 3u : 1u) && counts->self_edges == 1u &&
           counts->drops == (cleanup ? 4u : 0u);
}

static XrProgramBuildStatus write_cleanup_program(const XrTargetProfile *profile,
                                                  XrProgramArtifact *artifact, char *message,
                                                  size_t message_size) {
    XrProgramCleanupGraphFixture fixture;
    if (!xr_program_cleanup_graph_fixture_init(&fixture))
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    const XrTargetProviderContract *clock = NULL;
    for (size_t p = 0u; p < xr_target_profile_provider_count(profile); ++p) {
        const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, p);
        if (candidate && candidate->provider_kind == XR_TARGET_PROVIDER_CLOCK) {
            if (clock)
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            clock = candidate;
        }
    }
    if (!clock || clock->operation_count != 1u)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    fixture.requirement.contract_id = clock->contract_id;
    fixture.operation = clock->operations[0].stable_id;
    for (uint32_t b = 0u; b < XR_CLEANUP_GRAPH_BLOCK_COUNT; ++b) {
        for (uint32_t i = 0u; i < fixture.blocks[b].instruction_count; ++i) {
            XrCoreIrInstructionInput *op = &fixture.instructions[b][i];
            if (op->operation_id == XR_CORE_OP_CORE_PROVIDER_CALL) {
                op->immediate.provider_operation.contract_id = fixture.requirement.contract_id;
                op->immediate.provider_operation.operation_id = fixture.operation;
            }
        }
    }
    return xr_program_cleanup_graph_fixture_write_input(&fixture, artifact, message, message_size);
}

static bool write_stable_bytes(const char *path, const char *bytes, size_t size) {
    FILE *existing = fopen(path, "rb");
    if (existing) {
        char chunk[4096];
        size_t offset = 0u;
        bool same = true;
        size_t read;
        while ((read = fread(chunk, 1u, sizeof(chunk), existing)) != 0u) {
            if (offset > size || read > size - offset || memcmp(bytes + offset, chunk, read) != 0)
                same = false;
            if (read > SIZE_MAX - offset) {
                (void) fclose(existing);
                return false;
            }
            offset += read;
        }
        bool readable = !ferror(existing);
        if (fclose(existing) != 0 || !readable)
            return false;
        if (same && offset == size)
            return true;
    } else if (errno != ENOENT) {
        return false;
    }
    FILE *output = fopen(path, "wb");
    if (!output)
        return false;
    bool ok = fwrite(bytes, 1u, size, output) == size;
    return fclose(output) == 0 && ok;
}

static void digest_hex(const uint8_t *digest, char *hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0u; i < XR_PROGRAM_DIGEST_SIZE; ++i) {
        hex[i * 2u] = digits[digest[i] >> 4u];
        hex[i * 2u + 1u] = digits[digest[i] & 0x0fu];
    }
    hex[XR_PROGRAM_DIGEST_SIZE * 2u] = '\0';
}

typedef struct FactsBuffer {
    char bytes[768];
    size_t size;
    bool valid;
} FactsBuffer;

static void facts_append(FactsBuffer *buffer, const char *text) {
    size_t size = strlen(text);
    if (!buffer->valid || size > sizeof(buffer->bytes) - buffer->size) {
        buffer->valid = false;
        return;
    }
    memcpy(buffer->bytes + buffer->size, text, size);
    buffer->size += size;
}

static void facts_append_u32(FactsBuffer *buffer, uint32_t value) {
    char digits[10];
    size_t count = 0u;
    do {
        digits[count++] = (char) ('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    if (!buffer->valid || count > sizeof(buffer->bytes) - buffer->size) {
        buffer->valid = false;
        return;
    }
    while (count != 0u)
        buffer->bytes[buffer->size++] = digits[--count];
}

static bool write_facts(const char *path, bool cleanup, const GraphCounts *counts,
                        const XrGeneratedC *generated) {
    uint8_t digest[XR_PROGRAM_DIGEST_SIZE];
    char source_hex[65], execution_hex[65];
    FactsBuffer facts = {.valid = true};
    xr_sha256((const uint8_t *) generated->bytes, generated->size, digest);
    digest_hex(digest, source_hex);
    digest_hex(generated->execution_id.bytes, execution_hex);
    facts_append(&facts,
                 "{\"schema\":1,\"producer\":\"program-coroutine-graphs-v1\",\"scenario\":\"");
    facts_append(&facts, cleanup ? "cleanup" : "branch");
    facts_append(&facts, "\",\"cases\":");
    facts_append_u32(&facts, cleanup ? 4u : 2u);
    facts_append(&facts, ",\"functions\":1,\"entry\":0,\"blocks\":");
    facts_append_u32(&facts, counts->blocks);
    facts_append(&facts, ",\"yields\":");
    facts_append_u32(&facts, counts->yields);
    facts_append(&facts, ",\"provider_calls\":");
    facts_append_u32(&facts, counts->providers);
    facts_append(&facts, ",\"conditionals\":");
    facts_append_u32(&facts, counts->conditionals);
    facts_append(&facts, ",\"self_edges\":");
    facts_append_u32(&facts, counts->self_edges);
    facts_append(&facts, ",\"owner_drops\":");
    facts_append_u32(&facts, counts->drops);
    facts_append(&facts, ",\"execution_id\":\"");
    facts_append(&facts, execution_hex);
    facts_append(&facts, "\",\"sha256\":\"");
    facts_append(&facts, source_hex);
    facts_append(&facts, "\"}\n");
    return facts.valid && facts.size != 0u && write_stable_bytes(path, facts.bytes, facts.size);
}

int main(int argc, char **argv) {
    if (argc != 7 || strcmp(argv[1], "--scenario") != 0 ||
        (strcmp(argv[2], "cleanup") != 0 && strcmp(argv[2], "branch") != 0) ||
        strcmp(argv[3], "--output") != 0 || strcmp(argv[5], "--facts") != 0 || argv[4][0] == '\0' ||
        argv[6][0] == '\0' || strcmp(argv[4], argv[6]) == 0) {
        fprintf(stderr, "expected --scenario cleanup|branch --output GENERATED_C --facts JSON\n");
        return 2;
    }
    bool cleanup = strcmp(argv[2], "cleanup") == 0;
    XrTargetProfile *profile =
        cleanup ? xr_test_target_profile_build_with_scalar_clock(
                      false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
                      XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER)
                : xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    XrBackendIR *ir = NULL;
    XrGeneratedC generated = {0}, repeated = {0};
    XrBackendDiagnostic diagnostic = {0};
    XrProgramDiagnostic program_diagnostic = {0};
    XrBackendOptions options = xr_backend_default_options();
    char message[256] = {0};
    GraphCounts counts = {0};
    bool ok = profile &&
              (cleanup ? write_cleanup_program(profile, &artifact, message, sizeof(message))
                       : xr_program_coroutine_branch_fixture_write(
                             &artifact, message, sizeof(message))) == XR_PROGRAM_BUILD_OK &&
              xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                  &program_diagnostic) == XR_PROGRAM_VERIFY_OK &&
              exact_graph_counts(program, cleanup, &counts) &&
              xr_backend_ir_build(program, profile, &options, &ir, &diagnostic) == XR_BACKEND_OK &&
              xr_backend_ir_verify(ir, &diagnostic) &&
              xr_backend_ir_translation_validate(ir, &diagnostic) &&
              xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK &&
              xr_backend_ir_emit_c(ir, false, &repeated, &diagnostic) == XR_BACKEND_OK &&
              generated.size != 0u && generated.size == repeated.size &&
              memcmp(generated.bytes, repeated.bytes, generated.size) == 0 &&
              write_stable_bytes(argv[4], generated.bytes, generated.size) &&
              write_facts(argv[6], cleanup, &counts, &generated);
    if (!ok)
        fprintf(stderr, "coroutine graph %s generation failed: program=%u backend=%u %s\n", argv[2],
                (unsigned) program_diagnostic.kind, (unsigned) diagnostic.status, message);
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
    xr_target_profile_free(profile);
    return ok ? 0 : 1;
}
