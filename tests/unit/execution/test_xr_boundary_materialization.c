/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_boundary_materialization.c - Program/profile boundary layout KATs
 */

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/execution/xr_boundary_materialization.h"
#include "../../../src/program/xr_program.h"
#include "../plan/target_profile_test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

enum {
    TEST_AGGREGATE_TYPE = 101,
    TEST_VARIANT_TYPE = 77,
};

typedef struct TestProviderBindings {
    XrProviderBinding providers[XR_RUNTIME_ABI_MAX_PROVIDERS];
    XrProviderOperationBinding operations[XR_RUNTIME_ABI_MAX_PROVIDERS]
                                         [XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS];
    size_t count;
} TestProviderBindings;

static XrCoreIrKey test_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrValidatedProgram *build_boundary_program(void) {
    uint16_t aggregate_fields[] = {XR_CORE_TYPE_BOOL, XR_CORE_TYPE_I64, XR_CORE_TYPE_U32};
    uint16_t variant_one[] = {XR_CORE_TYPE_BOOL, XR_CORE_TYPE_I64};
    uint16_t variant_two[] = {XR_CORE_TYPE_U32};
    XrCoreIrVariantInput variants[] = {
        {0},
        {.payload_types = variant_one, .payload_count = 2u},
        {.payload_types = variant_two, .payload_count = 1u},
    };
    XrCoreIrTypeInput types[] = {
        {
            .key = test_key("boundary:type:aggregate"),
            .local_id = TEST_AGGREGATE_TYPE,
            .kind = XR_CORE_IR_TYPE_AGGREGATE,
            .field_types = aggregate_fields,
            .field_count = 3u,
        },
        {
            .key = test_key("boundary:type:variant"),
            .local_id = TEST_VARIANT_TYPE,
            .kind = XR_CORE_IR_TYPE_VARIANT,
            .variants = variants,
            .variant_count = 3u,
        },
    };
    XrCoreIrKey aggregate_argument = test_key("boundary:value:aggregate-argument");
    XrCoreIrKey variant_argument = test_key("boundary:value:variant-argument");
    XrCoreIrKey u16_argument = test_key("boundary:value:u16-argument");
    XrCoreIrValueInput arguments[] = {
        {.key = aggregate_argument, .type_id = TEST_AGGREGATE_TYPE},
        {.key = variant_argument, .type_id = TEST_VARIANT_TYPE},
        {.key = u16_argument, .type_id = XR_CORE_TYPE_U16},
    };
    XrCoreIrKey return_operands[] = {u16_argument};
    XrCoreIrInstructionInput instruction = {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = return_operands,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    XrCoreIrKey block_key = test_key("boundary:block:entry");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .arguments = arguments,
        .argument_count = 3u,
        .instructions = &instruction,
        .instruction_count = 1u,
    };
    uint16_t parameters[] = {TEST_AGGREGATE_TYPE, TEST_VARIANT_TYPE, XR_CORE_TYPE_U16};
    XrCoreIrFunctionInput function = {
        .key = test_key("boundary:function:entry"),
        .parameter_types = parameters,
        .parameter_count = 3u,
        .result_type_id = XR_CORE_TYPE_U16,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = test_key("boundary:module"),
        .functions = &function,
        .function_count = 1u,
    };
    XrCoreIrKey semantic_profile = test_key("boundary:semantic-profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic_profile.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .types = types,
        .type_count = 2u,
        .modules = &module,
        .module_count = 1u,
    };
    XrCoreIrProgram *core = NULL;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    char diagnostic[256] = {0};
    REQUIRE(xr_core_ir_program_build(&input, &core, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_write(core, &artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(core);
    REQUIRE(program != NULL);
    return program;
}

static void build_provider_bindings(const XrTargetProfile *profile,
                                    TestProviderBindings *bindings) {
    (void) profile;
    memset(bindings, 0, sizeof(*bindings));
}

static XrInstance *create_instance(XrValidatedProgram *program, XrTargetProfile *profile) {
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = bindings.count ? bindings.providers : NULL,
        .provider_count = bindings.count,
        .generation = 1u,
    };
    XrExecutionDiagnostic diagnostic;
    XrInstance *instance = NULL;
    REQUIRE(xr_execution_instance_create(&input, &instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(instance != NULL);
    return instance;
}

static void retire_and_free(XrInstance **instance) {
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(*instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(*instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(instance, &diagnostic) == XR_EXECUTION_OK);
}

static void require_fingerprint(XrFingerprint fingerprint, const char *expected) {
    char actual[XR_FINGERPRINT_BYTES * 2u + 1u];
    xr_fingerprint_hex(fingerprint, actual);
    if (strcmp(actual, expected) != 0)
        fprintf(stderr, "fingerprint mismatch: expected %s, got %s\n", expected, actual);
    REQUIRE(strcmp(actual, expected) == 0);
}

static bool fingerprint_is_nonzero(XrFingerprint fingerprint) {
    uint8_t combined = 0;
    for (uint32_t index = 0; index < sizeof(fingerprint.bytes); ++index)
        combined |= fingerprint.bytes[index];
    return combined != 0u;
}

static void require_aggregate_layout(const XrBoundaryTypeLayout *layout) {
    REQUIRE(layout != NULL);
    REQUIRE(layout->layout_kind == XR_BOUNDARY_TYPE_LAYOUT_AGGREGATE);
    REQUIRE(layout->cleanup_kind == XR_BOUNDARY_CLEANUP_TRIVIAL);
    REQUIRE(layout->size == 24u && layout->alignment == 8u);
    REQUIRE(layout->field_count == 3u && layout->variant_count == 0u);
    REQUIRE(layout->root_count == 0u && layout->roots == NULL);
    REQUIRE(layout->fields[0].type_id == XR_CORE_TYPE_BOOL);
    REQUIRE(layout->fields[0].offset == 0u && layout->fields[0].size == 1u);
    REQUIRE(layout->fields[1].type_id == XR_CORE_TYPE_I64);
    REQUIRE(layout->fields[1].offset == 8u && layout->fields[1].size == 8u);
    REQUIRE(layout->fields[2].type_id == XR_CORE_TYPE_U32);
    REQUIRE(layout->fields[2].offset == 16u && layout->fields[2].size == 4u);
}

static void require_variant_layout(const XrBoundaryTypeLayout *layout) {
    REQUIRE(layout != NULL);
    REQUIRE(layout->layout_kind == XR_BOUNDARY_TYPE_LAYOUT_VARIANT);
    REQUIRE(layout->cleanup_kind == XR_BOUNDARY_CLEANUP_TRIVIAL);
    REQUIRE(layout->size == 24u && layout->alignment == 8u);
    REQUIRE(layout->tag_offset == 0u && layout->tag_size == 4u);
    REQUIRE(layout->payload_offset == 8u);
    REQUIRE(layout->field_count == 3u && layout->variant_count == 3u);
    REQUIRE(layout->variants[0].field_count == 0u);
    REQUIRE(layout->variants[1].field_begin == 0u && layout->variants[1].field_count == 2u);
    REQUIRE(layout->variants[1].payload_size == 16u);
    REQUIRE(layout->variants[2].field_begin == 2u && layout->variants[2].field_count == 1u);
    REQUIRE(layout->fields[0].variant_ordinal == 1u && layout->fields[0].offset == 8u);
    REQUIRE(layout->fields[1].variant_ordinal == 1u && layout->fields[1].offset == 16u);
    REQUIRE(layout->fields[2].variant_ordinal == 2u && layout->fields[2].offset == 8u);
}

static void test_profile_bound_layouts(bool ilp32) {
    XrValidatedProgram *program = build_boundary_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(ilp32, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrInstance *instance = create_instance(program, profile);
    XrBoundaryMaterializationDiagnostic diagnostic;
    XrBoundaryCallLayout *call = NULL;
    REQUIRE(xr_execution_materialize_boundary_call(instance, XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL,
                                                   0u, NULL, &call,
                                                   &diagnostic) == XR_BOUNDARY_MATERIALIZATION_OK);
    REQUIRE(call->argument_count == 3u);
    REQUIRE(call->argument_size == 56u && call->argument_alignment == 8u);
    REQUIRE(call->arguments[0].offset == 0u && call->arguments[0].size == 24u);
    REQUIRE(call->arguments[1].offset == 24u && call->arguments[1].size == 24u);
    REQUIRE(call->arguments[2].type_id == XR_CORE_TYPE_U16);
    REQUIRE(call->arguments[2].offset == 48u && call->arguments[2].size == 2u);
    REQUIRE(call->result.type_id == XR_CORE_TYPE_U16);
    REQUIRE(call->result.size == 2u && call->result.alignment == 2u);

    XrBoundaryTypeLayout *aggregate = NULL;
    XrBoundaryTypeLayout *variant = NULL;
    XrBoundaryTypeLayout *u16 = NULL;
    REQUIRE(xr_execution_materialize_boundary_type(instance, XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL,
                                                   call->arguments[0].type_id, NULL, &aggregate,
                                                   &diagnostic) == XR_BOUNDARY_MATERIALIZATION_OK);
    REQUIRE(xr_execution_materialize_boundary_type(instance, XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL,
                                                   call->arguments[1].type_id, NULL, &variant,
                                                   &diagnostic) == XR_BOUNDARY_MATERIALIZATION_OK);
    REQUIRE(xr_execution_materialize_boundary_type(instance, XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL,
                                                   XR_CORE_TYPE_U16, NULL, &u16,
                                                   &diagnostic) == XR_BOUNDARY_MATERIALIZATION_OK);
    require_aggregate_layout(aggregate);
    require_variant_layout(variant);
    REQUIRE(u16->layout_kind == XR_BOUNDARY_TYPE_LAYOUT_SCALAR);
    REQUIRE(u16->cleanup_kind == XR_BOUNDARY_CLEANUP_TRIVIAL);
    REQUIRE(u16->size == 2u && u16->alignment == 2u);
    require_fingerprint(u16->id,
                        ilp32 ? "e89cdc3a3cec8bd1ec34bddceac088fe0f0e90aa945224a75d32961d7880f673"
                              : "936b366c0f1cc567edae7fd50028cbf409463a60f1f4dbb1e7e65766ddbb2b1c");
    REQUIRE(xr_fingerprint_equal(aggregate->id, call->arguments[0].type_layout_id));
    REQUIRE(xr_fingerprint_equal(variant->id, call->arguments[1].type_layout_id));
    REQUIRE(xr_fingerprint_equal(u16->id, call->arguments[2].type_layout_id));
    REQUIRE(xr_fingerprint_equal(u16->id, call->result.type_layout_id));
    uint16_t aggregate_type_id = aggregate->type_id;

    require_fingerprint(aggregate->id,
                        ilp32 ? "b98225baa3c9a5de1d7b5b96cd88e23ebf1d4073e34628700d4478118a6be067"
                              : "d1d9fa6ccc28edfadb7acb59c84548b19f6509f1e4646e4dad1b64d74582d3d8");
    require_fingerprint(variant->id,
                        ilp32 ? "7b6fa36b491c9a0568d743c020bbcbed4f76142689f85e014c53e73fcd83966a"
                              : "9c85e2ca71233c9ef5876ceb1811463facd825b9cd21d17cbc865c5fe601adf1");
    require_fingerprint(call->id,
                        ilp32 ? "dbb6ad70e23a6c026d920a1d1b28d6ee290a4143186d9714a28111bba76d4a8b"
                              : "19e294b70dbe7cd9963f4cf1c8d68f879ca734739ebce81b746a5967011df8d9");

    XrBoundaryTypeLayoutId public_id = aggregate->id;
    xr_boundary_type_layout_free(u16);
    xr_boundary_type_layout_free(variant);
    xr_boundary_type_layout_free(aggregate);
    xr_boundary_call_layout_free(call);

    for (XrMaterializedBoundaryKind kind = XR_MATERIALIZED_BOUNDARY_DYNAMIC_STORAGE;
         kind <= XR_MATERIALIZED_BOUNDARY_RELOADABLE; ++kind) {
        XrBoundaryTypeLayout *other = NULL;
        REQUIRE(xr_execution_materialize_boundary_type(instance, kind, aggregate_type_id, NULL,
                                                       &other, &diagnostic) ==
                XR_BOUNDARY_MATERIALIZATION_OK);
        require_aggregate_layout(other);
        REQUIRE(!xr_fingerprint_equal(public_id, other->id));
        xr_boundary_type_layout_free(other);
    }

    XrBoundaryMaterializationBudget tiny = xr_boundary_materialization_default_budget();
    tiny.max_extent = 8u;
    XrBoundaryTypeLayout *rejected = NULL;
    REQUIRE(xr_execution_materialize_boundary_type(
                instance, XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL, aggregate_type_id, &tiny, &rejected,
                &diagnostic) == XR_BOUNDARY_MATERIALIZATION_RESOURCE_LIMIT);
    REQUIRE(rejected == NULL);
    REQUIRE(fingerprint_is_nonzero(public_id));

    XrBoundaryMaterializationBudget shallow = xr_boundary_materialization_default_budget();
    shallow.max_type_depth = 1u;
    REQUIRE(xr_execution_materialize_boundary_type(
                instance, XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL, aggregate_type_id, &shallow,
                &rejected, &diagnostic) == XR_BOUNDARY_MATERIALIZATION_RESOURCE_LIMIT);
    REQUIRE(rejected == NULL);

    REQUIRE(xr_execution_materialize_boundary_type(instance, XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL,
                                                   5u, NULL, &rejected, &diagnostic) ==
            XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED);
    REQUIRE(rejected == NULL);
    REQUIRE(diagnostic.kind == XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_TYPE);

    const uint16_t nonmaterialized_target_enums[] = {
        XR_CORE_TYPE_TARGET_OS,
        XR_CORE_TYPE_TARGET_ARCH,
        XR_CORE_TYPE_TARGET_ABI,
        XR_CORE_TYPE_TARGET_ENDIAN,
    };
    for (size_t index = 0u;
         index < sizeof(nonmaterialized_target_enums) / sizeof(nonmaterialized_target_enums[0]);
         ++index) {
        REQUIRE(xr_execution_materialize_boundary_type(
                    instance, XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL,
                    nonmaterialized_target_enums[index], NULL, &rejected,
                    &diagnostic) == XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED);
        REQUIRE(rejected == NULL);
        REQUIRE(diagnostic.kind == XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_TYPE);
    }

    REQUIRE(xr_execution_materialize_boundary_type(
                instance, XR_MATERIALIZED_BOUNDARY_INVALID, aggregate_type_id, NULL, &rejected,
                &diagnostic) == XR_BOUNDARY_MATERIALIZATION_INVALID_INPUT);
    REQUIRE(rejected == NULL);
    REQUIRE(diagnostic.kind == XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_INPUT);

    XrBoundaryCallLayout *rejected_call = NULL;
    REQUIRE(xr_execution_materialize_boundary_call(instance, XR_MATERIALIZED_BOUNDARY_PUBLIC_CALL,
                                                   1u, NULL, &rejected_call, &diagnostic) ==
            XR_BOUNDARY_MATERIALIZATION_UNSUPPORTED);
    REQUIRE(rejected_call == NULL);
    REQUIRE(diagnostic.kind == XR_BOUNDARY_MATERIALIZATION_DIAGNOSTIC_FUNCTION);

    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

int main(void) {
    test_profile_bound_layouts(false);
    test_profile_bound_layouts(true);
    puts("boundary materialization tests passed");
    return 0;
}
