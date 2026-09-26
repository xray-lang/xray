#include "../program/xr_program_byte_compare_fixture.h"
/*
 * Task 300: private BackendIR and generated-C AOT over canonical XrProgram.
 */

#include "../program/xr_program_array_default_fixture.h"
#include "../program/xr_program_array_append_fixture.h"
#include "../program/xr_program_string_slice_fixture.h"
#include "../program/xr_program_string_builder_fixture.h"
#include "../program/xr_program_channel_fixture.h"
#include "../program/xr_program_atomic_fixture.h"
#include "../program/xr_program_module_fixture.h"
#include "../program/xr_program_f64_fixture.h"
#include "aot/program/xr_backend_ir.h"
#include "aot/program/xr_backend_ir_internal.h"
#include "base/xmalloc.h"
#include "core/xr_core_spec_gen.h"
#include "execution/xr_execution.h"
#include "runtime/abi/xr_runtime_target_profile.h"
#include "program/xr_program.h"
#include "program/xr_program_verify.h"
#include "program/xr_reference_evaluator.h"
#include "vm/xr_program_vm.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_provider_fixture.h"
#include "../program/xr_program_existential_fixture.h"
#include "../program/xr_program_reborrow_fixture.h"
#include "../program/xr_program_callable_fixture.h"
#include "../program/xr_program_invoke_fixture.h"
#include "../program/xr_program_panic_fixture.h"
#include "../program/xr_program_assert_fixture.h"
#include "../program/xr_program_coroutine_fixture.h"
#include "../program/xr_program_coroutine_branch_fixture.h"
#include "../program/xr_program_ref_coroutine_fixture.h"
#include "../program/xr_program_coroutine_trap_fixture.h"
#include "../program/xr_program_coroutine_outcome_fixture.h"
#include "../program/xr_program_cleanup_graph_fixture.h"
#include "../program/xr_program_output_fixture.h"
#include "../program/xr_program_text_fixture.h"
#include "../program/xr_program_integer_fixture.h"
#include "../program/xr_program_sequence_length_fixture.h"
#include "../program/xr_program_array_fixture.h"
#include "../program/xr_program_pipe_fixture.h"
#include "../program/xr_program_trap_fixture.h"
#include "../test_win_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(XR_CORE_OP_CORE_CALL_SEALED_INVOKE == 37, "sealed invoke stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT == 38, "indirect direct stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE == 39, "indirect invoke stable id drifted");
_Static_assert(XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT == 141,
               "indirect coroutine call stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CLASS_CONSTRUCT == 142, "class construct stable id drifted");
_Static_assert(XR_CORE_OP_CORE_OWNER_ALIAS == 143, "class share stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CLASS_FIELD_LOAD == 144, "class field load stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CLASS_FIELD_PLACE == 145, "class field place stable id drifted");
_Static_assert(XR_CORE_OP_CORE_PLACE_EXCHANGE == 146, "place exchange stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALL_WITNESS_DIRECT == 40, "witness direct stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALL_WITNESS_INVOKE == 41, "witness invoke stable id drifted");
_Static_assert(XR_CORE_OP_CORE_PANIC_PUBLISH == 50, "panic publish stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_PACK == 86, "existential pack stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_TEST == 87, "existential test stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_PROJECT == 88, "existential project stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALLABLE_PACK == 89, "callable pack stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ == 90,
               "existential READ reborrow stable id drifted");
_Static_assert(XR_CORE_OP_CORE_PLACE_TAKE == 108, "place take stable id drifted");
_Static_assert(XR_CORE_OP_CORE_ASSERT_CONDITION == 109, "condition assert stable id drifted");
_Static_assert(XR_CORE_OP_CORE_PROVIDER_CALL == 136, "provider call stable id drifted");
_Static_assert(XR_CORE_OP_CORE_OUTPUT_GROUP == 148, "output group stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CONSTANT_STRING == 4, "string constant stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CONSTANT_RUNE == 5, "rune constant stable id drifted");
_Static_assert(XR_CORE_OP_CORE_COMPARE_RUNE == 26, "rune compare stable id drifted");
_Static_assert(XR_CORE_OP_CORE_COMPARE_STRING == 27, "string compare stable id drifted");
_Static_assert(XR_CORE_OP_CORE_STRING_FROM_SCALAR == 167, "scalar string stable id drifted");
_Static_assert(XR_CORE_OP_CORE_STRING_CONCAT == 151, "string concat stable id drifted");
_Static_assert(XR_CORE_OP_CORE_COROUTINE_YIELD == 116, "coroutine yield stable id drifted");
_Static_assert(XR_CORE_OP_CORE_COROUTINE_SUSPEND == 140, "coroutine suspension stable id drifted");
_Static_assert(XR_CORE_OP_CORE_COROUTINE_CALL_SEALED == 138, "coroutine call stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CANCEL_PUBLISH == 51, "cancel publish stable id drifted");

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

static void build_bindings(const XrTargetProfile *profile, TestBindings *bindings) {
    (void) profile;
    memset(bindings, 0, sizeof(*bindings));
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

static XrValidatedProgram *build_empty_aggregate_program(void) {
    enum {
        EMPTY_TYPE = 64
    };
    XrCoreIrTypeInput type = {
        .key = fixture_key("aot-empty:type"),
        .local_id = EMPTY_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
    };
    XrCoreIrConstantInput constant = {
        .key = fixture_key("aot-empty:constant"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey empty_value = fixture_key("aot-empty:value");
    XrCoreIrKey scalar = fixture_key("aot-empty:scalar");
    XrCoreIrKey returned[] = {scalar};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = empty_value,
         .result_type_id = EMPTY_TYPE,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = scalar,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("aot-empty:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("aot-empty:function"),
        .result_type_id = XR_CORE_TYPE_I64,
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
    XrCoreIrKey v2_store = fixture_key("aot:value:2-store");
    XrCoreIrKey vtrue = fixture_key("aot:value:true");
    XrCoreIrKey vadd = fixture_key("aot:value:add");
    XrCoreIrKey vsub = fixture_key("aot:value:sub");
    XrCoreIrKey vmul = fixture_key("aot:value:mul");
    XrCoreIrKey vdiv = fixture_key("aot:value:div");
    XrCoreIrKey vcmp = fixture_key("aot:value:compare");
    XrCoreIrKey vnot = fixture_key("aot:value:not");
    XrCoreIrKey vand = fixture_key("aot:value:and");
    XrCoreIrKey vor = fixture_key("aot:value:or");
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
    XrCoreIrKey aggregate_place = fixture_key("aot:value:aggregate-place");
    XrCoreIrKey field_place = fixture_key("aot:value:field-place");
    XrCoreIrKey projected_place_2 = fixture_key("aot:value:projected-place-2");
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
    XrCoreIrKey updated_aggregate_operand[] = {updated_aggregate};
    XrCoreIrKey aggregate_place_operand[] = {aggregate_place};
    XrCoreIrKey field_store_operands[] = {field_place, v2_store};
    XrCoreIrKey field_place_operand[] = {field_place};
    XrCoreIrKey variant_operand[] = {variant_value};
    XrCoreIrKey projected_aggregate_operand[] = {projected_aggregate};
    XrCoreIrKey pair40_2[] = {projected_40, projected_place_2};
    XrCoreIrKey pair_add_2[] = {vadd, v2};
    XrCoreIrKey pair_sub_2[] = {vsub, v2};
    XrCoreIrKey pair_div_add[] = {vdiv, vadd};
    XrCoreIrKey logical_not_operand[] = {projected_true};
    XrCoreIrKey logical_and_operands[] = {variant_is_one, projected_true};
    XrCoreIrKey logical_or_operands[] = {vand, vnot};
    XrCoreIrKey conditional_operands[] = {vor, vadd, vsub};
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
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
         .result = aggregate_place,
         .result_type_id = AGGREGATE_TYPE,
         .result_category = XR_CORE_IR_PLACE,
         .operands = updated_aggregate_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_PLACE_PROJECT,
         .result = field_place,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_PLACE,
         .operands = aggregate_place_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2_store,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_PLACE_STORE,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = field_store_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD,
         .result = projected_place_2,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = field_place_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
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
        {.operation_id = XR_CORE_OP_CORE_LOGICAL_NOT,
         .result = vnot,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = logical_not_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_LOGICAL_AND,
         .result = vand,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = logical_and_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_LOGICAL_OR,
         .result = vor,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = logical_or_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
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
         .result_type_id = XR_CORE_TYPE_U16,
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

static XrValidatedProgram *build_target_query_program(uint16_t operation_id, uint16_t result_type,
                                                      uint32_t capability_mask) {
    XrCoreIrKey width = fixture_key("aot:width:value");
    XrCoreIrKey returned[] = {width};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = operation_id,
         .result = width,
         .result_type_id = result_type,
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
        .result_type_id = result_type,
        .effect_mask = UINT32_C(9),
        .capability_mask = capability_mask,
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

static XrValidatedProgram *build_target_enum_equality_program(void) {
    XrCoreIrKey queried = fixture_key("aot:target-enum:query");
    XrCoreIrKey wasi = fixture_key("aot:target-enum:wasi");
    XrCoreIrKey equal = fixture_key("aot:target-enum:equal");
    XrCoreIrKey compared[] = {queried, wasi};
    XrCoreIrKey returned[] = {equal};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM,
         .result = queried,
         .result_type_id = XR_CORE_TYPE_TARGET_OS,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM,
         .result = wasi,
         .result_type_id = XR_CORE_TYPE_TARGET_OS,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = XR_TARGET_OS_WASI},
        {.operation_id = XR_CORE_OP_CORE_COMPARE_TARGET_ENUM,
         .result = equal,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = compared,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("aot:target-enum:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("aot:target-enum:function"),
        .result_type_id = XR_CORE_TYPE_BOOL,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_TARGET_QUERY,
        .capability_mask = XR_CORE_CAPABILITY_PROFILE_OPERATING_SYSTEM,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_program(NULL, 0u, NULL, 0u, &function, 1u);
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

static XrValidatedProgram *build_provider_call_program(const XrTargetProfile *profile,
                                                       bool nullary) {
    const XrTargetProviderContract *contract = NULL;
    for (size_t index = 0u; index < xr_target_profile_provider_count(profile); ++index) {
        const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, index);
        if (xr_test_target_profile_is_scalar_provider(candidate)) {
            REQUIRE(contract == NULL);
            contract = candidate;
        }
    }
    REQUIRE(contract != NULL && contract->operation_count == 1u);
    XrStableId operation_id = contract->operations[0].stable_id;
    XrCoreIrConstantInput constant = {
        .key = fixture_key("aot-provider:constant"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 41,
    };
    XrCoreIrKey argument = fixture_key("aot-provider:argument");
    XrCoreIrKey result = fixture_key("aot-provider:result");
    XrCoreIrKey call_operands[] = {argument};
    XrCoreIrKey returned[] = {result};
    XrCoreIrInstructionInput instructions[3] = {0};
    uint32_t instruction_count = 0u;
    if (!nullary) {
        instructions[instruction_count++] =
            (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
                                        .result = argument,
                                        .result_type_id = XR_CORE_TYPE_I64,
                                        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
                                        .immediate.key = constant.key};
    }
    instructions[instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_PROVIDER_CALL,
        .result = result,
        .result_type_id = XR_CORE_TYPE_I64,
        .operands = nullary ? NULL : call_operands,
        .operand_count = nullary ? 0u : 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
        .immediate.provider_operation = {.contract_id = contract->contract_id,
                                         .operation_id = operation_id}};
    instructions[instruction_count++] =
        (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_RETURN,
                                    .result_type_id = XR_CORE_TYPE_VOID,
                                    .operands = returned,
                                    .operand_count = 1u,
                                    .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE};
    XrCoreIrKey block_key = fixture_key("aot-provider:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = instruction_count,
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("aot-provider:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = fixture_key("aot-provider:module"),
        .constants = nullary ? NULL : &constant,
        .constant_count = nullary ? 0u : 1u,
        .functions = &function,
        .function_count = 1u,
    };
    XrProgramProviderOperationRequirement operation_requirement = {
        .operation_id = operation_id,
        .logical_contract = xr_program_fixture_scalar_contract(nullary),
    };
    XrCoreIrProviderRequirementInput provider_requirement = {
        .contract_id = contract->contract_id,
        .operations = &operation_requirement,
        .operation_count = 1u,
    };
    XrCoreIrKey semantic = fixture_key("aot-provider:semantic");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .provider_requirements = &provider_requirement,
        .provider_requirement_count = 1u,
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
    XrProgramVerifyStatus status =
        xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &verify_diagnostic);
    if (status != XR_PROGRAM_VERIFY_OK)
        fprintf(stderr,
                "provider fixture verification failed: status=%s diagnostic=%s function=%u "
                "block=%u instruction=%u value=%u\n",
                xr_program_verify_status_name(status),
                xr_program_diagnostic_kind_name(verify_diagnostic.kind),
                verify_diagnostic.location.function_id, verify_diagnostic.location.block_id,
                verify_diagnostic.location.instruction_id, verify_diagnostic.location.value_id);
    REQUIRE(status == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(core);
    return program;
}

static XrValidatedProgram *build_output_program(int64_t value) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_output_fixture_write(value, &artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
}

static XrValidatedProgram *build_text_program(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_text_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
}

static XrValidatedProgram *build_pipe_program(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_pipe_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
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

static XrValidatedProgram *build_existential_reborrow_program(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_reborrow_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
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

static XrValidatedProgram *build_coroutine_program(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_coroutine_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
}

static XrValidatedProgram *build_owner_coroutine_program(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_coroutine_owner_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
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
                                     .providers = bindings->count ? bindings->providers : NULL,
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

static XrBackendIR *build_ir(const XrValidatedProgram *program, const XrTargetProfile *profile,
                             uint8_t optimization_policy) {
    XrBackendOptions options = xr_backend_default_options();
    options.optimization_policy = optimization_policy;
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    XrBackendStatus status = xr_backend_ir_build(program, profile, &options, &ir, &diagnostic);
    if (status != XR_BACKEND_OK)
        fprintf(stderr, "backend build failed: %s op=%u f=%u b=%u i=%u\n",
                xr_backend_status_name(status), diagnostic.operation_id, diagnostic.function_id,
                diagnostic.block_id, diagnostic.instruction_id);
    REQUIRE(status == XR_BACKEND_OK);
    REQUIRE(ir != NULL);
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(xr_backend_ir_binding_verify(ir, &diagnostic));
    return ir;
}

static void test_direct_program_lifetime_and_emission(void) {
    XrValidatedProgram *program = build_text_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    REQUIRE(ir->program == program && ir->profile == profile);
    XrGeneratedC before = {0}, after = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &before, &diagnostic) == XR_BACKEND_OK);
    XrBackendIR *retained = xr_backend_ir_retain(ir);
    REQUIRE(retained == ir);
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
    xr_backend_ir_free(ir);
    REQUIRE(xr_backend_ir_binding_verify(retained, &diagnostic));
    REQUIRE(xr_backend_ir_emit_c(retained, false, &after, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(before.size == after.size && memcmp(before.bytes, after.bytes, before.size) == 0);
    REQUIRE(xr_fingerprint_equal(before.source_digest, after.source_digest));
    xr_backend_ir_free(retained);
    REQUIRE(strstr(after.bytes, "xr_aot_string_concat"));
    xr_generated_c_free(&before);
    xr_generated_c_free(&after);
}

static void test_coroutine_private_state_machine_lowering(void) {
    XrValidatedProgram *program = build_coroutine_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "typedef struct XrAotCoroutineFrame0") != NULL);
    REQUIRE(strstr(generated.bytes, "switch (frame->state)") != NULL);
    REQUIRE(strstr(generated.bytes, "frame->live_0_0 = v") != NULL);
    REQUIRE(strstr(generated.bytes, "return xr_aot_suspend(UINT32_C(0), UINT32_C(1), UINT32_C(0), "
                                    "INT64_C(0))") != NULL);
    REQUIRE(strstr(generated.bytes, "xr_aot_entry_coroutine_step(void *opaque)") != NULL);
    REQUIRE(strstr(generated.bytes, "xr_aot_entry_coroutine_cancel(void *opaque)") != NULL);
    REQUIRE(strstr(generated.bytes, "return xr_aot_make(6, 0, 0)") != NULL);
    REQUIRE(strstr(generated.bytes, "XrBackendNativeOutcome") != NULL);
    REQUIRE(strstr(generated.bytes, "xr_aot_entry_coroutine_descriptor") != NULL);
    REQUIRE(strstr(generated.bytes, "XrBackendNativeExecutionId execution_id") != NULL);
    REQUIRE(strstr(generated.bytes, "if (result.kind != UINT32_C(5)) break;") != NULL);
    REQUIRE(strstr(generated.bytes, "result.suspension_operand_count == UINT32_C(0)) continue;") !=
            NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_resource_storage_admission(void) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    XrCoreIrTypeInput type = {0};
    type.key.bytes[0] = 1u;
    type.local_id = 90u;
    type.kind = XR_CORE_IR_TYPE_PROVIDER_RESOURCE;
    type.ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    type.copy_contract = XR_CORE_IR_COPY_FORBIDDEN;
    type.resource_id.bytes[0] = 1u;
    fixture.input.types = &type;
    fixture.input.type_count = 1u;
    XrCoreIrProgram *core = NULL;
    REQUIRE(xr_core_ir_program_build(&fixture.input, &core, NULL, 0u) == XR_PROGRAM_BUILD_OK);
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_write(core, &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
    xr_core_ir_program_free(core);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
    REQUIRE(code != NULL);
    xr_vm_code_free(code);
    XrBackendIR *ir = NULL;
    XrBackendOptions options = xr_backend_default_options();
      REQUIRE(xr_backend_ir_build(program, profile, &options, &ir, NULL) == XR_BACKEND_OK);
      REQUIRE(ir != NULL);
      xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_coroutine_self_loop_parallel_edges_lowering(void) {
    XrProgramArtifact artifact = {0};
    char message[256] = {0};
    REQUIRE(xr_program_coroutine_branch_fixture_write(&artifact, message, sizeof(message)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic program_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &program_diagnostic) == XR_PROGRAM_VERIFY_OK);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    const XrValidatedFunction *function = &ir->program->functions[ir->program->entry_function];
    REQUIRE(function->block_count == 4u && function->coroutine_state_count == 2u);
    uint32_t self_edges = 0u;
    for (uint32_t b = 0u; b < function->block_count; ++b) {
        const XrValidatedBlock *block = &function->blocks[b];
        const XrValidatedInstruction *terminal =
            &block->instructions[block->instruction_count - 1u];
        if (terminal->operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH) {
            REQUIRE(terminal->successors[0] == b && block->argument_count == 3u);
            REQUIRE(terminal->operands[1] == block->argument_ids[1]);
            REQUIRE(terminal->operands[2] == block->argument_ids[0]);
            ++self_edges;
        }
    }
    REQUIRE(self_edges == 1u);
    XrGeneratedC first = {0}, second = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &first, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(xr_backend_ir_emit_c(ir, false, &second, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(first.size != 0u && first.size == second.size);
    REQUIRE(memcmp(first.bytes, second.bytes, first.size) == 0);
    xr_generated_c_free(&second);
    xr_generated_c_free(&first);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

static void test_coroutine_owner_cancel_cleanup_lowering(void) {
    XrValidatedProgram *program = build_owner_coroutine_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    XrBackendStatus status = xr_backend_ir_emit_c(ir, true, &generated, &diagnostic);
    if (status != XR_BACKEND_OK)
        fprintf(stderr, "owner coroutine emit failed: %s op=%u f=%u b=%u i=%u\n",
                xr_backend_status_name(status), diagnostic.operation_id, diagnostic.function_id,
                diagnostic.block_id, diagnostic.instruction_id);
    REQUIRE(status == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "frame->live_0_0 = v") != NULL);
    REQUIRE(strstr(generated.bytes, " = frame->live_0_0;") != NULL);
    REQUIRE(strstr(generated.bytes, "edge_0 = v") != NULL);
    REQUIRE(strstr(generated.bytes, "(void)v") != NULL);
    REQUIRE(strstr(generated.bytes, "return xr_aot_make(6, 0, 0)") != NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
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
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, &vm_diagnostic) == XR_VM_CODE_OK);
    XrVmOutcome vm =
        xr_vm_code_execute(code, instance, xr_validated_program_entry_function(program), NULL, 0u);
    REQUIRE(vm.kind == XR_VM_OUTCOME_RETURN);
    REQUIRE(vm.value.kind == XR_VM_VALUE_I64);
    REQUIRE(vm.value.as.i64 == 42);
    xr_vm_code_free(code);

    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    REQUIRE(ir->instruction_count == 7u);
    bool found_copy = false;
    for (uint32_t function = 0; function < ir->program->function_count; ++function) {
        for (uint32_t block = 0; block < ir->program->functions[function].block_count; ++block) {
            const XrValidatedBlock *row = &ir->program->functions[function].blocks[block];
            for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
                const XrValidatedInstruction *op = &row->instructions[instruction];
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

static void test_empty_aggregate_has_portable_private_c_storage(void) {
    XrValidatedProgram *program = build_empty_aggregate_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "    uint8_t xr_unit;\n") != NULL);
    REQUIRE(strstr(generated.bytes, ".xr_unit = UINT8_C(0)") != NULL);
    REQUIRE(strstr(generated.bytes, "{\n};") == NULL);

    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
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
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
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
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

static void test_sealed_invoke_trap_continuation_lowering_and_mutation(void) {
    XrProgramArtifact artifact = {0};
    char build_diagnostic[256] = {0};
    REQUIRE(xr_program_invoke_fixture_write_mutated(XR_INVOKE_FIXTURE_TRAP_CONTINUATION, &artifact,
                                                    build_diagnostic, sizeof(build_diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrValidatedFunction *invoke_function = NULL;
    XrValidatedInstruction *invoke = NULL;
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        for (uint32_t block = 0u; block < ir->program->functions[function].block_count; ++block) {
            XrValidatedBlock *row = &ir->program->functions[function].blocks[block];
            for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
                if (row->instructions[instruction].operation_id ==
                    XR_CORE_OP_CORE_CALL_SEALED_INVOKE) {
                    REQUIRE(invoke == NULL);
                    invoke_function = &ir->program->functions[function];
                    invoke = &row->instructions[instruction];
                }
            }
        }
    }
    REQUIRE(invoke_function != NULL);
    REQUIRE(invoke != NULL);
    REQUIRE(invoke->successor_count == 3u);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, ".kind == 1 && call_") != NULL);
    REQUIRE(strstr(generated.bytes, ".trap == 7") != NULL);

    uint32_t trap_block = invoke->successors[2];
    REQUIRE(trap_block < invoke_function->block_count);
    XrValidatedBlock *target = &invoke_function->blocks[trap_block];
    REQUIRE(target->instruction_count != 0u);
    XrValidatedInstruction *trap = &target->instructions[target->instruction_count - 1u];
    REQUIRE(trap->operation_id == XR_CORE_OP_CORE_TRAP);
    trap->immediate.u32 = 4u;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    trap->immediate.u32 = 7u;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));

    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
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
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
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
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

static void test_existential_pack_test_project_lowering(void) {
    XrValidatedProgram *program = build_existential_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
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
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_callable_pack_and_indirect_call_lowering(void) {
    XrValidatedProgram *program = build_callable_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
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
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_reference_vm_aot_identity(XrValidatedProgram *program, XrTargetProfile *profile,
                                           XrInstance *instance) {
    XrReferenceProfile reference_profile = {.pointer_width = 64u};
    XrReferenceOutcome reference = xr_reference_evaluate(
        program, xr_validated_program_entry_function(program), NULL, 0u, &reference_profile, NULL);
    REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RETURN);
    REQUIRE(reference.value.kind == XR_REFERENCE_VALUE_I64);
    REQUIRE(reference.value.as.i64 == 42);

    XrVmCode *code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, &vm_diagnostic) == XR_VM_CODE_OK);
    XrVmOutcome vm =
        xr_vm_code_execute(code, instance, xr_validated_program_entry_function(program), NULL, 0u);
    REQUIRE(vm.kind == XR_VM_OUTCOME_RETURN);
    REQUIRE(vm.value.kind == XR_VM_VALUE_I64);
    REQUIRE(vm.value.as.i64 == reference.value.as.i64);
    xr_vm_code_free(code);

    XrBackendIR *none = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_NONE);
    XrBackendIR *portable = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrExecutionId pure_execution_id;
    REQUIRE(xr_execution_id_compute(program, profile, &pure_execution_id));
    REQUIRE(xr_fingerprint_equal(pure_execution_id, xr_execution_instance_id(instance)));
    REQUIRE(xr_fingerprint_equal(xr_backend_ir_execution_id(none),
                                 xr_backend_ir_execution_id(portable)));
    REQUIRE(!xr_fingerprint_equal(xr_backend_ir_optimization_policy_id(none),
                                  xr_backend_ir_optimization_policy_id(portable)));
    REQUIRE(xr_backend_ir_instruction_count(none) == 50u);

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
    REQUIRE(strstr(generated_none.bytes, " = &v") != NULL);
    REQUIRE(strstr(generated_none.bytes, "xr_place_") == NULL);
    REQUIRE(strstr(generated_none.bytes, " = *v") != NULL);
    REQUIRE(strstr(generated_none.bytes, "*v") != NULL);
    REQUIRE(strstr(generated_none.bytes, "XrVm") == NULL);
    REQUIRE(strstr(generated_none.bytes, "XrAotValue") == NULL);
    REQUIRE(strstr(generated_none.bytes, "TargetPlan") == NULL);
    REQUIRE(strstr(generated_none.bytes, "int main(void)") != NULL);
    REQUIRE(strstr(generated_none.bytes, "    return xr_aot_make(4, 0, 0);\n}\n") == NULL);

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

static void test_foreign_profile_and_program_binding_mutation(void) {
    uint8_t representation = 0u;
    REQUIRE(xr_backend_representation_for_type(XR_CORE_TYPE_U16, &representation));
    REQUIRE(representation == XR_BACKEND_VALUE_U16);
    REQUIRE(xr_backend_representation_for_type(XR_CORE_TYPE_TARGET_OS, &representation));
    REQUIRE(representation == XR_BACKEND_VALUE_TARGET_ENUM_U16);

    XrValidatedProgram *program =
        build_target_query_program(XR_CORE_OP_CORE_TARGET_POINTER_WIDTH, XR_CORE_TYPE_U16,
                                   XR_CORE_CAPABILITY_PROFILE_POINTER_WIDTH);
    XrTargetProfile *profile =
        xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_FREESTANDING);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "uint16_t v0") != NULL);
    REQUIRE(strstr(generated.bytes, "UINT16_C(32)") != NULL);
    REQUIRE(strstr(generated.bytes, "result.u16") != NULL);
    REQUIRE(strstr(generated.bytes, "v0 = UINT16_C(32);") != NULL);

    uint16_t saved_type = ir->program->functions[0].value_types[0];
    ir->program->functions[0].value_types[0] = XR_CORE_TYPE_U32;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    ir->program->functions[0].value_types[0] = saved_type;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));

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

    XrValidatedProgram *other =
        build_target_query_program(XR_CORE_OP_CORE_TARGET_ENDIANNESS, XR_CORE_TYPE_TARGET_ENDIAN,
                                   XR_CORE_CAPABILITY_PROFILE_ENDIANNESS);
    ir->program = other;
    REQUIRE(!xr_backend_ir_binding_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_BINDING_REJECTED);
    XrGeneratedC rejected_c = {0};
    REQUIRE(xr_backend_ir_emit_c(ir, false, &rejected_c, &diagnostic) != XR_BACKEND_OK);
    REQUIRE(!rejected_c.bytes && !rejected_c.size);
    ir->program = program;
    xr_validated_program_free(other);
    REQUIRE(xr_backend_ir_binding_verify(ir, &diagnostic));

    XrBackendOptions constrained = xr_backend_default_options();
    constrained.max_instructions = 1u;
    XrBackendIR *rejected = NULL;
    REQUIRE(xr_backend_ir_build(NULL, profile, &constrained, &rejected, &diagnostic) ==
            XR_BACKEND_INVALID_INPUT);
    REQUIRE(xr_backend_ir_build(program, NULL, &constrained, &rejected, &diagnostic) ==
            XR_BACKEND_INVALID_INPUT);
    REQUIRE(xr_backend_ir_build(program, profile, &constrained, &rejected, &diagnostic) ==
            XR_BACKEND_RESOURCE_LIMIT);
    REQUIRE(rejected == NULL);

    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);

    struct TargetQueryCase {
        uint16_t operation;
        uint16_t result_type;
        uint32_t capability;
        uint16_t expected;
    } cases[] = {
        {XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM, XR_CORE_TYPE_TARGET_OS,
         XR_CORE_CAPABILITY_PROFILE_OPERATING_SYSTEM, XR_TARGET_OS_WASI},
        {XR_CORE_OP_CORE_TARGET_ARCHITECTURE, XR_CORE_TYPE_TARGET_ARCH,
         XR_CORE_CAPABILITY_PROFILE_ARCHITECTURE, XR_TARGET_ARCH_WASM32},
        {XR_CORE_OP_CORE_TARGET_NATIVE_ABI, XR_CORE_TYPE_TARGET_ABI,
         XR_CORE_CAPABILITY_PROFILE_NATIVE_ABI, XR_TARGET_ABI_WASM},
        {XR_CORE_OP_CORE_TARGET_ENDIANNESS, XR_CORE_TYPE_TARGET_ENDIAN,
         XR_CORE_CAPABILITY_PROFILE_ENDIANNESS, XR_TARGET_ENDIAN_LITTLE},
    };
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        program = build_target_query_program(cases[index].operation, cases[index].result_type,
                                             cases[index].capability);
        profile = xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_FREESTANDING);
        REQUIRE(profile != NULL);
        ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
        generated = (XrGeneratedC) {0};
        REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
        char literal[48];
        (void) snprintf(literal, sizeof(literal), "v0 = UINT16_C(%u);", cases[index].expected);
        REQUIRE(strstr(generated.bytes, literal) != NULL);
        xr_generated_c_free(&generated);
        xr_backend_ir_free(ir);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
    }

    program = build_target_enum_equality_program();
    profile = xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_FREESTANDING);
    REQUIRE(profile != NULL);
    ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    generated = (XrGeneratedC) {0};
    REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "UINT16_C(4)") != NULL);
    REQUIRE(strstr(generated.bytes, " == ") != NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_provider_call_lowering_and_mutation(void) {
    for (uint32_t shape = 0u; shape < 2u; ++shape) {
        bool nullary = shape != 0u;
        XrTargetProfile *profile = nullary ? xr_test_target_profile_build_with_nullary_clock(
                                                 false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
                                                 XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER)
                                           : xr_test_target_profile_build_with_scalar_clock(
                                                 false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
                                                 XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
        REQUIRE(profile != NULL);
        XrValidatedProgram *program = build_provider_call_program(profile, nullary);
        XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
        XrValidatedInstruction *provider_call = NULL;
        uint16_t expected_operation = XR_CORE_OP_CORE_PROVIDER_CALL;
        for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
            for (uint32_t block = 0u; block < ir->program->functions[function].block_count;
                 ++block) {
                XrValidatedBlock *row = &ir->program->functions[function].blocks[block];
                for (uint32_t instruction = 0u; instruction < row->instruction_count;
                     ++instruction) {
                    if (row->instructions[instruction].operation_id == expected_operation) {
                        REQUIRE(provider_call == NULL);
                        provider_call = &row->instructions[instruction];
                    }
                }
            }
        }
        REQUIRE(provider_call != NULL);
        REQUIRE(provider_call->immediate.provider_operation.requirement_index == 0u);
        REQUIRE(provider_call->immediate.provider_operation.operation_index == 0u);
        XrGeneratedC generated = {0};
        XrBackendDiagnostic diagnostic;
        REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
        REQUIRE(strstr(generated.bytes,
                       "provider_call_typed") != NULL);
        REQUIRE(strstr(generated.bytes, "UINT32_C(0), UINT32_C(0)") != NULL);
        REQUIRE(strstr(generated.bytes, "xr_aot_make(1, 0, 7)") != NULL);

        provider_call->immediate.provider_operation.requirement_index = 1u;
        REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
        REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
        provider_call->immediate.provider_operation.requirement_index = 0u;
        REQUIRE(xr_backend_ir_verify(ir, &diagnostic));

        xr_generated_c_free(&generated);
        xr_backend_ir_free(ir);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
    }
}

static void test_condition_assert_panic_cleanup_lowering(void) {
    XrProgramArtifact artifact = {0};
    char build_diagnostic[256] = {0};
    REQUIRE(xr_program_assert_fixture_write(&artifact, build_diagnostic,
                                            sizeof(build_diagnostic)) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrValidatedInstruction *assertion = NULL;
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        for (uint32_t block = 0u; block < ir->program->functions[function].block_count; ++block) {
            XrValidatedBlock *row = &ir->program->functions[function].blocks[block];
            for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
                if (row->instructions[instruction].operation_id ==
                    XR_CORE_OP_CORE_ASSERT_CONDITION) {
                    REQUIRE(assertion == NULL);
                    assertion = &row->instructions[instruction];
                }
            }
        }
    }
    REQUIRE(assertion != NULL);
    REQUIRE(assertion->successor_count == 1u);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(ir->program->function_count == 1u);
    XrValidatedFunction *function = &ir->program->functions[ir->program->entry_function];
    XrValidatedBlock *panic = &function->blocks[assertion->successors[0]];
    REQUIRE(panic->argument_count == 2u);
    REQUIRE(panic->argument_types[0] == XR_CORE_TYPE_PANIC_INFO);
    REQUIRE(panic->argument_ownerships[0] == XR_CORE_IR_OWNER);
    uint32_t panic_value = panic->argument_ids[0];
    REQUIRE(function->value_ownerships[panic_value] == XR_CORE_IR_OWNER);
    XrFingerprint saved_digest = ir->lowering_digest;
    panic->argument_ownerships[0] = XR_CORE_IR_NON_OWNER;
    function->value_ownerships[panic_value] = XR_CORE_IR_NON_OWNER;
    xr_backend_compute_lowering_digest(ir, &ir->lowering_digest);
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    REQUIRE(diagnostic.operation_id == XR_CORE_OP_CORE_ASSERT_CONDITION);
    panic->argument_ownerships[0] = XR_CORE_IR_OWNER;
    function->value_ownerships[panic_value] = XR_CORE_IR_OWNER;
    ir->lowering_digest = saved_digest;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(xr_backend_ir_binding_verify(ir, &diagnostic));
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "if (!v") != NULL);
    REQUIRE(strstr(generated.bytes, "UINT32_C(1)") != NULL);
    REQUIRE(strstr(generated.bytes, "goto xr_f") != NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

static void test_provider_trap_continuation_lowering_and_mutation(void) {
    XrTargetProfile *profile = xr_test_target_profile_build_with_nullary_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED, XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    REQUIRE(profile != NULL);
    const XrTargetProviderContract *contract = NULL;
    for (size_t index = 0u; index < xr_target_profile_provider_count(profile); ++index) {
        const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, index);
        if (xr_test_target_profile_is_scalar_provider(candidate)) {
            REQUIRE(contract == NULL);
            contract = candidate;
        }
    }
    REQUIRE(contract != NULL && contract->operation_count == 1u);
    XrProgramArtifact artifact = {0};
    char build_diagnostic[256] = {0};
    REQUIRE(xr_program_trap_fixture_write_with_ids(
                contract->contract_id, contract->operations[0].stable_id, &artifact,
                build_diagnostic, sizeof(build_diagnostic)) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);

    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrValidatedInstruction *provider_call = NULL;
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        for (uint32_t block = 0u; block < ir->program->functions[function].block_count; ++block) {
            XrValidatedBlock *row = &ir->program->functions[function].blocks[block];
            for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
                if (row->instructions[instruction].operation_id == XR_CORE_OP_CORE_PROVIDER_CALL) {
                    REQUIRE(provider_call == NULL);
                    provider_call = &row->instructions[instruction];
                }
            }
        }
    }
    REQUIRE(provider_call != NULL);
    REQUIRE(provider_call->successor_count == 1u);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "provider_call_typed") != NULL);
    REQUIRE(strstr(generated.bytes, "goto xr_f0_b1") != NULL);
    REQUIRE(strstr(generated.bytes, "XR_AOT_FAIL(xr_aot_make(1, 0, 7))") != NULL);

    uint32_t *saved_successors = provider_call->successors;
    uint32_t duplicate_successors[] = {saved_successors[0], saved_successors[0]};
    provider_call->successors = duplicate_successors;
    provider_call->successor_count = 2u;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    provider_call->successors = saved_successors;
    provider_call->successor_count = 1u;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));

    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
}

static void require_backend_operation_rejected(XrBackendIR *ir, const char *mutation,
                                               uint16_t expected_operation) {
    // A refreshed digest prevents integrity drift from masking missing semantic checks.
    XrFingerprint saved = ir->lowering_digest;
    xr_backend_compute_lowering_digest(ir, &ir->lowering_digest);
    XrBackendDiagnostic diagnostic = {0};
    bool accepted = xr_backend_ir_verify(ir, &diagnostic);
    if (accepted || diagnostic.operation_id != expected_operation)
        fprintf(stderr, "coroutine BackendIR mutation %s: accepted=%d status=%s op=%u\n", mutation,
                accepted, xr_backend_status_name(diagnostic.status), diagnostic.operation_id);
    REQUIRE(!accepted);
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    REQUIRE(diagnostic.operation_id == expected_operation);
    ir->lowering_digest = saved;
}

static void require_coroutine_backend_rejected(XrBackendIR *ir, const char *mutation) {
    require_backend_operation_rejected(ir, mutation, XR_CORE_OP_CORE_COROUTINE_CALL_SEALED);
}

static void require_coroutine_backend_restored(XrBackendIR *ir) {
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(xr_backend_ir_binding_verify(ir, &diagnostic));
}

static XrValidatedProgram *build_cleanup_graph_program(const XrTargetProfile *profile) {
    XrProgramCleanupGraphFixture fixture;
    REQUIRE(xr_program_cleanup_graph_fixture_init(&fixture));
    const XrTargetProviderContract *contract = NULL;
    for (size_t p = 0u; p < xr_target_profile_provider_count(profile); ++p) {
        const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, p);
        if (xr_test_target_profile_is_scalar_provider(candidate)) {
            REQUIRE(contract == NULL);
            contract = candidate;
        }
    }
    REQUIRE(contract && contract->operation_count == 1u);
    fixture.requirement.contract_id = contract->contract_id;
    fixture.operation_requirement.operation_id = contract->operations[0].stable_id;
    for (uint32_t b = 0u; b < XR_CLEANUP_GRAPH_BLOCK_COUNT; ++b) {
        for (uint32_t i = 0u; i < fixture.blocks[b].instruction_count; ++i) {
            XrCoreIrInstructionInput *instruction = &fixture.instructions[b][i];
            if (instruction->operation_id == XR_CORE_OP_CORE_PROVIDER_CALL) {
                instruction->immediate.provider_operation.contract_id =
                    fixture.requirement.contract_id;
                instruction->immediate.provider_operation.operation_id =
                    fixture.operation_requirement.operation_id;
            }
        }
    }
    XrProgramArtifact artifact = {0};
    char message[256] = {0};
    REQUIRE(xr_program_cleanup_graph_fixture_write_input(&fixture, &artifact, message,
                                                         sizeof(message)) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic diagnostic;
    XrProgramVerifyStatus status =
        xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &diagnostic);
    if (status != XR_PROGRAM_VERIFY_OK)
        fprintf(stderr, "cleanup graph validation failed: kind=%u f=%u b=%u i=%u\n",
                diagnostic.kind, diagnostic.location.function_id, diagnostic.location.block_id,
                diagnostic.location.instruction_id);
    REQUIRE(status == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
}

static XrValidatedInstruction *cleanup_graph_terminal(XrValidatedFunction *function,
                                                      uint32_t block) {
    REQUIRE(block < function->block_count && function->blocks[block].instruction_count != 0u);
    XrValidatedBlock *row = &function->blocks[block];
    return &row->instructions[row->instruction_count - 1u];
}

static void test_cleanup_graph_exit_mutations(XrBackendIR *ir, uint32_t block_id, bool cancel) {
    XrValidatedFunction *function = &ir->program->functions[ir->program->entry_function];
    XrValidatedInstruction *terminal = cleanup_graph_terminal(function, block_id);
    XrValidatedInstruction saved = *terminal;
    uint32_t operand = function->blocks[block_id].argument_ids[0];
    const uint16_t exits[] = {XR_CORE_OP_CORE_RETURN, XR_CORE_OP_CORE_ERROR_PUBLISH,
                              XR_CORE_OP_CORE_PANIC_PUBLISH, XR_CORE_OP_CORE_TRAP,
                              XR_CORE_OP_CORE_CANCEL_PUBLISH};
    for (size_t i = 0u; i < sizeof(exits) / sizeof(exits[0]); ++i) {
        if (cancel && exits[i] == XR_CORE_OP_CORE_CANCEL_PUBLISH)
            continue;
        *terminal = saved;
        terminal->operation_id = exits[i];
        terminal->immediate_kind =
            exits[i] == XR_CORE_OP_CORE_TRAP ? XR_CORE_IR_IMMEDIATE_U32 : XR_CORE_IR_IMMEDIATE_NONE;
        terminal->immediate.u32 = cancel ? 7u : 4u;
        if (exits[i] == XR_CORE_OP_CORE_ERROR_PUBLISH ||
            exits[i] == XR_CORE_OP_CORE_PANIC_PUBLISH) {
            terminal->operand_count = 1u;
            terminal->operands = &operand;
        }
        require_backend_operation_rejected(ir, "cleanup explicit exit changes reason", exits[i]);
        *terminal = saved;
        require_coroutine_backend_restored(ir);
    }
}

static void test_cleanup_graph_storage_mutations(XrBackendIR *ir, uint32_t block_id) {
    XrValidatedFunction *function = &ir->program->functions[ir->program->entry_function];
    XrValidatedBlock *block = &function->blocks[block_id];
    XrValidatedInstruction *branch = cleanup_graph_terminal(function, block_id);
    XrValidatedInstruction saved = *branch;
    XrValidatedInstruction *saved_instructions = block->instructions;
    XrBackendDiagnostic diagnostic;
    // Invalid storage cannot be hashed. It must be rejected before following the graph.
    block->instructions = NULL;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    block->instructions = saved_instructions;
    branch->successors = NULL;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    *branch = saved;
    branch->operands = NULL;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    *branch = saved;
    uint32_t targets[] = {function->block_count, saved.successors[0]};
    branch->successors = targets;
    require_backend_operation_rejected(ir, "cleanup successor outside function",
                                       XR_CORE_OP_CORE_BRANCH);
    targets[0] = saved.successors[0];
    branch->successor_count = 2u;
    require_backend_operation_rejected(ir, "branch with invented reason successor",
                                       XR_CORE_OP_CORE_BRANCH);
    *branch = saved;
    require_coroutine_backend_restored(ir);
}

static void test_cleanup_graph_suspension_rejected(XrBackendIR *ir, uint32_t block_id,
                                                   uint32_t cancel_exit) {
    XrValidatedFunction *function = &ir->program->functions[ir->program->entry_function];
    REQUIRE(function->coroutine_state_count == 2u && function->coroutine_safepoint_count == 1u);
    XrValidatedInstruction *terminal = cleanup_graph_terminal(function, block_id);
    XrValidatedInstruction saved = *terminal;
    XrValidatedCoroutineState *saved_states = function->coroutine_states;
    XrValidatedCoroutineSafepoint *saved_points = function->coroutine_safepoints;
    uint32_t owner = function->blocks[block_id].argument_ids[0];
    uint32_t operands[] = {owner, owner};
    uint32_t successors[] = {block_id, cancel_exit};
    XrValidatedCoroutineState states[] = {
        saved_states[0], saved_states[1], {.continuation_block = block_id}};
    XrValidatedCoroutineSafepoint points[] = {
        saved_points[0], {.resume_state_id = 2u, .live_value_ids = &owner, .live_value_count = 1u}};
    *terminal = (XrValidatedInstruction) {.operation_id = XR_CORE_OP_CORE_COROUTINE_YIELD,
                                          .result_type_id = XR_CORE_TYPE_VOID,
                                          .result_id = XR_PROGRAM_LOCATION_NONE,
                                          .operands = operands,
                                          .operand_count = 2u,
                                          .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
                                          .immediate.u32 = 1u,
                                          .successors = successors,
                                          .successor_count = 2u};
    function->coroutine_states = states;
    function->coroutine_state_count = 3u;
    function->coroutine_safepoints = points;
    function->coroutine_safepoint_count = 2u;
    require_backend_operation_rejected(ir, "cleanup cannot publish suspended outcome",
                                       XR_CORE_OP_CORE_COROUTINE_YIELD);
    *terminal = saved;
    function->coroutine_states = saved_states;
    function->coroutine_state_count = 2u;
    function->coroutine_safepoints = saved_points;
    function->coroutine_safepoint_count = 1u;
    require_coroutine_backend_restored(ir);
}

static void test_cleanup_graph_owner_edges(XrBackendIR *ir, uint32_t adapter_id) {
    XrValidatedFunction *function = &ir->program->functions[ir->program->entry_function];
    XrValidatedFunction saved_function = *function;
    XrValidatedBlock *adapter = &function->blocks[adapter_id];
    XrValidatedBlock saved_adapter = *adapter;
    REQUIRE(adapter->instruction_count == 2u);
    uint32_t original = adapter->argument_ids[0];
    uint32_t copied = function->value_count;
    uint32_t second_argument = copied + 1u;
    size_t capacity = (size_t) function->value_count + 2u;
    function->value_types = xr_malloc(capacity * sizeof(*function->value_types));
    function->value_categories = xr_malloc(capacity * sizeof(*function->value_categories));
    function->value_ownerships = xr_malloc(capacity * sizeof(*function->value_ownerships));
    REQUIRE(function->value_types && function->value_categories && function->value_ownerships);
    for (uint32_t v = 0u; v < capacity; ++v) {
        uint32_t source = v < saved_function.value_count ? v : original;
        function->value_types[v] = saved_function.value_types[source];
        function->value_categories[v] = saved_function.value_categories[source];
        function->value_ownerships[v] = saved_function.value_ownerships[source];
    }
    function->value_count = copied + 1u;
    uint32_t passed[] = {original, original};
    XrValidatedInstruction instructions[4] = {
        saved_adapter.instructions[0],
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY,
         .result_type_id = function->value_types[original],
         .result_id = copied,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = &original,
         .operand_count = 1u},
        saved_adapter.instructions[1],
    };
    instructions[2].operands = passed;
    adapter->instructions = instructions;
    adapter->instruction_count = 3u;
    require_backend_operation_rejected(ir, "branch omits copied live owner",
                                       XR_CORE_OP_CORE_BRANCH);
    passed[0] = copied;
    require_backend_operation_rejected(ir, "same-type substitution abandons original owner",
                                       XR_CORE_OP_CORE_BRANCH);

    uint32_t target_id = instructions[2].successors[0];
    XrValidatedBlock *target = &function->blocks[target_id];
    XrValidatedBlock saved_target = *target;
    REQUIRE(target->argument_count == 1u && target->instruction_count < 6u);
    uint32_t ids[] = {target->argument_ids[0], second_argument};
    uint16_t types[] = {target->argument_types[0], target->argument_types[0]};
    XrCoreIrValueCategory categories[] = {XR_CORE_IR_VALUE, XR_CORE_IR_VALUE};
    XrCoreIrOwnershipDisposition ownerships[] = {XR_CORE_IR_OWNER, XR_CORE_IR_OWNER};
    XrValidatedInstruction target_instructions[8];
    target_instructions[0] = target->instructions[0];
    target_instructions[1] =
        (XrValidatedInstruction) {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
                                  .result_type_id = XR_CORE_TYPE_VOID,
                                  .result_id = XR_PROGRAM_LOCATION_NONE,
                                  .operands = &second_argument,
                                  .operand_count = 1u};
    target_instructions[2] = target_instructions[1];
    target_instructions[2].operation_id = XR_CORE_OP_CORE_OWNER_DROP;
    memcpy(target_instructions + 3u, target->instructions + 1u,
           (target->instruction_count - 1u) * sizeof(*target_instructions));
    target->instructions = target_instructions;
    target->instruction_count += 2u;
    target->argument_ids = ids;
    target->argument_types = types;
    target->argument_categories = categories;
    target->argument_ownerships = ownerships;
    target->argument_count = 2u;
    function->value_count++;
    instructions[2].operand_count = 2u;
    passed[0] = original;
    require_backend_operation_rejected(ir, "duplicate owner on one edge loses second owner",
                                       XR_CORE_OP_CORE_BRANCH);
    *target = saved_target;
    function->value_count--;
    instructions[2].operand_count = 1u;

    instructions[3] = instructions[2];
    instructions[2] = (XrValidatedInstruction) {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
                                                .result_type_id = XR_CORE_TYPE_VOID,
                                                .result_id = XR_PROGRAM_LOCATION_NONE,
                                                .operands = &copied,
                                                .operand_count = 1u};
    adapter->instruction_count = 4u;
    passed[0] = copied;
    require_backend_operation_rejected(ir, "edge uses already consumed owner",
                                       XR_CORE_OP_CORE_BRANCH);

    // A fresh owner may legally replace the old identity after the old one is dropped.
    instructions[2].operands = &original;
    XrFingerprint saved_digest = ir->lowering_digest;
    xr_backend_compute_lowering_digest(ir, &ir->lowering_digest);
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));
    /* This test edits the private fixture to exercise physical owner flow.
     * Program rebinding
     * is tested separately; no second logical graph exists. */
    ir->lowering_digest = saved_digest;
    xr_free(function->value_types);
    xr_free(function->value_categories);
    xr_free(function->value_ownerships);
    *adapter = saved_adapter;
    *function = saved_function;
    require_coroutine_backend_restored(ir);
}

static void test_cleanup_reason_graph_lowering_and_mutation(void) {
    XrTargetProfile *profile = xr_test_target_profile_build_with_scalar_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED, XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    REQUIRE(profile != NULL);
    XrValidatedProgram *program = build_cleanup_graph_program(profile);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrValidatedFunction *function = &ir->program->functions[ir->program->entry_function];
    REQUIRE(function->block_count == XR_CLEANUP_GRAPH_BLOCK_COUNT);
    XrValidatedInstruction *yield = cleanup_graph_terminal(function, function->entry_block);
    REQUIRE(yield->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD);
    uint32_t normal_id = yield->successors[0];
    uint32_t cancel_adapter = yield->successors[1];
    XrValidatedInstruction *cancel_entry = cleanup_graph_terminal(function, cancel_adapter);
    REQUIRE(cancel_entry->operation_id == XR_CORE_OP_CORE_BRANCH);
    XrValidatedInstruction *cancel_branch =
        cleanup_graph_terminal(function, cancel_entry->successors[0]);
    REQUIRE(cancel_branch->operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH);
    uint32_t cancel_body = cancel_branch->successors[0];
    uint32_t cancel_exit = cancel_branch->successors[1];
    XrValidatedInstruction *loop = cleanup_graph_terminal(function, cancel_body);
    REQUIRE(loop->successors[0] == cancel_body && loop->successors[1] == cancel_exit);
    XrValidatedInstruction *provider = NULL;
    for (uint32_t i = 0u; i < function->blocks[cancel_body].instruction_count; ++i) {
        XrValidatedInstruction *candidate = &function->blocks[cancel_body].instructions[i];
        if (candidate->operation_id == XR_CORE_OP_CORE_PROVIDER_CALL) {
            REQUIRE(provider == NULL);
            provider = candidate;
        }
    }
    REQUIRE(provider && provider->successor_count == 1u);
    uint32_t trap_adapter = provider->successors[0];
    XrValidatedInstruction *trap_entry = cleanup_graph_terminal(function, trap_adapter);
    REQUIRE(trap_entry->operation_id == XR_CORE_OP_CORE_BRANCH);
    XrValidatedInstruction *trap_branch =
        cleanup_graph_terminal(function, trap_entry->successors[0]);
    REQUIRE(trap_branch->operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH);
    bool saved_verified = ir->verified;
    ir->verified = false;
    require_coroutine_backend_restored(ir);
    ir->verified = saved_verified;

    test_cleanup_graph_exit_mutations(ir, cancel_exit, true);
    test_cleanup_graph_exit_mutations(ir, trap_branch->successors[0], false);
    test_cleanup_graph_exit_mutations(ir, trap_branch->successors[1], false);
    test_cleanup_graph_suspension_rejected(ir, cancel_body, cancel_exit);
    test_cleanup_graph_suspension_rejected(ir, trap_entry->successors[0], cancel_exit);

    uint32_t saved = trap_entry->successors[0];
    trap_entry->successors[0] = cancel_adapter;
    require_backend_operation_rejected(ir, "trap branches back to cancellation",
                                       XR_CORE_OP_CORE_BRANCH);
    trap_entry->successors[0] = saved;
    require_coroutine_backend_restored(ir);
    saved = provider->successors[0];
    provider->successors[0] = cancel_adapter;
    require_backend_operation_rejected(ir, "cancel refusal cannot remain cancellation",
                                       XR_CORE_OP_CORE_PROVIDER_CALL);
    provider->successors[0] = saved;
    require_coroutine_backend_restored(ir);

    XrValidatedBlock *normal = &function->blocks[normal_id];
    uint32_t saved_count = normal->instruction_count;
    REQUIRE(saved_count >= 2u);
    XrValidatedInstruction *drop = &normal->instructions[saved_count - 2u];
    XrValidatedInstruction saved_drop = *drop;
    REQUIRE(drop->operation_id == XR_CORE_OP_CORE_OWNER_DROP);
    drop->operation_id = XR_CORE_OP_CORE_BRANCH;
    drop->successor_count = 1u;
    drop->successors = &cancel_adapter;
    normal->instruction_count--;
    require_backend_operation_rejected(ir, "ordinary resume enters cancel through adapter",
                                       XR_CORE_OP_CORE_BRANCH);
    normal->instruction_count = saved_count;
    *drop = saved_drop;
    require_coroutine_backend_restored(ir);

    saved = trap_branch->successors[1];
    trap_branch->successors[1] = trap_branch->successors[0];
    require_backend_operation_rejected(ir, "orphan explicit trap alternative", 0u);
    trap_branch->successors[1] = saved;
    require_coroutine_backend_restored(ir);
    test_cleanup_graph_storage_mutations(ir, cancel_adapter);
    test_cleanup_graph_owner_edges(ir, cancel_adapter);
    xr_backend_ir_free(ir);
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
}

static void test_coroutine_backend_trap_payload_mutations(XrBackendIR *ir,
                                                          XrValidatedFunction *function,
                                                          XrValidatedInstruction *call) {
    const XrValidatedFunction *callee =
        &ir->program->functions[call->immediate.coroutine_call.function_id];
    const XrValidatedCoroutineSafepoint *point =
        &function->coroutine_safepoints[call->immediate.coroutine_call.safepoint_id];
    XrValidatedBlock *cancel = &function->blocks[call->successors[1]];
    XrValidatedBlock *trap = &function->blocks[call->successors[2]];
    uint32_t cancel_start = callee->parameter_count + point->live_value_count;
    uint32_t trap_start = cancel_start + cancel->argument_count;
    REQUIRE(call->operand_count == 7u && cancel_start == 4u && trap_start == 5u);
    REQUIRE(cancel->argument_count == 1u && trap->argument_count == 2u);
    XrValidatedInstruction saved_call = *call;
    uint32_t operands[8];
    memcpy(operands, call->operands, call->operand_count * sizeof(*operands));
    uint32_t owner = operands[trap_start];
    uint32_t snapshot = operands[trap_start + 1u];
    REQUIRE(function->value_ownerships[owner] == XR_CORE_IR_OWNER);
    REQUIRE(function->value_types[snapshot] == XR_CORE_TYPE_I64);
    REQUIRE(function->value_types[operands[0]] == XR_CORE_TYPE_I64);
    REQUIRE(operands[0] != owner && operands[0] != snapshot);

    call->operands = operands;
    operands[trap_start + 1u] = operands[0];
    require_coroutine_backend_rejected(ir, "non-live trap input");
    operands[trap_start + 1u] = snapshot;
    require_coroutine_backend_restored(ir);

    operands[trap_start + 1u] = owner;
    require_coroutine_backend_rejected(ir, "duplicate trap owner");
    operands[trap_start + 1u] = snapshot;
    require_coroutine_backend_restored(ir);

    XrValidatedBlock saved_trap = *trap;
    trap->argument_count = 1u;
    trap->argument_ids = saved_trap.argument_ids + 1u;
    trap->argument_types = saved_trap.argument_types + 1u;
    trap->argument_categories = saved_trap.argument_categories + 1u;
    trap->argument_ownerships = saved_trap.argument_ownerships + 1u;
    operands[trap_start] = snapshot;
    call->operand_count = 6u;
    require_coroutine_backend_rejected(ir, "missing trap owner");
    *trap = saved_trap;
    operands[trap_start] = owner;
    call->operand_count = saved_call.operand_count;
    require_coroutine_backend_restored(ir);

    call->operand_count = 6u;
    require_coroutine_backend_rejected(ir, "truncated trap payload");
    call->operand_count = saved_call.operand_count;
    require_coroutine_backend_restored(ir);

    operands[7] = snapshot;
    call->operand_count = 8u;
    require_coroutine_backend_rejected(ir, "extra trap payload");
    call->operand_count = saved_call.operand_count;
    require_coroutine_backend_restored(ir);

    XrValidatedBlock saved_cancel = *cancel;
    uint32_t cancel_ids[] = {cancel->argument_ids[0], cancel->argument_ids[0]};
    uint16_t cancel_types[] = {cancel->argument_types[0], cancel->argument_types[0]};
    XrCoreIrValueCategory cancel_categories[] = {XR_CORE_IR_VALUE, XR_CORE_IR_VALUE};
    XrCoreIrOwnershipDisposition cancel_ownerships[] = {XR_CORE_IR_OWNER, XR_CORE_IR_OWNER};
    cancel->argument_count = 2u;
    cancel->argument_ids = cancel_ids;
    cancel->argument_types = cancel_types;
    cancel->argument_categories = cancel_categories;
    cancel->argument_ownerships = cancel_ownerships;
    operands[cancel_start + 1u] = owner;
    operands[cancel_start + 2u] = owner;
    operands[cancel_start + 3u] = snapshot;
    call->operand_count = 8u;
    require_coroutine_backend_rejected(ir, "duplicate cancel owner");
    *cancel = saved_cancel;
    *call = saved_call;
    require_coroutine_backend_restored(ir);
}

static void test_coroutine_backend_trap_target_mutations(XrBackendIR *ir,
                                                         XrValidatedFunction *function,
                                                         XrValidatedInstruction *call) {
    XrValidatedBlock *trap = &function->blocks[call->successors[2]];
    REQUIRE(trap->argument_count == 2u && trap->instruction_count != 0u);
    uint32_t snapshot = trap->argument_ids[1];
    uint16_t saved_type = function->value_types[snapshot];
    trap->argument_types[1] = XR_CORE_TYPE_BOOL;
    function->value_types[snapshot] = XR_CORE_TYPE_BOOL;
    require_coroutine_backend_rejected(ir, "trap argument type");
    trap->argument_types[1] = saved_type;
    function->value_types[snapshot] = saved_type;
    require_coroutine_backend_restored(ir);

    XrCoreIrValueCategory saved_category = function->value_categories[snapshot];
    trap->argument_categories[1] = XR_CORE_IR_PLACE;
    function->value_categories[snapshot] = XR_CORE_IR_PLACE;
    require_coroutine_backend_rejected(ir, "trap argument category");
    trap->argument_categories[1] = saved_category;
    function->value_categories[snapshot] = saved_category;
    require_coroutine_backend_restored(ir);

    uint32_t owner = trap->argument_ids[0];
    XrCoreIrOwnershipDisposition saved_ownership = function->value_ownerships[owner];
    trap->argument_ownerships[0] = XR_CORE_IR_NON_OWNER;
    function->value_ownerships[owner] = XR_CORE_IR_NON_OWNER;
    require_coroutine_backend_rejected(ir, "trap argument ownership");
    trap->argument_ownerships[0] = saved_ownership;
    function->value_ownerships[owner] = saved_ownership;
    require_coroutine_backend_restored(ir);

    XrValidatedInstruction *terminal = &trap->instructions[trap->instruction_count - 1u];
    REQUIRE(terminal->operation_id == XR_CORE_OP_CORE_TRAP && terminal->immediate.u32 == 7u);
    terminal->immediate.u32 = 4u;
    require_backend_operation_rejected(ir, "non-provider trap terminal", XR_CORE_OP_CORE_TRAP);
    terminal->immediate.u32 = 7u;
    require_coroutine_backend_restored(ir);

    uint32_t saved_successor = call->successors[2];
    call->successors[2] = call->successors[1];
    require_coroutine_backend_rejected(ir, "trap targets cancellation");
    call->successors[2] = saved_successor;
    require_coroutine_backend_restored(ir);

    call->successor_count = 2u;
    require_coroutine_backend_rejected(ir, "trap payload without edge");
    call->successor_count = 3u;
    require_coroutine_backend_restored(ir);
}

static const char *require_coroutine_c_fragment(const char *text, const char *fragment) {
    const char *found = strstr(text, fragment);
    if (!found)
        fprintf(stderr, "missing generated coroutine C fragment: %s\n", fragment);
    REQUIRE(found != NULL);
    return found;
}

static void require_coroutine_trap_c_edges(const XrGeneratedC *generated,
                                           const XrValidatedInstruction *call, uint32_t function_id,
                                           uint32_t instruction_id) {
    char fragment[128];
    int count =
        snprintf(fragment, sizeof(fragment), "if (child_%u.kind == 1 && child_%u.trap == 7)",
                 instruction_id, instruction_id);
    REQUIRE(count > 0 && (size_t) count < sizeof(fragment));
    (void) require_coroutine_c_fragment(generated->bytes, fragment);
    uint32_t safepoint = call->immediate.coroutine_call.safepoint_id;
    count =
        snprintf(fragment, sizeof(fragment),
                 "if (cancel_%u.kind == UINT32_C(1) && cancel_%u.trap == 7)", safepoint, safepoint);
    REQUIRE(count > 0 && (size_t) count < sizeof(fragment));
    (void) require_coroutine_c_fragment(generated->bytes, fragment);
    for (uint32_t edge = 0u; edge < 3u; ++edge) {
        count = snprintf(fragment, sizeof(fragment), "goto xr_f%u_b%u;", function_id,
                         call->successors[edge]);
        REQUIRE(count > 0 && (size_t) count < sizeof(fragment));
        const char *first = require_coroutine_c_fragment(generated->bytes, fragment);
        if (edge == 2u) {
            const char *second = require_coroutine_c_fragment(first + (size_t) count, fragment);
            const char *extra = strstr(second + (size_t) count, fragment);
            if (extra)
                fprintf(stderr, "expected exactly two generated coroutine C fragments: %s\n",
                        fragment);
            REQUIRE(extra == NULL);
        }
    }
    REQUIRE(strstr(generated->bytes, "provider_call_typed") != NULL);
    REQUIRE(strstr(generated->bytes, "XR_AOT_FAIL(xr_aot_make(1, 0, 7))") != NULL);
    REQUIRE(strstr(generated->bytes, "XrVm") == NULL);
    REQUIRE(strstr(generated->bytes, "TargetPlan") == NULL);
}

static void test_coroutine_trap_continuation_lowering_and_mutation(void) {
    XrTargetProfile *profile = xr_test_target_profile_build_with_scalar_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED, XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    REQUIRE(profile != NULL);
    const XrTargetProviderContract *contract = NULL;
    for (size_t index = 0u; index < xr_target_profile_provider_count(profile); ++index) {
        const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, index);
        if (xr_test_target_profile_is_scalar_provider(candidate)) {
            REQUIRE(contract == NULL);
            contract = candidate;
        }
    }
    REQUIRE(contract != NULL && contract->operation_count == 1u);
    XrProgramArtifact artifact = {0};
    char build_diagnostic[256] = {0};
    REQUIRE(xr_program_coroutine_trap_fixture_write_with_ids(
                contract->contract_id, contract->operations[0].stable_id,
                XR_PROGRAM_COROUTINE_TRAP_REVERSED_LIVE_TUPLE, &artifact, build_diagnostic,
                sizeof(build_diagnostic)) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrValidatedFunction *function = &ir->program->functions[ir->program->entry_function];
    XrValidatedInstruction *call = NULL;
    uint32_t call_index = 0u;
    uint32_t instruction_base = 0u;
    for (uint32_t block = 0u; block < function->block_count; ++block) {
        XrValidatedBlock *row = &function->blocks[block];
        for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
            if (row->instructions[instruction].operation_id ==
                XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
                REQUIRE(call == NULL);
                call = &row->instructions[instruction];
                call_index = instruction_base + instruction;
            }
        }
        instruction_base += row->instruction_count;
    }
    REQUIRE(call != NULL && call->successor_count == 3u);
    REQUIRE(call->successors[0] != call->successors[1] &&
            call->successors[0] != call->successors[2] &&
            call->successors[1] != call->successors[2]);
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
    require_coroutine_trap_c_edges(&generated, call, ir->program->entry_function, call_index);
    XrBackendIR *rebuilt = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    REQUIRE(xr_backend_ir_emit_c(rebuilt, false, &repeated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(generated.size != 0u && generated.size == repeated.size);
    REQUIRE(memcmp(generated.bytes, repeated.bytes, generated.size) == 0);
    REQUIRE(xr_fingerprint_equal(generated.source_digest, repeated.source_digest));
    test_coroutine_backend_trap_payload_mutations(ir, function, call);
    test_coroutine_backend_trap_target_mutations(ir, function, call);
    xr_backend_ir_free(rebuilt);
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
}

static void test_witness_invoke_trap_continuation_lowering_and_mutation(void) {
    XrProgramArtifact artifact = {0};
    char build_diagnostic[256] = {0};
    REQUIRE(xr_program_existential_fixture_write_mutated(
                XR_EXISTENTIAL_FIXTURE_WITNESS_INVOKE_TRAP, &artifact, build_diagnostic,
                sizeof(build_diagnostic)) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrValidatedFunction *invoke_function = NULL;
    XrValidatedInstruction *invoke = NULL;
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        for (uint32_t block = 0u; block < ir->program->functions[function].block_count; ++block) {
            XrValidatedBlock *row = &ir->program->functions[function].blocks[block];
            for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
                if (row->instructions[instruction].operation_id ==
                    XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) {
                    REQUIRE(invoke == NULL);
                    invoke_function = &ir->program->functions[function];
                    invoke = &row->instructions[instruction];
                }
            }
        }
    }
    REQUIRE(invoke_function != NULL);
    REQUIRE(invoke != NULL);
    REQUIRE(invoke->successor_count == 3u);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, ".kind == 1 && call_") != NULL);
    REQUIRE(strstr(generated.bytes, ".trap == 7") != NULL);

    uint32_t trap_block = invoke->successors[2];
    REQUIRE(trap_block < invoke_function->block_count);
    XrValidatedBlock *target = &invoke_function->blocks[trap_block];
    REQUIRE(target->instruction_count != 0u);
    XrValidatedInstruction *trap = &target->instructions[target->instruction_count - 1u];
    REQUIRE(trap->operation_id == XR_CORE_OP_CORE_TRAP);
    trap->immediate.u32 = 4u;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    trap->immediate.u32 = 7u;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));

    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_callable_invoke_trap_continuation_lowering_and_mutation(void) {
    XrProgramArtifact artifact = {0};
    char build_diagnostic[256] = {0};
    REQUIRE(xr_program_callable_fixture_write_mutated(XR_CALLABLE_FIXTURE_INVOKE_TRAP, &artifact,
                                                      build_diagnostic, sizeof(build_diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrValidatedFunction *invoke_function = NULL;
    XrValidatedInstruction *invoke = NULL;
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        for (uint32_t block = 0u; block < ir->program->functions[function].block_count; ++block) {
            XrValidatedBlock *row = &ir->program->functions[function].blocks[block];
            for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
                if (row->instructions[instruction].operation_id ==
                    XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) {
                    REQUIRE(invoke == NULL);
                    invoke_function = &ir->program->functions[function];
                    invoke = &row->instructions[instruction];
                }
            }
        }
    }
    REQUIRE(invoke_function != NULL);
    REQUIRE(invoke != NULL);
    REQUIRE(invoke->successor_count == 3u);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, ".kind == 1 && call_") != NULL);
    REQUIRE(strstr(generated.bytes, ".trap == 7") != NULL);

    uint32_t trap_block = invoke->successors[2];
    REQUIRE(trap_block < invoke_function->block_count);
    XrValidatedBlock *target = &invoke_function->blocks[trap_block];
    REQUIRE(target->instruction_count != 0u);
    XrValidatedInstruction *trap = &target->instructions[target->instruction_count - 1u];
    REQUIRE(trap->operation_id == XR_CORE_OP_CORE_TRAP);
    trap->immediate.u32 = 4u;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    trap->immediate.u32 = 7u;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));

    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_provider_output_lowering_and_mutation(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrValidatedProgram *program = build_output_program(INT64_MIN);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrValidatedInstruction *output = NULL;
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        for (uint32_t block = 0u; block < ir->program->functions[function].block_count; ++block) {
            XrValidatedBlock *row = &ir->program->functions[function].blocks[block];
            for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
                if (row->instructions[instruction].operation_id == XR_CORE_OP_CORE_OUTPUT_GROUP) {
                    REQUIRE(output == NULL);
                    output = &row->instructions[instruction];
                }
            }
        }
    }
    REQUIRE(output != NULL);
    REQUIRE(output->immediate.provider_operation.requirement_index == 0u);
    REQUIRE(output->immediate.provider_operation.operation_index == 0u);
    XrGeneratedC embedded = {0};
    XrGeneratedC standalone = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &embedded, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(embedded.bytes, "provider_output_write") != NULL);
    REQUIRE(strstr(embedded.bytes, "xr_text_display_i64") != NULL);
    REQUIRE(strstr(embedded.bytes, "xr_text_group_render") != NULL);
    REQUIRE(strstr(embedded.bytes, "fwrite") == NULL);
    REQUIRE(xr_backend_ir_emit_c(ir, true, &standalone, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(standalone.bytes, "xr_aot_host_output_write") != NULL);
    REQUIRE(strstr(standalone.bytes, "fwrite(bytes, 1, size, stdout)") != NULL);
    REQUIRE(strstr(standalone.bytes, "xr_ctx->provider_output_write = xr_aot_host_output_write") !=
            NULL);

    output->immediate.provider_operation.operation_index = 1u;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    output->immediate.provider_operation.operation_index = 0u;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));

    xr_generated_c_free(&standalone);
    xr_generated_c_free(&embedded);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);

    profile =
        xr_test_target_profile_build_with_output(true, XR_TARGET_RUNTIME_PROFILE_FREESTANDING);
    REQUIRE(profile != NULL);
    program = build_output_program(42);
    ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    embedded = (XrGeneratedC) {0};
    standalone = (XrGeneratedC) {0};
    REQUIRE(xr_backend_ir_emit_c(ir, false, &embedded, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(xr_backend_ir_emit_c(ir, true, &standalone, &diagnostic) ==
            XR_BACKEND_EMISSION_REJECTED);
    REQUIRE(standalone.bytes == NULL && standalone.size == 0u);
    xr_generated_c_free(&embedded);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

/* String values lower to arena-owned handles, every text rule comes from the
 * embedded shared kernel, and the backend verifier rejects a text operation
 * whose operand types drift. */
static void test_array_values(void) {
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    for (unsigned scenario = 0u; scenario < 4u; ++scenario) {
        XrProgramArtifact artifact = {0};
        REQUIRE(xr_program_array_fixture_write(scenario, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
        XrGeneratedC generated = {0};
        XrBackendDiagnostic diagnostic;
        REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
        REQUIRE(strstr(generated.bytes, ".storage->length;") != NULL);
        XrValidatedInstruction *construct = &program->functions[0].blocks[0].instructions[scenario == 1u ? 0u : 2u];
        REQUIRE(construct->operation_id == XR_CORE_OP_CORE_ARRAY_CONSTRUCT);
        construct->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
        REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
        construct->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        REQUIRE(xr_backend_ir_verify(ir, &diagnostic));
        xr_generated_c_free(&generated);
        xr_backend_ir_free(ir);
        xr_validated_program_free(program);
    }
    xr_target_profile_free(profile);
}

static void test_sequence_length(void) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_sequence_length_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "->scalar_count;") != NULL);
    XrValidatedInstruction *length = &program->functions[0].blocks[0].instructions[4];
    REQUIRE(length->operation_id == XR_CORE_OP_CORE_SEQUENCE_LENGTH);
    length->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    length->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_text_lowering_and_mutation(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrValidatedProgram *program = build_text_program();
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrValidatedInstruction *concat = NULL;
    uint32_t concat_function = 0u;
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        for (uint32_t block = 0u; block < ir->program->functions[function].block_count; ++block) {
            XrValidatedBlock *row = &ir->program->functions[function].blocks[block];
            for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
                if (row->instructions[instruction].operation_id == XR_CORE_OP_CORE_STRING_CONCAT) {
                    REQUIRE(concat == NULL);
                    concat = &row->instructions[instruction];
                    concat_function = function;
                }
            }
        }
    }
    REQUIRE(concat != NULL);
    uint8_t representation = 0;
    REQUIRE(xr_backend_representation_for_program_type(
        ir->program, ir->program->functions[concat_function].value_types[concat->result_id],
        &representation));
    REQUIRE(representation == XR_BACKEND_VALUE_STRING_HANDLE);
    XrGeneratedC embedded = {0};
    XrGeneratedC standalone = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &embedded, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(embedded.bytes, "XR_TEXT_KERNEL_H") != NULL);
    REQUIRE(strstr(embedded.bytes, "xr_aot_string_concat") != NULL);
    REQUIRE(strstr(embedded.bytes, "xr_aot_string_from_scalar") != NULL);
    REQUIRE(strstr(embedded.bytes, "xr_aot_string_from_i64") == NULL);
    REQUIRE(strstr(embedded.bytes, "xr_text_compare") != NULL);
    REQUIRE(strstr(embedded.bytes, "xr_text_group_render") != NULL);
    REQUIRE(strstr(embedded.bytes, "xr_aot_free(xr_ctx, v") != NULL);
    REQUIRE(strstr(embedded.bytes, "fwrite") == NULL);
    REQUIRE(xr_backend_ir_emit_c(ir, true, &standalone, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(standalone.bytes, "xr_aot_host_output_write") != NULL);
    REQUIRE(strstr(standalone.bytes, "xr_aot_context_destroy(xr_ctx)") != NULL);

    /* A concat whose right operand is not a string is rejected before any C
     * is emitted. */
    uint32_t original = concat->operands[1];
    for (uint32_t value = 0u; value < ir->program->functions[concat_function].value_count;
         ++value) {
        if (ir->program->functions[concat_function].value_types[value] == XR_CORE_TYPE_I64) {
            concat->operands[1] = value;
            break;
        }
    }
    REQUIRE(concat->operands[1] != original);
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(diagnostic.status == XR_BACKEND_INVARIANT_REJECTED);
    concat->operands[1] = original;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));

    xr_generated_c_free(&standalone);
    xr_generated_c_free(&embedded);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_existential_owned_read_reborrow_lowering(void) {
    XrValidatedProgram *program = build_existential_reborrow_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, ".concrete_type_id = v") != NULL);
    REQUIRE(strstr(generated.bytes, ".conformance_id = v") != NULL);
    REQUIRE(strstr(generated.bytes, ".data = v") != NULL);
    REQUIRE(strstr(generated.bytes, "selector") == NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static XrTargetProfile *native_provider_profile(void) {
    XrTargetProfile *profile = NULL;
    char error[256] = {0};
    REQUIRE(xr_runtime_target_profile_build_native_hosted(&profile, error, sizeof(error)));
    return profile;
}

static void test_pipe_provider_lowering(void) {
    XrTargetProfile *profile = native_provider_profile();
    REQUIRE(profile != NULL);
    XrValidatedProgram *program = build_pipe_program();
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC embedded = {0};
    XrGeneratedC standalone = {0};
    XrBackendDiagnostic diagnostic;
    REQUIRE(xr_backend_ir_emit_c(ir, false, &embedded, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(embedded.bytes, "provider_call_typed") != NULL);
    REQUIRE(strstr(embedded.bytes, "xr_aot_native_provider_") == NULL);
    REQUIRE(xr_backend_ir_emit_c(ir, true, &standalone, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(strstr(standalone.bytes, "xr_pipe_create(&pipe, NULL)") != NULL);
    REQUIRE(strstr(standalone.bytes, "CreatePipe(") == NULL);
    REQUIRE(
        strstr(standalone.bytes,
               "provider_call_typed = xr_aot_host_typed") !=
        NULL);
    xr_generated_c_free(&standalone);
    xr_generated_c_free(&embedded);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

#include "test_xr_program_aot_class.inc.c"

static void test_class_reference_semantics_lowering_and_events(void) {
    XrValidatedProgram *program = build_class_reference_program();
    require_class_reference_oracle(program);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    require_native_record_c_literal_escaping();
    require_class_field_finalization_lowering(profile);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    XrBackendStatus status = xr_backend_ir_build(program, profile, &options, &ir, &diagnostic);
    REQUIRE(status == XR_BACKEND_OK);
    REQUIRE(ir != NULL && xr_backend_ir_verify(ir, &diagnostic));
    REQUIRE(ir->instruction_count == 10u);
    XrGeneratedC first = {0};
    XrGeneratedC second = {0};
    REQUIRE(xr_backend_ir_emit_c(ir, false, &first, &diagnostic) == XR_BACKEND_OK);
    REQUIRE(xr_backend_ir_emit_c(ir, false, &second, &diagnostic) == XR_BACKEND_OK);
    (void) "strict-native-host-run";
    (void) "h2-class-differential";
    REQUIRE(first.size == second.size && memcmp(first.bytes, second.bytes, first.size) == 0);
    REQUIRE(xr_fingerprint_equal(first.source_digest, second.source_digest));
    uint16_t class_type_id = program->types[0].type_id;
    char expected[96];
    (void) snprintf(expected, sizeof(expected), "typedef XrAotClass%u *XrAotType%u;", class_type_id,
                    class_type_id);
    REQUIRE(strstr(first.bytes, expected) != NULL);
    (void) snprintf(expected, sizeof(expected), "struct XrAotClass%u {", class_type_id);
    REQUIRE(strstr(first.bytes, expected) != NULL);
    (void) snprintf(expected, sizeof(expected), "struct XrAotType%u {", class_type_id);
    REQUIRE(strstr(first.bytes, expected) == NULL);
    (void) snprintf(expected, sizeof(expected), "xr_aot_class_drop_%u", class_type_id);
    REQUIRE(strstr(first.bytes, expected) != NULL);
    REQUIRE(strstr(first.bytes, ".type_id = UINT16_C(2)") != NULL);
    REQUIRE(strstr(first.bytes, ".has_i64_exchange = UINT8_C(1)") != NULL);
    REQUIRE(strstr(first.bytes, "XrVm") == NULL);
    xr_generated_c_free(&second);
    xr_generated_c_free(&first);
    xr_backend_ir_free(ir);
    XrValidatedProgram *copy_program = build_class_reference_copy_program();
    ir = NULL;
    status = xr_backend_ir_build(copy_program, profile, &options, &ir, &diagnostic);
    REQUIRE(status == XR_BACKEND_UNSUPPORTED_OPERATION && ir == NULL);
    REQUIRE(diagnostic.operation_id == XR_CORE_OP_CORE_OWNER_COPY);
    xr_validated_program_free(copy_program);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_generated_fixture(const char *path, const XrValidatedProgram *program,
                                    const XrTargetProfile *profile, bool standalone_main) {
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
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

static void write_array_alias_fixture(const char *path, const XrValidatedProgram *program,
                                     const XrTargetProfile *profile) {
    write_generated_fixture(path, program, profile, false);
    uint32_t entry = program->entry_function;
    uint16_t outer = program->functions[entry].result_type_id;
    const XrValidatedType *type = xr_validated_program_type(program, outer);
    REQUIRE(type && type->kind == XR_CORE_IR_TYPE_ARRAY);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    REQUIRE(fprintf(output,
        "\nint main(void) {\n"
        "  XrAotContext context = {0}; XrAotType%u value = {0};\n"
        "  XrAotOutcome result = xr_aot_fn_%u(&context, &value);\n"
        "  if (result.kind || !value.storage || value.storage->length != 3u) return 1;\n"
        "  if (value.storage->data[0].storage != value.storage->data[1].storage ||\n"
        "      value.storage->data[0].storage == value.storage->data[2].storage) return 2;\n"
        "  value.storage->data[0].storage->data[0] = INT64_C(99);\n"
        "  if (value.storage->data[1].storage->data[0] != INT64_C(99) ||\n"
        "      value.storage->data[2].storage->data[0] != INT64_C(42)) return 3;\n"
        "  xr_aot_drop_%u(&context, value.storage->data[0], UINT32_MAX);\n"
        "  value.storage->data[0].storage = NULL;\n"
        "  if (value.storage->data[1].storage->data[0] != INT64_C(99)) return 4;\n"
        "  xr_aot_drop_%u(&context, value, UINT32_MAX);\n"
        "  if (context.allocations) return 5;\n"
        "  xr_aot_context_destroy(&context); return 0;\n}\n",
        outer, entry, type->array_element_type, outer) > 0);
    REQUIRE(fclose(output) == 0);
}

static void write_allocation_probe(FILE *output) {
    REQUIRE(fputs("#include <stdlib.h>\n#include <stdio.h>\n"
                  "static void *records[128];\n"
                  "static size_t attempts, fail_at, live;\nstatic int bad_free;\n"
                  "static void *observed_malloc(size_t size) {\n"
                  "  if (++attempts == fail_at) return NULL;\n"
                  "  for (size_t i = 0; i < 128u; ++i) if (!records[i]) {\n"
                  "    records[i] = malloc(size); if (records[i]) ++live; return records[i];\n"
                  "  }\n  return NULL;\n}\n"
                  "static void observed_free(void *p) {\n"
                  "  if (!p) return;\n"
                  "  for (size_t i = 0; i < 128u; ++i) if (records[i] == p) {\n"
                  "    records[i] = NULL; --live; free(p); return;\n"
                  "  }\n  bad_free = 1;\n}\n"
                  "#define malloc observed_malloc\n#define free observed_free\n", output) >= 0);
}

static void write_owned_allocation_fixture(const char *path, const XrValidatedProgram *program,
                                           const XrTargetProfile *profile, unsigned scenario) {
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, NULL) == XR_BACKEND_OK);
    FILE *output = fopen(path, "wb");
    REQUIRE(output != NULL);
    write_allocation_probe(output);
    REQUIRE(fwrite(generated.bytes, 1u, generated.size, output) == generated.size);
    static const unsigned expected[] = {2u, 0u, 2u, 1u, 0u, 42u, 1u};
    char call[1024];
    if (scenario == 4u) {
        uint16_t result_type = program->functions[program->entry_function].result_type_id;
        int written = snprintf(call, sizeof(call),
            "    XrAotType%u value = {0};\n"
            "    XrAotOutcome result = xr_aot_fn_%u(&context, &value);\n"
            "    if (result.kind && value.storage) return 6;\n"
            "    if (!result.kind) {\n"
            "      if (!value.storage || value.storage->length != 3u ||\n"
            "          value.storage->data[0].storage != value.storage->data[1].storage ||\n"
            "          value.storage->data[0].storage == value.storage->data[2].storage) return 7;\n"
            "      xr_aot_drop_%u(&context, value, UINT32_MAX); result.i64 = 0;\n"
            "    }\n", result_type, program->entry_function, result_type);
        REQUIRE(written > 0 && (size_t)written < sizeof(call));
    } else {
        int written = snprintf(call, sizeof(call), "    XrAotOutcome result = xr_aot_fn_%u(&context);\n",
                               program->entry_function);
        REQUIRE(written > 0 && (size_t)written < sizeof(call));
    }
    REQUIRE(fprintf(output,
                    "\n#undef malloc\n#undef free\nint main(void) {\n"
                    "  for (fail_at = 1u; fail_at < 128u; ++fail_at) {\n"
                    "    attempts = 0u; XrAotContext context = {0};\n"
                    "%s"
                    "    if (context.allocations || live || bad_free) {\n"
                    "      fprintf(stderr, \"owned cleanup failed at %%zu\\n\", fail_at); return 1;\n"
                    "    }\n"
                    "    xr_aot_context_destroy(&context);\n"
                    "    if (live || bad_free) return 2;\n"
                    "    if (attempts < fail_at) return result.kind == 0 && result.%s == %u ? 0 : 3;\n"
                    "    if (result.kind != 4) return 4;\n"
                    "  } return 5;\n}\n", call, scenario == 6u ? "boolean" : "i64", expected[scenario]) > 0);
    REQUIRE(fclose(output) == 0);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
}

static void write_provider_generated_fixture(const char *path, const XrValidatedProgram *program,
                                             const XrTargetProfile *profile,
                                             uint32_t entry_function) {
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    REQUIRE(fprintf(output,
                    "\nstatic void xr_test_dispose(XrProviderValuePack *result) { *result = (XrProviderValuePack){0}; }\n"
                    "static int xr_test_provider(void *context, uint32_t requirement, "
                    "uint32_t operation, const XrProviderValuePack *arguments, XrProviderValuePack *result) {\n"
                    "    if (!context || requirement != UINT32_C(0) || operation != "
                    "UINT32_C(0) || !result) return 1;\n"
                    "    if (!arguments || arguments->count != 1 || arguments->nodes[0].token != 3) return 1;\n"
                    "    result->count = 1; result->nodes[0].token = 3;\n"
                    "    result->nodes[0].as.i64 = arguments->nodes[0].as.i64 + INT64_C(1);\n"
                    "    return 0;\n"
                    "}\n"
                    "int main(void) {\n"
                    "    uint8_t provider_context = UINT8_C(1);\n"
                    "    XrAotContext context = {.provider_context = &provider_context, "
                    ".provider_call_typed = xr_test_provider, .provider_dispose_typed = xr_test_dispose};\n"
                    "    XrAotOutcome outcome = xr_aot_fn_%u(&context);\n"
                    "    return outcome.kind == UINT32_C(0) ? "
                    "(int)((uint64_t)outcome.i64 & UINT64_C(255)) : 255;\n"
                    "}\n",
                    entry_function) > 0);
    REQUIRE(fclose(output) == 0);
}

static void write_condition_assert_generated_fixture(const char *path,
                                                     const XrValidatedProgram *program,
                                                     const XrTargetProfile *profile,
                                                     uint32_t entry_function) {
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    REQUIRE(fprintf(output,
                    "\nint main(void) {\n"
                    "    XrAotContext context = {0};\n"
                    "    XrAotPanicInfo panic = {0};\n"
                    "    XrAotOutcome normal = xr_aot_fn_%u(&context, UINT8_C(1), "
                    "INT64_C(9), &panic);\n"
                    "    if (normal.kind != UINT32_C(0) || normal.value_kind != UINT32_C(2) || "
                    "normal.i64 != INT64_C(42) || panic.code != UINT32_C(0)) return 1;\n"
                    "    XrAotOutcome failed = xr_aot_fn_%u(&context, UINT8_C(0), "
                    "INT64_C(9), &panic);\n"
                    "    return failed.kind == UINT32_C(3) && panic.code == UINT32_C(1) ? 0 : 2;\n"
                    "}\n",
                    entry_function, entry_function) > 0);
    REQUIRE(fclose(output) == 0);
}

static void seal_native_file(const char *path, const XrValidatedProgram *program,
                             const XrTargetProfile *profile) {
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

    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
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

#include "../program/xr_program_module_fixture.h"

static void test_module_operations_have_typed_native_storage(void) {
    _Static_assert(XR_CORE_OP_CORE_PLACE_MODULE == 152, "module place stable id drifted");
    _Static_assert(XR_CORE_OP_CORE_PLACE_INITIALIZE == 153, "initialization stable id drifted");
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_module_initializer_fixture_write(false, &artifact, NULL, 0u) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrBackendIR *ir = NULL;
    XrBackendOptions options = xr_backend_default_options();
    REQUIRE(xr_backend_ir_build(program, profile, &options, &ir, NULL) == XR_BACKEND_OK);
    XrGeneratedC generated = {0};
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, NULL) == XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "struct XrAotModules") != NULL);
    REQUIRE(strstr(generated.bytes, "publication_order") != NULL);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

static void write_integer_value_fixture(const char *path) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_integer_fixture_write(false, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic diagnostic = {0};
    XrProgramVerifyStatus status =
        xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &diagnostic);
    if (status != XR_PROGRAM_VERIFY_OK)
        fprintf(stderr,
                "integer fixture rejected: %s diagnostic=%s function=%u block=%u value=%u\n",
                xr_program_verify_status_name(status),
                xr_program_diagnostic_kind_name(diagnostic.kind), diagnostic.location.function_id,
                diagnostic.location.block_id, diagnostic.location.value_id);
    REQUIRE(status == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    REQUIRE(fputs("\nint main(void) {\n"
                  "    XrAotContext context = {0};\n"
                  "    XrAotOutcome result;\n",
                  output) >= 0);
    static const struct {
        uint16_t type_id;
        unsigned value_kind;
        const char *field;
        const char *minimum;
        const char *maximum;
        unsigned bytes;
    } cases[] = {
        {XR_CORE_TYPE_I8, 10u, "i8", "(-128)", "127", 1u},
        {XR_CORE_TYPE_U8, 11u, "u8", "0", "255", 1u},
        {XR_CORE_TYPE_I16, 12u, "i16", "(-32768)", "32767", 2u},
        {XR_CORE_TYPE_U16, 6u, "u16", "0", "65535", 2u},
        {XR_CORE_TYPE_I32, 13u, "i32", "(-INT32_C(2147483647)-INT32_C(1))", "INT32_C(2147483647)",
         4u},
        {XR_CORE_TYPE_U32, 3u, "u32", "0", "UINT32_C(4294967295)", 4u},
        {XR_CORE_TYPE_I64, 2u, "i64", "(-INT64_C(9223372036854775807)-INT64_C(1))",
         "INT64_C(9223372036854775807)", 8u},
        {XR_CORE_TYPE_U64, 14u, "u64", "0", "UINT64_C(18446744073709551615)", 8u},
    };
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        uint32_t function = xr_program_integer_fixture_function(program, cases[index].type_id,
                                                                cases[index].type_id);
        REQUIRE(function != UINT32_MAX);
        const char *values[] = {cases[index].minimum, cases[index].maximum};
        for (size_t bound = 0u; bound < 2u; ++bound)
            REQUIRE(fprintf(output,
                            "    result = xr_aot_fn_%u(&context, %s);\n"
                            "    if (result.kind != 0 || result.value_kind != %u || "
                            "result.%s != %s || sizeof(result.%s) != %u) return %u;\n",
                            function, values[bound], cases[index].value_kind, cases[index].field,
                            values[bound], cases[index].field, cases[index].bytes,
                            (unsigned) (index * 2u + bound + 1u)) > 0);
    }
    REQUIRE(fputs("    return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_integer_conversion_fixture(const char *path) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_integer_fixture_write(true, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    for (uint32_t index = 0u; index < program->function_count; ++index)
        REQUIRE(program->functions[index].blocks[0].instructions[1].operation_id ==
                XR_CORE_OP_CORE_INTEGER_CONVERT);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    REQUIRE(fputs("\nint main(void) {\n"
                  "    XrAotContext context = {0};\n"
                  "    XrAotOutcome result;\n",
                  output) >= 0);
    unsigned case_id = 0u;
#define XR_INTEGER_CONVERSION_CASE(source, input_member, input, target, output_member, expected)   \
    do {                                                                                           \
        uint32_t function = xr_program_integer_fixture_function(program, XR_CORE_TYPE_##source,    \
                                                                XR_CORE_TYPE_##target);            \
        REQUIRE(function != UINT32_MAX);                                                           \
        REQUIRE(fprintf(output,                                                                    \
                        "    result = xr_aot_fn_%u(&context, %s);\n"                               \
                        "    if (result.kind != 0 || result.%s != %s) return %u;\n",               \
                        function, #input, #output_member, #expected, ++case_id) > 0);              \
    } while (0);
#include "../program/xr_program_integer_conversion_cases.inc.c"
#undef XR_INTEGER_CONVERSION_CASE
    REQUIRE(fputs("    return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_integer_divmod_cleanup_fixture(const char *path) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_panic_point_fixture_write(XR_CORE_OP_CORE_INTEGER_DIVMOD,
                                                 XR_ASSERT_FIXTURE_CHAINED_CLEANUP, &artifact, NULL,
                                                 0u) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    uint32_t entry = xr_validated_program_entry_function(program);
    REQUIRE(fprintf(output,
                    "\nint main(void) {\n"
                    "    XrAotContext context = {0};\n"
                    "    XrAotPanicInfo panic = {0};\n"
                    "    XrAotOutcome result = xr_aot_fn_%u(&context, 2, 84, &panic);\n"
                    "    if (result.kind != 0 || result.i64 != 42) return 1;\n"
                    "    result = xr_aot_fn_%u(&context, 0, 84, &panic);\n"
                    "    if (result.kind != 3 || panic.code != 420) return 2;\n"
                    "    return 0;\n}\n",
                    entry, entry) > 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_integer_divmod_fixture(const char *path) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_integer_divmod_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    for (uint32_t index = 0u; index < program->function_count; ++index)
        REQUIRE(program->functions[index].blocks[0].instructions[1].operation_id ==
                XR_CORE_OP_CORE_INTEGER_DIVMOD);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    REQUIRE(fputs("\nint main(void) {\n"
                  "    XrAotContext context = {0};\n"
                  "    XrAotOutcome result;\n"
                  "    XrAotPanicInfo panic = {0};\n",
                  output) >= 0);
    unsigned case_id = 0u;
#define XR_INTEGER_DIVMOD_CASE(type, member, left, right, quotient, residue)                       \
    do {                                                                                           \
        for (unsigned remainder = 0u; remainder < 2u; ++remainder) {                               \
            uint32_t function = xr_program_integer_divmod_fixture_function(                        \
                program, XR_CORE_TYPE_##type, remainder);                                          \
            REQUIRE(function != UINT32_MAX);                                                       \
            REQUIRE(fprintf(output,                                                                \
                            "    result = xr_aot_fn_%u(&context, %s, %s, &panic);\n"               \
                            "    if (result.kind != 0 || result.%s != %s) return %u;\n",           \
                            function, #left, #right, #member, remainder ? #residue : #quotient,    \
                            ++case_id) > 0);                                                       \
            REQUIRE(fprintf(output,                                                                \
                            "    panic = (XrAotPanicInfo){0};\n"                                                     \
                            "    result = xr_aot_fn_%u(&context, %s, 0, &panic);\n"                \
                            "    if (result.kind != 3 || panic.code != %u) return %u;\n",               \
                            function, #left, remainder ? 421u : 420u, ++case_id) > 0);             \
        }                                                                                          \
    } while (0);
#include "../program/xr_program_integer_divmod_cases.inc.c"
#undef XR_INTEGER_DIVMOD_CASE
    REQUIRE(fputs("    return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_string_slice_fixture(const char *path) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_string_slice_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    REQUIRE(xr_core_spec_operation_by_id(XR_CORE_OP_CORE_STRING_SLICE) != NULL);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    REQUIRE(fputs("\nint main(void) {\n"
        "    XrAotContext context = {0}; XrAotString *value = NULL; XrAotPanicInfo panic = {0};\n"
        "    const uint8_t expected[] = {0xc3,0xa9,0xe4,0xb8,0xad,0xf0,0x9f,0x99,0x82};\n"
        "    XrAotOutcome result = xr_aot_fn_0(&context, 1, 4, &panic); value = (XrAotString *)result.pointer;\n"
        "    if (result.kind != 0 || !value || value->size != sizeof(expected) ||\n"
        "        memcmp(value->bytes, expected, sizeof(expected)) != 0) return 1;\n"
        "    xr_aot_free(&context, value); value = NULL;\n"
        "    result = xr_aot_fn_0(&context, 4, 4, &panic); value = (XrAotString *)result.pointer;\n"
        "    if (result.kind != 0 || !value || value->size != 0) return 2;\n"
        "    xr_aot_free(&context, value); value = NULL;\n"
        "    result = xr_aot_fn_0(&context, -1, 4, &panic); value = (XrAotString *)result.pointer;\n"
        "    if (result.kind != 3 || panic.code != 430 || value != NULL) return 3;\n"
        "    xr_aot_context_destroy(&context);\n"
        "    return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_string_slice_allocation_fixture(const char *path) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_string_slice_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, NULL) == XR_BACKEND_OK);
    FILE *output = fopen(path, "wb");
    REQUIRE(output);
    write_allocation_probe(output);
    REQUIRE(fwrite(generated.bytes, 1u, generated.size, output) == generated.size);
    REQUIRE(fputs(
        "\n#undef malloc\n#undef free\nint main(void) {\n"
        "  const uint8_t expected[] = {0xc3,0xa9,0xe4,0xb8,0xad,0xf0,0x9f,0x99,0x82};\n"
        "  for (unsigned scenario = 0; scenario < 3; ++scenario) {\n"
        "    int complete = 0;\n"
        "    for (fail_at = 1; fail_at < 128; ++fail_at) {\n"
        "      attempts = 0; XrAotContext context = {0}; XrAotPanicInfo panic = {0};\n"
        "      XrAotOutcome result = xr_aot_fn_0(&context, scenario == 0 ? 1 : scenario == 1 ? 4 : -1, 4, &panic);\n"
        "      if (attempts >= fail_at) {\n"
        "        if (result.kind != 4 || result.pointer) return 1;\n"
        "      } else if (scenario == 2) {\n"
        "        if (result.kind != 3 || panic.code != 430 || result.pointer) return 2;\n"
        "      } else {\n"
        "        XrAotString *value = (XrAotString *)result.pointer;\n"
        "        if (result.kind || !value || value->size != (scenario == 0 ? sizeof(expected) : 0)) return 3;\n"
        "        if (value->size && memcmp(value->bytes, expected, sizeof(expected))) return 4;\n"
        "        xr_aot_free(&context, value);\n"
        "      }\n"
        "      if (context.allocations || live || bad_free) return 5;\n"
        "      xr_aot_context_destroy(&context);\n"
        "      if (attempts < fail_at) {\n"
        "        printf(\"native string slice allocation failures: scenario=%u points=%zu\\n\", scenario, fail_at - 1);\n"
        "        complete = 1; break;\n"
        "      }\n"
        "    }\n"
        "    if (!complete) return 6;\n"
        "  }\n"
        "  return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_array_append_fixture(const char *path) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_array_append_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    REQUIRE(xr_core_spec_operation_by_id(XR_CORE_OP_CORE_ARRAY_APPEND) != NULL);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output);
    REQUIRE(fputs("\nint main(void) {\n"
        " for (unsigned i = 0; i < 2; ++i) {\n"
        "  XrAotContext context = {0};\n"
        "  XrAotOutcome result = xr_aot_fn_0(&context, 42);\n"
        "  if (result.kind != 0u || result.i64 != 9) return 1;\n"
        "  if (context.allocations) return 2;\n"
        "  xr_aot_context_destroy(&context);\n"
        " }\n return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}
static void write_array_default_fixture(const char *path) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_array_default_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    REQUIRE(xr_core_spec_operation_by_id(XR_CORE_OP_CORE_ARRAY_ALLOCATE_DEFAULT) != NULL);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output);
    REQUIRE(fputs("\nint main(void) {\n"
        " XrAotContext context = {0}; XrAotPanicInfo panic = {0};\n"
        " const int64_t counts[] = {3, 0, -1, INT64_MIN, INT64_MAX, 4};\n"
        " for (unsigned i = 0; i < 6; ++i) {\n"
        "  XrAotOutcome result = xr_aot_fn_0(&context, counts[i], &panic);\n"
        "  if (result.kind != (counts[i] < 0 ? 3u : counts[i] == INT64_MAX ? 4u : 0u)) return 1;\n"
        "  if (counts[i] < 0 && panic.code != 452) return 2;\n"
        "  if (result.kind == 0 && result.i64 != counts[i]) return 3;\n"
        "  if (context.allocations) return 4;\n"
        " }\n xr_aot_context_destroy(&context); return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}
static void write_array_append_allocation_fixture(const char *path, bool managed) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_array_append_fixture_write(managed ? 100u : 0u, &artifact) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    REQUIRE(xr_core_spec_operation_by_id(XR_CORE_OP_CORE_ARRAY_APPEND) != NULL);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, NULL) == XR_BACKEND_OK);
    FILE *output = fopen(path, "wb");
    REQUIRE(output);
    write_allocation_probe(output);
    REQUIRE(fwrite(generated.bytes, 1u, generated.size, output) == generated.size);
    REQUIRE(fputs("\n#undef malloc\n#undef free\nint main(void) {\n"
        " for (fail_at = 1; fail_at < 128; ++fail_at) {\n"
        "  attempts = 0; XrAotContext context = {0};\n"
        "  XrAotOutcome result = xr_aot_fn_0(&context, 42);\n"
        "  if (attempts >= fail_at) { if (result.kind != 4 || result.pointer) return 1; }\n"
        "  else if (result.kind || result.i64 != 9) return 2;\n"
        "  if (context.allocations || live || bad_free) return 3;\n"
        "  xr_aot_context_destroy(&context);\n"
        "  if (attempts < fail_at) {\n"
        "   printf(\"native array append allocation failure points=%zu\\n\", fail_at - 1);\n"
        "   return 0;\n  }\n }\n return 4;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_array_default_allocation_fixture(const char *path) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_array_default_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    REQUIRE(xr_core_spec_operation_by_id(XR_CORE_OP_CORE_ARRAY_ALLOCATE_DEFAULT) != NULL);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, NULL) == XR_BACKEND_OK);
    FILE *output = fopen(path, "wb");
    REQUIRE(output);
    write_allocation_probe(output);
    REQUIRE(fwrite(generated.bytes, 1u, generated.size, output) == generated.size);
    REQUIRE(fputs("\n#undef malloc\n#undef free\nint main(void) {\n"
        " for (unsigned scenario = 0; scenario < 3; ++scenario) {\n"
        "  int complete = 0; int64_t count = scenario == 0 ? 3 : scenario == 1 ? 0 : -1;\n"
        "  for (fail_at = 1; fail_at < 128; ++fail_at) {\n"
        "   attempts = 0; XrAotContext context = {0}; XrAotPanicInfo panic = {0};\n"
        "   XrAotOutcome result = xr_aot_fn_0(&context, count, &panic);\n"
        "   if (attempts >= fail_at) { if (result.kind != 4 || result.pointer) return 1; }\n"
        "   else if (count < 0) { if (result.kind != 3 || panic.code != 452) return 2; }\n"
        "   else if (result.kind || result.i64 != count) return 3;\n"
        "   if (context.allocations || live || bad_free) return 4;\n"
        "   xr_aot_context_destroy(&context);\n"
        "   if (attempts < fail_at) {\n"
        "    printf(\"native default array allocation failures: scenario=%u points=%zu\\n\", scenario, fail_at - 1);\n"
        "    complete = 1; break;\n"
        "   }\n"
        "  }\n if (!complete) return 5;\n }\n return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_integer_bitwise_fixture(const char *path) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_integer_bitwise_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    for (uint32_t index = 0u; index < program->function_count; ++index)
        REQUIRE(program->functions[index].blocks[0].instructions[1].operation_id ==
                XR_CORE_OP_CORE_INTEGER_BITWISE);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    REQUIRE(fputs("\nint main(void) {\n"
                  "    XrAotContext context = {0};\n"
                  "    XrAotOutcome result;\n"
                  "",
                  output) >= 0);
    unsigned case_id = 0u;
#define XR_INTEGER_BITWISE_CASE(type, member, mode, left, right, expected) \
    do { \
        uint32_t function = xr_program_integer_bitwise_fixture_function(program, XR_CORE_TYPE_##type, mode); \
        REQUIRE(function != UINT32_MAX); \
        REQUIRE(fprintf(output, \
            "    result = xr_aot_fn_%u(&context, %s, %s);\n" \
            "    if (result.kind != 0 || result.%s != %s) return %u;\n", \
            function, #left, #right, #member, #expected, ++case_id) > 0); \
    } while (0);
#include "../program/xr_program_integer_bitwise_cases.inc.c"
#undef XR_INTEGER_BITWISE_CASE
    REQUIRE(fputs("    return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_coroutine_outcome_fixture(const char *path, uint32_t scenario) {
    REQUIRE(scenario < 8u);
    bool handled = scenario >= 4u;
    const char *outputs = handled ? "" : ", &error, &panic";
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_coroutine_outcome_fixture_write((scenario & 2u) != 0u, (scenario & 1u) != 0u,
                                                       handled ? XR_CORO_OUTCOME_HANDLED
                                                               : XR_CORO_OUTCOME_VALID,
                                                       &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    uint32_t entry = xr_validated_program_entry_function(program);
    REQUIRE(
        fprintf(
            output,
            "\nint main(void) {\n"
            "  for (uint32_t instance = 0; instance < 2; ++instance) {\n"
            "    XrAotContext context = {0};\n"
            "    for (uint32_t iteration = 0; iteration < 4; ++iteration) {\n"
            "      uint8_t cancel = (uint8_t)(iteration & 1u);\n"
            "      int64_t error = -1; XrAotPanicInfo panic = {0};\n"
            "      XrAotCoroutineFrame%u frame = {0};\n"
            "      XrAotOutcome result = xr_aot_fn_%u_step(&context, &frame, 0%s);\n"
            "      if (result.kind != 5 || frame.state != 1 || !context.allocations) return 1;\n"
            "      if (error != -1 || panic.code != 0) return 2;\n"
            "      result = xr_aot_fn_%u_step(&context, &frame, cancel%s);\n"
            "      if (result.kind != (cancel ? 6u : %uu) || context.allocations) return 3;\n"
            "      if (frame.state != UINT32_MAX) return 5;\n"
            "      if (cancel ? error != -1 || panic.code != 0\n"
            "                 : error != %d || panic.code != %u) return 4;\n"
            "    }\n"
            "    xr_aot_context_destroy(&context);\n"
            "  }\n"
            "  for (uint32_t iteration = 0; iteration < 4; ++iteration) {\n"
            "    XrAotEntryCoroutineFrame frame;\n"
            "    const XrBackendNativeDescriptor *descriptor = &xr_aot_entry_coroutine_descriptor;\n"
            "    descriptor->initialize(&frame, NULL, NULL);\n"
            "    XrBackendNativeOutcome paused = descriptor->step(&frame);\n"
            "    if (paused.kind != 1 || paused.error_value || paused.panic_present) return 6;\n"
            "    XrBackendNativeOutcome done = (iteration & 1u) ? descriptor->cancel(&frame)\n"
            "                                                        : descriptor->step(&frame);\n"
            "    if (done.kind != ((iteration & 1u) ? 3u : %uu)) return 7;\n"
            "    if (done.kind == 6u && (done.error_type_id != %uu || !done.error_value ||\n"
            "        *(const int64_t *)done.error_value != 42)) return 8;\n"
            "    if (done.panic_present != (done.kind == 7u ? 1u : 0u) ||\n"
            "        (done.panic_present && done.panic_code != 420u)) return 9;\n"
            "    descriptor->drop(&frame);\n"
            "    descriptor->drop(&frame);\n"
            "  }\n"
            "  return 0;\n}\n",
            entry, entry, outputs, entry, outputs,
            handled           ? 0u
            : (scenario & 1u) ? 3u
                              : 2u,
            handled || (scenario & 1u) ? -1 : 42, !handled && (scenario & 1u) ? 420u : 0u,
            handled ? 0u : (scenario & 1u) ? 7u : 6u, XR_CORE_TYPE_I64) > 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_native_c_export_binding(void) {
    XrValidatedProgram *program = build_binary_program(XR_CORE_OP_CORE_ADD_I64, 19, 23, 0u);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendIR *backend = NULL;
    XrBackendDiagnostic diagnostic = {0};
    REQUIRE(program && profile);
    REQUIRE(xr_backend_ir_build(program, profile, &options, &backend, &diagnostic) == XR_BACKEND_OK);
    uint32_t entry = xr_validated_program_entry_function(program);
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
    XrBackendCExport binding = {.function_id = entry, .symbol = "public_answer", .header = 1u};
    XrGeneratedC generated = {0};
    REQUIRE(xr_backend_ir_emit_c_exports(backend, true, &binding, 1u, &generated, &diagnostic) ==
            XR_BACKEND_OK);
    REQUIRE(strstr(generated.bytes, "int64_t public_answer(void)") != NULL);
    REQUIRE(strstr(generated.header_bytes, "int64_t public_answer(void);") != NULL);
    REQUIRE(strstr(generated.bytes, "if (result.kind != UINT32_C(0)) abort();") != NULL);
    xr_generated_c_free(&generated);
    binding.header = 0u;
    binding.hidden = 1u;
    REQUIRE(xr_backend_ir_emit_c_exports(backend, false, &binding, 1u, &generated, &diagnostic) ==
            XR_BACKEND_OK);
    REQUIRE(strstr(generated.header_bytes, "public_answer") == NULL);
    xr_generated_c_free(&generated);
    const char *invalid[] = {"", "a;void injected(void)", "return", "main", "bool", "_Atomic",
                              "xr_aot_fn_0", "XrAotOutcome", "1answer", "answer\n"};
    for (size_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        binding.symbol = invalid[index];
        REQUIRE(xr_backend_ir_emit_c_exports(backend, true, &binding, 1u, &generated, &diagnostic) ==
                XR_BACKEND_INVALID_INPUT);
        REQUIRE(!generated.bytes && !generated.header_bytes && generated.size == 0u);
        REQUIRE(diagnostic.status == XR_BACKEND_INVALID_INPUT);
    }
    binding.symbol = "public_answer";
    binding.function_id = UINT32_MAX;
    REQUIRE(xr_backend_ir_emit_c_exports(backend, true, &binding, 1u, &generated, &diagnostic) ==
            XR_BACKEND_INVALID_INPUT);
    binding.function_id = entry;
    binding.reserved8[1] = 1u;
    REQUIRE(xr_backend_ir_emit_c_exports(backend, true, &binding, 1u, &generated, &diagnostic) ==
            XR_BACKEND_INVALID_INPUT);
    REQUIRE(!generated.bytes && !generated.header_bytes);
    xr_backend_ir_free(backend);
}

static void write_element_place_fixture(const char *path, unsigned scenario, bool managed) {
    static const unsigned mutations[] = {0u, 10u, 11u, 12u, 13u, 14u};
    static const char *indices[] = {"0", "1", "-1", "INT64_MAX", "INT64_MIN", "0"};
    static const unsigned loans[] = {0u, 2u, 3u, 5u, 8u};
    REQUIRE(scenario < (managed ? 5u : 6u));
    XrProgramArtifact artifact = {0};
    REQUIRE((managed ? xr_program_array_loan_fixture_write(loans[scenario], &artifact)
                     : xr_program_array_place_fixture_write(mutations[scenario], &artifact)) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    REQUIRE(xr_core_spec_operation_by_id(XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE)->aot_status ==
            XR_CORE_COVERAGE_COMPLETE);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab");
    REQUIRE(output != NULL);
    REQUIRE(fprintf(output,
        "\nint main(void) {\n"
        "  XrAotContext context = {0}; XrAotPanicInfo panic = {0};\n"
        "  XrAotOutcome result = xr_aot_fn_%u(&context, &panic);\n"
        "  if (context.allocations) return 1;\n",
        program->entry_function) > 0);
    if (scenario == 0u || managed) {
        REQUIRE(fprintf(output, "  if (result.kind || result.i64 != %u || panic.code || panic.has_bounds) return 2;\n",
                        managed ? 3u : 42u) > 0);
    } else {
        REQUIRE(fprintf(output,
            "  if (result.kind != 3 || panic.code != 430 || panic.has_bounds != 1 ||\n"
            "      panic.index != %s || panic.length != %uu) return 3;\n",
            indices[scenario], scenario == 5u ? 0u : 1u) > 0);
    }
    REQUIRE(fputs(
        "  XrAotEntryCoroutineFrame frame = {0};\n"
        "  const XrBackendNativeDescriptor *descriptor = &xr_aot_entry_coroutine_descriptor;\n"
        "  if (descriptor->schema_version != 9u) return 4;\n"
        "  descriptor->initialize(&frame, &context, NULL);\n"
        "  XrBackendNativeOutcome native = descriptor->step(&frame);\n"
        "  if (native.kind != (result.kind == 3u ? 7u : 0u) ||\n"
        "      native.panic_present != (result.kind == 3u ? 1u : 0u) ||\n"
        "      native.panic_code != panic.code || native.panic_has_bounds != panic.has_bounds ||\n"
        "      native.panic_index != panic.index || native.panic_length != panic.length) return 5;\n"
        "  if (frame.context.allocations) return 6;\n"
        "  descriptor->drop(&frame);\n"
        "  xr_aot_context_destroy(&context); return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_channel_storage_lowering(void) {
    for (int64_t capacity = 0; capacity <= 2; ++capacity) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_channel_fixture_write(capacity, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        const XrValidatedInstruction *ops = program->functions[program->entry_function].blocks[0].instructions;
        REQUIRE(ops[1].operation_id == XR_CORE_OP_CORE_CHANNEL_CONSTRUCT);
        REQUIRE(ops[4].operation_id == XR_CORE_OP_CORE_CHANNEL_IS_CLOSED);
        XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        XrBackendIR *ir = NULL;
        XrBackendDiagnostic diagnostic = {0};
        XrBackendOptions options = xr_backend_default_options();
        REQUIRE(xr_backend_ir_build(program, profile, &options, &ir, &diagnostic) == XR_BACKEND_OK);
        REQUIRE(xr_backend_ir_verify(ir, &diagnostic));
        XrGeneratedC generated = {0};
        REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK);
        xr_generated_c_free(&generated);
        xr_backend_ir_free(ir);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
        xr_program_artifact_free(&artifact);
    }
}

static void test_atomic_storage_lowering(void) {
    for (unsigned mode = 0u; mode <= 10u; ++mode) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE((mode ? xr_program_atomic_fixture_write_mode(false, 0u, mode, &artifact) : xr_program_atomic_fixture_write(false, 0u, &artifact)) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    const XrValidatedInstruction *ops = program->functions[program->entry_function].blocks[0].instructions;
    REQUIRE(ops[2].operation_id == XR_CORE_OP_CORE_ATOMIC_CONSTRUCT);
    REQUIRE(ops[4].operation_id == (mode >= 5u ? XR_CORE_OP_CORE_ATOMIC_UPDATE : mode ? XR_CORE_OP_CORE_ATOMIC_COMPARE_EXCHANGE : XR_CORE_OP_CORE_ATOMIC_EXCHANGE));
    REQUIRE(ops[6].operation_id == XR_CORE_OP_CORE_ATOMIC_LOAD);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic vm_diagnostic = {0};
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, &vm_diagnostic) == XR_VM_CODE_OK);
    REQUIRE(code != NULL && vm_diagnostic.status == XR_VM_CODE_OK);
    xr_vm_code_free(code);
    XrBackendIR *ir = NULL;
    XrBackendDiagnostic diagnostic = {0};
    XrBackendOptions options = xr_backend_default_options();
    REQUIRE(xr_backend_ir_build(program, profile, &options, &ir, &diagnostic) ==
            XR_BACKEND_OK);
    REQUIRE(ir != NULL && diagnostic.status == XR_BACKEND_OK);
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));
    XrValidatedInstruction *exchange = &program->functions[program->entry_function].blocks[0].instructions[4];
    exchange->immediate.u32 = 5u;
    REQUIRE(!xr_backend_ir_verify(ir, &diagnostic));
    exchange->immediate.u32 = 4u;
    REQUIRE(xr_backend_ir_verify(ir, &diagnostic));
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
    }
}

static void write_string_builder_fixture(const char *path, unsigned scenario) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE((scenario == 3u ? xr_program_string_builder_call_fixture_write(0u, &artifact) :
        xr_program_string_builder_fixture_write(scenario ? 99u + scenario : 0u, &artifact)) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrBackendIR *ir = build_ir(program, profile, XR_BACKEND_OPTIMIZATION_PORTABLE);
    XrGeneratedC generated = {0};
    REQUIRE(xr_backend_ir_emit_c(ir, false, &generated, NULL) == XR_BACKEND_OK);
    FILE *output = fopen(path, "wb");
    REQUIRE(output);
    write_allocation_probe(output);
    REQUIRE(fwrite(generated.bytes, 1u, generated.size, output) == generated.size);
    REQUIRE(fputs("\n#undef malloc\n#undef free\nint main(void) {\n"
        " for (unsigned independent = 0; independent < 2; ++independent) {\n"
        "  int complete = 0;\n"
        "  for (fail_at = 1; fail_at < 128; ++fail_at) {\n"
        "   attempts = 0; XrAotContext context = {0};\n", output) >= 0);
    REQUIRE(fprintf(output, "   XrAotOutcome result = xr_aot_fn_%u(&context);\n", program->entry_function) > 0);
    REQUIRE(fputs("   if (attempts >= fail_at) { if (result.kind != 4 || result.pointer) return 1; }\n"
                  "   else {\n", output) >= 0);
    if (scenario == 3u) {
        REQUIRE(fputs("    if (result.kind || result.i64) return 2;\n", output) >= 0);
    } else {
        REQUIRE(fprintf(output,
            "    const uint8_t expected[] = {65,0,195,169,240,159,152,128};\n"
            "    XrAotString *value = (XrAotString *)result.pointer;\n"
            "    if (result.kind || !value || value->size != %u || value->scalar_count != %u) return 2;\n"
            "    for (size_t offset = 0; offset < value->size; offset += sizeof(expected))\n"
            "     if (memcmp(value->bytes + offset, expected, sizeof(expected))) return 3;\n"
            "    xr_aot_free(&context, value);\n",
            scenario == 2u ? 0u : scenario == 1u ? 72u : 8u,
            scenario == 2u ? 0u : scenario == 1u ? 36u : 4u) > 0);
    }
    REQUIRE(fputs("   }\n"
        "   if (context.allocations || live || bad_free) return 4;\n"
        "   xr_aot_context_destroy(&context);\n"
        "   if (attempts < fail_at) {\n"
        "    printf(\"native StringBuilder allocation failures: instance=%u points=%zu\\n\", independent, fail_at - 1);\n"
        "    complete = 1; break;\n"
        "   }\n"
        "  }\n if (!complete) return 5;\n }\n return 0;\n}\n", output) >= 0);
    REQUIRE(fclose(output) == 0);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void write_byte_compare_fixture(const char *path, unsigned scenario) {
    XrProgramArtifact artifact = {0}; XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_byte_compare_fixture_write(scenario, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    write_generated_fixture(path, program, profile, false);
    FILE *output = fopen(path, "ab"); REQUIRE(output != NULL);
    REQUIRE(fprintf(output,
        "\nint main(void) {\n"
        "  for (unsigned repeat = 0; repeat < 2; ++repeat) {\n"
        "    XrAotContext context = {0}; XrAotOutcome result = xr_aot_fn_0(&context);\n"
        "    if (result.kind || result.boolean != %u || context.allocations) return 1;\n"
        "    xr_aot_context_destroy(&context);\n"
        "  }\n  return 0;\n}\n", scenario == 0u || scenario == 1u || scenario == 6u ? 1u : 0u) > 0);
    REQUIRE(fclose(output) == 0);
    xr_target_profile_free(profile); xr_validated_program_free(program);
}

int main(int argc, char **argv) {
    if (argc == 3 && strncmp(argv[2], "byte-compare-", 13u) == 0) {
        unsigned scenario = (unsigned)strtoul(argv[2] + 13u, NULL, 10);
        REQUIRE(scenario < 9u); write_byte_compare_fixture(argv[1], scenario); return 0;
    }

    const XrCoreOperationSpec *byte_compare =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_BYTES_TIMING_SAFE_EQUAL);
    REQUIRE(byte_compare && byte_compare->aot_status == XR_CORE_COVERAGE_COMPLETE &&
            byte_compare->decoder_status == XR_CORE_COVERAGE_COMPLETE);
    if (argc == 3 && strncmp(argv[2], "string-builder-", 15u) == 0) {
        unsigned scenario = (unsigned)strtoul(argv[2] + 15u, NULL, 10);
        REQUIRE(scenario <= 3u);
        write_string_builder_fixture(argv[1], scenario);
        return 0;
    }
    const uint16_t builder_operations[] = {
        XR_CORE_OP_CORE_STRING_BUILDER_CONSTRUCT,
        XR_CORE_OP_CORE_STRING_BUILDER_APPEND,
        XR_CORE_OP_CORE_STRING_BUILDER_CLEAR,
        XR_CORE_OP_CORE_STRING_BUILDER_LENGTH,
        XR_CORE_OP_CORE_STRING_BUILDER_SNAPSHOT
    };
    for (size_t index = 0u; index < sizeof(builder_operations) /
                                      sizeof(builder_operations[0]); ++index) {
        const XrCoreOperationSpec *spec = xr_core_spec_operation_by_id(builder_operations[index]);
        REQUIRE(spec != NULL);
        REQUIRE(spec->aot_status == XR_CORE_COVERAGE_COMPLETE);
    }

    if (argc == 3 && strcmp(argv[2], "record-reference") == 0) {
        REQUIRE(write_class_native_source(argv[1], true));
        return 0;
    }
    if (argc == 1) {
        test_channel_storage_lowering();
        test_atomic_storage_lowering();
    }
    if (argc == 3 && strncmp(argv[2], "element-loan-", 13u) == 0) {
        REQUIRE(strlen(argv[2]) == 14u && argv[2][13] >= '0' && argv[2][13] <= '4');
        write_element_place_fixture(argv[1], (unsigned)(argv[2][13] - '0'), true);
        return 0;
    }
    if (argc == 3 && strncmp(argv[2], "element-place-", 14u) == 0) {
        REQUIRE(strlen(argv[2]) == 15u && argv[2][14] >= '0' && argv[2][14] <= '5');
        write_element_place_fixture(argv[1], (unsigned)(argv[2][14] - '0'), false);
        return 0;
    }
    test_module_operations_have_typed_native_storage();
    xr_test_suppress_dialogs();
    if (argc == 3 && strncmp(argv[2], "coroutine-outcome-", 18u) == 0) {
        REQUIRE(strlen(argv[2]) == 19u && argv[2][18] >= '0' && argv[2][18] <= '7');
        write_coroutine_outcome_fixture(argv[1], (uint32_t) (argv[2][18] - '0'));
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "integer-divmod-cleanup") == 0) {
        write_integer_divmod_cleanup_fixture(argv[1]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "string-slice-allocations") == 0) {
        write_string_slice_allocation_fixture(argv[1]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "array-default-allocations") == 0) {
        write_array_default_allocation_fixture(argv[1]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "array-append-managed-allocations") == 0) {
        write_array_append_allocation_fixture(argv[1], true);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "array-append-allocations") == 0) {
        write_array_append_allocation_fixture(argv[1], false);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "array-append") == 0) {
        write_array_append_fixture(argv[1]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "array-default") == 0) {
        write_array_default_fixture(argv[1]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "string-slice") == 0) {
        write_string_slice_fixture(argv[1]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "integer-bitwise") == 0) {
        write_integer_bitwise_fixture(argv[1]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "integer-divmod") == 0) {
        write_integer_divmod_fixture(argv[1]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "integer-conversions") == 0) {
        write_integer_conversion_fixture(argv[1]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "integer-values") == 0) {
        write_integer_value_fixture(argv[1]);
        return 0;
    }
    bool class_differential = argc == 3 && strcmp(argv[1], "--h2-class-differential") == 0;
    if (class_differential && strcmp(argv[2], "backend-ir") == 0) {
        test_class_reference_semantics_lowering_and_events();
        puts(XR_H2_AOT_BACKEND_RECORD);
        return 0;
    }
    if (class_differential && strcmp(argv[2], "generated-c-native") == 0) {
        test_class_reference_semantics_lowering_and_events();
        REQUIRE(run_class_generated_c_native());
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "f64-comparisons") == 0) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_f64_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        REQUIRE(program->functions[program->entry_function].blocks[0].instructions[0].operation_id == XR_CORE_OP_CORE_CONSTANT_F64);
        REQUIRE(program->functions[program->entry_function].blocks[0].instructions[8].operation_id == XR_CORE_OP_CORE_COMPARE_F64);
        bool bitcast_seen = false;
        for (uint32_t index = 0u; index < program->functions[program->entry_function].blocks[0].instruction_count; ++index)
            bitcast_seen |= program->functions[program->entry_function].blocks[0].instructions[index].operation_id == XR_CORE_OP_CORE_SCALAR_BITCAST64;
        REQUIRE(bitcast_seen);
        XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        write_generated_fixture(argv[1], program, profile, true);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
        xr_program_artifact_free(&artifact);
        return 0;
    }
    if (argc == 3 && (strcmp(argv[2], "atomic-i64") == 0 || strcmp(argv[2], "atomic-bool") == 0 ||
                      strcmp(argv[2], "atomic-i64-allocations") == 0 || strcmp(argv[2], "atomic-bool-allocations") == 0 ||
                      strncmp(argv[2], "atomic-i64-cas-", 14u) == 0 || strncmp(argv[2], "atomic-bool-cas-", 15u) == 0)) {
        bool atomic_boolean = strncmp(argv[2], "atomic-bool", 11u) == 0;
        bool atomic_allocations = strstr(argv[2], "-allocations") != NULL;
        const char *cas_mode = strstr(argv[2], "-cas-");
        unsigned mode = cas_mode ? (unsigned)strtoul(cas_mode + 5u, NULL, 10) : 0u;
        REQUIRE(mode <= 10u);
        XrProgramArtifact artifact = {0};
        REQUIRE((mode ? xr_program_atomic_fixture_write_mode(atomic_boolean, 0u, mode, &artifact) : xr_program_atomic_fixture_write(atomic_boolean, 0u, &artifact)) == XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        if (atomic_allocations)
            write_owned_allocation_fixture(argv[1], program, profile, atomic_boolean ? 6u : 5u);
        else
            write_generated_fixture(argv[1], program, profile, true);
        xr_program_artifact_free(&artifact);
        xr_validated_program_free(program);
        xr_target_profile_free(profile);
        return 0;
    }
    bool array_alias = argc == 3 && strcmp(argv[2], "array-alias") == 0;
    bool array_allocations = argc == 3 && strncmp(argv[2], "array-allocations-", 18u) == 0;
    if (argc == 3 && (strncmp(argv[2], "array-values-", 13u) == 0 || array_allocations || array_alias)) {
        unsigned scenario = array_alias ? 4u : (unsigned)strtoul(argv[2] + (array_allocations ? 18u : 13u), NULL, 10);
        REQUIRE(scenario < 5u);
        XrProgramArtifact artifact = {0};
        REQUIRE(xr_program_array_fixture_write(scenario, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        if (array_alias)
            write_array_alias_fixture(argv[1], program, profile);
        else if (array_allocations)
            write_owned_allocation_fixture(argv[1], program, profile, scenario);
        else
            write_generated_fixture(argv[1], program, profile, true);
        xr_program_artifact_free(&artifact);
        xr_validated_program_free(program);
        xr_target_profile_free(profile);
        return 0;
    }
    REQUIRE(argc >= 1 && argc <= 3);
    bool seal_mode = argc == 3 && strcmp(argv[1], "--seal") == 0;
    bool invoke_object_mode = argc == 3 && strcmp(argv[2], "sealed-invoke-object") == 0;
    bool panic_object_mode = argc == 3 && strcmp(argv[2], "typed-panic-object") == 0;
    bool assertion_mode = argc == 3 && strcmp(argv[2], "condition-assert") == 0;
    bool coroutine_object_mode = argc == 3 && strcmp(argv[2], "coroutine-yield-object") == 0;
    bool pointer_width_mode = argc == 3 && strcmp(argv[2], "pointer-width") == 0;
    bool operating_system_mode = argc == 3 && strcmp(argv[2], "operating-system") == 0;
    bool architecture_mode = argc == 3 && strcmp(argv[2], "architecture") == 0;
    bool native_abi_mode = argc == 3 && strcmp(argv[2], "native-abi") == 0;
    bool endianness_mode = argc == 3 && strcmp(argv[2], "endianness") == 0;
    bool foreign_target_mode = pointer_width_mode || operating_system_mode || architecture_mode ||
                               native_abi_mode || endianness_mode;
    bool provider_call_mode = argc == 3 && strcmp(argv[2], "provider-call") == 0;
    bool provider_output_mode = argc == 3 && strcmp(argv[2], "provider-output") == 0;
    bool text_output_mode = argc == 3 && strcmp(argv[2], "text-output") == 0;
    bool provider_pipe_mode = argc == 3 && strcmp(argv[2], "provider-pipe") == 0;
    bool module_suspend_mode = argc == 3 && (strcmp(argv[2], "module-suspend-object") == 0 ||
                                             strcmp(argv[2], "module-suspend") == 0);
    bool module_object_mode = argc == 3 && (strcmp(argv[2], "module-output-object") == 0 ||
                                            strcmp(argv[2], "module-failure-object") == 0 ||
                                            strcmp(argv[2], "module-suspend-object") == 0);
    bool module_output_mode = argc == 3 && (strcmp(argv[2], "module-output") == 0 ||
                                            strcmp(argv[2], "module-output-object") == 0);
    bool module_failure_mode = argc == 3 && (strcmp(argv[2], "module-failure") == 0 ||
                                             strcmp(argv[2], "module-failure-object") == 0);
    bool owned_drop_mode = argc == 3 && strcmp(argv[2], "owned-drop") == 0;
    bool module_state_mode = argc == 3 && strncmp(argv[2], "module-state-", 13u) == 0;
    uint32_t module_state_scenario =
        module_state_mode ? (uint32_t) strtoul(argv[2] + 13u, NULL, 10) : 0u;
    XrValidatedProgram *program = NULL;
    if (module_state_mode) {
        XrProgramArtifact artifact = {0};
        REQUIRE(module_state_scenario < 14u);
        REQUIRE(xr_program_module_native_fixture_write(module_state_scenario, &artifact, NULL,
                                                       0u) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
                XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
    } else if (owned_drop_mode) {
        /* This fixture writes its own allocation and lifecycle observer. */
    } else if (seal_mode)
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
    else if (argc == 3 && (strcmp(argv[2], "coroutine-yield") == 0 || coroutine_object_mode))
        program = build_coroutine_program();
    else if (pointer_width_mode)
        program = build_target_query_program(XR_CORE_OP_CORE_TARGET_POINTER_WIDTH, XR_CORE_TYPE_U16,
                                             XR_CORE_CAPABILITY_PROFILE_POINTER_WIDTH);
    else if (operating_system_mode)
        program = build_target_query_program(XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM,
                                             XR_CORE_TYPE_TARGET_OS,
                                             XR_CORE_CAPABILITY_PROFILE_OPERATING_SYSTEM);
    else if (architecture_mode)
        program = build_target_query_program(XR_CORE_OP_CORE_TARGET_ARCHITECTURE,
                                             XR_CORE_TYPE_TARGET_ARCH,
                                             XR_CORE_CAPABILITY_PROFILE_ARCHITECTURE);
    else if (native_abi_mode)
        program =
            build_target_query_program(XR_CORE_OP_CORE_TARGET_NATIVE_ABI, XR_CORE_TYPE_TARGET_ABI,
                                       XR_CORE_CAPABILITY_PROFILE_NATIVE_ABI);
    else if (endianness_mode)
        program = build_target_query_program(XR_CORE_OP_CORE_TARGET_ENDIANNESS,
                                             XR_CORE_TYPE_TARGET_ENDIAN,
                                             XR_CORE_CAPABILITY_PROFILE_ENDIANNESS);
    else if (module_output_mode || module_failure_mode || module_suspend_mode) {
        XrProgramArtifact artifact = {0};
        REQUIRE(xr_program_module_output_fixture_write_with_suspension(
                    module_failure_mode ? 1u : UINT32_MAX, module_suspend_mode ? 1u : UINT32_MAX,
                    &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
                XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
    } else if (provider_call_mode || provider_output_mode || provider_pipe_mode ||
               text_output_mode) {
        /* The exact profile owns the provider contract used to build this Program. */
    } else if (invoke_object_mode || panic_object_mode || assertion_mode) {
        XrProgramArtifact artifact = {0};
        char diagnostic[256] = {0};
        XrProgramBuildStatus fixture_status =
            assertion_mode
                ? xr_program_assert_fixture_write(&artifact, diagnostic, sizeof(diagnostic))
            : panic_object_mode
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
        provider_call_mode ? xr_test_target_profile_build_with_scalar_clock(
                                 false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
                                 XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER)
        : (module_state_mode && (module_state_scenario == 1u || module_state_scenario == 3u ||
                                 module_state_scenario == 4u)) ||
                provider_output_mode || text_output_mode || module_output_mode ||
                module_failure_mode || module_suspend_mode
            ? xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED)
        : provider_pipe_mode
            ? native_provider_profile()
            : xr_test_target_profile_build(
                  foreign_target_mode, foreign_target_mode ? XR_TARGET_RUNTIME_PROFILE_FREESTANDING
                                                           : XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    if (provider_call_mode)
        program = build_provider_call_program(profile, false);
    if (provider_output_mode)
        program = build_output_program(42);
    if (text_output_mode)
        program = build_text_program();
    if (provider_pipe_mode)
        program = build_pipe_program();
    if (owned_drop_mode) {
        write_compound_owned_drop_fixture(argv[1], profile);
    } else if (seal_mode) {
        seal_native_file(argv[2], program, profile);
    } else if (argc >= 2) {
        if (provider_call_mode)
            write_provider_generated_fixture(argv[1], program, profile,
                                             xr_validated_program_entry_function(program));
        else if (assertion_mode)
            write_condition_assert_generated_fixture(argv[1], program, profile,
                                                     xr_validated_program_entry_function(program));
        else
            write_generated_fixture(argv[1], program, profile,
                                    !invoke_object_mode && !panic_object_mode &&
                                        !coroutine_object_mode && !module_object_mode &&
                                        (!module_state_mode || module_state_scenario == 10u ||
                                         module_state_scenario == 12u || module_state_scenario == 13u));
    } else {
        TestBindings bindings;
        build_bindings(profile, &bindings);
        XrInstance *instance = create_instance(program, profile, &bindings, 1u);
        test_reference_vm_aot_identity(program, profile, instance);
        test_native_c_export_binding();
        test_affine_copy_lowering();
        test_empty_aggregate_has_portable_private_c_storage();
        test_sealed_invoke_typed_error_cleanup_lowering();
        test_sealed_invoke_trap_continuation_lowering_and_mutation();
        test_typed_panic_cleanup_lowering();
        test_condition_assert_panic_cleanup_lowering();
        test_existential_pack_test_project_lowering();
        test_existential_owned_read_reborrow_lowering();
        test_callable_pack_and_indirect_call_lowering();
        test_callable_invoke_trap_continuation_lowering_and_mutation();
        test_coroutine_private_state_machine_lowering();
        test_resource_storage_admission();
        test_coroutine_self_loop_parallel_edges_lowering();
        test_coroutine_owner_cancel_cleanup_lowering();
        test_coroutine_trap_continuation_lowering_and_mutation();
        test_cleanup_reason_graph_lowering_and_mutation();
        test_direct_program_lifetime_and_emission();
        test_foreign_profile_and_program_binding_mutation();
        test_provider_call_lowering_and_mutation();
        test_provider_trap_continuation_lowering_and_mutation();
        test_witness_invoke_trap_continuation_lowering_and_mutation();
        test_provider_output_lowering_and_mutation();
    test_sequence_length();
    test_array_values();
        test_text_lowering_and_mutation();
        test_pipe_provider_lowering();
        test_class_reference_semantics_lowering_and_events();
        test_class_ref_coroutine_frame_root();
        puts("canonical XrProgram AOT tests passed");
        retire_instance(&instance);
    }
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    return 0;
}
