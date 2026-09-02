/*
 * Task 299: runtime-only typed VM over the validated XrProgram graph.
 */

#include "core/xr_core_spec_gen.h"
#include "execution/xr_execution.h"
#include "program/xr_program.h"
#include "program/xr_program_verify.h"
#include "program/xr_reference_evaluator.h"
#include "vm/xr_program_vm.h"
#include "os/os_thread.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_invoke_fixture.h"
#include "../program/xr_program_panic_fixture.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(XR_CORE_OP_CORE_CALL_SEALED_INVOKE == 37, "sealed invoke stable id drifted");
_Static_assert(XR_CORE_OP_CORE_PANIC_PUBLISH == 50, "panic publish stable id drifted");

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct TestProviderBindings {
    XrProviderBinding providers[XR_RUNTIME_ABI_MAX_PROVIDERS];
    XrProviderOperationBinding operations[XR_RUNTIME_ABI_MAX_PROVIDERS]
                                         [XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS];
    size_t count;
} TestProviderBindings;

static XrCoreIrKey fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrValidatedProgram *
validate_typed_fixture(const XrCoreIrTypeInput *types, uint32_t type_count,
                       const XrCoreIrConstantInput *constants, uint32_t constant_count,
                       const XrCoreIrFunctionInput *functions, uint32_t function_count) {
    XrCoreIrModuleInput module = {
        .key = fixture_key("task-299:module"),
        .constants = constants,
        .constant_count = constant_count,
        .functions = functions,
        .function_count = function_count,
    };
    XrCoreIrKey semantic_profile = fixture_key("task-299:semantic-profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic_profile.bytes,
        .required_features = &feature,
        .required_feature_count = 1,
        .types = types,
        .type_count = type_count,
        .modules = &module,
        .module_count = 1,
    };
    XrCoreIrProgram *core_program = NULL;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    char diagnostic[256] = {0};
    REQUIRE(xr_core_ir_program_build(&input, &core_program, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_write(core_program, &artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(core_program);
    REQUIRE(program != NULL);
    return program;
}

static XrValidatedProgram *validate_fixture(const XrCoreIrConstantInput *constants,
                                            uint32_t constant_count,
                                            const XrCoreIrFunctionInput *functions,
                                            uint32_t function_count) {
    return validate_typed_fixture(NULL, 0u, constants, constant_count, functions, function_count);
}

static XrValidatedProgram *build_affine_copy_program(void) {
    enum {
        AFFINE_TYPE = 63
    };
    uint16_t fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = fixture_key("vm-affine:type"),
        .local_id = AFFINE_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = fields,
        .field_count = 1u,
    };
    XrCoreIrConstantInput constant = {
        .key = fixture_key("vm-affine:constant"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey scalar = fixture_key("vm-affine:scalar");
    XrCoreIrKey owner = fixture_key("vm-affine:owner");
    XrCoreIrKey copied = fixture_key("vm-affine:copied");
    XrCoreIrKey projected = fixture_key("vm-affine:projected");
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
    XrCoreIrKey block_key = fixture_key("vm-affine:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("vm-affine:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_typed_fixture(&type, 1u, &constant, 1u, &function, 1u);
}

static XrValidatedProgram *build_aggregate_variant_program(bool wrong_variant) {
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
        {.key = fixture_key("aggregate:type:variant"),
         .local_id = VARIANT_TYPE,
         .kind = XR_CORE_IR_TYPE_VARIANT,
         .variants = variants,
         .variant_count = sizeof(variants) / sizeof(variants[0])},
        {.key = fixture_key("aggregate:type:record"),
         .local_id = AGGREGATE_TYPE,
         .kind = XR_CORE_IR_TYPE_AGGREGATE,
         .field_types = aggregate_fields,
         .field_count = sizeof(aggregate_fields) / sizeof(aggregate_fields[0])},
    };
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("aggregate:constant:40"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 40},
        {.key = fixture_key("aggregate:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
        {.key = fixture_key("aggregate:constant:true"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
    };
    XrCoreIrKey v40 = fixture_key("aggregate:value:40");
    XrCoreIrKey v2 = fixture_key("aggregate:value:2");
    XrCoreIrKey vtrue = fixture_key("aggregate:value:true");
    XrCoreIrKey aggregate = fixture_key("aggregate:value:record");
    XrCoreIrKey projected_40 = fixture_key("aggregate:value:projected-40");
    XrCoreIrKey updated = fixture_key("aggregate:value:updated");
    XrCoreIrKey variant = fixture_key("aggregate:value:variant");
    XrCoreIrKey tested = fixture_key("aggregate:value:tested");
    XrCoreIrKey projected_aggregate = fixture_key("aggregate:value:projected-record");
    XrCoreIrKey projected_2 = fixture_key("aggregate:value:projected-2");
    XrCoreIrKey construct_operands[] = {v40, vtrue};
    XrCoreIrKey aggregate_operand[] = {aggregate};
    XrCoreIrKey update_operands[] = {aggregate, v2};
    XrCoreIrKey variant_operands[] = {updated, vtrue};
    XrCoreIrKey variant_operand[] = {variant};
    XrCoreIrKey projected_operand[] = {projected_aggregate};
    XrCoreIrKey returned[] = {projected_2};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v40,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = vtrue,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[2].key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = aggregate,
         .result_type_id = AGGREGATE_TYPE,
         .operands = construct_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = projected_40,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = aggregate_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_UPDATE,
         .result = updated,
         .result_type_id = AGGREGATE_TYPE,
         .operands = update_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_CONSTRUCT,
         .result = variant,
         .result_type_id = VARIANT_TYPE,
         .operands = wrong_variant ? NULL : variant_operands,
         .operand_count = wrong_variant ? 0u : 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT,
         .immediate.variant_ordinal = wrong_variant ? 0u : 1u},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_TEST,
         .result = tested,
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
         .operands = projected_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("aggregate:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("aggregate:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = UINT32_C(1),
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_typed_fixture(types, sizeof(types) / sizeof(types[0]), constants,
                                  sizeof(constants) / sizeof(constants[0]), &function, 1u);
}

static XrValidatedProgram *build_scalar_program(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("scalar:6"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 6},
        {.key = fixture_key("scalar:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
        {.key = fixture_key("scalar:true"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
    };
    XrCoreIrKey v6 = fixture_key("scalar:v6");
    XrCoreIrKey v2 = fixture_key("scalar:v2");
    XrCoreIrKey v8 = fixture_key("scalar:v8");
    XrCoreIrKey vsub = fixture_key("scalar:vsub");
    XrCoreIrKey vmul = fixture_key("scalar:vmul");
    XrCoreIrKey vdiv = fixture_key("scalar:vdiv");
    XrCoreIrKey vcmp = fixture_key("scalar:vcmp");
    XrCoreIrKey vcopy = fixture_key("scalar:vcopy");
    XrCoreIrKey vbool = fixture_key("scalar:vbool");
    XrCoreIrKey two[] = {v6, v2};
    XrCoreIrKey sub[] = {v8, v2};
    XrCoreIrKey mul[] = {vsub, v2};
    XrCoreIrKey div[] = {vmul, v2};
    XrCoreIrKey compare[] = {vdiv, v6};
    XrCoreIrKey copy[] = {vcmp};
    XrCoreIrKey returned[] = {vcopy};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v6,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = vbool,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[2].key},
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = v8,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = two,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_SUB_I64,
         .result = vsub,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = sub,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_MUL_I64,
         .result = vmul,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = mul,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_DIV_I64,
         .result = vdiv,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = div,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_COMPARE_I64,
         .result = vcmp,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = compare,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY,
         .result = vcopy,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = copy,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("scalar:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("scalar:function"),
        .result_type_id = XR_CORE_TYPE_BOOL,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_fixture(constants, sizeof(constants) / sizeof(constants[0]), &function, 1);
}

static XrValidatedProgram *build_binary_program(uint16_t operation_id, int64_t left, int64_t right,
                                                uint32_t mode) {
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("binary:left"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = left},
        {.key = fixture_key("binary:right"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = right},
    };
    XrCoreIrKey left_value = fixture_key("binary:left-value");
    XrCoreIrKey right_value = fixture_key("binary:right-value");
    XrCoreIrKey result_value = fixture_key("binary:result-value");
    XrCoreIrKey operands[] = {left_value, right_value};
    XrCoreIrKey returned[] = {result_value};
    uint16_t result_type =
        operation_id == XR_CORE_OP_CORE_COMPARE_I64 ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64;
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
         .result_type_id = result_type,
         .operands = operands,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = mode},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("binary:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("binary:function"),
        .result_type_id = result_type,
        .effect_mask = operation_id == XR_CORE_OP_CORE_COMPARE_I64 ? 0u : 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_fixture(constants, sizeof(constants) / sizeof(constants[0]), &function, 1);
}

static XrValidatedProgram *build_control_program(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("control:1"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 1},
        {.key = fixture_key("control:true"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
    };
    XrCoreIrKey entry_key = fixture_key("control:entry");
    XrCoreIrKey true_key = fixture_key("control:true-block");
    XrCoreIrKey false_key = fixture_key("control:false-block");
    XrCoreIrKey merge_key = fixture_key("control:merge");
    XrCoreIrKey v1 = fixture_key("control:v1");
    XrCoreIrKey condition = fixture_key("control:condition");
    XrCoreIrKey true_arg = fixture_key("control:true-arg");
    XrCoreIrKey false_arg = fixture_key("control:false-arg");
    XrCoreIrKey true_width = fixture_key("control:true-width");
    XrCoreIrKey false_width = fixture_key("control:false-width");
    XrCoreIrKey merge_arg = fixture_key("control:merge-arg");
    XrCoreIrKey conditional_operands[] = {condition, v1, v1};
    XrCoreIrKey conditional_successors[] = {true_key, false_key};
    XrCoreIrKey true_arguments[] = {true_arg};
    XrCoreIrKey false_arguments[] = {false_arg};
    XrCoreIrKey merge_successor[] = {merge_key};
    XrCoreIrKey true_branch_operands[] = {true_width};
    XrCoreIrKey false_branch_operands[] = {false_width};
    XrCoreIrKey merge_arguments[] = {merge_arg};
    XrCoreIrKey returned[] = {merge_arg};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v1,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = condition,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = conditional_operands,
         .operand_count = 3,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = conditional_successors,
         .successor_count = 2},
    };
    XrCoreIrValueInput true_argument = {.key = true_arg, .type_id = XR_CORE_TYPE_I64};
    XrCoreIrInstructionInput true_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = true_arguments,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_TARGET_POINTER_WIDTH,
         .result = true_width,
         .result_type_id = XR_CORE_TYPE_U32,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = true_branch_operands,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = merge_successor,
         .successor_count = 1},
    };
    XrCoreIrValueInput false_argument = {.key = false_arg, .type_id = XR_CORE_TYPE_I64};
    XrCoreIrInstructionInput false_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_arguments,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_TARGET_POINTER_WIDTH,
         .result = false_width,
         .result_type_id = XR_CORE_TYPE_U32,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_branch_operands,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = merge_successor,
         .successor_count = 1},
    };
    XrCoreIrValueInput merge_argument = {.key = merge_arg, .type_id = XR_CORE_TYPE_U32};
    XrCoreIrInstructionInput merge_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = merge_arguments,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = entry_key,
         .instructions = entry_instructions,
         .instruction_count = sizeof(entry_instructions) / sizeof(entry_instructions[0])},
        {.key = true_key,
         .arguments = &true_argument,
         .argument_count = 1,
         .instructions = true_instructions,
         .instruction_count = sizeof(true_instructions) / sizeof(true_instructions[0])},
        {.key = false_key,
         .arguments = &false_argument,
         .argument_count = 1,
         .instructions = false_instructions,
         .instruction_count = sizeof(false_instructions) / sizeof(false_instructions[0])},
        {.key = merge_key,
         .arguments = &merge_argument,
         .argument_count = 1,
         .instructions = merge_instructions,
         .instruction_count = sizeof(merge_instructions) / sizeof(merge_instructions[0])},
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("control:function"),
        .result_type_id = XR_CORE_TYPE_U32,
        .effect_mask = 9u,
        .capability_mask = 1u,
        .entry_block = entry_key,
        .blocks = blocks,
        .block_count = sizeof(blocks) / sizeof(blocks[0]),
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_fixture(constants, sizeof(constants) / sizeof(constants[0]), &function, 1);
}

static XrValidatedProgram *build_call_program(void) {
    XrCoreIrConstantInput constant = {
        .key = fixture_key("call:42"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey helper_key = fixture_key("call:helper");
    XrCoreIrKey entry_key = fixture_key("call:entry");
    XrCoreIrKey helper_block_key = fixture_key("call:helper-block");
    XrCoreIrKey entry_block_key = fixture_key("call:entry-block");
    XrCoreIrKey constant_value = fixture_key("call:constant-value");
    XrCoreIrKey call_value = fixture_key("call:call-value");
    XrCoreIrKey helper_return[] = {constant_value};
    XrCoreIrKey entry_return[] = {call_value};
    XrCoreIrInstructionInput helper_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = constant_value,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = helper_return,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result = call_value,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = helper_key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = entry_return,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput helper_block = {
        .key = helper_block_key,
        .instructions = helper_instructions,
        .instruction_count = sizeof(helper_instructions) / sizeof(helper_instructions[0]),
    };
    XrCoreIrBlockInput entry_block = {
        .key = entry_block_key,
        .instructions = entry_instructions,
        .instruction_count = sizeof(entry_instructions) / sizeof(entry_instructions[0]),
    };
    XrCoreIrFunctionInput functions[] = {
        {.key = helper_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = 1u,
         .entry_block = helper_block_key,
         .blocks = &helper_block,
         .block_count = 1},
        {.key = entry_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = 5u,
         .entry_block = entry_block_key,
         .blocks = &entry_block,
         .block_count = 1,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
    };
    return validate_fixture(&constant, 1, functions, 2);
}

static XrValidatedProgram *build_local_ref_program(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("local-ref:40"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 40},
        {.key = fixture_key("local-ref:42"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 42},
        {.key = fixture_key("local-ref:drop"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 7},
    };
    XrCoreIrKey helper = fixture_key("local-ref:helper");
    XrCoreIrKey entry = fixture_key("local-ref:entry");
    XrCoreIrKey helper_block_key = fixture_key("local-ref:helper-block");
    XrCoreIrKey entry_block_key = fixture_key("local-ref:entry-block");
    XrCoreIrKey ref_arg = fixture_key("local-ref:arg");
    XrCoreIrKey v40 = fixture_key("local-ref:v40");
    XrCoreIrKey moved = fixture_key("local-ref:moved");
    XrCoreIrKey place = fixture_key("local-ref:place");
    XrCoreIrKey v42 = fixture_key("local-ref:v42");
    XrCoreIrKey loaded = fixture_key("local-ref:loaded");
    XrCoreIrKey dropped = fixture_key("local-ref:dropped");
    XrCoreIrValueInput helper_argument = {
        .key = ref_arg, .type_id = XR_CORE_TYPE_I64, .category = XR_CORE_IR_PLACE};
    XrCoreIrKey ref_operand[] = {ref_arg};
    XrCoreIrKey store_operands[] = {ref_arg, v42};
    XrCoreIrInstructionInput helper_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = ref_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v42,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_PLACE_STORE,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = store_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey move_operand[] = {v40};
    XrCoreIrKey local_operand[] = {moved};
    XrCoreIrKey place_operand[] = {place};
    XrCoreIrKey drop_operand[] = {dropped};
    XrCoreIrKey return_operand[] = {loaded};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v40,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_OWNER_MOVE,
         .result = moved,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = move_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
         .result = place,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_PLACE,
         .operands = local_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = place_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = helper},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOAD,
         .result = loaded,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = place_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = dropped,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[2].key},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput helper_block = {
        .key = helper_block_key,
        .arguments = &helper_argument,
        .argument_count = 1u,
        .instructions = helper_instructions,
        .instruction_count = sizeof(helper_instructions) / sizeof(helper_instructions[0]),
    };
    XrCoreIrBlockInput entry_block = {
        .key = entry_block_key,
        .instructions = entry_instructions,
        .instruction_count = sizeof(entry_instructions) / sizeof(entry_instructions[0]),
    };
    uint16_t parameter_type = XR_CORE_TYPE_I64;
    XrParamMode parameter_mode = XR_PARAM_REF;
    XrCoreIrFunctionInput functions[] = {
        {.key = helper,
         .parameter_types = &parameter_type,
         .parameter_modes = &parameter_mode,
         .parameter_count = 1u,
         .result_type_id = XR_CORE_TYPE_VOID,
         .entry_block = helper_block_key,
         .blocks = &helper_block,
         .block_count = 1u},
        {.key = entry,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = 4u,
         .entry_block = entry_block_key,
         .blocks = &entry_block,
         .block_count = 1u,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
    };
    return validate_fixture(constants, sizeof(constants) / sizeof(constants[0]), functions,
                            sizeof(functions) / sizeof(functions[0]));
}

static XrValidatedProgram *build_trap_program(void) {
    XrCoreIrInstructionInput instruction = {
        .operation_id = XR_CORE_OP_CORE_TRAP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
        .immediate.u32 = XR_REFERENCE_TRAP_EXPLICIT,
    };
    XrCoreIrKey block_key = fixture_key("trap:block");
    XrCoreIrBlockInput block = {
        .key = block_key, .instructions = &instruction, .instruction_count = 1};
    XrCoreIrFunctionInput function = {
        .key = fixture_key("trap:function"),
        .result_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_fixture(NULL, 0, &function, 1);
}

static XrValidatedProgram *build_error_program(void) {
    XrCoreIrKey error_value = fixture_key("error:value");
    XrCoreIrKey block_key = fixture_key("error:block");
    XrCoreIrValueInput block_argument = {.key = error_value, .type_id = XR_CORE_TYPE_ERROR};
    XrCoreIrKey operands[] = {error_value};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = operands,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = operands,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput block = {
        .key = block_key,
        .arguments = &block_argument,
        .argument_count = 1,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    uint16_t parameter = XR_CORE_TYPE_ERROR;
    XrCoreIrFunctionInput function = {
        .key = fixture_key("error:function"),
        .parameter_types = &parameter,
        .parameter_count = 1,
        .result_type_id = XR_CORE_TYPE_VOID,
        .error_type_id = XR_CORE_TYPE_ERROR,
        .effect_mask = XR_CORE_EFFECT_ERROR,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_fixture(NULL, 0, &function, 1);
}

static void provider_entry(void) {
}

static void build_provider_bindings(const XrTargetProfile *profile,
                                    TestProviderBindings *bindings) {
    memset(bindings, 0, sizeof(*bindings));
    bindings->count = xr_target_profile_provider_count(profile);
    REQUIRE(bindings->count > 0);
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

static XrInstance *create_instance(XrValidatedProgram *program, XrTargetProfile *profile,
                                   const TestProviderBindings *bindings, uint64_t generation) {
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = bindings->providers,
        .provider_count = bindings->count,
        .generation = generation,
    };
    XrExecutionDiagnostic diagnostic;
    XrInstance *instance = NULL;
    REQUIRE(xr_execution_instance_create(&input, &instance, &diagnostic) == XR_EXECUTION_OK);
    return instance;
}

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static void compare_value(XrReferenceValue reference, XrVmValue vm) {
    REQUIRE((unsigned) reference.kind == (unsigned) vm.kind);
    switch (reference.kind) {
        case XR_REFERENCE_VALUE_BOOL:
            REQUIRE(reference.as.boolean == vm.as.boolean);
            break;
        case XR_REFERENCE_VALUE_I64:
            REQUIRE(reference.as.i64 == vm.as.i64);
            break;
        case XR_REFERENCE_VALUE_U32:
            REQUIRE(reference.as.u32 == vm.as.u32);
            break;
        case XR_REFERENCE_VALUE_ERROR:
            REQUIRE(reference.as.error == vm.as.error);
            break;
        case XR_REFERENCE_VALUE_PANIC_INFO:
            REQUIRE(reference.as.panic_info == vm.as.panic_info);
            break;
        case XR_REFERENCE_VALUE_AGGREGATE:
            REQUIRE(reference.kind != XR_REFERENCE_VALUE_AGGREGATE);
            break;
        case XR_REFERENCE_VALUE_VOID:
            break;
    }
}

static void compare_outcomes(XrReferenceOutcome reference, XrVmOutcome vm) {
    REQUIRE((unsigned) reference.kind == (unsigned) vm.kind);
    REQUIRE(reference.steps == vm.steps);
    REQUIRE((unsigned) reference.trap == (unsigned) vm.trap);
    if (reference.kind == XR_REFERENCE_OUTCOME_ERROR)
        compare_value(reference.error_value, vm.error_value);
    if (reference.kind == XR_REFERENCE_OUTCOME_PANIC)
        compare_value(reference.panic_value, vm.panic_value);
    if (reference.kind == XR_REFERENCE_OUTCOME_RETURN)
        compare_value(reference.value, vm.value);
}

static XrVmOutcome execute_differential(XrValidatedProgram *program, XrInstance *instance,
                                        uint32_t pointer_width,
                                        const XrReferenceValue *reference_arguments,
                                        const XrVmValue *vm_arguments, uint32_t argument_count) {
    XrVmCodeOptions baseline_options = xr_vm_code_default_options();
    XrVmCodeOptions fixed_options = baseline_options;
    fixed_options.decode_policy = XR_VM_DECODE_FIXED_ROWS;
    XrVmCode *baseline = NULL;
    XrVmCode *fixed = NULL;
    XrVmCodeDiagnostic diagnostic;
    REQUIRE(xr_vm_code_build(instance, &baseline_options, &baseline, &diagnostic) == XR_VM_CODE_OK);
    REQUIRE(xr_vm_code_build(instance, &fixed_options, &fixed, &diagnostic) == XR_VM_CODE_OK);
    REQUIRE(xr_vm_code_matches_instance(baseline, instance));
    REQUIRE(xr_vm_code_matches_instance(fixed, instance));
    REQUIRE(xr_vm_code_decode_policy(baseline) == XR_VM_DECODE_BASELINE_VIEW);
    REQUIRE(xr_vm_code_decode_policy(fixed) == XR_VM_DECODE_FIXED_ROWS);
    REQUIRE(xr_vm_code_private_size(baseline) == 0u);
    REQUIRE(xr_vm_code_private_size(fixed) > 0u);
    REQUIRE(
        !fingerprint_equal(xr_vm_code_private_digest(baseline), xr_vm_code_private_digest(fixed)));

    uint32_t entry = xr_validated_program_entry_function(program);
    XrReferenceProfile profile = {.pointer_width = pointer_width};
    XrReferenceOutcome reference =
        xr_reference_evaluate(program, entry, reference_arguments, argument_count, &profile, NULL);
    XrVmOutcome baseline_result =
        xr_vm_code_execute(baseline, instance, entry, vm_arguments, argument_count);
    XrVmOutcome fixed_result =
        xr_vm_code_execute(fixed, instance, entry, vm_arguments, argument_count);
    compare_outcomes(reference, baseline_result);
    compare_outcomes(reference, fixed_result);
    REQUIRE(fingerprint_equal(baseline_result.logical_trace, fixed_result.logical_trace));

    xr_vm_code_free(fixed);
    xr_vm_code_free(baseline);
    return baseline_result;
}

static void retire_and_free(XrInstance **instance) {
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(*instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(*instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(instance, &diagnostic) == XR_EXECUTION_OK);
}

static void run_program(XrValidatedProgram *program, bool ilp32,
                        const XrReferenceValue *reference_arguments, const XrVmValue *vm_arguments,
                        uint32_t argument_count, XrVmOutcomeKind expected_kind,
                        XrVmValueKind expected_value_kind, uint64_t expected_value) {
    XrTargetProfile *profile =
        xr_test_target_profile_build(ilp32, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrVmOutcome result = execute_differential(program, instance, ilp32 ? 32u : 64u,
                                              reference_arguments, vm_arguments, argument_count);
    REQUIRE(result.kind == expected_kind);
    if (expected_kind == XR_VM_OUTCOME_RETURN) {
        REQUIRE(result.value.kind == expected_value_kind);
        if (expected_value_kind == XR_VM_VALUE_BOOL)
            REQUIRE(result.value.as.boolean == (expected_value != 0u));
        else if (expected_value_kind == XR_VM_VALUE_I64)
            REQUIRE(result.value.as.i64 == (int64_t) expected_value);
        else if (expected_value_kind == XR_VM_VALUE_U32)
            REQUIRE(result.value.as.u32 == (uint32_t) expected_value);
    } else if (expected_kind == XR_VM_OUTCOME_ERROR) {
        REQUIRE(result.error_value.kind == XR_VM_VALUE_ERROR);
        REQUIRE(result.error_value.as.error == (uint32_t) expected_value);
    } else if (expected_kind == XR_VM_OUTCOME_PANIC) {
        REQUIRE(result.panic_value.kind == XR_VM_VALUE_PANIC_INFO);
        REQUIRE(result.panic_value.as.panic_info == (uint32_t) expected_value);
    }
    retire_and_free(&instance);
    xr_target_profile_free(profile);
}

static void test_sealed_invoke_and_cleanup_cfg(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_invoke_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    XrReferenceValue reference_arguments[] = {
        {.kind = XR_REFERENCE_VALUE_BOOL, .as.boolean = true},
        {.kind = XR_REFERENCE_VALUE_ERROR, .as.error = 73u},
        {.kind = XR_REFERENCE_VALUE_I64, .as.i64 = 9},
    };
    XrVmValue vm_arguments[] = {
        {.kind = XR_VM_VALUE_BOOL, .as.boolean = true},
        {.kind = XR_VM_VALUE_ERROR, .as.error = 73u},
        {.kind = XR_VM_VALUE_I64, .as.i64 = 9},
    };
    run_program(program, false, reference_arguments, vm_arguments, 3u, XR_VM_OUTCOME_RETURN,
                XR_VM_VALUE_I64, 42u);
    reference_arguments[0].as.boolean = false;
    vm_arguments[0].as.boolean = false;
    run_program(program, false, reference_arguments, vm_arguments, 3u, XR_VM_OUTCOME_ERROR,
                XR_VM_VALUE_VOID, 73u);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

static void test_typed_panic_invoke_and_cleanup_cfg(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_panic_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    XrReferenceValue reference_arguments[] = {
        {.kind = XR_REFERENCE_VALUE_BOOL, .as.boolean = true},
        {.kind = XR_REFERENCE_VALUE_PANIC_INFO, .as.panic_info = 91u},
        {.kind = XR_REFERENCE_VALUE_I64, .as.i64 = 9},
    };
    XrVmValue vm_arguments[] = {
        {.kind = XR_VM_VALUE_BOOL, .as.boolean = true},
        {.kind = XR_VM_VALUE_PANIC_INFO, .as.panic_info = 91u},
        {.kind = XR_VM_VALUE_I64, .as.i64 = 9},
    };
    run_program(program, false, reference_arguments, vm_arguments, 3u, XR_VM_OUTCOME_RETURN,
                XR_VM_VALUE_I64, 42u);
    reference_arguments[0].as.boolean = false;
    vm_arguments[0].as.boolean = false;
    run_program(program, false, reference_arguments, vm_arguments, 3u, XR_VM_OUTCOME_PANIC,
                XR_VM_VALUE_VOID, 91u);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

static void test_operation_semantics(void) {
    XrValidatedProgram *aggregate = build_aggregate_variant_program(false);
    run_program(aggregate, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64, 2u);
    XrTargetProfile *aggregate_profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(aggregate_profile != NULL);
    TestProviderBindings aggregate_bindings;
    build_provider_bindings(aggregate_profile, &aggregate_bindings);
    XrInstance *aggregate_instance =
        create_instance(aggregate, aggregate_profile, &aggregate_bindings, 1u);
    XrVmCodeOptions aggregate_budget = xr_vm_code_default_options();
    aggregate_budget.max_value_cells = 1u;
    XrVmCodeDiagnostic aggregate_diagnostic;
    XrVmCode *aggregate_code = NULL;
    REQUIRE(xr_vm_code_build(aggregate_instance, &aggregate_budget, &aggregate_code,
                             &aggregate_diagnostic) == XR_VM_CODE_OK);
    XrVmOutcome aggregate_limited =
        xr_vm_code_execute(aggregate_code, aggregate_instance,
                           xr_validated_program_entry_function(aggregate), NULL, 0u);
    REQUIRE(aggregate_limited.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
    xr_vm_code_free(aggregate_code);
    retire_and_free(&aggregate_instance);
    xr_target_profile_free(aggregate_profile);

    XrValidatedProgram *variant_trap = build_aggregate_variant_program(true);
    run_program(variant_trap, false, NULL, NULL, 0, XR_VM_OUTCOME_TRAP, XR_VM_VALUE_VOID, 0u);

    XrValidatedProgram *scalar = build_scalar_program();
    run_program(scalar, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_BOOL, 1u);

    XrValidatedProgram *affine = build_affine_copy_program();
    run_program(affine, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64, 42u);

    XrValidatedProgram *control = build_control_program();
    run_program(control, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_U32, 64u);
    run_program(control, true, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_U32, 32u);

    XrValidatedProgram *call = build_call_program();
    run_program(call, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64, 42u);

    XrValidatedProgram *local_ref = build_local_ref_program();
    run_program(local_ref, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64, 42u);

    XrValidatedProgram *trap = build_trap_program();
    run_program(trap, false, NULL, NULL, 0, XR_VM_OUTCOME_TRAP, XR_VM_VALUE_VOID, 0u);

    XrValidatedProgram *error = build_error_program();
    XrReferenceValue reference_argument = {.kind = XR_REFERENCE_VALUE_ERROR, .as.error = 73u};
    XrVmValue vm_argument = {.kind = XR_VM_VALUE_ERROR, .as.error = 73u};
    run_program(error, false, &reference_argument, &vm_argument, 1, XR_VM_OUTCOME_ERROR,
                XR_VM_VALUE_VOID, 73u);

    xr_validated_program_free(error);
    xr_validated_program_free(trap);
    xr_validated_program_free(local_ref);
    xr_validated_program_free(call);
    xr_validated_program_free(control);
    xr_validated_program_free(affine);
    xr_validated_program_free(scalar);
    xr_validated_program_free(variant_trap);
    xr_validated_program_free(aggregate);
}

static void test_arithmetic_edges(void) {
    struct ArithmeticCase {
        uint16_t operation_id;
        int64_t left;
        int64_t right;
        uint32_t mode;
        XrVmOutcomeKind outcome;
        XrVmValueKind value_kind;
        uint64_t value;
    } cases[] = {
        {XR_CORE_OP_CORE_ADD_I64, INT64_MAX, 1, 0, XR_VM_OUTCOME_TRAP, XR_VM_VALUE_VOID, 0},
        {XR_CORE_OP_CORE_ADD_I64, INT64_MAX, 1, 1, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64,
         UINT64_C(0x8000000000000000)},
        {XR_CORE_OP_CORE_SUB_I64, INT64_MIN, 1, 0, XR_VM_OUTCOME_TRAP, XR_VM_VALUE_VOID, 0},
        {XR_CORE_OP_CORE_MUL_I64, INT64_MAX, 2, 0, XR_VM_OUTCOME_TRAP, XR_VM_VALUE_VOID, 0},
        {XR_CORE_OP_CORE_DIV_I64, 7, 0, 0, XR_VM_OUTCOME_TRAP, XR_VM_VALUE_VOID, 0},
        {XR_CORE_OP_CORE_DIV_I64, INT64_MIN, -1, 0, XR_VM_OUTCOME_TRAP, XR_VM_VALUE_VOID, 0},
        {XR_CORE_OP_CORE_DIV_I64, -7, 2, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64, (uint64_t) -3},
        {XR_CORE_OP_CORE_COMPARE_I64, -1, 0, 2, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_BOOL, 1},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        XrValidatedProgram *program = build_binary_program(
            cases[index].operation_id, cases[index].left, cases[index].right, cases[index].mode);
        run_program(program, false, NULL, NULL, 0, cases[index].outcome, cases[index].value_kind,
                    cases[index].value);
        xr_validated_program_free(program);
    }
}

enum {
    VM_RACE_THREADS = 4
};

typedef struct VmRace {
    XrVmCode *code;
    XrInstance *instance;
    uint32_t entry;
    atomic_bool start;
    atomic_bool drain_started;
    atomic_uint_least64_t returned;
    atomic_uint_least64_t stale;
    atomic_uint_least64_t invalid;
} VmRace;

static void *vm_race_worker(void *opaque) {
    VmRace *race = opaque;
    while (!atomic_load_explicit(&race->start, memory_order_acquire)) {
    }
    while (!atomic_load_explicit(&race->drain_started, memory_order_acquire)) {
        XrVmOutcome result = xr_vm_code_execute(race->code, race->instance, race->entry, NULL, 0u);
        if (result.kind == XR_VM_OUTCOME_RETURN)
            atomic_fetch_add_explicit(&race->returned, 1u, memory_order_relaxed);
        else if (result.kind == XR_VM_OUTCOME_STALE_CODE)
            atomic_fetch_add_explicit(&race->stale, 1u, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(&race->invalid, 1u, memory_order_relaxed);
    }
    XrVmOutcome final = xr_vm_code_execute(race->code, race->instance, race->entry, NULL, 0u);
    if (final.kind == XR_VM_OUTCOME_STALE_CODE)
        atomic_fetch_add_explicit(&race->stale, 1u, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&race->invalid, 1u, memory_order_relaxed);
    return NULL;
}

static void test_concurrent_execution_and_drain(void) {
    XrValidatedProgram *program = build_scalar_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic code_diagnostic;
    REQUIRE(xr_vm_code_build(instance, NULL, &code, &code_diagnostic) == XR_VM_CODE_OK);
    VmRace race = {
        .code = code,
        .instance = instance,
        .entry = xr_validated_program_entry_function(program),
        .start = ATOMIC_VAR_INIT(false),
        .drain_started = ATOMIC_VAR_INIT(false),
        .returned = ATOMIC_VAR_INIT(0),
        .stale = ATOMIC_VAR_INIT(0),
        .invalid = ATOMIC_VAR_INIT(0),
    };
    xr_thread_t threads[VM_RACE_THREADS];
    for (size_t index = 0; index < VM_RACE_THREADS; ++index)
        REQUIRE(xr_thread_create(&threads[index], vm_race_worker, &race));
    atomic_store_explicit(&race.start, true, memory_order_release);
    while (atomic_load_explicit(&race.returned, memory_order_acquire) < 100u) {
    }
    XrExecutionDiagnostic execution_diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    atomic_store_explicit(&race.drain_started, true, memory_order_release);
    for (size_t index = 0; index < VM_RACE_THREADS; ++index)
        REQUIRE(xr_thread_join(threads[index], NULL) == 0);
    REQUIRE(atomic_load_explicit(&race.returned, memory_order_acquire) >= 100u);
    REQUIRE(atomic_load_explicit(&race.stale, memory_order_acquire) >= VM_RACE_THREADS);
    REQUIRE(atomic_load_explicit(&race.invalid, memory_order_acquire) == 0u);
    REQUIRE(xr_execution_instance_pin_count(instance) == 0u);
    REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    xr_vm_code_free(code);
    REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) == XR_EXECUTION_OK);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_policy_budget_generation_and_smoke_benchmark(void) {
    XrValidatedProgram *program = build_scalar_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 41u);
    XrVmCodeDiagnostic code_diagnostic;
    XrVmCodeOptions rejected = xr_vm_code_default_options();
    rejected.quickening_policy = 1u;
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(instance, &rejected, &code, &code_diagnostic) ==
            XR_VM_CODE_POLICY_REJECTED);
    REQUIRE(code == NULL);

    XrVmCodeOptions limited = xr_vm_code_default_options();
    limited.max_steps = 1u;
    REQUIRE(xr_vm_code_build(instance, &limited, &code, &code_diagnostic) == XR_VM_CODE_OK);
    uint32_t entry = xr_validated_program_entry_function(program);
    XrVmOutcome result = xr_vm_code_execute(code, instance, entry, NULL, 0);
    REQUIRE(result.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
    xr_vm_code_free(code);

    XrVmCodeOptions baseline_options = xr_vm_code_default_options();
    XrVmCodeOptions fixed_options = baseline_options;
    fixed_options.decode_policy = XR_VM_DECODE_FIXED_ROWS;
    XrVmCode *baseline = NULL;
    clock_t baseline_build_begin = clock();
    REQUIRE(xr_vm_code_build(instance, &baseline_options, &baseline, &code_diagnostic) ==
            XR_VM_CODE_OK);
    clock_t baseline_build_ticks = clock() - baseline_build_begin;
    clock_t fixed_build_begin = clock();
    REQUIRE(xr_vm_code_build(instance, &fixed_options, &code, &code_diagnostic) == XR_VM_CODE_OK);
    clock_t fixed_build_ticks = clock() - fixed_build_begin;
    clock_t baseline_begin = clock();
    for (unsigned iteration = 0; iteration < 20000u; ++iteration) {
        result = xr_vm_code_execute(baseline, instance, entry, NULL, 0);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
    }
    clock_t baseline_ticks = clock() - baseline_begin;
    clock_t begin = clock();
    for (unsigned iteration = 0; iteration < 20000u; ++iteration) {
        result = xr_vm_code_execute(code, instance, entry, NULL, 0);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
    }
    clock_t fixed_ticks = clock() - begin;
    fprintf(stderr,
            "task-299 vm smoke benchmark: build baseline=%ld fixed=%ld ticks; "
            "execute-20000 baseline=%ld fixed=%ld ticks; private-bytes baseline=%lu fixed=%lu\n",
            (long) baseline_build_ticks, (long) fixed_build_ticks, (long) baseline_ticks,
            (long) fixed_ticks, (unsigned long) xr_vm_code_private_size(baseline),
            (unsigned long) xr_vm_code_private_size(code));
    xr_vm_code_free(baseline);

    XrExecutionDiagnostic execution_diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    XrInstance *successor = NULL;
    REQUIRE(xr_execution_instance_create_successor(instance, bindings.providers, bindings.count,
                                                   &successor,
                                                   &execution_diagnostic) == XR_EXECUTION_OK);
    REQUIRE(!xr_vm_code_matches_instance(code, successor));
    result = xr_vm_code_execute(code, successor, entry, NULL, 0);
    REQUIRE(result.kind == XR_VM_OUTCOME_STALE_CODE);
    result = xr_vm_code_execute(code, instance, entry, NULL, 0);
    REQUIRE(result.kind == XR_VM_OUTCOME_STALE_CODE);
    xr_vm_code_free(code);
    REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) == XR_EXECUTION_OK);
    retire_and_free(&successor);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

int main(void) {
    test_operation_semantics();
    test_sealed_invoke_and_cleanup_cfg();
    test_typed_panic_invoke_and_cleanup_cfg();
    test_arithmetic_edges();
    test_concurrent_execution_and_drain();
    test_policy_budget_generation_and_smoke_benchmark();
    puts("task-299 typed XrProgram VM tests passed");
    return 0;
}
