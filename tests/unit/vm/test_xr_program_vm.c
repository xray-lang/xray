#include "../program/xr_program_byte_compare_fixture.h"
/*
 * Task 299: runtime-only typed VM over the validated XrProgram graph.
 */

#include "../program/xr_program_string_builder_fixture.h"
#include "../program/xr_program_array_default_fixture.h"
#include "../program/xr_program_array_append_fixture.h"
#include "../program/xr_program_string_slice_fixture.h"
#include "../program/xr_program_channel_fixture.h"
#include "../program/xr_program_atomic_fixture.h"
#include "../program/xr_program_f64_fixture.h"
#include "core/xr_core_spec_gen.h"
#include "execution/xr_execution.h"
#include "program/xr_program.h"
#include "program/xr_program_verify.h"
#include "program/xr_reference_evaluator.h"
#include "vm/xr_program_vm.h"
#include "os/os_thread.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_provider_fixture.h"
#include "../program/xr_program_invoke_fixture.h"
#include "../program/xr_program_existential_fixture.h"
#include "../program/xr_program_reborrow_fixture.h"
#include "../program/xr_program_callable_fixture.h"
#include "../program/xr_program_panic_fixture.h"
#include "../program/xr_program_assert_fixture.h"
#include "../program/xr_program_coroutine_fixture.h"
#include "../program/xr_program_coroutine_trap_fixture.h"
#include "../program/xr_program_coroutine_outcome_fixture.h"
#include "../program/xr_program_output_fixture.h"
#include "../program/xr_program_text_fixture.h"
#include "../program/xr_program_integer_fixture.h"
#include "../program/xr_program_sequence_length_fixture.h"
#include "../program/xr_program_array_fixture.h"
#include "../program/xr_program_trap_fixture.h"
#include "../program/xr_program_construct_fixture.h"
#include "../program/xr_program_cleanup_graph_fixture.h"
#include "../program/xr_program_coroutine_branch_fixture.h"

#include <stdatomic.h>
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
_Static_assert(XR_CORE_OP_CORE_ASSERT_CONDITION == 109, "condition assert stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_PACK == 86, "existential pack stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_TEST == 87, "existential test stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_PROJECT == 88, "existential project stable id drifted");
_Static_assert(XR_CORE_OP_CORE_CALLABLE_PACK == 89, "callable pack stable id drifted");
_Static_assert(XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ == 90,
               "existential READ reborrow stable id drifted");
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
    XrProgramVerifyStatus verify_status =
        xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &verify_diagnostic);
    if (verify_status != XR_PROGRAM_VERIFY_OK)
        fprintf(stderr, "unexpected verify reject: %s/%s decode=%s at f=%u b=%u i=%u v=%u\n",
                xr_program_verify_status_name(verify_status),
                xr_program_diagnostic_kind_name(verify_diagnostic.kind),
                xr_program_decode_status_name(verify_diagnostic.decode_status),
                verify_diagnostic.location.function_id, verify_diagnostic.location.block_id,
                verify_diagnostic.location.instruction_id, verify_diagnostic.location.value_id);
    REQUIRE(verify_status == XR_PROGRAM_VERIFY_OK);
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

static XrValidatedProgram *build_coroutine_program(void) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    char diagnostic[256] = {0};
    REQUIRE(xr_program_coroutine_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
}

static XrValidatedProgram *build_owner_coroutine_program(void) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    char diagnostic[256] = {0};
    REQUIRE(xr_program_coroutine_owner_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    return program;
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

static XrValidatedProgram *build_class_alias_mutation_program(bool record) {
    enum {
        CLASS_TYPE = 63
    };
    uint16_t fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = fixture_key("vm-class-alias:type"),
        .local_id = CLASS_TYPE,
        .kind = record ? XR_CORE_IR_TYPE_RECORD_REFERENCE : XR_CORE_IR_TYPE_CLASS_REFERENCE,
        .nominal_kind = record ? XR_CORE_IR_NOMINAL_NONE : XR_CORE_IR_NOMINAL_CLASS,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = fields,
        .field_count = 1u,
    };
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("vm-class-alias:constant:7"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 7},
        {.key = fixture_key("vm-class-alias:constant:42"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 42},
    };
    XrCoreIrKey seven = fixture_key("vm-class-alias:seven");
    XrCoreIrKey forty_two = fixture_key("vm-class-alias:forty-two");
    XrCoreIrKey object = fixture_key("vm-class-alias:object");
    XrCoreIrKey alias = fixture_key("vm-class-alias:alias");
    XrCoreIrKey place = fixture_key("vm-class-alias:place");
    XrCoreIrKey old = fixture_key("vm-class-alias:old");
    XrCoreIrKey loaded = fixture_key("vm-class-alias:loaded");
    XrCoreIrKey construct_operands[] = {seven};
    XrCoreIrKey object_operand[] = {object};
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
         .result = object,
         .result_type_id = CLASS_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_ALIAS,
         .result = alias,
         .result_type_id = CLASS_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = object_operand,
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
         .operands = object_operand,
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
         .operands = object_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("vm-class-alias:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("vm-class-alias:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_typed_fixture(&type, 1u, constants, sizeof(constants) / sizeof(constants[0]),
                                  &function, 1u);
}

static XrValidatedProgram *build_class_ref_receiver_program(void) {
    enum {
        CLASS_TYPE = 63
    };
    uint16_t fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = fixture_key("vm-class-ref-receiver:type"),
        .local_id = CLASS_TYPE,
        .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
        .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = fields,
        .field_count = 1u,
    };
    XrCoreIrConstantInput constant = {
        .key = fixture_key("vm-class-ref-receiver:constant"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };

    XrCoreIrKey method_key = fixture_key("vm-class-ref-receiver:function:method");
    XrCoreIrKey method_block_key = fixture_key("vm-class-ref-receiver:block:method");
    XrCoreIrKey receiver = fixture_key("vm-class-ref-receiver:receiver");
    XrCoreIrKey loaded = fixture_key("vm-class-ref-receiver:loaded");
    XrCoreIrValueInput method_argument = {
        .key = receiver,
        .type_id = CLASS_TYPE,
        .ownership = XR_CORE_IR_NON_OWNER,
    };
    XrCoreIrKey receiver_operand[] = {receiver};
    XrCoreIrKey loaded_operand[] = {loaded};
    XrCoreIrInstructionInput method_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = receiver_operand,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_CLASS_FIELD_LOAD,
         .result = loaded,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = receiver_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = loaded_operand,
         .operand_count = 1u},
    };
    XrCoreIrBlockInput method_block = {
        .key = method_block_key,
        .arguments = &method_argument,
        .argument_count = 1u,
        .instructions = method_instructions,
        .instruction_count = sizeof(method_instructions) / sizeof(method_instructions[0]),
    };
    uint16_t method_parameter_type = CLASS_TYPE;
    XrParamMode method_parameter_mode = XR_PARAM_REF;

    XrCoreIrKey entry_block_key = fixture_key("vm-class-ref-receiver:block:entry");
    XrCoreIrKey scalar = fixture_key("vm-class-ref-receiver:scalar");
    XrCoreIrKey object = fixture_key("vm-class-ref-receiver:object");
    XrCoreIrKey result = fixture_key("vm-class-ref-receiver:result");
    XrCoreIrKey object_operand[] = {object};
    XrCoreIrKey result_operand[] = {result};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = scalar,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
         .result = object,
         .result_type_id = CLASS_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = &scalar,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result = result,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = object_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = method_key},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = object_operand,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = result_operand,
         .operand_count = 1u},
    };
    XrCoreIrBlockInput entry_block = {
        .key = entry_block_key,
        .instructions = entry_instructions,
        .instruction_count = sizeof(entry_instructions) / sizeof(entry_instructions[0]),
    };
    XrCoreIrFunctionInput functions[] = {
        {.key = method_key,
         .parameter_types = &method_parameter_type,
         .parameter_modes = &method_parameter_mode,
         .parameter_count = 1u,
         .has_receiver = true,
         .receiver_mode = XR_PARAM_REF,
         .result_type_id = XR_CORE_TYPE_I64,
         .entry_block = method_block_key,
         .blocks = &method_block,
         .block_count = 1u},
        {.key = fixture_key("vm-class-ref-receiver:function:entry"),
         .result_type_id = XR_CORE_TYPE_I64,
         .effect_mask = XR_CORE_EFFECT_CALL,
         .entry_block = entry_block_key,
         .blocks = &entry_block,
         .block_count = 1u,
         .flags = XR_PROGRAM_FUNCTION_ENTRY},
    };
    return validate_typed_fixture(&type, 1u, &constant, 1u, functions,
                                  sizeof(functions) / sizeof(functions[0]));
}

static XrValidatedProgram *build_class_owned_exchange_program(bool self_assignment) {
    enum {
        CHILD_CLASS_TYPE = 63,
        PARENT_CLASS_TYPE = 64,
    };
    uint16_t child_fields[] = {XR_CORE_TYPE_I64};
    uint16_t parent_fields[] = {CHILD_CLASS_TYPE};
    XrCoreIrTypeInput types[] = {
        {.key = fixture_key("vm-owned-exchange:type:child"),
         .local_id = CHILD_CLASS_TYPE,
         .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
         .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
         .field_types = child_fields,
         .field_count = 1u},
        {.key = fixture_key("vm-owned-exchange:type:parent"),
         .local_id = PARENT_CLASS_TYPE,
         .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
         .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
         .field_types = parent_fields,
         .field_count = 1u},
    };
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("vm-owned-exchange:constant:7"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 7},
        {.key = fixture_key("vm-owned-exchange:constant:42"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 42},
    };
    XrCoreIrKey seven = fixture_key("vm-owned-exchange:seven");
    XrCoreIrKey forty_two = fixture_key("vm-owned-exchange:forty-two");
    XrCoreIrKey original_child = fixture_key("vm-owned-exchange:original-child");
    XrCoreIrKey parent = fixture_key("vm-owned-exchange:parent");
    XrCoreIrKey borrowed = fixture_key("vm-owned-exchange:borrowed");
    XrCoreIrKey replacement = fixture_key("vm-owned-exchange:replacement");
    XrCoreIrKey place = fixture_key("vm-owned-exchange:place");
    XrCoreIrKey old = fixture_key("vm-owned-exchange:old");
    XrCoreIrKey current = fixture_key("vm-owned-exchange:current");
    XrCoreIrKey loaded = fixture_key("vm-owned-exchange:loaded");
    XrCoreIrKey seven_operand[] = {seven};
    XrCoreIrKey forty_two_operand[] = {forty_two};
    XrCoreIrKey parent_construct_operands[] = {original_child};
    XrCoreIrKey parent_operand[] = {parent};
    XrCoreIrKey borrowed_operand[] = {borrowed};
    XrCoreIrKey exchange_operands[] = {place, replacement};
    XrCoreIrKey old_operand[] = {old};
    XrCoreIrKey current_operand[] = {current};
    XrCoreIrKey returned[] = {loaded};
    XrCoreIrInstructionInput instructions[16] = {0};
    uint32_t count = 0u;
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
        .result = seven,
        .result_type_id = XR_CORE_TYPE_I64,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
        .immediate.key = constants[0].key,
    };
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
        .result = forty_two,
        .result_type_id = XR_CORE_TYPE_I64,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
        .immediate.key = constants[1].key,
    };
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
        .result = original_child,
        .result_type_id = CHILD_CLASS_TYPE,
        .result_ownership = XR_CORE_IR_OWNER,
        .operands = seven_operand,
        .operand_count = 1u,
    };
    if (!self_assignment) {
        instructions[count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
            .result = replacement,
            .result_type_id = CHILD_CLASS_TYPE,
            .result_ownership = XR_CORE_IR_OWNER,
            .operands = forty_two_operand,
            .operand_count = 1u,
        };
    }
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
        .result = parent,
        .result_type_id = PARENT_CLASS_TYPE,
        .result_ownership = XR_CORE_IR_OWNER,
        .operands = parent_construct_operands,
        .operand_count = 1u,
    };
    if (self_assignment) {
        instructions[count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CLASS_FIELD_LOAD,
            .result = borrowed,
            .result_type_id = CHILD_CLASS_TYPE,
            .operands = parent_operand,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
            .immediate.field_ordinal = 0u,
        };
        instructions[count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_ALIAS,
            .result = replacement,
            .result_type_id = CHILD_CLASS_TYPE,
            .result_ownership = XR_CORE_IR_OWNER,
            .operands = borrowed_operand,
            .operand_count = 1u,
        };
    }
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CLASS_FIELD_PLACE,
        .result = place,
        .result_type_id = CHILD_CLASS_TYPE,
        .result_category = XR_CORE_IR_PLACE,
        .operands = parent_operand,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
        .immediate.field_ordinal = 0u,
    };
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_PLACE_EXCHANGE,
        .result = old,
        .result_type_id = CHILD_CLASS_TYPE,
        .result_ownership = XR_CORE_IR_OWNER,
        .operands = exchange_operands,
        .operand_count = 2u,
    };
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = old_operand,
        .operand_count = 1u,
    };
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CLASS_FIELD_LOAD,
        .result = current,
        .result_type_id = CHILD_CLASS_TYPE,
        .operands = parent_operand,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
        .immediate.field_ordinal = 0u,
    };
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CLASS_FIELD_LOAD,
        .result = loaded,
        .result_type_id = XR_CORE_TYPE_I64,
        .operands = current_operand,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
        .immediate.field_ordinal = 0u,
    };
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = parent_operand,
        .operand_count = 1u,
    };
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = returned,
        .operand_count = 1u,
    };
    XrCoreIrKey block_key =
        fixture_key(self_assignment ? "vm-self-exchange:block" : "vm-owned-exchange:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = count,
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key(self_assignment ? "vm-self-exchange:function"
                                           : "vm-owned-exchange:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_typed_fixture(types, sizeof(types) / sizeof(types[0]), constants,
                                  sizeof(constants) / sizeof(constants[0]), &function, 1u);
}

static XrValidatedProgram *build_class_trivial_snapshot_program(void) {
    enum {
        RECORD_TYPE = 63,
        CLASS_TYPE = 64,
    };
    uint16_t record_fields[] = {XR_CORE_TYPE_I64};
    uint16_t class_fields[] = {RECORD_TYPE};
    XrCoreIrTypeInput types[] = {
        {.key = fixture_key("vm-class-snapshot:type:record"),
         .local_id = RECORD_TYPE,
         .kind = XR_CORE_IR_TYPE_AGGREGATE,
         .nominal_kind = XR_CORE_IR_NOMINAL_STRUCT,
         .field_types = record_fields,
         .field_count = 1u},
        {.key = fixture_key("vm-class-snapshot:type:class"),
         .local_id = CLASS_TYPE,
         .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
         .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
         .field_types = class_fields,
         .field_count = 1u},
    };
    XrCoreIrConstantInput constants[] = {
        {.key = fixture_key("vm-class-snapshot:constant:7"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 7},
        {.key = fixture_key("vm-class-snapshot:constant:9"),
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 9},
    };
    XrCoreIrKey seven = fixture_key("vm-class-snapshot:seven");
    XrCoreIrKey nine = fixture_key("vm-class-snapshot:nine");
    XrCoreIrKey record = fixture_key("vm-class-snapshot:record");
    XrCoreIrKey instance = fixture_key("vm-class-snapshot:instance");
    XrCoreIrKey snapshot = fixture_key("vm-class-snapshot:snapshot");
    XrCoreIrKey snapshot_place = fixture_key("vm-class-snapshot:snapshot-place");
    XrCoreIrKey item_place = fixture_key("vm-class-snapshot:item-place");
    XrCoreIrKey original = fixture_key("vm-class-snapshot:original");
    XrCoreIrKey result = fixture_key("vm-class-snapshot:result");
    XrCoreIrKey record_operands[] = {seven};
    XrCoreIrKey instance_operands[] = {record};
    XrCoreIrKey instance_operand[] = {instance};
    XrCoreIrKey snapshot_operand[] = {snapshot};
    XrCoreIrKey snapshot_place_operand[] = {snapshot_place};
    XrCoreIrKey store_operands[] = {item_place, nine};
    XrCoreIrKey original_operand[] = {original};
    XrCoreIrKey returned[] = {result};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = seven,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[0].key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = nine,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constants[1].key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = record,
         .result_type_id = RECORD_TYPE,
         .operands = record_operands,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
         .result = instance,
         .result_type_id = CLASS_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = instance_operands,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_CLASS_FIELD_LOAD,
         .result = snapshot,
         .result_type_id = RECORD_TYPE,
         .operands = instance_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
         .result = snapshot_place,
         .result_type_id = RECORD_TYPE,
         .result_category = XR_CORE_IR_PLACE,
         .operands = snapshot_operand,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_PLACE_PROJECT,
         .result = item_place,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_PLACE,
         .operands = snapshot_place_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_PLACE_STORE,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = store_operands,
         .operand_count = 2u},
        {.operation_id = XR_CORE_OP_CORE_CLASS_FIELD_LOAD,
         .result = original,
         .result_type_id = RECORD_TYPE,
         .operands = instance_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = result,
         .result_type_id = XR_CORE_TYPE_I64,
         .operands = original_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = instance_operand,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u},
    };
    XrCoreIrKey block_key = fixture_key("vm-class-snapshot:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("vm-class-snapshot:function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_typed_fixture(types, sizeof(types) / sizeof(types[0]), constants,
                                  sizeof(constants) / sizeof(constants[0]), &function, 1u);
}

static XrValidatedProgram *build_propagated_class_error_program(bool aggregate_error) {
    enum {
        CLASS_TYPE = 63,
        AGGREGATE_TYPE = 64,
    };
    uint16_t class_fields[] = {XR_CORE_TYPE_I64};
    uint16_t aggregate_fields[] = {CLASS_TYPE};
    XrCoreIrTypeInput types[] = {
        {.key = fixture_key("vm-class-error:type:class"),
         .local_id = CLASS_TYPE,
         .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE,
         .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
         .field_types = class_fields,
         .field_count = 1u},
        {.key = fixture_key("vm-class-error:type:aggregate"),
         .local_id = AGGREGATE_TYPE,
         .kind = XR_CORE_IR_TYPE_AGGREGATE,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
         .field_types = aggregate_fields,
         .field_count = 1u},
    };
    XrCoreIrConstantInput constant = {
        .key = fixture_key("vm-class-error:constant"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 7,
    };
    uint16_t error_type = aggregate_error ? AGGREGATE_TYPE : CLASS_TYPE;
    XrCoreIrKey error_argument = fixture_key("vm-class-error:argument");
    XrCoreIrKey error_operand[] = {error_argument};
    XrCoreIrValueInput error_block_argument = {
        .key = error_argument,
        .type_id = error_type,
        .ownership = XR_CORE_IR_OWNER,
    };
    XrCoreIrInstructionInput error_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = error_operand,
         .operand_count = 1u},
        {
            .operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = error_operand,
            .operand_count = 1u,
        },
    };

    XrCoreIrKey error_function_key = fixture_key("vm-class-error:function:error");
    XrCoreIrKey error_block_key = fixture_key("vm-class-error:block:error");
    XrCoreIrBlockInput error_block = {
        .key = error_block_key,
        .arguments = &error_block_argument,
        .argument_count = 1u,
        .instructions = error_instructions,
        .instruction_count = sizeof(error_instructions) / sizeof(error_instructions[0]),
    };
    uint16_t error_parameter = error_type;
    XrParamMode error_parameter_mode = XR_PARAM_MOVE;
    XrCoreIrFunctionInput functions[2] = {
        {.key = error_function_key,
         .parameter_types = &error_parameter,
         .parameter_modes = &error_parameter_mode,
         .parameter_count = 1u,
         .result_type_id = XR_CORE_TYPE_VOID,
         .error_type_id = error_type,
         .effect_mask = XR_CORE_EFFECT_ERROR,
         .entry_block = error_block_key,
         .blocks = &error_block,
         .block_count = 1u},
    };

    XrCoreIrKey scalar = fixture_key("vm-class-error:scalar");
    XrCoreIrKey object = fixture_key("vm-class-error:object");
    XrCoreIrKey aggregate = fixture_key("vm-class-error:aggregate");
    XrCoreIrKey scalar_operand[] = {scalar};
    XrCoreIrKey object_operand[] = {object};
    XrCoreIrKey aggregate_operand[] = {aggregate};
    XrCoreIrKey normal_block_key = fixture_key("vm-class-error:block:normal");
    XrCoreIrKey caller_error_block_key = fixture_key("vm-class-error:block:caller-error");
    XrCoreIrKey invoke_successors[] = {normal_block_key, caller_error_block_key};
    XrCoreIrInstructionInput entry_instructions[4] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = scalar,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT,
         .result = object,
         .result_type_id = CLASS_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = scalar_operand,
         .operand_count = 1u},
    };
    uint32_t entry_instruction_count = 2u;
    if (aggregate_error) {
        entry_instructions[entry_instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
            .result = aggregate,
            .result_type_id = AGGREGATE_TYPE,
            .result_ownership = XR_CORE_IR_OWNER,
            .operands = object_operand,
            .operand_count = 1u,
        };
    }
    entry_instructions[entry_instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CALL_SEALED_INVOKE,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = aggregate_error ? aggregate_operand : object_operand,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
        .immediate.key = error_function_key,
        .successors = invoke_successors,
        .successor_count = sizeof(invoke_successors) / sizeof(invoke_successors[0]),
    };
    XrCoreIrInstructionInput normal_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_RETURN, .result_type_id = XR_CORE_TYPE_VOID},
    };
    XrCoreIrKey caller_error = fixture_key("vm-class-error:caller-error");
    XrCoreIrKey caller_error_operand[] = {caller_error};
    XrCoreIrValueInput caller_error_argument = {
        .key = caller_error,
        .type_id = error_type,
        .ownership = XR_CORE_IR_OWNER,
    };
    XrCoreIrInstructionInput caller_error_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = caller_error_operand,
         .operand_count = 1u},
        {.operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = caller_error_operand,
         .operand_count = 1u},
    };
    XrCoreIrKey entry_block_key = fixture_key("vm-class-error:block:entry");
    XrCoreIrKey entry_function_key = fixture_key("vm-class-error:function:entry");
    XrCoreIrBlockInput entry_blocks[] = {
        {.key = entry_block_key,
         .instructions = entry_instructions,
         .instruction_count = entry_instruction_count},
        {.key = normal_block_key,
         .instructions = normal_instructions,
         .instruction_count = sizeof(normal_instructions) / sizeof(normal_instructions[0])},
        {.key = caller_error_block_key,
         .arguments = &caller_error_argument,
         .argument_count = 1u,
         .instructions = caller_error_instructions,
         .instruction_count =
             sizeof(caller_error_instructions) / sizeof(caller_error_instructions[0])},
    };
    functions[1] = (XrCoreIrFunctionInput) {
        .key = entry_function_key,
        .result_type_id = XR_CORE_TYPE_VOID,
        .error_type_id = error_type,
        .effect_mask = XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_ERROR,
        .entry_block = entry_block_key,
        .blocks = entry_blocks,
        .block_count = sizeof(entry_blocks) / sizeof(entry_blocks[0]),
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_typed_fixture(types, aggregate_error ? 2u : 1u, &constant, 1u, functions,
                                  sizeof(functions) / sizeof(functions[0]));
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
    XrCoreIrKey vnot = fixture_key("scalar:vnot");
    XrCoreIrKey vand = fixture_key("scalar:vand");
    XrCoreIrKey vor = fixture_key("scalar:vor");
    XrCoreIrKey vcopy = fixture_key("scalar:vcopy");
    XrCoreIrKey vbool = fixture_key("scalar:vbool");
    XrCoreIrKey two[] = {v6, v2};
    XrCoreIrKey sub[] = {v8, v2};
    XrCoreIrKey mul[] = {vsub, v2};
    XrCoreIrKey div[] = {vmul, v2};
    XrCoreIrKey compare[] = {vdiv, v6};
    XrCoreIrKey logical_not[] = {vbool};
    XrCoreIrKey logical_and[] = {vcmp, vnot};
    XrCoreIrKey logical_or[] = {vand, vbool};
    XrCoreIrKey copy[] = {vor};
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
        {.operation_id = XR_CORE_OP_CORE_LOGICAL_NOT,
         .result = vnot,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = logical_not,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_LOGICAL_AND,
         .result = vand,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = logical_and,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_LOGICAL_OR,
         .result = vor,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = logical_or,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
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
         .result_type_id = XR_CORE_TYPE_U16,
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
         .result_type_id = XR_CORE_TYPE_U16,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_branch_operands,
         .operand_count = 1,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = merge_successor,
         .successor_count = 1},
    };
    XrCoreIrValueInput merge_argument = {.key = merge_arg, .type_id = XR_CORE_TYPE_U16};
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
        .result_type_id = XR_CORE_TYPE_U16,
        .effect_mask = 9u,
        .capability_mask = 1u,
        .entry_block = entry_key,
        .blocks = blocks,
        .block_count = sizeof(blocks) / sizeof(blocks[0]),
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_fixture(constants, sizeof(constants) / sizeof(constants[0]), &function, 1);
}

static XrValidatedProgram *build_target_query_program(uint16_t operation_id, uint16_t result_type,
                                                      uint32_t capability) {
    XrCoreIrKey value = fixture_key("vm-target-query:value");
    XrCoreIrKey returned[] = {value};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = operation_id,
         .result = value,
         .result_type_id = result_type,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("vm-target-query:block");
    XrCoreIrBlockInput block = {
        .key = block_key, .instructions = instructions, .instruction_count = 2u};
    XrCoreIrFunctionInput function = {
        .key = fixture_key("vm-target-query:function"),
        .result_type_id = result_type,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_TARGET_QUERY,
        .capability_mask = capability,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_fixture(NULL, 0u, &function, 1u);
}

static XrValidatedProgram *build_target_enum_equality_program(void) {
    XrCoreIrKey queried = fixture_key("vm-target-enum:query");
    XrCoreIrKey wasi = fixture_key("vm-target-enum:wasi");
    XrCoreIrKey equal = fixture_key("vm-target-enum:equal");
    XrCoreIrKey compare_operands[] = {queried, wasi};
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
         .operands = compare_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("vm-target-enum:block");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("vm-target-enum:function"),
        .result_type_id = XR_CORE_TYPE_BOOL,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_TARGET_QUERY,
        .capability_mask = XR_CORE_CAPABILITY_PROFILE_OPERATING_SYSTEM,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    return validate_fixture(NULL, 0u, &function, 1u);
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
    enum {
        AGGREGATE_TYPE = 62
    };
    uint16_t aggregate_fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = fixture_key("local-ref:type"),
        .local_id = AGGREGATE_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = aggregate_fields,
        .field_count = 1u,
    };
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
    XrCoreIrKey aggregate = fixture_key("local-ref:aggregate");
    XrCoreIrKey place = fixture_key("local-ref:place");
    XrCoreIrKey field_place = fixture_key("local-ref:field-place");
    XrCoreIrKey v42 = fixture_key("local-ref:v42");
    XrCoreIrKey loaded = fixture_key("local-ref:loaded");
    XrCoreIrKey taken = fixture_key("local-ref:taken");
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
    XrCoreIrKey construct_operand[] = {moved};
    XrCoreIrKey local_operand[] = {aggregate};
    XrCoreIrKey aggregate_place_operand[] = {place};
    XrCoreIrKey place_operand[] = {field_place};
    XrCoreIrKey take_operand[] = {place};
    XrCoreIrKey taken_operand[] = {taken};
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
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = aggregate,
         .result_type_id = AGGREGATE_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
         .result = place,
         .result_type_id = AGGREGATE_TYPE,
         .result_category = XR_CORE_IR_PLACE,
         .operands = local_operand,
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
        {.operation_id = XR_CORE_OP_CORE_PLACE_TAKE,
         .result = taken,
         .result_type_id = AGGREGATE_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = take_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = taken_operand,
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
    return validate_typed_fixture(&type, 1u, constants, sizeof(constants) / sizeof(constants[0]),
                                  functions, sizeof(functions) / sizeof(functions[0]));
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

static const XrTargetProviderContract *scalar_clock_contract(const XrTargetProfile *profile) {
    for (size_t index = 0u; index < xr_target_profile_provider_count(profile); ++index) {
        const XrTargetProviderContract *contract = xr_target_profile_provider(profile, index);
        if (xr_test_target_profile_is_scalar_provider(contract))
            return contract;
    }
    return NULL;
}

static const XrTargetProviderContract *pipe_contract(const XrTargetProfile *profile) {
    for (size_t index = 0u; index < xr_target_profile_provider_count(profile); ++index) {
        const XrTargetProviderContract *contract = xr_target_profile_provider(profile, index);
        if (xr_test_target_profile_is_provider(contract, XR_PROVIDER_IO_CONTRACT_KEY))
            return contract;
    }
    return NULL;
}

static XrValidatedProgram *build_provider_call_program(const XrTargetProfile *profile,
                                                       bool nullary) {
    const XrTargetProviderContract *contract = scalar_clock_contract(profile);
    REQUIRE(contract && contract->operation_count != 0u);
    XrCoreIrConstantInput constant = {
        .key = fixture_key("provider:constant:41"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 41,
    };
    XrCoreIrKey input_value = fixture_key("provider:value:input");
    XrCoreIrKey result_value = fixture_key("provider:value:result");
    XrCoreIrKey call_operand[] = {input_value};
    XrCoreIrKey return_operand[] = {result_value};
    XrCoreIrInstructionInput instructions[3] = {0};
    uint32_t instruction_count = 0u;
    if (!nullary) {
        instructions[instruction_count++] =
            (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
                                        .result = input_value,
                                        .result_type_id = XR_CORE_TYPE_I64,
                                        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
                                        .immediate.key = constant.key};
    }
    instructions[instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_PROVIDER_CALL,
        .result = result_value,
        .result_type_id = XR_CORE_TYPE_I64,
        .operands = nullary ? NULL : call_operand,
        .operand_count = nullary ? 0u : 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
        .immediate.provider_operation = {.contract_id = contract->contract_id,
                                         .operation_id = contract->operations[0].stable_id}};
    instructions[instruction_count++] =
        (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_RETURN,
                                    .result_type_id = XR_CORE_TYPE_VOID,
                                    .operands = return_operand,
                                    .operand_count = 1u,
                                    .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE};
    XrCoreIrKey block_key = fixture_key("provider:block:entry");
    XrCoreIrBlockInput block = {
        .key = block_key, .instructions = instructions, .instruction_count = instruction_count};
    XrCoreIrFunctionInput function = {
        .key = fixture_key("provider:function:entry"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {.key = fixture_key("provider:module"),
                                  .constants = nullary ? NULL : &constant,
                                  .constant_count = nullary ? 0u : 1u,
                                  .functions = &function,
                                  .function_count = 1u};
    XrCoreIrKey semantic = fixture_key("provider:semantic");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrProgramProviderOperationRequirement operation = {
        .operation_id = contract->operations[0].stable_id,
        .logical_contract = xr_program_fixture_scalar_contract(nullary),
    };
    XrCoreIrProviderRequirementInput requirement = {
        .contract_id = contract->contract_id,
        .operations = &operation,
        .operation_count = 1u,
    };
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = semantic.bytes,
                                  .required_features = &feature,
                                  .required_feature_count = 1u,
                                  .provider_requirements = &requirement,
                                  .provider_requirement_count = 1u,
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

static XrValidatedProgram *build_pipe_provider_program(const XrTargetProfile *profile) {
    enum {
        PIPE_PAIR_TYPE = 200,
        PIPE_OPTIONAL_TYPE = 201,
    };
    const XrTargetProviderContract *contract = pipe_contract(profile);
    REQUIRE(contract && contract->operation_count == 1u);
    uint16_t pair_fields[] = {XR_CORE_TYPE_I64, XR_CORE_TYPE_I64};
    uint16_t some_payload[] = {PIPE_PAIR_TYPE};
    XrCoreIrVariantInput optional_variants[] = {
        {0},
        {.payload_types = some_payload, .payload_count = 1u},
    };
    XrCoreIrTypeInput types[] = {
        {.key = fixture_key("pipe:type:pair"),
         .local_id = PIPE_PAIR_TYPE,
         .kind = XR_CORE_IR_TYPE_AGGREGATE,
         .field_types = pair_fields,
         .field_count = 2u},
        {.key = fixture_key("pipe:type:optional"),
         .local_id = PIPE_OPTIONAL_TYPE,
         .kind = XR_CORE_IR_TYPE_VARIANT,
         .variants = optional_variants,
         .variant_count = 2u},
    };
    XrCoreIrKey optional = fixture_key("pipe:value:optional");
    XrCoreIrKey present = fixture_key("pipe:value:present");
    XrCoreIrKey optional_operand[] = {optional};
    XrCoreIrKey return_operand[] = {present};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_PROVIDER_CALL,
         .result = optional,
         .result_type_id = PIPE_OPTIONAL_TYPE,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract->contract_id,
                                          .operation_id = contract->operations[0].stable_id}},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_TEST,
         .result = present,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = optional_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT,
         .immediate.variant_ordinal = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = fixture_key("pipe:block:entry");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("pipe:function:entry"),
        .result_type_id = XR_CORE_TYPE_BOOL,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = fixture_key("pipe:module"),
        .functions = &function,
        .function_count = 1u,
    };
    XrCoreIrKey semantic = fixture_key("pipe:semantic");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrProgramProviderOperationRequirement operation = {
        .operation_id = contract->operations[0].stable_id,
        .logical_contract = xr_program_fixture_pipe_contract(),
    };
    XrCoreIrProviderRequirementInput requirement = {
        .contract_id = contract->contract_id,
        .operations = &operation,
        .operation_count = 1u,
    };
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .provider_requirements = &requirement,
        .provider_requirement_count = 1u,
        .types = types,
        .type_count = sizeof(types) / sizeof(types[0]),
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
    return program;
}

static XrProviderCallStatus provider_increment(void *context, int64_t argument,
                                               int64_t *result_out) {
    if (!context || !result_out)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = argument + 1;
    return XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus provider_nullary(void *context, int64_t *result_out) {
    if (!context || !result_out)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = 73;
    return XR_PROVIDER_CALL_OK;
}

typedef struct ProviderRefusalProbe {
    uint32_t calls;
} ProviderRefusalProbe;

static XrProviderCallStatus provider_refuse_nullary(void *context, int64_t *result_out) {
    ProviderRefusalProbe *probe = context;
    if (!probe || !result_out)
        return XR_PROVIDER_CALL_FAILED;
    ++probe->calls;
    return XR_PROVIDER_CALL_FAILED;
}

static XrProviderCallStatus provider_pipe(void *context, bool *present_out, int64_t *first_out,
                                          int64_t *second_out) {
    if (!context || !present_out || !first_out || !second_out)
        return XR_PROVIDER_CALL_FAILED;
    *present_out = *(const bool *) context;
    *first_out = 17;
    *second_out = 29;
    return XR_PROVIDER_CALL_OK;
}

static void build_scalar_clock_binding(const XrTargetProfile *profile, bool nullary,
                                       TestProviderBindings *bindings) {
    memset(bindings, 0, sizeof(*bindings));
    const XrTargetProviderContract *contract = scalar_clock_contract(profile);
    REQUIRE(contract && contract->operation_count != 0u);
    bindings->count = 1u;
    bindings->providers[0].contract_id = contract->contract_id;
    REQUIRE(xr_target_provider_contract_fingerprint(
                contract, &bindings->providers[0].contract_fingerprint) == XR_RUNTIME_ABI_OK);
    bindings->providers[0].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
    bindings->providers[0].operations = bindings->operations[0];
    bindings->providers[0].operation_count = 1u;
    bindings->operations[0][0].operation_id = contract->operations[0].stable_id;
    bindings->operations[0][0].trampoline_kind =
        nullary ? XR_PROVIDER_TRAMPOLINE_I64_NULLARY : XR_PROVIDER_TRAMPOLINE_I64_UNARY;
    if (nullary)
        bindings->operations[0][0].entry.i64_nullary = provider_nullary;
    else
        bindings->operations[0][0].entry.i64_unary = provider_increment;
    bindings->operations[0][0].context = &bindings->operations[0][0];
}

static void build_pipe_binding(const XrTargetProfile *profile, bool *present,
                               TestProviderBindings *bindings) {
    memset(bindings, 0, sizeof(*bindings));
    const XrTargetProviderContract *contract = pipe_contract(profile);
    REQUIRE(contract && contract->operation_count == 1u);
    bindings->count = 1u;
    bindings->providers[0].contract_id = contract->contract_id;
    REQUIRE(xr_target_provider_contract_fingerprint(
                contract, &bindings->providers[0].contract_fingerprint) == XR_RUNTIME_ABI_OK);
    bindings->providers[0].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
    bindings->providers[0].operations = bindings->operations[0];
    bindings->providers[0].operation_count = 1u;
    bindings->operations[0][0].operation_id = contract->operations[0].stable_id;
    bindings->operations[0][0].trampoline_kind = XR_PROVIDER_TRAMPOLINE_OPTIONAL_I64_PAIR_NULLARY;
    bindings->operations[0][0].entry.optional_i64_pair_nullary = provider_pipe;
    bindings->operations[0][0].context = present;
}

static void build_provider_bindings(const XrTargetProfile *profile,
                                    TestProviderBindings *bindings) {
    (void) profile;
    memset(bindings, 0, sizeof(*bindings));
}

static XrInstance *create_instance(XrValidatedProgram *program, XrTargetProfile *profile,
                                   const TestProviderBindings *bindings, uint64_t generation) {
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = bindings->count ? bindings->providers : NULL,
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
    switch (reference.kind) {
        case XR_REFERENCE_VALUE_BOOL:
            REQUIRE(vm.kind == XR_VM_VALUE_BOOL);
            REQUIRE(reference.as.boolean == vm.as.boolean);
            break;
        case XR_REFERENCE_VALUE_I64:
            REQUIRE(vm.kind == XR_VM_VALUE_I64);
            REQUIRE(reference.as.i64 == vm.as.i64);
            break;
        case XR_REFERENCE_VALUE_U32:
            REQUIRE(vm.kind == XR_VM_VALUE_U32);
            REQUIRE(reference.as.u32 == vm.as.u32);
            break;
        case XR_REFERENCE_VALUE_U16:
            REQUIRE(vm.kind == XR_VM_VALUE_U16);
            REQUIRE(reference.as.u16 == vm.as.u16);
            break;
        case XR_REFERENCE_VALUE_TARGET_OS:
            REQUIRE(vm.kind == XR_VM_VALUE_TARGET_OS);
            REQUIRE(reference.as.target_enum == vm.as.target_enum);
            break;
        case XR_REFERENCE_VALUE_TARGET_ARCH:
            REQUIRE(vm.kind == XR_VM_VALUE_TARGET_ARCH);
            REQUIRE(reference.as.target_enum == vm.as.target_enum);
            break;
        case XR_REFERENCE_VALUE_TARGET_ABI:
            REQUIRE(vm.kind == XR_VM_VALUE_TARGET_ABI);
            REQUIRE(reference.as.target_enum == vm.as.target_enum);
            break;
        case XR_REFERENCE_VALUE_TARGET_ENDIAN:
            REQUIRE(vm.kind == XR_VM_VALUE_TARGET_ENDIAN);
            REQUIRE(reference.as.target_enum == vm.as.target_enum);
            break;
        case XR_REFERENCE_VALUE_ERROR:
            REQUIRE(vm.kind == XR_VM_VALUE_ERROR);
            REQUIRE(reference.as.error == vm.as.error);
            break;
        case XR_REFERENCE_VALUE_RUNE:
            REQUIRE(vm.kind == XR_VM_VALUE_RUNE);
            REQUIRE(reference.as.rune == vm.as.rune);
            break;
        case XR_REFERENCE_VALUE_STRING: {
            XrReferenceStringView expected;
            XrVmStringView actual;
            REQUIRE(xr_reference_value_string_view(&reference, &expected));
            REQUIRE(xr_vm_value_string_view(&vm, &actual));
            REQUIRE(expected.size == actual.size);
            REQUIRE(expected.size == 0u ||
                    memcmp(expected.bytes, actual.bytes, expected.size) == 0);
            break;
        }
        case XR_REFERENCE_VALUE_PANIC_INFO:
            REQUIRE(vm.kind == XR_VM_VALUE_PANIC_INFO);
            REQUIRE(reference.as.panic_info == vm.as.panic_info.code);
            break;
        case XR_REFERENCE_VALUE_AGGREGATE:
        case XR_REFERENCE_VALUE_CLASS_REFERENCE:
        case XR_REFERENCE_VALUE_EXISTENTIAL:
        case XR_REFERENCE_VALUE_CALLABLE:
            REQUIRE(false);
            break;
        case XR_REFERENCE_VALUE_VOID:
            REQUIRE(vm.kind == XR_VM_VALUE_VOID);
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
                                        const XrTargetProfile *profile,
                                        const XrReferenceValue *reference_arguments,
                                        const XrVmValue *vm_arguments, uint32_t argument_count) {
    uint16_t pointer_width =
        (uint16_t) (xr_target_profile_machine_facts(profile)->data_layout.pointer.size * 8u);
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, &diagnostic) == XR_VM_CODE_OK);
    REQUIRE(xr_vm_code_matches_instance(code, instance));

    uint32_t entry = xr_validated_program_entry_function(program);
    XrExecutionLease lease = {0};
    REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
    XrTargetProfile *target_profile = xr_execution_lease_retain_profile(&lease);
    REQUIRE(target_profile != NULL);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(target_profile);
    REQUIRE(machine != NULL);
    XrReferenceProfile reference_profile = {.pointer_width = pointer_width,
                                            .operating_system = machine->operating_system,
                                            .architecture = machine->architecture,
                                            .native_abi = machine->native_abi,
                                            .endianness = machine->data_layout.endian};
    REQUIRE(xr_execution_lease_release(&lease));
    xr_target_profile_free(target_profile);
    XrReferenceOutcome reference = xr_reference_evaluate(program, entry, reference_arguments,
                                                         argument_count, &reference_profile, NULL);
    XrVmOutcome result = xr_vm_code_execute(code, instance, entry, vm_arguments, argument_count);
    compare_outcomes(reference, result);

    XrVmExecution *execution = NULL;
    REQUIRE(xr_vm_execution_create(code, instance, entry, vm_arguments, argument_count, &execution));
    REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
    XrVmOutcome stepped = xr_vm_execution_step(execution);
    compare_outcomes(reference, stepped);
    REQUIRE(!fingerprint_equal(stepped.logical_trace, (XrFingerprint) {{0}}));
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_vm_execution_step(execution).kind == XR_VM_OUTCOME_INVALID_INVOCATION);
    xr_vm_execution_free(execution);
    xr_vm_code_free(code);
    return result;
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
    XrVmOutcome result = execute_differential(program, instance, profile, reference_arguments,
                                              vm_arguments, argument_count);
    REQUIRE(result.kind == expected_kind);
    if (expected_kind == XR_VM_OUTCOME_RETURN) {
        REQUIRE(result.value.kind == expected_value_kind);
        if (expected_value_kind == XR_VM_VALUE_BOOL)
            REQUIRE(result.value.as.boolean == (expected_value != 0u));
        else if (expected_value_kind == XR_VM_VALUE_I64)
            REQUIRE(result.value.as.i64 == (int64_t) expected_value);
        else if (expected_value_kind == XR_VM_VALUE_U32)
            REQUIRE(result.value.as.u32 == (uint32_t) expected_value);
        else if (expected_value_kind == XR_VM_VALUE_U16)
            REQUIRE(result.value.as.u16 == (uint16_t) expected_value);
        else if (expected_value_kind >= XR_VM_VALUE_TARGET_OS &&
                 expected_value_kind <= XR_VM_VALUE_TARGET_ENDIAN)
            REQUIRE(result.value.as.target_enum == (uint16_t) expected_value);
    } else if (expected_kind == XR_VM_OUTCOME_ERROR) {
        REQUIRE(result.error_value.kind == XR_VM_VALUE_ERROR);
        REQUIRE(result.error_value.as.error == (uint32_t) expected_value);
    } else if (expected_kind == XR_VM_OUTCOME_PANIC) {
        REQUIRE(result.panic_value.kind == XR_VM_VALUE_PANIC_INFO);
        REQUIRE(result.panic_value.as.panic_info.code == (uint32_t) expected_value);
    }
    retire_and_free(&instance);
    xr_target_profile_free(profile);
}

static bool reference_provider_call(void *context, uint32_t requirement_index,
                                    uint32_t operation_index, int64_t argument,
                                    int64_t *result_out) {
    return xr_execution_lease_provider_call_i64_unary(context, requirement_index, operation_index,
                                                      argument,
                                                      result_out) == XR_EXECUTION_PROVIDER_CALL_OK;
}

static bool reference_provider_call_nullary(void *context, uint32_t requirement_index,
                                            uint32_t operation_index, int64_t *result_out) {
    return xr_execution_lease_provider_call_i64_nullary(context, requirement_index, operation_index,
                                                        result_out) ==
           XR_EXECUTION_PROVIDER_CALL_OK;
}

static bool reference_provider_call_optional_i64_pair(void *context, uint32_t requirement_index,
                                                      uint32_t operation_index, bool *present_out,
                                                      int64_t *first_out, int64_t *second_out) {
    return xr_execution_lease_provider_call_optional_i64_pair_nullary(
               context, requirement_index, operation_index, present_out, first_out, second_out) ==
           XR_EXECUTION_PROVIDER_CALL_OK;
}

static void test_provider_call_differential(void) {
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
        TestProviderBindings bindings;
        build_scalar_clock_binding(profile, nullary, &bindings);
        XrInstance *instance = create_instance(program, profile, &bindings, 1u);
        XrVmCodeOptions baseline_options = xr_vm_code_default_options();

        XrVmCode *baseline = NULL;

        XrVmCodeDiagnostic diagnostic;
        REQUIRE(xr_vm_code_build(program, profile, &baseline_options, &baseline, &diagnostic) ==
                XR_VM_CODE_OK);

        XrExecutionLease lease = {0};
        REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
        XrReferenceProviderBinding reference_binding = {
            .context = &lease,
            .call_i64_unary = reference_provider_call,
            .call_i64_nullary = reference_provider_call_nullary,
        };
        uint32_t entry = xr_validated_program_entry_function(program);
        XrReferenceOutcome reference =
            xr_reference_evaluate_bound(program, entry, NULL, 0u, NULL, NULL, &reference_binding);
        XrVmOutcome baseline_result = xr_vm_code_execute(baseline, instance, entry, NULL, 0u);

        compare_outcomes(reference, baseline_result);

        REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RETURN);
        REQUIRE(reference.value.kind == XR_REFERENCE_VALUE_I64);
        REQUIRE(reference.value.as.i64 == (nullary ? 73 : 42));
        if (nullary) {
            TestProviderBindings other_bindings;
            build_scalar_clock_binding(profile, true, &other_bindings);
            ProviderRefusalProbe refusal = {0};
            other_bindings.operations[0][0].entry.i64_nullary = provider_refuse_nullary;
            other_bindings.operations[0][0].context = &refusal;
            XrInstance *other = create_instance(program, profile, &other_bindings, 99u);
            REQUIRE(xr_vm_code_matches_instance(baseline, other));
            XrVmOutcome refused = xr_vm_code_execute(baseline, other, entry, NULL, 0u);
            REQUIRE(refused.kind == XR_VM_OUTCOME_TRAP);
            REQUIRE(refused.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED);
            REQUIRE(refusal.calls == 1u);
            xr_vm_outcome_dispose(&refused);
            XrVmOutcome original = xr_vm_code_execute(baseline, instance, entry, NULL, 0u);
            REQUIRE(original.kind == XR_VM_OUTCOME_RETURN);
            REQUIRE(original.value.as.i64 == 73);
            REQUIRE(refusal.calls == 1u);
            xr_vm_outcome_dispose(&original);
            retire_and_free(&other);
        }
        REQUIRE(xr_execution_lease_release(&lease));

        xr_vm_code_free(baseline);
        retire_and_free(&instance);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
    }
}

static void test_provider_trap_continuation_differential(void) {
    XrTargetProfile *profile = xr_test_target_profile_build_with_nullary_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED, XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    REQUIRE(profile != NULL);
    const XrTargetProviderContract *contract = scalar_clock_contract(profile);
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

    TestProviderBindings bindings;
    build_scalar_clock_binding(profile, true, &bindings);
    ProviderRefusalProbe probe = {0};
    bindings.operations[0][0].entry.i64_nullary = provider_refuse_nullary;
    bindings.operations[0][0].context = &probe;
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);

    XrVmCodeOptions baseline_options = xr_vm_code_default_options();

    XrVmCode *baseline = NULL;

    XrVmCodeDiagnostic diagnostic;
    REQUIRE(xr_vm_code_build(program, profile, &baseline_options, &baseline, &diagnostic) ==
            XR_VM_CODE_OK);

    XrExecutionLease lease = {0};
    REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
    XrReferenceProviderBinding reference_binding = {
        .context = &lease,
        .call_i64_nullary = reference_provider_call_nullary,
    };
    uint32_t entry = xr_validated_program_entry_function(program);
    XrReferenceOutcome reference =
        xr_reference_evaluate_bound(program, entry, NULL, 0u, NULL, NULL, &reference_binding);
    XrVmOutcome baseline_result = xr_vm_code_execute(baseline, instance, entry, NULL, 0u);

    compare_outcomes(reference, baseline_result);

    REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_TRAP);
    REQUIRE(reference.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
    REQUIRE(probe.calls == 2u);
    REQUIRE(xr_execution_lease_release(&lease));

    xr_vm_code_free(baseline);
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_pipe_provider_call_differential(void) {
    for (uint32_t present_value = 0u; present_value < 2u; ++present_value) {
        bool present = present_value != 0u;
        XrTargetProfile *profile =
            xr_test_target_profile_build_with_pipe(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        XrValidatedProgram *program = build_pipe_provider_program(profile);
        TestProviderBindings bindings;
        build_pipe_binding(profile, &present, &bindings);
        XrInstance *instance = create_instance(program, profile, &bindings, 1u);
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic diagnostic;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, &diagnostic) == XR_VM_CODE_OK);
        XrExecutionLease lease = {0};
        REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
        XrReferenceProviderBinding reference_binding = {
            .context = &lease,
            .call_optional_i64_pair_nullary = reference_provider_call_optional_i64_pair,
        };
        uint32_t entry = xr_validated_program_entry_function(program);
        XrReferenceOutcome reference =
            xr_reference_evaluate_bound(program, entry, NULL, 0u, NULL, NULL, &reference_binding);
        XrVmOutcome vm = xr_vm_code_execute(code, instance, entry, NULL, 0u);
        compare_outcomes(reference, vm);
        REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RETURN);
        REQUIRE(reference.value.kind == XR_REFERENCE_VALUE_BOOL);
        REQUIRE(reference.value.as.boolean == present);
        REQUIRE(xr_execution_lease_release(&lease));
        xr_vm_code_free(code);
        retire_and_free(&instance);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
    }
}

typedef struct OutputCapture {
    uint8_t bytes[256];
    size_t size;
    uint32_t calls;
    bool fail;
} OutputCapture;

static XrProviderCallStatus capture_output_write(void *opaque, const uint8_t *bytes, size_t size) {
    OutputCapture *capture = opaque;
    if (!capture || (!bytes && size != 0u) || size > sizeof(capture->bytes) - capture->size)
        return XR_PROVIDER_CALL_FAILED;
    ++capture->calls;
    if (capture->fail)
        return XR_PROVIDER_CALL_FAILED;
    memcpy(capture->bytes + capture->size, bytes, size);
    capture->size += size;
    return XR_PROVIDER_CALL_OK;
}

static const XrTargetProviderContract *output_contract(const XrTargetProfile *profile) {
    const XrTargetProviderContract *result = NULL;
    for (size_t index = 0u; index < xr_target_profile_provider_count(profile); ++index) {
        const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, index);
        if (xr_test_target_profile_is_provider(candidate, XR_PROVIDER_IO_CONTRACT_KEY)) {
            REQUIRE(result == NULL);
            result = candidate;
        }
    }
    return result;
}

static void build_output_binding(const XrTargetProfile *profile, OutputCapture *capture,
                                 TestProviderBindings *bindings) {
    memset(bindings, 0, sizeof(*bindings));
    const XrTargetProviderContract *contract = output_contract(profile);
    REQUIRE(contract && contract->operation_count == 1u);
    bindings->count = 1u;
    bindings->providers[0].contract_id = contract->contract_id;
    REQUIRE(xr_target_provider_contract_fingerprint(
                contract, &bindings->providers[0].contract_fingerprint) == XR_RUNTIME_ABI_OK);
    bindings->providers[0].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
    bindings->providers[0].operations = bindings->operations[0];
    bindings->providers[0].operation_count = 1u;
    bindings->operations[0][0].operation_id = contract->operations[0].stable_id;
    bindings->operations[0][0].trampoline_kind = XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE;
    bindings->operations[0][0].entry.output_write = capture_output_write;
    bindings->operations[0][0].context = capture;
}

static bool reference_output_write(void *context, uint32_t requirement_index,
                                   uint32_t operation_index, const uint8_t *bytes, size_t size) {
    return xr_execution_lease_provider_output_write(context, requirement_index, operation_index,
                                                    bytes, size) == XR_EXECUTION_PROVIDER_CALL_OK;
}

/* The typed text family runs identically on the reference evaluator, the
 * validated graph view, and every executor writes the exact
 * canonical byte stream through the same provider sink. */
static void test_text_differential(void) {
    XrProgramArtifact artifact = {0};
    char build_diagnostic[256] = {0};
    REQUIRE(xr_program_text_fixture_write(&artifact, build_diagnostic, sizeof(build_diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    OutputCapture capture = {0};
    TestProviderBindings bindings;
    build_output_binding(profile, &capture, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrVmCodeOptions baseline_options = xr_vm_code_default_options();

    XrVmCode *baseline = NULL;

    XrVmCodeDiagnostic vm_diagnostic;
    REQUIRE(xr_vm_code_build(program, profile, &baseline_options, &baseline, &vm_diagnostic) ==
            XR_VM_CODE_OK);

    XrExecutionLease lease = {0};
    REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
    XrReferenceProviderBinding reference_binding = {
        .context = &lease,
        .output_write = reference_output_write,
    };
    uint32_t entry = xr_validated_program_entry_function(program);
    XrReferenceOutcome reference =
        xr_reference_evaluate_bound(program, entry, NULL, 0u, NULL, NULL, &reference_binding);
    XrVmOutcome baseline_result = xr_vm_code_execute(baseline, instance, entry, NULL, 0u);

    compare_outcomes(reference, baseline_result);

    REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RETURN);
    REQUIRE(reference.value.kind == XR_REFERENCE_VALUE_I64);
    REQUIRE(reference.value.as.i64 == 0);
    size_t stream_size = sizeof(XR_PROGRAM_TEXT_FIXTURE_STDOUT) - 1u;
    REQUIRE(capture.calls == 6u);
    REQUIRE(capture.size == stream_size * 2u);
    for (size_t copy = 0u; copy < 2u; ++copy)
        REQUIRE(memcmp(capture.bytes + copy * stream_size, XR_PROGRAM_TEXT_FIXTURE_STDOUT,
                       stream_size) == 0);

    /* A refused sink traps every executor at the first group with nothing
     * written and no owner left behind for the outcome to carry. */
    capture = (OutputCapture) {.fail = true};
    reference =
        xr_reference_evaluate_bound(program, entry, NULL, 0u, NULL, NULL, &reference_binding);
    baseline_result = xr_vm_code_execute(baseline, instance, entry, NULL, 0u);

    compare_outcomes(reference, baseline_result);

    REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_TRAP);
    REQUIRE(reference.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
    REQUIRE(capture.calls == 2u);
    REQUIRE(capture.size == 0u);

    REQUIRE(xr_execution_lease_release(&lease));

    xr_vm_code_free(baseline);
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_provider_output_differential(void) {
    static const struct {
        int64_t value;
        const char *line;
    } cases[] = {
        {0, "0\n"},
        {42, "42\n"},
        {-42, "-42\n"},
        {INT64_MIN, "-9223372036854775808\n"},
        {INT64_MAX, "9223372036854775807\n"},
    };
    for (size_t case_index = 0u; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        XrProgramArtifact artifact = {0};
        char build_diagnostic[256] = {0};
        REQUIRE(xr_program_output_fixture_write(cases[case_index].value, &artifact,
                                                build_diagnostic,
                                                sizeof(build_diagnostic)) == XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *program = NULL;
        XrProgramDiagnostic verify_diagnostic;
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                    &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        XrTargetProfile *profile =
            xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        OutputCapture capture = {0};
        TestProviderBindings bindings;
        build_output_binding(profile, &capture, &bindings);
        XrInstance *instance = create_instance(program, profile, &bindings, 1u);
        XrVmCodeOptions baseline_options = xr_vm_code_default_options();

        XrVmCode *baseline = NULL;

        XrVmCodeDiagnostic vm_diagnostic;
        REQUIRE(xr_vm_code_build(program, profile, &baseline_options, &baseline, &vm_diagnostic) ==
                XR_VM_CODE_OK);

        XrExecutionLease lease = {0};
        REQUIRE(xr_execution_instance_acquire(instance, &lease) == XR_EXECUTION_OK);
        XrReferenceProviderBinding reference_binding = {
            .context = &lease,
            .output_write = reference_output_write,
        };
        uint32_t entry = xr_validated_program_entry_function(program);
        XrReferenceOutcome reference =
            xr_reference_evaluate_bound(program, entry, NULL, 0u, NULL, NULL, &reference_binding);
        XrVmOutcome baseline_result = xr_vm_code_execute(baseline, instance, entry, NULL, 0u);

        compare_outcomes(reference, baseline_result);

        REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RETURN);
        REQUIRE(reference.value.kind == XR_REFERENCE_VALUE_I64);
        REQUIRE(reference.value.as.i64 == 0);
        size_t line_size = strlen(cases[case_index].line);
        REQUIRE(capture.calls == 2u);
        REQUIRE(capture.size == line_size * 2u);
        for (size_t copy = 0u; copy < 2u; ++copy)
            REQUIRE(memcmp(capture.bytes + copy * line_size, cases[case_index].line, line_size) ==
                    0);

        capture = (OutputCapture) {.fail = true};
        reference =
            xr_reference_evaluate_bound(program, entry, NULL, 0u, NULL, NULL, &reference_binding);
        baseline_result = xr_vm_code_execute(baseline, instance, entry, NULL, 0u);

        compare_outcomes(reference, baseline_result);

        REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_TRAP);
        REQUIRE(reference.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
        REQUIRE(capture.calls == 2u);
        REQUIRE(capture.size == 0u);

        REQUIRE(xr_execution_lease_release(&lease));

        xr_vm_code_free(baseline);
        retire_and_free(&instance);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
    }
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

static void test_existential_pack_test_project(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_existential_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    run_program(program, false, NULL, NULL, 0u, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64, 42u);
    xr_validated_program_free(program);
}

static void test_existential_owned_read_reborrow(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_reborrow_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    run_program(program, false, NULL, NULL, 0u, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64, 42u);
    xr_validated_program_free(program);
}

static void test_callable_pack_and_indirect_calls(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_callable_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    run_program(program, false, NULL, NULL, 0u, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64, 42u);
    xr_validated_program_free(program);
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
        {.kind = XR_VM_VALUE_PANIC_INFO, .as.panic_info.code = 91u},
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

static void test_condition_assert_panic_cleanup_cfg(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_assert_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    XrReferenceValue reference_arguments[] = {
        {.kind = XR_REFERENCE_VALUE_BOOL, .as.boolean = true},
        {.kind = XR_REFERENCE_VALUE_I64, .as.i64 = 9},
    };
    XrVmValue vm_arguments[] = {
        {.kind = XR_VM_VALUE_BOOL, .as.boolean = true},
        {.kind = XR_VM_VALUE_I64, .as.i64 = 9},
    };
    run_program(program, false, reference_arguments, vm_arguments, 2u, XR_VM_OUTCOME_RETURN,
                XR_VM_VALUE_I64, 42u);
    reference_arguments[0].as.boolean = false;
    vm_arguments[0].as.boolean = false;
    run_program(program, false, reference_arguments, vm_arguments, 2u, XR_VM_OUTCOME_PANIC,
                XR_VM_VALUE_VOID, XR_ASSERTION_FAILURE_CONDITION_FALSE);
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
    REQUIRE(xr_vm_code_build(aggregate, aggregate_profile, &aggregate_budget, &aggregate_code,
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
    run_program(control, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_U16, 64u);
    run_program(control, true, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_U16, 32u);

    XrValidatedProgram *operating_system =
        build_target_query_program(XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM, XR_CORE_TYPE_TARGET_OS,
                                   XR_CORE_CAPABILITY_PROFILE_OPERATING_SYSTEM);
    run_program(operating_system, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_TARGET_OS,
                XR_TARGET_OS_WINDOWS);
    run_program(operating_system, true, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_TARGET_OS,
                XR_TARGET_OS_WASI);
    XrValidatedProgram *architecture =
        build_target_query_program(XR_CORE_OP_CORE_TARGET_ARCHITECTURE, XR_CORE_TYPE_TARGET_ARCH,
                                   XR_CORE_CAPABILITY_PROFILE_ARCHITECTURE);
    run_program(architecture, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_TARGET_ARCH,
                XR_TARGET_ARCH_X86_64);
    run_program(architecture, true, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_TARGET_ARCH,
                XR_TARGET_ARCH_WASM32);
    XrValidatedProgram *native_abi =
        build_target_query_program(XR_CORE_OP_CORE_TARGET_NATIVE_ABI, XR_CORE_TYPE_TARGET_ABI,
                                   XR_CORE_CAPABILITY_PROFILE_NATIVE_ABI);
    run_program(native_abi, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_TARGET_ABI,
                XR_TARGET_ABI_WIN64_X86_64);
    run_program(native_abi, true, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_TARGET_ABI,
                XR_TARGET_ABI_WASM);
    XrValidatedProgram *endianness =
        build_target_query_program(XR_CORE_OP_CORE_TARGET_ENDIANNESS, XR_CORE_TYPE_TARGET_ENDIAN,
                                   XR_CORE_CAPABILITY_PROFILE_ENDIANNESS);
    run_program(endianness, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_TARGET_ENDIAN,
                XR_TARGET_ENDIAN_LITTLE);
    run_program(endianness, true, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_TARGET_ENDIAN,
                XR_TARGET_ENDIAN_LITTLE);

    XrValidatedProgram *target_enum_equality = build_target_enum_equality_program();
    run_program(target_enum_equality, false, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_BOOL,
                0u);
    run_program(target_enum_equality, true, NULL, NULL, 0, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_BOOL,
                1u);

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
    xr_validated_program_free(target_enum_equality);
    xr_validated_program_free(endianness);
    xr_validated_program_free(native_abi);
    xr_validated_program_free(architecture);
    xr_validated_program_free(operating_system);
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
        XrVmCode *held = xr_vm_code_retain(race->code);
        REQUIRE(held != NULL);
        XrVmOutcome result = xr_vm_code_execute(held, race->instance, race->entry, NULL, 0u);
        xr_vm_code_free(held);
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
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, &code_diagnostic) == XR_VM_CODE_OK);
    VmRace race = {
        .code = code,
        .instance = instance,
        .entry = xr_validated_program_entry_function(program),
    };
    atomic_init(&race.start, false);
    atomic_init(&race.drain_started, false);
    atomic_init(&race.returned, 0u);
    atomic_init(&race.stale, 0u);
    atomic_init(&race.invalid, 0u);
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
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    xr_vm_code_free(code);
    REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) == XR_EXECUTION_OK);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_policy_budget_generation_and_identity(void) {
    XrValidatedProgram *program = build_scalar_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrVmCodeDiagnostic code_diagnostic;
    XrVmCodeOptions rejected = xr_vm_code_default_options();
    rejected.quickening_policy = 1u;
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, &rejected, &code, &code_diagnostic) ==
            XR_VM_CODE_POLICY_REJECTED);
    REQUIRE(code == NULL);
    REQUIRE(xr_vm_code_build(NULL, profile, NULL, &code, &code_diagnostic) ==
            XR_VM_CODE_INVALID_INPUT);
    REQUIRE(code == NULL);
    REQUIRE(xr_vm_code_build(program, NULL, NULL, &code, &code_diagnostic) ==
            XR_VM_CODE_INVALID_INPUT);
    REQUIRE(code == NULL);

    for (unsigned invalid = 0u; invalid < 6u; ++invalid) {
        rejected = xr_vm_code_default_options();
        switch (invalid) {
            case 0u: rejected.schema_version = 1u; break;
            case 1u: rejected.reserved8 = 1u; break;
            case 2u: rejected.reserved16 = 1u; break;
            case 3u: rejected.max_steps = 0u; break;
            case 4u: rejected.max_value_cells = 0u; break;
            case 5u: rejected.max_call_depth = 0u; break;
        }
        REQUIRE(xr_vm_code_build(program, profile, &rejected, &code, &code_diagnostic) ==
                XR_VM_CODE_INVALID_INPUT);
        REQUIRE(code == NULL && code_diagnostic.status == XR_VM_CODE_INVALID_INPUT);
    }

    XrVmCodeOptions limited = xr_vm_code_default_options();
    limited.max_steps = 1u;
    REQUIRE(xr_vm_code_build(program, profile, &limited, &code, &code_diagnostic) == XR_VM_CODE_OK);
    XrInstance *instance = create_instance(program, profile, &bindings, 41u);
    uint32_t entry = xr_validated_program_entry_function(program);
    XrVmOutcome result = xr_vm_code_execute(code, instance, entry, NULL, 0);
    REQUIRE(result.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
    XrFingerprint limited_digest = xr_vm_code_private_digest(code);
    xr_vm_code_free(code);

    XrVmCodeOptions options = xr_vm_code_default_options();
    REQUIRE(xr_vm_code_build(program, profile, &options, &code, &code_diagnostic) == XR_VM_CODE_OK);
    REQUIRE(fingerprint_equal(limited_digest, xr_vm_code_private_digest(code)));
    result = xr_vm_code_execute(code, instance, entry, NULL, 0u);
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN && result.value.kind == XR_VM_VALUE_BOOL);
    REQUIRE(result.value.as.boolean);
    XrFingerprint trace = result.logical_trace;
    xr_vm_outcome_dispose(&result);
    result = xr_vm_code_execute(code, instance, entry, NULL, 0u);
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN && result.value.kind == XR_VM_VALUE_BOOL);
    REQUIRE(result.value.as.boolean && fingerprint_equal(trace, result.logical_trace));
    xr_vm_outcome_dispose(&result);

    XrExecutionDiagnostic execution_diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) == XR_EXECUTION_OK);
    XrInstance *successor = NULL;
    REQUIRE(xr_execution_instance_create_successor(
                instance, bindings.count ? bindings.providers : NULL, bindings.count, &successor,
                &execution_diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_vm_code_matches_instance(code, successor));
    result = xr_vm_code_execute(code, successor, entry, NULL, 0);
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
    REQUIRE(result.value.kind == XR_VM_VALUE_BOOL);
    REQUIRE(result.value.as.boolean);
    xr_vm_outcome_dispose(&result);
    result = xr_vm_code_execute(code, instance, entry, NULL, 0);
    REQUIRE(result.kind == XR_VM_OUTCOME_STALE_CODE);
    XrVmCode *rebuilt = NULL;
    REQUIRE(xr_vm_code_build(program, profile, &options, &rebuilt, &code_diagnostic) ==
            XR_VM_CODE_OK);
    REQUIRE(fingerprint_equal(xr_vm_code_private_digest(code), xr_vm_code_private_digest(rebuilt)));
    xr_vm_code_free(rebuilt);
    XrTargetProfile *foreign_profile =
        xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(foreign_profile != NULL);
    XrVmCode *foreign_code = NULL;
    REQUIRE(xr_vm_code_build(program, foreign_profile, &options, &foreign_code, &code_diagnostic) == XR_VM_CODE_OK);
    REQUIRE(!fingerprint_equal(xr_vm_code_private_digest(code), xr_vm_code_private_digest(foreign_code)));
    xr_vm_code_free(foreign_code);
    TestProviderBindings foreign_bindings;
    build_provider_bindings(foreign_profile, &foreign_bindings);
    XrInstance *foreign = create_instance(program, foreign_profile, &foreign_bindings, 42u);
    REQUIRE(!xr_vm_code_matches_instance(code, foreign));
    result = xr_vm_code_execute(code, foreign, entry, NULL, 0);
    REQUIRE(result.kind == XR_VM_OUTCOME_STALE_CODE);
    retire_and_free(&foreign);
    xr_target_profile_free(foreign_profile);
    xr_vm_code_free(code);
    REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) == XR_EXECUTION_OK);
    retire_and_free(&successor);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

typedef struct ClassLifecycleLog {
    XrReferenceLifecycleEvent reference[32];
    XrVmLifecycleEvent vm[32];
    uint32_t reference_count;
    uint32_t vm_count;
} ClassLifecycleLog;

static void record_reference_class_lifecycle(void *context,
                                             const XrReferenceLifecycleEvent *event) {
    ClassLifecycleLog *log = context;
    REQUIRE(log != NULL && event != NULL && log->reference_count < 32u);
    log->reference[log->reference_count++] = *event;
}

static void record_vm_class_lifecycle(void *context, const XrVmLifecycleEvent *event) {
    ClassLifecycleLog *log = context;
    REQUIRE(log != NULL && event != NULL && log->vm_count < 32u);
    log->vm[log->vm_count++] = *event;
}

static void require_class_alias_vm_oracle(const ClassLifecycleLog *log, XrVmOutcome result) {
    static const XrVmLifecycleEventKind expected[] = {
        XR_VM_EVENT_CLASS_CONSTRUCT, XR_VM_EVENT_CLASS_SHARE,      XR_VM_EVENT_CLASS_FIELD_PLACE,
        XR_VM_EVENT_PLACE_EXCHANGE,  XR_VM_EVENT_CLASS_FIELD_LOAD, XR_VM_EVENT_OWNER_DROP,
        XR_VM_EVENT_OWNER_DROP,      XR_VM_EVENT_CLASS_FINALIZE,   XR_VM_EVENT_CLASS_RECLAIM,
    };
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
    REQUIRE(result.value.kind == XR_VM_VALUE_I64);
    REQUIRE(result.value.as.i64 == 42);
    REQUIRE(log->vm_count == sizeof(expected) / sizeof(expected[0]));
    uint64_t identity = log->vm[0].identity;
    REQUIRE(identity != UINT64_MAX);
    for (uint32_t index = 0u; index < log->vm_count; ++index) {
        REQUIRE(log->vm[index].kind == expected[index]);
        REQUIRE(log->vm[index].origin == XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION);
        if (index != 3u)
            REQUIRE(log->vm[index].identity == identity);
    }
    REQUIRE(log->vm[1].related_identity == identity);
    REQUIRE(log->vm[2].field_ordinal == 0u);
    REQUIRE(log->vm[3].type_id == XR_CORE_TYPE_I64);
    REQUIRE(log->vm[3].identity == UINT64_MAX);
    REQUIRE(log->vm[3].related_identity == UINT64_MAX);
    REQUIRE(log->vm[3].previous_value_kind == XR_VM_VALUE_I64);
    REQUIRE(log->vm[3].replacement_value_kind == XR_VM_VALUE_I64);
    REQUIRE(log->vm[3].previous_i64 == 7);
    REQUIRE(log->vm[3].replacement_i64 == 42);
    REQUIRE(log->vm[4].field_ordinal == 0u);
}

static void require_class_alias_reference_oracle(const ClassLifecycleLog *log,
                                                 XrReferenceOutcome result) {
    static const XrReferenceLifecycleEventKind expected[] = {
        XR_REFERENCE_EVENT_CLASS_CONSTRUCT,   XR_REFERENCE_EVENT_CLASS_SHARE,
        XR_REFERENCE_EVENT_CLASS_FIELD_PLACE, XR_REFERENCE_EVENT_PLACE_EXCHANGE,
        XR_REFERENCE_EVENT_CLASS_FIELD_LOAD,  XR_REFERENCE_EVENT_OWNER_DROP,
        XR_REFERENCE_EVENT_OWNER_DROP,        XR_REFERENCE_EVENT_CLASS_FINALIZE,
        XR_REFERENCE_EVENT_CLASS_RECLAIM,
    };
    REQUIRE(result.kind == XR_REFERENCE_OUTCOME_RETURN);
    REQUIRE(result.value.kind == XR_REFERENCE_VALUE_I64);
    REQUIRE(result.value.as.i64 == 42);
    REQUIRE(log->reference_count == sizeof(expected) / sizeof(expected[0]));
    uint64_t identity = log->reference[0].identity;
    REQUIRE(identity != UINT64_MAX);
    for (uint32_t index = 0u; index < log->reference_count; ++index) {
        REQUIRE(log->reference[index].kind == expected[index]);
        REQUIRE(log->reference[index].origin == XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION);
        if (index != 3u)
            REQUIRE(log->reference[index].identity == identity);
    }
    REQUIRE(log->reference[1].related_identity == identity);
    REQUIRE(log->reference[2].field_ordinal == 0u);
    REQUIRE(log->reference[3].type_id == XR_CORE_TYPE_I64);
    REQUIRE(log->reference[3].identity == UINT64_MAX);
    REQUIRE(log->reference[3].related_identity == UINT64_MAX);
    REQUIRE(log->reference[4].field_ordinal == 0u);
}

static XrVmOutcome execute_class_alias_vm(XrValidatedProgram *program, XrInstance *instance,
                                          const XrTargetProfile *profile,
                                          ClassLifecycleLog *log) {
    XrVmCodeOptions options = xr_vm_code_default_options();
    options.lifecycle_context = log;
    options.lifecycle_event = record_vm_class_lifecycle;
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    REQUIRE(xr_vm_code_build(program, profile, &options, &code, &diagnostic) == XR_VM_CODE_OK);
    XrVmOutcome result =
        xr_vm_code_execute(code, instance, xr_validated_program_entry_function(program), NULL, 0u);
    xr_vm_code_free(code);
    return result;
}

static void test_record_reference_lifecycle(void) {
    XrValidatedProgram *program = build_class_alias_mutation_program(true);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    ClassLifecycleLog log = {0};
    XrVmOutcome result = execute_class_alias_vm(program, instance, profile, &log);
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
    REQUIRE(result.value.kind == XR_VM_VALUE_I64);
    require_class_alias_vm_oracle(&log, result);
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_class_reference_semantics_differential(void) {

    XrValidatedProgram *program = build_class_alias_mutation_program(false);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    ClassLifecycleLog reference_log = {0};
    XrReferenceProviderBinding reference_binding = {
        .lifecycle_context = &reference_log,
        .lifecycle_event = record_reference_class_lifecycle,
    };
    XrReferenceOutcome reference =
        xr_reference_evaluate_bound(program, xr_validated_program_entry_function(program), NULL, 0u,
                                    NULL, NULL, &reference_binding);
    require_class_alias_reference_oracle(&reference_log, reference);
    xr_reference_outcome_dispose(&reference);
    {
        ClassLifecycleLog log = {0};
        XrVmOutcome result =
            execute_class_alias_vm(program, instance, profile, &log);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
        REQUIRE(result.value.kind == XR_VM_VALUE_I64);
        require_class_alias_vm_oracle(&log, result);
        REQUIRE(log.vm_count == reference_log.reference_count);
        for (uint32_t event = 0u; event < log.vm_count; ++event) {
            REQUIRE((uint32_t) log.vm[event].kind ==
                    (uint32_t) reference_log.reference[event].kind);
            REQUIRE((uint32_t) log.vm[event].origin ==
                    (uint32_t) reference_log.reference[event].origin);
            REQUIRE(log.vm[event].type_id == reference_log.reference[event].type_id);
            REQUIRE(log.vm[event].field_ordinal == reference_log.reference[event].field_ordinal);
            REQUIRE(log.vm[event].identity == reference_log.reference[event].identity);
            REQUIRE(log.vm[event].related_identity ==
                    reference_log.reference[event].related_identity);
        }
    }
    static const char probe_argument[] = "--h2-class-differential";
    REQUIRE(probe_argument[0] == '-');
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_class_ref_receiver_direct_call(void) {

    XrValidatedProgram *program = build_class_ref_receiver_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 2u);
    {
        ClassLifecycleLog log = {0};
        XrVmOutcome result =
            execute_class_alias_vm(program, instance, profile, &log);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
        REQUIRE(result.value.kind == XR_VM_VALUE_I64);
        REQUIRE(result.value.as.i64 == 42);
        REQUIRE(log.vm_count == 5u);
        REQUIRE(log.vm[0].kind == XR_VM_EVENT_CLASS_CONSTRUCT);
        REQUIRE(log.vm[1].kind == XR_VM_EVENT_CLASS_FIELD_LOAD);
        REQUIRE(log.vm[2].kind == XR_VM_EVENT_OWNER_DROP);
        REQUIRE(log.vm[3].kind == XR_VM_EVENT_CLASS_FINALIZE);
        REQUIRE(log.vm[4].kind == XR_VM_EVENT_CLASS_RECLAIM);
    }
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_class_owned_exchange_and_self_assignment(void) {

    for (uint32_t self_assignment = 0u; self_assignment != 2u; ++self_assignment) {
        XrValidatedProgram *program = build_class_owned_exchange_program(self_assignment != 0u);
        XrTargetProfile *profile =
            xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(program != NULL && profile != NULL);
        TestProviderBindings bindings;
        build_provider_bindings(profile, &bindings);
        XrInstance *instance = create_instance(program, profile, &bindings, 1u);
        ClassLifecycleLog reference_log = {0};
        XrReferenceProviderBinding reference_binding = {
            .lifecycle_context = &reference_log,
            .lifecycle_event = record_reference_class_lifecycle,
        };
        XrReferenceOutcome reference =
            xr_reference_evaluate_bound(program, xr_validated_program_entry_function(program), NULL,
                                        0u, NULL, NULL, &reference_binding);
        REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RETURN);
        REQUIRE(reference.value.kind == XR_REFERENCE_VALUE_I64);
        REQUIRE(reference.value.as.i64 == (self_assignment ? 7 : 42));
        uint32_t exchange = UINT32_MAX;
        for (uint32_t event = 0u; event < reference_log.reference_count; ++event)
            if (reference_log.reference[event].kind == XR_REFERENCE_EVENT_PLACE_EXCHANGE)
                exchange = event;
        REQUIRE(exchange != UINT32_MAX);
        REQUIRE(reference_log.reference[exchange].type_id != XR_CORE_TYPE_VOID);
        REQUIRE(reference_log.reference[exchange].identity != UINT64_MAX);
        REQUIRE(reference_log.reference[exchange].related_identity != UINT64_MAX);
        REQUIRE((reference_log.reference[exchange].identity ==
                 reference_log.reference[exchange].related_identity) == (self_assignment != 0u));
        REQUIRE(reference_log.reference[exchange + 1u].kind == XR_REFERENCE_EVENT_OWNER_DROP);
        if (self_assignment)
            REQUIRE(reference_log.reference[exchange + 2u].kind ==
                    XR_REFERENCE_EVENT_CLASS_FIELD_LOAD);
        else {
            REQUIRE(reference_log.reference[exchange + 2u].kind ==
                    XR_REFERENCE_EVENT_CLASS_FINALIZE);
            REQUIRE(reference_log.reference[exchange + 3u].kind ==
                    XR_REFERENCE_EVENT_CLASS_RECLAIM);
        }
        {
            ClassLifecycleLog log = {0};
            XrVmOutcome result =
                execute_class_alias_vm(program, instance, profile, &log);
            REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
            REQUIRE(result.value.kind == XR_VM_VALUE_I64);
            REQUIRE(result.value.as.i64 == (self_assignment ? 7 : 42));
            REQUIRE(log.vm_count == reference_log.reference_count);
            for (uint32_t event = 0u; event < log.vm_count; ++event) {
                REQUIRE((uint32_t) log.vm[event].kind ==
                        (uint32_t) reference_log.reference[event].kind);
                REQUIRE((uint32_t) log.vm[event].origin ==
                        (uint32_t) reference_log.reference[event].origin);
                REQUIRE(log.vm[event].type_id == reference_log.reference[event].type_id);
                REQUIRE(log.vm[event].field_ordinal ==
                        reference_log.reference[event].field_ordinal);
                REQUIRE(log.vm[event].identity == reference_log.reference[event].identity);
                REQUIRE(log.vm[event].related_identity ==
                        reference_log.reference[event].related_identity);
            }
        }
        xr_reference_outcome_dispose(&reference);
        retire_and_free(&instance);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
    }
}

static void test_class_trivial_field_load_is_snapshot_and_bounded(void) {
    XrValidatedProgram *program = build_class_trivial_snapshot_program();
    run_program(program, false, NULL, NULL, 0u, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64, 7u);
    XrReferenceBudget reference_budget = xr_reference_default_budget();
    reference_budget.max_value_cells = 2u;
    XrReferenceOutcome reference = xr_reference_evaluate(
        program, xr_validated_program_entry_function(program), NULL, 0u, NULL, &reference_budget);
    REQUIRE(reference.kind == XR_REFERENCE_OUTCOME_RESOURCE_LIMIT);
    xr_reference_outcome_dispose(&reference);

    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.max_value_cells = 2u;
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic diagnostic;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, &diagnostic) == XR_VM_CODE_OK);
        XrVmOutcome result = xr_vm_code_execute(
            code, instance, xr_validated_program_entry_function(program), NULL, 0u);
        REQUIRE(result.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
        xr_vm_code_free(code);
    }
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_class_error_outcomes_retain_owners(void) {

    for (uint32_t aggregate_error = 0u; aggregate_error != 2u; ++aggregate_error) {
        XrValidatedProgram *program = build_propagated_class_error_program(aggregate_error != 0u);
        XrTargetProfile *profile =
            xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(program != NULL && profile != NULL);
        TestProviderBindings bindings;
        build_provider_bindings(profile, &bindings);
        XrInstance *instance = create_instance(program, profile, &bindings, 1u);
        {
            ClassLifecycleLog log = {0};
            XrVmCodeOptions options = xr_vm_code_default_options();
            options.lifecycle_context = &log;
            options.lifecycle_event = record_vm_class_lifecycle;
            XrVmCode *code = NULL;
            XrVmCodeDiagnostic diagnostic;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, &diagnostic) ==
                    XR_VM_CODE_OK);
            XrVmOutcome result = xr_vm_code_execute(
                code, instance, xr_validated_program_entry_function(program), NULL, 0u);
            REQUIRE(result.kind == XR_VM_OUTCOME_ERROR);
            REQUIRE(result.error_value.kind == (aggregate_error ? XR_VM_VALUE_AGGREGATE
                                                               : XR_VM_VALUE_CLASS_REFERENCE));
            REQUIRE(result.owns_dynamic_values && result.private_owner != NULL);
            REQUIRE(log.vm_count == 1u);
            REQUIRE(log.vm[0].kind == XR_VM_EVENT_CLASS_CONSTRUCT);
            xr_vm_code_free(code);
            REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
            if (aggregate_error) {
                XrVmAggregateView view;
                REQUIRE(xr_vm_value_aggregate_view(&result.error_value, &view));
                REQUIRE(view.field_count == 1u);
                REQUIRE(view.fields[0].kind == XR_VM_VALUE_CLASS_REFERENCE);
            }
            xr_vm_outcome_dispose(&result);
            REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
            REQUIRE(log.vm_count == 4u);
            REQUIRE(log.vm[1].kind == XR_VM_EVENT_OWNER_DROP);
            REQUIRE(log.vm[2].kind == XR_VM_EVENT_CLASS_FINALIZE);
            REQUIRE(log.vm[3].kind == XR_VM_EVENT_CLASS_RECLAIM);
            log = (ClassLifecycleLog) {0};
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, &diagnostic) == XR_VM_CODE_OK);
            XrVmExecution *execution = NULL;
            REQUIRE(xr_vm_execution_create(code, instance,
                xr_validated_program_entry_function(program), NULL, 0u, &execution));
            result = xr_vm_execution_step(execution);
            REQUIRE(result.kind == XR_VM_OUTCOME_ERROR);
            REQUIRE(result.error_value.kind == (aggregate_error ? XR_VM_VALUE_AGGREGATE
                                                               : XR_VM_VALUE_CLASS_REFERENCE));
            REQUIRE(!result.owns_dynamic_values && !result.private_owner);
            xr_vm_code_free(code);
            REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
            REQUIRE(log.vm_count == 1u);
            xr_vm_outcome_dispose(&result);
            REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
            xr_vm_execution_free(execution);
            REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
            REQUIRE(log.vm_count == 4u);
            REQUIRE(log.vm[3].kind == XR_VM_EVENT_CLASS_RECLAIM);
        }
        retire_and_free(&instance);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
    }
}

static void test_coroutine_arithmetic_uses_ordinary_operation_semantics(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings = {0};
    for (unsigned zero = 0u; zero < 2u; ++zero) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        char diagnostic[256] = {0};
        REQUIRE(xr_program_coroutine_fixture_write_mutated(
                    zero ? XR_PROGRAM_COROUTINE_FIXTURE_DIVIDE_BY_ZERO_AFTER_YIELD
                         : XR_PROGRAM_COROUTINE_FIXTURE_DIVIDE_AFTER_YIELD,
                    &artifact, diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
                XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        {
            XrVmCodeOptions options = xr_vm_code_default_options();
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            XrInstance *instance = create_instance(program, profile, &bindings, 1u);
            XrVmExecution *execution = NULL;
            REQUIRE(xr_vm_execution_create(code, instance, 0u, NULL, 0u, &execution));
            REQUIRE(xr_vm_execution_step(execution).kind == XR_VM_OUTCOME_SUSPENDED);
            XrVmOutcome result = xr_vm_execution_step(execution);
            if (zero) {
                REQUIRE(result.kind == XR_VM_OUTCOME_TRAP);
                REQUIRE(result.trap == XR_VM_TRAP_INTEGER_DIVISION_BY_ZERO);
            } else {
                REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
                REQUIRE(result.value.kind == XR_VM_VALUE_I64 && result.value.as.i64 == 20);
            }
            REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
            REQUIRE(xr_vm_execution_step(execution).kind == XR_VM_OUTCOME_INVALID_INVOCATION);
            xr_vm_execution_free(execution);
            xr_vm_code_free(code);
            retire_and_free(&instance);
        }
        xr_validated_program_free(program);
    }
    xr_target_profile_free(profile);
}

static void test_coroutine_suspend_resume_generation_lease(void) {
    XrValidatedProgram *program = build_coroutine_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);

    XrInstance *reference_instance = create_instance(program, profile, &bindings, 101u);
    XrReferenceExecution *reference = NULL;
    REQUIRE(xr_reference_execution_create(reference_instance, 0u, NULL, 0u, NULL, &reference));
    REQUIRE(xr_execution_instance_lease_count(reference_instance) == 1u);
    XrReferenceOutcome reference_yield = xr_reference_execution_step(reference);
    REQUIRE(reference_yield.kind == XR_REFERENCE_OUTCOME_SUSPENDED);
    REQUIRE(reference_yield.state_id == 1u && reference_yield.safepoint_id == 0u);
    XrExecutionDiagnostic execution_diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(reference_instance, &execution_diagnostic) ==
            XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(reference_instance, &execution_diagnostic) ==
            XR_EXECUTION_GENERATION_REJECTED);
    REQUIRE(execution_diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY);
    XrReferenceOutcome reference_return = xr_reference_execution_step(reference);
    REQUIRE(reference_return.kind == XR_REFERENCE_OUTCOME_RETURN);
    REQUIRE(reference_return.value.kind == XR_REFERENCE_VALUE_I64);
    REQUIRE(reference_return.value.as.i64 == 42);
    REQUIRE(xr_execution_instance_lease_count(reference_instance) == 0u);
    REQUIRE(xr_execution_instance_retire(reference_instance, &execution_diagnostic) ==
            XR_EXECUTION_OK);
    XrReferenceExecution *stale_reference = NULL;
    REQUIRE(
        !xr_reference_execution_create(reference_instance, 0u, NULL, 0u, NULL, &stale_reference));
    xr_reference_execution_free(reference);
    XrInstance *reference_successor = NULL;
    REQUIRE(xr_execution_instance_create_successor(reference_instance, NULL, 0u,
                                                   &reference_successor,
                                                   &execution_diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&reference_instance, &execution_diagnostic) ==
            XR_EXECUTION_OK);
    retire_and_free(&reference_successor);

    XrInstance *reference_cancel_instance = create_instance(program, profile, &bindings, 102u);
    XrReferenceExecution *reference_cancel = NULL;
    REQUIRE(xr_reference_execution_create(reference_cancel_instance, 0u, NULL, 0u, NULL,
                                          &reference_cancel));
    REQUIRE(xr_reference_execution_cancel(reference_cancel).kind ==
            XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
    XrReferenceOutcome reference_cancel_yield = xr_reference_execution_step(reference_cancel);
    REQUIRE(reference_cancel_yield.kind == XR_REFERENCE_OUTCOME_SUSPENDED);
    XrReferenceOutcome reference_cancelled = xr_reference_execution_cancel(reference_cancel);
    REQUIRE(reference_cancelled.kind == XR_REFERENCE_OUTCOME_CANCELLED);
    REQUIRE(reference_cancelled.state_id == reference_cancel_yield.state_id);
    REQUIRE(reference_cancelled.steps == reference_cancel_yield.steps + 1u);
    REQUIRE(xr_reference_execution_step(reference_cancel).kind ==
            XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
    REQUIRE(xr_execution_instance_lease_count(reference_cancel_instance) == 0u);
    xr_reference_execution_free(reference_cancel);
    retire_and_free(&reference_cancel_instance);

    XrFingerprint traces[1] = {{{0}}};

    {
        XrInstance *instance = create_instance(program, profile, &bindings, 201u + 0u);
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic code_diagnostic;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, &code_diagnostic) ==
                XR_VM_CODE_OK);
        XrVmOutcome one_shot = xr_vm_code_execute(code, instance, 0u, NULL, 0u);
        REQUIRE(one_shot.kind == XR_VM_OUTCOME_INVALID_INVOCATION);
        XrVmExecution *execution = NULL;
        REQUIRE(xr_vm_execution_create(code, instance, 0u, NULL, 0u, &execution));
        XrVmCode *failed_create_code = xr_vm_code_retain(code);
        REQUIRE(failed_create_code != NULL);
        XrVmOutcome yielded = xr_vm_execution_step(execution);
        REQUIRE(yielded.kind == XR_VM_OUTCOME_SUSPENDED);
        REQUIRE(yielded.state_id == 1u && yielded.safepoint_id == 0u);
        xr_vm_code_free(code);
        code = NULL;
        REQUIRE(xr_execution_instance_begin_drain(instance, &execution_diagnostic) ==
                XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) ==
                XR_EXECUTION_GENERATION_REJECTED);
        REQUIRE(execution_diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY);
        XrVmExecution *rejected_execution = NULL;
        REQUIRE(!xr_vm_execution_create(failed_create_code, instance, 0u, NULL, 0u,
                                        &rejected_execution));
        REQUIRE(rejected_execution == NULL);
        REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
        xr_vm_code_free(failed_create_code);
        XrVmOutcome returned = xr_vm_execution_step(execution);
        REQUIRE(returned.kind == XR_VM_OUTCOME_RETURN);
        REQUIRE(returned.value.kind == XR_VM_VALUE_I64 && returned.value.as.i64 == 42);
        traces[0u] = returned.logical_trace;
        REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
        REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) == XR_EXECUTION_OK);
        xr_vm_execution_free(execution);
        REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) == XR_EXECUTION_OK);

        XrInstance *cancel_instance =
            create_instance(program, profile, &bindings, 301u + 0u);
        XrVmCode *cancel_code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &cancel_code, &code_diagnostic) ==
                XR_VM_CODE_OK);
        XrVmExecution *cancel_execution = NULL;
        REQUIRE(
            xr_vm_execution_create(cancel_code, cancel_instance, 0u, NULL, 0u, &cancel_execution));
        REQUIRE(xr_vm_execution_cancel(cancel_execution).kind == XR_VM_OUTCOME_INVALID_INVOCATION);
        XrVmOutcome cancel_yield = xr_vm_execution_step(cancel_execution);
        REQUIRE(cancel_yield.kind == XR_VM_OUTCOME_SUSPENDED);
        XrVmOutcome cancelled = xr_vm_execution_cancel(cancel_execution);
        REQUIRE(cancelled.kind == XR_VM_OUTCOME_CANCELLED);
        REQUIRE(cancelled.state_id == cancel_yield.state_id);
        REQUIRE(cancelled.steps == cancel_yield.steps + 1u);
        REQUIRE(xr_vm_execution_step(cancel_execution).kind == XR_VM_OUTCOME_INVALID_INVOCATION);
        REQUIRE(xr_execution_instance_lease_count(cancel_instance) == 0u);
        xr_vm_execution_free(cancel_execution);
        xr_vm_code_free(cancel_code);
        retire_and_free(&cancel_instance);
    }
    REQUIRE(!fingerprint_equal(traces[0], (XrFingerprint) {{0}}));
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_coroutine_cancel_drops_exact_live_owner(void) {
    XrValidatedProgram *program = build_owner_coroutine_program();
    REQUIRE(xr_validated_program_function_count(program) == 2u);
    uint32_t owner_function = 1u - xr_validated_program_entry_function(program);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);

    XrReferenceValue reference_argument = {
        .kind = XR_REFERENCE_VALUE_PANIC_INFO,
        .as.panic_info = 91u,
    };
    XrInstance *reference_instance = create_instance(program, profile, &bindings, 401u);
    XrReferenceExecution *reference = NULL;
    REQUIRE(xr_reference_execution_create(reference_instance, owner_function, &reference_argument,
                                          1u, NULL, &reference));
    XrReferenceOutcome reference_yield = xr_reference_execution_step(reference);
    REQUIRE(reference_yield.kind == XR_REFERENCE_OUTCOME_SUSPENDED);
    REQUIRE(reference_yield.steps == 2u);
    XrReferenceOutcome reference_return = xr_reference_execution_step(reference);
    REQUIRE(reference_return.kind == XR_REFERENCE_OUTCOME_RETURN);
    REQUIRE(reference_return.value.kind == XR_REFERENCE_VALUE_PANIC_INFO);
    REQUIRE(reference_return.value.as.panic_info == 91u);
    REQUIRE(reference_return.steps == 4u);
    xr_reference_execution_free(reference);
    retire_and_free(&reference_instance);

    reference_instance = create_instance(program, profile, &bindings, 402u);
    reference = NULL;
    REQUIRE(xr_reference_execution_create(reference_instance, owner_function, &reference_argument,
                                          1u, NULL, &reference));
    reference_yield = xr_reference_execution_step(reference);
    REQUIRE(reference_yield.kind == XR_REFERENCE_OUTCOME_SUSPENDED);
    XrReferenceOutcome reference_cancelled = xr_reference_execution_cancel(reference);
    REQUIRE(reference_cancelled.kind == XR_REFERENCE_OUTCOME_CANCELLED);
    REQUIRE(reference_cancelled.steps == 5u);
    xr_reference_execution_free(reference);
    retire_and_free(&reference_instance);

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmValue argument = {
            .kind = XR_VM_VALUE_PANIC_INFO,
            .as.panic_info.code = 91u,
        };
        XrVmCodeDiagnostic code_diagnostic;

        XrInstance *instance = create_instance(program, profile, &bindings, 501u + 0u);
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, &code_diagnostic) ==
                XR_VM_CODE_OK);
        XrVmExecution *execution = NULL;
        REQUIRE(xr_vm_execution_create(code, instance, owner_function, &argument, 1u, &execution));
        XrVmOutcome yielded = xr_vm_execution_step(execution);
        REQUIRE(yielded.kind == XR_VM_OUTCOME_SUSPENDED && yielded.steps == 2u);
        XrVmOutcome returned = xr_vm_execution_step(execution);
        REQUIRE(returned.kind == XR_VM_OUTCOME_RETURN && returned.steps == 4u);
        REQUIRE(returned.value.kind == XR_VM_VALUE_PANIC_INFO);
        REQUIRE(returned.value.as.panic_info.code == 91u);
        xr_vm_execution_free(execution);
        xr_vm_code_free(code);
        retire_and_free(&instance);

        instance = create_instance(program, profile, &bindings, 601u + 0u);
        code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, &code_diagnostic) ==
                XR_VM_CODE_OK);
        execution = NULL;
        REQUIRE(xr_vm_execution_create(code, instance, owner_function, &argument, 1u, &execution));
        yielded = xr_vm_execution_step(execution);
        REQUIRE(yielded.kind == XR_VM_OUTCOME_SUSPENDED && yielded.steps == 2u);
        XrVmOutcome cancelled = xr_vm_execution_cancel(execution);
        REQUIRE(cancelled.kind == XR_VM_OUTCOME_CANCELLED && cancelled.steps == 5u);
        xr_vm_execution_free(execution);
        xr_vm_code_free(code);
        retire_and_free(&instance);
    }
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

typedef enum CoroutineTrapExpected {
    COROUTINE_TRAP_RETURN,
    COROUTINE_TRAP_CANCELLED,
    COROUTINE_TRAP_PROVIDER_FAILED,
    COROUTINE_TRAP_EXPLICIT,
} CoroutineTrapExpected;

typedef struct CoroutineTrapCase {
    const char *name;
    XrProgramCoroutineTrapMutation mutation;
    bool cancel;
    bool refuse_child;
    CoroutineTrapExpected expected;
    uint64_t steps;
    uint32_t event_count;
    int64_t events[3];
} CoroutineTrapCase;

typedef struct CoroutineTrapProbe {
    XrInstance *instance;
    bool refuse_child;
    bool refuse_all;
    uint32_t event_count;
    int64_t events[3];
} CoroutineTrapProbe;

static XrProviderCallStatus coroutine_trap_record(void *context, int64_t argument,
                                                  int64_t *result_out) {
    CoroutineTrapProbe *probe = context;
    REQUIRE(probe != NULL && probe->instance != NULL && result_out != NULL);
    REQUIRE(xr_execution_instance_state(probe->instance) == XR_INSTANCE_DRAINING);
    REQUIRE(xr_execution_instance_lease_count(probe->instance) == 1u);
    REQUIRE(xr_execution_instance_cache_key(probe->instance).generation == 701u);
    REQUIRE(probe->event_count < sizeof(probe->events) / sizeof(probe->events[0]));
    probe->events[probe->event_count++] = argument;
    *result_out = -999;
    return probe->refuse_all || (probe->refuse_child && argument == 11) ? XR_PROVIDER_CALL_FAILED
                                                                        : XR_PROVIDER_CALL_OK;
}

static void require_coroutine_trap_events(const CoroutineTrapProbe *probe,
                                          const CoroutineTrapCase *test) {
    REQUIRE(probe->event_count == test->event_count);
    for (uint32_t event = 0u; event < test->event_count; ++event)
        REQUIRE(probe->events[event] == test->events[event]);
}

static XrInstance *create_coroutine_trap_instance(XrValidatedProgram *program,
                                                  XrTargetProfile *profile,
                                                  CoroutineTrapProbe *probe) {
    TestProviderBindings bindings;
    build_scalar_clock_binding(profile, false, &bindings);
    bindings.operations[0][0].entry.i64_unary = coroutine_trap_record;
    bindings.operations[0][0].context = probe;
    probe->instance = create_instance(program, profile, &bindings, 701u);
    REQUIRE(xr_execution_instance_state(probe->instance) == XR_INSTANCE_ACTIVE);
    REQUIRE(xr_execution_instance_lease_count(probe->instance) == 0u);
    return probe->instance;
}

static void require_coroutine_trap_generation_busy(XrInstance *instance) {
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_state(instance) == XR_INSTANCE_DRAINING);
    REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
    REQUIRE(xr_execution_instance_retire(instance, &diagnostic) ==
            XR_EXECUTION_GENERATION_REJECTED);
    REQUIRE(diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY);
}

static void run_reference_coroutine_trap_case(XrValidatedProgram *program, XrTargetProfile *profile,
                                              const CoroutineTrapCase *test) {
    CoroutineTrapProbe probe = {.refuse_child = test->refuse_child};
    XrInstance *instance = create_coroutine_trap_instance(program, profile, &probe);
    uint32_t entry = xr_validated_program_entry_function(program);
    XrReferenceExecution *execution = NULL;
    REQUIRE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &execution));
    REQUIRE(xr_reference_execution_cancel(execution).kind ==
            XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
    REQUIRE(probe.event_count == 0u);
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(instance, &diagnostic) == XR_EXECUTION_OK);
    require_coroutine_trap_generation_busy(instance);
    XrReferenceExecution *rejected = NULL;
    REQUIRE(!xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &rejected));
    REQUIRE(rejected == NULL);
    XrReferenceOutcome result = xr_reference_execution_step(execution);
    bool before_yield = test->mutation == XR_PROGRAM_COROUTINE_TRAP_BEFORE_YIELD;
    if (!before_yield) {
        REQUIRE(result.kind == XR_REFERENCE_OUTCOME_SUSPENDED);
        REQUIRE(result.state_id == 1u && result.safepoint_id == 0u && result.steps == 12u);
        REQUIRE(probe.event_count == 0u);
        require_coroutine_trap_generation_busy(instance);
        result = test->cancel ? xr_reference_execution_cancel(execution)
                              : xr_reference_execution_step(execution);
    }
    REQUIRE(result.state_id == (before_yield ? 0u : 1u));
    REQUIRE(result.steps == test->steps);
    REQUIRE(!result.owns_dynamic_values);
    switch (test->expected) {
        case COROUTINE_TRAP_RETURN:
            REQUIRE(result.kind == XR_REFERENCE_OUTCOME_RETURN);
            REQUIRE(result.trap == XR_REFERENCE_TRAP_NONE);
            REQUIRE(result.value.kind == XR_REFERENCE_VALUE_I64 && result.value.as.i64 == 44);
            break;
        case COROUTINE_TRAP_CANCELLED:
            REQUIRE(result.kind == XR_REFERENCE_OUTCOME_CANCELLED);
            REQUIRE(result.trap == XR_REFERENCE_TRAP_NONE);
            break;
        case COROUTINE_TRAP_PROVIDER_FAILED:
            REQUIRE(result.kind == XR_REFERENCE_OUTCOME_TRAP);
            REQUIRE(result.trap == XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
            break;
        case COROUTINE_TRAP_EXPLICIT:
            REQUIRE(result.kind == XR_REFERENCE_OUTCOME_TRAP);
            REQUIRE(result.trap == XR_REFERENCE_TRAP_EXPLICIT);
            break;
    }
    require_coroutine_trap_events(&probe, test);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_reference_execution_step(execution).kind == XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
    REQUIRE(xr_reference_execution_cancel(execution).kind ==
            XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
    REQUIRE(xr_execution_instance_retire(instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_state(instance) == XR_INSTANCE_RETIRED);
    xr_reference_execution_free(execution);
    require_coroutine_trap_events(&probe, test);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_execution_instance_free(&instance, &diagnostic) == XR_EXECUTION_OK);
}

static XrFingerprint run_vm_coroutine_trap_case(XrValidatedProgram *program,
                                                XrTargetProfile *profile,
                                                const CoroutineTrapCase *test) {
    CoroutineTrapProbe probe = {.refuse_child = test->refuse_child};
    XrInstance *instance = create_coroutine_trap_instance(program, profile, &probe);
    uint32_t entry = xr_validated_program_entry_function(program);
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic code_diagnostic;
    REQUIRE(xr_vm_code_build(program, profile, &options, &code, &code_diagnostic) == XR_VM_CODE_OK);
    REQUIRE(
        xr_fingerprint_equal(xr_vm_code_execution_id(code), xr_execution_instance_id(instance)));
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    XrVmExecution *execution = NULL;
    REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
    REQUIRE(xr_vm_execution_cancel(execution).kind == XR_VM_OUTCOME_INVALID_INVOCATION);
    REQUIRE(probe.event_count == 0u);
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(instance, &diagnostic) == XR_EXECUTION_OK);
    require_coroutine_trap_generation_busy(instance);
    XrVmExecution *rejected = NULL;
    REQUIRE(!xr_vm_execution_create(code, instance, entry, NULL, 0u, &rejected));
    REQUIRE(rejected == NULL);
    xr_vm_code_free(code);
    XrVmOutcome result = xr_vm_execution_step(execution);
    bool before_yield = test->mutation == XR_PROGRAM_COROUTINE_TRAP_BEFORE_YIELD;
    if (!before_yield) {
        REQUIRE(result.kind == XR_VM_OUTCOME_SUSPENDED);
        REQUIRE(result.state_id == 1u && result.safepoint_id == 0u && result.steps == 12u);
        REQUIRE(probe.event_count == 0u);
        require_coroutine_trap_generation_busy(instance);
        result = test->cancel ? xr_vm_execution_cancel(execution) : xr_vm_execution_step(execution);
    }
    REQUIRE(result.state_id == (before_yield ? 0u : 1u));
    REQUIRE(result.steps == test->steps);
    REQUIRE(!result.owns_dynamic_values);
    switch (test->expected) {
        case COROUTINE_TRAP_RETURN:
            REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
            REQUIRE(result.trap == XR_VM_TRAP_NONE);
            REQUIRE(result.value.kind == XR_VM_VALUE_I64 && result.value.as.i64 == 44);
            break;
        case COROUTINE_TRAP_CANCELLED:
            REQUIRE(result.kind == XR_VM_OUTCOME_CANCELLED);
            REQUIRE(result.trap == XR_VM_TRAP_NONE);
            break;
        case COROUTINE_TRAP_PROVIDER_FAILED:
            REQUIRE(result.kind == XR_VM_OUTCOME_TRAP);
            REQUIRE(result.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED);
            break;
        case COROUTINE_TRAP_EXPLICIT:
            REQUIRE(result.kind == XR_VM_OUTCOME_TRAP);
            REQUIRE(result.trap == XR_VM_TRAP_EXPLICIT);
            break;
    }
    require_coroutine_trap_events(&probe, test);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_vm_execution_step(execution).kind == XR_VM_OUTCOME_INVALID_INVOCATION);
    REQUIRE(xr_vm_execution_cancel(execution).kind == XR_VM_OUTCOME_INVALID_INVOCATION);
    REQUIRE(xr_execution_instance_retire(instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_state(instance) == XR_INSTANCE_RETIRED);
    xr_vm_execution_free(execution);
    require_coroutine_trap_events(&probe, test);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_execution_instance_free(&instance, &diagnostic) == XR_EXECUTION_OK);
    return result.logical_trace;
}

static void test_coroutine_child_provider_failure_continuations(void) {
    // Counts include re-entering the sealed call on resume, but not on cancellation.
    // Field events distinguish 22 before yield, 44 at yield, and 55 after resume;
    // the independent snapshot stays 33, including after either child failure.
    const CoroutineTrapCase cases[] = {
        {"resume",
         XR_PROGRAM_COROUTINE_TRAP_VALID,
         false,
         false,
         COROUTINE_TRAP_RETURN,
         26u,
         2u,
         {11, 55}},
        {"cancel",
         XR_PROGRAM_COROUTINE_TRAP_VALID,
         true,
         false,
         COROUTINE_TRAP_CANCELLED,
         22u,
         2u,
         {11, 44}},
        {"resume with canonical set order distinct from tuple",
         XR_PROGRAM_COROUTINE_TRAP_REVERSED_LIVE_TUPLE,
         false,
         false,
         COROUTINE_TRAP_RETURN,
         26u,
         2u,
         {11, 55}},
        {"cancel with canonical set order distinct from tuple",
         XR_PROGRAM_COROUTINE_TRAP_REVERSED_LIVE_TUPLE,
         true,
         false,
         COROUTINE_TRAP_CANCELLED,
         22u,
         2u,
         {11, 44}},
        {"child resume refusal",
         XR_PROGRAM_COROUTINE_TRAP_VALID,
         false,
         true,
         COROUTINE_TRAP_PROVIDER_FAILED,
         25u,
         3u,
         {11, 55, 33}},
        {"child cancel refusal",
         XR_PROGRAM_COROUTINE_TRAP_VALID,
         true,
         true,
         COROUTINE_TRAP_PROVIDER_FAILED,
         22u,
         3u,
         {11, 44, 33}},
        {"before-yield refusal",
         XR_PROGRAM_COROUTINE_TRAP_BEFORE_YIELD,
         false,
         true,
         COROUTINE_TRAP_PROVIDER_FAILED,
         15u,
         3u,
         {11, 22, 33}},
        {"resume without trap edge",
         XR_PROGRAM_COROUTINE_TRAP_NO_TRAP_EDGE,
         false,
         true,
         COROUTINE_TRAP_PROVIDER_FAILED,
         19u,
         1u,
         {11}},
        {"cancel without trap edge",
         XR_PROGRAM_COROUTINE_TRAP_NO_TRAP_EDGE,
         true,
         true,
         COROUTINE_TRAP_PROVIDER_FAILED,
         16u,
         1u,
         {11}},
        {"other child trap",
         XR_PROGRAM_COROUTINE_TRAP_OTHER_CHILD_TRAP,
         false,
         false,
         COROUTINE_TRAP_EXPLICIT,
         20u,
         1u,
         {11}},
    };
    XrTargetProfile *profile = xr_test_target_profile_build_with_scalar_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED, XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    REQUIRE(profile != NULL);
    const XrTargetProviderContract *contract = scalar_clock_contract(profile);
    REQUIRE(contract != NULL && contract->operation_count == 1u);
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const CoroutineTrapCase *test = &cases[index];
        fprintf(stderr, "coroutine child provider cleanup: %s\n", test->name);
        XrProgramArtifact artifact = {0};
        char diagnostic[256] = {0};
        REQUIRE(xr_program_coroutine_trap_fixture_write_with_ids(
                    contract->contract_id, contract->operations[0].stable_id, test->mutation,
                    &artifact, diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *program = NULL;
        XrProgramDiagnostic verify_diagnostic;
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                    &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        run_reference_coroutine_trap_case(program, profile, test);
        XrFingerprint baseline =
            run_vm_coroutine_trap_case(program, profile, test);
        XrFingerprint zero = {{0}};
        REQUIRE(!fingerprint_equal(baseline, zero));
        xr_validated_program_free(program);
    }
    xr_target_profile_free(profile);
}

static void test_coroutine_self_edge_parallel_arguments(void) {
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_coroutine_branch_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    uint32_t entry = xr_validated_program_entry_function(program);
    for (uint32_t cancel = 0u; cancel < 2u; ++cancel) {
        uint64_t expected_steps = cancel ? 5u : 13u;
        XrInstance *instance = create_instance(program, profile, &bindings, 801u);
        XrReferenceExecution *reference = NULL;
        REQUIRE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference));
        XrReferenceOutcome expected = xr_reference_execution_step(reference);
        REQUIRE(expected.kind == XR_REFERENCE_OUTCOME_SUSPENDED && expected.steps == 4u);
        expected = cancel ? xr_reference_execution_cancel(reference)
                          : xr_reference_execution_step(reference);
        REQUIRE(expected.kind ==
                (cancel ? XR_REFERENCE_OUTCOME_CANCELLED : XR_REFERENCE_OUTCOME_RETURN));
        REQUIRE(expected.steps == expected_steps && expected.trap == XR_REFERENCE_TRAP_NONE);
        if (!cancel)
            REQUIRE(expected.value.kind == XR_REFERENCE_VALUE_I64 && expected.value.as.i64 == 11);
        REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
        xr_reference_execution_free(reference);
        retire_and_free(&instance);
        XrFingerprint traces[1];

        {
            instance = create_instance(program, profile, &bindings, 801u);
            XrVmCodeOptions options = xr_vm_code_default_options();
            XrVmCode *code = NULL;
            XrVmCodeDiagnostic code_diagnostic;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, &code_diagnostic) ==
                    XR_VM_CODE_OK);
            XrVmExecution *execution = NULL;
            REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
            XrVmOutcome result = xr_vm_execution_step(execution);
            REQUIRE(result.kind == XR_VM_OUTCOME_SUSPENDED && result.steps == 4u);
            result = cancel ? xr_vm_execution_cancel(execution) : xr_vm_execution_step(execution);
            REQUIRE(result.kind == (cancel ? XR_VM_OUTCOME_CANCELLED : XR_VM_OUTCOME_RETURN));
            REQUIRE(result.steps == expected_steps && result.trap == XR_VM_TRAP_NONE);
            if (!cancel)
                REQUIRE(result.value.kind == XR_VM_VALUE_I64 && result.value.as.i64 == 11);
            REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
            traces[0u] = result.logical_trace;
            xr_vm_execution_free(execution);
            xr_vm_code_free(code);
            retire_and_free(&instance);
        }
        REQUIRE(!fingerprint_equal(traces[0], (XrFingerprint) {{0}}));
    }
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_reason_private_cleanup_graph_differential(void) {
    XrTargetProfile *profile = xr_test_target_profile_build_with_scalar_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED, XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    REQUIRE(profile != NULL);
    const XrTargetProviderContract *contract = scalar_clock_contract(profile);
    REQUIRE(contract != NULL && contract->operation_count == 1u);
    XrProgramCleanupGraphFixture fixture;
    REQUIRE(xr_program_cleanup_graph_fixture_init(&fixture));
    fixture.requirement.contract_id = contract->contract_id;
    fixture.operation_requirement.operation_id = contract->operations[0].stable_id;
    for (uint32_t block = 0u; block < XR_CLEANUP_GRAPH_BLOCK_COUNT; ++block) {
        for (uint32_t index = 0u; index < fixture.blocks[block].instruction_count; ++index) {
            XrCoreIrInstructionInput *instruction = &fixture.instructions[block][index];
            if (instruction->operation_id == XR_CORE_OP_CORE_PROVIDER_CALL) {
                instruction->immediate.provider_operation.contract_id =
                    fixture.requirement.contract_id;
                instruction->immediate.provider_operation.operation_id =
                    fixture.operation_requirement.operation_id;
            }
        }
    }
    XrProgramArtifact artifact = {0};
    char diagnostic[256] = {0};
    REQUIRE(xr_program_cleanup_graph_fixture_write_input(
                &fixture, &artifact, diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify_diagnostic;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    uint32_t entry = xr_validated_program_entry_function(program);
    for (uint32_t mode = 0u; mode < 4u; ++mode) {
        bool cancel = (mode & 1u) != 0u;
        bool refuse = (mode & 2u) != 0u;
        // These counts follow the fixture's explicit blocks, not another executor.
        const uint64_t expected_steps[] = {8u, 16u, 14u, 19u};
        int64_t expected_event = cancel ? 72 : 71;
        CoroutineTrapProbe probe = {.refuse_all = refuse};
        XrInstance *instance = create_coroutine_trap_instance(program, profile, &probe);
        XrReferenceExecution *reference = NULL;
        REQUIRE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference));
        XrExecutionDiagnostic execution_diagnostic;
        REQUIRE(xr_execution_instance_begin_drain(instance, &execution_diagnostic) ==
                XR_EXECUTION_OK);
        XrReferenceOutcome reference_result = xr_reference_execution_step(reference);
        REQUIRE(reference_result.kind == XR_REFERENCE_OUTCOME_SUSPENDED);
        REQUIRE(reference_result.steps == 3u && reference_result.state_id == 1u &&
                reference_result.safepoint_id == 0u && probe.event_count == 0u);
        require_coroutine_trap_generation_busy(instance);
        reference_result = cancel ? xr_reference_execution_cancel(reference)
                                  : xr_reference_execution_step(reference);
        REQUIRE(reference_result.kind == (refuse   ? XR_REFERENCE_OUTCOME_TRAP
                                          : cancel ? XR_REFERENCE_OUTCOME_CANCELLED
                                                   : XR_REFERENCE_OUTCOME_RETURN));
        REQUIRE(reference_result.trap ==
                (refuse ? XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED : XR_REFERENCE_TRAP_NONE));
        REQUIRE(reference_result.steps == expected_steps[mode] && reference_result.state_id == 1u);
        REQUIRE(!reference_result.owns_dynamic_values);
        if (!refuse && !cancel)
            REQUIRE(reference_result.value.kind == XR_REFERENCE_VALUE_VOID);
        REQUIRE(probe.event_count == 1u && probe.events[0] == expected_event);
        REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
        REQUIRE(xr_reference_execution_step(reference).kind ==
                XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
        REQUIRE(xr_reference_execution_cancel(reference).kind ==
                XR_REFERENCE_OUTCOME_INVALID_INVOCATION);
        xr_reference_execution_free(reference);
        REQUIRE(probe.event_count == 1u);
        REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) == XR_EXECUTION_OK);

        XrFingerprint traces[1];

        {
            probe = (CoroutineTrapProbe) {.refuse_all = refuse};
            instance = create_coroutine_trap_instance(program, profile, &probe);
            XrVmCodeOptions options = xr_vm_code_default_options();
            XrVmCode *code = NULL;
            XrVmCodeDiagnostic code_diagnostic;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, &code_diagnostic) ==
                    XR_VM_CODE_OK);
            XrVmExecution *execution = NULL;
            REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
            REQUIRE(xr_execution_instance_begin_drain(instance, &execution_diagnostic) ==
                    XR_EXECUTION_OK);
            xr_vm_code_free(code);
            XrVmOutcome result = xr_vm_execution_step(execution);
            REQUIRE(result.kind == XR_VM_OUTCOME_SUSPENDED);
            REQUIRE(result.steps == 3u && result.state_id == 1u && result.safepoint_id == 0u &&
                    probe.event_count == 0u);
            require_coroutine_trap_generation_busy(instance);
            result = cancel ? xr_vm_execution_cancel(execution) : xr_vm_execution_step(execution);
            REQUIRE(result.kind == (refuse   ? XR_VM_OUTCOME_TRAP
                                    : cancel ? XR_VM_OUTCOME_CANCELLED
                                             : XR_VM_OUTCOME_RETURN));
            REQUIRE(result.trap == (refuse ? XR_VM_TRAP_PROVIDER_CALL_FAILED : XR_VM_TRAP_NONE));
            REQUIRE(result.steps == expected_steps[mode] && result.state_id == 1u);
            REQUIRE(!result.owns_dynamic_values);
            if (!refuse && !cancel)
                REQUIRE(result.value.kind == XR_VM_VALUE_VOID);
            REQUIRE(probe.event_count == 1u && probe.events[0] == expected_event);
            REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
            REQUIRE(xr_vm_execution_step(execution).kind == XR_VM_OUTCOME_INVALID_INVOCATION);
            REQUIRE(xr_vm_execution_cancel(execution).kind == XR_VM_OUTCOME_INVALID_INVOCATION);
            traces[0u] = result.logical_trace;
            xr_vm_execution_free(execution);
            REQUIRE(probe.event_count == 1u);
            REQUIRE(xr_execution_instance_retire(instance, &execution_diagnostic) ==
                    XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, &execution_diagnostic) ==
                    XR_EXECUTION_OK);
        }
        XrFingerprint zero = {{0}};
        REQUIRE(!fingerprint_equal(traces[0], zero));
        REQUIRE(!fingerprint_equal(traces[0], (XrFingerprint) {{0}}));
    }
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
}

static void test_aggregate_construct_owner_transfers(void) {
    uint32_t executed = 0u;
    for (size_t index = 0u; index < XR_PROGRAM_CONSTRUCT_CASE_COUNT; ++index) {
        const XrProgramConstructCase *test = &xr_program_construct_cases[index];
        REQUIRE(test->kind == (XrProgramConstructCaseKind) index);
        if (test->diagnostic != XR_PROGRAM_DIAGNOSTIC_NONE)
            continue;
        fprintf(stderr, "aggregate owner transfer differential: %s\n", test->name);
        XrProgramArtifact artifact = {0};
        char diagnostic[256] = {0};
        XrProgramBuildStatus status = xr_program_construct_fixture_write(
            test->kind, &artifact, diagnostic, sizeof(diagnostic));
        if (status != XR_PROGRAM_BUILD_OK)
            fprintf(stderr, "construct fixture build failed: %s\n", diagnostic);
        REQUIRE(status == XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *program = NULL;
        XrProgramDiagnostic verify_diagnostic;
        XrProgramVerifyStatus verified =
            xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &verify_diagnostic);
        if (verified != XR_PROGRAM_VERIFY_OK)
            fprintf(stderr, "construct rejected: %s at f=%u b=%u i=%u v=%u\n",
                    xr_program_diagnostic_kind_name(verify_diagnostic.kind),
                    verify_diagnostic.location.function_id, verify_diagnostic.location.block_id,
                    verify_diagnostic.location.instruction_id, verify_diagnostic.location.value_id);
        REQUIRE(verified == XR_PROGRAM_VERIFY_OK && program != NULL);
        xr_program_artifact_free(&artifact);
        run_program(program, false, NULL, NULL, 0u, XR_VM_OUTCOME_RETURN, XR_VM_VALUE_I64, 42u);
        xr_validated_program_free(program);
        ++executed;
    }
    REQUIRE(executed > 0u);
}

static int run_h2_class_differential_probe(const char *mode) {
    REQUIRE(strcmp(mode, "baseline") == 0);
    XrValidatedProgram *program = build_class_alias_mutation_program(false);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(program != NULL && profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    ClassLifecycleLog log = {0};
    XrVmOutcome result =
        execute_class_alias_vm(program, instance, profile, &log);
    require_class_alias_vm_oracle(&log, result);
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);

    printf("{\"schema\":1,\"executor\":\"%s\",\"route\":\"%s\",\"oracle\":{"
           "\"scenario\":\"class-alias-mutation-lifecycle\","
           "\"outcome\":{\"kind\":\"return\"},"
           "\"value\":{\"kind\":\"i64\",\"data\":42},"
           "\"identities\":{\"constructed\":\"class-0\",\"shared\":\"class-0\"},"
           "\"events\":["
           "{\"kind\":\"class-construct\",\"identity\":\"class-0\"},"
           "{\"kind\":\"class-share\",\"identity\":\"class-0\","
           "\"related\":\"class-0\"},"
           "{\"kind\":\"class-field-place\",\"identity\":\"class-0\",\"field\":0},"
           "{\"kind\":\"place-exchange\",\"type\":\"i64\","
           "\"old\":{\"value\":7,\"identity\":\"none\"},"
           "\"replacement\":{\"value\":42,\"identity\":\"none\"}},"
           "{\"kind\":\"class-field-load\",\"identity\":\"class-0\",\"field\":0},"
           "{\"kind\":\"owner-drop\",\"identity\":\"class-0\"},"
           "{\"kind\":\"owner-drop\",\"identity\":\"class-0\"},"
           "{\"kind\":\"class-finalize\",\"identity\":\"class-0\"},"
           "{\"kind\":\"class-reclaim\",\"identity\":\"class-0\"}]}}\n",
           "vm-baseline", "vm-baseline-view");
    return 0;
}

#include "../program/xr_program_module_fixture.h"

static void test_module_initializer_execution(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    for (uint32_t scenario = 0u; scenario < 3u; ++scenario) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_module_output_fixture_write(scenario == 1u ? 1u : UINT32_MAX, &artifact,
                                                       NULL, 0u) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
                XR_PROGRAM_VERIFY_OK);
        {
            XrVmCodeOptions options = xr_vm_code_default_options();
            if (scenario == 2u)
                options.max_steps = 5u;
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            for (uint32_t instance_index = 0u; instance_index < 2u; ++instance_index) {
                OutputCapture capture = {0};
                TestProviderBindings bindings;
                build_output_binding(profile, &capture, &bindings);
                XrInstance *instance = create_instance(program, profile, &bindings, 1u);
                uint32_t entry = xr_validated_program_entry_function(program);
                for (uint32_t repeat = 0u; repeat < 2u; ++repeat) {
                    XrVmOutcome outcome;
                    if (instance_index == 0u) {
                        outcome = xr_vm_code_execute(code, instance, entry, NULL, 0u);
                    } else {
                        XrVmExecution *execution = NULL;
                        REQUIRE(
                            xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
                        outcome = xr_vm_execution_step(execution);
                        xr_vm_execution_free(execution);
                    }
                    REQUIRE(outcome.kind == (scenario == 0u   ? XR_VM_OUTCOME_RETURN
                                             : scenario == 1u ? XR_VM_OUTCOME_TRAP
                                                              : XR_VM_OUTCOME_RESOURCE_LIMIT));
                    if (scenario == 1u)
                        REQUIRE(outcome.trap == XR_VM_TRAP_EXPLICIT);
                    REQUIRE(outcome.steps == (repeat           ? 0u
                                              : scenario == 0u ? 12u
                                              : scenario == 1u ? 6u
                                                               : 5u));
                    const char *expected = scenario == 0u ? "1\n2\n3\n4\n" : "1\n2\n";
                    REQUIRE(capture.size == strlen(expected));
                    REQUIRE(memcmp(capture.bytes, expected, capture.size) == 0);
                }
                retire_and_free(&instance);
            }
            xr_vm_code_free(code);
        }
        xr_validated_program_free(program);
        xr_program_artifact_free(&artifact);
    }
    xr_target_profile_free(profile);
}

static void test_module_instance_value_storage(void) {
    _Static_assert(XR_CORE_OP_CORE_PLACE_MODULE == 152, "module place stable id drifted");
    _Static_assert(XR_CORE_OP_CORE_PLACE_INITIALIZE == 153, "initialization stable id drifted");
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    for (uint32_t text = 0u; text < 2u; ++text) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        char build_diagnostic[256] = {0};
        XrProgramBuildStatus built = xr_program_module_state_fixture_write(
            text != 0u, 0u, &artifact, build_diagnostic, sizeof(build_diagnostic));
        if (built != XR_PROGRAM_BUILD_OK)
            fprintf(stderr, "module state text=%u build=%u: %s\n", text, built, build_diagnostic);
        REQUIRE(built == XR_PROGRAM_BUILD_OK);
        XrProgramDiagnostic diagnostic = {0};
        XrProgramVerifyStatus status =
            xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &diagnostic);
        if (status != XR_PROGRAM_VERIFY_OK)
            fprintf(stderr,
                    "module state text=%u: status=%u diagnostic=%u function=%u block=%u "
                    "instruction=%u\n",
                    text, status, diagnostic.kind, diagnostic.location.function_id,
                    diagnostic.location.block_id, diagnostic.location.instruction_id);
        REQUIRE(status == XR_PROGRAM_VERIFY_OK);
        {
            XrVmCodeOptions options = xr_vm_code_default_options();
            options.max_value_cells = 64u;
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            XrInstance *instances[] = {
                create_instance(program, profile, &(TestProviderBindings) {0}, 1u),
                create_instance(program, profile, &(TestProviderBindings) {0}, 2u)};
            for (uint32_t repeat = 0u; repeat < 24u; ++repeat) {
                for (uint32_t instance = 0u; instance < 2u; ++instance) {
                    XrVmExecution *execution = NULL;
                    uint32_t entry = xr_validated_program_entry_function(program);
                    XrVmOutcome result;
                    if (instance == 0u) {
                        result = xr_vm_code_execute(code, instances[instance], entry, NULL, 0u);
                    } else {
                        REQUIRE(xr_vm_execution_create(code, instances[instance], entry, NULL, 0u,
                                                       &execution));
                        result = xr_vm_execution_step(execution);
                    }
                    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
                    if (text) {
                        XrVmStringView view = {0};
                        REQUIRE(xr_vm_value_string_view(&result.value, &view));
                        const char *expected = repeat ? "changed" : "module-owned-text";
                        REQUIRE(view.size == strlen(expected) &&
                                memcmp(view.bytes, expected, view.size) == 0);
                    } else {
                        REQUIRE(result.value.kind == XR_VM_VALUE_I64 &&
                                result.value.as.i64 == 41 + repeat);
                    }
                    REQUIRE(result.steps == 7u + (repeat ? 0u : 7u));
                    xr_vm_outcome_dispose(&result);
                    xr_vm_execution_free(execution);
                }
            }
            xr_vm_code_free(code);
            retire_and_free(&instances[0]);
            retire_and_free(&instances[1]);
        }
        xr_validated_program_free(program);
        xr_program_artifact_free(&artifact);
    }
    xr_target_profile_free(profile);
}

static void test_module_slot_failures(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    for (uint32_t fault = 1u; fault <= 3u; ++fault) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_module_state_fixture_write(fault == 3u, fault, &artifact, NULL, 0u) ==
                XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
                XR_PROGRAM_VERIFY_OK);
        {
            XrVmCodeOptions options = xr_vm_code_default_options();
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            for (uint32_t framed = 0u; framed < 2u; ++framed) {
                XrInstance *instance =
                    create_instance(program, profile, &(TestProviderBindings) {0}, 1u);
                for (uint32_t repeat = 0u; repeat < 2u; ++repeat) {
                    uint32_t entry = xr_validated_program_entry_function(program);
                    XrVmExecution *execution = NULL;
                    XrVmOutcome result;
                    if (framed) {
                        REQUIRE(
                            xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
                        result = xr_vm_execution_step(execution);
                    } else {
                        result = xr_vm_code_execute(code, instance, entry, NULL, 0u);
                    }
                    REQUIRE(result.kind == XR_VM_OUTCOME_TRAP);
                    REQUIRE(result.trap == (fault == 1u ? XR_VM_TRAP_MODULE_SLOT_UNINITIALIZED
                                            : fault == 2u
                                                ? XR_VM_TRAP_MODULE_SLOT_ALREADY_INITIALIZED
                                                : XR_VM_TRAP_EXPLICIT));
                    REQUIRE(result.steps == (fault == 1u ? (repeat ? 2u : 8u)
                                                         : (repeat        ? 0u
                                                            : fault == 2u ? 5u
                                                                          : 4u)));
                    xr_vm_execution_free(execution);
                }
                retire_and_free(&instance);
            }
            xr_vm_code_free(code);
        }
        xr_validated_program_free(program);
        xr_program_artifact_free(&artifact);
    }
    xr_target_profile_free(profile);
}

static void test_module_reverse_publication_cleanup(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    const uint32_t finishes[] = {0u, 1u, 4u, 5u, 6u, 7u};
    for (uint32_t scenario = 0u; scenario < XR_COUNTOF(finishes); ++scenario) {
        bool failed = finishes[scenario] != 0u;
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        char reason[256] = {0};
        XrProgramBuildStatus built = xr_program_module_cleanup_fixture_write(
            finishes[scenario], &artifact, reason, sizeof(reason));
        if (built != XR_PROGRAM_BUILD_OK)
            fprintf(stderr, "module cleanup fixture: %s\n", reason);
        REQUIRE(built == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
                XR_PROGRAM_VERIFY_OK);
        {
            ClassLifecycleLog log = {0};
            XrVmCodeOptions options = xr_vm_code_default_options();
            options.lifecycle_context = &log;
            options.lifecycle_event = record_vm_class_lifecycle;
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            XrInstance *instance =
                create_instance(program, profile, &(TestProviderBindings) {0}, 1u);
            XrExecutionLease keepalive = {0};
            REQUIRE(xr_execution_instance_acquire(instance, &keepalive) == XR_EXECUTION_OK);
            uint32_t count = failed ? 3u : 4u;
            bool error = finishes[scenario] == 4u || finishes[scenario] == 6u;
            bool panic = finishes[scenario] == 5u || finishes[scenario] == 7u;
            uint32_t before_retire = error ? 21u : failed ? count * 8u : count * 5u;
            for (uint32_t repeat = 0u; repeat < 2u; ++repeat) {
                XrVmExecution *execution = NULL;
                REQUIRE(xr_vm_execution_create(code, instance,
                                               xr_validated_program_entry_function(program), NULL,
                                               0u, &execution));
                XrVmOutcome result = xr_vm_execution_step(execution);
                if (finishes[scenario] >= 6u && repeat == 0u) {
                    REQUIRE(result.kind == XR_VM_OUTCOME_SUSPENDED && log.vm_count == 0u);
                    result = xr_vm_execution_step(execution);
                }
                REQUIRE(result.kind == (error ? XR_VM_OUTCOME_ERROR : panic ? XR_VM_OUTCOME_PANIC
                                        : failed ? XR_VM_OUTCOME_TRAP : XR_VM_OUTCOME_RETURN));
                REQUIRE(result.trap == (failed && !error && !panic
                                            ? XR_VM_TRAP_EXPLICIT : XR_VM_TRAP_NONE));
                if (error) {
                    REQUIRE(result.error_value.kind == XR_VM_VALUE_CLASS_REFERENCE);
                    REQUIRE(xr_execution_instance_lease_count(instance) == 2u);
                }
                REQUIRE(result.panic_value.kind ==
                        (panic ? XR_VM_VALUE_PANIC_INFO : XR_VM_VALUE_VOID));
                if (panic)
                    REQUIRE(result.panic_value.as.panic_info.code == 1u);
                xr_vm_execution_free(execution);
                REQUIRE(log.vm_count == before_retire);
            }
            XrVmOutcome owned_failure = {0};
            if (error) {
                owned_failure = xr_vm_code_execute(code, instance,
                    xr_validated_program_entry_function(program), NULL, 0u);
                REQUIRE(owned_failure.kind == XR_VM_OUTCOME_ERROR);
                REQUIRE(owned_failure.error_value.kind == XR_VM_VALUE_CLASS_REFERENCE);
                REQUIRE(owned_failure.owns_dynamic_values && owned_failure.private_owner);
                REQUIRE(xr_execution_instance_lease_count(instance) == 2u);
            }
            xr_vm_code_free(code);
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(log.vm_count == before_retire);
            REQUIRE(xr_execution_lease_release(&keepalive));
            if (error) {
                REQUIRE(log.vm_count == before_retire);
                REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
                xr_vm_outcome_dispose(&owned_failure);
                REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
            }
            REQUIRE(log.vm_count == count * 8u);
            const XrVmLifecycleEventKind releases[] = {
                XR_VM_EVENT_OWNER_DROP, XR_VM_EVENT_CLASS_FINALIZE, XR_VM_EVENT_CLASS_RECLAIM};
            if (error) {
                REQUIRE(log.vm[14].kind == XR_VM_EVENT_OWNER_DROP);
                REQUIRE(log.vm[14].identity == log.vm[10].identity);
                REQUIRE(log.vm[14].origin == XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
                const uint32_t release_order[] = {1u, 0u, 2u};
                for (uint32_t index = 0u; index < 3u; ++index) {
                    REQUIRE(log.vm[index * 5u].kind == XR_VM_EVENT_CLASS_CONSTRUCT);
                    for (uint32_t event = 0u; event < 3u; ++event) {
                        const XrVmLifecycleEvent *actual = &log.vm[15u + index * 3u + event];
                        REQUIRE(actual->kind == releases[event]);
                        REQUIRE(actual->identity == log.vm[release_order[index] * 5u].identity);
                        REQUIRE(actual->origin == XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
                    }
                }
            } else for (uint32_t index = 0u; index < count; ++index) {
                REQUIRE(log.vm[index * 5u].kind == XR_VM_EVENT_CLASS_CONSTRUCT);
                for (uint32_t event = 0u; event < 3u; ++event) {
                    const XrVmLifecycleEvent *actual = &log.vm[count * 5u + index * 3u + event];
                    REQUIRE(actual->kind == releases[event]);
                    REQUIRE(actual->identity == log.vm[(count - index - 1u) * 5u].identity);
                    REQUIRE(actual->origin == XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
                }
            }
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(log.vm_count == count * 8u);
        }
        xr_validated_program_free(program);
        xr_program_artifact_free(&artifact);
    }
    xr_target_profile_free(profile);
}

static void test_returned_callable_owner(void) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_callable_fixture_write_mutated(XR_CALLABLE_FIXTURE_RETURN_CAPTURE,
        &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
    XrProgramDiagnostic diagnostic = {0};
    XrProgramVerifyStatus status = xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &diagnostic);
    if (status != XR_PROGRAM_VERIFY_OK)
        fprintf(stderr, "callable return admission: kind=%u function=%u block=%u instruction=%u value=%u\n",
            (unsigned) diagnostic.kind, diagnostic.location.function_id, diagnostic.location.block_id,
            diagnostic.location.instruction_id, diagnostic.location.value_id);
    REQUIRE(status == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
        XrInstance *instance = create_instance(program, profile, &(TestProviderBindings) {0}, 1u);
        XrVmExecution *execution = NULL;
        REQUIRE(xr_vm_execution_create(code, instance, xr_validated_program_entry_function(program), NULL, 0u, &execution));
        XrVmOutcome borrowed = xr_vm_execution_step(execution);
        REQUIRE(borrowed.kind == XR_VM_OUTCOME_RETURN && borrowed.value.kind == XR_VM_VALUE_CALLABLE);
        XrVmOutcome owned = xr_vm_code_execute(code, instance, xr_validated_program_entry_function(program), NULL, 0u);
        REQUIRE(owned.kind == XR_VM_OUTCOME_RETURN && owned.value.kind == XR_VM_VALUE_CALLABLE);
        REQUIRE(owned.private_owner && owned.owns_dynamic_values);
        REQUIRE(owned.value.as.callable != borrowed.value.as.callable);
        uint32_t observer = UINT32_MAX;
        for (uint32_t index = 0u; index < program->function_count; ++index) {
            const XrValidatedFunction *function = &program->functions[index];
            const XrValidatedType *parameter = function->parameter_count == 1u
                ? xr_validated_program_type(program, function->parameter_types[0]) : NULL;
            if (parameter && parameter->kind == XR_CORE_IR_TYPE_CALLABLE) observer = index;
        }
        REQUIRE(observer != UINT32_MAX);
        XrVmOutcome observed = xr_vm_code_execute(code, instance, observer, &owned.value, 1u);
        REQUIRE(observed.kind == XR_VM_OUTCOME_RETURN && observed.value.kind == XR_VM_VALUE_I64);
        REQUIRE(observed.value.as.i64 == 42);
        xr_vm_outcome_dispose(&observed);
        observed = xr_vm_code_execute(code, instance, observer, &borrowed.value, 1u);
        REQUIRE(observed.kind == XR_VM_OUTCOME_RETURN && observed.value.as.i64 == 42);
        xr_vm_outcome_dispose(&observed);
        xr_vm_code_free(code);
        REQUIRE(xr_execution_instance_lease_count(instance) == 2u);
        REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
        xr_vm_outcome_dispose(&borrowed);
        xr_vm_execution_free(execution);
        REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
        xr_vm_outcome_dispose(&owned);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
    }
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_returned_existential_owner(void) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_reborrow_fixture_write_mutated(XR_REBORROW_FIXTURE_RETURN_OWNER,
        &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
    XrProgramDiagnostic diagnostic = {0};
    XrProgramVerifyStatus status = xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &diagnostic);
    if (status != XR_PROGRAM_VERIFY_OK)
        fprintf(stderr, "existential return admission: kind=%u function=%u block=%u instruction=%u value=%u\n",
            (unsigned) diagnostic.kind, diagnostic.location.function_id, diagnostic.location.block_id,
            diagnostic.location.instruction_id, diagnostic.location.value_id);
    REQUIRE(status == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
        XrInstance *instance = create_instance(program, profile, &(TestProviderBindings) {0}, 1u);
        XrVmExecution *execution = NULL;
        REQUIRE(xr_vm_execution_create(code, instance, xr_validated_program_entry_function(program), NULL, 0u, &execution));
        XrVmOutcome borrowed = xr_vm_execution_step(execution);
        REQUIRE(borrowed.kind == XR_VM_OUTCOME_RETURN && borrowed.value.kind == XR_VM_VALUE_EXISTENTIAL);
        XrVmOutcome owned = xr_vm_code_execute(code, instance, xr_validated_program_entry_function(program), NULL, 0u);
        REQUIRE(owned.kind == XR_VM_OUTCOME_RETURN && owned.value.kind == XR_VM_VALUE_EXISTENTIAL);
        REQUIRE(owned.private_owner && owned.owns_dynamic_values);
        REQUIRE(owned.value.as.existential != borrowed.value.as.existential);
        uint32_t observer = UINT32_MAX;
        for (uint32_t index = 0u; index < program->function_count; ++index) {
            const XrValidatedFunction *function = &program->functions[index];
            const XrValidatedType *parameter = function->parameter_count == 1u
                ? xr_validated_program_type(program, function->parameter_types[0]) : NULL;
            if (parameter && parameter->kind == XR_CORE_IR_TYPE_EXISTENTIAL && parameter->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) observer = index;
        }
        REQUIRE(observer != UINT32_MAX);
        XrVmOutcome observed = xr_vm_code_execute(code, instance, observer, &owned.value, 1u);
        REQUIRE(observed.kind == XR_VM_OUTCOME_RETURN && observed.value.kind == XR_VM_VALUE_I64);
        REQUIRE(observed.value.as.i64 == 42);
        xr_vm_outcome_dispose(&observed);
        observed = xr_vm_code_execute(code, instance, observer, &borrowed.value, 1u);
        REQUIRE(observed.kind == XR_VM_OUTCOME_RETURN && observed.value.as.i64 == 42);
        xr_vm_outcome_dispose(&observed);
        xr_vm_code_free(code);
        REQUIRE(xr_execution_instance_lease_count(instance) == 2u);
        REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
        xr_vm_outcome_dispose(&borrowed);
        xr_vm_execution_free(execution);
        REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
        xr_vm_outcome_dispose(&owned);
        REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
    }
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_one_shot_string_error_owner(void) {
    XrCoreIrConstantInput constant = {
        .key = fixture_key("string-error:constant"), .type_id = XR_CORE_TYPE_STRING,
        .kind = XR_CORE_IR_CONSTANT_STRING,
        .value.string = {.bytes = (const uint8_t *) "direct error", .size = 12u},
    };
    XrCoreIrKey value = fixture_key("string-error:value");
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_STRING, .result = value,
         .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = constant.key},
        {.operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH, .operands = &value, .operand_count = 1u},
    };
    XrCoreIrBlockInput block = {
        .key = fixture_key("string-error:block"),
        .instructions = instructions, .instruction_count = XR_COUNTOF(instructions),
    };
    XrCoreIrFunctionInput function = {
        .key = fixture_key("string-error:function"), .entry_block = block.key,
        .error_type_id = XR_CORE_TYPE_STRING, .effect_mask = XR_CORE_EFFECT_ERROR,
        .blocks = &block, .block_count = 1u, .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrValidatedProgram *program = validate_fixture(&constant, 1u, &function, 1u);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
        XrInstance *instance = create_instance(program, profile, &(TestProviderBindings) {0}, 1u);
        XrVmOutcome outcome = xr_vm_code_execute(code, instance,
            xr_validated_program_entry_function(program), NULL, 0u);
        REQUIRE(outcome.kind == XR_VM_OUTCOME_ERROR && outcome.owns_dynamic_values);
        REQUIRE(outcome.private_owner != NULL);
        xr_vm_code_free(code);
        REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
        XrVmStringView view;
        REQUIRE(xr_vm_value_string_view(&outcome.error_value, &view));
        REQUIRE(view.size == 12u && memcmp(view.bytes, "direct error", 12u) == 0);
        xr_vm_outcome_dispose(&outcome);
        REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
        retire_and_free(&instance);
    }
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static bool interrupt_after_operations(void *context) {
    uint32_t *remaining = context;
    if (*remaining == 0u)
        return true;
    --*remaining;
    return false;
}

static void test_module_initializer_budget_cleanup(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    for (uint32_t interrupted = 0u; interrupted < 2u; ++interrupted) {
        for (uint32_t delegated = 0u; delegated < 2u; ++delegated) {
            XrProgramArtifact artifact = {0};
            XrValidatedProgram *program = NULL;
            REQUIRE(xr_program_module_cleanup_fixture_write(delegated ? 3u : 0u, &artifact, NULL, 0u) ==
                    XR_PROGRAM_BUILD_OK);
            REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
                    XR_PROGRAM_VERIFY_OK);
            {
                uint32_t stride = delegated ? 14u : 10u;
                for (uint32_t budget = 1u; budget <= 4u * stride; ++budget) {
                    ClassLifecycleLog log = {0};
                    XrVmCodeOptions options = xr_vm_code_default_options();
                    options.max_steps = interrupted ? UINT64_MAX : budget;
                    options.lifecycle_context = &log;
                    options.lifecycle_event = record_vm_class_lifecycle;
                    XrVmCode *code = NULL;
                    REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
                    XrInstance *instance =
                        create_instance(program, profile, &(TestProviderBindings) {0}, 1u);
                    XrExecutionLease held = {0};
                    REQUIRE(xr_execution_instance_acquire(instance, &held) == XR_EXECUTION_OK);
                    XrVmExecution *execution = NULL;
                    REQUIRE(xr_vm_execution_create(code, instance,
                                                   xr_validated_program_entry_function(program), NULL,
                                                   0u, &execution));
                    uint32_t remaining = budget;
                    if (interrupted)
                        REQUIRE(xr_vm_execution_set_interrupt(execution, &remaining,
                                                              interrupt_after_operations));
                    XrVmOutcome result = xr_vm_execution_step(execution);
                    REQUIRE(!xr_vm_execution_set_interrupt(execution, NULL, NULL));
                    REQUIRE(result.kind == XR_VM_OUTCOME_RESOURCE_LIMIT && result.steps == budget);
                    uint32_t constructed = 0u, finalized = 0u, reclaimed = 0u;
                    for (uint32_t event = 0u; event < log.vm_count; ++event) {
                        constructed += log.vm[event].kind == XR_VM_EVENT_CLASS_CONSTRUCT;
                        finalized += log.vm[event].kind == XR_VM_EVENT_CLASS_FINALIZE;
                        reclaimed += log.vm[event].kind == XR_VM_EVENT_CLASS_RECLAIM;
                    }
                    REQUIRE(constructed == (budget + stride - 2u) / stride);
                    if (constructed != finalized || constructed != reclaimed)
                        fprintf(
                            stderr,
                            "module budget=%u delegated=%u constructed=%u finalized=%u reclaimed=%u\n",
                            budget, delegated, constructed, finalized, reclaimed);
                    REQUIRE(constructed == finalized && constructed == reclaimed);
                    uint32_t events = log.vm_count;
                    xr_vm_execution_free(execution);
                    result = xr_vm_code_execute(code, instance,
                                                xr_validated_program_entry_function(program), NULL, 0u);
                    REQUIRE(result.kind == XR_VM_OUTCOME_RESOURCE_LIMIT && result.steps == 0u);
                    REQUIRE(log.vm_count == events);
                    REQUIRE(xr_execution_lease_release(&held));
                    retire_and_free(&instance);
                    REQUIRE(log.vm_count == events);
                    xr_vm_code_free(code);
                }
            }
            xr_validated_program_free(program);
            xr_program_artifact_free(&artifact);
        }
    }
    xr_target_profile_free(profile);
}

static void test_module_suspended_initializer_cleanup(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_module_cleanup_fixture_write(2u, &artifact, NULL, 0u) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    {
        for (uint32_t finish = 0u; finish < 4u; ++finish) {
            ClassLifecycleLog log = {0};
            XrVmCodeOptions options = xr_vm_code_default_options();
            options.lifecycle_context = &log;
            options.lifecycle_event = record_vm_class_lifecycle;
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            XrInstance *instance =
                create_instance(program, profile, &(TestProviderBindings) {0}, 1u);
            uint32_t entry = xr_validated_program_entry_function(program);
            XrVmExecution *first = NULL;
            XrVmExecution *waiting = NULL;
            REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &waiting));
            if (finish == 3u) {
                REQUIRE(xr_vm_code_execute(code, instance, entry, NULL, 0u).kind ==
                        XR_VM_OUTCOME_INVALID_INVOCATION);
            } else {
                REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &first));
                REQUIRE(xr_vm_execution_step(first).kind == XR_VM_OUTCOME_SUSPENDED);
                REQUIRE(log.vm_count == 15u);
                REQUIRE(xr_vm_execution_step(waiting).kind == XR_VM_OUTCOME_INITIALIZING);
                if (finish == 0u)
                    REQUIRE(xr_vm_execution_step(first).kind == XR_VM_OUTCOME_RETURN);
                else if (finish == 1u)
                    REQUIRE(xr_vm_execution_cancel(first).kind == XR_VM_OUTCOME_CANCELLED);
                xr_vm_execution_free(first);
            }
            /* A waiting lease must not delay failed initialization cleanup. */
            REQUIRE(log.vm_count == (finish ? 24u : 15u));
            XrVmOutcome repeated = xr_vm_execution_step(waiting);
            REQUIRE(repeated.kind == (finish == 0u   ? XR_VM_OUTCOME_RETURN
                                      : finish == 1u ? XR_VM_OUTCOME_CANCELLED
                                                     : XR_VM_OUTCOME_TRAP));
            if (finish >= 2u)
                REQUIRE(repeated.trap == XR_VM_TRAP_EXPLICIT);
            xr_vm_execution_free(waiting);
            xr_vm_code_free(code);
            retire_and_free(&instance);
            REQUIRE(log.vm_count == 24u);
            for (uint32_t index = 0u; index < 3u; ++index) {
                const XrVmLifecycleEvent *drop = &log.vm[15u + index * 3u];
                REQUIRE(drop->kind == XR_VM_EVENT_OWNER_DROP);
                REQUIRE(drop->identity == log.vm[(2u - index) * 5u].identity);
                REQUIRE(log.vm[16u + index * 3u].kind == XR_VM_EVENT_CLASS_FINALIZE);
                REQUIRE(log.vm[17u + index * 3u].kind == XR_VM_EVENT_CLASS_RECLAIM);
            }
        }
    }
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

#include "xr_program_vm_child_storage_tests.inc.c"

static void test_sequence_length(void) {
    _Static_assert(XR_CORE_OP_CORE_SEQUENCE_LENGTH == 156u, "sequence length stable id drifted");
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_sequence_length_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    uint32_t entry = xr_validated_program_entry_function(program);
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
        XrVmOutcome result = xr_vm_code_execute(code, instance, entry, NULL, 0u);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
        REQUIRE(result.value.kind == XR_VM_VALUE_I64 && result.value.as.i64 == 6);
        xr_vm_outcome_dispose(&result);
        XrVmExecution *execution = NULL;
        REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
        result = xr_vm_execution_step(execution);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
        REQUIRE(result.value.kind == XR_VM_VALUE_I64 && result.value.as.i64 == 6);
        xr_vm_outcome_dispose(&result);
        xr_vm_execution_free(execution);
        xr_vm_code_free(code);
    }
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_array_alias_identity(void) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_array_fixture_write(4u, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
        XrVmExecution *execution = NULL;
        REQUIRE(xr_vm_execution_create(code, instance, xr_validated_program_entry_function(program), NULL, 0u, &execution));
        XrVmOutcome result = xr_vm_execution_step(execution);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
        XrVmAggregateView outer, original, alias, copied;
        REQUIRE(xr_vm_value_aggregate_view(&result.value, &outer) && outer.field_count == 3u);
        REQUIRE(outer.fields[0].as.aggregate == outer.fields[1].as.aggregate);
        REQUIRE(outer.fields[0].as.aggregate != outer.fields[2].as.aggregate);
        REQUIRE(xr_vm_value_aggregate_view(&outer.fields[0], &original));
        REQUIRE(xr_vm_value_aggregate_view(&outer.fields[1], &alias));
        REQUIRE(xr_vm_value_aggregate_view(&outer.fields[2], &copied));
        REQUIRE(original.field_count == 2u && alias.field_count == 2u && copied.field_count == 2u);
        REQUIRE(original.fields == alias.fields && original.fields != copied.fields);
        REQUIRE(original.fields[0].as.i64 == 42 && copied.fields[0].as.i64 == 42);
        xr_vm_outcome_dispose(&result);
        xr_vm_execution_free(execution);
        result = xr_vm_code_execute(code, instance, xr_validated_program_entry_function(program), NULL, 0u);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN && result.owns_dynamic_values && result.private_owner);
        xr_vm_code_free(code);
        REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
        {
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_GENERATION_REJECTED);
        }
        REQUIRE(xr_vm_value_aggregate_view(&result.value, &outer) && outer.field_count == 3u);
        REQUIRE(outer.fields[0].as.aggregate == outer.fields[1].as.aggregate);
        REQUIRE(outer.fields[0].as.aggregate != outer.fields[2].as.aggregate);
        REQUIRE(xr_vm_value_aggregate_view(&outer.fields[1], &alias));
        REQUIRE(alias.fields[0].as.i64 == 42);
        xr_vm_outcome_dispose(&result);
    }
    REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_array_values(unsigned scenario) {
    _Static_assert(XR_CORE_OP_CORE_ARRAY_CONSTRUCT == 157u, "array construct stable id drifted");
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_array_fixture_write(scenario, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    uint32_t entry = xr_validated_program_entry_function(program);
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
        XrVmOutcome result = xr_vm_code_execute(code, instance, entry, NULL, 0u);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
        REQUIRE(result.value.kind == XR_VM_VALUE_I64 && result.value.as.i64 == (scenario == 1u ? 0 : scenario == 3u ? 1 : 2));
        xr_vm_outcome_dispose(&result);
        XrVmExecution *execution = NULL;
        REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
        result = xr_vm_execution_step(execution);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
        REQUIRE(result.value.kind == XR_VM_VALUE_I64 && result.value.as.i64 == (scenario == 1u ? 0 : scenario == 3u ? 1 : 2));
        xr_vm_outcome_dispose(&result);
        xr_vm_execution_free(execution);
        xr_vm_code_free(code);
    }
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_exact_integer_value_abi(void) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_integer_fixture_write(false, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
#define CHECK_INTEGER_VALUE(type, member, expected)                                                \
    do {                                                                                           \
        uint32_t entry = xr_program_integer_fixture_function(program, XR_CORE_TYPE_##type,         \
                                                             XR_CORE_TYPE_##type);                 \
        REQUIRE(entry != UINT32_MAX);                                                              \
        XrVmValue argument = {.kind = XR_VM_VALUE_##type, .as.member = (expected)};                \
        XrVmOutcome result = xr_vm_code_execute(code, instance, entry, &argument, 1u);             \
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);                                              \
        REQUIRE(result.value.kind == XR_VM_VALUE_##type);                                          \
        REQUIRE(result.value.as.member == (expected));                                             \
        xr_vm_outcome_dispose(&result);                                                            \
        argument = (XrVmValue) {.kind = XR_VM_VALUE_BOOL, .as.boolean = true};                     \
        result = xr_vm_code_execute(code, instance, entry, &argument, 1u);                         \
        REQUIRE(result.kind == XR_VM_OUTCOME_INVALID_INVOCATION);                                  \
        xr_vm_outcome_dispose(&result);                                                            \
    } while (0)
    CHECK_INTEGER_VALUE(I8, i8, -128);
    CHECK_INTEGER_VALUE(I8, i8, 127);
    CHECK_INTEGER_VALUE(U8, u8, 0u);
    CHECK_INTEGER_VALUE(U8, u8, 255u);
    CHECK_INTEGER_VALUE(I16, i16, -32768);
    CHECK_INTEGER_VALUE(I16, i16, 32767);
    CHECK_INTEGER_VALUE(U16, u16, 0u);
    CHECK_INTEGER_VALUE(U16, u16, 65535u);
    CHECK_INTEGER_VALUE(I32, i32, (-INT32_C(2147483647) - INT32_C(1)));
    CHECK_INTEGER_VALUE(I32, i32, INT32_C(2147483647));
    CHECK_INTEGER_VALUE(U32, u32, UINT32_C(0));
    CHECK_INTEGER_VALUE(U32, u32, UINT32_C(4294967295));
    CHECK_INTEGER_VALUE(I64, i64, (-INT64_C(9223372036854775807) - INT64_C(1)));
    CHECK_INTEGER_VALUE(I64, i64, INT64_C(9223372036854775807));
    CHECK_INTEGER_VALUE(U64, u64, UINT64_C(0));
    CHECK_INTEGER_VALUE(U64, u64, UINT64_C(18446744073709551615));
#undef CHECK_INTEGER_VALUE
    xr_vm_code_free(code);
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_exact_integer_conversions(void) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_integer_fixture_write(true, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
#define XR_INTEGER_CONVERSION_CASE(source, input_member, input, target, output_member, expected)   \
    do {                                                                                           \
        uint32_t entry = xr_program_integer_fixture_function(program, XR_CORE_TYPE_##source,       \
                                                             XR_CORE_TYPE_##target);               \
        REQUIRE(entry != UINT32_MAX);                                                              \
        REQUIRE(program->functions[entry].blocks[0].instructions[1].operation_id ==                \
                XR_CORE_OP_CORE_INTEGER_CONVERT);                                                  \
        XrVmValue argument = {.kind = XR_VM_VALUE_##source, .as.input_member = (input)};           \
        XrVmOutcome result = xr_vm_code_execute(code, instance, entry, &argument, 1u);             \
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);                                              \
        REQUIRE(result.value.kind == XR_VM_VALUE_##target);                                        \
        REQUIRE(result.value.as.output_member == (expected));                                      \
        xr_vm_outcome_dispose(&result);                                                            \
    } while (0);
#include "../program/xr_program_integer_conversion_cases.inc.c"
#undef XR_INTEGER_CONVERSION_CASE
    xr_vm_code_free(code);
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_panic_point_chained_cleanup(void) {
    const uint16_t operations[] = {XR_CORE_OP_CORE_ASSERT_CONDITION,
                                   XR_CORE_OP_CORE_INTEGER_DIVMOD};
    for (unsigned operation = 0u; operation < 2u; ++operation) {
        XrProgramArtifact artifact = {0};
        REQUIRE(xr_program_panic_point_fixture_write(operations[operation],
                                                     XR_ASSERT_FIXTURE_CHAINED_CLEANUP, &artifact,
                                                     NULL, 0u) == XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
                XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        XrTargetProfile *profile =
            xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        TestProviderBindings bindings;
        build_provider_bindings(profile, &bindings);
        XrInstance *instance = create_instance(program, profile, &bindings, 1u);
        {
            XrVmCodeOptions options = xr_vm_code_default_options();
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            for (unsigned succeeds = 0u; succeeds < 2u; ++succeeds) {
                XrVmValue arguments[] = {
                    operation
                        ? (XrVmValue) {.kind = XR_VM_VALUE_I64, .as.i64 = succeeds ? 2 : 0}
                        : (XrVmValue) {.kind = XR_VM_VALUE_BOOL, .as.boolean = succeeds != 0u},
                    {.kind = XR_VM_VALUE_I64, .as.i64 = 84},
                };
                XrVmOutcome result = xr_vm_code_execute(
                    code, instance, xr_validated_program_entry_function(program), arguments, 2u);
                REQUIRE(result.kind == (succeeds ? XR_VM_OUTCOME_RETURN : XR_VM_OUTCOME_PANIC));
                if (succeeds) {
                    REQUIRE(result.value.kind == XR_VM_VALUE_I64);
                    REQUIRE(result.value.as.i64 == 42);
                } else {
                    REQUIRE(result.panic_value.kind == XR_VM_VALUE_PANIC_INFO);
                    REQUIRE(result.panic_value.as.panic_info.code == (operation ? 420u : 1u));
                }
                xr_vm_outcome_dispose(&result);
            }
            xr_vm_code_free(code);
        }
        retire_and_free(&instance);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
    }
}

static void test_exact_integer_divmod(void) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_integer_divmod_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
#define XR_INTEGER_DIVMOD_CASE(type, member, left, right, quotient, residue)                       \
    do {                                                                                           \
        for (unsigned remainder = 0u; remainder < 2u; ++remainder) {                               \
            uint32_t entry = xr_program_integer_divmod_fixture_function(                           \
                program, XR_CORE_TYPE_##type, remainder);                                          \
            REQUIRE(entry != UINT32_MAX);                                                          \
            REQUIRE(program->functions[entry].blocks[0].instructions[1].operation_id ==            \
                    XR_CORE_OP_CORE_INTEGER_DIVMOD);                                               \
            XrVmValue arguments[] = {                                                              \
                {.kind = XR_VM_VALUE_##type, .as.member = (left)},                                 \
                {.kind = XR_VM_VALUE_##type, .as.member = (right)},                                \
            };                                                                                     \
            XrVmOutcome result = xr_vm_code_execute(code, instance, entry, arguments, 2u);         \
            REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);                                          \
            REQUIRE(result.value.kind == XR_VM_VALUE_##type);                                      \
            REQUIRE(result.value.as.member == (remainder ? (residue) : (quotient)));               \
            xr_vm_outcome_dispose(&result);                                                        \
            arguments[1].as.member = 0;                                                            \
            result = xr_vm_code_execute(code, instance, entry, arguments, 2u);                     \
            REQUIRE(result.kind == XR_VM_OUTCOME_PANIC);                                           \
            REQUIRE(result.panic_value.kind == XR_VM_VALUE_PANIC_INFO);                            \
            REQUIRE(result.panic_value.as.panic_info.code == (remainder ? 421u : 420u));                \
            xr_vm_outcome_dispose(&result);                                                        \
        }                                                                                          \
    } while (0);
#include "../program/xr_program_integer_divmod_cases.inc.c"
#undef XR_INTEGER_DIVMOD_CASE
        xr_vm_code_free(code);
    }
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_array_append_execution(void) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_array_append_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    REQUIRE(xr_core_spec_operation_by_id(XR_CORE_OP_CORE_ARRAY_APPEND) != NULL);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
    XrVmValue arg = {.kind = XR_VM_VALUE_I64, .as.i64 = 42};
    for (unsigned i = 0u; i < 2u; ++i) {
        XrVmOutcome result = xr_vm_code_execute(code, instance, 0u, &arg, 1u);
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
        REQUIRE(result.value.as.i64 == 9);
        xr_vm_outcome_dispose(&result);
    }
    xr_vm_code_free(code);
    code = NULL;
    options.max_value_cells = 8u;
    REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
    XrVmOutcome limited = xr_vm_code_execute(code, instance, 0u, &arg, 1u);
    REQUIRE(limited.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
    xr_vm_outcome_dispose(&limited);
    xr_vm_code_free(code);
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_array_default_execution(void) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_array_default_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    REQUIRE(xr_core_spec_operation_by_id(XR_CORE_OP_CORE_ARRAY_ALLOCATE_DEFAULT) != NULL);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
    const int64_t counts[] = {3, 0, -1, INT64_MIN, INT64_MAX, 4};
    for (unsigned i = 0; i < XR_COUNTOF(counts); ++i) {
        XrVmValue arg = {.kind = XR_VM_VALUE_I64, .as.i64 = counts[i]};
        XrVmOutcome result = xr_vm_code_execute(code, instance, 0u, &arg, 1u);
        REQUIRE(result.kind == (counts[i] < 0 ? XR_VM_OUTCOME_PANIC :
            counts[i] == INT64_MAX ? XR_VM_OUTCOME_RESOURCE_LIMIT : XR_VM_OUTCOME_RETURN));
        if (counts[i] < 0) REQUIRE(result.panic_value.as.panic_info.code == 452u);
        else if (counts[i] != INT64_MAX) REQUIRE(result.value.as.i64 == counts[i]);
        xr_vm_outcome_dispose(&result);
    }
    xr_vm_code_free(code);
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_string_slice_execution(void) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_string_slice_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
    REQUIRE(xr_core_spec_operation_by_id(XR_CORE_OP_CORE_STRING_SLICE) != NULL);
    XrVmValue args[2] = {{.kind = XR_VM_VALUE_I64, .as.i64 = 1},
                         {.kind = XR_VM_VALUE_I64, .as.i64 = 4}};
    XrVmOutcome result = xr_vm_code_execute(code, instance, 0u, args, 2u);
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN && result.value.kind == XR_VM_VALUE_STRING);
    XrVmStringView view;
    static const uint8_t expected[] = {0xc3,0xa9,0xe4,0xb8,0xad,0xf0,0x9f,0x99,0x82};
    REQUIRE(xr_vm_value_string_view(&result.value, &view));
    REQUIRE(view.size == sizeof(expected) && memcmp(view.bytes, expected, sizeof(expected)) == 0);
    xr_vm_outcome_dispose(&result);
    args[0].as.i64 = 4;
    result = xr_vm_code_execute(code, instance, 0u, args, 2u);
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
    REQUIRE(xr_vm_value_string_view(&result.value, &view) && view.size == 0u);
    xr_vm_outcome_dispose(&result);
    args[0].as.i64 = -1;
    result = xr_vm_code_execute(code, instance, 0u, args, 2u);
    REQUIRE(result.kind == XR_VM_OUTCOME_PANIC && result.panic_value.as.panic_info.code == 430u);
    xr_vm_outcome_dispose(&result);
    args[0].as.i64 = 1;
    result = xr_vm_code_execute(code, instance, 0u, args, 2u);
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
    xr_vm_code_free(code);
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(instance, &diagnostic) == XR_EXECUTION_OK);
    require_coroutine_trap_generation_busy(instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    REQUIRE(xr_vm_value_string_view(&result.value, &view));
    REQUIRE(view.size == sizeof(expected) && memcmp(view.bytes, expected, sizeof(expected)) == 0);
    xr_vm_outcome_dispose(&result);
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_execution_instance_retire(instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, &diagnostic) == XR_EXECUTION_OK);
}

static void test_exact_integer_bitwise(void) {
    XrProgramArtifact artifact = {0};
    REQUIRE(xr_program_integer_bitwise_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
#define XR_INTEGER_BITWISE_CASE(type, member, mode, left, right, expected) \
    do { \
        uint32_t entry = xr_program_integer_bitwise_fixture_function(program, XR_CORE_TYPE_##type, mode); \
        REQUIRE(entry != UINT32_MAX); \
        REQUIRE(program->functions[entry].blocks[0].instructions[1].operation_id == XR_CORE_OP_CORE_INTEGER_BITWISE); \
        XrVmValue arguments[2] = {{.kind = XR_VM_VALUE_##type, .as.member = (left)}, \
                                  {.kind = XR_VM_VALUE_##type, .as.member = (right)}}; \
        XrVmOutcome result = xr_vm_code_execute(code, instance, entry, arguments, 2u); \
        REQUIRE(result.kind == XR_VM_OUTCOME_RETURN && result.value.kind == XR_VM_VALUE_##type); \
        REQUIRE(result.value.as.member == (expected)); \
        xr_vm_outcome_dispose(&result); \
    } while (0);
#include "../program/xr_program_integer_bitwise_cases.inc.c"
#undef XR_INTEGER_BITWISE_CASE
        xr_vm_code_free(code);
    }
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_coroutine_typed_outcomes_after_suspension(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    for (uint32_t scenario = 0u; scenario < 8u; ++scenario) {
        bool handled = scenario >= 4u;
        bool panics = (scenario & 1u) != 0u;
        XrProgramArtifact artifact = {0};
        REQUIRE(xr_program_coroutine_outcome_fixture_write(
                    (scenario & 2u) != 0u, panics,
                    handled ? XR_CORO_OUTCOME_HANDLED : XR_CORO_OUTCOME_VALID, &artifact, NULL,
                    0u) == XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
                XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        {
            XrInstance *instance = create_instance(program, profile, &bindings, 1u);
            XrVmCodeOptions options = xr_vm_code_default_options();
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            for (uint32_t iteration = 0u; iteration < 4u; ++iteration) {
                bool cancel = (iteration & 1u) != 0u;
                XrVmExecution *execution = NULL;
                REQUIRE(xr_vm_execution_create(code, instance,
                                               xr_validated_program_entry_function(program), NULL,
                                               0u, &execution));
                XrVmOutcome result = xr_vm_execution_step(execution);
                REQUIRE(result.kind == XR_VM_OUTCOME_SUSPENDED);
                REQUIRE(result.state_id == 1u && result.safepoint_id == 0u);
                xr_vm_outcome_dispose(&result);
                result =
                    cancel ? xr_vm_execution_cancel(execution) : xr_vm_execution_step(execution);
                REQUIRE(result.kind == (cancel    ? XR_VM_OUTCOME_CANCELLED
                                        : handled ? XR_VM_OUTCOME_RETURN
                                        : panics  ? XR_VM_OUTCOME_PANIC
                                                  : XR_VM_OUTCOME_ERROR));
                if (!cancel && !handled && panics) {
                    REQUIRE(result.panic_value.kind == XR_VM_VALUE_PANIC_INFO);
                    REQUIRE(result.panic_value.as.panic_info.code == 420u);
                } else if (!cancel && !handled) {
                    REQUIRE(result.error_value.kind == XR_VM_VALUE_I64);
                    REQUIRE(result.error_value.as.i64 == 42);
                }
                xr_vm_outcome_dispose(&result);
                xr_vm_execution_free(execution);
            }
            xr_vm_code_free(code);
            retire_and_free(&instance);
        }
        xr_validated_program_free(program);
    }
    xr_target_profile_free(profile);
}

static void test_array_element_places(void) {
    const unsigned scenarios[] = {0u, 10u, 11u, 12u, 13u, 14u, 0u, 2u, 3u, 5u, 8u};
    const int64_t indices[] = {0, 1, -1, INT64_MAX, INT64_MIN, 0};
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    for (size_t scenario = 0u; scenario < sizeof(scenarios) / sizeof(scenarios[0]); ++scenario) {
        XrProgramArtifact artifact = {0};
        REQUIRE((scenario < 6u
            ? xr_program_array_place_fixture_write(scenarios[scenario], &artifact)
            : xr_program_array_loan_fixture_write(scenarios[scenario], &artifact)) == XR_PROGRAM_BUILD_OK);
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        REQUIRE(xr_core_spec_operation_by_id(XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE)->vm_status ==
                XR_CORE_COVERAGE_COMPLETE);
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
        XrInstance *instance = create_instance(program, profile, &bindings, 1u);
        for (unsigned mode = 0u; mode < 2u; ++mode) {
            XrVmExecution *execution = NULL;
            uint32_t entry = xr_validated_program_entry_function(program);
            if (mode) REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
            XrVmOutcome result = mode ? xr_vm_execution_step(execution)
                                     : xr_vm_code_execute(code, instance, entry, NULL, 0u);
            if (scenario == 0u || scenario >= 6u) {
                REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
                REQUIRE(result.value.kind == XR_VM_VALUE_I64 && result.value.as.i64 == (scenario >= 6u ? 3 : 42));
            } else {
                REQUIRE(result.kind == XR_VM_OUTCOME_PANIC);
                REQUIRE(result.panic_value.kind == XR_VM_VALUE_PANIC_INFO);
                REQUIRE(result.panic_value.as.panic_info.code == 430u);
                REQUIRE(result.panic_value.as.panic_info.has_bounds);
                REQUIRE(result.panic_value.as.panic_info.index == indices[scenario]);
                REQUIRE(result.panic_value.as.panic_info.length == (scenarios[scenario] == 14u ? 0u : 1u));
            }
            xr_vm_outcome_dispose(&result);
            xr_vm_execution_free(execution);
        }
        xr_vm_code_free(code);
        retire_and_free(&instance);
        xr_validated_program_free(program);
    }
    xr_target_profile_free(profile);
}

static void test_byte_compare_execution(void) {
    for (unsigned scenario = 0u; scenario < 9u; ++scenario) {
        XrProgramArtifact artifact = {0}; XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_byte_compare_fixture_write(scenario, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        TestProviderBindings bindings; build_provider_bindings(profile, &bindings);
        for (unsigned repeat = 0u; repeat < 2u; ++repeat) {
            XrInstance *instance = create_instance(program, profile, &bindings, 1u);
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
            XrVmOutcome result = xr_vm_code_execute(code, instance, program->entry_function, NULL, 0u);
            REQUIRE(result.kind == XR_VM_OUTCOME_RETURN && result.value.kind == XR_VM_VALUE_BOOL);
            REQUIRE(result.value.as.boolean == (scenario == 0u || scenario == 1u || scenario == 6u));
            xr_vm_outcome_dispose(&result); xr_vm_code_free(code); retire_and_free(&instance);
        }
        xr_target_profile_free(profile); xr_validated_program_free(program);
    }
}

static void test_channel_storage_execution(void) {
    for (int64_t capacity = -1; capacity <= 2; ++capacity) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_channel_fixture_write(capacity, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        const XrValidatedInstruction *ops = program->functions[program->entry_function].blocks[0].instructions;
        REQUIRE(ops[1].operation_id == XR_CORE_OP_CORE_CHANNEL_CONSTRUCT);
        REQUIRE(ops[4].operation_id == XR_CORE_OP_CORE_CHANNEL_IS_CLOSED);
        XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        TestProviderBindings bindings;
        build_provider_bindings(profile, &bindings);
        for (unsigned budget = 1u; budget <= 2u; ++budget) {
            XrInstance *instance = create_instance(program, profile, &bindings, 1u);
            XrVmCodeOptions options = xr_vm_code_default_options();
            options.max_value_cells = budget == 1u ? 1u : 16u;
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            XrVmOutcome result = xr_vm_code_execute(code, instance, program->entry_function, NULL, 0u);
            if (capacity < 0 || budget == 1u) REQUIRE(result.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
            else {
                REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
                REQUIRE(result.value.kind == XR_VM_VALUE_BOOL && !result.value.as.boolean);
            }
            xr_vm_outcome_dispose(&result);
            xr_vm_code_free(code);
            retire_and_free(&instance);
        }
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
        xr_program_artifact_free(&artifact);
    }
}

static void test_atomic_storage_execution(void) {
    for (unsigned mode = 0u; mode <= 10u; ++mode)
    for (unsigned boolean = 0u; boolean < 3u; ++boolean) {
        if (boolean == 1u && mode >= 5u && mode != 7u && mode != 10u) continue;
        if (boolean == 2u && (mode == 7u || mode == 10u)) continue;
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE((boolean == 2u ? xr_program_atomic_fixture_write_scalar(XR_CORE_TYPE_F64, 0u, mode, &artifact) : mode ? xr_program_atomic_fixture_write_mode(boolean != 0u, 0u, mode, &artifact) : xr_program_atomic_fixture_write(boolean != 0u, 0u, &artifact)) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        const XrValidatedInstruction *ops = program->functions[program->entry_function].blocks[0].instructions;
        REQUIRE(ops[2].operation_id == XR_CORE_OP_CORE_ATOMIC_CONSTRUCT);
        REQUIRE(ops[4].operation_id == (mode >= 5u ? XR_CORE_OP_CORE_ATOMIC_UPDATE : mode ? XR_CORE_OP_CORE_ATOMIC_COMPARE_EXCHANGE : XR_CORE_OP_CORE_ATOMIC_EXCHANGE));
        REQUIRE(ops[6].operation_id == XR_CORE_OP_CORE_ATOMIC_LOAD);
        XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile != NULL);
        TestProviderBindings bindings;
        build_provider_bindings(profile, &bindings);
        for (unsigned budget = 1u; budget <= 2u; ++budget) {
            XrInstance *instance = create_instance(program, profile, &bindings, 1u);
            XrVmCodeOptions options = xr_vm_code_default_options();
            options.max_value_cells = budget == 1u ? 1u : program->functions[program->entry_function].value_count;
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            XrVmOutcome result = xr_vm_code_execute(code, instance, program->entry_function, NULL, 0u);
            if (budget == 1u) {
                REQUIRE(result.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
            } else {
                REQUIRE(result.kind == XR_VM_OUTCOME_RETURN);
                REQUIRE(result.value.kind == (boolean == 2u ? XR_VM_VALUE_F64 : boolean ? XR_VM_VALUE_BOOL : XR_VM_VALUE_I64));
                bool changed = mode == 0u || mode == 1u || mode == 5u || mode == 7u;
                if (boolean == 2u) {
                    uint64_t expected = mode == 6u ? UINT64_C(0x4043000000000000) :
                        changed ? UINT64_C(0x4045000000000000) : UINT64_C(0x4044000000000000);
                    REQUIRE(result.value.as.f64_bits == expected);
                } else REQUIRE(boolean ? result.value.as.boolean == changed : result.value.as.i64 == (mode == 6u ? 38 : changed ? 42 : 40));
            }
            xr_vm_outcome_dispose(&result);
            xr_vm_code_free(code);
            retire_and_free(&instance);
        }
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
        xr_program_artifact_free(&artifact);
    }
}

static void test_f64_scalar_execution(void) {
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
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
    XrVmOutcome result = xr_vm_code_execute(code, instance, program->entry_function, NULL, 0u);
    REQUIRE(result.kind == XR_VM_OUTCOME_RETURN && result.value.kind == XR_VM_VALUE_BOOL);
    REQUIRE(result.value.as.boolean);
    xr_vm_outcome_dispose(&result);
    xr_vm_code_free(code);
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

static void test_string_builder_execution(void) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_string_builder_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    const XrValidatedInstruction *ops = program->functions[program->entry_function].blocks[0].instructions;
    REQUIRE(ops[0].operation_id == XR_CORE_OP_CORE_STRING_BUILDER_CONSTRUCT);
    REQUIRE(ops[3].operation_id == XR_CORE_OP_CORE_STRING_BUILDER_APPEND);
    REQUIRE(ops[6].operation_id == XR_CORE_OP_CORE_STRING_BUILDER_SNAPSHOT);
    REQUIRE(ops[7].operation_id == XR_CORE_OP_CORE_STRING_BUILDER_CLEAR);
    REQUIRE(ops[8].operation_id == XR_CORE_OP_CORE_STRING_BUILDER_LENGTH);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    const uint8_t expected[] = {'A', 0, 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80};
    for (unsigned budget = 0u; budget < 2u; ++budget) {
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.max_value_cells = budget ? 1024u : 1u;
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
        for (unsigned independent = 0u; independent < 2u; ++independent) {
            XrInstance *instance = create_instance(program, profile, &bindings, 1u);
            XrVmOutcome result = xr_vm_code_execute(code, instance, program->entry_function, NULL, 0u);
            if (!budget) REQUIRE(result.kind == XR_VM_OUTCOME_RESOURCE_LIMIT);
            else {
                REQUIRE(result.kind == XR_VM_OUTCOME_RETURN && result.owns_dynamic_values);
                XrVmStringView view;
                REQUIRE(xr_vm_value_string_view(&result.value, &view));
                REQUIRE(view.size == sizeof(expected) && memcmp(view.bytes, expected, sizeof(expected)) == 0);
            }
            xr_vm_outcome_dispose(&result);
            retire_and_free(&instance);
        }
        xr_vm_code_free(code);
    }
    XrProgramArtifact call_artifact = {0};
    XrValidatedProgram *call_program = NULL;
    REQUIRE(xr_program_string_builder_call_fixture_write(0u, &call_artifact) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(call_artifact.bytes, call_artifact.size, NULL, &call_program, NULL) == XR_PROGRAM_VERIFY_OK);
    XrInstance *call_instance = create_instance(call_program, profile, &bindings, 1u);
    XrVmCode *call_code = NULL;
    REQUIRE(xr_vm_code_build(call_program, profile, NULL, &call_code, NULL) == XR_VM_CODE_OK);
    XrVmOutcome call_result = xr_vm_code_execute(call_code, call_instance, call_program->entry_function, NULL, 0u);
    REQUIRE(call_result.kind == XR_VM_OUTCOME_RETURN && call_result.value.kind == XR_VM_VALUE_I64);
    REQUIRE(call_result.value.as.i64 == 0);
    xr_vm_outcome_dispose(&call_result);
    xr_vm_code_free(call_code);
    retire_and_free(&call_instance);
    xr_validated_program_free(call_program);
    xr_program_artifact_free(&call_artifact);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    xr_program_artifact_free(&artifact);
}

int main(int argc, char **argv) {
    const XrCoreOperationSpec *byte_compare =
        xr_core_spec_operation_by_id(XR_CORE_OP_CORE_BYTES_TIMING_SAFE_EQUAL);
    REQUIRE(byte_compare && byte_compare->vm_status == XR_CORE_COVERAGE_COMPLETE &&
          byte_compare->decoder_status == XR_CORE_COVERAGE_COMPLETE);
    if (argc == 1) {
        test_string_builder_execution();
        test_f64_scalar_execution();
    }
    if (argc == 1) {
        test_byte_compare_execution();
        test_channel_storage_execution();
        test_atomic_storage_execution();
    }
    if (argc == 3 && strcmp(argv[1], "--h2-class-differential") == 0)
        return run_h2_class_differential_probe(argv[2]);
    if (argc != 1)
        return 2;
    test_array_element_places();
    test_sequence_length();
    for (unsigned scenario = 0u; scenario < 4u; ++scenario)
        test_array_values(scenario);
    test_array_alias_identity();
    test_exact_integer_value_abi();
    test_coroutine_typed_outcomes_after_suspension();
    test_exact_integer_conversions();
    test_exact_integer_divmod();
    test_exact_integer_bitwise();
    test_string_slice_execution();
    test_array_append_execution();
    test_array_default_execution();
    test_panic_point_chained_cleanup();
    test_coroutine_child_string_storage();
    test_coroutine_arithmetic_uses_ordinary_operation_semantics();
    test_coroutine_self_edge_parallel_arguments();
    test_reason_private_cleanup_graph_differential();
    test_aggregate_construct_owner_transfers();
    test_operation_semantics();
    test_provider_call_differential();
    test_provider_trap_continuation_differential();
    test_pipe_provider_call_differential();
    test_provider_output_differential();
    test_module_initializer_execution();
    test_module_instance_value_storage();
    test_module_slot_failures();
    test_returned_callable_owner();
    test_returned_existential_owner();
    test_one_shot_string_error_owner();
    test_module_reverse_publication_cleanup();
    test_module_initializer_budget_cleanup();
    test_module_suspended_initializer_cleanup();
    test_text_differential();
    test_sealed_invoke_and_cleanup_cfg();
    test_typed_panic_invoke_and_cleanup_cfg();
    test_condition_assert_panic_cleanup_cfg();
    test_existential_pack_test_project();
    test_existential_owned_read_reborrow();
    test_callable_pack_and_indirect_calls();
    test_arithmetic_edges();
    test_concurrent_execution_and_drain();
    test_policy_budget_generation_and_identity();
    test_record_reference_lifecycle();
    test_class_reference_semantics_differential();
    test_class_ref_receiver_direct_call();
    test_class_owned_exchange_and_self_assignment();
    test_class_trivial_field_load_is_snapshot_and_bounded();
    test_class_error_outcomes_retain_owners();
    test_coroutine_suspend_resume_generation_lease();
    test_coroutine_cancel_drops_exact_live_owner();
    test_coroutine_child_provider_failure_continuations();
    puts("task-299 typed XrProgram VM tests passed");
    return 0;
}
