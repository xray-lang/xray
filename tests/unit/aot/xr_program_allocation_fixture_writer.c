/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_allocation_fixture_writer.c - Verified typed allocation fixture
 */

#include "aot/program/xr_backend_ir.h"
#include "base/xchecks.h"
#include "base/xsha256.h"
#include "program/xr_reference_evaluator.h"
#include "program/xr_validated_program_internal.h"
#include "vm/xr_program_vm.h"
#include "../plan/target_profile_test_fixture.h"

#include <stdio.h>
#include <string.h>

enum {
    PLAIN = 100,
    INNER,
    TAGGED,
    CALLABLE,
    CAPTURE,
    ERASED,
    TYPE_COUNT = 6,
    FUNCTION_COUNT = 4,
    ENTRY_INSTRUCTION_LIMIT = 16,
};

typedef struct ReceiverFixture {
    XrCoreIrValueInput argument;
    XrCoreIrKey arguments[1];
    XrCoreIrKey returned[1];
    XrCoreIrInstructionInput instructions[3];
    XrCoreIrBlockInput block;
    uint16_t parameter;
    XrParamMode mode;
} ReceiverFixture;

typedef struct AllocationFixture {
    XrCoreIrTypeInput types[TYPE_COUNT];
    uint16_t scalar_field[1];
    uint16_t variant_field[1];
    uint16_t capture_fields[3];
    XrCoreIrVariantInput variants[2];
    XrCoreIrCallableSignatureInput callable_signature;
    XrCoreIrCallableSignatureInput interface_signature;
    uint16_t interface_parameter;
    XrParamMode interface_mode;
    XrCoreIrInterfaceInput interface;
    XrCoreIrConformanceInput conformance;
    XrCoreIrKey slot;
    ReceiverFixture receivers[3];
    XrCoreIrFunctionInput functions[FUNCTION_COUNT];
    XrCoreIrConstantInput constant;
    XrCoreIrKey values[10];
    XrCoreIrKey operands[ENTRY_INSTRUCTION_LIMIT][3];
    XrCoreIrInstructionInput instructions[ENTRY_INSTRUCTION_LIMIT];
    XrCoreIrBlockInput entry;
} AllocationFixture;

static XrCoreIrKey key(const char *name) {
    return xr_core_ir_key(name, strlen(name));
}

static XrCoreIrKey child_key(const char *name, const char *suffix) {
    char text[128];
    (void) snprintf(text, sizeof(text), "allocation:%s:%s", name, suffix);
    return key(text);
}

static void init_types(AllocationFixture *fixture) {
    const char *names[] = {"plain", "inner", "tagged", "callable", "capture", "erased"};
    for (uint32_t index = 0u; index < TYPE_COUNT; ++index) {
        fixture->types[index].key = child_key(names[index], "type");
        fixture->types[index].local_id = (uint16_t) (PLAIN + index);
    }
    fixture->scalar_field[0] = XR_CORE_TYPE_I64;
    fixture->variant_field[0] = CALLABLE;
    fixture->capture_fields[0] = TAGGED;
    fixture->capture_fields[1] = PLAIN;
    fixture->capture_fields[2] = XR_CORE_TYPE_I64;
    XrCoreIrTypeInput *plain = &fixture->types[PLAIN - PLAIN];
    plain->kind = XR_CORE_IR_TYPE_AGGREGATE;
    plain->nominal_kind = XR_CORE_IR_NOMINAL_STRUCT;
    plain->field_types = fixture->scalar_field;
    plain->field_count = 1u;
    XrCoreIrTypeInput *inner = &fixture->types[INNER - PLAIN];
    inner->kind = XR_CORE_IR_TYPE_AGGREGATE;
    inner->ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    inner->copy_contract = XR_CORE_IR_COPY_EXPLICIT;
    inner->field_types = fixture->scalar_field;
    inner->field_count = 1u;
    fixture->variants[0].payload_types = fixture->variant_field;
    fixture->variants[0].payload_count = 1u;
    XrCoreIrTypeInput *tagged = &fixture->types[TAGGED - PLAIN];
    tagged->kind = XR_CORE_IR_TYPE_VARIANT;
    tagged->ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    tagged->copy_contract = XR_CORE_IR_COPY_EXPLICIT;
    tagged->variants = fixture->variants;
    tagged->variant_count = 2u;
    fixture->callable_signature.result_type_id = XR_CORE_TYPE_I64;
    XrCoreIrTypeInput *callable = &fixture->types[CALLABLE - PLAIN];
    callable->kind = XR_CORE_IR_TYPE_CALLABLE;
    callable->ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    callable->copy_contract = XR_CORE_IR_COPY_EXPLICIT;
    callable->callable_signature = &fixture->callable_signature;
    XrCoreIrTypeInput *capture = &fixture->types[CAPTURE - PLAIN];
    capture->kind = XR_CORE_IR_TYPE_AGGREGATE;
    capture->ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    capture->copy_contract = XR_CORE_IR_COPY_EXPLICIT;
    capture->field_types = fixture->capture_fields;
    capture->field_count = 3u;
    XrCoreIrTypeInput *erased = &fixture->types[ERASED - PLAIN];
    erased->kind = XR_CORE_IR_TYPE_EXISTENTIAL;
    erased->existential_interface = key("allocation:interface");
    erased->interface_use_kind = XR_CORE_IR_INTERFACE_EXISTENTIAL_READ;
    fixture->interface_parameter = ERASED;
    fixture->interface_mode = XR_PARAM_READ;
    fixture->interface_signature = (XrCoreIrCallableSignatureInput) {
        .parameter_types = &fixture->interface_parameter,
        .parameter_modes = &fixture->interface_mode,
        .parameter_count = 1u,
        .has_receiver = true,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = XR_CORE_TYPE_I64,
    };
    fixture->interface = (XrCoreIrInterfaceInput) {
        .key = erased->existential_interface,
        .slots = &fixture->interface_signature,
        .slot_count = 1u,
    };
    fixture->slot = child_key("plain", "function");
    fixture->conformance = (XrCoreIrConformanceInput) {
        .key = key("allocation:conformance"),
        .implementor_type_id = PLAIN,
        .implementor_kind = XR_CORE_IR_NOMINAL_STRUCT,
        .interface_key = fixture->interface.key,
        .slot_functions = &fixture->slot,
        .slot_count = 1u,
    };
}

static void init_receiver(AllocationFixture *fixture, uint32_t index, const char *name,
                          uint16_t type, uint32_t field) {
    ReceiverFixture *receiver = &fixture->receivers[index];
    receiver->argument = (XrCoreIrValueInput) {.key = child_key(name, "argument"), .type_id = type};
    receiver->arguments[0] = receiver->argument.key;
    receiver->returned[0] = child_key(name, "result");
    receiver->instructions[0] =
        (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
                                    .operands = receiver->arguments,
                                    .operand_count = 1u};
    receiver->instructions[1] =
        (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
                                    .result = receiver->returned[0],
                                    .result_type_id = XR_CORE_TYPE_I64,
                                    .operands = receiver->arguments,
                                    .operand_count = 1u,
                                    .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
                                    .immediate.field_ordinal = field};
    receiver->instructions[2] = (XrCoreIrInstructionInput) {.operation_id = XR_CORE_OP_CORE_RETURN,
                                                            .operands = receiver->returned,
                                                            .operand_count = 1u};
    receiver->block = (XrCoreIrBlockInput) {.key = child_key(name, "block"),
                                            .arguments = &receiver->argument,
                                            .argument_count = 1u,
                                            .instructions = receiver->instructions,
                                            .instruction_count = 3u};
    receiver->parameter = type;
    receiver->mode = XR_PARAM_READ;
    fixture->functions[index] = (XrCoreIrFunctionInput) {.key = child_key(name, "function"),
                                                         .parameter_types = &receiver->parameter,
                                                         .parameter_modes = &receiver->mode,
                                                         .parameter_count = 1u,
                                                         .has_receiver = true,
                                                         .receiver_mode = XR_PARAM_READ,
                                                         .result_type_id = XR_CORE_TYPE_I64,
                                                         .entry_block = receiver->block.key,
                                                         .blocks = &receiver->block,
                                                         .block_count = 1u};
}

static XrCoreIrInstructionInput *entry_op(AllocationFixture *fixture, uint16_t operation,
                                          uint32_t result, uint16_t type, uint32_t operand,
                                          bool owner) {
    XR_CHECK(fixture->entry.instruction_count < ENTRY_INSTRUCTION_LIMIT,
             "allocation fixture instruction capacity exhausted");
    XR_CHECK(type == XR_CORE_TYPE_VOID || result < 10u, "allocation fixture value is invalid");
    uint32_t index = fixture->entry.instruction_count++;
    XrCoreIrInstructionInput *instruction = &fixture->instructions[index];
    instruction->operation_id = operation;
    instruction->result_type_id = type;
    if (type != XR_CORE_TYPE_VOID)
        instruction->result = fixture->values[result];
    instruction->result_ownership = owner ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER;
    if (operand < 10u) {
        fixture->operands[index][0] = fixture->values[operand];
        instruction->operands = fixture->operands[index];
        instruction->operand_count = 1u;
    }
    return instruction;
}

static void init_entry(AllocationFixture *fixture) {
    for (uint32_t index = 0u; index < 10u; ++index) {
        char name[32];
        (void) snprintf(name, sizeof(name), "value-%u", index);
        fixture->values[index] = child_key(name, "entry");
    }
    fixture->constant = (XrCoreIrConstantInput) {.key = key("allocation:constant"),
                                                 .type_id = XR_CORE_TYPE_I64,
                                                 .kind = XR_CORE_IR_CONSTANT_I64,
                                                 .value.i64 = 42};
    fixture->entry.key = key("allocation:entry:block");
    fixture->entry.instructions = fixture->instructions;
    (void) entry_op(fixture, XR_CORE_OP_CORE_BLOCK_ARGUMENT, 0u, XR_CORE_TYPE_VOID, 10u, false);
    XrCoreIrInstructionInput *op =
        entry_op(fixture, XR_CORE_OP_CORE_CONSTANT_I64, 0u, XR_CORE_TYPE_I64, 10u, false);
    op->immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT;
    op->immediate.key = fixture->constant.key;
    (void) entry_op(fixture, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT, 1u, PLAIN, 0u, false);
    (void) entry_op(fixture, XR_CORE_OP_CORE_EXISTENTIAL_PACK, 2u, ERASED, 1u, false);
    (void) entry_op(fixture, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT, 3u, INNER, 0u, true);
    op = entry_op(fixture, XR_CORE_OP_CORE_CALLABLE_PACK, 4u, CALLABLE, 3u, true);
    op->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
    op->immediate.key = fixture->functions[1].key;
    op = entry_op(fixture, XR_CORE_OP_CORE_VARIANT_CONSTRUCT, 5u, TAGGED, 4u, true);
    op->immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT;
    op->immediate.variant_ordinal = 0u;
    op = entry_op(fixture, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT, 6u, CAPTURE, 5u, true);
    fixture->operands[fixture->entry.instruction_count - 1u][1] = fixture->values[1];
    fixture->operands[fixture->entry.instruction_count - 1u][2] = fixture->values[0];
    op->operand_count = 3u;
    op = entry_op(fixture, XR_CORE_OP_CORE_CALLABLE_PACK, 7u, CALLABLE, 6u, true);
    op->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
    op->immediate.key = fixture->functions[2].key;
    (void) entry_op(fixture, XR_CORE_OP_CORE_OWNER_COPY, 8u, CALLABLE, 7u, true);
    (void) entry_op(fixture, XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT, 9u, XR_CORE_TYPE_I64, 8u, false);
    (void) entry_op(fixture, XR_CORE_OP_CORE_OWNER_DROP, 0u, XR_CORE_TYPE_VOID, 8u, false);
    (void) entry_op(fixture, XR_CORE_OP_CORE_OWNER_DROP, 0u, XR_CORE_TYPE_VOID, 7u, false);
    (void) entry_op(fixture, XR_CORE_OP_CORE_RETURN, 0u, XR_CORE_TYPE_VOID, 9u, false);
    fixture->functions[3] =
        (XrCoreIrFunctionInput) {.key = key("allocation:entry"),
                                 .result_type_id = XR_CORE_TYPE_I64,
                                 .effect_mask = XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_TRAP,
                                 .entry_block = fixture->entry.key,
                                 .blocks = &fixture->entry,
                                 .block_count = 1u,
                                 .flags = XR_PROGRAM_FUNCTION_ENTRY};
}

static XrValidatedProgram *build_program(void) {
    AllocationFixture fixture = {0};
    init_types(&fixture);
    init_receiver(&fixture, 0u, "plain", PLAIN, 0u);
    init_receiver(&fixture, 1u, "inner", INNER, 0u);
    init_receiver(&fixture, 2u, "capture", CAPTURE, 2u);
    init_entry(&fixture);
    XrCoreIrModuleInput module = {.key = key("allocation:module"),
                                  .constants = &fixture.constant,
                                  .constant_count = 1u,
                                  .functions = fixture.functions,
                                  .function_count = FUNCTION_COUNT};
    XrCoreIrKey semantic = key("allocation:semantic");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = semantic.bytes,
                                  .required_features = &feature,
                                  .required_feature_count = 1u,
                                  .types = fixture.types,
                                  .type_count = TYPE_COUNT,
                                  .interfaces = &fixture.interface,
                                  .interface_count = 1u,
                                  .conformances = &fixture.conformance,
                                  .conformance_count = 1u,
                                  .modules = &module,
                                  .module_count = 1u};
    XrCoreIrProgram *core = NULL;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic verify = {0};
    char diagnostic[256] = {0};
    bool ok =
        xr_core_ir_program_build(&input, &core, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK &&
        xr_program_write(core, &artifact, diagnostic, sizeof(diagnostic)) == XR_PROGRAM_BUILD_OK &&
        xr_program_validate(artifact.bytes, artifact.size, NULL, &program, &verify) ==
            XR_PROGRAM_VERIFY_OK;
    if (!ok) {
        fprintf(stderr,
                "allocation Program rejected: %s; kind=%u function=%u instruction=%u value=%u\n",
                diagnostic, (unsigned) verify.kind, verify.location.function_id,
                verify.location.instruction_id, verify.location.value_id);
        xr_validated_program_free(program);
        program = NULL;
    }
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(core);
    return program;
}

static uint16_t find_type(const XrValidatedProgram *program, const char *name) {
    XrCoreIrKey expected = child_key(name, "type");
    uint16_t found = 0u;
    for (uint32_t index = 0u; index < program->type_count; ++index) {
        if (memcmp(program->types[index].key.bytes, expected.bytes, sizeof(expected.bytes)) == 0) {
            if (found != 0u)
                return 0u;
            found = program->types[index].type_id;
        }
    }
    return found;
}

static bool has_exact_producers(const XrValidatedProgram *program) {
    uint32_t existential = 0u, pack = 0u, copy = 0u;
    for (uint32_t f = 0u; f < program->function_count; ++f) {
        const XrValidatedFunction *function = &program->functions[f];
        for (uint32_t b = 0u; b < function->block_count; ++b) {
            const XrValidatedBlock *block = &function->blocks[b];
            for (uint32_t i = 0u; i < block->instruction_count; ++i) {
                const XrValidatedInstruction *op = &block->instructions[i];
                if (op->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PACK)
                    ++existential;
                if (op->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK && op->operand_count == 1u)
                    ++pack;
                const XrValidatedType *type =
                    xr_validated_program_type(program, op->result_type_id);
                if (op->operation_id == XR_CORE_OP_CORE_OWNER_COPY && type &&
                    type->kind == XR_CORE_IR_TYPE_CALLABLE)
                    ++copy;
            }
        }
    }
    return program->type_count == TYPE_COUNT && program->function_count == FUNCTION_COUNT &&
           existential == 1u && pack == 2u && copy == 1u;
}

static bool write_bytes(const char *path, const char *bytes, size_t size) {
    FILE *output = fopen(path, "wb");
    if (!output)
        return false;
    bool ok = fwrite(bytes, 1u, size, output) == size;
    return fclose(output) == 0 && ok;
}

static bool write_facts(const char *path, const XrValidatedProgram *program,
                        const XrGeneratedC *generated) {
    uint16_t plain = find_type(program, "plain");
    uint16_t inner = find_type(program, "inner");
    uint16_t capture = find_type(program, "capture");
    if (plain == 0u || inner == 0u || capture == 0u || !has_exact_producers(program))
        return false;
    uint8_t digest[32];
    char hex[65];
    xr_sha256((const uint8_t *) generated->bytes, generated->size, digest);
    for (size_t index = 0u; index < sizeof(digest); ++index)
        (void) snprintf(hex + index * 2u, 3u, "%02x", (unsigned) digest[index]);
    char facts[512];
    int size = snprintf(facts, sizeof(facts),
                        "{\"schema\":1,\"producer\":\"program-allocation-v1\","
                        "\"entry\":%u,\"plain_type\":%u,\"inner_type\":%u,\"capture_type\":%u,"
                        "\"existential_pack\":1,\"capturing_pack\":2,\"callable_copy\":1,"
                        "\"allocations\":5,\"sha256\":\"%s\"}\n",
                        xr_validated_program_entry_function(program), (unsigned) plain,
                        (unsigned) inner, (unsigned) capture, hex);
    return size > 0 && (size_t) size < sizeof(facts) && write_bytes(path, facts, (size_t) size);
}

static bool verify_execution(XrValidatedProgram *program, XrTargetProfile *profile) {
    uint32_t entry = xr_validated_program_entry_function(program);
    XrReferenceProfile reference_profile = {.pointer_width = 64u};
    XrReferenceOutcome reference =
        xr_reference_evaluate(program, entry, NULL, 0u, &reference_profile, NULL);
    bool ok = reference.kind == XR_REFERENCE_OUTCOME_RETURN &&
              reference.value.kind == XR_REFERENCE_VALUE_I64 && reference.value.as.i64 == 42;
    uint64_t expected_steps = reference.steps;
    xr_reference_outcome_dispose(&reference);
    XrExecutionBindingInput input = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                                     .program = program,
                                     .profile = profile,
                                     .generation = 1u};
    XrExecutionDiagnostic diagnostic = {0};
    XrInstance *instance = NULL;
    if (!ok || xr_execution_instance_create(&input, &instance, &diagnostic) != XR_EXECUTION_OK)
        return false;
    if (ok) {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic code_diagnostic = {0};
        ok = xr_vm_code_build(program, profile, &options, &code, &code_diagnostic) == XR_VM_CODE_OK;
        if (ok) {
            XrVmOutcome result = xr_vm_code_execute(code, instance, entry, NULL, 0u);
            ok = result.kind == XR_VM_OUTCOME_RETURN && result.value.kind == XR_VM_VALUE_I64 &&
                 result.value.as.i64 == 42 && result.steps == expected_steps;
            xr_vm_outcome_dispose(&result);
        }
        xr_vm_code_free(code);
    }
    bool disposed = xr_execution_instance_begin_drain(instance, &diagnostic) == XR_EXECUTION_OK &&
                    xr_execution_instance_retire(instance, &diagnostic) == XR_EXECUTION_OK &&
                    xr_execution_instance_free(&instance, &diagnostic) == XR_EXECUTION_OK;
    return ok && disposed;
}

int main(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[1], "--output") != 0 || strcmp(argv[3], "--facts") != 0 ||
        argv[2][0] == '\0' || argv[4][0] == '\0' || strcmp(argv[2], argv[4]) == 0) {
        fprintf(stderr, "expected --output GENERATED_C --facts JSON\n");
        return 2;
    }
    XrValidatedProgram *program = build_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic = {0};
    XrBackendIR *ir = NULL;
    XrGeneratedC generated = {0}, repeated = {0};
    bool ok =
        program && profile && has_exact_producers(program) && verify_execution(program, profile) &&
        xr_backend_ir_build(program, profile, &options, &ir, &diagnostic) == XR_BACKEND_OK &&
        xr_backend_ir_verify(ir, &diagnostic) && xr_backend_ir_binding_verify(ir, &diagnostic) &&
        xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) == XR_BACKEND_OK &&
        xr_backend_ir_emit_c(ir, false, &repeated, &diagnostic) == XR_BACKEND_OK &&
        generated.size == repeated.size &&
        memcmp(generated.bytes, repeated.bytes, generated.size) == 0 &&
        write_bytes(argv[2], generated.bytes, generated.size) &&
        write_facts(argv[4], program, &generated);
    if (!ok)
        fprintf(stderr, "allocation fixture generation failed; backend status=%u\n",
                (unsigned) diagnostic.status);
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
    return ok ? 0 : 1;
}
