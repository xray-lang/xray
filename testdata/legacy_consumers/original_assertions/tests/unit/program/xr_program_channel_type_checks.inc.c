/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_channel_type_checks.inc.c - Exact channel element and handle contracts
 */

static void channel_types_init(XrCoreIrTypeInput types[3]) {
    memset(types, 0, 3u * sizeof(*types));
    for (uint32_t i = 0u; i < 3u; ++i) {
        types[i].local_id = (uint16_t) (90u + i);
        types[i].key.bytes[0] = (uint8_t) (i + 1u);
        types[i].kind = XR_CORE_IR_TYPE_CHANNEL;
        types[i].ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
        types[i].copy_contract = XR_CORE_IR_COPY_EXPLICIT;
    }
    types[0].channel_element_type = 92u;
    types[1].channel_element_type = XR_CORE_TYPE_STRING;
    types[2].kind = XR_CORE_IR_TYPE_PROVIDER_RESOURCE;
    types[2].copy_contract = XR_CORE_IR_COPY_FORBIDDEN;
    types[2].resource_id.bytes[0] = 1u;
}

static XrProgramBuildStatus write_channel_types(const XrCoreIrTypeInput types[3],
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

static void test_channel_types(void) {
    XrCoreIrTypeInput types[3];
    channel_types_init(types);
    XrProgramArtifact artifact = {0}, reordered = {0};
    CHECK(write_channel_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    if (!artifact.bytes)
        return;
    XrCoreIrTypeInput swap = types[0];
    types[0] = types[2];
    types[2] = swap;
    CHECK(write_channel_types(types, &reordered) == XR_PROGRAM_BUILD_OK);
    CHECK(reordered.size == artifact.size);
    if (reordered.bytes && reordered.size == artifact.size)
        CHECK(memcmp(reordered.bytes, artifact.bytes, artifact.size) == 0);
    xr_program_artifact_free(&reordered);
    XrValidatedProgram *validated = NULL;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) ==
          XR_PROGRAM_VERIFY_OK);
    if (validated) {
        CHECK(validated->types[0].kind == XR_CORE_IR_TYPE_CHANNEL);
        CHECK(validated->types[0].channel_element_type == 34u);
        CHECK(validated->types[0].copy_contract == XR_CORE_IR_COPY_EXPLICIT);
        CHECK(validated->types[2].copy_contract == XR_CORE_IR_COPY_FORBIDDEN);
        CHECK(validated->types[1].channel_element_type == XR_CORE_TYPE_STRING);
    }
    xr_validated_program_free(validated);
    XrProgramView view = {0};
    CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &view, NULL, 0u) ==
          XR_PROGRAM_DECODE_OK);
    size_t row = (size_t) view.sections[XR_PROGRAM_SECTION_TYPES - 1u].offset + 1u + 20u * 4u;
    /* Fixed independent wire row: local resource 92 canonicalizes to 34. */
    const uint8_t expected[38] = {32u, 42u, 1u, 1u, 1u, [36] = 34u};
    CHECK(row + sizeof(expected) <= artifact.size);
    if (row + sizeof(expected) <= artifact.size)
        CHECK(memcmp(artifact.bytes + row, expected, sizeof(expected)) == 0);
    uint8_t *bytes = xr_malloc(artifact.size);
    CHECK(bytes != NULL);
    if (bytes) {
        for (uint32_t i = 0u; i < 5u; ++i) {
            memcpy(bytes, artifact.bytes, artifact.size);
            if (i == 0u) bytes[row + 36u] = 0u;
            if (i == 1u) bytes[row + 36u] = 35u;
            if (i == 2u) bytes[row + 2u] = 0u;
            if (i == 3u) bytes[row + 3u] = 0u;
            if (i == 4u) bytes[row + 3u] = 2u;
            validated = NULL;
            CHECK(xr_program_validate(bytes, artifact.size, NULL, &validated, NULL) ==
                  XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED);
            CHECK(validated == NULL);
            xr_validated_program_free(validated);
        }
        xr_free(bytes);
    }
    xr_program_artifact_free(&artifact);
    for (uint32_t i = 0u; i < 7u; ++i) {
        channel_types_init(types);
        if (i == 0u) types[0].channel_element_type = XR_CORE_TYPE_VOID;
        if (i == 1u) types[0].channel_element_type = 93u;
        if (i == 2u) types[0].ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
        if (i == 3u) types[0].copy_contract = XR_CORE_IR_COPY_FORBIDDEN;
        if (i == 4u) types[0].nominal_kind = XR_CORE_IR_NOMINAL_CLASS;
        if (i == 5u) types[0].array_element_type = XR_CORE_TYPE_I64;
        if (i == 6u) types[0].kind = XR_CORE_IR_TYPE_ATOMIC;
        XrProgramBuildStatus status = write_channel_types(types, &artifact);
        CHECK(status == (i == 1u ? XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE
                                 : XR_PROGRAM_BUILD_INVALID_INPUT));
        CHECK(artifact.bytes == NULL);
        xr_program_artifact_free(&artifact);
    }
}

static void test_channel_type_allocation_failures(void) {
    XrCoreIrTypeInput types[3];
    channel_types_init(types);
    size_t allocations = 0u;
    for (size_t failure = 1u; failure < 256u; ++failure) {
        XrProgramArtifact artifact = {0};
        allocation_probe_begin(failure);
        XrProgramBuildStatus status = write_channel_types(types, &artifact);
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
    printf("Channel type constructor/writer allocation points: %zu\n", allocations);
    XrProgramArtifact artifact = {0};
    CHECK(write_channel_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
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
    printf("Channel type verifier allocation points: %zu\n", allocations);
    xr_program_artifact_free(&artifact);
}
