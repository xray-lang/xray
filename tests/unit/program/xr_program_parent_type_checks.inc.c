/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_parent_type_checks.inc.c - Nominal parent graph admission
 */

static void parent_types_init(XrCoreIrTypeInput types[3]) {
    static const uint16_t fields[] = {XR_CORE_TYPE_I64, XR_CORE_TYPE_STRING};
    memset(types, 0, 3u * sizeof(*types));
    for (uint32_t index = 0u; index < 3u; ++index) {
        types[index].local_id = (uint16_t) (90u + index);
        types[index].key.bytes[0] = (uint8_t) (index + 1u);
        types[index].kind = XR_CORE_IR_TYPE_CLASS_REFERENCE;
        types[index].nominal_kind = XR_CORE_IR_NOMINAL_CLASS;
        types[index].ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
        types[index].copy_contract = XR_CORE_IR_COPY_EXPLICIT;
        types[index].field_count = 2u - index;
        types[index].field_types = index == 2u ? NULL : fields;
        types[index].parent_type_id = index == 2u ? XR_CORE_TYPE_VOID
                                                 : (uint16_t) (91u + index);
    }
}

static XrProgramBuildStatus write_parent_types(XrCoreIrTypeInput types[3],
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

static void test_parent_types(void) {
    int previous_failures = failures;
    XrCoreIrTypeInput types[3];
    parent_types_init(types);
    XrProgramArtifact artifact = {0}, reordered = {0};
    CHECK(write_parent_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    XrCoreIrTypeInput swap = types[0];
    types[0] = types[2];
    types[2] = swap;
    CHECK(write_parent_types(types, &reordered) == XR_PROGRAM_BUILD_OK);
    CHECK(artifact.bytes && reordered.bytes && artifact.size == reordered.size);
    if (artifact.bytes && reordered.bytes && artifact.size == reordered.size)
        CHECK(memcmp(artifact.bytes, reordered.bytes, artifact.size) == 0);
    xr_program_artifact_free(&reordered);
    XrValidatedProgram *validated = NULL;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) ==
          XR_PROGRAM_VERIFY_OK);
    if (validated) {
        CHECK(validated->types[0].parent_type_id == 33u);
        CHECK(validated->types[1].parent_type_id == 34u);
        CHECK(validated->types[2].parent_type_id == XR_CORE_TYPE_VOID);
    }
    xr_validated_program_free(validated);
    XrProgramView view = {0};
    CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &view, NULL, 0u) ==
          XR_PROGRAM_DECODE_OK);
    if (artifact.bytes) {
        size_t row = (size_t) view.sections[XR_PROGRAM_SECTION_TYPES - 1u].offset + 1u + 20u * 4u;
        const uint8_t first[41] = {32u, 37u, 1u, 1u, 1u,
                                  [36] = 2u, [37] = 2u, [38] = 12u, [39] = 33u};
        CHECK(row + sizeof(first) <= artifact.size);
        if (row + sizeof(first) > artifact.size) {
            xr_program_artifact_free(&artifact);
            return;
        }
        CHECK(memcmp(artifact.bytes + row, first, sizeof(first)) == 0);
        uint8_t *mutated = xr_malloc(artifact.size);
        CHECK(mutated != NULL);
        if (mutated) {
            for (uint32_t mutation = 0u; mutation < 3u; ++mutation) {
                memcpy(mutated, artifact.bytes, artifact.size);
                if (mutation == 0u) mutated[row + 39u] = 32u;
                if (mutation == 1u) mutated[row + 39u] = XR_CORE_TYPE_I64;
                if (mutation == 2u) mutated[row + 37u] = XR_CORE_TYPE_BOOL;
                validated = NULL;
                if (mutation == 2u)
                    CHECK(xr_program_decode_structure(mutated, artifact.size, NULL, &view,
                                                       NULL, 0u) == XR_PROGRAM_DECODE_OK);
                CHECK(xr_program_validate(mutated, artifact.size, NULL, &validated, NULL) !=
                      XR_PROGRAM_VERIFY_OK);
                CHECK(validated == NULL);
            }
            xr_free(mutated);
        }
    }
    xr_program_artifact_free(&artifact);
    for (uint32_t mutation = 0u; mutation < 6u; ++mutation) {
        parent_types_init(types);
        if (mutation == 0u) types[0].parent_type_id = 90u;
        if (mutation == 1u) types[0].parent_type_id = XR_CORE_TYPE_I64;
        if (mutation == 2u) {
            types[0].field_count = 0u;
            types[0].field_types = NULL;
        }
        if (mutation == 3u) types[0].parent_type_id = 900u;
        if (mutation == 4u) {
            types[1].kind = XR_CORE_IR_TYPE_RECORD_REFERENCE;
            types[1].nominal_kind = XR_CORE_IR_NOMINAL_NONE;
            types[1].parent_type_id = XR_CORE_TYPE_VOID;
        }
        if (mutation == 5u) {
            for (uint32_t index = 0u; index < 3u; ++index) {
                types[index].field_count = 0u;
                types[index].field_types = NULL;
            }
            types[2].parent_type_id = 90u;
        }
        XrProgramBuildStatus status = write_parent_types(types, &artifact);
        XrProgramBuildStatus expected = mutation == 3u ? XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE
                                                      : XR_PROGRAM_BUILD_INVALID_INPUT;
        if (status != expected)
            fprintf(stderr, "parent mutation %u status %u\n", mutation, (unsigned) status);
        CHECK(status == expected);
        CHECK(artifact.bytes == NULL);
    }
    printf("Parent type admission: %s\n", failures == previous_failures ? "PASS" : "FAIL");
}

static void test_parent_type_cycle_wire_and_allocations(void) {
    XrCoreIrTypeInput types[3];
    parent_types_init(types);
    for (uint32_t index = 0u; index < 3u; ++index) {
        types[index].field_count = 0u;
        types[index].field_types = NULL;
    }
    XrProgramArtifact artifact = {0};
    CHECK(write_parent_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    XrProgramView view = {0};
    CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &view, NULL, 0u) ==
          XR_PROGRAM_DECODE_OK);
    if (artifact.bytes) {
        size_t row = (size_t) view.sections[XR_PROGRAM_SECTION_TYPES - 1u].offset + 1u + 20u * 4u;
        size_t parent_offset = row + 2u * 39u + 37u;
        CHECK(parent_offset < artifact.size);
        if (parent_offset < artifact.size) {
            CHECK(artifact.bytes[parent_offset] == 0u);
            artifact.bytes[parent_offset] = 32u;
            CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &view,
                                               NULL, 0u) == XR_PROGRAM_DECODE_OK);
            XrValidatedProgram *validated = NULL;
            CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) !=
                  XR_PROGRAM_VERIFY_OK);
            CHECK(validated == NULL);
        }
    }
    xr_program_artifact_free(&artifact);
    parent_types_init(types);
    size_t allocations = 0u;
    for (size_t failure = 1u; failure < 256u; ++failure) {
        allocation_probe_begin(failure);
        XrProgramBuildStatus status = write_parent_types(types, &artifact);
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
    CHECK(allocations > 0u && allocations < 255u);
}

static void test_parent_type_requires_nominal_target_wire(void) {
    XrCoreIrTypeInput types[3];
    parent_types_init(types);
    types[0].parent_type_id = XR_CORE_TYPE_VOID;
    types[1].parent_type_id = XR_CORE_TYPE_VOID;
    types[1].kind = XR_CORE_IR_TYPE_RECORD_REFERENCE;
    types[1].nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    XrProgramArtifact artifact = {0};
    CHECK(write_parent_types(types, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *validated = NULL;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) ==
          XR_PROGRAM_VERIFY_OK);
    xr_validated_program_free(validated);
    XrProgramView view = {0};
    CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &view, NULL, 0u) ==
          XR_PROGRAM_DECODE_OK);
    size_t parent_offset = (size_t) view.sections[XR_PROGRAM_SECTION_TYPES - 1u].offset +
                           1u + 20u * 4u + 39u;
    CHECK(parent_offset < artifact.size);
    if (parent_offset < artifact.size) {
        CHECK(artifact.bytes[parent_offset] == XR_CORE_TYPE_VOID);
        artifact.bytes[parent_offset] = 33u;
        CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &view,
                                           NULL, 0u) == XR_PROGRAM_DECODE_OK);
        validated = NULL;
        CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) !=
              XR_PROGRAM_VERIFY_OK);
        CHECK(validated == NULL);
    }
    xr_program_artifact_free(&artifact);
}
