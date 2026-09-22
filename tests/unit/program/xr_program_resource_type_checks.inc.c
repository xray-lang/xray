/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_resource_type_checks.inc.c - Stable provider resource admission
 */

static void resource_types_init(XrCoreIrTypeInput types[3]) {
    static const uint16_t resource_field[] = {92u};
    static const XrCoreIrVariantInput optional[] = {
        {.payload_count = 0u}, {.payload_types = resource_field, .payload_count = 1u},
    };
    memset(types, 0, 3u * sizeof(*types));
    for (uint32_t i = 0u; i < 3u; ++i) {
        types[i].local_id = (uint16_t) (90u + i);
        types[i].key.bytes[0] = (uint8_t) (3u - i);
        types[i].ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
        types[i].copy_contract = XR_CORE_IR_COPY_FORBIDDEN;
    }
    types[0].kind = XR_CORE_IR_TYPE_RECORD_REFERENCE;
    types[0].field_types = resource_field;
    types[0].field_count = 1u;
    types[1].kind = XR_CORE_IR_TYPE_VARIANT;
    types[1].variants = optional;
    types[1].variant_count = 2u;
    types[2].kind = XR_CORE_IR_TYPE_PROVIDER_RESOURCE;
    for (uint32_t i = 0u; i < 16u; ++i)
        types[2].resource_id.bytes[i] = (uint8_t) (i + 1u);
}

static XrProgramBuildStatus write_resource_types(const XrCoreIrTypeInput *types,
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

static void test_resource_type_admission(void) {
    XrCoreIrTypeInput types[3];
    resource_types_init(types);
    XrProgramArtifact artifact = {0}, reordered = {0};
    CHECK(write_resource_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    if (!artifact.bytes) return;
    XrCoreIrTypeInput temp = types[0]; types[0] = types[2]; types[2] = temp;
    CHECK(write_resource_types(types, &reordered) == XR_PROGRAM_BUILD_OK);
    CHECK(reordered.bytes && reordered.size == artifact.size);
    if (reordered.bytes && reordered.size == artifact.size)
        CHECK(memcmp(artifact.bytes, reordered.bytes, artifact.size) == 0);
    xr_program_artifact_free(&reordered);
    XrValidatedProgram *validated = NULL;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) == XR_PROGRAM_VERIFY_OK);
    if (validated) {
        CHECK(validated->types[0].kind == XR_CORE_IR_TYPE_PROVIDER_RESOURCE);
        CHECK(validated->types[0].nominal_kind == XR_CORE_IR_NOMINAL_NONE);
        for (uint32_t i = 0u; i < 16u; ++i)
            CHECK(validated->types[0].resource_id.bytes[i] == i + 1u);
        for (uint32_t i = 0u; i < 3u; ++i) {
            CHECK(validated->types[i].ownership == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
            CHECK(validated->types[i].copy_contract == XR_CORE_IR_COPY_FORBIDDEN);
        }
        CHECK(validated->types[1].variants[1].payload_types[0] == 32u);
        CHECK(validated->types[2].field_types[0] == 32u);
    }
    xr_validated_program_free(validated);
    XrProgramView view;
    CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &view, NULL, 0u) == XR_PROGRAM_DECODE_OK);
    size_t row = (size_t) view.sections[XR_PROGRAM_SECTION_TYPES - 1u].offset + 1u + 19u * 4u;
    /* Independent fixed row: TypeId 32, wire kind 41, affine, forbidden copy,
     * key 01 followed by 31 zeros, length 16, identity 01..10, absent name. */
    const uint8_t expected[54] = {32u, 41u, 1u, 2u, 1u, [36] = 16u,
        1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 0u};
    CHECK(row + sizeof(expected) <= artifact.size);
    if (row + sizeof(expected) <= artifact.size)
        CHECK(memcmp(artifact.bytes + row, expected, sizeof(expected)) == 0);
    uint8_t *mutated = xr_malloc(artifact.size);
    CHECK(mutated != NULL);
    if (mutated) {
        for (uint32_t mutation = 0u; mutation < 6u; ++mutation) {
            memcpy(mutated, artifact.bytes, artifact.size);
            switch (mutation) {
                case 0u: mutated[row + 1u] = 42u; break;
                case 1u: mutated[row + 2u] = 0u; mutated[row + 3u] = 0u; break;
                case 2u: mutated[row + 3u] = 1u; break;
                case 3u: mutated[row + 36u] = 15u; break;
                case 4u: mutated[row + 36u] = 17u; break;
                case 5u: memset(mutated + row + 37u, 0, 16u); break;
            }
            CHECK(xr_program_decode_structure(mutated, artifact.size, NULL, &view, NULL, 0u) != XR_PROGRAM_DECODE_OK);
            validated = NULL;
            CHECK(xr_program_validate(mutated, artifact.size, NULL, &validated, NULL) != XR_PROGRAM_VERIFY_OK);
            CHECK(validated == NULL);
            xr_validated_program_free(validated);
        }
        xr_free(mutated);
    }
    for (size_t size = 0u; size < artifact.size; ++size) {
        validated = NULL;
        CHECK(xr_program_validate(artifact.bytes, size, NULL, &validated, NULL) != XR_PROGRAM_VERIFY_OK);
        CHECK(validated == NULL);
        xr_validated_program_free(validated);
    }
    xr_program_artifact_free(&artifact);
    for (uint32_t mutation = 0u; mutation < 9u; ++mutation) {
        resource_types_init(types);
        switch (mutation) {
            case 0u: memset(types[2].resource_id.bytes, 0, 16u); break;
            case 1u: types[2].copy_contract = XR_CORE_IR_COPY_EXPLICIT; break;
            case 2u: types[2].ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL; break;
            case 3u: types[2].nominal_kind = XR_CORE_IR_NOMINAL_CLASS; break;
            case 4u: types[2].field_types = types[0].field_types; break;
            case 5u: types[2].array_element_type = XR_CORE_TYPE_I64; break;
            case 6u: types[0].copy_contract = XR_CORE_IR_COPY_EXPLICIT; break;
            case 7u: types[1].copy_contract = XR_CORE_IR_COPY_EXPLICIT; break;
            case 8u: types[0].resource_id.bytes[0] = 1u; break;
        }
        CHECK(write_resource_types(types, &artifact) == XR_PROGRAM_BUILD_INVALID_INPUT);
        CHECK(artifact.bytes == NULL);
        xr_program_artifact_free(&artifact);
    }
}

static void test_resource_type_allocation_failures(void) {
    XrCoreIrTypeInput types[3];
    resource_types_init(types);
    size_t allocations = 0u;
    for (size_t failure = 1u; failure < 256u; ++failure) {
        XrProgramArtifact artifact = {0};
        allocation_probe_begin(failure);
        XrProgramBuildStatus status = write_resource_types(types, &artifact);
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
    printf("Resource type constructor/writer allocation points: %zu\n", allocations);
    XrProgramArtifact artifact = {0};
    CHECK(write_resource_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    for (size_t failure = 1u; failure < 256u; ++failure) {
        XrValidatedProgram *validated = NULL;
        allocation_probe_begin(failure);
        XrProgramVerifyStatus status = xr_program_validate(artifact.bytes, artifact.size, NULL,
                                                            &validated, NULL);
        if (status != XR_PROGRAM_VERIFY_OK) {
            CHECK(status == XR_PROGRAM_VERIFY_OUT_OF_MEMORY);
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
    printf("Resource type verifier allocation points: %zu\n", allocations);
    xr_program_artifact_free(&artifact);
}
