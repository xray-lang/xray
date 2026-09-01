/*
 * Task 297: XrProgram semantic verifier and independent reference evaluator.
 */

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"
#include "program/xr_program_verify.h"
#include "program/xr_reference_evaluator.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static XrCoreIrKey key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus write_modules(const XrCoreIrModuleInput *modules, uint32_t module_count,
                                          XrProgramArtifact *artifact) {
    XrCoreIrKey profile = key("task-297:reference-profile");
    uint16_t features[] = {XR_CORE_FEATURE_CORE_BASE};
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile.bytes,
        .required_features = features,
        .required_feature_count = 1,
        .modules = modules,
        .module_count = module_count,
    };
    XrCoreIrProgram *program = NULL;
    char diagnostic[256] = {0};
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&input, &program, diagnostic, sizeof(diagnostic));
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, sizeof(diagnostic));
    if (status != XR_PROGRAM_BUILD_OK)
        fprintf(stderr, "fixture build failed: %s\n", diagnostic);
    xr_core_ir_program_free(program);
    return status;
}

static XrProgramBuildStatus write_one_function(const XrCoreIrConstantInput *constants,
                                               uint32_t constant_count,
                                               const XrCoreIrFunctionInput *function,
                                               XrProgramArtifact *artifact) {
    XrCoreIrModuleInput module = {
        .key = key("task-297:module"),
        .constants = constants,
        .constant_count = constant_count,
        .functions = function,
        .function_count = 1,
    };
    return write_modules(&module, 1, artifact);
}

static XrValidatedProgram *validate_ok(const XrProgramArtifact *artifact) {
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic diagnostic;
    XrProgramVerifyStatus status =
        xr_program_validate(artifact->bytes, artifact->size, NULL, &program, &diagnostic);
    if (status != XR_PROGRAM_VERIFY_OK)
        fprintf(stderr, "unexpected verify reject: %s/%s at f=%u b=%u i=%u v=%u\n",
                xr_program_verify_status_name(status),
                xr_program_diagnostic_kind_name(diagnostic.kind), diagnostic.location.function_id,
                diagnostic.location.block_id, diagnostic.location.instruction_id,
                diagnostic.location.value_id);
    CHECK(status == XR_PROGRAM_VERIFY_OK);
    CHECK(program != NULL);
    return program;
}

static void expect_semantic_reject(const XrProgramArtifact *artifact,
                                   XrProgramDiagnosticKind expected) {
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic first;
    XrProgramDiagnostic second;
    XrProgramVerifyStatus first_status =
        xr_program_validate(artifact->bytes, artifact->size, NULL, &program, &first);
    CHECK(first_status == XR_PROGRAM_VERIFY_SEMANTIC_REJECTED);
    CHECK(program == NULL);
    CHECK(first.kind == expected);
    XrProgramVerifyStatus second_status =
        xr_program_validate(artifact->bytes, artifact->size, NULL, &program, &second);
    CHECK(second_status == first_status);
    CHECK(memcmp(&first, &second, sizeof(first)) == 0);
    CHECK(program == NULL);
}

static void test_scalar_operations(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("scalar:constant:6"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 6},
        {.key = key("scalar:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
        {.key = key("scalar:constant:true"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
    };
    XrCoreIrKey v6 = key("scalar:value:6");
    XrCoreIrKey v2 = key("scalar:value:2");
    XrCoreIrKey v8 = key("scalar:value:8");
    XrCoreIrKey vsub = key("scalar:value:sub");
    XrCoreIrKey vmul = key("scalar:value:mul");
    XrCoreIrKey vdiv = key("scalar:value:div");
    XrCoreIrKey vcmp = key("scalar:value:compare");
    XrCoreIrKey vbool = key("scalar:value:bool");
    XrCoreIrKey add_args[] = {v6, v2};
    XrCoreIrKey sub_args[] = {v8, v2};
    XrCoreIrKey mul_args[] = {vsub, v2};
    XrCoreIrKey div_args[] = {vmul, v2};
    XrCoreIrKey compare_args[] = {vdiv, v6};
    XrCoreIrKey return_args[] = {vcmp};
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
         .operands = add_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_SUB_I64,
         .result = vsub,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = sub_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_MUL_I64,
         .result = vmul,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = mul_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_DIV_I64,
         .result = vdiv,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = div_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_COMPARE_I64,
         .result = vcmp,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = compare_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = key("scalar:block:entry");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = key("scalar:function"),
        .result_type_id = XR_CORE_TYPE_BOOL,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_one_function(constants, sizeof(constants) / sizeof(constants[0]), &function,
                             &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceProfile profile = {.pointer_width = 64u};
        XrReferenceOutcome result = xr_reference_evaluate(
            program, xr_validated_program_entry_function(program), NULL, 0, &profile, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_BOOL);
        CHECK(result.value.as.boolean);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);
}

static void test_control_and_profile(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("control:constant:1"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 1},
        {.key = key("control:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
        {.key = key("control:constant:true"),
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
    };
    XrCoreIrKey entry_key = key("control:block:entry");
    XrCoreIrKey true_key = key("control:block:true");
    XrCoreIrKey false_key = key("control:block:false");
    XrCoreIrKey merge_key = key("control:block:merge");
    XrCoreIrKey v1 = key("control:value:1");
    XrCoreIrKey v2 = key("control:value:2");
    XrCoreIrKey vcmp = key("control:value:compare");
    XrCoreIrKey vcond = key("control:value:condition");
    XrCoreIrKey true_arg = key("control:value:true-arg");
    XrCoreIrKey false_arg = key("control:value:false-arg");
    XrCoreIrKey true_width = key("control:value:true-width");
    XrCoreIrKey false_width = key("control:value:false-width");
    XrCoreIrKey merge_arg = key("control:value:merge-arg");
    XrCoreIrKey compare_args[] = {v1, v2};
    XrCoreIrKey conditional_args[] = {vcond, v1, v2};
    XrCoreIrKey conditional_successors[] = {true_key, false_key};
    XrCoreIrKey true_block_args[] = {true_arg};
    XrCoreIrKey false_block_args[] = {false_arg};
    XrCoreIrKey true_branch_args[] = {true_width};
    XrCoreIrKey false_branch_args[] = {false_width};
    XrCoreIrKey merge_successor[] = {merge_key};
    XrCoreIrKey merge_block_args[] = {merge_arg};
    XrCoreIrKey return_args[] = {merge_arg};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v1,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_COMPARE_I64,
         .result = vcmp,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = compare_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 2},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = vcond,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[2].key},
        {.operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = conditional_args,
         .operand_count = 3,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = conditional_successors,
         .successor_count = 2},
    };
    XrCoreIrValueInput true_argument = {.key = true_arg, .type_id = XR_CORE_TYPE_I64};
    XrCoreIrInstructionInput true_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = true_block_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_TARGET_POINTER_WIDTH,
         .result = true_width,
         .result_type_id = XR_CORE_TYPE_U32,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = true_branch_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = merge_successor,
         .successor_count = 1},
    };
    XrCoreIrValueInput false_argument = {.key = false_arg, .type_id = XR_CORE_TYPE_I64};
    XrCoreIrInstructionInput false_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_block_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_TARGET_POINTER_WIDTH,
         .result = false_width,
         .result_type_id = XR_CORE_TYPE_U32,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_branch_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = merge_successor,
         .successor_count = 1},
    };
    XrCoreIrValueInput merge_argument = {.key = merge_arg, .type_id = XR_CORE_TYPE_U32};
    XrCoreIrInstructionInput merge_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = merge_block_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_args,
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
        .key = key("control:function"),
        .result_type_id = XR_CORE_TYPE_U32,
        .effect_mask = 9u,
        .capability_mask = 1u,
        .entry_block = entry_key,
        .blocks = blocks,
        .block_count = sizeof(blocks) / sizeof(blocks[0]),
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_one_function(constants, sizeof(constants) / sizeof(constants[0]), &function,
                             &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceProfile profile = {.pointer_width = 64u};
        XrReferenceOutcome result = xr_reference_evaluate(program, 0, NULL, 0, &profile, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_U32);
        CHECK(result.value.as.u32 == 64u);
        profile.pointer_width = 0;
        result = xr_reference_evaluate(program, 0, NULL, 0, &profile, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_TRAP);
        CHECK(result.trap == XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);
}

static void test_direct_call(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("call:constant:40"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 40},
        {.key = key("call:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
    };
    XrCoreIrKey helper_key = key("call:function:helper");
    XrCoreIrKey main_key = key("call:function:main");
    XrCoreIrKey helper_block_key = key("call:block:helper");
    XrCoreIrKey main_block_key = key("call:block:main");
    XrCoreIrKey v40 = key("call:value:40");
    XrCoreIrKey v2 = key("call:value:2");
    XrCoreIrKey vsum = key("call:value:sum");
    XrCoreIrKey vcall = key("call:value:result");
    XrCoreIrKey add_args[] = {v40, v2};
    XrCoreIrKey helper_return[] = {vsum};
    XrCoreIrKey main_return[] = {vcall};
    XrCoreIrInstructionInput helper_instructions[] = {
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
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = vsum,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = add_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = helper_return,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrInstructionInput main_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result = vcall,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = helper_key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = main_return,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput helper_block = {
        .key = helper_block_key,
        .instructions = helper_instructions,
        .instruction_count = sizeof(helper_instructions) / sizeof(helper_instructions[0]),
    };
    XrCoreIrBlockInput main_block = {
        .key = main_block_key,
        .instructions = main_instructions,
        .instruction_count = sizeof(main_instructions) / sizeof(main_instructions[0]),
    };
    XrCoreIrFunctionInput functions[] = {
        {.key = helper_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = 1u,
         .entry_block = helper_block_key,
         .blocks = &helper_block,
         .block_count = 1},
        {.key = main_key,
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = 5u,
         .entry_block = main_block_key,
         .blocks = &main_block,
         .block_count = 1,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
    };
    XrCoreIrModuleInput module = {
        .key = key("call:module"),
        .constants = constants,
        .constant_count = sizeof(constants) / sizeof(constants[0]),
        .functions = functions,
        .function_count = sizeof(functions) / sizeof(functions[0]),
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_modules(&module, 1, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceProfile profile = {.pointer_width = 64u};
        XrReferenceOutcome result = xr_reference_evaluate(
            program, xr_validated_program_entry_function(program), NULL, 0, &profile, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
        CHECK(result.value.kind == XR_REFERENCE_VALUE_I64);
        CHECK(result.value.as.i64 == 42);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);
}

static void test_terminal_operations(void) {
    XrCoreIrKey trap_block_key = key("trap:block");
    XrCoreIrInstructionInput trap_instruction = {
        .operation_id = XR_CORE_OP_CORE_TRAP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
        .immediate.u32 = XR_REFERENCE_TRAP_EXPLICIT,
    };
    XrCoreIrBlockInput trap_block = {
        .key = trap_block_key, .instructions = &trap_instruction, .instruction_count = 1};
    XrCoreIrFunctionInput trap_function = {
        .key = key("trap:function"),
        .result_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = 1u,
        .entry_block = trap_block_key,
        .blocks = &trap_block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_one_function(NULL, 0, &trap_function, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceOutcome result = xr_reference_evaluate(program, 0, NULL, 0, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_TRAP);
        CHECK(result.trap == XR_REFERENCE_TRAP_EXPLICIT);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);

    XrCoreIrKey error_block_key = key("error:block");
    XrCoreIrKey error_value = key("error:value");
    XrCoreIrValueInput error_argument = {.key = error_value, .type_id = XR_CORE_TYPE_ERROR};
    XrCoreIrKey block_args[] = {error_value};
    XrCoreIrInstructionInput error_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = block_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = block_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput error_block = {
        .key = error_block_key,
        .arguments = &error_argument,
        .argument_count = 1,
        .instructions = error_instructions,
        .instruction_count = sizeof(error_instructions) / sizeof(error_instructions[0]),
    };
    uint16_t parameter_types[] = {XR_CORE_TYPE_ERROR};
    XrCoreIrFunctionInput error_function = {
        .key = key("error:function"),
        .parameter_types = parameter_types,
        .parameter_count = 1,
        .result_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = 2u,
        .entry_block = error_block_key,
        .blocks = &error_block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    CHECK(write_one_function(NULL, 0, &error_function, &artifact) == XR_PROGRAM_BUILD_OK);
    program = validate_ok(&artifact);
    if (program) {
        XrReferenceValue argument = {.kind = XR_REFERENCE_VALUE_ERROR, .as.error = 73u};
        XrReferenceOutcome result = xr_reference_evaluate(program, 0, &argument, 1, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_ERROR);
        CHECK(result.error == 73u);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);
}

typedef enum InvalidFixture {
    INVALID_EFFECT = 0,
    INVALID_RESULT_TYPE,
    INVALID_MISSING_TERMINATOR,
} InvalidFixture;

static XrProgramArtifact build_invalid_fixture(InvalidFixture kind) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("invalid:constant:1"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 1},
        {.key = key("invalid:constant:2"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 2},
    };
    XrCoreIrKey v1 = key("invalid:value:1");
    XrCoreIrKey v2 = key("invalid:value:2");
    XrCoreIrKey v3 = key("invalid:value:3");
    XrCoreIrKey add_args[] = {v1, v2};
    XrCoreIrKey return_args[] = {v3};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v1,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = v2,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = v3,
         .result_type_id = kind == INVALID_RESULT_TYPE ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64,
         .operands = add_args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_args,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = key("invalid:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = kind == INVALID_MISSING_TERMINATOR ? 3u : 4u,
    };
    XrCoreIrFunctionInput function = {
        .key = key("invalid:function"),
        .result_type_id = kind == INVALID_RESULT_TYPE ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64,
        .effect_mask = kind == INVALID_EFFECT ? 0u : 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_one_function(constants, sizeof(constants) / sizeof(constants[0]), &function,
                             &artifact) == XR_PROGRAM_BUILD_OK);
    return artifact;
}

static void test_negative_diagnostics_and_budget(void) {
    XrProgramArtifact artifact = build_invalid_fixture(INVALID_EFFECT);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_EFFECT);
    xr_program_artifact_free(&artifact);

    artifact = build_invalid_fixture(INVALID_RESULT_TYPE);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE);
    xr_program_artifact_free(&artifact);

    artifact = build_invalid_fixture(INVALID_MISSING_TERMINATOR);
    expect_semantic_reject(&artifact, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW);
    xr_program_artifact_free(&artifact);

    test_scalar_operations();
    /* Build a known-valid fixture again for budget and identity rejection. */
    XrCoreIrKey block_key = key("budget:block");
    XrCoreIrInstructionInput instruction = {
        .operation_id = XR_CORE_OP_CORE_TRAP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
        .immediate.u32 = XR_REFERENCE_TRAP_EXPLICIT,
    };
    XrCoreIrBlockInput block = {
        .key = block_key, .instructions = &instruction, .instruction_count = 1};
    XrCoreIrFunctionInput function = {
        .key = key("budget:function"),
        .result_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    CHECK(write_one_function(NULL, 0, &function, &artifact) == XR_PROGRAM_BUILD_OK);
    XrProgramVerifyBudget budget = xr_program_verify_default_budget();
    budget.max_work = 1u;
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic diagnostic;
    CHECK(xr_program_validate(artifact.bytes, artifact.size, &budget, &program, &diagnostic) ==
          XR_PROGRAM_VERIFY_RESOURCE_LIMIT);
    CHECK(program == NULL);
    CHECK(diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT);

    uint8_t *mutated = malloc(artifact.size);
    CHECK(mutated != NULL);
    if (mutated) {
        memcpy(mutated, artifact.bytes, artifact.size);
        size_t fingerprint_offset = XR_PROGRAM_MAGIC_SIZE + 4u + 1u;
        mutated[fingerprint_offset] ^= UINT8_C(1);
        CHECK(xr_program_validate(mutated, artifact.size, NULL, &program, &diagnostic) ==
              XR_PROGRAM_VERIFY_SEMANTIC_REJECTED);
        CHECK(diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_CORE_SPEC_IDENTITY);
        CHECK(program == NULL);
        free(mutated);
    }
    xr_program_artifact_free(&artifact);
}

static void test_evaluator_traps_and_budget(void) {
    XrCoreIrConstantInput constants[] = {
        {.key = key("overflow:max"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = INT64_MAX},
        {.key = key("overflow:one"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 1},
    };
    XrCoreIrKey vmax = key("overflow:value:max");
    XrCoreIrKey vone = key("overflow:value:one");
    XrCoreIrKey vsum = key("overflow:value:sum");
    XrCoreIrKey args[] = {vmax, vone};
    XrCoreIrKey returns[] = {vsum};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = vmax,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = vone,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_ADD_I64,
         .result = vsum,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = args,
         .operand_count = 2,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returns,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = key("overflow:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = key("overflow:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrProgramArtifact artifact = {0};
    CHECK(write_one_function(constants, sizeof(constants) / sizeof(constants[0]), &function,
                             &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = validate_ok(&artifact);
    if (program) {
        XrReferenceOutcome result = xr_reference_evaluate(program, 0, NULL, 0, NULL, NULL);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_TRAP);
        CHECK(result.trap == XR_REFERENCE_TRAP_INTEGER_OVERFLOW);
        XrReferenceBudget budget = {.max_steps = 2u, .max_call_depth = 8u};
        result = xr_reference_evaluate(program, 0, NULL, 0, NULL, &budget);
        CHECK(result.kind == XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
        xr_validated_program_free(program);
    }
    xr_program_artifact_free(&artifact);
}

static int64_t i64_from_bits(uint64_t bits) {
    if (bits <= (uint64_t) INT64_MAX)
        return (int64_t) bits;
    return -(int64_t) (~bits) - 1;
}

static XrProgramArtifact build_typed_chain(uint32_t operation_count, uint64_t seed,
                                           const char *identity_prefix, int64_t *expected_out) {
    XrProgramArtifact artifact = {0};
    XrCoreIrConstantInput constants[2];
    XrCoreIrInstructionInput *instructions =
        calloc((size_t) operation_count + 3u, sizeof(XrCoreIrInstructionInput));
    XrCoreIrKey(*operands)[2] = calloc(operation_count ? operation_count : 1u, sizeof(*operands));
    XrCoreIrKey *results = calloc((size_t) operation_count + 2u, sizeof(XrCoreIrKey));
    CHECK(instructions != NULL && operands != NULL && results != NULL);
    if (!instructions || !operands || !results) {
        free(results);
        free(operands);
        free(instructions);
        return artifact;
    }
    int64_t left = i64_from_bits(seed * UINT64_C(0x9e3779b97f4a7c15));
    int64_t right = i64_from_bits((seed | 1u) * UINT64_C(0xd6e8feb86659fd93));
    char name[96];
    snprintf(name, sizeof(name), "%s:constant:left", identity_prefix);
    constants[0] = (XrCoreIrConstantInput) {
        .key = key(name),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = left,
    };
    snprintf(name, sizeof(name), "%s:constant:right", identity_prefix);
    constants[1] = (XrCoreIrConstantInput) {
        .key = key(name),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = right,
    };
    snprintf(name, sizeof(name), "%s:value:left", identity_prefix);
    results[0] = key(name);
    snprintf(name, sizeof(name), "%s:value:right", identity_prefix);
    results[1] = key(name);
    instructions[0] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
        .result = results[0],
        .result_type_id = XR_CORE_TYPE_I64,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
        .immediate.key = constants[0].key,
    };
    instructions[1] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
        .result = results[1],
        .result_type_id = XR_CORE_TYPE_I64,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
        .immediate.key = constants[1].key,
    };
    uint64_t expected_bits = (uint64_t) left;
    for (uint32_t index = 0; index < operation_count; ++index) {
        snprintf(name, sizeof(name), "%s:value:%u", identity_prefix, index);
        results[index + 2u] = key(name);
        operands[index][0] = index == 0 ? results[0] : results[index + 1u];
        operands[index][1] = results[1];
        uint16_t operation = index % 3u == 0u   ? XR_CORE_OP_CORE_ADD_I64
                             : index % 3u == 1u ? XR_CORE_OP_CORE_SUB_I64
                                                : XR_CORE_OP_CORE_MUL_I64;
        instructions[index + 2u] = (XrCoreIrInstructionInput) {
            .operation_id = operation,
            .result = results[index + 2u],
            .result_type_id = XR_CORE_TYPE_I64,
            .operands = operands[index],
            .operand_count = 2,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
            .immediate.u32 = 1u,
        };
        if (operation == XR_CORE_OP_CORE_ADD_I64)
            expected_bits += (uint64_t) right;
        else if (operation == XR_CORE_OP_CORE_SUB_I64)
            expected_bits -= (uint64_t) right;
        else
            expected_bits *= (uint64_t) right;
    }
    XrCoreIrKey return_operand[] = {operation_count == 0 ? results[0]
                                                         : results[operation_count + 1u]};
    instructions[operation_count + 2u] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = return_operand,
        .operand_count = 1,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    snprintf(name, sizeof(name), "%s:block", identity_prefix);
    XrCoreIrKey block_key = key(name);
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = operation_count + 3u,
    };
    snprintf(name, sizeof(name), "%s:function", identity_prefix);
    XrCoreIrFunctionInput function = {
        .key = key(name),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    CHECK(write_one_function(constants, 2, &function, &artifact) == XR_PROGRAM_BUILD_OK);
    *expected_out = i64_from_bits(expected_bits);
    free(results);
    free(operands);
    free(instructions);
    return artifact;
}

static void test_typed_random_and_linear_work(void) {
    for (uint32_t sample = 1; sample <= 32u; ++sample) {
        char prefix[48];
        snprintf(prefix, sizeof(prefix), "random:%u", sample);
        int64_t expected = 0;
        XrProgramArtifact artifact =
            build_typed_chain(1u + sample % 31u, UINT64_C(0x29700000) + sample, prefix, &expected);
        XrValidatedProgram *program = validate_ok(&artifact);
        if (program) {
            XrReferenceOutcome result = xr_reference_evaluate(program, 0, NULL, 0, NULL, NULL);
            CHECK(result.kind == XR_REFERENCE_OUTCOME_RETURN);
            CHECK(result.value.kind == XR_REFERENCE_VALUE_I64);
            CHECK(result.value.as.i64 == expected);
            xr_validated_program_free(program);
        }
        xr_program_artifact_free(&artifact);
    }

    const uint32_t counts[] = {16u, 64u, 256u};
    uint64_t previous_work = 0;
    for (size_t index = 0; index < sizeof(counts) / sizeof(counts[0]); ++index) {
        int64_t expected = 0;
        XrProgramArtifact artifact =
            build_typed_chain(counts[index], 17u, "resource-ladder", &expected);
        XrValidatedProgram *program = validate_ok(&artifact);
        if (program) {
            uint64_t work = xr_validated_program_verifier_work(program);
            CHECK(work > previous_work);
            CHECK(work <= UINT64_C(20) * counts[index] + UINT64_C(128));
            previous_work = work;
            xr_validated_program_free(program);
        }
        xr_program_artifact_free(&artifact);
    }

    int64_t expected = 0;
    XrProgramArtifact first = build_typed_chain(8u, 99u, "alpha-a", &expected);
    XrProgramArtifact renamed = build_typed_chain(8u, 99u, "alpha-b", &expected);
    CHECK(first.size == renamed.size);
    CHECK(first.size != 0 && memcmp(first.bytes, renamed.bytes, first.size) == 0);
    CHECK(xr_program_id_equal(first.id, renamed.id));
    xr_program_artifact_free(&renamed);
    xr_program_artifact_free(&first);
}

int main(void) {
    test_scalar_operations();
    test_control_and_profile();
    test_direct_call();
    test_terminal_operations();
    test_negative_diagnostics_and_budget();
    test_evaluator_traps_and_budget();
    test_typed_random_and_linear_work();
    if (failures != 0) {
        fprintf(stderr, "XrProgram verifier tests failed: %d\n", failures);
        return 1;
    }
    puts("XrProgram verifier and reference evaluator tests passed");
    return 0;
}
