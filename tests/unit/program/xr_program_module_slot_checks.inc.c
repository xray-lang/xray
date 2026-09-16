/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_module_slot_checks.inc.c - Independent module declaration admission checks
 */

static XrProgramBuildStatus write_module_slots_fixture(XrProgramArtifact *artifact,
                                                       char *diagnostic, size_t diagnostic_size) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    xr_program_module_fixture_add_slots(&fixture);
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&fixture.input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static void test_module_slots_invalid_construction(void) {
    for (uint32_t mutation = 0u; mutation < 10u; ++mutation) {
        XrProgramModuleFixture fixture;
        xr_program_module_fixture_init(&fixture);
        xr_program_module_fixture_add_slots(&fixture);
        switch (mutation) {
            case 0u:
                fixture.slots[0][0].key = (XrCoreIrKey) {0};
                break;
            case 1u:
                fixture.slots[0][0].key = fixture.slots[0][1].key;
                break;
            case 2u:
                fixture.slots[0][0].type_id = XR_CORE_TYPE_VOID;
                break;
            case 3u:
                fixture.slots[0][0].type_id = 11u;
                break;
            case 4u:
                fixture.slots[0][0].type_id = 127u;
                break;
            case 5u:
                fixture.slots[0][0].flags = 2u;
                break;
            case 6u:
                fixture.modules[0].initializer = (XrCoreIrKey) {0};
                break;
            case 7u:
                fixture.modules[0].slots = NULL;
                break;
            case 8u:
                fixture.modules[0].slot_count = 0u;
                break;
            default:
                fixture.modules[0].slot_count = XR_PROGRAM_LIMIT_MODULE_SLOTS + 1u;
                break;
        }
        XrCoreIrProgram *program = NULL;
        char diagnostic[256] = {0};
        XrProgramBuildStatus status =
            xr_core_ir_program_build(&fixture.input, &program, diagnostic, sizeof(diagnostic));
        XrProgramBuildStatus expected = mutation == 3u || mutation == 4u
                                           ? XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE
                                       : mutation == 9u ? XR_PROGRAM_BUILD_RESOURCE_LIMIT
                                                        : XR_PROGRAM_BUILD_INVALID_INPUT;
        CHECK(status == expected);
        CHECK(program == NULL);
        xr_core_ir_program_free(program);
    }
}

static void test_module_slot_identity(void) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    xr_program_module_fixture_add_slots(&fixture);
    XrProgramId identities[4] = {0};
    for (uint32_t variant = 0u; variant < 4u; ++variant) {
        XrCoreIrProgram *program = NULL;
        XrProgramArtifact artifact = {0};
        char diagnostic[256] = {0};
        if (variant == 1u) {
            XrCoreIrModuleSlotInput first = fixture.slots[0][0];
            fixture.slots[0][0] = fixture.slots[0][1];
            fixture.slots[0][1] = first;
        } else if (variant == 2u) {
            fixture.slots[0][0].flags ^= XR_PROGRAM_MODULE_SLOT_CONST;
        } else if (variant == 3u) {
            fixture.slots[0][0].type_id = XR_CORE_TYPE_BOOL;
        }
        CHECK(xr_core_ir_program_build(&fixture.input, &program, diagnostic, sizeof(diagnostic)) ==
              XR_PROGRAM_BUILD_OK);
        if (program)
            CHECK(xr_program_write(program, &artifact, diagnostic, sizeof(diagnostic)) ==
                  XR_PROGRAM_BUILD_OK);
        identities[variant] = artifact.id;
        xr_program_artifact_free(&artifact);
        xr_core_ir_program_free(program);
    }
    CHECK(xr_program_id_equal(identities[0], identities[1]));
    CHECK(!xr_program_id_equal(identities[1], identities[2]));
    CHECK(!xr_program_id_equal(identities[2], identities[3]));
}

static void test_module_slots_construction(void) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    xr_program_module_fixture_add_slots(&fixture);
    const uint16_t field = XR_CORE_TYPE_I64;
    XrCoreIrTypeInput type = {
        .key = key("module-slot-aggregate"), .local_id = 19u,
        .kind = XR_CORE_IR_TYPE_AGGREGATE, .field_types = &field, .field_count = 1u,
    };
    fixture.input.types = &type;
    fixture.input.type_count = 1u;
    fixture.slots[0][0].type_id = type.local_id;
    XrCoreIrProgram *program = NULL;
    char diagnostic[256] = {0};
    CHECK(xr_core_ir_program_build(&fixture.input, &program, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    if (!program)
        return;
    XrProgramArtifact first = {0};
    CHECK(xr_program_write(program, &first, diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    fixture.slots[0][0].flags = XR_PROGRAM_MODULE_SLOT_CONST;
    fixture.slots[0][0].key = key("changed-producer-key");
    XrProgramArtifact copied = {0};
    CHECK(xr_program_write(program, &copied, diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    CHECK(first.size == copied.size && memcmp(first.bytes, copied.bytes, first.size) == 0);
    xr_program_artifact_free(&copied);
    xr_core_ir_program_free(program);
    XrValidatedProgram *validated = NULL;
    XrProgramDiagnostic rejection = {0};
    CHECK(xr_program_validate(first.bytes, first.size, NULL, &validated, &rejection) ==
          XR_PROGRAM_VERIFY_OK);
    if (validated) {
        CHECK(validated->module_slot_count == 8u);
        for (uint32_t module = 0u; module < 4u; ++module) {
            CHECK(validated->modules[module].slot_count == 2u);
            for (uint32_t index = 0u; index < 2u; ++index) {
                const XrValidatedModuleSlot *slot = &validated->modules[module].slots[index];
                if (xr_core_ir_key_equal(slot->key, key("counter:1"))) {
                    CHECK(slot->type_id == (module == 0u ? 16u : XR_CORE_TYPE_I64));
                    CHECK(slot->flags == 0u);
                } else {
                    CHECK(xr_core_ir_key_equal(slot->key, key("label:2")));
                    CHECK(slot->type_id == XR_CORE_TYPE_STRING);
                    CHECK(slot->flags == XR_PROGRAM_MODULE_SLOT_CONST);
                }
            }
        }
    }
    xr_validated_program_free(validated);
    xr_program_artifact_free(&first);
    test_module_slots_invalid_construction();
    test_module_slot_identity();
}

static void test_module_slots_hostile_wire(void) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    CHECK(write_module_slots_fixture(&artifact, diagnostic, sizeof(diagnostic)) ==
          XR_PROGRAM_BUILD_OK);
    if (!artifact.bytes)
        return;
    size_t row = artifact.size;
    for (size_t offset = 0u; offset + XR_CORE_IR_KEY_SIZE <= artifact.size; ++offset)
        if (memcmp(artifact.bytes + offset, fixture.modules[0].key.bytes, XR_CORE_IR_KEY_SIZE) == 0)
            row = offset;
    /* The base module owns one function, has no dependencies and two slots.
     * Each fixed fixture slot has a key, one-byte TypeId and one-byte flags. */
    const size_t first = row + XR_CORE_IR_KEY_SIZE + 5u;
    const size_t second = first + XR_CORE_IR_KEY_SIZE + 2u;
    CHECK(second + XR_CORE_IR_KEY_SIZE + 2u <= artifact.size);
    if (second + XR_CORE_IR_KEY_SIZE + 2u > artifact.size) {
        xr_program_artifact_free(&artifact);
        return;
    }
    uint8_t *mutated = xr_malloc(artifact.size);
    CHECK(mutated != NULL);
    if (!mutated) {
        xr_program_artifact_free(&artifact);
        return;
    }
    for (uint32_t mutation = 0u; mutation < 6u; ++mutation) {
        memcpy(mutated, artifact.bytes, artifact.size);
        if (mutation == 0u)
            memset(mutated + first, 0, XR_CORE_IR_KEY_SIZE);
        else if (mutation < 4u)
            mutated[first + XR_CORE_IR_KEY_SIZE] = mutation == 1u ? 0u : mutation == 2u ? 11u : 127u;
        else if (mutation == 4u)
            memcpy(mutated + second, mutated + first, XR_CORE_IR_KEY_SIZE);
        else
            mutated[first + XR_CORE_IR_KEY_SIZE + 1u] = 2u;
        expect_decode_status(mutated, artifact.size, NULL,
                             mutation < 4u ? XR_PROGRAM_DECODE_OK : XR_PROGRAM_DECODE_NONCANONICAL);
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic rejection = {0};
        CHECK(xr_program_validate(mutated, artifact.size, NULL, &validated, &rejection) !=
              XR_PROGRAM_VERIFY_OK);
        CHECK(validated == NULL);
        if (mutation == 0u)
            CHECK(rejection.kind == XR_PROGRAM_DIAGNOSTIC_FUNCTION);
        else if (mutation < 4u)
            CHECK(rejection.kind == XR_PROGRAM_DIAGNOSTIC_TYPE);
        xr_validated_program_free(validated);
    }
    xr_free(mutated);
    xr_program_artifact_free(&artifact);
}
