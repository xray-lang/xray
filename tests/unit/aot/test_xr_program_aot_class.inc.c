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

static bool append_class_native_harness(FILE *output, uint32_t entry_function,
                                        uint16_t class_type_id) {
    return fprintf(
               output,
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
               "    puts(\"%s\");\n"
               "    return 0;\n"
               "}\n",
               entry_function, class_type_id, XR_H2_AOT_NATIVE_RECORD) > 0;
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
    const char *default_compiler = "clang";
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
    const char *configured = getenv("CC");
    const char *compiler = configured && configured[0] ? configured : default_compiler;
    char command[640];
    int length = snprintf(command, sizeof(command),
                          "%s -std=c11 -pedantic-errors -Wall -Wextra -Werror %s -o %s >%s 2>&1",
                          compiler, source, executable, compile_log);
    bool success = length > 0 && (size_t) length < sizeof(command) &&
                   write_class_native_source(source) && system(command) == 0;
#ifdef _WIN32
    if (success) {
        length = snprintf(command, sizeof(command), "%s >%s 2>%s", executable, output, errors);
        success = length > 0 && (size_t) length < sizeof(command) && system(command) == 0;
    }
#else
    if (success) {
        length = snprintf(command, sizeof(command), "./%s >%s 2>%s", executable, output, errors);
        success = length > 0 && (size_t) length < sizeof(command) && system(command) == 0;
    }
#endif
    if (success)
        success = file_is_empty(errors) && publish_exact_file(output, XR_H2_AOT_NATIVE_RECORD);
    (void) remove(source);
    (void) remove(executable);
    (void) remove(output);
    (void) remove(errors);
    (void) remove(compile_log);
    return success;
}
