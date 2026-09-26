/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_record_type_checks.inc.c - Structural reference shape admission
 */

static void record_types_init(XrCoreIrTypeInput types[3]) {
    static const uint16_t fields0[] = {XR_CORE_TYPE_I64, 91u};
    static const uint16_t fields1[] = {XR_CORE_TYPE_STRING};
    static const uint16_t fields2[] = {XR_CORE_TYPE_PANIC_INFO};
    memset(types, 0, 3u * sizeof(*types));
    for (uint32_t i = 0u; i < 3u; ++i) {
        types[i].local_id = (uint16_t) (90u + i);
        types[i].key.bytes[0] = (uint8_t) (3u - i);
        types[i].kind = XR_CORE_IR_TYPE_RECORD_REFERENCE;
        types[i].ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
        types[i].copy_contract = i == 2u ? XR_CORE_IR_COPY_FORBIDDEN : XR_CORE_IR_COPY_EXPLICIT;
        types[i].field_types = i == 0u ? fields0 : i == 1u ? fields1 : fields2;
        types[i].field_count = i == 0u ? 2u : 1u;
    }
}

static XrProgramBuildStatus write_record_types(const XrCoreIrTypeInput *types,
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

static void test_record_type_roundtrip(void) {
    XrCoreIrTypeInput types[3];
    record_types_init(types);
    XrProgramArtifact artifact = {0}, reordered = {0};
    CHECK(write_record_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    XrCoreIrTypeInput temp = types[0];
    types[0] = types[2];
    types[2] = temp;
    CHECK(write_record_types(types, &reordered) == XR_PROGRAM_BUILD_OK);
    CHECK(artifact.bytes && reordered.bytes && artifact.size == reordered.size);
    if (artifact.bytes && reordered.bytes && artifact.size == reordered.size)
        CHECK(memcmp(artifact.bytes, reordered.bytes, artifact.size) == 0);
    XrValidatedProgram *validated = NULL;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) ==
          XR_PROGRAM_VERIFY_OK);
    if (validated) {
        CHECK(validated->type_count == 3u);
        for (uint32_t i = 0u; i < 3u; ++i) {
            CHECK(validated->types[i].kind == XR_CORE_IR_TYPE_RECORD_REFERENCE);
            CHECK(validated->types[i].nominal_kind == XR_CORE_IR_NOMINAL_NONE);
            CHECK(validated->types[i].ownership == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
        }
        CHECK(validated->types[0].copy_contract == XR_CORE_IR_COPY_FORBIDDEN);
        CHECK(validated->types[1].copy_contract == XR_CORE_IR_COPY_EXPLICIT);
        CHECK(validated->types[2].field_count == 2u);
        CHECK(validated->types[2].field_types[1] == 33u);
    }
    xr_validated_program_free(validated);
    xr_program_artifact_free(&artifact);
    xr_program_artifact_free(&reordered);
    /* Reference recursion is finite storage, unlike an inline aggregate cycle. */
    record_types_init(types);
    uint16_t self = 90u;
    types[0].field_types = &self;
    types[0].field_count = 1u;
    types[1].field_types = NULL;
    types[1].field_count = 0u;
    CHECK(write_record_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    validated = NULL;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) ==
          XR_PROGRAM_VERIFY_OK);
    if (validated) {
        CHECK(validated->types[2].field_types[0] == 34u);
        CHECK(validated->types[1].field_count == 0u);
    }
    xr_validated_program_free(validated);
    xr_program_artifact_free(&artifact);
}

static void test_record_type_invalid_and_hostile(void) {
    XrCoreIrTypeInput types[3];
    for (uint32_t mutation = 0u; mutation < 5u; ++mutation) {
        record_types_init(types);
        switch (mutation) {
            case 0u: types[0].nominal_kind = XR_CORE_IR_NOMINAL_CLASS; break;
            case 1u: types[0].ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL; break;
            case 2u: types[0].copy_contract = XR_CORE_IR_COPY_TRIVIAL; break;
            case 3u: types[2].copy_contract = XR_CORE_IR_COPY_EXPLICIT; break;
            case 4u: types[0].view_element_type = XR_CORE_TYPE_I64; break;
        }
        XrProgramArtifact artifact = {0};
        CHECK(write_record_types(types, &artifact) == XR_PROGRAM_BUILD_INVALID_INPUT);
        CHECK(artifact.bytes == NULL);
        xr_program_artifact_free(&artifact);
    }
    record_types_init(types);
    XrProgramArtifact artifact = {0};
    CHECK(write_record_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    if (!artifact.bytes) return;
    XrProgramView view;
    CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &view, NULL, 0u) ==
          XR_PROGRAM_DECODE_OK);
    size_t row = (size_t) view.sections[XR_PROGRAM_SECTION_TYPES - 1u].offset + 1u + 20u * 4u;
    /* Exact independent row: reference root, forbidden copy, one PanicInfo field. */
    const uint8_t first[39] = {32u, 40u, 1u, 2u, 1u, [36] = 1u, [37] = 5u};
    CHECK(row + sizeof(first) <= artifact.size);
    if (row + sizeof(first) > artifact.size) {
        xr_program_artifact_free(&artifact);
        return;
    }
    CHECK(memcmp(artifact.bytes + row, first, sizeof(first)) == 0);
    uint8_t *mutated = xr_malloc(artifact.size);
    CHECK(mutated != NULL);
    if (!mutated) { xr_program_artifact_free(&artifact); return; }
    for (uint32_t mutation = 0u; mutation < 5u; ++mutation) {
        memcpy(mutated, artifact.bytes, artifact.size);
        switch (mutation) {
            case 0u: mutated[row + 1u] = 41u; break;
            case 1u: mutated[row + 2u] = 0u; mutated[row + 3u] = 0u; break;
            case 2u: mutated[row + 37u] = 0u; break;
            case 3u: mutated[row + 37u] = 11u; break;
            case 4u: mutated[row + 3u] = 1u; break;
        }
        CHECK(xr_program_decode_structure(mutated, artifact.size, NULL, &view, NULL, 0u) ==
              (mutation == 4u ? XR_PROGRAM_DECODE_OK : XR_PROGRAM_DECODE_NONCANONICAL));
        XrValidatedProgram *validated = NULL;
        CHECK(xr_program_validate(mutated, artifact.size, NULL, &validated, NULL) !=
              XR_PROGRAM_VERIFY_OK);
        CHECK(validated == NULL);
        xr_validated_program_free(validated);
    }
    xr_free(mutated);
    xr_program_artifact_free(&artifact);
}

static void test_record_type_allocation_failures(void) {
    XrCoreIrTypeInput types[3];
    record_types_init(types);
    size_t allocations = 0u;
    for (size_t failure = 1u; failure < 256u; ++failure) {
        XrProgramArtifact artifact = {0};
        allocation_probe_begin(failure);
        XrProgramBuildStatus status = write_record_types(types, &artifact);
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
    printf("Record type constructor/writer allocation points: %zu\n", allocations);
    XrProgramArtifact artifact = {0};
    CHECK(write_record_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
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
    printf("Record type verifier allocation points: %zu\n", allocations);
    xr_program_artifact_free(&artifact);
}
