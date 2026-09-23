/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_program_provider_requirements.c - Hostile logical import contracts
 */

#include "base/xmalloc.h"
#include "program/xr_program_verify.h"
#include "xr_program_trap_fixture.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static size_t logical_offset(const XrProgramArtifact *artifact) {
    XrProgramView view = {0};
    char diagnostic[128] = {0};
    if (xr_program_decode_structure(artifact->bytes, artifact->size, NULL, &view, diagnostic,
                                    sizeof(diagnostic)) != XR_PROGRAM_DECODE_OK)
        return 0u;
    for (uint32_t index = 0u; index < view.section_count; ++index) {
        if (view.sections[index].stable_id != XR_PROGRAM_SECTION_IMPORTS)
            continue;
        size_t offset = (size_t) view.sections[index].offset;
        /* This independent fixture has one contract and one nullary query.
         * Its count and logical length fields each fit one minimal byte. */
        CHECK(view.sections[index].size == 59u);
        CHECK(artifact->bytes[offset] == 1u);
        CHECK(artifact->bytes[offset + 17u] == 1u);
        CHECK(artifact->bytes[offset + 34u] == 24u);
        return offset + 35u;
    }
    return 0u;
}

static void check_mutation(const XrProgramArtifact *artifact, size_t offset, uint8_t value,
                           XrProgramVerifyStatus expected, XrProgramDiagnosticKind kind) {
    uint8_t *bytes = xr_malloc(artifact->size);
    CHECK(bytes != NULL);
    if (!bytes)
        return;
    memcpy(bytes, artifact->bytes, artifact->size);
    bytes[offset] = value;
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic diagnostic = {0};
    CHECK(xr_program_validate(bytes, artifact->size, NULL, &program, &diagnostic) == expected);
    CHECK(program == NULL);
    if (diagnostic.kind != kind)
        fprintf(stderr, "Provider mutation offset=%zu value=%u: diagnostic=%u expected=%u\n",
                offset, (unsigned) value, (unsigned) diagnostic.kind, (unsigned) kind);
    CHECK(diagnostic.kind == kind);
    if (kind == XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE) {
        CHECK(diagnostic.location.section_id == XR_PROGRAM_SECTION_CODE);
        CHECK(diagnostic.location.function_id == 0u);
        CHECK(diagnostic.location.block_id == 0u);
        CHECK(diagnostic.location.instruction_id == 0u);
    }
    xr_validated_program_free(program);
    xr_free(bytes);
}

static void test_declared_semantics_and_owned_storage(void) {
    XrProgramArtifact artifact = {0};
    char message[128] = {0};
    CHECK(xr_program_trap_fixture_write(&artifact, message, sizeof(message)) ==
          XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic diagnostic = {0};
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &diagnostic) ==
          XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrProgramProviderRequirementView requirement = {0};
    CHECK(xr_validated_program_provider_requirement(program, 0u, &requirement));
    CHECK(requirement.operation_count == 1u);
    if (requirement.operation_count == 1u) {
        XrProviderLogicalContract expected = xr_program_fixture_scalar_contract(true);
        XrFingerprint wanted, actual;
        CHECK(xr_provider_logical_contract_fingerprint(&expected, &wanted));
        CHECK(xr_provider_logical_contract_fingerprint(&requirement.operations[0].logical_contract,
                                                       &actual));
        CHECK(memcmp(wanted.bytes, actual.bytes, sizeof(wanted.bytes)) == 0);
    }
    xr_validated_program_free(program);
}

static void test_hostile_logical_contracts(void) {
    XrProgramArtifact artifact = {0};
    char message[128] = {0};
    CHECK(xr_program_trap_fixture_write(&artifact, message, sizeof(message)) ==
          XR_PROGRAM_BUILD_OK);
    size_t offset = logical_offset(&artifact);
    CHECK(offset != 0u);
    if (offset == 0u) {
        xr_program_artifact_free(&artifact);
        return;
    }
    const struct {
        uint8_t field;
        uint8_t value;
    } valid_but_unadmitted[] = {
        {4u, XR_PROVIDER_EFFECT_READS_CLOCK | XR_PROVIDER_EFFECT_MAY_PANIC},
        {4u, XR_PROVIDER_EFFECT_READS_CLOCK | XR_PROVIDER_EFFECT_MAY_SUSPEND},
        {18u, XR_PROVIDER_THREADS_INSTANCE_AFFINE},
        {19u, XR_PROVIDER_REENTRY_FORBIDDEN},
        {20u, XR_PROVIDER_CALLBACK_SYNCHRONOUS},
        {22u, XR_PROVIDER_TYPE_BOOL},
    };
    /* These rows decode as logical contracts, but disagree with the exact
     * synchronous i64 call. Per-operation checking precedes import-use closure. */
    for (size_t index = 0u; index < sizeof(valid_but_unadmitted) / sizeof(valid_but_unadmitted[0]);
         ++index) {
        check_mutation(&artifact, offset + valid_but_unadmitted[index].field,
                       valid_but_unadmitted[index].value, XR_PROGRAM_VERIFY_SEMANTIC_REJECTED,
                       XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
    }
    const uint8_t malformed_fields[] = {0u, 8u, 12u, 16u, 17u, 18u, 19u, 20u, 21u, 22u};
    for (size_t index = 0u; index < sizeof(malformed_fields); ++index) {
        check_mutation(&artifact, offset + malformed_fields[index], 0u,
                       XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL);
    }
    check_mutation(&artifact, offset - 1u, 0u, XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED,
                   XR_PROGRAM_DIAGNOSTIC_STRUCTURAL);
    check_mutation(&artifact, offset - 1u, 127u, XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED,
                   XR_PROGRAM_DIAGNOSTIC_STRUCTURAL);
    check_mutation(&artifact, XR_PROGRAM_MAGIC_SIZE, 2u, XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED,
                   XR_PROGRAM_DIAGNOSTIC_STRUCTURAL);
    xr_program_artifact_free(&artifact);
}

int main(void) {
    test_declared_semantics_and_owned_storage();
    test_hostile_logical_contracts();
    if (failures != 0u)
        return 1;
    puts("Program logical provider requirements: PASS");
    return 0;
}
