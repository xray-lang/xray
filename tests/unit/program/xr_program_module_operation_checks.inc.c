/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_module_operation_checks.inc.c - Module place admission tests
 */

static void test_module_hostile_indices(const XrProgramArtifact *artifact) {
    size_t cursor = find_operation_offset(artifact, XR_CORE_OP_CORE_PLACE_MODULE);
    CHECK(cursor != SIZE_MAX);
    if (cursor == SIZE_MAX)
        return;
    for (uint32_t field = 0u; field < 6u; ++field)
        (void) test_take_uvar(artifact->bytes, artifact->size, &cursor);
    CHECK(test_take_uvar(artifact->bytes, artifact->size, &cursor) ==
          XR_CORE_IR_IMMEDIATE_MODULE_SLOT);
    size_t module_offset = cursor;
    CHECK(test_take_uvar(artifact->bytes, artifact->size, &cursor) == 0u);
    size_t slot_offset = cursor;
    expect_mutated_verify(artifact, module_offset, 4u, XR_PROGRAM_VERIFY_SEMANTIC_REJECTED,
                          XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
    expect_mutated_verify(artifact, slot_offset, 2u, XR_PROGRAM_VERIFY_SEMANTIC_REJECTED,
                          XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
    expect_mutated_verify(artifact, module_offset, 1u, XR_PROGRAM_VERIFY_SEMANTIC_REJECTED,
                          XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
}

static void test_module_operation_admission(void) {
    for (uint32_t mutation = 0u; mutation < 12u; ++mutation) {
        XrProgramModuleFixture fixture;
        xr_program_module_fixture_init(&fixture);
        xr_program_module_fixture_add_slots(&fixture);
        XrCoreIrConstantInput constant = {
            .key = key("module-constant"), .type_id = XR_CORE_TYPE_I64,
            .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = 40,
        };
        fixture.modules[0].constants = &constant;
        fixture.modules[0].constant_count = 1u;
        XrCoreIrKey operands[] = {key("module-place"), key("initial-value")};
        XrCoreIrInstructionInput instructions[] = {
            {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE,
             .result = operands[0], .result_type_id = XR_CORE_TYPE_I64,
             .result_category = XR_CORE_IR_PLACE,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
             .immediate.module_slot = {fixture.modules[0].key, fixture.slots[0][0].key}},
            {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
             .result = operands[1], .result_type_id = XR_CORE_TYPE_I64,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = constant.key},
            {.operation_id = XR_CORE_OP_CORE_PLACE_INITIALIZE,
             .operands = operands, .operand_count = 2u},
            {.operation_id = XR_CORE_OP_CORE_RETURN},
        };
        uint32_t body = mutation == 1u ? 1u : 0u;
        fixture.blocks[body].instructions = instructions;
        fixture.blocks[body].instruction_count = 4u;
        fixture.functions[body].effect_mask = XR_CORE_EFFECT_TRAP;
        if (mutation == 2u)
            instructions[0].result_type_id = XR_CORE_TYPE_BOOL;
        else if (mutation == 3u) {
            instructions[1].operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL;
            instructions[1].result_type_id = XR_CORE_TYPE_BOOL;
            constant.type_id = XR_CORE_TYPE_BOOL;
            constant.kind = XR_CORE_IR_CONSTANT_BOOL;
            constant.value.boolean = true;
        } else if (mutation == 4u || mutation == 5u) {
            fixture.slots[0][0].flags = XR_PROGRAM_MODULE_SLOT_CONST;
        } else if (mutation == 7u) {
            instructions[0].immediate.module_slot.module = key("missing-module");
        } else if (mutation == 8u) {
            instructions[0].immediate.module_slot.declaration = key("missing-slot");
        } else if (mutation == 9u) {
            fixture.slots[0][0].type_id = XR_CORE_TYPE_STRING;
            constant.type_id = XR_CORE_TYPE_STRING;
            constant.kind = XR_CORE_IR_CONSTANT_STRING;
            constant.value.string.bytes = (const uint8_t *) "text";
            constant.value.string.size = 4u;
            instructions[0].result_type_id = XR_CORE_TYPE_STRING;
            instructions[1].operation_id = XR_CORE_OP_CORE_CONSTANT_STRING;
            instructions[1].result_type_id = XR_CORE_TYPE_STRING;
            instructions[1].result_ownership = XR_CORE_IR_OWNER;
            instructions[2].operation_id = XR_CORE_OP_CORE_PLACE_TAKE;
            instructions[2].operand_count = 1u;
            instructions[2].result = key("taken-value");
            instructions[2].result_type_id = XR_CORE_TYPE_STRING;
            instructions[2].result_ownership = XR_CORE_IR_OWNER;
        } else if (mutation == 10u) {
            instructions[0] = instructions[1];
            instructions[1] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
                .result = operands[0], .result_type_id = XR_CORE_TYPE_I64,
                .result_category = XR_CORE_IR_PLACE,
                .operands = &operands[1], .operand_count = 1u,
            };
        } else if (mutation == 11u) {
            fixture.functions[body].effect_mask = 0u;
        }
        if (mutation == 5u || mutation == 6u)
            instructions[2].operation_id = XR_CORE_OP_CORE_PLACE_STORE;
        XrCoreIrProgram *program = NULL;
        XrProgramArtifact artifact = {0};
        char diagnostic[256] = {0};
        CHECK(xr_core_ir_program_build(&fixture.input, &program, diagnostic, sizeof(diagnostic)) ==
              XR_PROGRAM_BUILD_OK);
        if (!program)
            continue;
        XrProgramBuildStatus written =
            xr_program_write(program, &artifact, diagnostic, sizeof(diagnostic));
        if (mutation == 7u || mutation == 8u) {
            CHECK(written == XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE);
            CHECK(artifact.bytes == NULL);
        } else {
            CHECK(written == XR_PROGRAM_BUILD_OK);
            if (mutation == 0u)
                test_module_hostile_indices(&artifact);
            XrValidatedProgram *validated = NULL;
            XrProgramDiagnostic rejection = {0};
            XrProgramVerifyStatus status =
                xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, &rejection);
            bool expected = mutation == 0u || mutation == 4u || mutation == 6u;
            if ((status == XR_PROGRAM_VERIFY_OK) != expected)
                fprintf(stderr, "module operation mutation %u: status=%u diagnostic=%u\n",
                        mutation, (unsigned) status, (unsigned) rejection.kind);
            CHECK((status == XR_PROGRAM_VERIFY_OK) == expected);
            CHECK((validated != NULL) == expected);
            if (!expected)
                CHECK(rejection.kind == (mutation == 11u ? XR_PROGRAM_DIAGNOSTIC_EFFECT
                                                         : XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE));
            xr_validated_program_free(validated);
        }
        xr_program_artifact_free(&artifact);
        xr_core_ir_program_free(program);
    }
}

static void test_module_const_place_survives_branch(void) {
    for (uint32_t constant_slot = 0u; constant_slot < 2u; ++constant_slot) {
        XrProgramModuleFixture fixture;
        xr_program_module_fixture_init(&fixture);
        xr_program_module_fixture_add_slots(&fixture);
        fixture.slots[0][0].flags = constant_slot ? XR_PROGRAM_MODULE_SLOT_CONST : 0u;
        XrCoreIrKey operands[] = {key("module-place"), key("new-value")};
        XrCoreIrKey forwarded[] = {key("forwarded-place"), key("forwarded-value")};
        XrCoreIrKey successor = key("module-write");
        XrCoreIrConstantInput constant = {
            .key = key("forty"), .type_id = XR_CORE_TYPE_I64,
            .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = 40,
        };
        fixture.modules[0].constants = &constant;
        fixture.modules[0].constant_count = 1u;
        XrCoreIrInstructionInput entry[] = {
            {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE,
             .result = operands[0], .result_type_id = XR_CORE_TYPE_I64,
             .result_category = XR_CORE_IR_PLACE,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
             .immediate.module_slot = {fixture.modules[0].key, fixture.slots[0][0].key}},
            {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
             .result = operands[1], .result_type_id = XR_CORE_TYPE_I64,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = constant.key},
            {.operation_id = XR_CORE_OP_CORE_BRANCH, .operands = operands, .operand_count = 2u,
             .successors = &successor, .successor_count = 1u},
        };
        XrCoreIrValueInput arguments[] = {
            {.key = forwarded[0], .type_id = XR_CORE_TYPE_I64, .category = XR_CORE_IR_PLACE},
            {.key = forwarded[1], .type_id = XR_CORE_TYPE_I64},
        };
        XrCoreIrInstructionInput tail[] = {
            {.operation_id = XR_CORE_OP_CORE_PLACE_STORE,
             .operands = forwarded, .operand_count = 2u},
            {.operation_id = XR_CORE_OP_CORE_RETURN},
        };
        XrCoreIrBlockInput blocks[] = {
            {.key = fixture.functions[0].entry_block, .instructions = entry,
             .instruction_count = 3u},
            {.key = successor, .arguments = arguments, .argument_count = 2u,
             .instructions = tail, .instruction_count = 2u},
        };
        fixture.functions[0].blocks = blocks;
        fixture.functions[0].block_count = 2u;
        fixture.functions[0].effect_mask = XR_CORE_EFFECT_TRAP;
        XrCoreIrProgram *program = NULL;
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic rejection = {0};
        CHECK(xr_core_ir_program_build(&fixture.input, &program, NULL, 0u) == XR_PROGRAM_BUILD_OK);
        CHECK(xr_program_write(program, &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
        XrProgramVerifyStatus status =
            xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, &rejection);
        if ((status == XR_PROGRAM_VERIFY_OK) != (constant_slot == 0u))
            fprintf(stderr, "module place branch const=%u: diagnostic=%u\n", constant_slot,
                    (unsigned) rejection.kind);
        CHECK((status == XR_PROGRAM_VERIFY_OK) == (constant_slot == 0u));
        if (constant_slot)
            CHECK(rejection.kind == XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
        xr_validated_program_free(validated);
        xr_program_artifact_free(&artifact);
        xr_core_ir_program_free(program);
    }
}

static void test_module_place_ref_call_admission(void) {
    for (uint32_t mutation = 0u; mutation < 3u; ++mutation) {
        XrProgramModuleFixture fixture;
        xr_program_module_fixture_init(&fixture);
        xr_program_module_fixture_add_slots(&fixture);
        fixture.slots[0][0].flags = mutation == 1u ? XR_PROGRAM_MODULE_SLOT_CONST : 0u;
        XrCoreIrKey place = key("module-ref-call-place");
        XrCoreIrKey callee = key("ref-callee");
        uint16_t parameter = XR_CORE_TYPE_I64;
        XrParamMode mode = XR_PARAM_REF;
        XrCoreIrValueInput argument = {
            .key = key("ref-argument"), .type_id = XR_CORE_TYPE_I64,
            .category = XR_CORE_IR_PLACE,
        };
        XrCoreIrInstructionInput returned = {.operation_id = XR_CORE_OP_CORE_RETURN};
        XrCoreIrBlockInput callee_block = {
            .key = key("ref-callee-block"), .arguments = &argument, .argument_count = 1u,
            .instructions = &returned, .instruction_count = 1u,
        };
        XrCoreIrFunctionInput functions[] = {
            fixture.functions[0],
            {.key = callee, .parameter_types = &parameter, .parameter_modes = &mode,
             .parameter_count = 1u, .entry_block = callee_block.key,
             .blocks = &callee_block, .block_count = 1u},
        };
        XrCoreIrInstructionInput instructions[] = {
            {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE,
             .result = place, .result_type_id = XR_CORE_TYPE_I64,
             .result_category = XR_CORE_IR_PLACE,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
             .immediate.module_slot = {fixture.modules[0].key, fixture.slots[0][0].key}},
            {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
             .operands = &place, .operand_count = 1u,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION, .immediate.key = callee},
            {.operation_id = XR_CORE_OP_CORE_RETURN},
        };
        fixture.blocks[0].instructions = instructions;
        fixture.blocks[0].instruction_count = 3u;
        functions[0].effect_mask = XR_CORE_EFFECT_CALL | (mutation == 2u ? 0u : XR_CORE_EFFECT_TRAP);
        fixture.modules[0].functions = functions;
        fixture.modules[0].function_count = 2u;
        XrCoreIrProgram *program = NULL;
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic rejection = {0};
        CHECK(xr_core_ir_program_build(&fixture.input, &program, NULL, 0u) == XR_PROGRAM_BUILD_OK);
        CHECK(xr_program_write(program, &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
        XrProgramVerifyStatus status =
            xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, &rejection);
        if ((status == XR_PROGRAM_VERIFY_OK) != (mutation == 0u))
            fprintf(stderr, "module ref call mutation=%u: diagnostic=%u\n", mutation,
                    (unsigned) rejection.kind);
        CHECK((status == XR_PROGRAM_VERIFY_OK) == (mutation == 0u));
        if (mutation != 0u)
            CHECK(rejection.kind == (mutation == 2u ? XR_PROGRAM_DIAGNOSTIC_EFFECT
                                                   : XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE));
        xr_validated_program_free(validated);
        xr_program_artifact_free(&artifact);
        xr_core_ir_program_free(program);
    }
}

static void test_module_place_ref_interface_admission(void) {
    const XrProgramExistentialFixtureMutation variants[] = {
        XR_EXISTENTIAL_FIXTURE_MODULE_REF, XR_EXISTENTIAL_FIXTURE_CONST_MODULE_REF,
        XR_EXISTENTIAL_FIXTURE_MODULE_REF_MISSING_TRAP,
    };
    for (uint32_t index = 0u; index < 3u; ++index) {
        XrProgramArtifact artifact = {0};
        char diagnostic[256] = {0};
        CHECK(xr_program_existential_fixture_write_mutated(variants[index], &artifact,
                                                            diagnostic, sizeof(diagnostic)) ==
              XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic semantic = {0};
        XrProgramVerifyStatus status =
            xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, &semantic);
        if ((status == XR_PROGRAM_VERIFY_OK) != (index == 0u))
            fprintf(stderr, "module ref interface variant=%u: status=%u diagnostic=%u\n",
                    index, (unsigned) status, (unsigned) semantic.kind);
        CHECK(status == (index == 0u ? XR_PROGRAM_VERIFY_OK : XR_PROGRAM_VERIFY_SEMANTIC_REJECTED));
        if (index == 1u)
            CHECK(semantic.kind == XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
        if (index == 2u)
            CHECK(semantic.kind == XR_PROGRAM_DIAGNOSTIC_EFFECT);
        xr_validated_program_free(validated);
        xr_program_artifact_free(&artifact);
    }
}

static void test_cross_block_module_array_borrow(void) {
    for (uint32_t variant = 0u; variant < 4u; ++variant) {
        XrProgramArtifact artifact = {0};
        CHECK(xr_program_module_array_borrow_fixture_write(variant, &artifact) == XR_PROGRAM_BUILD_OK);
        if (variant == 1u || variant == 2u)
            expect_semantic_reject(&artifact, variant == 2u
                ? XR_PROGRAM_DIAGNOSTIC_ROOT : XR_PROGRAM_DIAGNOSTIC_VALUE_USE);
        else {
            XrValidatedProgram *validated = validate_ok(&artifact);
            xr_validated_program_free(validated);
        }
        xr_program_artifact_free(&artifact);
    }
}

static void test_module_borrow_after_exchange(void) {
    for (uint32_t variant = 0u; variant < 6u; ++variant) {
        bool released = (variant & 1u) != 0u;
        bool local = variant >= 4u;
        bool alias = variant >= 2u;
        XrProgramModuleFixture fixture;
        xr_program_module_fixture_init(&fixture);
        xr_program_module_fixture_add_slots(&fixture);
        fixture.slots[0][0].type_id = XR_CORE_TYPE_STRING;
        XrCoreIrConstantInput constant = {
            .key = key("module:borrow:text"),
            .type_id = XR_CORE_TYPE_STRING,
            .kind = XR_CORE_IR_CONSTANT_STRING,
            .value.string = {(const uint8_t *) "kept", 4u},
        };
        fixture.modules[0].constants = &constant;
        fixture.modules[0].constant_count = 1u;
        XrCoreIrKey place = key("module:borrow:place");
        XrCoreIrKey initial = key("module:borrow:initial");
        XrCoreIrKey borrow = key("module:borrow:value");
        XrCoreIrKey replacement = key("module:borrow:replacement");
        XrCoreIrKey old = key("module:borrow:old");
        XrCoreIrKey copied = key("module:borrow:copy");
        XrCoreIrKey second_place = key("module:borrow:alias");
        XrCoreIrKey initialize[] = {place, initial};
        XrCoreIrKey exchange[] = {alias ? second_place : place, replacement};
        XrCoreIrInstructionInput instructions[] = {
            {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
             .result = initial,
             .result_type_id = XR_CORE_TYPE_STRING,
             .result_ownership = XR_CORE_IR_OWNER,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
             .immediate.key = constant.key},
            {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE,
             .result = place,
             .result_type_id = XR_CORE_TYPE_STRING,
             .result_category = XR_CORE_IR_PLACE,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
             .immediate.module_slot = {fixture.modules[0].key, fixture.slots[0][0].key}},
            {.operation_id = XR_CORE_OP_CORE_PLACE_INITIALIZE,
             .operands = initialize,
             .operand_count = 2u},
            {.operation_id = XR_CORE_OP_CORE_PLACE_MODULE,
             .result = second_place,
             .result_type_id = XR_CORE_TYPE_STRING,
             .result_category = XR_CORE_IR_PLACE,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_MODULE_SLOT,
             .immediate.module_slot = {fixture.modules[0].key, fixture.slots[0][0].key}},
            {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD,
             .result = borrow,
             .result_type_id = XR_CORE_TYPE_STRING,
             .operands = &place,
             .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
             .result = replacement,
             .result_type_id = XR_CORE_TYPE_STRING,
             .result_ownership = XR_CORE_IR_OWNER,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
             .immediate.key = constant.key},
            {.operation_id = XR_CORE_OP_CORE_PLACE_EXCHANGE,
             .result = old,
             .result_type_id = XR_CORE_TYPE_STRING,
             .result_ownership = XR_CORE_IR_OWNER,
             .operands = exchange,
             .operand_count = 2u},
            {.operation_id = XR_CORE_OP_CORE_OWNER_COPY,
             .result = copied,
             .result_type_id = XR_CORE_TYPE_STRING,
             .result_ownership = XR_CORE_IR_OWNER,
             .operands = &borrow,
             .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &old, .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &copied, .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &initial, .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_RETURN},
        };
        if (local) {
            instructions[1] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
                .result = place,
                .result_type_id = XR_CORE_TYPE_STRING,
                .result_category = XR_CORE_IR_PLACE,
                .operands = &initial,
                .operand_count = 1u,
            };
            instructions[3] = instructions[1];
            instructions[3].result = second_place;
            memmove(&instructions[2], &instructions[3], 9u * sizeof(instructions[0]));
        } else {
            instructions[10] = instructions[11];
        }
        uint32_t copy_index = local ? 6u : 7u;
        if (released) {
            XrCoreIrInstructionInput before = instructions[copy_index];
            instructions[copy_index] = instructions[copy_index + 1u];
            instructions[copy_index + 1u] = before;
        }
        fixture.blocks[0].instructions = instructions;
        fixture.blocks[0].instruction_count = 11u;
        fixture.functions[0].effect_mask = XR_CORE_EFFECT_TRAP;
        XrCoreIrProgram *program = NULL;
        XrProgramArtifact artifact = {0};
        char diagnostic[256] = {0};
        CHECK(xr_core_ir_program_build(&fixture.input, &program, diagnostic, sizeof(diagnostic)) ==
              XR_PROGRAM_BUILD_OK);
        CHECK(xr_program_write(program, &artifact, diagnostic, sizeof(diagnostic)) ==
              XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic semantic = {0};
        XrProgramVerifyStatus status =
            xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, &semantic);
        if ((status == XR_PROGRAM_VERIFY_OK) == released)
            fprintf(stderr, "exchanged borrow variant=%u status=%u diagnostic=%u\n", variant,
                    (unsigned) status, (unsigned) semantic.kind);
        CHECK(status == (released ? XR_PROGRAM_VERIFY_SEMANTIC_REJECTED : XR_PROGRAM_VERIFY_OK));
        if (released)
            CHECK(semantic.kind == XR_PROGRAM_DIAGNOSTIC_VALUE_USE);
        xr_validated_program_free(validated);
        xr_program_artifact_free(&artifact);
        xr_core_ir_program_free(program);
    }
}

static void test_projected_borrow_after_parent_exchange(void) {
    for (uint32_t variant = 0u; variant < 5u; ++variant) {
        bool released = (variant & 1u) != 0u || variant == 4u;
        XrProgramModuleFixture fixture;
        xr_program_module_fixture_init(&fixture);
        uint16_t field = XR_CORE_TYPE_STRING;
        XrCoreIrTypeInput type = {.key = key("borrow-parent-type"),
                                  .local_id = 100u,
                                  .kind = XR_CORE_IR_TYPE_AGGREGATE,
                                  .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
                                  .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
                                  .field_types = &field,
                                  .field_count = 1u};
        fixture.input.types = &type;
        fixture.input.type_count = 1u;
        XrCoreIrConstantInput constant = {
            .key = key("borrow-parent-text"),
            .type_id = field,
            .kind = XR_CORE_IR_CONSTANT_STRING,
            .value.string = {.bytes = (const uint8_t *) "snapshot", .size = 8u}};
        fixture.modules[0].constants = &constant;
        fixture.modules[0].constant_count = 1u;
        XrCoreIrKey initial = key("initial-string"), aggregate = key("initial-aggregate");
        XrCoreIrKey place = key("parent-place"), projected = key("field-place");
        XrCoreIrKey borrow = key("field-borrow"), replacement = key("replacement-string");
        XrCoreIrKey next = key("replacement-aggregate"), old = key("old-aggregate"),
                    copy = key("borrow-copy");
        XrCoreIrKey exchange[] = {place, next};
        XrCoreIrInstructionInput rows[] = {
            {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
             .result = initial,
             .result_type_id = field,
             .result_ownership = XR_CORE_IR_OWNER,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
             .immediate.key = constant.key},
            {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
             .result = aggregate,
             .result_type_id = 100u,
             .result_ownership = XR_CORE_IR_OWNER,
             .operands = &initial,
             .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
             .result = place,
             .result_type_id = 100u,
             .result_category = XR_CORE_IR_PLACE,
             .operands = &aggregate,
             .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_PLACE_PROJECT,
             .result = projected,
             .result_type_id = field,
             .result_category = XR_CORE_IR_PLACE,
             .operands = &place,
             .operand_count = 1u,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
             .immediate.field_ordinal = 0u},
            {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD,
             .result = borrow,
             .result_type_id = field,
             .operands = &projected,
             .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING,
             .result = replacement,
             .result_type_id = field,
             .result_ownership = XR_CORE_IR_OWNER,
             .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
             .immediate.key = constant.key},
            {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
             .result = next,
             .result_type_id = 100u,
             .result_ownership = XR_CORE_IR_OWNER,
             .operands = &replacement,
             .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_PLACE_EXCHANGE,
             .result = old,
             .result_type_id = 100u,
             .result_ownership = XR_CORE_IR_OWNER,
             .operands = exchange,
             .operand_count = 2u},
            {.operation_id = XR_CORE_OP_CORE_OWNER_COPY,
             .result = copy,
             .result_type_id = field,
             .result_ownership = XR_CORE_IR_OWNER,
             .operands = &borrow,
             .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &old, .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_OWNER_DROP, .operands = &copy, .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
             .operands = &aggregate,
             .operand_count = 1u},
            {.operation_id = XR_CORE_OP_CORE_RETURN},
        };
        if (variant >= 2u) {
            XrCoreIrInstructionInput load = rows[4];
            memmove(&rows[4], &rows[5], 3u * sizeof(rows[0]));
            rows[7] = load;
        }
        if (variant == 4u) {
            XrCoreIrInstructionInput load = rows[7];
            rows[7] = rows[9];
            rows[9] = rows[8];
            rows[8] = load;
        } else if (released) {
            XrCoreIrInstructionInput saved = rows[8];
            rows[8] = rows[9];
            rows[9] = saved;
        }
        fixture.blocks[0].instructions = rows;
        fixture.blocks[0].instruction_count = sizeof(rows) / sizeof(rows[0]);
        XrCoreIrProgram *program = NULL;
        XrProgramArtifact artifact = {0};
        char reason[256] = {0};
        XrProgramBuildStatus built =
            xr_core_ir_program_build(&fixture.input, &program, reason, sizeof(reason));
        if (built != XR_PROGRAM_BUILD_OK)
            fprintf(stderr, "projected fixture: %s\n", reason);
        CHECK(built == XR_PROGRAM_BUILD_OK);
        CHECK(xr_program_write(program, &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic diagnostic = {0};
        XrProgramVerifyStatus status =
            xr_program_validate(artifact.bytes, artifact.size, NULL, &validated, &diagnostic);
        fprintf(stderr, "projected borrow variant=%u status=%u diagnostic=%u\n", variant, status,
                diagnostic.kind);
        CHECK(status == (released ? XR_PROGRAM_VERIFY_SEMANTIC_REJECTED : XR_PROGRAM_VERIFY_OK));
        if (released)
            CHECK(diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_VALUE_USE);
        xr_validated_program_free(validated);
        xr_program_artifact_free(&artifact);
        xr_core_ir_program_free(program);
    }
}
