/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_program_aot_class.inc.c - Private AOT class-reference fixtures
 *
 * KEY CONCEPT: One canonical class program is observed through Reference,
 * BackendIR, and a separately compiled generated-C executable.  The test
 * normalizes only opaque identities; values, event order, and reclamation are
 * checked before a differential record is published.
 */

#define XR_H2_CLASS_TYPE UINT16_C(42)

static const char XR_H2_AOT_BACKEND_RECORD[] =
    "{\"schema\":1,\"executor\":\"aot-backendir\",\"route\":\"aot-backendir\","
    "\"oracle\":{\"scenario\":\"class-alias-mutation-lifecycle\","
    "\"outcome\":{\"kind\":\"return\"},\"value\":{\"kind\":\"i64\",\"data\":42},"
    "\"identities\":{\"constructed\":\"class-0\",\"shared\":\"class-0\"},"
    "\"events\":[{\"kind\":\"class-construct\",\"identity\":\"class-0\"},"
    "{\"kind\":\"class-share\",\"identity\":\"class-0\",\"related\":\"class-0\"},"
    "{\"kind\":\"class-field-place\",\"identity\":\"class-0\",\"field\":0},"
    "{\"kind\":\"place-exchange\",\"type\":\"i64\","
    "\"old\":{\"value\":7,\"identity\":\"none\"},"
    "\"replacement\":{\"value\":42,\"identity\":\"none\"}},"
    "{\"kind\":\"class-field-load\",\"identity\":\"class-0\",\"field\":0},"
    "{\"kind\":\"owner-drop\",\"identity\":\"class-0\"},"
    "{\"kind\":\"owner-drop\",\"identity\":\"class-0\"},"
    "{\"kind\":\"class-finalize\",\"identity\":\"class-0\"},"
    "{\"kind\":\"class-reclaim\",\"identity\":\"class-0\"}]}}";

static const char XR_H2_AOT_NATIVE_RECORD[] =
    "{\"schema\":1,\"executor\":\"aot-generated-c-native\","
    "\"route\":\"aot-generated-c-strict-native-host-run\","
    "\"oracle\":{\"scenario\":\"class-alias-mutation-lifecycle\","
    "\"outcome\":{\"kind\":\"return\"},\"value\":{\"kind\":\"i64\",\"data\":42},"
    "\"identities\":{\"constructed\":\"class-0\",\"shared\":\"class-0\"},"
    "\"events\":[{\"kind\":\"class-construct\",\"identity\":\"class-0\"},"
    "{\"kind\":\"class-share\",\"identity\":\"class-0\",\"related\":\"class-0\"},"
    "{\"kind\":\"class-field-place\",\"identity\":\"class-0\",\"field\":0},"
    "{\"kind\":\"place-exchange\",\"type\":\"i64\","
    "\"old\":{\"value\":7,\"identity\":\"none\"},"
    "\"replacement\":{\"value\":42,\"identity\":\"none\"}},"
    "{\"kind\":\"class-field-load\",\"identity\":\"class-0\",\"field\":0},"
    "{\"kind\":\"owner-drop\",\"identity\":\"class-0\"},"
    "{\"kind\":\"owner-drop\",\"identity\":\"class-0\"},"
    "{\"kind\":\"class-finalize\",\"identity\":\"class-0\"},"
    "{\"kind\":\"class-reclaim\",\"identity\":\"class-0\"}]}}";

typedef struct XrH2LifecycleLog {
    XrReferenceLifecycleEvent events[9];
    uint32_t count;
} XrH2LifecycleLog;

static void xr_h2_record_lifecycle(void *context, const XrReferenceLifecycleEvent *event) {
    XrH2LifecycleLog *log = context;
    if (log && event && log->count < 9u)
        log->events[log->count++] = *event;
}

static XrValidatedProgram *build_class_reference_program_with_alias(uint16_t alias_operation) {
    uint16_t fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = fixture_key("aot-class:type"),
        .local_id = XR_H2_CLASS_TYPE,
        .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
        .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = fields,
        .field_count = 1u,
    };
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("aot-class:constant:7"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 7},
        {.key = fixture_key("aot-class:constant:42"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 42},
    };
    XrCoreIrKey seven = fixture_key("aot-class:value:7");
    XrCoreIrKey forty_two = fixture_key("aot-class:value:42");
    XrCoreIrKey original = fixture_key("aot-class:value:original");
    XrCoreIrKey alias = fixture_key("aot-class:value:alias");
    XrCoreIrKey place = fixture_key("aot-class:value:place");
    XrCoreIrKey old = fixture_key("aot-class:value:old");
    XrCoreIrKey loaded = fixture_key("aot-class:value:loaded");
    XrCoreIrKey construct_operands[] = {seven};
    XrCoreIrKey original_operand[] = {original};
    XrCoreIrKey alias_operand[] = {alias};
    XrCoreIrKey exchange_operands[] = {place, forty_two};
    XrCoreIrKey returned[] = {loaded};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = seven,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = forty_two,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
         .result = original,
         .result_type_id = XR_H2_CLASS_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = alias_operation,
         .result = alias,
         .result_type_id = XR_H2_CLASS_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = original_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CLASS_FIELD_PLACE,
         .result = place,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_PLACE,
         .operands = alias_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_PLACE_EXCHANGE,
         .result = old,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = exchange_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CLASS_FIELD_LOAD,
         .result = loaded,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = original_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = alias_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = original_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("aot-class:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("aot-class:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_program(&type, 1u, constants, 2u, &function, 1u);
}

static XrValidatedProgram *build_class_reference_program(void) {
    return build_class_reference_program_with_alias(XR_CORE_OP_CORE_CLASS_SHARE);
}

static XrValidatedProgram *build_class_reference_copy_program(void) {
    return build_class_reference_program_with_alias(XR_CORE_OP_CORE_OWNER_COPY);
}

static XrValidatedProgram *build_class_ref_coroutine_program(void) {
    (void) xr_program_ref_coroutine_fixture_write;
    XrProgramRefCoroutineChild child;
    xr_program_ref_coroutine_child_init(&child);
    XrProgramRefCoroutineParent parent;
    xr_program_ref_coroutine_parent_init(&parent, child.function.key);
    for (uint32_t field = 0u; field < 2u; ++field) {
        parent.entry_instructions[1u + field] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CLASS_FIELD_PLACE,
            .result = parent.call_operands[field],
            .result_type_id = XR_CORE_TYPE_I64,
            .result_category = XR_CORE_IR_PLACE,
            .operands = parent.owner_operand,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
            .immediate.field_ordinal = field,
        };
    }
    parent.entry_instructions[3] = parent.entry_instructions[4];
    parent.blocks[0].instruction_count = 4u;
    uint16_t fields[] = {XR_CORE_TYPE_I64, XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = fixture_key("aot-class-ref-coroutine:type"),
        .local_id = XR_PROGRAM_REF_COROUTINE_AGGREGATE,
        .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
        .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = fields,
        .field_count = 2u,
    };
    XrCoreIrInstructionInput main_return = {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
    };
    XrCoreIrBlockInput main_block = {
        .key = fixture_key("aot-class-ref-coroutine:main:block"),
        .instructions = &main_return,
        .instruction_count = 1u,
    };
    XrCoreIrFunctionInput main_function = {
        .key = fixture_key("aot-class-ref-coroutine:main"),
        .result_type_id = XR_CORE_TYPE_VOID,
        .entry_block = main_block.key,
        .blocks = &main_block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    parent.function.flags = 0u;
    XrCoreIrFunctionInput functions[] = {main_function, parent.function, child.function};
    return validate_program(&type, 1u, NULL, 0u, functions, 3u);
}

static XrBackendInstruction *find_class_ref_coroutine_operation(XrBackendFunction *function,
                                                                uint16_t operation,
                                                                uint32_t ordinal) {
    uint32_t found = 0u;
    XrBackendInstruction *result = NULL;
    for (uint32_t block = 0u; block < function->block_count; ++block) {
        for (uint32_t instruction = 0u;
             instruction < function->blocks[block].instruction_count; ++instruction) {
            XrBackendInstruction *candidate = &function->blocks[block].instructions[instruction];
            if (candidate->operation_id != operation || found++ != ordinal)
                continue;
            result = candidate;
        }
    }
    return result;
}

static uint32_t count_c_fragment(const char *text, const char *fragment) {
    uint32_t count = 0u;
    size_t length = strlen(fragment);
    for (const char *found = strstr(text, fragment); found;
         found = strstr(found + length, fragment))
        ++count;
    return count;
}

static XrBackendFunction *find_class_ref_coroutine_parent(XrBackendIR *ir) {
    for (uint32_t function = 0u; function < ir->function_count; ++function) {
        XrBackendFunction *candidate = &ir->functions[function];
        if (find_class_ref_coroutine_operation(
                candidate, XR_CORE_OP_CORE_COROUTINE_CALL_SEALED, 0u))
            return candidate;
    }
    return NULL;
}

static void test_class_ref_coroutine_frame_root(void) {
    XrValidatedProgram *program = build_class_ref_coroutine_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrBackendFunction *parent = find_class_ref_coroutine_parent(ir);
    REQUIRE(parent != NULL);
    XrBackendInstruction *left = find_class_ref_coroutine_operation(
        parent, XR_CORE_OP_CORE_CLASS_FIELD_PLACE, 0u);
    XrBackendInstruction *right = find_class_ref_coroutine_operation(
        parent, XR_CORE_OP_CORE_CLASS_FIELD_PLACE, 1u);
    XrBackendInstruction *call = find_class_ref_coroutine_operation(
        parent, XR_CORE_OP_CORE_COROUTINE_CALL_SEALED, 0u);
    REQUIRE(left && right && call && parent->coroutine_safepoint_count == 1u);
    XrBackendCoroutineSafepoint *point = &parent->coroutine_safepoints[0];
    REQUIRE(point->live_value_count == 1u && call->operand_count == 4u);
    uint32_t owner = point->live_value_ids[0];
    REQUIRE(left->operands[0] == owner && right->operands[0] == owner);

    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    XrBackendStatus emission = xr_backend_ir_emit_c(ir, false, &generated, &diagnostic);
    if (emission != XR_BACKEND_OK)
        fprintf(stderr, "class ref coroutine emission failed: status=%s op=%u f=%u b=%u i=%u\n",
                xr_backend_status_name(emission), diagnostic.operation_id,
                diagnostic.function_id, diagnostic.block_id, diagnostic.instruction_id);
    REQUIRE(emission == XR_BACKEND_OK);
    char fragment[128];
    uint16_t class_type_id = parent->value_types[owner];
    (void) snprintf(fragment, sizeof(fragment), "XrAotType%u live_0_0;",
                    class_type_id);
    REQUIRE(count_c_fragment(generated.bytes, fragment) == 1u);
    (void) snprintf(fragment, sizeof(fragment), "XrAotType%u live_0_1;", class_type_id);
    REQUIRE(strstr(generated.bytes, fragment) == NULL);
    (void) snprintf(fragment, sizeof(fragment), "v%u = &frame->live_0_0->f0;",
                    left->result_id);
    REQUIRE(count_c_fragment(generated.bytes, fragment) == 1u);
    (void) snprintf(fragment, sizeof(fragment), "v%u = &frame->live_0_0->f1;",
                    right->result_id);
    REQUIRE(count_c_fragment(generated.bytes, fragment) == 1u);
    (void) snprintf(fragment, sizeof(fragment), "v%u = frame->live_0_0;", owner);
    REQUIRE(count_c_fragment(generated.bytes, fragment) != 0u);
    (void) snprintf(fragment, sizeof(fragment),
                    "v%u = frame->live_0_0;\n        v%u = frame->live_0_0;", owner,
                    owner);
    REQUIRE(strstr(generated.bytes, fragment) == NULL);
    (void) snprintf(fragment, sizeof(fragment),
                    "frame->live_0_0 = v%u;\n        v%u = &frame->live_0_0->f0;", owner,
                    left->result_id);
    REQUIRE(count_c_fragment(generated.bytes, fragment) == 1u);

    uint32_t saved_live_operand = call->operands[2];
    call->operands[2] = left->result_id;
    require_coroutine_backend_rejected(ir, "missing class owner live root");
    call->operands[2] = saved_live_operand;
    require_coroutine_backend_restored(ir);

    uint32_t duplicated_live[] = {owner, owner};
    uint32_t *saved_live_values = point->live_value_ids;
    point->live_value_ids = duplicated_live;
    point->live_value_count = 2u;
    require_coroutine_backend_rejected(ir, "duplicate class owner live root");
    point->live_value_ids = saved_live_values;
    point->live_value_count = 1u;
    require_coroutine_backend_restored(ir);

    uint32_t saved_ordinal = right->immediate.field_ordinal;
    right->immediate.field_ordinal = 2u;
    require_coroutine_backend_rejected(ir, "class ref ordinal");
    right->immediate.field_ordinal = saved_ordinal;
    require_coroutine_backend_restored(ir);

    uint16_t saved_result_type = right->result_type_id;
    uint16_t saved_value_type = parent->value_types[right->result_id];
    uint8_t saved_representation = parent->value_representations[right->result_id];
    right->result_type_id = XR_CORE_TYPE_BOOL;
    parent->value_types[right->result_id] = XR_CORE_TYPE_BOOL;
    parent->value_representations[right->result_id] = XR_BACKEND_VALUE_BOOL_U8;
    require_coroutine_backend_rejected(ir, "class ref field type");
    right->result_type_id = saved_result_type;
    parent->value_types[right->result_id] = saved_value_type;
    parent->value_representations[right->result_id] = saved_representation;
    require_coroutine_backend_restored(ir);

    uint32_t saved_root = right->operands[0];
    right->operands[0] = left->result_id;
    require_coroutine_backend_rejected(ir, "class ref non-owner root");
    right->operands[0] = saved_root;
    require_coroutine_backend_restored(ir);

    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static XrValidatedProgram *build_nested_class_reference_program(void) {
    uint16_t leaf_fields[] = {XR_CORE_TYPE_I64};
    uint16_t parent_fields[] = {XR_H2_CLASS_TYPE};
    XrCoreIrTypeInput types[] = {
        {.key = fixture_key("aot-class-finalize:type:leaf"),
         .local_id = XR_H2_CLASS_TYPE,
         .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
         .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
         .field_types = leaf_fields,
         .field_count = 1u},
        {.key = fixture_key("aot-class-finalize:type:parent"),
         .local_id = UINT16_C(43),
         .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
         .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
         .field_types = parent_fields,
         .field_count = 1u},
    };
    XrCoreIrConstantInput constant = {
        .key = fixture_key("aot-class-finalize:constant"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey scalar = fixture_key("aot-class-finalize:value:scalar");
    XrCoreIrKey leaf = fixture_key("aot-class-finalize:value:leaf");
    XrCoreIrKey parent = fixture_key("aot-class-finalize:value:parent");
    XrCoreIrKey leaf_operand[] = {leaf};
    XrCoreIrKey parent_operand[] = {parent};
    XrCoreIrKey scalar_operand[] = {scalar};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = scalar,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
         .result = leaf,
         .result_type_id = XR_H2_CLASS_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = scalar_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
         .result = parent,
         .result_type_id = UINT16_C(43),
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = leaf_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = parent_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = scalar_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("aot-class-finalize:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("aot-class-finalize:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_program(types, 2u, &constant, 1u, &function, 1u);
}

static void require_class_field_finalization_lowering(const XrTargetProfile *profile) {
    XrValidatedProgram *program = build_nested_class_reference_program();
    XrH2LifecycleLog log = {0};
    XrReferenceProviderBinding binding = {
        .lifecycle_context = &log,
        .lifecycle_event = xr_h2_record_lifecycle,
    };
    XrReferenceOutcome outcome = xr_reference_evaluate_bound(
        program, xr_validated_program_entry_function(program), NULL, 0u, NULL, NULL, &binding);
    REQUIRE(outcome.kind == XR_REFERENCE_OUTCOME_RETURN && outcome.value.as.i64 == 42);
    REQUIRE(log.count == 8u);
    REQUIRE(log.events[4].kind == XR_REFERENCE_EVENT_OWNER_DROP &&
            log.events[4].origin == XR_REFERENCE_EVENT_ORIGIN_FIELD_FINALIZATION);
    REQUIRE(log.events[5].kind == XR_REFERENCE_EVENT_CLASS_FINALIZE &&
            log.events[5].origin == XR_REFERENCE_EVENT_ORIGIN_FIELD_FINALIZATION);
    REQUIRE(log.events[6].kind == XR_REFERENCE_EVENT_CLASS_RECLAIM &&
            log.events[6].origin == XR_REFERENCE_EVENT_ORIGIN_FIELD_FINALIZATION);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
    char drop[96];
    (void) snprintf(drop, sizeof(drop),
                    "xr_aot_class_drop_%u(xr_ctx, value->f0, UINT32_C(2));",
                    program->types[0].type_id);
    REQUIRE(strstr(generated.bytes, drop) != NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_reference_outcome_dispose(&outcome);
    xr_validated_program_free(program);
}

static void require_class_reference_oracle(const XrValidatedProgram *program) {
    XrH2LifecycleLog log = {0};
    XrReferenceProviderBinding binding = {
        .lifecycle_context = &log,
        .lifecycle_event = xr_h2_record_lifecycle,
    };
    XrReferenceOutcome outcome = xr_reference_evaluate_bound(
        program, xr_validated_program_entry_function(program), NULL, 0u, NULL, NULL, &binding);
    const XrReferenceLifecycleEventKind kinds[] = {
        XR_REFERENCE_EVENT_CLASS_CONSTRUCT, XR_REFERENCE_EVENT_CLASS_SHARE,
        XR_REFERENCE_EVENT_CLASS_FIELD_PLACE, XR_REFERENCE_EVENT_PLACE_EXCHANGE,
        XR_REFERENCE_EVENT_CLASS_FIELD_LOAD, XR_REFERENCE_EVENT_OWNER_DROP,
        XR_REFERENCE_EVENT_OWNER_DROP, XR_REFERENCE_EVENT_CLASS_FINALIZE,
        XR_REFERENCE_EVENT_CLASS_RECLAIM,
    };
    REQUIRE(outcome.kind == XR_REFERENCE_OUTCOME_RETURN);
    REQUIRE(outcome.value.kind == XR_REFERENCE_VALUE_I64 && outcome.value.as.i64 == 42);
    REQUIRE(log.count == 9u && log.events[0].identity != UINT64_MAX);
    uint16_t class_type_id = program->types[0].type_id;
    for (uint32_t index = 0u; index < log.count; ++index) {
        REQUIRE(log.events[index].kind == kinds[index]);
        REQUIRE(log.events[index].origin == XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION);
        REQUIRE(log.events[index].type_id ==
                (index == 3u ? XR_CORE_TYPE_I64 : class_type_id));
        if (index != 3u)
            REQUIRE(log.events[index].identity == log.events[0].identity);
    }
    REQUIRE(log.events[1].related_identity == log.events[0].identity);
    REQUIRE(log.events[2].field_ordinal == 0u && log.events[4].field_ordinal == 0u);
    REQUIRE(log.events[3].identity == UINT64_MAX &&
            log.events[3].related_identity == UINT64_MAX);
    xr_reference_outcome_dispose(&outcome);
}

static bool append_c_string_literal(FILE *output, const char *text) {
    if (!output || !text || fputc('"', output) == EOF)
        return false;
    for (const unsigned char *cursor = (const unsigned char *) text; *cursor; ++cursor) {
        if ((*cursor == '"' || *cursor == '\\') && fputc('\\', output) == EOF)
            return false;
        if (fputc(*cursor, output) == EOF)
            return false;
    }
    return fputc('"', output) != EOF;
}

static bool append_class_native_harness(FILE *output, uint32_t entry_function,
                                        uint16_t class_type_id) {
    if (fprintf(output,
                "\n#include <stdio.h>\n"
               "typedef struct XrH2NativeLog { XrAotLifecycleEvent events[9]; "
               "uint32_t count; } XrH2NativeLog;\n"
               "static void xr_h2_native_event(void *opaque, const XrAotLifecycleEvent *event) "
               "{\n"
               "    XrH2NativeLog *log = (XrH2NativeLog *)opaque;\n"
               "    if (log && event && log->count < UINT32_C(9)) "
               "log->events[log->count++] = *event;\n"
               "}\n"
               "int main(void) {\n"
               "    static const uint32_t kinds[9] = {UINT32_C(1), UINT32_C(2), "
               "UINT32_C(5), UINT32_C(6), UINT32_C(4), UINT32_C(7), UINT32_C(7), "
               "UINT32_C(8), UINT32_C(9)};\n"
               "    XrH2NativeLog log = {0};\n"
               "    XrAotContext context = {.lifecycle_context = &log, "
               ".lifecycle_event = xr_h2_native_event};\n"
               "    XrAotOutcome outcome = xr_aot_fn_%u(&context);\n"
               "    if (outcome.kind != UINT32_C(0) || outcome.value_kind != UINT32_C(2) || "
               "outcome.i64 != INT64_C(42) || log.count != UINT32_C(9) || "
               "context.allocations != NULL) return 10;\n"
               "    uint64_t identity = log.events[0].identity;\n"
               "    if (identity == UINT64_MAX) return 11;\n"
               "    for (uint32_t index = 0; index < UINT32_C(9); ++index) {\n"
               "        if (log.events[index].kind != kinds[index] || "
               "log.events[index].origin != UINT32_C(1) || "
               "log.events[index].type_id != (index == UINT32_C(3) ? "
               "UINT16_C(2) : UINT16_C(%u))) return 12;\n"
               "        if (index != UINT32_C(3) && log.events[index].identity != identity) "
               "return 13;\n"
               "    }\n"
               "    if (log.events[1].related_identity != identity || "
               "log.events[2].field_ordinal != UINT32_C(0) || "
               "log.events[4].field_ordinal != UINT32_C(0)) return 14;\n"
               "    if (log.events[3].identity != UINT64_MAX || "
               "log.events[3].related_identity != UINT64_MAX || "
               "!log.events[3].has_i64_exchange || "
               "log.events[3].old_i64 != INT64_C(7) || "
               "log.events[3].replacement_i64 != INT64_C(42)) return 15;\n"
               "    xr_aot_context_destroy(&context);\n"
               "    puts(",
                entry_function, class_type_id) <= 0 ||
        !append_c_string_literal(output, XR_H2_AOT_NATIVE_RECORD))
        return false;
    return fprintf(output,
               ");\n"
               "    return 0;\n"
               "}\n") > 0;
}

static void require_native_record_c_literal_escaping(void) {
    FILE *output = tmpfile();
    REQUIRE(output != NULL);
    REQUIRE(append_c_string_literal(output, "{\"schema\":1}"));
    REQUIRE(fflush(output) == 0 && fseek(output, 0, SEEK_SET) == 0);
    char bytes[32] = {0};
    REQUIRE(fread(bytes, 1u, sizeof(bytes) - 1u, output) == strlen("\"{\\\"schema\\\":1}\""));
    REQUIRE(strcmp(bytes, "\"{\\\"schema\\\":1}\"") == 0);
    REQUIRE(fclose(output) == 0);
}

static bool write_class_native_source(const char *path) {
    XrValidatedProgram *program = build_class_reference_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrBackendIR *ir = profile ? build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE) : NULL;
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    bool written = ir && xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK;
    FILE *output = written ? fopen(path, "wb") : NULL;
    if (output) {
        written = fwrite(generated.bytes, 1u, generated.size, output) == generated.size &&
                  append_class_native_harness(output,
                                              xr_validated_program_entry_function(program),
                                              program->types[0].type_id);
        bool closed = fclose(output) == 0;
        written = written && closed;
    } else {
        written = false;
    }
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    return written;
}

static bool file_is_empty(const char *path) {
    FILE *input = fopen(path, "rb");
    if (!input)
        return false;
    bool empty = fgetc(input) == EOF;
    (void) fclose(input);
    return empty;
}

static void report_h2_file(const char *label, const char *path) {
    FILE *input = fopen(path, "rb");
    fprintf(stderr, "generated-C native %s (%s):\n", label, path);
    if (!input) {
        fputs("<unavailable>\n", stderr);
        return;
    }
    uint8_t bytes[1024];
    size_t size = 0u;
    while ((size = fread(bytes, 1u, sizeof(bytes), input)) != 0u)
        (void) fwrite(bytes, 1u, size, stderr);
    (void) fclose(input);
    fputc('\n', stderr);
}

static bool publish_exact_file(const char *path, const char *expected) {
    FILE *input = fopen(path, "rb");
    if (!input)
        return false;
    size_t expected_size = strlen(expected);
    char *bytes = xr_malloc(expected_size + 3u);
    size_t size = bytes ? fread(bytes, 1u, expected_size + 2u, input) : 0u;
    bool one_line =
        (size == expected_size + 1u && bytes[expected_size] == '\n') ||
        (size == expected_size + 2u && bytes[expected_size] == '\r' &&
         bytes[expected_size + 1u] == '\n');
    bool exact = bytes && one_line && fgetc(input) == EOF &&
                 memcmp(bytes, expected, expected_size) == 0;
    (void) fclose(input);
    if (exact)
        exact = puts(expected) != EOF;
    xr_free(bytes);
    return exact;
}

static bool run_class_generated_c_native(void) {
    char source[96];
    char executable[96];
    char output[96];
    char errors[96];
    char compile_log[96];
    int pid = xr_test_getpid();
    (void) snprintf(source, sizeof(source), "xr-h2-class-%d.c", pid);
#ifdef _WIN32
    (void) snprintf(executable, sizeof(executable), "xr-h2-class-%d.exe", pid);
    const char *default_compiler = "C:/Program Files/LLVM/bin/clang.exe";
#else
    (void) snprintf(executable, sizeof(executable), "xr-h2-class-%d", pid);
    const char *default_compiler = "cc";
#endif
    (void) snprintf(output, sizeof(output), "xr-h2-class-%d.out", pid);
    (void) snprintf(errors, sizeof(errors), "xr-h2-class-%d.err", pid);
    (void) snprintf(compile_log, sizeof(compile_log), "xr-h2-class-%d.compile", pid);
    (void) remove(source);
    (void) remove(executable);
    (void) remove(output);
    (void) remove(errors);
    (void) remove(compile_log);
    const char *configured = getenv("XRAY_H2_C_COMPILER");
    if (!configured || !configured[0])
        configured = getenv("CC");
    const char *compiler = configured && configured[0] ? configured : default_compiler;
    char command[640];
#ifdef _WIN32
    int length = snprintf(
        command, sizeof(command),
        "\"%s\" -std=c11 -pedantic-errors -Wall -Wextra -Werror -fuse-ld=lld "
        "%s -o %s >%s 2>&1",
        compiler, source, executable, compile_log);
#else
    int length = snprintf(command, sizeof(command),
                          "\"%s\" -std=c11 -pedantic-errors -Wall -Wextra -Werror "
                          "%s -o %s >%s 2>&1",
                          compiler, source, executable, compile_log);
#endif
    bool source_written = write_class_native_source(source);
    int compile_status = source_written && length > 0 && (size_t) length < sizeof(command)
                             ? system(command)
                             : -1;
    bool success = compile_status == 0;
    if (!source_written)
        fputs("generated-C native source emission failed\n", stderr);
    else if (compile_status != 0) {
        fprintf(stderr, "generated-C native compile exit=%d command=%s\n", compile_status,
                command);
        report_h2_file("compiler output", compile_log);
    }
    int run_status = -1;
#ifdef _WIN32
    if (success) {
        length = snprintf(command, sizeof(command), "%s >%s 2>%s", executable, output, errors);
        run_status = length > 0 && (size_t) length < sizeof(command) ? system(command) : -1;
        success = run_status == 0;
    }
#else
    if (success) {
        length = snprintf(command, sizeof(command), "./%s >%s 2>%s", executable, output, errors);
        run_status = length > 0 && (size_t) length < sizeof(command) ? system(command) : -1;
        success = run_status == 0;
    }
#endif
    if (compile_status == 0 && run_status != 0) {
        fprintf(stderr, "generated-C native execution exit=%d command=%s\n", run_status,
                command);
        report_h2_file("stdout", output);
        report_h2_file("stderr", errors);
    }
    if (success && !file_is_empty(errors)) {
        report_h2_file("unexpected stderr", errors);
        success = false;
    }
    if (success && !publish_exact_file(output, XR_H2_AOT_NATIVE_RECORD)) {
        report_h2_file("non-exact stdout", output);
        success = false;
    }
    (void) remove(source);
    (void) remove(executable);
    (void) remove(output);
    (void) remove(errors);
    (void) remove(compile_log);
    return success;
}
