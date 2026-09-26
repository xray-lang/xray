/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_atomic_type_checks.inc.c - Independent atomic type wire and ownership checks
 */

static void atomic_types_init(XrCoreIrTypeInput types[3]) {
    memset(types, 0, 3u * sizeof(*types));
    for (uint32_t index = 0u; index < 3u; ++index) {
        types[index].local_id = (uint16_t) (90u + index);
        types[index].key.bytes[0] = (uint8_t) (3u - index);
        types[index].kind = XR_CORE_IR_TYPE_ATOMIC;
        types[index].ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
        types[index].copy_contract = XR_CORE_IR_COPY_EXPLICIT;
        types[index].atomic_element_type = index == 1u ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64;
    }
}

static XrProgramBuildStatus write_atomic_types(const XrCoreIrTypeInput *types,
                                              XrProgramArtifact *artifact) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    fixture.input.types = types;
    fixture.input.type_count = 3u;
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&fixture.input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}

static void test_atomic_type_roundtrip(void) {
    XrCoreIrTypeInput types[3];
    atomic_types_init(types);
    XrProgramArtifact artifact = {0}, reordered = {0};
    CHECK(write_atomic_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    XrCoreIrTypeInput temp = types[0];
    types[0] = types[2];
    types[2] = temp;
    CHECK(write_atomic_types(types, &reordered) == XR_PROGRAM_BUILD_OK);
    CHECK(artifact.size == reordered.size && artifact.bytes && reordered.bytes);
    if (artifact.bytes && reordered.bytes)
        CHECK(memcmp(artifact.bytes, reordered.bytes, artifact.size) == 0);
    XrValidatedProgram *validated = NULL;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) ==
          XR_PROGRAM_VERIFY_OK);
    if (validated) {
        CHECK(validated->type_count == 3u);
        CHECK(validated->types[0].kind == XR_CORE_IR_TYPE_ATOMIC);
        CHECK(validated->types[0].atomic_element_type == XR_CORE_TYPE_I64);
        CHECK(validated->types[1].atomic_element_type == XR_CORE_TYPE_BOOL);
        CHECK(validated->types[1].copy_contract == XR_CORE_IR_COPY_EXPLICIT);
        CHECK(validated->types[2].atomic_element_type == XR_CORE_TYPE_I64);
        CHECK(validated->types[2].copy_contract == XR_CORE_IR_COPY_EXPLICIT);
    }
    xr_validated_program_free(validated);
    xr_program_artifact_free(&reordered);
    xr_program_artifact_free(&artifact);
}

static void test_atomic_type_invalid_construction(void) {
    for (uint32_t mutation = 0u; mutation < 12u; ++mutation) {
        XrCoreIrTypeInput types[3];
        atomic_types_init(types);
        uint16_t field = XR_CORE_TYPE_I64;
        switch (mutation) {
            case 0u: types[0].atomic_element_type = XR_CORE_TYPE_VOID; break;
            case 1u: types[0].atomic_element_type = XR_CORE_TYPE_STRING; break;
            case 2u: types[0].atomic_element_type = 91u; break;
            case 3u: types[0].nominal_kind = XR_CORE_IR_NOMINAL_CLASS; break;
            case 4u: types[0].ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL; break;
            case 5u: types[0].copy_contract = XR_CORE_IR_COPY_TRIVIAL; break;
            case 6u: types[0].copy_contract = XR_CORE_IR_COPY_FORBIDDEN; break;
            case 7u: types[0].array_element_type = XR_CORE_TYPE_I64; break;
            case 8u: types[0].field_types = &field; types[0].field_count = 1u; break;
            case 9u: types[0].view_element_type = XR_CORE_TYPE_I64; break;
            case 10u: types[0].atomic_element_type = UINT16_MAX; break;
            case 11u: types[0].kind = XR_CORE_IR_TYPE_AGGREGATE; break;
        }
        XrProgramArtifact artifact = {0};
        CHECK(write_atomic_types(types, &artifact) == XR_PROGRAM_BUILD_INVALID_INPUT);
        CHECK(artifact.bytes == NULL);
        xr_program_artifact_free(&artifact);
    }
}

static void test_atomic_type_hostile_wire(void) {
    XrCoreIrTypeInput types[3];
    atomic_types_init(types);
    XrProgramArtifact artifact = {0};
    CHECK(write_atomic_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    if (!artifact.bytes)
        return;
    XrProgramView view;
    CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &view, NULL, 0u) ==
          XR_PROGRAM_DECODE_OK);
    size_t row = (size_t) view.sections[XR_PROGRAM_SECTION_TYPES - 1u].offset + 1u + 20u * 4u;
    /* Independent wire expectation: TypeId, kind, ownership, copy, 32-byte key,
     * then canonical element TypeId and an absent display name. No physical capacity. */
    const uint8_t first[38] = {32u, 39u, 1u, 1u, 1u, [36] = 2u};
    CHECK(row + sizeof(first) * 3u <= artifact.size);
    if (row + sizeof(first) * 3u > artifact.size) {
        xr_program_artifact_free(&artifact);
        return;
    }
    CHECK(memcmp(artifact.bytes + row, first, sizeof(first)) == 0);
    uint8_t *mutated = xr_malloc(artifact.size);
    CHECK(mutated != NULL);
    if (!mutated) {
        xr_program_artifact_free(&artifact);
        return;
    }
    for (uint32_t mutation = 0u; mutation < 7u; ++mutation) {
        memcpy(mutated, artifact.bytes, artifact.size);
        switch (mutation) {
            case 0u: mutated[row + 36u] = 0u; break;
            case 1u: mutated[row + 36u] = 11u; break;
            case 2u: mutated[row + 2u] = 0u; mutated[row + 3u] = 0u; break;
            case 3u: mutated[row + 1u] = 40u; break;
            case 4u: mutated[row + 3u] = 2u; break;
            case 5u: mutated[row + 38u + 3u] = 2u; break;
            case 6u: mutated[row + 36u] = 33u; break;
        }
        XrProgramDecodeStatus structural =
            xr_program_decode_structure(mutated, artifact.size, NULL, &view, NULL, 0u);
        CHECK(structural == (XR_PROGRAM_DECODE_NONCANONICAL));
        XrValidatedProgram *validated = NULL;
        CHECK(xr_program_validate(mutated, artifact.size, NULL, &validated, NULL) !=
              XR_PROGRAM_VERIFY_OK);
        CHECK(validated == NULL);
        xr_validated_program_free(validated);
    }
    xr_free(mutated);
    xr_program_artifact_free(&artifact);
}

static void test_atomic_type_allocation_failures(void) {
    XrCoreIrTypeInput types[3];
    atomic_types_init(types);
    size_t allocations = 0u;
    for (size_t failure = 1u; failure < 256u; ++failure) {
        XrProgramArtifact artifact = {0};
        allocation_probe_begin(failure);
        XrProgramBuildStatus status = write_atomic_types(types, &artifact);
        if (status != XR_PROGRAM_BUILD_OK)
            CHECK(status == XR_PROGRAM_BUILD_OUT_OF_MEMORY && artifact.bytes == NULL);
        xr_program_artifact_free(&artifact);
        allocations = allocation_probe.attempts;
        allocation_probe_end();
        if (status == XR_PROGRAM_BUILD_OK) {
            CHECK(allocations + 1u == failure);
            break;
        }
    }
    CHECK(allocations != 0u && allocations < 255u);
    printf("Atomic type constructor/writer allocation points: %zu\n", allocations);
    XrProgramArtifact artifact = {0};
    CHECK(write_atomic_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    if (!artifact.bytes)
        return;
    allocations = 0u;
    for (size_t failure = 1u; failure < 256u; ++failure) {
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic diagnostic = {0};
        allocation_probe_begin(failure);
        XrProgramVerifyStatus status =
            xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, &diagnostic);
        if (status != XR_PROGRAM_VERIFY_OK) {
            CHECK(diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY);
            CHECK(validated == NULL);
        }
        xr_validated_program_free(validated);
        allocations = allocation_probe.attempts;
        allocation_probe_end();
        if (status == XR_PROGRAM_VERIFY_OK) {
            CHECK(allocations + 1u == failure);
            break;
        }
    }
    CHECK(allocations != 0u && allocations < 255u);
    printf("Atomic type verifier allocation points: %zu\n", allocations);
    xr_program_artifact_free(&artifact);
}
