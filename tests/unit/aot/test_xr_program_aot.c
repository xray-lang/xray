/*
 * Task 300: private BackendIR and generated-C AOT over canonical XrProgram.
 */

#include "aot/program/xr_backend_ir.h"
#include "aot/program/xr_backend_ir_internal.h"
#include "base/xmalloc.h"
#include "core/xr_core_spec_gen.h"
#include "execution/xr_execution.h"
#include "program/xr_program.h"
#include "program/xr_program_verify.h"
#include "program/xr_reference_evaluator.h"
#include "vm/xr_program_vm.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_existential_fixture.h"
#include "../program/xr_program_callable_fixture.h"
#include "../program/xr_program_invoke_fixture.h"
#include "../program/xr_program_panic_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(XR_CORE_OP_CORE_CALL_SEALED_INVOKE == 37, "sealed invoke stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT == 38, "indirect direct stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE == 39, "indirect invoke stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALL_WITNESS_DIRECT == 40, "witness direct stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALL_WITNESS_INVOKE == 41, "witness invoke stable id drifted");
_Static_assert(XR_CORE_OP_CORE_PANIC_PUBLISH == 50, "panic publish stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_PACK == 86, "existential pack stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_TEST == 87, "existential test stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_PROJECT == 88, "existential project stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALLABLE_PACK == 89, "callable pack stable id drifted");

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct TestBindings {
    XrProviderBinding providers[XR_RUNTIME_ABI_MAX_PROVIDERS];
    XrProviderOperationBinding operations[XR_RUNTIME_ABI_MAX_PROVIDERS]
                                         [XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS];
    size_t count;
} TestBindings;

static void provider_entry(void) {
}

static void build_bindings(const XrTargetProfile *profile, TestBindings *bindings) {
    memset(bindings, 0, sizeof(*bindings));
    bindings->count = xr_target_profile_provider_count(profile);
    REQUIRE(bindings->count > 0u);
    for (size_t provider_index = 0; provider_index < bindings->count; ++provider_index) {
        const XrTargetProviderContract *contract =
            xr_target_profile_provider(profile, provider_index);
        REQUIRE(contract != NULL);
        XrProviderBinding *provider = &bindings->providers[provider_index];
        provider->contract_id = contract->contract_id;
        REQUIRE(xr_target_provider_contract_fingerprint(
                    contract, &provider->contract_fingerprint) == XR_RUNTIME_ABI_OK);
        provider->behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        provider->operations = bindings->operations[provider_index];
        provider->operation_count = contract->operation_count;
        for (uint16_t operation_index = 0; operation_index < contract->operation_count;
             ++operation_index) {
            XrProviderOperationBinding *operation =
                &bindings->operations[provider_index][operation_index];
            operation->operation_id = contract->operations[operation_index].stable_id;
            operation->entry = provider_entry;
        }
    }
}

static XrCoreIrKey fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrValidatedProgram *validate_program(const XrCoreIrTypeInput *types, uint32_t type_count,
                                            const XrCoreIrConstantInput *constants,
                                            uint32_t constant_count,
                                            const XrCoreIrFunctionInput *functions,
                                            uint32_t function_count) {
    XrCoreIrModuleInput module = {.key = fixture_key("aot:module"),
                                  .constants = constants,
                                  .constant_count = constant_count,
                                  .functions = functions,
                                  .function_count = function_count};
    XrCoreIrKey semantic = fixture_key("aot:semantic");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = semantic.bytes,
                                  .required_features = &feature,
                                  .required_feature_count = 1u,
                                  .types = types,
                                  .type_count = type_count,
                                  .modules = &module,
                                  .module_count = 1u};
    XrCoreIrProgram *core = NULL;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    char diagnostic[256] = {0};
    REQUIRE(xr_core_ir_program_build(&input, &core, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_write(core, &artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrProgramVerifyStatus verify_status =
        xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &verify_diagnostic);
    if (verify_status != XR_PROGRAM_VERIFY_OK) {
        fprintf(stderr,
                "fixture verification failed: status=%s diagnostic=%s function=%u "
                "block=%u instruction=%u value=%u\n",
                xr_program_verify_status_name(verify_status),
                xr_program_diagnostic_kind_name(verify_diagnostic.kind),
                verify_diagnostic.location.function_id, verify_diagnostic.location.block_id,
                verify_diagnostic.location.instruction_id, verify_diagnostic.location.value_id);
    }
    REQUIRE(verify_status == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(core);
    REQUIRE(program != NULL);
    return program;
}

static XrValidatedProgram *validate_functions(const XrCoreIrConstantInput *constants,
                                              uint32_t constant_count,
                                              const XrCoreIrFunctionInput *functions,
                                              uint32_t function_count) {
    return validate_program(NULL, 0u, constants, constant_count, functions, function_count);
}

static XrValidatedProgram *build_affine_copy_program(void) {
    enum {
        AFFINE_TYPE = 64
    };
    uint16_t fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = fixture_key("aot-affine:type"),
        .local_id = AFFINE_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = fields,
        .field_count = 1u,
    };
    XrCoreIrConstantInput constant = {
        .key = fixture_key("aot-affine:constant"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey scalar = fixture_key("aot-affine:scalar");
    XrCoreIrKey owner = fixture_key("aot-affine:owner");
    XrCoreIrKey copied = fixture_key("aot-affine:copied");
    XrCoreIrKey projected = fixture_key("aot-affine:projected");
    XrCoreIrKey construct_operands[] = {scalar};
    XrCoreIrKey owner_operand[] = {owner};
    XrCoreIrKey copied_operand[] = {copied};
    XrCoreIrKey returned[] = {projected};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = scalar,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = owner,
         .result_type_id = AFFINE_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY,
         .result = copied,
         .result_type_id = AFFINE_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = owner_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = projected,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = copied_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = copied_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = owner_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("aot-affine:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("aot-affine:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_program(&type, 1u, &constant, 1u, &function, 1u);
}

static XrValidatedProgram *build_full_program(void) {
    enum {
        AGGREGATE_TYPE = 101,
        VARIANT_TYPE = 77,
    };
    uint16_t aggregate_fields[] = {XR_CORE_TYPE_I64, XR_CORE_TYPE_BOOL};
    uint16_t variant_payload[] = {AGGREGATE_TYPE, XR_CORE_TYPE_BOOL};
    XrCoreIrVariantInput variants[] = {
        {0},
        {.payload_types = variant_payload,
         .payload_count = sizeof(variant_payload) / sizeof(variant_payload[0])},
    };
    XrCoreIrTypeInput types[] = {
        {.key = fixture_key("aot:type:variant"),
         .local_id = VARIANT_TYPE,
         .kind = XR_CORE_IR_TYPE_VARIANT,
         .variants = variants,
         .variant_count = sizeof(variants) / sizeof(variants[0])},
        {.key = fixture_key("aot:type:aggregate"),
         .local_id = AGGREGATE_TYPE,
         .kind = XR_CORE_IR_TYPE_AGGREGATE,
         .field_types = aggregate_fields,
         .field_count = sizeof(aggregate_fields) / sizeof(aggregate_fields[0])},
    };
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("aot:constant:40"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 40},
        {.key = fixture_key("aot:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
        {.key = fixture_key("aot:constant:true"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
        {.key = fixture_key("aot:constant:42"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 42},
    };
    XrCoreIrKey helper_key = fixture_key("aot:function:helper");
    XrCoreIrKey make_key = fixture_key("aot:function:make-aggregate");
    XrCoreIrKey entry_key = fixture_key("aot:function:entry");
    XrCoreIrKey trap_key = fixture_key("aot:function:trap");
    XrCoreIrKey error_key = fixture_key("aot:function:error");
    XrCoreIrKey mutator_key = fixture_key("aot:function:mutator");
    XrCoreIrKey helper_entry = fixture_key("aot:block:helper-entry");
    XrCoreIrKey make_entry = fixture_key("aot:block:make-entry");
    XrCoreIrKey helper_true = fixture_key("aot:block:helper-true");
    XrCoreIrKey helper_false = fixture_key("aot:block:helper-false");
    XrCoreIrKey helper_merge = fixture_key("aot:block:helper-merge");
    XrCoreIrKey entry_block_key = fixture_key("aot:block:entry");
    XrCoreIrKey trap_block_key = fixture_key("aot:block:trap");
    XrCoreIrKey error_block_key = fixture_key("aot:block:error");
    XrCoreIrKey mutator_block_key = fixture_key("aot:block:mutator");
    XrCoreIrKey v40 = fixture_key("aot:value:40");
    XrCoreIrKey v2 = fixture_key("aot:value:2");
    XrCoreIrKey vtrue = fixture_key("aot:value:true");
    XrCoreIrKey vadd = fixture_key("aot:value:add");
    XrCoreIrKey vsub = fixture_key("aot:value:sub");
    XrCoreIrKey vmul = fixture_key("aot:value:mul");
    XrCoreIrKey vdiv = fixture_key("aot:value:div");
    XrCoreIrKey vcmp = fixture_key("aot:value:compare");
    XrCoreIrKey true_arg = fixture_key("aot:value:true-arg");
    XrCoreIrKey false_arg = fixture_key("aot:value:false-arg");
    XrCoreIrKey width = fixture_key("aot:value:width");
    XrCoreIrKey merge_arg = fixture_key("aot:value:merge-arg");
    XrCoreIrKey call_result = fixture_key("aot:value:call");
    XrCoreIrKey error_arg = fixture_key("aot:value:error-arg");
    XrCoreIrKey aggregate_value = fixture_key("aot:value:aggregate");
    XrCoreIrKey projected_40 = fixture_key("aot:value:projected-40");
    XrCoreIrKey projected_true = fixture_key("aot:value:projected-true");
    XrCoreIrKey updated_aggregate = fixture_key("aot:value:updated-aggregate");
    XrCoreIrKey variant_value = fixture_key("aot:value:variant");
    XrCoreIrKey variant_is_one = fixture_key("aot:value:variant-is-one");
    XrCoreIrKey projected_aggregate = fixture_key("aot:value:projected-aggregate");
    XrCoreIrKey projected_2 = fixture_key("aot:value:projected-2");
    XrCoreIrKey mutator_argument = fixture_key("aot:value:mutator-argument");
    XrCoreIrKey mutator_42 = fixture_key("aot:value:mutator-42");
    XrCoreIrKey moved_result = fixture_key("aot:value:moved-result");
    XrCoreIrKey local_place = fixture_key("aot:value:local-place");
    XrCoreIrKey loaded_result = fixture_key("aot:value:loaded-result");
    XrCoreIrKey dropped_value = fixture_key("aot:value:dropped-value");
    XrCoreIrKey construct_operands[] = {v40, vtrue};
    XrCoreIrKey aggregate_operand[] = {aggregate_value};
    XrCoreIrKey update_operands[] = {aggregate_value, v2};
    XrCoreIrKey variant_operands[] = {updated_aggregate, projected_true};
    XrCoreIrKey variant_operand[] = {variant_value};
    XrCoreIrKey projected_aggregate_operand[] = {projected_aggregate};
    XrCoreIrKey pair40_2[] = {projected_40, projected_2};
    XrCoreIrKey pair_add_2[] = {vadd, v2};
    XrCoreIrKey pair_sub_2[] = {vsub, v2};
    XrCoreIrKey pair_div_add[] = {vdiv, vadd};
    XrCoreIrKey conditional_operands[] = {variant_is_one, vadd, vsub};
    XrCoreIrKey conditional_successors[] = {helper_true, helper_false};
    XrCoreIrInstructionInput helper_entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result = aggregate_value,
         .result_type_id = AGGREGATE_TYPE,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = make_key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = projected_40,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = aggregate_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = projected_true,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = aggregate_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 1u},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_UPDATE,
         .result = updated_aggregate,
         .result_type_id = AGGREGATE_TYPE,
         .operands = update_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_CONSTRUCT,
         .result = variant_value,
         .result_type_id = VARIANT_TYPE,
         .operands = variant_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT,
         .immediate.variant_ordinal = 1u},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_TEST,
         .result = variant_is_one,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = variant_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT,
         .immediate.variant_ordinal = 1u},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_PROJECT,
         .result = projected_aggregate,
         .result_type_id = AGGREGATE_TYPE,
         .operands = variant_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT_FIELD,
         .immediate.variant_field = {.variant_ordinal = 1u, .field_ordinal = 0u}},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = projected_2,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = projected_aggregate_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = vadd,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = pair40_2,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u},
        {.operation_id = XR_CORE_OP_CORE_SUB_I64,
         .result = vsub,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = pair_add_2,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u},
        {.operation_id = XR_CORE_OP_CORE_MUL_I64,
         .result = vmul,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = pair_sub_2,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u},
        {.operation_id = XR_CORE_OP_CORE_DIV_I64,
         .result = vdiv,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = pair_sub_2,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u},
        {.operation_id = XR_CORE_OP_CORE_COMPARE_I64,
         .result = vcmp,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = pair_div_add,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 2u},
        {.operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = conditional_operands,
         .operand_count = 3u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = conditional_successors,
         .successor_count = 2u},
    };
    XrCoreIrKey constructed[] = {aggregate_value};
    XrCoreIrInstructionInput make_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v40,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = vtrue,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[2].key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = aggregate_value,
         .result_type_id = AGGREGATE_TYPE,
         .operands = construct_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = constructed,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput make_block = {
        .key = make_entry,
        .instructions = make_instructions,
        .instruction_count = sizeof(make_instructions) / sizeof(make_instructions[0]),
    };
    XrCoreIrValueInput true_argument = {.key = true_arg, .type_id = XR_CORE_TYPE_I64};
    XrCoreIrKey true_arguments[] = {true_arg};
    XrCoreIrKey true_branch_values[] = {true_arg};
    XrCoreIrKey merge_successor[] = {helper_merge};
    XrCoreIrInstructionInput true_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = true_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_TARGET_POINTER_WIDTH,
         .result = width,
         .result_type_id = XR_CORE_TYPE_U32,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = true_branch_values,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = merge_successor,
         .successor_count = 1u},
    };
    XrCoreIrValueInput false_argument = {.key = false_arg, .type_id = XR_CORE_TYPE_I64};
    XrCoreIrKey false_arguments[] = {false_arg};
    XrCoreIrKey false_branch_values[] = {false_arg};
    XrCoreIrInstructionInput false_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_branch_values,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = merge_successor,
         .successor_count = 1u},
    };
    XrCoreIrValueInput merge_argument = {.key = merge_arg, .type_id = XR_CORE_TYPE_I64};
    XrCoreIrKey merge_arguments[] = {merge_arg};
    XrCoreIrKey helper_returned[] = {merge_arg};
    XrCoreIrInstructionInput merge_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = merge_arguments,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = helper_returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput helper_blocks[] = {
        {.key = helper_entry,
         .instructions = helper_entry_instructions,
         .instruction_count =
             sizeof(helper_entry_instructions) / sizeof(helper_entry_instructions[0])},
        {.key = helper_true,
         .arguments = &true_argument,
         .argument_count = 1u,
         .instructions = true_instructions,
         .instruction_count = sizeof(true_instructions) / sizeof(true_instructions[0])},
        {.key = helper_false,
         .arguments = &false_argument,
         .argument_count = 1u,
         .instructions = false_instructions,
         .instruction_count = sizeof(false_instructions) / sizeof(false_instructions[0])},
        {.key = helper_merge,
         .arguments = &merge_argument,
         .argument_count = 1u,
         .instructions = merge_instructions,
         .instruction_count = sizeof(merge_instructions) / sizeof(merge_instructions[0])},
    };
    XrCoreIrKey copied_result = fixture_key("aot:value:copied-result");
    XrCoreIrKey copy_operand[] = {call_result};
    XrCoreIrKey move_operand[] = {copied_result};
    XrCoreIrKey moved_operand[] = {moved_result};
    XrCoreIrKey place_operand[] = {local_place};
    XrCoreIrKey dropped_operand[] = {dropped_value};
    XrCoreIrKey entry_returned[] = {loaded_result};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result = call_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = helper_key},
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY,
         .result = copied_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = copy_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_MOVE,
         .result = moved_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = move_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
         .result = local_place,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_PLACE,
         .operands = moved_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = place_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = mutator_key},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD,
         .result = loaded_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = place_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = dropped_value,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = dropped_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = entry_returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput entry_block = {
        .key = entry_block_key,
        .instructions = entry_instructions,
        .instruction_count = sizeof(entry_instructions) / sizeof(entry_instructions[0]),
    };
    XrCoreIrInstructionInput trap_instruction = {
        .operation_id = XR_CORE_OP_CORE_TRAP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
        .immediate.u32 = 4u,
    };
    XrCoreIrBlockInput trap_block = {
        .key = trap_block_key, .instructions = &trap_instruction, .instruction_count = 1u};
    XrCoreIrValueInput error_argument = {.key = error_arg, .type_id = XR_CORE_TYPE_ERROR};
    XrCoreIrKey error_operands[] = {error_arg};
    XrCoreIrInstructionInput error_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = error_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = error_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput error_block = {
        .key = error_block_key,
        .arguments = &error_argument,
        .argument_count = 1u,
        .instructions = error_instructions,
        .instruction_count = sizeof(error_instructions) / sizeof(error_instructions[0]),
    };
    XrCoreIrValueInput mutator_block_argument = {
        .key = mutator_argument,
        .type_id = XR_CORE_TYPE_I64,
        .category = XR_CORE_IR_PLACE,
    };
    XrCoreIrKey mutator_argument_operand[] = {mutator_argument};
    XrCoreIrKey mutator_store_operands[] = {mutator_argument, mutator_42};
    XrCoreIrInstructionInput mutator_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = mutator_argument_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = mutator_42,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[3].key},
        {.operation_id = XR_CORE_OP_CORE_PLACE_STORE,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = mutator_store_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput mutator_block = {
        .key = mutator_block_key,
        .arguments = &mutator_block_argument,
        .argument_count = 1u,
        .instructions = mutator_instructions,
        .instruction_count = sizeof(mutator_instructions) / sizeof(mutator_instructions[0]),
    };
    uint16_t error_parameter = XR_CORE_TYPE_ERROR;
    uint16_t mutator_parameter = XR_CORE_TYPE_I64;
    XrParamMode mutator_mode = XR_PARAM_REF;
    XrCoreIrFunctionInput functions[] = {
        {.key = make_key,
         .result_type_id = AGGREGATE_TYPE,
         .effect_mask = UINT32_C(1),
         .entry_block = make_entry,
         .blocks = &make_block,
         .block_count = 1u},
        {.key = helper_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = UINT32_C(13),
         .capability_mask = UINT32_C(1),
         .entry_block = helper_entry,
         .blocks = helper_blocks,
         .block_count = sizeof(helper_blocks) / sizeof(helper_blocks[0])},
        {.key = entry_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = UINT32_C(13),
         .capability_mask = UINT32_C(1),
         .entry_block = entry_block_key,
         .blocks = &entry_block,
         .block_count = 1u,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
        {.key = trap_key,
         .result_type_id = XR_CORE_TYPE_VOID,
         .effect_mask = UINT32_C(1),
         .entry_block = trap_block_key,
         .blocks = &trap_block,
         .block_count = 1u},
        {.key = error_key,
         .parameter_types = &error_parameter,
         .parameter_count = 1u,
         .result_type_id = XR_CORE_TYPE_VOID,
         .error_type_id = XR_CORE_TYPE_ERROR,
         .effect_mask = XR_CORE_EFFECT_ERROR,
         .entry_block = error_block_key,
         .blocks = &error_block,
         .block_count = 1u},
        {.key = mutator_key,
         .parameter_types = &mutator_parameter,
         .parameter_modes = &mutator_mode,
         .parameter_count = 1u,
         .result_type_id = XR_CORE_TYPE_VOID,
         .entry_block = mutator_block_key,
         .blocks = &mutator_block,
         .block_count = 1u},
    };
    return validate_program(types, sizeof(types) / sizeof(types[0]), constants,
                            sizeof(constants) / sizeof(constants[0]), functions,
                            sizeof(functions) / sizeof(functions[0]));
}

static XrValidatedProgram *build_pointer_width_program(void) {
    XrCoreIrKey width = fixture_key("aot:width:value");
    XrCoreIrKey returned[] = {width};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_TARGET_POINTER_WIDTH,
         .result = width,
         .result_type_id = XR_CORE_TYPE_U32,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("aot:width:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("aot:width:function"),
        .result_type_id = XR_CORE_TYPE_U32,
        .effect_mask = UINT32_C(9),
        .capability_mask = UINT32_C(1),
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = fixture_key("aot:width:module"), .functions = &function, .function_count = 1u};
    XrCoreIrKey semantic = fixture_key("aot:width:semantic");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = semantic.bytes,
                                  .required_features = &feature,
                                  .required_feature_count = 1u,
                                  .modules = &module,
                                  .module_count = 1u};
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
    return program;
}

static XrValidatedProgram *build_binary_program(uint16_t operation_id, int64_t left, int64_t right,
                                                uint32_t mode) {
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("aot:binary:left"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = left},
        {.key = fixture_key("aot:binary:right"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = right},
    };
    XrCoreIrKey left_value = fixture_key("aot:binary:left-value");
    XrCoreIrKey right_value = fixture_key("aot:binary:right-value");
    XrCoreIrKey result_value = fixture_key("aot:binary:result");
    XrCoreIrKey operands[] = {left_value, right_value};
    XrCoreIrKey returned[] = {result_value};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = left_value,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = right_value,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = operation_id,
         .result = result_value,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = mode},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("aot:binary:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("aot:binary:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = UINT32_C(1),
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_functions(constants, sizeof(constants) / sizeof(constants[0]), &function, 1u);
}

static XrValidatedProgram *build_existential_program(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_existential_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
}

static XrValidatedProgram *build_callable_program(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_callable_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
}

static XrInstance *create_instance(XrValidatedProgram *program, XrTargetProfile *profile,
                                   const TestBindings *bindings, uint64_t generation) {
    XrExecutionBindingInput input = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                                     .program = program,
                                     .profile = profile,
                                     .providers = bindings->providers,
                                     .provider_count = bindings->count,
                                     .generation = generation};
    XrExecutionDiagnostic diagnostic;
    XrInstance *instance = NULL;
    REQUIRE(xr_execution_instance_create(&input, &instance, &diagnostic) == XR_EXECUTION_OK);
    return instance;
}

static void retire_instance(XrInstance **instance) {
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(*instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(*instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(instance, &diagnostic) == XR_EXECUTION_OK);
}

static XrFingerprint fingerprint_text(const char *text) {
    XrFingerprint fingerprint;
    xr_semantic_fingerprint((const uint8_t *) text, strlen(text), &fingerprint);
    return fingerprint;
}

static XrAotToolchainBinding toolchain_for(XrFingerprint profile_id) {
    XrAotToolchainInput input = {
        .schema_version = XR_AOT_TOOLCHAIN_SCHEMA_VERSION,
        .provider = XR_AOT_TOOLCHAIN_CLANG,
        .provider_version = "test-clang-21",
        .target_triple = "aarch64-apple-darwin",
        .codegen_options = "c11;strict;O2",
        .sysroot_id = fingerprint_text("test-sysroot"),
        .runtime_objects_id = fingerprint_text("test-runtime-objects"),
        .target_profile_id = profile_id,
    };
    XrAotToolchainBinding binding;
    REQUIRE(xr_aot_toolchain_binding_build(&input, &binding));
    return binding;
}

static XrBackendIR *build_ir(XrInstance *instance, uint8_t optimization_policy) {
    XrBackendOptions options = xr_backend_default_options();
    options.optimization_policy = optimization_policy;
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    REQUIRE(xr_backend_ir_build(instance, &options, &ir, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(ir != NULL);
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(xr_backend_ir_translation_validate(ir, &diagnostic));
    return ir;
}

static void test_affine_copy_lowering(void) {
    XrValidatedProgram *program = build_affine_copy_program();
    XrReferenceOutcome reference = xr_reference_evaluate(
        program, xr_validated_program_entry_function(program), NULL, 0u, NULL, NULL);
    REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RETURN);
    REQUIRE(reference.value.kind == XR_REFERENCE_VALUE_I64);
    REQUIRE(reference.value.as.i64 == 42);

    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestBindings bindings;
    build_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);

    XrVmCode *code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    REQUIRE(xr_vm_code_build(instance, NULL, &code, &vm_diagnostic) == XR_VM_CODE_OK);
    XrVmOutcome vm =
        xr_vm_code_execute(code, instance, xr_validated_program_entry_function(program), NULL, 0u);
    REQUIRE(vm.kind == XR_VM_OUTCOME_RETURN);
    REQUIRE(vm.value.kind == XR_VM_VALUE_I64);
    REQUIRE(vm.value.as.i64 == 42);
    xr_vm_code_free(code);

    XrBackendIR *ir = build_ir(instance, XR_BACKEND_OPTIMIZATION_PORTABLE);
    REQUIRE(ir->instruction_count == 7u);
    bool found_copy = false;
    for (uint32_t function = 0; function < ir->function_count; ++function) {
        for (uint32_t block = 0; block < ir->functions[function].block_count; ++block) {
            const XrBackendBlock *row = &ir->functions[function].blocks[block];
            for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
                const XrBackendInstruction *op = &row->instructions[instruction];
                if (op->operation_id == XR_CORE_OP_CORE_OWNER_COPY) {
                    REQUIRE(op->result_ownership == XR_CORE_IR_OWNER);
                    found_copy = true;
                }
            }
        }
    }
    REQUIRE(found_copy);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "struct XrAotType") != NULL);
    REQUIRE(strstr(generated.bytes, " = v") != NULL);

    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    retire_instance(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_sealed_invoke_typed_error_cleanup_lowering(void) {
    XrProgramArtifact artifact = {0};
    char build_diagnostic[256] = {0};
    REQUIRE(xr_program_invoke_fixture_write(&artifact, build_diagnostic,
                                            sizeof(build_diagnostic)) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestBindings bindings;
    build_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 77u);
    XrBackendIR *ir = build_ir(instance, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "out_error") != NULL);
    REQUIRE(strstr(generated.bytes, "invoke_error_") != NULL);
    REQUIRE(strstr(generated.bytes, "if (call_") != NULL);
    REQUIRE(strstr(generated.bytes, ".kind == 2") != NULL);
    REQUIRE(strstr(generated.bytes, "goto xr_f") != NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    retire_instance(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

static void test_typed_panic_cleanup_lowering(void) {
    XrProgramArtifact artifact = {0};
    char build_diagnostic[256] = {0};
    REQUIRE(xr_program_panic_fixture_write(&artifact, build_diagnostic, sizeof(build_diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestBindings bindings;
    build_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 78u);
    XrBackendIR *ir = build_ir(instance, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "out_panic") != NULL);
    REQUIRE(strstr(generated.bytes, "invoke_panic_") != NULL);
    REQUIRE(strstr(generated.bytes, ".kind == 3") != NULL);
    REQUIRE(strstr(generated.bytes, "xr_aot_make(3, 0, 0)") != NULL);
    REQUIRE(strstr(generated.bytes, "goto xr_f") != NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    retire_instance(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

static void test_existential_pack_test_project_lowering(void) {
    XrValidatedProgram *program = build_existential_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestBindings bindings;
    build_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 79u);
    XrBackendIR *ir = build_ir(instance, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "concrete_type_id") != NULL);
    REQUIRE(strstr(generated.bytes, "conformance_id") != NULL);
    REQUIRE(strstr(generated.bytes, "xr_aot_alloc(xr_ctx") != NULL);
    REQUIRE(strstr(generated.bytes, ".data = (void *)existential_payload_") != NULL);
    REQUIRE(strstr(generated.bytes, ".concrete_type_id == UINT16_C(") != NULL);
    REQUIRE(strstr(generated.bytes, "switch (v") != NULL);
    REQUIRE(strstr(generated.bytes, ".conformance_id) {") != NULL);
    REQUIRE(strstr(generated.bytes, "*(const XrAotType") != NULL);
    REQUIRE(strstr(generated.bytes, "selector") == NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    retire_instance(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_callable_pack_and_indirect_call_lowering(void) {
    XrValidatedProgram *program = build_callable_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestBindings bindings;
    build_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 80u);
    XrBackendIR *ir = build_ir(instance, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, ".function_id = UINT32_C(") != NULL);
    REQUIRE(strstr(generated.bytes, ".capture = (void *)callable_capture_") != NULL);
    REQUIRE(strstr(generated.bytes, ".capture = NULL") != NULL);
    REQUIRE(strstr(generated.bytes, ".function_id) {") != NULL);
    REQUIRE(strstr(generated.bytes, "*(const XrAotType") != NULL);
    REQUIRE(strstr(generated.bytes, "selector") == NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    retire_instance(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_reference_vm_aot_identity(XrValidatedProgram *program, XrInstance *instance) {
    XrReferenceProfile reference_profile = {.pointer_width = 64u};
    XrReferenceOutcome reference = xr_reference_evaluate(
        program, xr_validated_program_entry_function(program), NULL, 0u, &reference_profile, NULL);
    REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RETURN);
    REQUIRE(reference.value.kind == XR_REFERENCE_VALUE_I64);
    REQUIRE(reference.value.as.i64 == 42);

    XrVmCode *code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    REQUIRE(xr_vm_code_build(instance, NULL, &code, &vm_diagnostic) == XR_VM_CODE_OK);
    XrVmOutcome vm =
        xr_vm_code_execute(code, instance, xr_validated_program_entry_function(program), NULL, 0u);
    REQUIRE(vm.kind == XR_VM_OUTCOME_RETURN);
    REQUIRE(vm.value.kind == XR_VM_VALUE_I64);
    REQUIRE(vm.value.as.i64 == reference.value.as.i64);
    xr_vm_code_free(code);

    XrBackendIR *none = build_ir(instance, XR_BACKEND_OPTIMIZATION_NONE);
    XrBackendIR *portable = build_ir(instance, XR_BACKEND_OPTIMIZATION_PORTABLE);
    REQUIRE(xr_fingerprint_equal(xr_backend_ir_execution_id(none),
                                 xr_backend_ir_execution_id(portable)));
    REQUIRE(!xr_fingerprint_equal(xr_backend_ir_optimization_policy_id(none),
                                  xr_backend_ir_optimization_policy_id(portable)));
    REQUIRE(xr_backend_ir_instruction_count(none) == 42u);

    XrGeneratedC generated_none = {0};
    XrGeneratedC generated_portable = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(none, true, &generated_none, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(xr_backend_ir_emit_c(portable, true, &generated_portable, &diagnostic) ==
            XR_BACKEND_OK);
    REQUIRE(strstr(generated_none.bytes, "int64_t v0") != NULL);
    REQUIRE(strstr(generated_none.bytes, "struct XrAotType") != NULL);
    REQUIRE(strstr(generated_none.bytes, "payload.case_1.f0") != NULL);
    REQUIRE(strstr(generated_none.bytes, "int64_t * p") != NULL);
    REQUIRE(strstr(generated_none.bytes, " = &xr_place_") != NULL);
    REQUIRE(strstr(generated_none.bytes, " = *v") != NULL);
    REQUIRE(strstr(generated_none.bytes, "*v") != NULL);
    REQUIRE(strstr(generated_none.bytes, "XrVm") == NULL);
    REQUIRE(strstr(generated_none.bytes, "XrAotValue") == NULL);
    REQUIRE(strstr(generated_none.bytes, "TargetPlan") == NULL);
    REQUIRE(strstr(generated_none.bytes, "int main(void)") != NULL);

    XrAotToolchainBinding toolchain = toolchain_for(generated_none.target_profile_id);
    static const uint8_t native_bytes[] = {0x7f, 'X', 'R', 'A', 'O', 'T'};
    XrNativeArtifact artifact = {0};
    REQUIRE(xr_native_artifact_seal(&generated_none, &toolchain, native_bytes, sizeof(native_bytes),
                                    &artifact) == XR_BACKEND_OK);
    REQUIRE(xr_native_artifact_verify(&artifact, generated_none.execution_id,
                                      generated_none.backend_id,
                                      generated_none.optimization_policy_id, &toolchain));
    artifact.bytes[1] ^= UINT8_C(1);
    REQUIRE(!xr_native_artifact_verify(&artifact, generated_none.execution_id,
                                       generated_none.backend_id,
                                       generated_none.optimization_policy_id, &toolchain));
    artifact.bytes[1] ^= UINT8_C(1);
    XrAotToolchainBinding wrong_toolchain = toolchain;
    wrong_toolchain.sysroot_id.bytes[0] ^= UINT8_C(1);
    REQUIRE(!xr_native_artifact_verify(&artifact, generated_none.execution_id,
                                       generated_none.backend_id,
                                       generated_none.optimization_policy_id, &wrong_toolchain));

    xr_native_artifact_free(&artifact);
    xr_generated_c_free(&generated_portable);
    xr_generated_c_free(&generated_none);
    xr_backend_ir_free(portable);
    xr_backend_ir_free(none);
}

static void test_foreign_profile_and_translation_mutation(void) {
    XrValidatedProgram *program = build_pointer_width_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_FREESTANDING);
    REQUIRE(profile != NULL);
    TestBindings bindings;
    build_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 7u);
    XrBackendIR *ir = build_ir(instance, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "UINT32_C(32)") != NULL);
    REQUIRE(strstr(generated.bytes, "sizeof(void") == NULL);

    ir->pointer_width = 64u;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    ir->pointer_width = 32u;

    ir->backend_id.bytes[0] ^= UINT8_C(1);
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    ir->backend_id.bytes[0] ^= UINT8_C(1);

    ir->optimization_policy_id.bytes[0] ^= UINT8_C(1);
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    ir->optimization_policy_id.bytes[0] ^= UINT8_C(1);

    ir->lowering_digest.bytes[0] ^= UINT8_C(1);
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    ir->lowering_digest.bytes[0] ^= UINT8_C(1);
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));

    uint16_t saved = ir->functions[0].blocks[0].instructions[0].operation_id;
    ir->functions[0].blocks[0].instructions[0].operation_id = XR_CORE_OP_CORE_TRAP;
    REQUIRE(!xr_backend_ir_translation_validate(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_TRANSLATION_REJECTED);
    ir->functions[0].blocks[0].instructions[0].operation_id = saved;
    REQUIRE(xr_backend_ir_translation_validate(ir, &diagnostic));

    XrBackendOptions constrained = xr_backend_default_options();
    constrained.max_instructions = 1u;
    XrBackendIR *rejected = NULL;
    REQUIRE(xr_backend_ir_build(instance, &constrained, &rejected, &diagnostic) ==
            XR_BACKEND_RESOURCE_LIMIT);
    REQUIRE(rejected == NULL);

    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    retire_instance(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_generated_fixture(const char *path, XrInstance *instance, bool standalone_main) {
    XrBackendIR *ir = build_ir(instance, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, standalone_main, &generated, &diagnostic) == XR_BACKEND_OK);
    FILE *output = fopen(path, "wb");
    REQUIRE(output != NULL);
    REQUIRE(fwrite(generated.bytes, 1u, generated.size, output) == generated.size);
    REQUIRE(fclose(output) == 0);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
}

static void seal_native_file(const char *path, XrInstance *instance) {
    FILE *input = fopen(path, "rb");
    REQUIRE(input != NULL);
    REQUIRE(fseek(input, 0, SEEK_END) == 0);
    long end = ftell(input);
    REQUIRE(end > 0);
    REQUIRE(fseek(input, 0, SEEK_SET) == 0);
    size_t size = (size_t) end;
    uint8_t *bytes = xr_malloc(size);
    REQUIRE(bytes != NULL);
    REQUIRE(fread(bytes, 1u, size, input) == size);
    REQUIRE(fclose(input) == 0);

    XrBackendIR *ir = build_ir(instance, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
    XrAotToolchainBinding toolchain = toolchain_for(generated.target_profile_id);
    XrNativeArtifact artifact = {0};
    REQUIRE(xr_native_artifact_seal(&generated, &toolchain, bytes, size, &artifact) ==
            XR_BACKEND_OK);
    REQUIRE(xr_native_artifact_verify(&artifact, generated.execution_id, generated.backend_id,
                                      generated.optimization_policy_id, &toolchain));
    char artifact_id[XR_FINGERPRINT_BYTES * 2u + 1u];
    xr_fingerprint_hex(artifact.id, artifact_id);
    printf("sealed native artifact %s (%lu bytes)\n", artifact_id, (unsigned long) artifact.size);
    xr_native_artifact_free(&artifact);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_free(bytes);
}

int main(int argc, char **argv) {
    REQUIRE(argc >= 1 && argc <= 3);
    bool seal_mode = argc == 3 && strcmp(argv[1], "--seal") == 0;
    bool invoke_object_mode = argc == 3 && strcmp(argv[2], "sealed-invoke-object") == 0;
    bool panic_object_mode = argc == 3 && strcmp(argv[2], "typed-panic-object") == 0;
    XrValidatedProgram *program = NULL;
    if (seal_mode)
        program = build_full_program();
    else if (argc == 3 && strcmp(argv[2], "checked-overflow") == 0)
        program = build_binary_program(XR_CORE_OP_CORE_ADD_I64, INT64_MAX, 1, 0u);
    else if (argc == 3 && strcmp(argv[2], "wrapping-overflow") == 0)
        program = build_binary_program(XR_CORE_OP_CORE_ADD_I64, INT64_MAX, 1, 1u);
    else if (argc == 3 && strcmp(argv[2], "division-zero") == 0)
        program = build_binary_program(XR_CORE_OP_CORE_DIV_I64, 42, 0, 0u);
    else if (argc == 3 && strcmp(argv[2], "existential") == 0)
        program = build_existential_program();
    else if (argc == 3 && strcmp(argv[2], "callable") == 0)
        program = build_callable_program();
    else if (invoke_object_mode || panic_object_mode) {
        XrProgramArtifact artifact = {0};
        char diagnostic[256] = {0};
        XrProgramBuildStatus fixture_status =
            panic_object_mode
                ? xr_program_panic_fixture_write(&artifact, diagnostic, sizeof(diagnostic))
                : xr_program_invoke_fixture_write(&artifact, diagnostic, sizeof(diagnostic));
        REQUIRE(fixture_status == XR_PROGRAM_BUILD_OK);
        XrProgramDiagnostic verify_diagnostic;
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                    &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
    } else {
        REQUIRE(argc != 3);
        program = build_full_program();
    }
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestBindings bindings;
    build_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    if (seal_mode) {
        seal_native_file(argv[2], instance);
    } else if (argc >= 2) {
        write_generated_fixture(argv[1], instance, !invoke_object_mode && !panic_object_mode);
    } else {
        test_reference_vm_aot_identity(program, instance);
        test_affine_copy_lowering();
        test_sealed_invoke_typed_error_cleanup_lowering();
        test_typed_panic_cleanup_lowering();
        test_existential_pack_test_project_lowering();
        test_callable_pack_and_indirect_call_lowering();
        test_foreign_profile_and_translation_mutation();
        puts("canonical XrProgram AOT tests passed");
    }
    retire_instance(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    return 0;
}
