/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir.c - Validated-program to private AOT realization lowering
 */

#include "xr_backend_ir_internal.h"

#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../core/xr_core_spec_gen.h"

#include <limits.h>
#include <string.h>


static bool operation_is_supported(uint16_t operation_id) {
    switch (operation_id) {
        case XR_CORE_OP_CORE_PLACE_MODULE:
        case XR_CORE_OP_CORE_PLACE_INITIALIZE:
        case XR_CORE_OP_CORE_CONSTANT_F64:
        case XR_CORE_OP_CORE_CONSTANT_I64:
        case XR_CORE_OP_CORE_CONSTANT_BOOL:
        case XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM:
        case XR_CORE_OP_CORE_CONSTANT_STRING:
        case XR_CORE_OP_CORE_CONSTANT_RUNE:
        case XR_CORE_OP_CORE_COMPARE_STRING:
        case XR_CORE_OP_CORE_COMPARE_RUNE:
        case XR_CORE_OP_CORE_INTEGER_DIVMOD:
        case XR_CORE_OP_CORE_SCALAR_BITCAST64:
        case XR_CORE_OP_CORE_INTEGER_CONVERT:
        case XR_CORE_OP_CORE_STRING_FROM_SCALAR:
        case XR_CORE_OP_CORE_STRING_CONCAT:
        case XR_CORE_OP_CORE_SEQUENCE_LENGTH:
        case XR_CORE_OP_CORE_SEQUENCE_ELEMENT_PLACE:
        case XR_CORE_OP_CORE_ADD_I64:
        case XR_CORE_OP_CORE_SUB_I64:
        case XR_CORE_OP_CORE_MUL_I64:
        case XR_CORE_OP_CORE_DIV_I64:
        case XR_CORE_OP_CORE_LOGICAL_NOT:
        case XR_CORE_OP_CORE_LOGICAL_AND:
        case XR_CORE_OP_CORE_LOGICAL_OR:
        case XR_CORE_OP_CORE_COMPARE_F64:
        case XR_CORE_OP_CORE_COMPARE_I64:
        case XR_CORE_OP_CORE_COMPARE_TARGET_ENUM:
        case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
        case XR_CORE_OP_CORE_BRANCH:
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH:
        case XR_CORE_OP_CORE_ASSERT_CONDITION:
        case XR_CORE_OP_CORE_RETURN:
        case XR_CORE_OP_CORE_CANCEL_PUBLISH:
        case XR_CORE_OP_CORE_COROUTINE_YIELD:
        case XR_CORE_OP_CORE_COROUTINE_SUSPEND:
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
        case XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT:
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
        case XR_CORE_OP_CORE_TRAP:
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
        case XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM:
        case XR_CORE_OP_CORE_TARGET_ARCHITECTURE:
        case XR_CORE_OP_CORE_TARGET_NATIVE_ABI:
        case XR_CORE_OP_CORE_TARGET_ENDIANNESS:
        case XR_CORE_OP_CORE_PROVIDER_CALL:
        case XR_CORE_OP_CORE_OUTPUT_GROUP:
        case XR_CORE_OP_CORE_ATOMIC_CONSTRUCT:
        case XR_CORE_OP_CORE_ATOMIC_LOAD:
        case XR_CORE_OP_CORE_ATOMIC_EXCHANGE:
        case XR_CORE_OP_CORE_ATOMIC_COMPARE_EXCHANGE:
        case XR_CORE_OP_CORE_ATOMIC_UPDATE:
        case XR_CORE_OP_CORE_ARRAY_CONSTRUCT:
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT:
        case XR_CORE_OP_CORE_AGGREGATE_PROJECT:
        case XR_CORE_OP_CORE_AGGREGATE_UPDATE:
        case XR_CORE_OP_CORE_VARIANT_CONSTRUCT:
        case XR_CORE_OP_CORE_VARIANT_TEST:
        case XR_CORE_OP_CORE_VARIANT_PROJECT:
        case XR_CORE_OP_CORE_EXISTENTIAL_PACK:
        case XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ:
        case XR_CORE_OP_CORE_EXISTENTIAL_TEST:
        case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT:
        case XR_CORE_OP_CORE_CALLABLE_PACK:
        case XR_CORE_OP_CORE_OWNER_COPY:
        case XR_CORE_OP_CORE_OWNER_MOVE:
        case XR_CORE_OP_CORE_OWNER_DROP:
        case XR_CORE_OP_CORE_PLACE_LOCAL:
        case XR_CORE_OP_CORE_PLACE_LOAD:
        case XR_CORE_OP_CORE_PLACE_STORE:
        case XR_CORE_OP_CORE_PLACE_PROJECT:
        case XR_CORE_OP_CORE_PLACE_TAKE:
        case XR_CORE_OP_CORE_CLASS_CONSTRUCT:
        case XR_CORE_OP_CORE_OWNER_ALIAS:
        case XR_CORE_OP_CORE_CLASS_FIELD_LOAD:
        case XR_CORE_OP_CORE_CLASS_FIELD_PLACE:
        case XR_CORE_OP_CORE_PLACE_EXCHANGE:
            return true;
        default:
            return false;
    }
}

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (size_t index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t) (value >> (index * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (size_t index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (uint8_t) (value >> (index * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static XrFingerprint hash_text(const char *domain, const void *bytes, size_t size) {
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    xr_sha256_update(&context, (const uint8_t *) domain, strlen(domain));
    if (bytes && size != 0u)
        xr_sha256_update(&context, bytes, size);
    xr_sha256_final(&context, result.bytes);
    return result;
}

XrBackendId xr_backend_compute_id(void) {
    const uint8_t backend_version[] = {XR_AOT_BACKEND_VERSION, 0u, 0u, 0u};
    return hash_text(XR_AOT_BACKEND_NAME, backend_version, sizeof(backend_version));
}

XrOptimizationPolicyId xr_backend_compute_optimization_policy_id(const XrBackendOptions *options) {
    uint8_t bytes[8] = {0};
    bytes[0] = (uint8_t) options->schema_version;
    bytes[1] = (uint8_t) (options->schema_version >> 8u);
    bytes[2] = (uint8_t) (options->schema_version >> 16u);
    bytes[3] = (uint8_t) (options->schema_version >> 24u);
    bytes[4] = options->optimization_policy;
    return hash_text("xray:aot:optimization-policy:v1", bytes, sizeof(bytes));
}

void xr_backend_set_diagnostic(XrBackendDiagnostic *diagnostic, XrBackendStatus status,
                               uint16_t operation_id, uint32_t function_id, uint32_t block_id,
                               uint32_t instruction_id) {
    if (!diagnostic)
        return;
    *diagnostic = (XrBackendDiagnostic) {.status = status,
                                         .operation_id = operation_id,
                                         .function_id = function_id,
                                         .block_id = block_id,
                                         .instruction_id = instruction_id};
}

bool xr_backend_representation_for_type(uint16_t type_id, uint8_t *representation_out) {
    uint8_t representation = XR_BACKEND_VALUE_VOID;
    switch (type_id) {
        case XR_CORE_TYPE_I8:
            representation = XR_BACKEND_VALUE_I8;
            break;
        case XR_CORE_TYPE_U8:
            representation = XR_BACKEND_VALUE_U8;
            break;
        case XR_CORE_TYPE_I16:
            representation = XR_BACKEND_VALUE_I16;
            break;
        case XR_CORE_TYPE_I32:
            representation = XR_BACKEND_VALUE_I32;
            break;
        case XR_CORE_TYPE_F64:
            representation = XR_BACKEND_VALUE_F64;
            break;
        case XR_CORE_TYPE_U64:
            representation = XR_BACKEND_VALUE_U64;
            break;
        case XR_CORE_TYPE_VOID:
            representation = XR_BACKEND_VALUE_VOID;
            break;
        case XR_CORE_TYPE_BOOL:
            representation = XR_BACKEND_VALUE_BOOL_U8;
            break;
        case XR_CORE_TYPE_I64:
            representation = XR_BACKEND_VALUE_I64;
            break;
        case XR_CORE_TYPE_U32:
            representation = XR_BACKEND_VALUE_U32;
            break;
        case XR_CORE_TYPE_U16:
            representation = XR_BACKEND_VALUE_U16;
            break;
        case XR_CORE_TYPE_TARGET_OS:
        case XR_CORE_TYPE_TARGET_ARCH:
        case XR_CORE_TYPE_TARGET_ABI:
        case XR_CORE_TYPE_TARGET_ENDIAN:
            representation = XR_BACKEND_VALUE_TARGET_ENUM_U16;
            break;
        case XR_CORE_TYPE_ERROR:
            representation = XR_BACKEND_VALUE_ERROR_U32;
            break;
        case XR_CORE_TYPE_PANIC_INFO:
            representation = XR_BACKEND_VALUE_PANIC_U32;
            break;
        case XR_CORE_TYPE_STRING:
            representation = XR_BACKEND_VALUE_STRING_HANDLE;
            break;
        case XR_CORE_TYPE_RUNE:
            representation = XR_BACKEND_VALUE_RUNE_U32;
            break;
        default:
            if (type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
                return false;
            representation = XR_BACKEND_VALUE_AGGREGATE;
            break;
    }
    if (representation_out)
        *representation_out = representation;
    return true;
}

bool xr_backend_representation_for_program_type(const XrValidatedProgram *program,
                                                uint16_t type_id,
                                                uint8_t *representation_out) {
    const XrValidatedType *type = xr_validated_program_type(program, type_id);
    if (type && xr_program_type_kind_is_reference_record(type->kind)) {
        if (representation_out)
            *representation_out = XR_BACKEND_VALUE_CLASS_HANDLE;
        return true;
    }
    return xr_backend_representation_for_type(type_id, representation_out);
}

static bool class_graph_is_acyclic(const XrValidatedProgram *program, uint32_t index,
                                   uint8_t *state) {
    if (state[index] == 2u)
        return true;
    if (state[index] == 1u)
        return false;
    state[index] = 1u;
    const XrValidatedType *type = &program->types[index];
    for (uint32_t field = 0u; field < type->field_count; ++field) {
        const XrValidatedType *child =
            xr_validated_program_type(program, type->field_types[field]);
        if (!child)
            continue;
        uint32_t child_index = child->type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
        if (child_index >= program->type_count ||
            !class_graph_is_acyclic(program, child_index, state))
            return false;
    }
    for (uint32_t variant = 0u; variant < type->variant_count; ++variant) {
        const XrValidatedVariant *row = &type->variants[variant];
        for (uint32_t field = 0u; field < row->payload_count; ++field) {
            const XrValidatedType *child =
                xr_validated_program_type(program, row->payload_types[field]);
            if (child && !class_graph_is_acyclic(
                             program, child->type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
                return false;
        }
    }
    if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
        for (uint32_t f = 0u; f < program->function_count; ++f) {
            const XrValidatedFunction *function = &program->functions[f];
            for (uint32_t b = 0u; b < function->block_count; ++b) {
                const XrValidatedBlock *block = &function->blocks[b];
                for (uint32_t i = 0u; i < block->instruction_count; ++i) {
                    const XrValidatedInstruction *op = &block->instructions[i];
                    if (op->operation_id != XR_CORE_OP_CORE_CALLABLE_PACK ||
                        op->result_type_id != type->type_id || op->operand_count == 0u)
                        continue;
                    const XrValidatedFunction *target =
                        &program->functions[op->immediate.function_id];
                    const XrValidatedType *child =
                        xr_validated_program_type(program, target->parameter_types[0]);
                    if (child &&
                        !class_graph_is_acyclic(
                            program, child->type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
                        return false;
                }
            }
        }
    } else if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
               type->ownership == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE) {
        for (uint32_t conformance = 0u; conformance < program->conformance_count; ++conformance) {
            const XrValidatedConformance *row = &program->conformances[conformance];
            const XrValidatedType *child =
                xr_validated_program_type(program, row->implementor_type_id);
            if (row->interface_id == type->interface_id && child &&
                !class_graph_is_acyclic(program, child->type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE,
                                        state))
                return false;
        }
    }
    state[index] = 2u;
    return true;
}

static XrBackendStatus class_storage_status(const XrValidatedProgram *program) {
    if (!program)
        return XR_BACKEND_INVALID_INPUT;
    bool has_class = false;
    for (uint32_t index = 0u; index < program->type_count; ++index) {
        const XrValidatedType *type = &program->types[index];
        if (!xr_program_type_kind_is_reference_record(type->kind))
            continue;
        has_class = true;
    }
    if (!has_class)
        return XR_BACKEND_OK;
    uint8_t *state = xr_calloc(program->type_count ? program->type_count : 1u, sizeof(*state));
    if (!state)
        return XR_BACKEND_OUT_OF_MEMORY;
    bool acyclic = true;
    for (uint32_t index = 0u; acyclic && index < program->type_count; ++index)
        if (xr_program_type_kind_is_reference_record(program->types[index].kind))
            acyclic = class_graph_is_acyclic(program, index, state);
    xr_free(state);
    return acyclic ? XR_BACKEND_OK : XR_BACKEND_UNSUPPORTED_OPERATION;
}

XrBackendOptions xr_backend_default_options(void) {
    return (XrBackendOptions) {.schema_version = XR_BACKEND_IR_SCHEMA_VERSION,
                               .optimization_policy = XR_BACKEND_OPTIMIZATION_PORTABLE,
                               .max_functions = UINT32_C(65536),
                               .max_blocks = UINT32_C(1000000),
                               .max_instructions = UINT32_C(10000000),
                               .max_values = UINT32_C(10000000)};
}

static bool options_valid(const XrBackendOptions *options) {
    return options && options->schema_version == XR_BACKEND_IR_SCHEMA_VERSION &&
           (options->optimization_policy == XR_BACKEND_OPTIMIZATION_NONE ||
            options->optimization_policy == XR_BACKEND_OPTIMIZATION_PORTABLE) &&
           options->max_functions != 0u && options->max_blocks != 0u &&
           options->max_instructions != 0u && options->max_values != 0u;
}

/* Program identity already binds the immutable logical graph. The realization
 * digest binds only backend policy, exact machine facts and its bounded scan. */
void xr_backend_compute_lowering_digest(const XrBackendIR *ir, XrFingerprint *digest_out) {
    XrSHA256Context context;
    xr_sha256_init(&context);
    const char domain[] = "xray:aot:realization:v2";
    xr_sha256_update(&context, (const uint8_t *) domain, sizeof(domain));
    xr_sha256_update(&context, ir->execution_id.bytes, sizeof(ir->execution_id.bytes));
    xr_sha256_update(&context, ir->backend_id.bytes, sizeof(ir->backend_id.bytes));
    xr_sha256_update(&context, ir->optimization_policy_id.bytes,
                     sizeof(ir->optimization_policy_id.bytes));
    hash_u32(&context, ir->pointer_width);
    hash_u32(&context, ir->operating_system);
    hash_u32(&context, ir->architecture);
    hash_u32(&context, ir->native_abi);
    hash_u32(&context, ir->endianness);
    hash_u64(&context, (uint64_t) ir->instruction_count);
    xr_sha256_final(&context, digest_out->bytes);
}

XrBackendStatus xr_backend_ir_build(const XrValidatedProgram *program,
                                    const XrTargetProfile *profile, const XrBackendOptions *options,
                                    XrBackendIR **ir_out, XrBackendDiagnostic *diagnostic_out) {
    if (ir_out)
        *ir_out = NULL;
    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OK, 0u, 0u, 0u, 0u);
    if (!program || !profile || !ir_out || !options_valid(options) ||
        !xr_target_profile_verify(profile, NULL, 0)) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVALID_INPUT, 0u, 0u, 0u, 0u);
        return XR_BACKEND_INVALID_INPUT;
    }
    XrBackendStatus class_status = class_storage_status(program);
    if (class_status != XR_BACKEND_OK) {
        xr_backend_set_diagnostic(diagnostic_out, class_status,
                                  XR_CORE_OP_CORE_CLASS_CONSTRUCT, 0u, 0u, 0u);
        return class_status;
    }
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    XrExecutionId execution_id;
    if (!machine || !xr_execution_id_compute(program, profile, &execution_id)) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVALID_INPUT, 0u, 0u, 0u, 0u);
        return XR_BACKEND_INVALID_INPUT;
    }
    XrBackendIR *ir = xr_calloc(1u, sizeof(*ir));
    if (!ir) {
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OUT_OF_MEMORY, 0u, 0u, 0u, 0u);
        return XR_BACKEND_OUT_OF_MEMORY;
    }
    atomic_init(&ir->references, 1u);
    ir->program = xr_validated_program_retain(program);
    ir->profile = xr_target_profile_retain(profile);
    ir->execution_id = execution_id;
    ir->backend_id = xr_backend_compute_id();
    ir->optimization_policy_id = xr_backend_compute_optimization_policy_id(options);
    ir->options = *options;
    ir->pointer_width = machine ? (uint16_t) (machine->data_layout.pointer.size * UINT16_C(8)) : 0u;
    ir->operating_system = machine ? machine->operating_system : 0u;
    ir->architecture = machine ? machine->architecture : 0u;
    ir->native_abi = machine ? machine->native_abi : 0u;
    ir->endianness = machine ? (uint16_t) machine->data_layout.endian : 0u;
    if (program->function_count > options->max_functions ||
        (ir->pointer_width != 32u && ir->pointer_width != 64u)) {
        xr_backend_ir_free(ir);
        xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_RESOURCE_LIMIT, 0u, 0u, 0u, 0u);
        return XR_BACKEND_RESOURCE_LIMIT;
    }
    uint64_t blocks = 0u;
    uint64_t instructions = 0u;
    uint64_t values = 0u;
    for (uint32_t function = 0; function < program->function_count; ++function) {
        const XrValidatedFunction *source = &program->functions[function];
        blocks += source->block_count;
        values += source->value_count;
        for (uint32_t block = 0; block < source->block_count; ++block) {
            instructions += source->blocks[block].instruction_count;
            for (uint32_t instruction = 0; instruction < source->blocks[block].instruction_count;
                 ++instruction) {
                uint16_t operation_id =
                    source->blocks[block].instructions[instruction].operation_id;
                const XrValidatedInstruction *operation =
                    &source->blocks[block].instructions[instruction];
                const XrValidatedType *result_type =
                    xr_validated_program_type(program, operation->result_type_id);
                bool deferred_class_copy = operation_id == XR_CORE_OP_CORE_OWNER_COPY &&
                                           result_type &&
                                           xr_program_type_kind_is_reference_record(result_type->kind);
                if (!operation_is_supported(operation_id) || deferred_class_copy) {
                    xr_backend_ir_free(ir);
                    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_UNSUPPORTED_OPERATION,
                                              operation_id, function, block, instruction);
                    return XR_BACKEND_UNSUPPORTED_OPERATION;
                }
            }
        }
        if (blocks > options->max_blocks || instructions > options->max_instructions ||
            values > options->max_values) {
            xr_backend_ir_free(ir);
            XrBackendStatus status = XR_BACKEND_RESOURCE_LIMIT;
            xr_backend_set_diagnostic(diagnostic_out, status, 0u, function, 0u, 0u);
            return status;
        }
    }
    ir->instruction_count = (size_t) instructions;
    xr_backend_compute_lowering_digest(ir, &ir->lowering_digest);
    bool verified = xr_backend_ir_verify(ir, diagnostic_out);
    if (!verified) {
        XrBackendStatus status =
            diagnostic_out ? diagnostic_out->status : XR_BACKEND_INVARIANT_REJECTED;
        xr_backend_ir_free(ir);
        return status;
    }
    ir->verified = true;
    *ir_out = ir;
    return XR_BACKEND_OK;
}

void xr_backend_ir_free(XrBackendIR *ir) {
    if (!ir)
        return;
    if (atomic_fetch_sub_explicit(&ir->references, 1u, memory_order_acq_rel) != 1u)
        return;
    xr_target_profile_free(ir->profile);
    xr_validated_program_free(ir->program);
    xr_free(ir);
}

XrBackendIR *xr_backend_ir_retain(const XrBackendIR *ir) {
    if (!ir)
        return NULL;
    atomic_fetch_add_explicit((atomic_uint_least32_t *) &ir->references, 1u, memory_order_relaxed);
    return (XrBackendIR *) ir;
}

XrExecutionId xr_backend_ir_execution_id(const XrBackendIR *ir) {
    return ir ? ir->execution_id : (XrExecutionId) {0};
}

XrBackendId xr_backend_ir_backend_id(const XrBackendIR *ir) {
    return ir ? ir->backend_id : (XrBackendId) {0};
}

XrOptimizationPolicyId xr_backend_ir_optimization_policy_id(const XrBackendIR *ir) {
    return ir ? ir->optimization_policy_id : (XrOptimizationPolicyId) {0};
}

XrFingerprint xr_backend_ir_lowering_digest(const XrBackendIR *ir) {
    return ir ? ir->lowering_digest : (XrFingerprint) {0};
}

size_t xr_backend_ir_instruction_count(const XrBackendIR *ir) {
    return ir ? ir->instruction_count : 0u;
}

const char *xr_backend_status_name(XrBackendStatus status) {
    switch (status) {
        case XR_BACKEND_OK:
            return "ok";
        case XR_BACKEND_INVALID_INPUT:
            return "invalid-input";
        case XR_BACKEND_UNSUPPORTED_OPERATION:
            return "unsupported-operation";
        case XR_BACKEND_RESOURCE_LIMIT:
            return "resource-limit";
        case XR_BACKEND_OUT_OF_MEMORY:
            return "out-of-memory";
        case XR_BACKEND_INVARIANT_REJECTED:
            return "invariant-rejected";
        case XR_BACKEND_BINDING_REJECTED:
            return "binding-rejected";
        case XR_BACKEND_EMISSION_REJECTED:
            return "emission-rejected";
        case XR_BACKEND_TOOLCHAIN_REJECTED:
            return "toolchain-rejected";
        case XR_BACKEND_ARTIFACT_REJECTED:
            return "artifact-rejected";
    }
    return "unknown";
}
