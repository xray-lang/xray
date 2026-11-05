/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_display_name_checks.inc.c - Owned diagnostic names and hostile wire checks
 */

static XrProgramBuildStatus write_named_type_fixture(XrProgramArtifact *artifact,
                                                     char *diagnostic, size_t diagnostic_size) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    uint16_t payload = XR_CORE_TYPE_I64;
    char type_name[] = "TopErr", variant_name[] = "Failed";
    XrCoreIrVariantInput variants[] = {
        {.payload_types = &payload, .payload_count = 1u, .display_name = variant_name},
        {.display_name = "Empty"},
    };
    XrCoreIrTypeInput type = {
        .key = {{0xd7u}}, .local_id = 90u, .kind = XR_CORE_IR_TYPE_VARIANT,
        .nominal_kind = XR_CORE_IR_NOMINAL_ENUM,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, .copy_contract = XR_CORE_IR_COPY_TRIVIAL,
        .variants = variants, .variant_count = 2u, .display_name = type_name,
    };
    fixture.input.types = &type;
    fixture.input.type_count = 1u;
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&fixture.input, &program, diagnostic, diagnostic_size);
    /* The writer must not depend on the lifetime or contents of source spellings. */
    memset(type_name, 'x', sizeof(type_name) - 1u);
    memset(variant_name, 'y', sizeof(variant_name) - 1u);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static void test_display_names_roundtrip_and_wire(void) {
    XrProgramArtifact artifact = {0};
    CHECK(write_named_type_fixture(&artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
    if (!artifact.bytes)
        return;
    XrProgramView view;
    CHECK(xr_program_decode_structure(artifact.bytes, artifact.size, NULL, &view, NULL, 0u) ==
          XR_PROGRAM_DECODE_OK);
    const XrProgramSectionView *section = &view.sections[XR_PROGRAM_SECTION_TYPES - 1u];
    size_t row = (size_t) section->offset + 1u + 19u * 4u;
    const uint8_t expected[61] = {
        32u, 33u, 0u, 0u, 0xd7u, [36] = 3u, 2u, 1u, 2u, 0u,
        6u, 'T', 'o', 'p', 'E', 'r', 'r',
        6u, 'F', 'a', 'i', 'l', 'e', 'd', 5u, 'E', 'm', 'p', 't', 'y',
    };
    CHECK(row + sizeof(expected) == section->offset + section->size);
    CHECK(row + sizeof(expected) <= artifact.size);
    if (row + sizeof(expected) > artifact.size) {
        xr_program_artifact_free(&artifact);
        return;
    }
    CHECK(memcmp(artifact.bytes + row, expected, sizeof(expected)) == 0);
    XrValidatedProgram *validated = NULL;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) ==
          XR_PROGRAM_VERIFY_OK);
    uint8_t *mutated = xr_malloc(artifact.size);
    CHECK(mutated != NULL);
    if (mutated) {
        for (uint32_t mutation = 0u; mutation < 10u; ++mutation) {
            memcpy(mutated, artifact.bytes, artifact.size);
            switch (mutation) {
                case 0u: mutated[row + 42u] = 0u; break;
                case 1u: mutated[row + 42u] = '\n'; break;
                case 2u: mutated[row + 42u] = 127u; break;
                case 3u: mutated[row + 42u] = 0x80u; break;
                case 4u: mutated[row + 42u] = 0xc0u; break;
                case 5u: mutated[row + 41u] = 0x80u; mutated[row + 42u] = 0u; break;
                case 6u: mutated[row + 41u] = 0x81u; mutated[row + 42u] = 0x20u; break;
                case 7u: mutated[row + 55u] = 6u; break;
                case 8u: mutated[row + 49u] = 0u; break;
                case 9u: mutated[row + 56u] = 0xffu; break;
            }
            CHECK(xr_program_decode_structure(mutated, artifact.size, NULL, &view, NULL, 0u) !=
                  XR_PROGRAM_DECODE_OK);
            XrValidatedProgram *rejected = NULL;
            CHECK(xr_program_validate(mutated, artifact.size, NULL, &rejected, NULL) !=
                  XR_PROGRAM_VERIFY_OK);
            CHECK(rejected == NULL);
            xr_validated_program_free(rejected);
        }
        xr_free(mutated);
    }
    xr_program_artifact_free(&artifact);
    /* Both the builder and the encoded input have gone away. */
    if (validated) {
        const char *name = xr_validated_program_type_display_name(validated, 32u);
        CHECK(name && strcmp(name, "TopErr") == 0);
        name = xr_validated_program_variant_display_name(validated, 32u, 0u);
        CHECK(name && strcmp(name, "Failed") == 0);
        name = xr_validated_program_variant_display_name(validated, 32u, 1u);
        CHECK(name && strcmp(name, "Empty") == 0);
        CHECK(xr_validated_program_type_display_name(validated, XR_CORE_TYPE_I64) == NULL);
        CHECK(xr_validated_program_type_display_name(validated, 33u) == NULL);
        CHECK(xr_validated_program_variant_display_name(validated, 32u, 2u) == NULL);
        CHECK(xr_validated_program_variant_display_name(validated, 31u, 0u) == NULL);
    }
    CHECK(xr_validated_program_type_display_name(NULL, 32u) == NULL);
    CHECK(xr_validated_program_variant_display_name(NULL, 32u, 0u) == NULL);
    xr_validated_program_free(validated);
}

static void test_display_names_invalid_construction(void) {
    char too_long[4098];
    memset(too_long, 'a', sizeof(too_long));
    too_long[sizeof(too_long) - 1u] = 0;
    const char *invalid[] = {"", "line\nbreak", "\x7f", "\x80", "\xc0\x80", too_long};
    for (uint32_t location = 0u; location < 2u; ++location) {
        for (size_t index = 0u; index < XR_COUNTOF(invalid); ++index) {
            XrProgramModuleFixture fixture;
            xr_program_module_fixture_init(&fixture);
            XrCoreIrVariantInput variant = {.display_name = location ? invalid[index] : NULL};
            XrCoreIrTypeInput type = {
                .key = {{0xd7u}}, .local_id = 90u, .kind = XR_CORE_IR_TYPE_VARIANT,
                .nominal_kind = XR_CORE_IR_NOMINAL_ENUM,
                .ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL,
                .copy_contract = XR_CORE_IR_COPY_TRIVIAL,
                .variants = &variant, .variant_count = 1u,
                .display_name = location ? NULL : invalid[index],
            };
            fixture.input.types = &type;
            fixture.input.type_count = 1u;
            XrCoreIrProgram *program = NULL;
            CHECK(xr_core_ir_program_build(&fixture.input, &program, NULL, 0u) ==
                  (index == 5u ? XR_PROGRAM_BUILD_RESOURCE_LIMIT : XR_PROGRAM_BUILD_INVALID_INPUT));
            CHECK(program == NULL);
            xr_core_ir_program_free(program);
        }
    }
}

static void test_display_names_are_not_type_identity(void) {
    char boundary[4097];
    memset(boundary, 'a', sizeof(boundary) - 1u);
    boundary[sizeof(boundary) - 1u] = 0;
    const char *names[] = {NULL, "\xe9\x94\x99\xe8\xaf\xaf", boundary};
    for (size_t index = 0u; index < XR_COUNTOF(names); ++index) {
        XrProgramModuleFixture fixture;
        xr_program_module_fixture_init(&fixture);
        XrCoreIrVariantInput variants[] = {
            {.display_name = names[index]}, {.display_name = names[index]},
        };
        XrCoreIrTypeInput types[2] = {{
            .key = {{2u}}, .local_id = 90u, .kind = XR_CORE_IR_TYPE_VARIANT,
            .nominal_kind = XR_CORE_IR_NOMINAL_ENUM,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL,
            .copy_contract = XR_CORE_IR_COPY_TRIVIAL,
            .variants = variants, .variant_count = 2u, .display_name = names[index],
        }};
        types[1] = types[0];
        types[1].key.bytes[0] = 1u;
        types[1].local_id = 91u;
        fixture.input.types = types;
        fixture.input.type_count = 2u;
        XrCoreIrProgram *program = NULL;
        XrProgramArtifact artifact = {0};
        CHECK(xr_core_ir_program_build(&fixture.input, &program, NULL, 0u) == XR_PROGRAM_BUILD_OK);
        CHECK(xr_program_write(program, &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
        xr_core_ir_program_free(program);
        XrValidatedProgram *validated = NULL;
        CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, NULL) ==
              XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        if (validated) {
            CHECK(validated->type_count == 2u);
            CHECK(validated->types[0].key.bytes[0] == 1u);
            CHECK(validated->types[1].key.bytes[0] == 2u);
            for (uint16_t type = 32u; type < 34u; ++type) {
                const char *name = xr_validated_program_type_display_name(validated, type);
                CHECK(names[index] ? name && strcmp(name, names[index]) == 0 : name == NULL);
                for (uint32_t variant = 0u; variant < 2u; ++variant) {
                    name = xr_validated_program_variant_display_name(validated, type, variant);
                    CHECK(names[index] ? name && strcmp(name, names[index]) == 0 : name == NULL);
                }
            }
        }
        xr_validated_program_free(validated);
    }
}
