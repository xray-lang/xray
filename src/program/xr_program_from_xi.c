/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_from_xi.c - Verified Xi to canonical XrProgram producer
 */

#include "xr_program_from_xi.h"
#include "xr_program_internal.h"
#include "xr_program_xi_projection_gen.h"
#include "../ir/xi_op_name.h"

#include "../base/xmalloc.h"
#include "../analysis/xglobal_summary.h"
#include "../core/xr_core_spec_gen.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/parser/xtype_ref.h"
#include "../ir/xi.h"
#include "../ir/xi_analysis.h"
#include "../ir/xi_cleanup.h"
#include "../ir/xi_coro_analyze.h"
#include "../ir/xi_coro_lower.h"
#include "../ir/xi_core_api.h"
#include "../ir/xi_module.h"
#include "../ir/xi_own.h"
#include "../plan/semantic/xr_semantic_ids.h"
#include "../runtime/abi/xr_builtin_provider_contract.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/class/xenum.h"
#include "../runtime/value/xenum_layout.h"
#include "../runtime/value/xtype.h"
#include "../shared/xr_target_query_registry_gen.h"
#include "../stdlib/xstdlib_metadata.h"
#include "xr_program_verify.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XrXiBlockArgumentStorage {
    const XiValue *source;
    const XiPhi *phi;
    XrCoreIrKey key;
    uint16_t type_id;
    XrCoreIrValueCategory category;
    XrCoreIrOwnershipDisposition ownership;
    uint8_t implicit_invoke_kind;
} XrXiBlockArgumentStorage;

typedef struct XrXiReconstructedPlaceStorage {
    const XiValue *place;
    const XiValue *owner;
    const XiValue *base_place;
    XrCoreIrKey key;
    uint16_t type_id;
    uint32_t field_ordinal;
} XrXiReconstructedPlaceStorage;

typedef struct XrXiEmissionSpan {
    uint32_t begin;
    uint32_t end;
} XrXiEmissionSpan;

typedef struct XrXiCleanupPointStorage {
    const XiCleanupBoundary *boundary;
    const XiBlock *enter_block;
    const XiBlock *leave_block;
    uint32_t enter_value_index;
    uint32_t leave_value_index;
    uint32_t enter_gap;
    uint32_t leave_gap;
    bool enter_emitted;
    bool leave_emitted;
} XrXiCleanupPointStorage;

typedef struct XrXiCleanupProjectionStorage {
    XrXiCleanupPointStorage *point;
    const XiBlock *source_block;
    uint32_t begin_gap;
    uint32_t end_gap;
    bool ends_in_trap;
    uint8_t flow_state;
    uint8_t argument_state;
    XrCoreIrKey *trap_argument_sources;
    XrCoreIrValueInput *trap_arguments;
    uint32_t trap_argument_count;
    XrCoreIrInstructionInput *trap_instructions;
    uint32_t trap_instruction_count;
} XrXiCleanupProjectionStorage;

typedef struct XrXiCleanupChainNode {
    const XiCleanupBoundary *boundary;
    uint32_t parent;
} XrXiCleanupChainNode;

typedef struct XrXiCleanupHandlerChainNode {
    const XiValue *registration;
    uint32_t parent;
} XrXiCleanupHandlerChainNode;

typedef struct XrXiCleanupRegistrationStorage {
    const XiValue *registration;
    const XiBlock *handler;
    const XiValue *outer_registration;
    uint32_t cleanup_state;
    uint32_t outer_handler_state;
    bool context_ready;
} XrXiCleanupRegistrationStorage;

enum {
    XR_XI_INVOKE_ARGUMENT_NONE = 0,
    XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT = 1,
    XR_XI_INVOKE_ARGUMENT_ERROR = 2,
    XR_XI_INVOKE_ARGUMENT_PANIC = 3,
};

enum {
    XR_XI_STATIC_BRANCH_UNKNOWN = 0,
    XR_XI_STATIC_BRANCH_TRUE = 1,
    XR_XI_STATIC_BRANCH_FALSE = 2,
};

typedef struct XrXiBlockStorage {
    const XiBlock *xi;
    bool reachable;
    bool trap_cleanup;
    bool panic_cleanup;
    bool cancel_cleanup;
    XrXiBlockArgumentStorage *argument_storage;
    uint32_t argument_count;
    uint32_t argument_capacity;
    XrXiReconstructedPlaceStorage *reconstructed_places;
    uint32_t reconstructed_place_count;
    uint32_t reconstructed_place_capacity;
    XrCoreIrValueInput *arguments;
    XrCoreIrInstructionInput *instructions;
    uint32_t instruction_count;
    XrXiEmissionSpan *emission_spans;
    uint32_t emission_span_count;
    uint32_t cleanup_entry_state;
    uint32_t cleanup_handler_entry_state;
    bool emission_ready;
    bool cleanup_entry_ready;
    uint8_t static_branch_outcome;
} XrXiBlockStorage;

typedef struct XrXiTrapEdge {
    const XiFunc *function;
    const XiValue *call;
    const XiValue *registration;
    const XiBlock *handler;
} XrXiTrapEdge;

typedef struct XrXiPanicEdge {
    const XiFunc *function;
    const XiValue *point;
    const XiValue *registration;
    const XiBlock *handler;
} XrXiPanicEdge;

typedef struct XrXiCancelEdge {
    const XiFunc *function;
    const XiCoroSuspendPoint *point;
    const XiValue *registration;
    const XiBlock *handler;
    bool private_projection;
} XrXiCancelEdge;

typedef struct XrXiCancelGraphStorage {
    XrCoreIrBlockInput *blocks;
    uint32_t block_count;
} XrXiCancelGraphStorage;

typedef struct XrXiFunctionStorage {
    const XiFunc *xi;
    XrCoreIrKey key;
    XiValue capture_receiver;
    uint16_t capture_type_id;
    uint16_t *parameter_types;
    XrParamMode *parameter_modes;
    XrCoreIrBlockInput *blocks;
    XrXiCancelGraphStorage *cancel_graphs;
    XrXiBlockStorage *block_storage;
    XrXiCleanupPointStorage *cleanup_points;
    uint32_t cleanup_point_count;
    XrXiCleanupProjectionStorage *cleanup_projections;
    uint32_t cleanup_projection_count;
    uint32_t cleanup_projection_capacity;
    XrXiCleanupChainNode *cleanup_chain_nodes;
    uint32_t cleanup_chain_node_count;
    XrXiCleanupHandlerChainNode *cleanup_handler_chain_nodes;
    XrXiCleanupRegistrationStorage *cleanup_registrations;
    uint32_t *cleanup_registration_by_value;
    uint32_t *cleanup_state_before_value;
    uint32_t *cleanup_handler_state_before_value;
    uint32_t cleanup_registration_count;
    XrCoreIrCoroutineStateInput *coroutine_states;
    XrCoreIrCoroutineSafepointInput *coroutine_safepoints;
    uint32_t local_effect_mask;
    uint32_t local_capability_mask;
    uint32_t closed_effect_mask;
    uint32_t closed_capability_mask;
    bool closed_contract_ready;
} XrXiFunctionStorage;

typedef struct XrXiModuleStorage {
    const XiFunc *root;
    const XrProgramSemanticModuleInput *source_authority;
    const XiFunc **xi_functions;
    bool *function_reachable;
    uint32_t function_count;
    XrCoreIrConstantInput *constants;
    uint32_t constant_count;
    uint32_t constant_capacity;
    XrCoreIrFunctionInput *functions;
    XrXiFunctionStorage *function_storage;
} XrXiModuleStorage;

typedef struct XrXiTypeStorage {
    XrCoreIrTypeInput input;
    uint16_t *field_types;
    XrCoreIrVariantInput *variants;
    uint16_t **variant_payload_types;
    XrCoreIrCallableSignatureInput *callable_signature;
    uint16_t *callable_parameter_types;
    XrParamMode *callable_parameter_modes;
} XrXiTypeStorage;

typedef struct XrXiInterfaceStorage {
    XgInterfaceId interface_id;
    XrCoreIrCallableSignatureInput *slots;
    uint16_t **parameter_types;
    XrParamMode **parameter_modes;
} XrXiInterfaceStorage;

typedef struct XrXiConformanceStorage {
    XgInterfaceConformanceId conformance_id;
    XrCoreIrKey *slot_functions;
} XrXiConformanceStorage;

typedef struct XrXiBuildContext {
    const XrProgramFromXiInput *source;
    XrCoreIrModuleInput *modules;
    XrXiModuleStorage *storage;
    XrXiTypeStorage *type_storage;
    XrCoreIrTypeInput *types;
    uint32_t type_count;
    uint32_t type_capacity;
    XrCoreIrInterfaceInput *interfaces;
    XrXiInterfaceStorage *interface_storage;
    uint32_t interface_count;
    uint32_t interface_capacity;
    XrCoreIrConformanceInput *conformances;
    XrXiConformanceStorage *conformance_storage;
    uint32_t conformance_count;
    uint32_t conformance_capacity;
    XrCoreIrProviderRequirementInput *provider_requirements;
    uint32_t *provider_operation_capacities;
    uint32_t provider_requirement_count;
    uint32_t provider_requirement_capacity;
    XrXiTrapEdge *trap_edges;
    uint32_t trap_edge_count;
    uint32_t trap_edge_capacity;
    XrXiPanicEdge *panic_edges;
    uint32_t panic_edge_count;
    uint32_t panic_edge_capacity;
    XrXiCancelEdge *cancel_edges;
    uint32_t cancel_edge_count;
    uint32_t cancel_edge_capacity;
} XrXiBuildContext;

typedef struct XrXiCallableTargetSet {
    const XgCallsiteSummary *callsite;
    const XgCallableTargetSummary *targets;
    uint32_t target_count;
} XrXiCallableTargetSet;

typedef struct XrXiCallableContract {
    uint64_t structural_signature_key;
    uint32_t effect_mask;
    uint32_t capability_mask;
    uint16_t error_type_id;
    uint16_t panic_type_id;
    uint32_t target_count;
} XrXiCallableContract;

static bool canonical_block_is_reachable(const XrXiBuildContext *context, const XiFunc *function,
                                         const XiBlock *block);
static bool canonical_block_is_trap_cleanup(const XrXiBuildContext *context, const XiFunc *function,
                                            const XiBlock *block);
static bool canonical_block_is_panic_cleanup(const XrXiBuildContext *context,
                                             const XiFunc *function, const XiBlock *block);
static bool canonical_block_is_cancel_cleanup(const XrXiBuildContext *context,
                                              const XiFunc *function, const XiBlock *block);
static bool cleanup_trap_capable_operation(uint16_t operation_id);
static const XrXiPanicEdge *find_panic_edge(const XrXiBuildContext *context, const XiFunc *function,
                                            const XiValue *point);
static const XrXiCancelEdge *find_cancel_edge(const XrXiBuildContext *context,
                                              const XiFunc *function,
                                              const XiCoroSuspendPoint *point);
static XrProgramBuildStatus append_cancel_edge(XrXiBuildContext *context, const XiFunc *function,
                                               const XiCoroSuspendPoint *point,
                                               const XiValue *registration,
                                               bool private_projection);
static bool coroutine_function_has_proven_suspend(const XrXiBuildContext *context,
                                                  const XiFunc *function, const XiFunc **stack,
                                                  uint32_t depth);
static bool exact_condition_assertion(const XiValue *value);
static bool map_logical_value_type(XrXiBuildContext *context, const XiFunc *function,
                                   const XiValue *value, uint16_t *type_id);

static XrProgramBuildStatus fail(char *diagnostic, size_t diagnostic_size,
                                 XrProgramBuildStatus status, const char *format, ...) {
    if (diagnostic && diagnostic_size != 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(diagnostic, diagnostic_size, format, arguments);
        va_end(arguments);
    }
    return status;
}

static bool builtin_provider_id(const char *key, XrStableId *out) {
    XrFingerprint digest;
    return xr_stable_id_from_key(key, out, &digest);
}

static bool stable_id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static XrProgramBuildStatus require_provider_operation(XrXiBuildContext *context,
                                                       const char *contract_key,
                                                       const char *operation_key,
                                                       XrStableId *contract_id_out,
                                                       XrStableId *operation_id_out) {
    XrStableId contract_id = {{0}};
    XrStableId operation_id = {{0}};
    if (!context || !contract_key || !contract_key[0] || !operation_key || !operation_key[0] ||
        !builtin_provider_id(contract_key, &contract_id) ||
        !builtin_provider_id(operation_key, &operation_id))
        return XR_PROGRAM_BUILD_INVALID_INPUT;

    uint32_t requirement_index = context->provider_requirement_count;
    for (uint32_t index = 0u; index < context->provider_requirement_count; ++index) {
        if (stable_id_equal(context->provider_requirements[index].contract_id, contract_id)) {
            requirement_index = index;
            break;
        }
    }
    if (requirement_index == context->provider_requirement_count) {
        if (context->provider_requirement_count == XR_PROGRAM_LIMIT_PROVIDER_CONTRACTS)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        if (context->provider_requirement_count == context->provider_requirement_capacity) {
            uint32_t capacity = context->provider_requirement_capacity
                                    ? context->provider_requirement_capacity * 2u
                                    : 4u;
            if (capacity < context->provider_requirement_capacity ||
                capacity > XR_PROGRAM_LIMIT_PROVIDER_CONTRACTS)
                capacity = XR_PROGRAM_LIMIT_PROVIDER_CONTRACTS;
            XrCoreIrProviderRequirementInput *requirements =
                xr_calloc(capacity, sizeof(*requirements));
            uint32_t *operation_capacities = xr_calloc(capacity, sizeof(*operation_capacities));
            if (!requirements || !operation_capacities) {
                xr_free(requirements);
                xr_free(operation_capacities);
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            }
            if (context->provider_requirement_count != 0u) {
                memcpy(requirements, context->provider_requirements,
                       context->provider_requirement_count * sizeof(*requirements));
                memcpy(operation_capacities, context->provider_operation_capacities,
                       context->provider_requirement_count * sizeof(*operation_capacities));
            }
            xr_free(context->provider_requirements);
            xr_free(context->provider_operation_capacities);
            context->provider_requirements = requirements;
            context->provider_operation_capacities = operation_capacities;
            context->provider_requirement_capacity = capacity;
        }
        context->provider_requirements[requirement_index].contract_id = contract_id;
        ++context->provider_requirement_count;
    }

    XrCoreIrProviderRequirementInput *requirement =
        &context->provider_requirements[requirement_index];
    for (uint32_t index = 0u; index < requirement->operation_count; ++index) {
        if (stable_id_equal(requirement->operation_ids[index], operation_id)) {
            if (contract_id_out)
                *contract_id_out = contract_id;
            if (operation_id_out)
                *operation_id_out = operation_id;
            return XR_PROGRAM_BUILD_OK;
        }
    }
    if (requirement->operation_count == XR_PROGRAM_LIMIT_PROVIDER_OPERATIONS_PER_CONTRACT)
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    uint32_t operation_capacity = context->provider_operation_capacities[requirement_index];
    if (requirement->operation_count == operation_capacity) {
        uint32_t capacity = operation_capacity ? operation_capacity * 2u : 4u;
        if (capacity < operation_capacity ||
            capacity > XR_PROGRAM_LIMIT_PROVIDER_OPERATIONS_PER_CONTRACT)
            capacity = XR_PROGRAM_LIMIT_PROVIDER_OPERATIONS_PER_CONTRACT;
        XrStableId *operations = xr_realloc((void *) requirement->operation_ids,
                                            (size_t) capacity * sizeof(*operations));
        if (!operations)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        requirement->operation_ids = operations;
        context->provider_operation_capacities[requirement_index] = capacity;
    }
    ((XrStableId *) requirement->operation_ids)[requirement->operation_count++] = operation_id;
    if (contract_id_out)
        *contract_id_out = contract_id;
    if (operation_id_out)
        *operation_id_out = operation_id;
    return XR_PROGRAM_BUILD_OK;
}

static void put_u32_be(uint8_t output[4], uint32_t value) {
    output[0] = (uint8_t) (value >> 24u);
    output[1] = (uint8_t) (value >> 16u);
    output[2] = (uint8_t) (value >> 8u);
    output[3] = (uint8_t) value;
}

static void put_u64_be(uint8_t output[8], uint64_t value) {
    for (uint32_t index = 0; index < 8u; ++index)
        output[index] = (uint8_t) (value >> (56u - index * 8u));
}

static XrCoreIrKey key_from_stable_id(uint8_t domain, XrStableId id) {
    uint8_t material[1u + XR_STABLE_ID_BYTES];
    material[0] = domain;
    memcpy(material + 1u, id.bytes, sizeof(id.bytes));
    return xr_core_ir_key(material, sizeof(material));
}

static XrCoreIrKey key_from_stable_id_and_u32(uint8_t domain, XrStableId id, uint32_t value) {
    uint8_t material[1u + XR_STABLE_ID_BYTES + 4u];
    material[0] = domain;
    memcpy(material + 1u, id.bytes, sizeof(id.bytes));
    put_u32_be(material + 1u + sizeof(id.bytes), value);
    return xr_core_ir_key(material, sizeof(material));
}

static XrCoreIrKey constant_key(XrStableId module_id, uint16_t type_id, int64_t value) {
    uint8_t material[1u + XR_STABLE_ID_BYTES + 2u + 8u];
    material[0] = UINT8_C(0x43);
    memcpy(material + 1u, module_id.bytes, sizeof(module_id.bytes));
    material[1u + sizeof(module_id.bytes)] = (uint8_t) (type_id >> 8u);
    material[2u + sizeof(module_id.bytes)] = (uint8_t) type_id;
    put_u64_be(material + 3u + sizeof(module_id.bytes), (uint64_t) value);
    return xr_core_ir_key(material, sizeof(material));
}

static XrCoreIrKey key_from_key_and_u32(uint8_t domain, XrCoreIrKey key, uint32_t value) {
    uint8_t material[1u + XR_CORE_IR_KEY_SIZE + 4u];
    material[0] = domain;
    memcpy(material + 1u, key.bytes, sizeof(key.bytes));
    put_u32_be(material + 1u + sizeof(key.bytes), value);
    return xr_core_ir_key(material, sizeof(material));
}

static XrCoreIrKey function_key(XrStableId module_id, uint32_t function_index) {
    return key_from_stable_id_and_u32(UINT8_C(0x46), module_id, function_index);
}

static XrCoreIrKey block_key(const XrXiFunctionStorage *function, const XiBlock *block) {
    return key_from_key_and_u32(UINT8_C(0x42), function->key, block->id);
}

static XrCoreIrKey cancel_block_key(const XrXiFunctionStorage *function, uint32_t safepoint_id) {
    return key_from_key_and_u32(UINT8_C(0x4b), function->key, safepoint_id);
}

static XrCoreIrKey cancel_argument_key(const XrXiFunctionStorage *function, uint32_t safepoint_id,
                                       uint32_t argument) {
    return key_from_key_and_u32(UINT8_C(0x59), cancel_block_key(function, safepoint_id), argument);
}

static XrCoreIrKey cancel_graph_block_key(const XrXiFunctionStorage *function,
                                          uint32_t safepoint_id, const XiBlock *source,
                                          const XiBlock *entry) {
    XrCoreIrKey root = cancel_block_key(function, safepoint_id);
    return source == entry ? root : key_from_key_and_u32(UINT8_C(0x4c), root, source->id);
}

static XrCoreIrKey cancel_graph_argument_key(XrCoreIrKey block, uint32_t argument) {
    return key_from_key_and_u32(UINT8_C(0x59), block, argument);
}

static XrCoreIrKey cancel_graph_result_key(XrCoreIrKey block, uint32_t instruction) {
    return key_from_key_and_u32(UINT8_C(0x5b), block, instruction);
}

static XrCoreIrKey cleanup_trap_block_key(const XrXiFunctionStorage *function,
                                          const XrXiCleanupProjectionStorage *projection) {
    const XrXiCleanupPointStorage *point = projection ? projection->point : NULL;
    uint32_t identity = point && point->boundary && point->boundary->enter
                            ? point->boundary->enter->id
                            : UINT32_MAX;
    XrCoreIrKey boundary_key = key_from_key_and_u32(UINT8_C(0x5c), function->key, identity);
    XrCoreIrKey block_key = key_from_key_and_u32(
        UINT8_C(0x5f), boundary_key,
        projection && projection->source_block ? projection->source_block->id : UINT32_MAX);
    return key_from_key_and_u32(UINT8_C(0x60), block_key,
                                projection ? projection->begin_gap : UINT32_MAX);
}

static XrCoreIrKey cleanup_trap_argument_key(const XrXiFunctionStorage *function,
                                             const XrXiCleanupProjectionStorage *projection,
                                             uint32_t argument) {
    return key_from_key_and_u32(UINT8_C(0x5d), cleanup_trap_block_key(function, projection),
                                argument);
}

static XrCoreIrKey cleanup_trap_result_key(const XrXiFunctionStorage *function,
                                           const XrXiCleanupProjectionStorage *projection,
                                           uint32_t instruction) {
    return key_from_key_and_u32(UINT8_C(0x5e), cleanup_trap_block_key(function, projection),
                                instruction);
}

static XrCoreIrKey value_key(const XrXiFunctionStorage *function, const XiValue *value) {
    if (function && value == &function->capture_receiver)
        return key_from_key_and_u32(UINT8_C(0x55), function->key, 0u);
    return key_from_key_and_u32(UINT8_C(0x56), function->key, value->id);
}

static XrCoreIrKey closure_capture_key(const XrXiFunctionStorage *function,
                                       const XiValue *closure) {
    return key_from_key_and_u32(UINT8_C(0x45), function->key, closure->id);
}

static XrCoreIrKey existential_owner_copy_key(const XrXiFunctionStorage *function,
                                              const XiValue *pack) {
    return key_from_key_and_u32(UINT8_C(0x59), value_key(function, pack), 0u);
}

static XrCoreIrKey aggregate_field_place_key(const XrXiFunctionStorage *function,
                                             const XiValue *access) {
    return key_from_key_and_u32(UINT8_C(0x5a), value_key(function, access), 0u);
}

static bool map_builtin_type(const XrType *type, uint16_t *type_id) {
    if (!type || !type_id || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_UNIT:
            *type_id = XR_CORE_TYPE_VOID;
            return true;
        case XR_KIND_BOOL:
            *type_id = XR_CORE_TYPE_BOOL;
            return true;
        case XR_KIND_INT:
            if (type->scalar_rep == XR_NATIVE_I64) {
                *type_id = XR_CORE_TYPE_I64;
                return true;
            }
            if (type->scalar_rep == XR_NATIVE_U32) {
                *type_id = XR_CORE_TYPE_U32;
                return true;
            }
            if (type->scalar_rep == XR_NATIVE_U16) {
                *type_id = XR_CORE_TYPE_U16;
                return true;
            }
            return false;
        case XR_KIND_ENUM:
            if (!type->enum_type.enum_name)
                return false;
            const XrTargetQueryEnumDesc *target_enum =
                xr_target_query_enum_by_type_name(type->enum_type.enum_name);
            const XrClassInfo *nominal = type->enum_type.nominal_ref;
            if (!target_enum || !nominal || nominal->nominal_kind != XA_NOMINAL_ENUM ||
                !nominal->declaration_symbol || !nominal->declaration_symbol->is_builtin ||
                !nominal->declaration_symbol->name ||
                strcmp(nominal->declaration_symbol->name, target_enum->type_name) != 0)
                return false;
            *type_id = target_enum->core_type_id;
            return true;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
            if (type->instance.class_name && strcmp(type->instance.class_name, "PanicInfo") == 0) {
                *type_id = XR_CORE_TYPE_PANIC_INFO;
                return true;
            }
            return false;
        default:
            return false;
    }
}

static const XrXiTypeStorage *find_dynamic_type_by_id(const XrXiBuildContext *context,
                                                      uint16_t type_id) {
    for (uint32_t index = 0; context && index < context->type_count; ++index) {
        if (context->type_storage[index].input.local_id == type_id)
            return &context->type_storage[index];
    }
    return NULL;
}

static bool type_stack_contains(const XrType *const *stack, uint32_t depth, const XrType *type) {
    for (uint32_t index = 0; index < depth; ++index) {
        if (stack[index] == type)
            return true;
    }
    return false;
}

static bool map_type_recursive(XrXiBuildContext *context, const XrType *type, uint16_t *type_id,
                               const XrType *const *stack, uint32_t depth);
static bool map_type_for_mode(XrXiBuildContext *context, const XrType *type, XrParamMode mode,
                              uint16_t *type_id, const XrType *const *stack, uint32_t depth);
static XrCoreIrOwnershipDisposition logical_ownership_for_type(const XrXiBuildContext *context,
                                                               uint16_t type_id);
static XrCoreIrCopyContract logical_copy_contract_for_type(const XrXiBuildContext *context,
                                                           uint16_t type_id);
static bool nominal_type_contract_is_exact(const XrXiBuildContext *context,
                                           XgDeclId implementor_decl_id, uint64_t nominal_key,
                                           uint8_t implementor_kind,
                                           XrCoreIrTypeOwnership ownership,
                                           XrCoreIrCopyContract copy_contract);

static const XrClassInfo *nominal_info_for_type(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_ENUM)
        return type->enum_type.nominal_ref;
    if (type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE)
        return type->instance.class_ref;
    return NULL;
}

static bool nominal_contract(const XrXiBuildContext *context, const XrType *type,
                             uint8_t *decl_kind_out, XrCoreIrNominalKind *nominal_kind_out,
                             uint64_t *nominal_key_out) {
    const XrClassInfo *info = nominal_info_for_type(type);
    if (!context || !context->source || !context->source->global_evidence || !info ||
        info->xg_decl_id == XG_NO_ID || info->xg_nominal_key == 0)
        return false;
    uint8_t decl_kind = 0u;
    XrCoreIrNominalKind nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    switch ((XaNominalKind) info->nominal_kind) {
        case XA_NOMINAL_CLASS:
            decl_kind = XG_DECL_CLASS;
            nominal_kind = XR_CORE_IR_NOMINAL_CLASS;
            break;
        case XA_NOMINAL_STRUCT:
            decl_kind = XG_DECL_STRUCT;
            nominal_kind = XR_CORE_IR_NOMINAL_STRUCT;
            break;
        case XA_NOMINAL_ENUM:
            decl_kind = XG_DECL_ENUM;
            nominal_kind = XR_CORE_IR_NOMINAL_ENUM;
            break;
        default:
            return false;
    }
    const XgDeclSummary *found = NULL;
    const XgGlobalEvidence *evidence = context->source->global_evidence;
    for (uint32_t index = 0u; index < evidence->ndecls; ++index) {
        const XgDeclSummary *decl = &evidence->decls[index];
        if (decl->decl_id != info->xg_decl_id)
            continue;
        if (found || decl->kind != decl_kind || decl->type_key == 0u || decl->nominal_key == 0u ||
            decl->nominal_key != info->xg_nominal_key)
            return false;
        found = decl;
    }
    if (!found)
        return false;
    if (decl_kind_out)
        *decl_kind_out = decl_kind;
    if (nominal_kind_out)
        *nominal_kind_out = nominal_kind;
    if (nominal_key_out)
        *nominal_key_out = found->nominal_key;
    return true;
}

static const XgClassSummary *find_xg_class_by_id(const XgGlobalEvidence *evidence,
                                                 XgClassId class_id);
static bool nominal_instance_identity_equal(const XrXiBuildContext *context, const XrType *left,
                                            const XrType *right);

static const XgDeclSummary *find_xg_decl_by_id(const XgGlobalEvidence *evidence, XgDeclId decl_id) {
    const XgDeclSummary *found = NULL;
    for (uint32_t index = 0; evidence && index < evidence->ndecls; ++index) {
        const XgDeclSummary *candidate = &evidence->decls[index];
        if (candidate->decl_id != decl_id)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static const XiClassData *find_aggregate_schema(const XrXiBuildContext *context, const XrType *type,
                                                const XiModule **owner_module) {
    const XiClassData *found = NULL;
    const XiModule *found_module = NULL;
    const XrClassInfo *nominal_info = nominal_info_for_type(type);
    const XgGlobalEvidence *evidence =
        context && context->source ? context->source->global_evidence : NULL;
    if (!context || !type || (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS) ||
        !nominal_info || nominal_info->xg_decl_id == XG_NO_ID ||
        nominal_info->xg_nominal_key == 0 || !evidence)
        return NULL;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        const XiFunc *root = context->source->module_roots[module_index];
        const XiModule *module = root ? root->module : NULL;
        for (uint16_t index = 0; module && index < module->nclasses; ++index) {
            const XiClassData *candidate = module->classes ? module->classes[index] : NULL;
            const XgClassSummary *class_row =
                candidate ? find_xg_class_by_id(evidence, candidate->xg_class_id) : NULL;
            const XgDeclSummary *decl_row =
                class_row ? find_xg_decl_by_id(evidence, class_row->decl_id) : NULL;
            if (!candidate || !class_row || !decl_row ||
                class_row->decl_id != nominal_info->xg_decl_id ||
                decl_row->nominal_key != nominal_info->xg_nominal_key ||
                class_row->module_id != decl_row->module_id ||
                class_row->decl_kind != decl_row->kind ||
                (candidate->class_info &&
                 (candidate->class_info->xg_class_id != candidate->xg_class_id ||
                  candidate->class_info->xg_decl_id != nominal_info->xg_decl_id ||
                  candidate->class_info->xg_nominal_key != nominal_info->xg_nominal_key)) ||
                candidate->is_generic_skeleton ||
                (candidate->struct_layout &&
                 candidate->struct_layout->kind == XR_AGG_LAYOUT_UNION) ||
                (candidate->instance_field_count != 0u &&
                 (!candidate->instance_field_names || !candidate->instance_field_types)) ||
                (candidate->struct_layout &&
                 candidate->instance_field_count != candidate->struct_layout->field_count))
                continue;
            if (found)
                return NULL;
            found = candidate;
            found_module = module;
        }
    }
    if (owner_module)
        *owner_module = found_module;
    return found;
}

static bool variant_schema_matches_type(const XiEnumData *schema, const XrType *type) {
    if (!schema || !type || type->kind != XR_KIND_ENUM || !type->enum_type.layout)
        return false;
    uint32_t layout_id =
        type->enum_type.layout ? type->enum_type.layout->layout_id : type->enum_type.layout_id;
    if (layout_id == 0u || schema->layout_id != layout_id || !schema->members ||
        schema->member_count == 0u || schema->member_count != type->enum_type.layout->variant_count)
        return false;
    for (uint32_t variant = 0; variant < schema->member_count; variant++) {
        const XiEnumMemberData *member = &schema->members[variant];
        const XrEnumVariantLayout *layout_variant =
            xr_enum_layout_variant(type->enum_type.layout, variant);
        if (!member->name || !member->name[0] || member->ordinal != variant ||
            member->payload_count < 0 || member->payload_count > UINT16_MAX || !layout_variant ||
            layout_variant->tag != variant ||
            layout_variant->payload_count != (uint16_t) member->payload_count ||
            !xr_enum_payload_names_are_exact(member->payload_names,
                                             (uint16_t) member->payload_count) ||
            (member->payload_count > 0 && !member->payload_types))
            return false;
        for (uint32_t prior = 0; prior < variant; prior++) {
            if (strcmp(member->name, schema->members[prior].name) == 0)
                return false;
        }
        for (int field = 0; field < member->payload_count; field++) {
            if (!member->payload_types[field])
                return false;
        }
    }
    return true;
}

static const XiEnumData *find_variant_schema(const XrXiBuildContext *context, const XrType *type) {
    const XiEnumData *found = NULL;
    for (uint32_t module_index = 0; context && module_index < context->source->module_count;
         ++module_index) {
        const XiFunc *root = context->source->module_roots[module_index];
        const XiModule *module = root ? root->module : NULL;
        for (uint16_t slot = 0; module && module->slot_enums && slot < module->nslots; ++slot) {
            const XiEnumData *candidate = module->slot_enums[slot];
            if (!variant_schema_matches_type(candidate, type))
                continue;
            if (found && found != candidate)
                return NULL;
            found = candidate;
        }
        for (uint16_t function = 0; module && function < module->nfuncs; ++function) {
            const XiFunc *owner = module->functions[function];
            for (uint32_t block = 0; owner && block < owner->nblocks; ++block) {
                const XiBlock *row = owner->blocks[block];
                if (!canonical_block_is_reachable(context, owner, row))
                    continue;
                for (uint32_t value = 0; row && value < row->nvalues; ++value) {
                    const XiValue *candidate_value = row->values[value];
                    const XiEnumData *candidate =
                        candidate_value && candidate_value->op == XI_CONST &&
                                candidate_value->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE
                            ? (const XiEnumData *) candidate_value->aux
                            : NULL;
                    if (!variant_schema_matches_type(candidate, type))
                        continue;
                    if (found && found != candidate)
                        return NULL;
                    found = candidate;
                }
            }
        }
    }
    return found;
}

static const XrType *substitute_variant_payload_type(const XiEnumData *schema,
                                                     const XrType *enum_type,
                                                     const XrType *payload_type) {
    if (!schema || !enum_type || !payload_type || !schema->type_param_names ||
        schema->type_param_count != enum_type->enum_type.type_arg_count ||
        !enum_type->enum_type.type_args)
        return payload_type;
    XrType *substituted =
        xr_type_substitute(NULL, (XrType *) payload_type, schema->type_param_names,
                           enum_type->enum_type.type_args, schema->type_param_count);
    return substituted ? substituted : payload_type;
}

static void put_dynamic_type_reference(const XrXiBuildContext *context,
                                       uint8_t row[1u + XR_CORE_IR_KEY_SIZE], uint16_t type_id) {
    const XrXiTypeStorage *dynamic = find_dynamic_type_by_id(context, type_id);
    memset(row, 0, 1u + XR_CORE_IR_KEY_SIZE);
    if (dynamic) {
        row[0] = UINT8_C(1);
        memcpy(row + 1u, dynamic->input.key.bytes, XR_CORE_IR_KEY_SIZE);
    } else {
        row[1] = (uint8_t) (type_id >> 8u);
        row[2] = (uint8_t) type_id;
    }
}

static XrXiTypeStorage *append_type_storage(XrXiBuildContext *context) {
    if (!context || context->type_count >= UINT16_MAX - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + 1u)
        return NULL;
    if (context->type_count == context->type_capacity) {
        uint32_t capacity = context->type_capacity ? context->type_capacity * 2u : 8u;
        if (capacity < context->type_count ||
            (size_t) capacity > SIZE_MAX / sizeof(*context->type_storage))
            return NULL;
        XrXiTypeStorage *grown =
            xr_realloc(context->type_storage, (size_t) capacity * sizeof(*context->type_storage));
        if (!grown)
            return NULL;
        context->type_storage = grown;
        context->type_capacity = capacity;
    }
    XrXiTypeStorage *storage = &context->type_storage[context->type_count];
    memset(storage, 0, sizeof(*storage));
    storage->input.local_id = (uint16_t) (XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + context->type_count);
    return storage;
}

/* Nullable source types have no target-visible runtime representation.  They
 * normalize to the canonical sum None | Some(T), whose identity depends only
 * on the canonical Core type reference for T. */
static bool map_optional_type_recursive(XrXiBuildContext *context, const XrType *type,
                                        uint16_t *type_id, const XrType *const *stack,
                                        uint32_t depth) {
    if (!context || !type || !type_id || !type->is_nullable || depth >= 64u ||
        type_stack_contains(stack, depth, type))
        return false;

    const XrType *nested_stack[64];
    if (depth != 0u)
        memcpy(nested_stack, stack, depth * sizeof(*nested_stack));
    nested_stack[depth] = type;
    XrType payload_type = *type;
    payload_type.is_nullable = false;
    uint16_t payload_type_id = XR_CORE_TYPE_VOID;
    if (!map_type_recursive(context, &payload_type, &payload_type_id, nested_stack, depth + 1u) ||
        payload_type_id == XR_CORE_TYPE_VOID)
        return false;
    bool affine = logical_ownership_for_type(context, payload_type_id) == XR_CORE_IR_OWNER;
    XrCoreIrCopyContract payload_copy = logical_copy_contract_for_type(context, payload_type_id);
    XrCoreIrTypeOwnership optional_ownership =
        affine ? XR_CORE_IR_TYPE_OWNERSHIP_AFFINE : XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
    XrCoreIrCopyContract optional_copy = affine ? payload_copy : XR_CORE_IR_COPY_TRIVIAL;

    uint8_t material[2u + XR_CORE_IR_KEY_SIZE];
    material[0] = UINT8_C(0x4e);
    put_dynamic_type_reference(context, material + 1u, payload_type_id);
    XrCoreIrKey semantic_key = xr_core_ir_key(material, sizeof(material));
    for (uint32_t index = 0u; index < context->type_count; ++index) {
        if (!xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key))
            continue;
        const XrCoreIrTypeInput *candidate = &context->type_storage[index].input;
        if (candidate->kind != XR_CORE_IR_TYPE_VARIANT ||
            candidate->nominal_kind != XR_CORE_IR_NOMINAL_NONE || candidate->variant_count != 2u ||
            !candidate->variants || candidate->variants[0].payload_count != 0u ||
            candidate->variants[0].payload_types || candidate->variants[1].payload_count != 1u ||
            !candidate->variants[1].payload_types ||
            candidate->variants[1].payload_types[0] != payload_type_id ||
            candidate->ownership != optional_ownership || candidate->copy_contract != optional_copy)
            return false;
        *type_id = candidate->local_id;
        return true;
    }

    XrCoreIrVariantInput *variants = xr_calloc(2u, sizeof(*variants));
    uint16_t **payload_storage = xr_calloc(2u, sizeof(*payload_storage));
    if (!variants || !payload_storage) {
        xr_free(payload_storage);
        xr_free(variants);
        return false;
    }
    payload_storage[1] = xr_calloc(1u, sizeof(*payload_storage[1]));
    if (!payload_storage[1]) {
        xr_free(payload_storage);
        xr_free(variants);
        return false;
    }
    payload_storage[1][0] = payload_type_id;
    variants[1].payload_types = payload_storage[1];
    variants[1].payload_count = 1u;

    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage) {
        xr_free(payload_storage[1]);
        xr_free(payload_storage);
        xr_free(variants);
        return false;
    }
    storage->variants = variants;
    storage->variant_payload_types = payload_storage;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_VARIANT;
    storage->input.nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    storage->input.ownership = optional_ownership;
    storage->input.copy_contract = optional_copy;
    storage->input.variants = variants;
    storage->input.variant_count = 2u;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;
}

/* A closure environment is a compiler-created logical product, not a VM/C
 * layout.  Its canonical identity is the ordered capture TypeId sequence;
 * every executor remains free to choose its own physical carrier.  This first
 * source slice admits immutable by-copy captures of trivial values only. */
static bool map_capture_type(XrXiBuildContext *context, const XiFunc *function, uint16_t *type_id) {
    if (!context || !function || !type_id || function->ncaptures == 0u ||
        function->ncaptures > XI_MAX_CAPTURES)
        return false;
    uint32_t field_count = function->ncaptures;
    uint16_t *field_types = xr_calloc(field_count, sizeof(*field_types));
    if (!field_types)
        return false;
    for (uint32_t field = 0; field < field_count; ++field) {
        const XiCapture *capture = &function->captures[field];
        if (!capture->type || capture->needs_cell || capture->capture_kind != XI_CAPTURE_BY_COPY ||
            !map_type_recursive(context, capture->type, &field_types[field], NULL, 0u) ||
            logical_ownership_for_type(context, field_types[field]) != XR_CORE_IR_NON_OWNER ||
            logical_copy_contract_for_type(context, field_types[field]) !=
                XR_CORE_IR_COPY_TRIVIAL) {
            xr_free(field_types);
            return false;
        }
    }

    const size_t reference_size = 1u + XR_CORE_IR_KEY_SIZE;
    if ((size_t) field_count > (SIZE_MAX - 5u) / reference_size) {
        xr_free(field_types);
        return false;
    }
    size_t material_size = 5u + (size_t) field_count * reference_size;
    uint8_t *material = xr_calloc(material_size, 1u);
    if (!material) {
        xr_free(field_types);
        return false;
    }
    size_t cursor = 0u;
    material[cursor++] = UINT8_C(0x45);
    put_u32_be(material + cursor, field_count);
    cursor += 4u;
    for (uint32_t field = 0; field < field_count; ++field) {
        put_dynamic_type_reference(context, material + cursor, field_types[field]);
        cursor += reference_size;
    }
    XrCoreIrKey semantic_key = xr_core_ir_key(material, material_size);
    xr_free(material);
    for (uint32_t index = 0; index < context->type_count; ++index) {
        if (!xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key))
            continue;
        *type_id = context->type_storage[index].input.local_id;
        xr_free(field_types);
        return true;
    }

    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage) {
        xr_free(field_types);
        return false;
    }
    storage->field_types = field_types;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_AGGREGATE;
    storage->input.ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    storage->input.copy_contract = XR_CORE_IR_COPY_EXPLICIT;
    storage->input.field_types = field_types;
    storage->input.field_count = field_count;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;
}

static bool map_callable_type_contract_recursive(XrXiBuildContext *context, const XrType *type,
                                                 uint32_t effect_mask, uint32_t capability_mask,
                                                 uint16_t error_type_id, uint16_t panic_type_id,
                                                 uint16_t *type_id, const XrType *const *stack,
                                                 uint32_t depth) {
    /* A visible MAY_THROW type is a safe supertype for an exact closed target
     * set that
     * Xglobal proves cannot error.  The reverse is not safe: a visible
     * NO_THROW contract may
     * never admit an error-producing target set. */
    if (!context || !type || !type_id || type->kind != XR_KIND_FUNCTION || type->is_nullable ||
        type->function.param_count < 0 || type->function.param_count > UINT16_MAX ||
        (type->function.param_count != 0 && !type->function.params) ||
        !type->function.return_type || type->function.is_variadic || type->function.is_c_abi ||
        type->function.type_param_count != 0 || type->function.view_origin_count != 0 ||
        depth >= 64u || type_stack_contains(stack, depth, type) ||
        (type->function.throw_effect != XR_FN_EFFECT_NO_THROW &&
         type->function.throw_effect != XR_FN_EFFECT_MAY_THROW) ||
        (error_type_id != XR_CORE_TYPE_VOID &&
         type->function.throw_effect != XR_FN_EFFECT_MAY_THROW))
        return false;

    uint32_t parameter_count = (uint32_t) type->function.param_count;
    uint16_t *parameter_types =
        parameter_count ? xr_calloc(parameter_count, sizeof(*parameter_types)) : NULL;
    XrParamMode *parameter_modes =
        parameter_count ? xr_calloc(parameter_count, sizeof(*parameter_modes)) : NULL;
    if (parameter_count && (!parameter_types || !parameter_modes)) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }

    const XrType *nested_stack[64];
    if (depth != 0u)
        memcpy(nested_stack, stack, depth * sizeof(*nested_stack));
    nested_stack[depth] = type;
    bool valid = true;
    for (uint32_t parameter = 0; parameter < parameter_count; ++parameter) {
        XrFunctionParam *source_parameter = &type->function.params[parameter];
        if (!source_parameter->type || !xr_param_mode_is_valid(source_parameter->mode) ||
            !map_type_for_mode(context, source_parameter->type, source_parameter->mode,
                               &parameter_types[parameter], nested_stack, depth + 1u) ||
            parameter_types[parameter] == XR_CORE_TYPE_VOID) {
            valid = false;
            break;
        }
        parameter_modes[parameter] = source_parameter->mode;
    }
    uint16_t result_type = XR_CORE_TYPE_VOID;
    if (!valid || !map_type_recursive(context, type->function.return_type, &result_type,
                                      nested_stack, depth + 1u)) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }

    const size_t type_reference_size = 1u + XR_CORE_IR_KEY_SIZE;
    if (parameter_count >
        (SIZE_MAX - 14u - 3u * type_reference_size) / (type_reference_size + 1u)) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }
    size_t material_size =
        5u + (size_t) parameter_count * (type_reference_size + 1u) + 3u * type_reference_size + 9u;
    uint8_t *material = xr_calloc(material_size, 1u);
    if (!material) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }
    size_t cursor = 0u;
    material[cursor++] = UINT8_C(0x4c);
    put_u32_be(material + cursor, parameter_count);
    cursor += 4u;
    for (uint32_t parameter = 0; parameter < parameter_count; ++parameter) {
        put_dynamic_type_reference(context, material + cursor, parameter_types[parameter]);
        cursor += type_reference_size;
        material[cursor++] = (uint8_t) parameter_modes[parameter];
    }
    put_dynamic_type_reference(context, material + cursor, result_type);
    cursor += type_reference_size;
    material[cursor++] = (uint8_t) logical_ownership_for_type(context, result_type);
    put_dynamic_type_reference(context, material + cursor, error_type_id);
    cursor += type_reference_size;
    put_dynamic_type_reference(context, material + cursor, panic_type_id);
    cursor += type_reference_size;
    put_u32_be(material + cursor, effect_mask);
    cursor += 4u;
    put_u32_be(material + cursor, capability_mask);
    cursor += 4u;
    if (cursor != material_size) {
        xr_free(material);
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }
    XrCoreIrKey semantic_key = xr_core_ir_key(material, material_size);
    xr_free(material);
    for (uint32_t index = 0; index < context->type_count; ++index) {
        if (xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key)) {
            *type_id = context->type_storage[index].input.local_id;
            xr_free(parameter_types);
            xr_free(parameter_modes);
            return true;
        }
    }

    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }
    storage->callable_signature = xr_calloc(1u, sizeof(*storage->callable_signature));
    if (!storage->callable_signature) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }
    storage->callable_parameter_types = parameter_types;
    storage->callable_parameter_modes = parameter_modes;
    storage->callable_signature->parameter_types = parameter_types;
    storage->callable_signature->parameter_modes = parameter_modes;
    storage->callable_signature->parameter_count = parameter_count;
    storage->callable_signature->receiver_mode = XR_PARAM_READ;
    storage->callable_signature->result_type_id = result_type;
    storage->callable_signature->result_ownership =
        logical_ownership_for_type(context, result_type);
    storage->callable_signature->error_type_id = error_type_id;
    storage->callable_signature->panic_type_id = panic_type_id;
    storage->callable_signature->effect_mask = effect_mask;
    storage->callable_signature->capability_mask = capability_mask;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_CALLABLE;
    storage->input.ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    storage->input.copy_contract = XR_CORE_IR_COPY_EXPLICIT;
    storage->input.callable_signature = storage->callable_signature;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;
}

static bool map_callable_type_recursive(XrXiBuildContext *context, const XrType *type,
                                        uint16_t *type_id, const XrType *const *stack,
                                        uint32_t depth) {
    return map_callable_type_contract_recursive(context, type, 0u, 0u, XR_CORE_TYPE_VOID,
                                                XR_CORE_TYPE_VOID, type_id, stack, depth);
}

static XrCoreIrKey key_from_u32(uint8_t domain, uint32_t value) {
    uint8_t material[5u];
    material[0] = domain;
    put_u32_be(material + 1u, value);
    return xr_core_ir_key(material, sizeof(material));
}

static XrCoreIrKey interface_key(XgInterfaceId interface_id) {
    return key_from_u32(UINT8_C(0x49), interface_id);
}

static XrCoreIrKey conformance_key(XgInterfaceConformanceId conformance_id) {
    return key_from_u32(UINT8_C(0x4f), conformance_id);
}

static bool map_variant_type_recursive(XrXiBuildContext *context, const XrType *type,
                                       uint16_t *type_id, const XrType *const *stack,
                                       uint32_t depth) {
    const XiEnumData *schema = find_variant_schema(context, type);
    uint8_t decl_kind = 0u;
    XrCoreIrNominalKind nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    uint64_t nominal_key = 0u;
    if (!schema || !type->enum_type.layout || depth >= 64u ||
        type_stack_contains(stack, depth, type) ||
        !nominal_contract(context, type, &decl_kind, &nominal_kind, &nominal_key) ||
        decl_kind != XG_DECL_ENUM || nominal_kind != XR_CORE_IR_NOMINAL_ENUM)
        return false;

    XrCoreIrVariantInput *variants = xr_calloc(schema->member_count, sizeof(*variants));
    uint16_t **payload_storage = xr_calloc(schema->member_count, sizeof(*payload_storage));
    if (!variants || !payload_storage) {
        xr_free(variants);
        xr_free(payload_storage);
        return false;
    }
    const XrType *nested_stack[64];
    if (depth != 0u)
        memcpy(nested_stack, stack, depth * sizeof(*nested_stack));
    nested_stack[depth] = type;
    uint64_t payload_total = 0u;
    bool valid = true;
    for (uint32_t variant = 0; valid && variant < schema->member_count; ++variant) {
        const XiEnumMemberData *member = &schema->members[variant];
        if (!member->name || !member->name[0] || member->ordinal != variant ||
            member->payload_count < 0 || member->payload_count > UINT16_MAX ||
            !xr_enum_payload_names_are_exact(member->payload_names,
                                             (uint16_t) member->payload_count) ||
            (member->payload_count > 0 && !member->payload_types)) {
            valid = false;
            break;
        }
        variants[variant].payload_count = (uint32_t) member->payload_count;
        payload_total += variants[variant].payload_count;
        if (payload_total > UINT32_MAX) {
            valid = false;
            break;
        }
        if (member->payload_count == 0)
            continue;
        payload_storage[variant] =
            xr_calloc((size_t) member->payload_count, sizeof(*payload_storage[variant]));
        if (!payload_storage[variant]) {
            valid = false;
            break;
        }
        variants[variant].payload_types = payload_storage[variant];
        for (int field = 0; field < member->payload_count; ++field) {
            const XrType *payload_type =
                substitute_variant_payload_type(schema, type, member->payload_types[field]);
            if (!map_type_recursive(context, payload_type, &payload_storage[variant][field],
                                    nested_stack, depth + 1u)) {
                valid = false;
                break;
            }
        }
    }
    if (!valid)
        goto fail;

    XrCoreIrTypeOwnership ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
    XrCoreIrCopyContract copy_contract = XR_CORE_IR_COPY_TRIVIAL;
    for (uint32_t variant = 0u; variant < schema->member_count; ++variant) {
        for (uint32_t field = 0u; field < variants[variant].payload_count; ++field) {
            uint16_t child = variants[variant].payload_types[field];
            if (logical_ownership_for_type(context, child) != XR_CORE_IR_OWNER)
                continue;
            ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
            XrCoreIrCopyContract child_copy = logical_copy_contract_for_type(context, child);
            if (child_copy == XR_CORE_IR_COPY_FORBIDDEN)
                copy_contract = XR_CORE_IR_COPY_FORBIDDEN;
            else if (copy_contract == XR_CORE_IR_COPY_TRIVIAL)
                copy_contract = XR_CORE_IR_COPY_EXPLICIT;
        }
    }
    const XrClassInfo *nominal_info = nominal_info_for_type(type);
    if (!nominal_info ||
        !nominal_type_contract_is_exact(context, nominal_info->xg_decl_id, nominal_key, decl_kind,
                                        ownership, copy_contract))
        goto fail;

    const char *owner = type->enum_type.layout->nominal_owner;
    const char *name = type->enum_type.enum_name;
    size_t owner_length = owner ? strlen(owner) : 0u;
    size_t name_length = name ? strlen(name) : 0u;
    if (owner_length > UINT32_MAX || name_length > UINT32_MAX || owner_length > SIZE_MAX - 22u ||
        name_length > SIZE_MAX - 22u - owner_length)
        goto fail;
    const size_t type_reference_size = 1u + XR_CORE_IR_KEY_SIZE;
    size_t material_size = 22u + owner_length + name_length;
    for (uint32_t variant = 0; variant < schema->member_count; ++variant) {
        size_t variant_name_length =
            schema->members[variant].name ? strlen(schema->members[variant].name) : 0u;
        if (variant_name_length > UINT32_MAX)
            goto fail;
        size_t row_size = 8u + variant_name_length;
        for (uint32_t field = 0; field < variants[variant].payload_count; field++) {
            size_t field_name_length = strlen(schema->members[variant].payload_names[field]);
            if (field_name_length > UINT32_MAX ||
                field_name_length > SIZE_MAX - 4u - type_reference_size ||
                row_size > SIZE_MAX - 4u - type_reference_size - field_name_length)
                goto fail;
            row_size += 4u + field_name_length + type_reference_size;
        }
        if (row_size < variant_name_length || material_size > SIZE_MAX - row_size)
            goto fail;
        material_size += row_size;
    }
    uint8_t *material = xr_calloc(material_size, 1u);
    if (!material)
        goto fail;
    size_t cursor = 0u;
    material[cursor++] = UINT8_C(0x57);
    material[cursor++] = (uint8_t) nominal_kind;
    put_u64_be(material + cursor, nominal_key);
    cursor += 8u;
    put_u32_be(material + cursor, (uint32_t) owner_length);
    cursor += 4u;
    memcpy(material + cursor, owner, owner_length);
    cursor += owner_length;
    put_u32_be(material + cursor, (uint32_t) name_length);
    cursor += 4u;
    memcpy(material + cursor, name, name_length);
    cursor += name_length;
    put_u32_be(material + cursor, schema->member_count);
    cursor += 4u;
    for (uint32_t variant = 0; variant < schema->member_count; ++variant) {
        const char *variant_name = schema->members[variant].name;
        size_t variant_name_length = variant_name ? strlen(variant_name) : 0u;
        put_u32_be(material + cursor, (uint32_t) variant_name_length);
        cursor += 4u;
        memcpy(material + cursor, variant_name, variant_name_length);
        cursor += variant_name_length;
        put_u32_be(material + cursor, variants[variant].payload_count);
        cursor += 4u;
        for (uint32_t field = 0; field < variants[variant].payload_count; ++field) {
            const char *field_name = schema->members[variant].payload_names[field];
            size_t field_name_length = strlen(field_name);
            put_u32_be(material + cursor, (uint32_t) field_name_length);
            cursor += 4u;
            memcpy(material + cursor, field_name, field_name_length);
            cursor += field_name_length;
            put_dynamic_type_reference(context, material + cursor,
                                       variants[variant].payload_types[field]);
            cursor += type_reference_size;
        }
    }
    if (cursor != material_size) {
        xr_free(material);
        goto fail;
    }
    XrCoreIrKey semantic_key = xr_core_ir_key(material, material_size);
    xr_free(material);
    for (uint32_t index = 0; index < context->type_count; ++index) {
        if (xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key)) {
            *type_id = context->type_storage[index].input.local_id;
            goto reuse;
        }
    }
    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage)
        goto fail;
    storage->variants = variants;
    storage->variant_payload_types = payload_storage;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_VARIANT;
    storage->input.nominal_kind = nominal_kind;
    storage->input.ownership = ownership;
    storage->input.copy_contract = copy_contract;
    storage->input.variants = variants;
    storage->input.variant_count = schema->member_count;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;

reuse:
    for (uint32_t variant = 0; variant < schema->member_count; ++variant)
        xr_free(payload_storage[variant]);
    xr_free(payload_storage);
    xr_free(variants);
    return true;

fail:
    for (uint32_t variant = 0; variant < schema->member_count; ++variant)
        xr_free(payload_storage[variant]);
    xr_free(payload_storage);
    xr_free(variants);
    return false;
}

static bool map_aggregate_type_recursive(XrXiBuildContext *context, const XrType *type,
                                         uint16_t *type_id, const XrType *const *stack,
                                         uint32_t depth) {
    const XiModule *owner_module = NULL;
    const XiClassData *schema = find_aggregate_schema(context, type, &owner_module);
    uint8_t decl_kind = 0u;
    XrCoreIrNominalKind nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    uint64_t nominal_key = 0u;
    if (!schema || !owner_module || !owner_module->identity || !schema->class_name ||
        depth >= 64u || type_stack_contains(stack, depth, type) ||
        !nominal_contract(context, type, &decl_kind, &nominal_kind, &nominal_key) ||
        (decl_kind != XG_DECL_CLASS && decl_kind != XG_DECL_STRUCT) ||
        (decl_kind == XG_DECL_CLASS) != (nominal_kind == XR_CORE_IR_NOMINAL_CLASS) ||
        (decl_kind == XG_DECL_STRUCT) != (nominal_kind == XR_CORE_IR_NOMINAL_STRUCT))
        return false;

    uint32_t field_count = schema->instance_field_count;
    uint16_t *field_types = field_count != 0u ? xr_calloc(field_count, sizeof(*field_types)) : NULL;
    if (field_count != 0u && !field_types)
        return false;
    const XrType *nested_stack[64];
    if (depth != 0u)
        memcpy(nested_stack, stack, depth * sizeof(*nested_stack));
    nested_stack[depth] = type;
    for (uint32_t field = 0; field < field_count; ++field) {
        const char *field_name = schema->instance_field_names[field];
        const XrType *field_type = schema->instance_field_types[field];
        if (!field_name || !field_name[0] || !field_type ||
            (schema->struct_layout &&
             (!schema->struct_layout->field_names || !schema->struct_layout->field_names[field] ||
              strcmp(field_name, schema->struct_layout->field_names[field]) != 0)) ||
            !map_type_recursive(context, field_type, &field_types[field], nested_stack,
                                depth + 1u)) {
            xr_free(field_types);
            return false;
        }
    }

    XrCoreIrTypeOwnership ownership = nominal_kind == XR_CORE_IR_NOMINAL_CLASS
                                          ? XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
                                          : XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
    XrCoreIrCopyContract copy_contract = nominal_kind == XR_CORE_IR_NOMINAL_CLASS
                                             ? XR_CORE_IR_COPY_EXPLICIT
                                             : XR_CORE_IR_COPY_TRIVIAL;
    for (uint32_t field = 0u; field < field_count; ++field) {
        if (logical_ownership_for_type(context, field_types[field]) != XR_CORE_IR_OWNER)
            continue;
        ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
        XrCoreIrCopyContract child_copy =
            logical_copy_contract_for_type(context, field_types[field]);
        if (child_copy == XR_CORE_IR_COPY_FORBIDDEN)
            copy_contract = XR_CORE_IR_COPY_FORBIDDEN;
        else if (copy_contract == XR_CORE_IR_COPY_TRIVIAL)
            copy_contract = XR_CORE_IR_COPY_EXPLICIT;
    }
    const XrClassInfo *nominal_info = nominal_info_for_type(type);
    if (!nominal_info ||
        !nominal_type_contract_is_exact(context, nominal_info->xg_decl_id, nominal_key, decl_kind,
                                        ownership, copy_contract)) {
        xr_free(field_types);
        return false;
    }

    const size_t type_reference_size = 1u + XR_CORE_IR_KEY_SIZE;
    size_t material_size = 14u;
    for (uint32_t field = 0; field < field_count; ++field) {
        size_t field_name_length = strlen(schema->instance_field_names[field]);
        if (field_name_length > UINT32_MAX ||
            field_name_length > SIZE_MAX - 4u - type_reference_size ||
            material_size > SIZE_MAX - 4u - type_reference_size - field_name_length) {
            xr_free(field_types);
            return false;
        }
        material_size += 4u + field_name_length + type_reference_size;
    }
    uint8_t *material = xr_calloc(material_size, 1u);
    if (!material) {
        xr_free(field_types);
        return false;
    }
    size_t cursor = 0u;
    material[cursor++] = UINT8_C(0x53);
    material[cursor++] = (uint8_t) nominal_kind;
    put_u64_be(material + cursor, nominal_key);
    cursor += 8u;
    put_u32_be(material + cursor, field_count);
    cursor += 4u;
    for (uint32_t field = 0; field < field_count; ++field) {
        const char *field_name = schema->instance_field_names[field];
        size_t field_name_length = strlen(field_name);
        put_u32_be(material + cursor, (uint32_t) field_name_length);
        cursor += 4u;
        memcpy(material + cursor, field_name, field_name_length);
        cursor += field_name_length;
        put_dynamic_type_reference(context, material + cursor, field_types[field]);
        cursor += type_reference_size;
    }
    if (cursor != material_size) {
        xr_free(material);
        xr_free(field_types);
        return false;
    }
    XrCoreIrKey semantic_key = xr_core_ir_key(material, material_size);
    xr_free(material);
    for (uint32_t index = 0; index < context->type_count; ++index) {
        if (xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key)) {
            *type_id = context->type_storage[index].input.local_id;
            xr_free(field_types);
            return true;
        }
    }
    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage) {
        xr_free(field_types);
        return false;
    }
    storage->field_types = field_types;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_AGGREGATE;
    storage->input.nominal_kind = nominal_kind;
    storage->input.ownership = ownership;
    storage->input.copy_contract = copy_contract;
    storage->input.field_types = field_types;
    storage->input.field_count = field_count;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;
}

static bool map_existential_interface_id(XrXiBuildContext *context, XgInterfaceId interface_id,
                                         XiInterfaceUseKind use_kind, uint16_t *type_id) {
    if (!context || !type_id || interface_id == XG_NO_ID || use_kind < XI_INTERFACE_USE_READ ||
        use_kind > XI_INTERFACE_USE_OWNED_STORAGE)
        return false;
    XrCoreIrKey owner = interface_key(interface_id);
    uint8_t material[2u + XR_CORE_IR_KEY_SIZE];
    material[0] = UINT8_C(0x58);
    material[1] = (uint8_t) use_kind;
    memcpy(material + 2u, owner.bytes, sizeof(owner.bytes));
    XrCoreIrKey semantic_key = xr_core_ir_key(material, sizeof(material));
    for (uint32_t index = 0u; index < context->type_count; ++index) {
        if (!xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key))
            continue;
        *type_id = context->type_storage[index].input.local_id;
        return true;
    }
    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage)
        return false;
    bool read = use_kind == XI_INTERFACE_USE_READ;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_EXISTENTIAL;
    storage->input.ownership =
        read ? XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL : XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    storage->input.copy_contract = read ? XR_CORE_IR_COPY_TRIVIAL : XR_CORE_IR_COPY_FORBIDDEN;
    storage->input.existential_interface = owner;
    storage->input.interface_use_kind = (XrCoreIrInterfaceUseKind) use_kind;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;
}

static bool map_existential_type(XrXiBuildContext *context, const XrType *type,
                                 XiInterfaceUseKind use_kind, uint16_t *type_id) {
    const XrClassInfo *info =
        type && type->kind == XR_KIND_INTERFACE ? type->instance.class_ref : NULL;
    return info && !type->is_nullable && info->xg_interface_id != XG_NO_ID &&
           map_existential_interface_id(context, (XgInterfaceId) info->xg_interface_id, use_kind,
                                        type_id);
}

static bool map_type_for_mode(XrXiBuildContext *context, const XrType *type, XrParamMode mode,
                              uint16_t *type_id, const XrType *const *stack, uint32_t depth) {
    if (!type || type->kind != XR_KIND_INTERFACE)
        return map_type_recursive(context, type, type_id, stack, depth);
    XiInterfaceUseKind use_kind = XI_INTERFACE_USE_NONE;
    switch (mode) {
        case XR_PARAM_READ:
            use_kind = XI_INTERFACE_USE_READ;
            break;
        case XR_PARAM_REF:
            use_kind = XI_INTERFACE_USE_REF;
            break;
        case XR_PARAM_MOVE:
            use_kind = XI_INTERFACE_USE_MOVE;
            break;
        default:
            return false;
    }
    return map_existential_type(context, type, use_kind, type_id);
}

static bool map_type_recursive(XrXiBuildContext *context, const XrType *type, uint16_t *type_id,
                               const XrType *const *stack, uint32_t depth) {
    if (context && type && type_id && type->is_nullable)
        return map_optional_type_recursive(context, type, type_id, stack, depth);
    if (map_builtin_type(type, type_id))
        return true;
    if (context && type && type_id && type->kind == XR_KIND_FUNCTION)
        return map_callable_type_recursive(context, type, type_id, stack, depth);
    if (context && type && type_id && type->kind == XR_KIND_INTERFACE)
        return map_existential_type(context, type, XI_INTERFACE_USE_READ, type_id);
    if (context && type && type_id && !type->is_nullable && type->kind == XR_KIND_ENUM)
        return map_variant_type_recursive(context, type, type_id, stack, depth);
    if (context && type && type_id && !type->is_nullable &&
        (type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS) && type->instance.class_ref)
        return map_aggregate_type_recursive(context, type, type_id, stack, depth);
    if (!context || !type || !type_id || type->is_nullable || type->kind != XR_KIND_TUPLE ||
        type->tuple.element_count <= 0 || !type->tuple.element_types || depth >= 64u ||
        type_stack_contains(stack, depth, type))
        return false;

    uint32_t field_count = (uint32_t) type->tuple.element_count;
    if (field_count > UINT16_MAX)
        return false;
    uint16_t *field_types = xr_calloc(field_count, sizeof(*field_types));
    if (!field_types)
        return false;
    const XrType *nested_stack[64];
    if (depth != 0u)
        memcpy(nested_stack, stack, depth * sizeof(*nested_stack));
    nested_stack[depth] = type;
    for (uint32_t field = 0; field < field_count; ++field) {
        if (!map_type_recursive(context, type->tuple.element_types[field], &field_types[field],
                                nested_stack, depth + 1u)) {
            xr_free(field_types);
            return false;
        }
    }

    XrCoreIrTypeOwnership ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
    XrCoreIrCopyContract copy_contract = XR_CORE_IR_COPY_TRIVIAL;
    for (uint32_t field = 0u; field < field_count; ++field) {
        if (logical_ownership_for_type(context, field_types[field]) != XR_CORE_IR_OWNER)
            continue;
        ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
        XrCoreIrCopyContract child_copy =
            logical_copy_contract_for_type(context, field_types[field]);
        if (child_copy == XR_CORE_IR_COPY_FORBIDDEN)
            copy_contract = XR_CORE_IR_COPY_FORBIDDEN;
        else if (copy_contract == XR_CORE_IR_COPY_TRIVIAL)
            copy_contract = XR_CORE_IR_COPY_EXPLICIT;
    }

    const size_t row_size = 1u + XR_CORE_IR_KEY_SIZE;
    if (field_count > (SIZE_MAX - 5u) / row_size) {
        xr_free(field_types);
        return false;
    }
    size_t material_size = 5u + (size_t) field_count * row_size;
    uint8_t *material = xr_calloc(material_size, 1u);
    if (!material) {
        xr_free(field_types);
        return false;
    }
    material[0] = UINT8_C(0x54);
    put_u32_be(material + 1u, field_count);
    for (uint32_t field = 0; field < field_count; ++field) {
        uint8_t *row = material + 5u + (size_t) field * row_size;
        put_dynamic_type_reference(context, row, field_types[field]);
    }
    XrCoreIrKey semantic_key = xr_core_ir_key(material, material_size);
    xr_free(material);
    for (uint32_t index = 0; index < context->type_count; ++index) {
        if (xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key)) {
            *type_id = context->type_storage[index].input.local_id;
            xr_free(field_types);
            return true;
        }
    }
    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage) {
        xr_free(field_types);
        return false;
    }
    storage->field_types = field_types;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_AGGREGATE;
    storage->input.ownership = ownership;
    storage->input.copy_contract = copy_contract;
    storage->input.field_types = storage->field_types;
    storage->input.field_count = field_count;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;
}

static bool map_type(XrXiBuildContext *context, const XrType *type, uint16_t *type_id) {
    return map_type_recursive(context, type, type_id, NULL, 0u);
}

static const XiFunc *resolved_sealed_callee(const XrXiBuildContext *context, const XiFunc *caller,
                                            const XiValue *call);
static const XrStdlibDefEntry *resolved_provider_native_call(const XrXiBuildContext *context,
                                                             const XiFunc *caller,
                                                             const XiValue *call);
static const XrStdlibDefEntry *resolved_suspension_native_call(const XrXiBuildContext *context,
                                                               const XiFunc *caller,
                                                               const XiValue *call);
static const XiCoroSuspendPoint *coroutine_point_for_block(const XrXiFunctionStorage *function,
                                                           const XiBlock *block);

static bool exact_static_cleanup_try(const XiFunc *function, const XiValue *value) {
    if (!function || !value || value->op != XI_TRY || value->aux_int != XI_TRY_AUX_STATIC_CLEANUP ||
        value->nargs != 0u || !value->aux || !value->block || value->block->func != function ||
        (value->flags & XI_FLAG_SIDE_EFFECT) == 0u)
        return false;
    const XiBlock *handler = (const XiBlock *) value->aux;
    if (handler->func != function)
        return false;
    for (uint32_t index = 0u; index < handler->nvalues; ++index) {
        const XiValue *candidate = handler->values[index];
        if (candidate && candidate->op == XI_CATCH && candidate->aux == value &&
            candidate->nargs == 0u)
            return true;
    }
    return false;
}

static bool exact_static_cleanup_end(const XiFunc *function, const XiValue *value) {
    const XiValue *registration =
        value && value->op == XI_END_TRY ? (const XiValue *) value->aux : NULL;
    return value && value->nargs == 0u && (value->flags & XI_FLAG_SIDE_EFFECT) != 0u &&
           exact_static_cleanup_try(function, registration);
}

typedef enum XrXiCleanupReason {
    XR_XI_CLEANUP_REASON_TRAP = 1,
    XR_XI_CLEANUP_REASON_PANIC = 2,
    XR_XI_CLEANUP_REASON_CANCEL = 3,
} XrXiCleanupReason;

static XrProgramBuildStatus
claim_static_cleanup_handler_graph(XrXiFunctionStorage *function, const XiValue *registration,
                                   XrXiCleanupReason reason, bool *private_projection,
                                   char *diagnostic, size_t diagnostic_size);

static bool exact_cleanup_boundary_marker(const XiValue *value) {
    return value && (value->op == XI_CLEANUP_ENTER || value->op == XI_CLEANUP_LEAVE) &&
           value->nargs == 0u && value->type && value->type->kind == XR_KIND_UNIT &&
           (value->flags & XI_FLAG_SIDE_EFFECT) != 0u;
}

static XrProgramBuildStatus function_has_uncaught_panic(XrXiBuildContext *context,
                                                        const XiFunc *function,
                                                        const XiFunc **stack, uint32_t depth,
                                                        uint32_t capacity, bool *has_panic,
                                                        char *diagnostic, size_t diagnostic_size) {
    if (!context || !function || !stack || !has_panic || depth >= capacity)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi panic call graph is not finitely representable");
    for (uint32_t index = 0u; index < depth; ++index) {
        if (stack[index] == function) {
            *has_panic = false;
            return XR_PROGRAM_BUILD_OK;
        }
    }
    stack[depth] = function;
    bool found = false;
    bool has_static_cleanup = false;
    bool has_unrouted_panic = false;
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!canonical_block_is_reachable(context, function, block))
            continue;
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *value = block->values[value_index];
            if (!value)
                continue;
            if (value->op == XI_TRY) {
                if (!exact_static_cleanup_try(function, value))
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi function %s still uses implicit panic-handler state",
                                function->name ? function->name : "<anonymous>");
                has_static_cleanup = true;
                continue;
            }
            if (value->op == XI_END_TRY) {
                if (exact_static_cleanup_end(function, value))
                    continue;
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi function %s still uses implicit panic-handler state",
                            function->name ? function->name : "<anonymous>");
            }
            if (value->op == XI_CATCH &&
                (canonical_block_is_trap_cleanup(context, function, block) ||
                 canonical_block_is_panic_cleanup(context, function, block) ||
                 canonical_block_is_cancel_cleanup(context, function, block)))
                continue;
            if (value->op == XI_CATCH)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi function %s still uses implicit panic-handler state",
                            function->name ? function->name : "<anonymous>");
            if (value->op == XI_THROW) {
                if (canonical_block_is_trap_cleanup(context, function, block) ||
                    canonical_block_is_panic_cleanup(context, function, block) ||
                    canonical_block_is_cancel_cleanup(context, function, block))
                    continue;
                uint16_t payload_type = XR_CORE_TYPE_VOID;
                if (value->nargs != 1u || !value->args || !value->args[0] ||
                    !map_type(context, value->args[0]->type, &payload_type) ||
                    payload_type != XR_CORE_TYPE_PANIC_INFO)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi throw v%u does not publish typed PanicInfo", value->id);
                found = true;
                has_unrouted_panic |= !canonical_block_is_panic_cleanup(context, function, block);
            }
            if (value->op == XI_ASSERTION) {
                const XrAssertionPlan *plan = xi_assertion_plan(value);
                uint16_t condition_type = XR_CORE_TYPE_VOID;
                if (!plan || !xr_assertion_plan_validate(plan) ||
                    plan->kind != XR_ASSERTION_KIND_CONDITION || plan->arity != 1u ||
                    plan->message_operand != XR_ASSERTION_OPERAND_NONE || value->nargs != 1u ||
                    !value->args || !value->args[0] ||
                    !map_logical_value_type(context, function, value->args[0], &condition_type) ||
                    condition_type != XR_CORE_TYPE_BOOL)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi assertion v%u is not an exact condition assertion", value->id);
                found = true;
                has_unrouted_panic |= find_panic_edge(context, function, value) == NULL;
            }
            if (value->op == XI_CALL || value->op == XI_CALL_METHOD ||
                value->op == XI_CALL_METHOD_DIRECT) {
                const XiFunc *callee = resolved_sealed_callee(context, function, value);
                if (!callee)
                    continue;
                bool callee_panic = false;
                XrProgramBuildStatus status =
                    function_has_uncaught_panic(context, callee, stack, depth + 1u, capacity,
                                                &callee_panic, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
                found = found || callee_panic;
                has_unrouted_panic |=
                    callee_panic && find_panic_edge(context, function, value) == NULL;
            }
        }
    }
    if (found && has_static_cleanup && has_unrouted_panic)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %s lacks an explicit panic continuation for static cleanup",
                    function->name ? function->name : "<anonymous>");
    *has_panic = found;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus map_function_panic_type(XrXiBuildContext *context,
                                                    const XiFunc *function, uint16_t *panic_type_id,
                                                    char *diagnostic, size_t diagnostic_size) {
    if (!context || !function || !panic_type_id)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint32_t capacity = 1u;
    for (uint32_t module = 0u; module < context->source->module_count; ++module)
        capacity += context->storage[module].function_count;
    const XiFunc **stack = xr_calloc(capacity, sizeof(*stack));
    if (!stack)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    bool has_panic = false;
    XrProgramBuildStatus status = function_has_uncaught_panic(
        context, function, stack, 0u, capacity, &has_panic, diagnostic, diagnostic_size);
    xr_free(stack);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    *panic_type_id = has_panic ? XR_CORE_TYPE_PANIC_INFO : XR_CORE_TYPE_VOID;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus map_function_error_type(XrXiBuildContext *context,
                                                    const XiFunc *function, uint16_t *error_type_id,
                                                    char *diagnostic, size_t diagnostic_size) {
    if (!context || !function || !error_type_id)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    *error_type_id = XR_CORE_TYPE_VOID;
    if (function->module && function->module->init == function) {
        bool publishes_error = false;
        for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
            const XiBlock *block = function->blocks[block_index];
            if (!canonical_block_is_reachable(context, function, block))
                continue;
            publishes_error |= block && block->control && block->control->op == XI_ERR_RETURN;
            for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index)
                publishes_error |=
                    block->values[value_index] && block->values[value_index]->op == XI_ERR_RETURN;
        }
        if (!publishes_error)
            return XR_PROGRAM_BUILD_OK;
    }
    if (function->error_effect_nothrow)
        return XR_PROGRAM_BUILD_OK;
    const XaAnalyzer *analyzer = function->analyzer;
    const XaEffectSummary *effect =
        analyzer && function->analyzer_effect_id != XA_EFFECT_NONE
            ? xa_effect_db_get(analyzer->effect_db, function->analyzer_effect_id)
            : NULL;
    /* Error continuation construction depends only on the error-channel
     * component of the effect product.  Uncertainty in allocation,
     * suspension, or task-spawn semantics must not poison a complete typed
     * escaping-error set. */
    if (!effect || effect->error_set_completeness != XA_EFFECT_COMPLETE ||
        effect->error_unknown_reasons != XA_UNKNOWN_NONE || effect->escaping.count != 1u)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %s (Xglobal body %u, effect %u, complete=%u, nothrow=%u) "
                    "does not have one complete typed escaping-error set",
                    function->name ? function->name : "<anonymous>", function->xg_body_func_id,
                    function->analyzer_effect_id, function->analyzer_effect_complete ? 1u : 0u,
                    function->error_effect_nothrow ? 1u : 0u);
    const XaErrorTypeSet *error = &effect->escaping.types[0];
    XrType *type = xa_effect_db_error_type_handle(analyzer->effect_db, error->type_id);
    if (!type || !map_type(context, type, error_type_id) || *error_type_id == XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %s escaping error type is not active in CoreSpec",
                    function->name ? function->name : "<anonymous>");
    const XrXiTypeStorage *mapped = find_dynamic_type_by_id(context, *error_type_id);
    if (!mapped || mapped->input.kind != XR_CORE_IR_TYPE_VARIANT)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %s escaping error is not a closed record enum",
                    function->name ? function->name : "<anonymous>");
    return XR_PROGRAM_BUILD_OK;
}

static XrCoreIrOwnershipDisposition logical_ownership_for_type(const XrXiBuildContext *context,
                                                               uint16_t type_id) {
    if (type_id == XR_CORE_TYPE_PANIC_INFO)
        return XR_CORE_IR_OWNER;
    const XrXiTypeStorage *type = find_dynamic_type_by_id(context, type_id);
    return type && type->input.ownership == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ? XR_CORE_IR_OWNER
                                                                             : XR_CORE_IR_NON_OWNER;
}

static XrCoreIrCopyContract logical_copy_contract_for_type(const XrXiBuildContext *context,
                                                           uint16_t type_id) {
    if (type_id == XR_CORE_TYPE_VOID || type_id == XR_CORE_TYPE_PANIC_INFO)
        return XR_CORE_IR_COPY_FORBIDDEN;
    const XrXiTypeStorage *type = find_dynamic_type_by_id(context, type_id);
    return type ? type->input.copy_contract : XR_CORE_IR_COPY_TRIVIAL;
}

static bool logical_type_is_optional_i64_pair(const XrXiBuildContext *context, uint16_t type_id) {
    const XrXiTypeStorage *optional = find_dynamic_type_by_id(context, type_id);
    if (!optional || optional->input.kind != XR_CORE_IR_TYPE_VARIANT ||
        optional->input.nominal_kind != XR_CORE_IR_NOMINAL_NONE ||
        optional->input.ownership != XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL ||
        optional->input.copy_contract != XR_CORE_IR_COPY_TRIVIAL ||
        optional->input.variant_count != 2u || !optional->input.variants ||
        optional->input.variants[0].payload_count != 0u ||
        optional->input.variants[1].payload_count != 1u ||
        !optional->input.variants[1].payload_types)
        return false;
    const XrXiTypeStorage *pair =
        find_dynamic_type_by_id(context, optional->input.variants[1].payload_types[0]);
    return pair && pair->input.kind == XR_CORE_IR_TYPE_AGGREGATE &&
           pair->input.nominal_kind == XR_CORE_IR_NOMINAL_NONE &&
           pair->input.ownership == XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL &&
           pair->input.copy_contract == XR_CORE_IR_COPY_TRIVIAL && pair->input.field_count == 2u &&
           pair->input.field_types && pair->input.field_types[0] == XR_CORE_TYPE_I64 &&
           pair->input.field_types[1] == XR_CORE_TYPE_I64;
}

static const XrXiFunctionStorage *find_xi_function(const XrXiBuildContext *context,
                                                   const XiFunc *needle, uint32_t *module_index_out,
                                                   uint32_t *function_index_out) {
    if (!context || !needle)
        return NULL;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        const XrXiModuleStorage *storage = &context->storage[module_index];
        if (!storage->root || !storage->function_storage || !storage->xi_functions)
            continue;
        for (uint32_t function_index = 0; function_index < storage->function_count;
             ++function_index) {
            const XiFunc *function = storage->xi_functions[function_index];
            if (function == needle) {
                if (module_index_out)
                    *module_index_out = module_index;
                if (function_index_out)
                    *function_index_out = function_index;
                return &storage->function_storage[function_index];
            }
        }
    }
    return NULL;
}

/* Function collection precedes canonical reachability and storage allocation.
 * Queries that only
 * need source-module ownership must therefore inspect the
 * immutable collected Xi pointers, not
 * the later XrXiFunctionStorage rows. */
static bool find_collected_xi_function_module(const XrXiBuildContext *context, const XiFunc *needle,
                                              uint32_t *module_index_out) {
    if (!context || !needle)
        return false;
    for (uint32_t module_index = 0u; module_index < context->source->module_count; ++module_index) {
        const XrXiModuleStorage *storage = &context->storage[module_index];
        for (uint32_t function_index = 0u;
             storage->xi_functions && function_index < storage->function_count; ++function_index) {
            if (storage->xi_functions[function_index] != needle)
                continue;
            if (module_index_out)
                *module_index_out = module_index;
            return true;
        }
    }
    return false;
}

static bool map_callable_target_type(XrXiBuildContext *context, const XrType *type,
                                     const XiFunc *target, uint16_t *type_id);

static const XiFunc *resolved_sealed_callee(const XrXiBuildContext *context, const XiFunc *caller,
                                            const XiValue *call);
static const XiValue *logical_value_identity(const XiValue *value);
static const XiClassData *resolved_empty_class_allocation(const XrXiBuildContext *context,
                                                          const XiFunc *caller,
                                                          const XiValue *call);
static const XiClassData *resolved_fieldwise_class_construction(const XrXiBuildContext *context,
                                                                const XiFunc *caller,
                                                                const XiValue *call);
static const XiClassData *resolved_canonical_class_construction(const XrXiBuildContext *context,
                                                                const XiFunc *caller,
                                                                const XiValue *call,
                                                                XiValue *const **field_values_out,
                                                                uint32_t *field_count_out);
static const XiClassData *resolved_value_aggregate_construction(const XrXiBuildContext *context,
                                                                const XiFunc *caller,
                                                                const XiValue *allocation,
                                                                XiValue **field_values_out,
                                                                uint32_t *field_count_out);
static bool value_aggregate_initializer_is_exact(const XrXiBuildContext *context,
                                                 const XiFunc *caller, const XiValue *initializer);
static const XiClassData *resolved_empty_struct_literal(const XrXiBuildContext *context,
                                                        const XiFunc *caller, const XiValue *call);
static const XiClassData *resolved_class_carrier(const XrXiBuildContext *context,
                                                 const XiFunc *caller, const XiValue *value,
                                                 XgClassId expected_class_id,
                                                 uint32_t *module_index_out);
static const XiEnumData *resolved_unit_enum_literal(const XrXiBuildContext *context,
                                                    const XiFunc *caller, const XiValue *load,
                                                    uint32_t *variant_ordinal);

static bool value_is_only_elided_operand_recursive(const XrXiBuildContext *context,
                                                   const XiFunc *function, const XiValue *value,
                                                   uint32_t depth) {
    if (!function || !value || depth > function->next_value_id + 1u)
        return false;
    bool found = false;
    for (uint32_t block_index = 0; function && block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!canonical_block_is_reachable(context, function, block))
            continue;
        if (block->control == value)
            return false;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            for (uint16_t argument = 0u; argument < phi->value.nargs; ++argument)
                if (phi->value.args && phi->value.args[argument] == value)
                    return false;
        for (uint32_t value_index = 0; block && value_index < block->nvalues; ++value_index) {
            const XiValue *consumer = block->values[value_index];
            for (uint16_t argument = 0; consumer && argument < consumer->nargs; ++argument) {
                if (consumer->args[argument] != value)
                    continue;
                bool elided = false;
                if (argument == 0u &&
                    (consumer->op == XI_VARIANT_CONSTRUCT ||
                     (((consumer->op == XI_CALL || consumer->op == XI_CALL_METHOD ||
                        consumer->op == XI_CALL_METHOD_DIRECT) &&
                       (resolved_sealed_callee(context, function, consumer) ||
                        resolved_provider_native_call(context, function, consumer) ||
                        resolved_suspension_native_call(context, function, consumer))) ||
                      resolved_canonical_class_construction(context, function, consumer, NULL,
                                                            NULL)) ||
                     resolved_value_aggregate_construction(context, function, consumer, NULL,
                                                           NULL) ||
                     resolved_class_carrier(context, function, consumer, XG_NO_ID, NULL) ||
                     resolved_empty_struct_literal(context, function, consumer) ||
                     resolved_unit_enum_literal(context, function, consumer, NULL))) {
                    elided = true;
                    found = true;
                } else if (argument == 0u &&
                           (xi_value_forwards_identity(consumer) || consumer->op == XI_RETAIN)) {
                    elided = value_is_only_elided_operand_recursive(context, function, consumer,
                                                                    depth + 1u);
                    found = found || elided;
                } else if (argument == 1u && (consumer->op == XI_IS || consumer->op == XI_AS)) {
                    elided = true;
                    found = true;
                } else if (consumer->op == XI_RELEASE) {
                    elided = true;
                }
                if (!elided)
                    return false;
            }
        }
    }
    /* Lowering may emit a dead physical retain next to a logical value use.
     * It is
     * representation-only, but a retained value with any reachable use
     * must still prove that
     * every such use is elided. */
    return found || value->op == XI_RETAIN;
}

static bool value_is_only_elided_operand(const XrXiBuildContext *context, const XiFunc *function,
                                         const XiValue *value) {
    return value_is_only_elided_operand_recursive(context, function, value, 0u);
}

static const XgCallsiteSummary *resolved_callsite(const XrXiBuildContext *context,
                                                  const XiFunc *caller, const XiValue *call) {
    if (!context || !context->source || !context->source->global_evidence || !caller || !call ||
        call->xg_callsite_id == XG_NO_ID || caller->xg_body_func_id == XG_NO_ID)
        return NULL;
    const XgCallsiteSummary *row = xg_global_evidence_find_callsite(
        context->source->global_evidence, (XgCallsiteId) call->xg_callsite_id);
    if (!row || row->owner_func_id != (XgFuncId) caller->xg_body_func_id)
        return NULL;
    return row;
}

/* The generated stdlib definition is the sole source-side authority that maps
 * a grounded native
 * import to a logical provider contract. Xglobal still has
 * to prove the exact native callsite
 * identity; module/member spelling alone
 * never grants provider authority. */
static const XrStdlibDefEntry *resolved_provider_native_call(const XrXiBuildContext *context,
                                                             const XiFunc *caller,
                                                             const XiValue *call) {
    if (!context || !caller || !call || call->op != XI_CALL || call->nargs == 0u || !call->args ||
        call->nargs - 1u > UINT16_MAX)
        return NULL;
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    const XiImportRef *reference = xi_value_import_ref(caller, call->args[0]);
    if (!row || row->kind != XG_CALL_NATIVE || (row->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u ||
        (row->flags & (XG_CALL_MAY_ERROR | XG_CALL_MAY_PANIC)) != 0u ||
        !xi_import_ref_is_grounded_native(reference) || !reference->member_name ||
        row->arg_count != (uint16_t) (call->nargs - 1u) ||
        row->method_id != (XgMethodId) xg_name_id(reference->member_name))
        return NULL;
    const XrStdlibDefEntry *entry = xr_stdlib_metadata_exact_native_direct_call(
        reference->module_path, reference->member_name, row->arg_count);
    if (!entry || !entry->provider_contract_key || !entry->provider_contract_key[0] ||
        !entry->provider_operation_key || !entry->provider_operation_key[0] ||
        entry->runtime_capabilities != 0u || entry->return_ownership[0] != '\0' || entry->argc > 1u)
        return NULL;
    return entry;
}

static const XrStdlibDefEntry *resolved_suspension_native_call(const XrXiBuildContext *context,
                                                               const XiFunc *caller,
                                                               const XiValue *call) {
    if (!context || !caller || !call || call->op != XI_CALL || call->nargs != 2u || !call->args)
        return NULL;
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    const XiImportRef *reference = xi_value_import_ref(caller, call->args[0]);
    if (!row || row->kind != XG_CALL_NATIVE || (row->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u ||
        (row->flags & (XG_CALL_MAY_ERROR | XG_CALL_MAY_PANIC)) != 0u ||
        !xi_import_ref_is_grounded_native(reference) || !reference->member_name ||
        row->arg_count != 1u || row->method_id != (XgMethodId) xg_name_id(reference->member_name))
        return NULL;
    return xr_stdlib_metadata_exact_native_suspension_call(reference->module_path,
                                                           reference->member_name, row->arg_count);
}

static bool projected_native_import_reference_is_exact(const XrXiBuildContext *context,
                                                       const XiFunc *function,
                                                       const XiValue *value) {
    const XiImportRef *reference = value && value->op == XI_IMPORT_REF && value->nargs == 0u
                                       ? (const XiImportRef *) value->aux
                                       : NULL;
    if (!xi_import_ref_is_grounded_native(reference))
        return false;
    bool found = false;
    for (uint32_t block_index = 0u; function && block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!canonical_block_is_reachable(context, function, block))
            continue;
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *call = block->values[value_index];
            if (!resolved_provider_native_call(context, function, call) &&
                !resolved_suspension_native_call(context, function, call))
                continue;
            if (xi_value_import_ref(function, call->args[0]) == reference)
                found = true;
        }
    }
    return found;
}

static const XgCallsiteSummary *resolved_witness_callsite(const XrXiBuildContext *context,
                                                          const XiFunc *caller,
                                                          const XiValue *call) {
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    uint32_t slot = UINT32_MAX;
    if (!row || !call || row->kind != XG_CALL_INTERFACE ||
        row->receiver_static_interface_id != call->xg_interface_id ||
        row->method_id != call->xg_method_id ||
        (row->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u ||
        !xg_global_evidence_interface_dispatch_slot(context->source->global_evidence,
                                                    row->receiver_static_interface_id,
                                                    row->method_id, &slot) ||
        slot != call->xg_interface_dispatch_slot)
        return NULL;
    bool fallible = (row->flags & (XG_CALL_MAY_ERROR | XG_CALL_MAY_PANIC)) != 0u;
    if ((call->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE) != fallible ||
        (call->xg_existential_kind != XI_EXISTENTIAL_WITNESS_DIRECT &&
         call->xg_existential_kind != XI_EXISTENTIAL_WITNESS_INVOKE))
        return NULL;
    return row;
}

static const XiFunc *find_xi_function_by_xg_id(const XrXiBuildContext *context,
                                               XgFuncId function_id) {
    if (!context || function_id == XG_NO_ID)
        return NULL;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        const XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0;
             module->xi_functions && function_index < module->function_count; ++function_index) {
            const XiFunc *function = module->xi_functions[function_index];
            if (function && function->xg_body_func_id == function_id)
                return function;
        }
    }
    return NULL;
}

static bool canonical_block_is_reachable(const XrXiBuildContext *context, const XiFunc *function,
                                         const XiBlock *block) {
    const XrXiFunctionStorage *storage = find_xi_function(context, function, NULL, NULL);
    if (!storage || !storage->block_storage || !block)
        return false;
    for (uint32_t index = 0u; index < function->nblocks; ++index)
        if (storage->block_storage[index].xi == block)
            return storage->block_storage[index].reachable;
    return false;
}

static const XgClassSummary *find_xg_class_by_id(const XgGlobalEvidence *evidence,
                                                 XgClassId class_id) {
    const XgClassSummary *found = NULL;
    for (uint32_t index = 0u; evidence && index < evidence->nclasses; ++index) {
        const XgClassSummary *candidate = &evidence->classes[index];
        if (candidate->class_id != class_id)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static bool resolved_module_namespace_carrier(const XrXiBuildContext *context, const XiFunc *caller,
                                              const XiValue *value) {
    value = logical_value_identity(value);
    const XiImportRef *ref = xi_value_import_ref(caller, value);
    if (!context || !context->source || !value || value->op != XI_GET_SHARED || !ref ||
        ref->member_name || !ref->resolution_attempted || ref->resolved_mod_index < 0 ||
        (uint32_t) ref->resolved_mod_index >= context->source->module_count ||
        ref->resolved_shared_slot >= 0 || ref->resolved_export_slot >= 0 || ref->resolved_func ||
        !ref->resolved_module)
        return false;
    const XiFunc *root = context->source->module_roots[ref->resolved_mod_index];
    return root && root->module == ref->resolved_module && ref->resolved_module->init == root;
}

static bool resolved_namespace_reexport(const XrXiBuildContext *context,
                                        const XiFunc *namespace_root, const char *public_name,
                                        uint32_t *module_index_out, uint32_t *export_index_out) {
    if (module_index_out)
        *module_index_out = UINT32_MAX;
    if (export_index_out)
        *export_index_out = UINT32_MAX;
    if (!context || !context->source || !namespace_root || !public_name ||
        !namespace_root->reexports)
        return false;

    uint32_t selected_module = UINT32_MAX;
    uint32_t selected_export = UINT32_MAX;
    uint32_t selective_matches = 0u;
    for (uint16_t index = 0u; index < namespace_root->reexport_count; ++index) {
        const XiReexportEntry *entry = &namespace_root->reexports[index];
        const char *visible = entry->alias ? entry->alias : entry->name;
        if (!entry->name || !visible || strcmp(visible, public_name) != 0)
            continue;
        if (!entry->resolution_attempted || !entry->resolution_complete ||
            entry->resolved_mod_index < 0 || entry->resolved_export_slot < 0)
            return false;
        selected_module = (uint32_t) entry->resolved_mod_index;
        selected_export = (uint32_t) entry->resolved_export_slot;
        ++selective_matches;
    }
    if (selective_matches > 1u)
        return false;

    uint32_t star_matches = 0u;
    if (selective_matches == 0u) {
        for (uint16_t index = 0u; index < namespace_root->reexport_count; ++index) {
            const XiReexportEntry *entry = &namespace_root->reexports[index];
            if (entry->name)
                continue;
            if (!entry->resolution_attempted || !entry->resolution_complete ||
                (entry->resolved_member_count != 0u && !entry->resolved_members))
                return false;
            for (uint16_t member = 0u; member < entry->resolved_member_count; ++member) {
                const XiResolvedReexport *resolved = &entry->resolved_members[member];
                if (!resolved->export_name || strcmp(resolved->export_name, public_name) != 0)
                    continue;
                if (resolved->mod_index < 0 || resolved->export_slot < 0)
                    return false;
                selected_module = (uint32_t) resolved->mod_index;
                selected_export = (uint32_t) resolved->export_slot;
                ++star_matches;
            }
        }
        if (star_matches != 1u)
            return false;
    }

    if (selected_module >= context->source->module_count)
        return false;
    const XiFunc *owner_root = context->source->module_roots[selected_module];
    const XiModule *owner_module = owner_root ? owner_root->module : NULL;
    if (!owner_module || owner_module->init != owner_root ||
        selected_export >= owner_module->nexports || !owner_module->exports)
        return false;
    if (module_index_out)
        *module_index_out = selected_module;
    if (export_index_out)
        *export_index_out = selected_export;
    return true;
}

static const XiClassData *resolved_class_carrier(const XrXiBuildContext *context,
                                                 const XiFunc *caller, const XiValue *value,
                                                 XgClassId expected_class_id,
                                                 uint32_t *module_index_out) {
    if (module_index_out)
        *module_index_out = UINT32_MAX;
    value = logical_value_identity(value);
    if (!context || !context->source || !caller || !value)
        return NULL;

    if (value->op == XI_LOAD_FIELD) {
        const XiValue *namespace_value = value->nargs == 1u && value->args ? value->args[0] : NULL;
        const XiImportRef *namespace_ref = xi_value_import_ref(caller, namespace_value);
        if (!resolved_module_namespace_carrier(context, caller, namespace_value) ||
            !namespace_ref || !value->aux || !value->type || value->type->kind != XR_KIND_CLASS ||
            !value->type->instance.class_ref ||
            value->type->instance.class_ref->xg_class_id == XG_NO_ID)
            return NULL;
        uint32_t module_index = (uint32_t) namespace_ref->resolved_mod_index;
        const XiFunc *root = context->source->module_roots[module_index];
        const XiModule *module = root ? root->module : NULL;
        XgClassId class_id = value->type->instance.class_ref->xg_class_id;
        const char *public_name = (const char *) value->aux;
        const XiModuleExport *export_row = NULL;
        for (uint16_t index = 0u; module && module->exports && index < module->nexports; ++index) {
            const XiModuleExport *candidate = &module->exports[index];
            if (!candidate->name || strcmp(candidate->name, public_name) != 0)
                continue;
            if (export_row)
                return NULL;
            export_row = candidate;
        }
        if (!export_row) {
            uint32_t export_index = UINT32_MAX;
            if (!resolved_namespace_reexport(context, root, public_name, &module_index,
                                             &export_index))
                return NULL;
            root = context->source->module_roots[module_index];
            module = root ? root->module : NULL;
            export_row = module && module->exports && export_index < module->nexports
                             ? &module->exports[export_index]
                             : NULL;
        }
        const XiClassData *class_data = export_row ? export_row->class_data : NULL;
        const XgClassSummary *class_row =
            class_data ? find_xg_class_by_id(context->source->global_evidence, class_id) : NULL;
        if (!module || module->init != root || !class_data || !export_row ||
            class_data->xg_class_id != class_id || export_row->function ||
            export_row->shared_slot >= module->nslots || !module->slot_classes ||
            module->slot_classes[export_row->shared_slot] != class_data || !class_row ||
            class_row->module_id != (XgModuleId) (module_index + 1u) ||
            (expected_class_id != XG_NO_ID && class_id != expected_class_id))
            return NULL;
        if (module_index_out)
            *module_index_out = module_index;
        return class_data;
    }

    const XiImportRef *ref = xi_value_import_ref(caller, value);
    if (value->op == XI_IMPORT_REF) {
        if (!ref)
            return NULL;
    } else if (value->op != XI_GET_SHARED || value->aux_int < 0) {
        return NULL;
    }
    uint32_t module_index = UINT32_MAX;
    const XiModule *module = NULL;
    const XiClassData *class_data = NULL;
    if (ref) {
        if (!ref->resolution_attempted || ref->resolved_mod_index < 0 ||
            (uint32_t) ref->resolved_mod_index >= context->source->module_count ||
            ref->resolved_shared_slot < 0 || ref->resolved_export_slot < 0 ||
            !ref->resolved_module || ref->resolved_func)
            return NULL;
        module_index = (uint32_t) ref->resolved_mod_index;
        const XiFunc *root = context->source->module_roots[module_index];
        module = root ? root->module : NULL;
        uint32_t slot = (uint32_t) ref->resolved_shared_slot;
        uint32_t export_slot = (uint32_t) ref->resolved_export_slot;
        if (!module || module != ref->resolved_module || module->init != root ||
            slot >= module->nslots || export_slot >= module->nexports || !module->slot_classes ||
            !module->exports)
            return NULL;
        class_data = module->slot_classes[slot];
        const XiModuleExport *export_row = &module->exports[export_slot];
        if (!class_data || export_row->shared_slot != slot || export_row->function ||
            export_row->class_data != class_data)
            return NULL;
    } else {
        if (!find_collected_xi_function_module(context, caller, &module_index) ||
            module_index >= context->source->module_count)
            return NULL;
        const XiFunc *root = context->source->module_roots[module_index];
        module = root ? root->module : NULL;
        uint32_t slot = (uint32_t) value->aux_int;
        if (!module || module->init != root || slot >= module->nslots || !module->slot_classes)
            return NULL;
        class_data = module->slot_classes[slot];
    }
    const XgClassSummary *class_row =
        class_data ? find_xg_class_by_id(context->source->global_evidence, class_data->xg_class_id)
                   : NULL;
    if (!class_row || class_row->class_id == XG_NO_ID ||
        class_row->module_id != (XgModuleId) (module_index + 1u) ||
        (expected_class_id != XG_NO_ID && class_row->class_id != expected_class_id))
        return NULL;
    if (value->type && value->type->kind != XR_KIND_UNKNOWN &&
        (value->type->kind != XR_KIND_CLASS || !value->type->instance.class_ref ||
         value->type->instance.class_ref->xg_class_id != class_row->class_id))
        return NULL;
    if (module_index_out)
        *module_index_out = module_index;
    return class_data;
}

static const XiFunc *
resolved_class_method_body_callee(const XrXiBuildContext *context, const XgCallsiteSummary *row,
                                  const XiClassData *class_data, uint32_t module_index,
                                  const XrType *receiver_type, bool expect_static) {
    const XgGlobalEvidence *evidence = context->source->global_evidence;
    const XgClassSummary *class_row =
        class_data ? find_xg_class_by_id(evidence, row->receiver_static_class_id) : NULL;
    const XgMethodSummary *method = NULL;
    for (uint32_t index = 0u; evidence && index < evidence->nmethods; ++index) {
        const XgMethodSummary *candidate = &evidence->methods[index];
        if (candidate->method_id != row->method_id)
            continue;
        if (method)
            return NULL;
        method = candidate;
    }
    if (!class_row || !method || class_data->xg_class_id != class_row->class_id ||
        class_row->module_id != (XgModuleId) (module_index + 1u) ||
        method->owner_class_id != class_row->class_id ||
        ((method->flags & XG_METHOD_STATIC) != 0u) != expect_static ||
        (method->flags & (XG_METHOD_CONSTRUCTOR | XG_METHOD_NATIVE | XG_METHOD_GENERIC_TEMPLATE |
                          XG_METHOD_OVERRIDDEN)) != 0u)
        return NULL;

    const XgBodySummary *body = NULL;
    for (uint32_t index = 0u; index < evidence->nbodies; ++index) {
        const XgBodySummary *candidate = &evidence->bodies[index];
        if (candidate->kind != XG_BODY_METHOD || candidate->owner_method_id != method->method_id)
            continue;
        if (body)
            return NULL;
        body = candidate;
    }
    if (!body || body->func_id == XG_NO_ID || body->module_id != class_row->module_id ||
        body->module_id != (XgModuleId) (module_index + 1u) ||
        body->owner_decl_id != class_row->decl_id || body->owner_class_id != class_row->class_id ||
        body->source_node_id != method->source_node_id || body->name_id != method->name_id ||
        body->signature_key == 0u || body->signature_key != method->signature_key ||
        (body->flags & XG_BODY_GENERIC_TEMPLATE) != 0u)
        return NULL;

    const XiFunc *callee = find_xi_function_by_xg_id(context, body->func_id);
    const XiFunc *root = context->source->module_roots[module_index];
    uint32_t matches = 0u;
    for (uint16_t index = 0u; class_data && class_data->methods && class_data->child_idx && root &&
                              index < class_data->nmethod;
         ++index) {
        uint16_t child = class_data->child_idx[index];
        const XiClassMethod *xi_method = &class_data->methods[index];
        if (child >= root->nchildren || root->children[child] != callee)
            continue;
        if (xi_method->is_static != expect_static || xi_method->is_constructor ||
            xi_method->is_static_constructor || !xi_method->name ||
            xg_name_id(xi_method->name) != method->name_id)
            return NULL;
        ++matches;
    }
    uint32_t expected_parameters = row->arg_count + (expect_static ? 0u : 1u);
    if (!callee || matches != 1u || callee->has_receiver != !expect_static ||
        callee->nparams != expected_parameters || (expected_parameters != 0u && !callee->params))
        return NULL;
    if (!expect_static &&
        (!receiver_type || receiver_type->kind != XR_KIND_INSTANCE || receiver_type->is_nullable ||
         !callee->params[0] || !callee->params[0]->type ||
         !nominal_instance_identity_equal(context, callee->params[0]->type, receiver_type) ||
         !xr_param_mode_is_valid(callee->receiver_mode)))
        return NULL;
    return callee;
}

static const XiFunc *resolved_static_method_callee(const XrXiBuildContext *context,
                                                   const XiFunc *caller, const XiValue *call) {
    if (!context || !caller || !call ||
        (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) || call->nargs == 0u ||
        !call->args)
        return NULL;
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    if (!row || row->kind != XG_CALL_METHOD || row->receiver_static_class_id == XG_NO_ID ||
        row->method_id == XG_NO_ID || call->xg_method_id != row->method_id ||
        (row->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u || row->arg_count == UINT16_MAX ||
        call->nargs != (uint16_t) (row->arg_count + 1u))
        return NULL;

    uint32_t module_index = UINT32_MAX;
    const XiClassData *class_data = resolved_class_carrier(
        context, caller, call->args[0], row->receiver_static_class_id, &module_index);
    return class_data ? resolved_class_method_body_callee(context, row, class_data, module_index,
                                                          NULL, true)
                      : NULL;
}

static const XiFunc *resolved_instance_method_callee(const XrXiBuildContext *context,
                                                     const XiFunc *caller, const XiValue *call) {
    if (!context || !caller || !call ||
        (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) || call->nargs == 0u ||
        !call->args)
        return NULL;
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    const XiValue *receiver = logical_value_identity(call->args[0]);
    if (!row || row->kind != XG_CALL_METHOD || row->receiver_static_class_id == XG_NO_ID ||
        row->method_id == XG_NO_ID || call->xg_method_id != row->method_id ||
        (row->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u || row->arg_count == UINT16_MAX ||
        call->nargs != (uint16_t) (row->arg_count + 1u) || !receiver || !receiver->type ||
        receiver->type->kind != XR_KIND_INSTANCE || receiver->type->is_nullable ||
        !nominal_contract(context, receiver->type, NULL, NULL, NULL))
        return NULL;

    const XiModule *owner_module = NULL;
    const XiClassData *class_data = find_aggregate_schema(context, receiver->type, &owner_module);
    uint32_t module_index = UINT32_MAX;
    for (uint32_t index = 0u; owner_module && index < context->source->module_count; ++index) {
        const XiFunc *root = context->source->module_roots[index];
        if (!root || root->module != owner_module)
            continue;
        if (module_index != UINT32_MAX)
            return NULL;
        module_index = index;
    }
    if (!class_data || class_data->xg_class_id != row->receiver_static_class_id ||
        module_index == UINT32_MAX)
        return NULL;
    return resolved_class_method_body_callee(context, row, class_data, module_index, receiver->type,
                                             false);
}

/* A qualified source call such as `time.now()` is represented in Xi as a
 * method-shaped call on a
 * phase-only module namespace carrier.  Xglobal owns
 * the actual declaration target, so admit the
 * call only when the namespace
 * import, export table, callsite target and Xi body all join
 * exactly. */
static const XiFunc *resolved_module_function_callee(const XrXiBuildContext *context,
                                                     const XiFunc *caller, const XiValue *call) {
    if (!context || !caller || !call ||
        (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) || call->nargs == 0u ||
        !call->args)
        return NULL;
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    const XiValue *namespace_value = call->args[0];
    if (!row || row->kind != XG_CALL_DIRECT_FUNC || row->static_target_func_id == XG_NO_ID ||
        (row->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u || row->arg_count == UINT16_MAX ||
        call->nargs != (uint16_t) (row->arg_count + 1u) ||
        !resolved_module_namespace_carrier(context, caller, namespace_value))
        return NULL;
    const XiFunc *callee = find_xi_function_by_xg_id(context, row->static_target_func_id);
    const XiFunc *exported = xi_value_resolve_method_callee(caller, call);
    if (!callee || exported != callee)
        return NULL;
    return callee;
}

static const XiFunc *resolved_sealed_callee(const XrXiBuildContext *context, const XiFunc *caller,
                                            const XiValue *call) {
    if (!call || call->nargs == 0u)
        return NULL;
    if (call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT) {
        const XiFunc *callee = resolved_static_method_callee(context, caller, call);
        if (!callee)
            callee = resolved_instance_method_callee(context, caller, call);
        return callee ? callee : resolved_module_function_callee(context, caller, call);
    }
    if (call->op != XI_CALL)
        return NULL;
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    if (!row || row->kind != XG_CALL_DIRECT_FUNC || row->static_target_func_id == XG_NO_ID)
        return NULL;
    return find_xi_function_by_xg_id(context, row->static_target_func_id);
}

/* A source `C()` with no declared constructor still uses a class carrier in
 * Xi. The carrier is
 * phase-only in Program, but it may disappear only when
 * its exact local, imported, or namespace
 * export resolution and the Xglobal
 * class-allocation row name the same declaration. Restrict
 * this normalization
 * to fieldless declarations; defaults need an explicit value graph. */
static const XiClassData *resolved_empty_class_allocation(const XrXiBuildContext *context,
                                                          const XiFunc *caller,
                                                          const XiValue *call) {
    if (!context || !context->source || !context->source->global_evidence || !caller || !call ||
        (call->op != XI_CALL && call->op != XI_CALL_METHOD) || call->nargs != 1u || !call->args ||
        !call->type || call->type->kind != XR_KIND_INSTANCE || call->type->is_nullable)
        return NULL;
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    if (!row || row->kind != XG_CALL_CLASS_ALLOC || row->receiver_static_class_id == XG_NO_ID ||
        row->arg_count != 0u)
        return NULL;
    uint32_t module_index = UINT32_MAX;
    const XiClassData *class_data = resolved_class_carrier(
        context, caller, call->args[0], row->receiver_static_class_id, &module_index);
    if (!class_data || module_index >= context->source->module_count ||
        class_data->instance_field_count != 0u || class_data->is_generic_skeleton)
        return NULL;
    for (uint16_t method = 0u; method < class_data->nmethod; ++method)
        if (class_data->methods && class_data->methods[method].is_constructor &&
            !class_data->methods[method].is_static)
            return NULL;
    const XrClassInfo *info = nominal_info_for_type(call->type);
    uint8_t decl_kind = 0u;
    XrCoreIrNominalKind nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    uint64_t nominal_key = 0u;
    const XgClassSummary *class_row =
        find_xg_class_by_id(context->source->global_evidence, row->receiver_static_class_id);
    const XgDeclSummary *decl_row =
        class_row ? find_xg_decl_by_id(context->source->global_evidence, class_row->decl_id) : NULL;
    if (!info || !class_row || !decl_row ||
        class_row->module_id != (XgModuleId) (module_index + 1u) ||
        decl_row->module_id != class_row->module_id || class_row->class_id != info->xg_class_id ||
        class_row->class_id != class_data->xg_class_id || class_row->decl_id != info->xg_decl_id ||
        decl_row->decl_id != class_row->decl_id || class_row->decl_kind != XG_DECL_CLASS ||
        decl_row->kind != XG_DECL_CLASS || class_row->parent_class_id != XG_NO_ID ||
        (class_row->flags & XG_CLASS_GENERIC_SKELETON) != 0u ||
        !nominal_contract(context, call->type, &decl_kind, &nominal_kind, &nominal_key) ||
        decl_kind != XG_DECL_CLASS || nominal_kind != XR_CORE_IR_NOMINAL_CLASS ||
        nominal_key != info->xg_nominal_key || decl_row->nominal_key != nominal_key)
        return NULL;
    return class_data;
}

/* Canonical Program has one target-neutral aggregate construction primitive;
 * a source
 * constructor may collapse into it only when the complete constructor
 * body proves that it is
 * exactly a record initializer. Xglobal owns every
 * declaration identity in this proof, while Xi
 * owns the concrete parameter to
 * field value graph. No spelling-based lookup, default
 * evaluation, inherited
 * layout, computation, effect, or control flow is admitted. */
static const XiClassData *resolved_fieldwise_class_construction(const XrXiBuildContext *context,
                                                                const XiFunc *caller,
                                                                const XiValue *call) {
    if (!context || !context->source || !context->source->global_evidence || !caller || !call ||
        (call->op != XI_CALL && call->op != XI_CALL_METHOD) || call->nargs <= 1u || !call->args ||
        !xi_value_is_constructor_call(call))
        return NULL;

    const XgGlobalEvidence *evidence = context->source->global_evidence;
    const XgCallsiteSummary *callsite = resolved_callsite(context, caller, call);
    if (!callsite || callsite->kind != XG_CALL_METHOD ||
        callsite->receiver_static_class_id == XG_NO_ID || callsite->method_id == XG_NO_ID ||
        call->xg_method_id != callsite->method_id ||
        (callsite->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u ||
        (callsite->flags & (XG_CALL_MAY_ERROR | XG_CALL_MAY_PANIC)) != 0u ||
        callsite->arg_count == UINT16_MAX || callsite->arg_count != (uint16_t) (call->nargs - 1u))
        return NULL;

    uint32_t module_index = UINT32_MAX;
    const XiClassData *class_data = resolved_class_carrier(
        context, caller, call->args[0], callsite->receiver_static_class_id, &module_index);
    const XgClassSummary *class_row =
        class_data ? find_xg_class_by_id(evidence, callsite->receiver_static_class_id) : NULL;
    const XrClassInfo *result_info = nominal_info_for_type(call->type);
    uint8_t result_decl_kind = 0u;
    if (!class_data || !class_row || !result_info ||
        class_row->module_id != (XgModuleId) (module_index + 1u) ||
        class_row->decl_kind != XG_DECL_CLASS || class_row->parent_class_id != XG_NO_ID ||
        class_row->decl_id != result_info->xg_decl_id ||
        class_data->xg_class_id != class_row->class_id || class_data->instance_field_count == 0u ||
        class_data->instance_field_count != call->nargs - 1u ||
        class_row->field_count != class_data->instance_field_count ||
        class_row->field_start == 0u || !class_data->instance_field_names ||
        !class_data->instance_field_types || !class_data->instance_field_source_node_ids ||
        !nominal_contract(context, call->type, &result_decl_kind, NULL, NULL) ||
        result_decl_kind != XG_DECL_CLASS)
        return NULL;

    const XgMethodSummary *method = NULL;
    for (uint32_t index = 0u; index < evidence->nmethods; ++index) {
        const XgMethodSummary *candidate = &evidence->methods[index];
        if (candidate->method_id != callsite->method_id)
            continue;
        if (method)
            return NULL;
        method = candidate;
    }
    if (!method || method->owner_class_id != class_row->class_id ||
        (method->flags & XG_METHOD_CONSTRUCTOR) == 0u ||
        (method->flags & (XG_METHOD_STATIC | XG_METHOD_NATIVE | XG_METHOD_GENERIC_TEMPLATE |
                          XG_METHOD_OVERRIDDEN)) != 0u)
        return NULL;

    const XgBodySummary *body = NULL;
    for (uint32_t index = 0u; index < evidence->nbodies; ++index) {
        const XgBodySummary *candidate = &evidence->bodies[index];
        if (candidate->kind != XG_BODY_METHOD || candidate->owner_method_id != method->method_id)
            continue;
        if (body)
            return NULL;
        body = candidate;
    }
    if (!body || body->func_id == XG_NO_ID || body->module_id != class_row->module_id ||
        body->owner_decl_id != class_row->decl_id || body->owner_class_id != class_row->class_id ||
        body->source_node_id != method->source_node_id || body->name_id != method->name_id ||
        body->signature_key == 0u || body->signature_key != method->signature_key ||
        (body->flags & XG_BODY_GENERIC_TEMPLATE) != 0u)
        return NULL;

    const XiFunc *constructor = NULL;
    const XiFunc *root = module_index < context->source->module_count
                             ? context->source->module_roots[module_index]
                             : NULL;
    uint32_t constructor_matches = 0u;
    for (uint16_t index = 0u;
         class_data->methods && class_data->child_idx && root && index < class_data->nmethod;
         ++index) {
        uint16_t child = class_data->child_idx[index];
        const XiClassMethod *descriptor = &class_data->methods[index];
        const XiFunc *candidate = child < root->nchildren ? root->children[child] : NULL;
        if (!candidate || candidate->xg_body_func_id != body->func_id)
            continue;
        if (!descriptor->is_constructor || descriptor->is_static ||
            descriptor->is_static_constructor)
            return NULL;
        if (constructor && constructor != candidate)
            return NULL;
        constructor = candidate;
        ++constructor_matches;
    }
    if (!constructor || constructor_matches != 1u || !constructor->is_constructor ||
        constructor->has_receiver || constructor->nparams != call->nargs || !constructor->params ||
        !constructor->return_type || !call->type ||
        !xr_type_equals(constructor->return_type, call->type) || constructor->nblocks != 1u ||
        !constructor->entry || constructor->blocks[0] != constructor->entry)
        return NULL;

    const XiBlock *block = constructor->entry;
    if (block->kind != XI_BLOCK_RETURN || block->phis || block->npreds != 0u || block->succs[0] ||
        block->succs[1] || logical_value_identity(block->control) != constructor->params[0])
        return NULL;

    for (uint16_t parameter = 0u; parameter < constructor->nparams; ++parameter) {
        const XiValue *value = constructor->params[parameter];
        if (!value || value->op != XI_PARAM || value->block != block || value->aux_int != parameter)
            return NULL;
        if (parameter == 0u) {
            if (!xr_type_equals(value->type, call->type))
                return NULL;
        } else if (!call->args[parameter] ||
                   !xr_type_equals(value->type, call->args[parameter]->type) ||
                   !xr_type_equals(value->type, class_data->instance_field_types[parameter - 1u])) {
            return NULL;
        }
    }

    uint32_t stores = 0u;
    uint64_t seen_fields = 0u;
    if (class_data->instance_field_count > 64u)
        return NULL;
    for (uint32_t index = 0u; index < block->nvalues; ++index) {
        const XiValue *value = block->values[index];
        if (!value)
            return NULL;
        if (value == block->control || value->op == XI_PARAM)
            continue;
        if (value->op != XI_STORE_FIELD || value->nargs != 2u || !value->args ||
            logical_value_identity(value->args[0]) != constructor->params[0] ||
            value->xg_class_field_id == XG_NO_ID)
            return NULL;

        const XgClassFieldSummary *field = NULL;
        for (uint32_t field_index = 0u; field_index < evidence->nclass_fields; ++field_index) {
            const XgClassFieldSummary *candidate = &evidence->class_fields[field_index];
            if (candidate->field_id != value->xg_class_field_id)
                continue;
            if (field)
                return NULL;
            field = candidate;
        }
        if (!field || field->owner_class_id != class_row->class_id ||
            field->module_id != class_row->module_id ||
            (field->flags & XG_CLASS_FIELD_STATIC) != 0u ||
            field->instance_slot >= class_data->instance_field_count ||
            field->decl_ordinal != field->instance_slot ||
            field->source_node_id !=
                class_data->instance_field_source_node_ids[field->instance_slot] ||
            field->name_id != xg_name_id(class_data->instance_field_names[field->instance_slot]) ||
            !value->aux ||
            strcmp((const char *) value->aux,
                   class_data->instance_field_names[field->instance_slot]) != 0 ||
            logical_value_identity(value->args[1]) !=
                constructor->params[field->instance_slot + 1u] ||
            !xr_type_equals(value->args[1]->type,
                            class_data->instance_field_types[field->instance_slot]) ||
            !xr_type_equals(value->type, class_data->instance_field_types[field->instance_slot]) ||
            (seen_fields & (UINT64_C(1) << field->instance_slot)) != 0u)
            return NULL;
        seen_fields |= UINT64_C(1) << field->instance_slot;
        ++stores;
    }
    uint64_t expected_fields = class_data->instance_field_count == 64u
                                   ? UINT64_MAX
                                   : (UINT64_C(1) << class_data->instance_field_count) - 1u;
    return stores == class_data->instance_field_count && seen_fields == expected_fields ? class_data
                                                                                        : NULL;
}

static const XiClassData *resolved_canonical_class_construction(const XrXiBuildContext *context,
                                                                const XiFunc *caller,
                                                                const XiValue *call,
                                                                XiValue *const **field_values_out,
                                                                uint32_t *field_count_out) {
    if (field_values_out)
        *field_values_out = NULL;
    if (field_count_out)
        *field_count_out = 0u;
    const XiClassData *class_data = resolved_empty_class_allocation(context, caller, call);
    if (class_data)
        return class_data;
    class_data = resolved_fieldwise_class_construction(context, caller, call);
    if (!class_data)
        return NULL;
    if (field_values_out)
        *field_values_out = call->args + 1u;
    if (field_count_out)
        *field_count_out = class_data->instance_field_count;
    return class_data;
}

/* A value-struct literal is physical Xi allocation followed by one AGG_SET per
 * field. Canonical
 * Program has no uninitialized aggregate state: collapse that
 * exact sequence into one logical
 * aggregate.construct only after the
 * specialized nominal declaration, detached layout, Xglobal
 * field table and
 * every initializer operand agree. This is also the point where a
 *
 * monomorphized Box$i64 becomes a distinct Program TypeId; the open Box<T>
 * skeleton is never a
 * runtime value or an erased construction fallback. */
static const XiClassData *resolved_value_aggregate_construction(const XrXiBuildContext *context,
                                                                const XiFunc *caller,
                                                                const XiValue *allocation,
                                                                XiValue **field_values_out,
                                                                uint32_t *field_count_out) {
    if (field_values_out)
        memset(field_values_out, 0, XR_MAX_AGG_FIELDS * sizeof(*field_values_out));
    if (field_count_out)
        *field_count_out = 0u;
    const XrAggregateLayout *layout = allocation && allocation->op == XI_AGG_NEW
                                          ? (const XrAggregateLayout *) allocation->aux
                                          : NULL;
    const XrClassInfo *info =
        allocation && allocation->type ? nominal_info_for_type(allocation->type) : NULL;
    if (!context || !context->source || !context->source->global_evidence || !caller ||
        !allocation || allocation->nargs != 1u || !allocation->args || !allocation->args[0] ||
        !allocation->block || allocation->block->func != caller || !layout ||
        (layout->kind != XR_AGG_LAYOUT_STRUCT && layout->kind != XR_AGG_LAYOUT_PACKED_STRUCT) ||
        layout->field_count > XR_MAX_AGG_FIELDS || !layout->nominal_name ||
        (layout->field_count != 0u && !layout->field_names) || !allocation->type ||
        allocation->type->is_nullable ||
        (allocation->type->kind != XR_KIND_INSTANCE && allocation->type->kind != XR_KIND_CLASS) ||
        !info || info->xg_class_id == XG_NO_ID || info->xg_decl_id == XG_NO_ID ||
        info->xg_nominal_key == 0u)
        return NULL;

    uint32_t module_index = UINT32_MAX;
    const XiClassData *class_data = resolved_class_carrier(context, caller, allocation->args[0],
                                                           info->xg_class_id, &module_index);
    const XgGlobalEvidence *evidence = context->source->global_evidence;
    const XgClassSummary *class_row = find_xg_class_by_id(evidence, info->xg_class_id);
    const XgDeclSummary *decl_row =
        class_row ? find_xg_decl_by_id(evidence, class_row->decl_id) : NULL;
    uint8_t decl_kind = 0u;
    XrCoreIrNominalKind nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    uint64_t nominal_key = 0u;
    bool identity_exact =
        class_data && class_row && decl_row && module_index < context->source->module_count &&
        class_data->xg_class_id == class_row->class_id && class_row->decl_id == info->xg_decl_id &&
        decl_row->decl_id == info->xg_decl_id && decl_row->nominal_key == info->xg_nominal_key &&
        class_row->module_id == (XgModuleId) (module_index + 1u) &&
        decl_row->module_id == class_row->module_id;
    bool declaration_exact = identity_exact && class_row->decl_kind == XG_DECL_STRUCT &&
                             decl_row->kind == XG_DECL_STRUCT &&
                             class_row->parent_class_id == XG_NO_ID;
    bool specialization_exact =
        declaration_exact && (class_row->flags & XG_CLASS_GENERIC_SKELETON) == 0u &&
        !class_data->is_generic_skeleton &&
        (((class_row->flags & XG_CLASS_MONOMORPHIZED) != 0u) == class_data->is_monomorphized);
    bool layout_exact = specialization_exact && class_data->struct_layout &&
                        xr_aggregate_layout_semantically_equal(class_data->struct_layout, layout) &&
                        class_data->instance_field_count == layout->field_count &&
                        class_row->field_count == layout->field_count &&
                        (layout->field_count == 0u ||
                         (class_data->instance_field_names && class_data->instance_field_types &&
                          class_data->instance_field_source_node_ids));
    bool nominal_exact =
        nominal_contract(context, allocation->type, &decl_kind, &nominal_kind, &nominal_key) &&
        decl_kind == XG_DECL_STRUCT && nominal_kind == XR_CORE_IR_NOMINAL_STRUCT &&
        nominal_key == info->xg_nominal_key;
    if (!layout_exact || !nominal_exact)
        return NULL;

    if ((class_row->flags & XG_CLASS_MONOMORPHIZED) != 0u) {
        const XgClassSummary *origin =
            find_xg_class_by_id(evidence, class_row->generic_origin_class_id);
        const XgGenericInstSummary *instance = NULL;
        for (uint32_t index = 0u; index < evidence->ngeneric_insts; ++index) {
            const XgGenericInstSummary *candidate = &evidence->generic_insts[index];
            if (candidate->specialized_class_id != class_row->class_id)
                continue;
            if (instance)
                return NULL;
            instance = candidate;
        }
        const uint32_t required_flags = XG_GENERIC_INST_CONCRETE_TYPES |
                                        XG_GENERIC_INST_SPECIALIZED_ABI |
                                        XG_GENERIC_INST_CONCRETE_STORAGE;
        if (!origin || !instance || (origin->flags & XG_CLASS_GENERIC_SKELETON) == 0u ||
            instance->kind != XG_GENERIC_INST_CLASS ||
            instance->origin_class_id != class_row->generic_origin_class_id ||
            instance->origin_decl_id != origin->decl_id ||
            instance->name_id != class_row->generic_origin_name_id ||
            instance->receiver_class_id != XG_NO_ID || instance->receiver_type_key != 0u ||
            instance->receiver_type_arg_key_start != 0u ||
            instance->receiver_type_arg_count != 0u ||
            instance->declaration_type_key != class_row->generic_type_key ||
            instance->declaration_type_arg_key_start != class_row->generic_type_arg_key_start ||
            instance->declaration_type_arg_count != class_row->generic_type_arg_count ||
            instance->declaration_type_arg_count == 0u ||
            instance->specialization_effect != XG_GENERIC_SPECIALIZATION_EFFECT_NONE ||
            (instance->flags & required_flags) != required_flags ||
            !class_data->generic_origin_name ||
            xg_name_id(class_data->generic_origin_name) != class_row->generic_origin_name_id ||
            class_data->mono_type_arg_count != class_row->generic_type_arg_count)
            return NULL;
    } else if (class_row->generic_origin_class_id != XG_NO_ID ||
               class_row->generic_origin_name_id != 0u || class_row->generic_type_key != 0u ||
               class_row->generic_type_arg_key_start != 0u ||
               class_row->generic_type_arg_count != 0u || class_data->generic_origin_name ||
               class_data->mono_type_arg_count != 0u) {
        return NULL;
    }

    uint64_t seen_fields = 0u;
    uint32_t initialization_count = 0u;
    bool saw_allocation = false;
    bool saw_non_initializer_use = false;
    const XiBlock *allocation_block = allocation->block;
    for (const XiPhi *phi = allocation_block->phis; phi; phi = phi->next)
        for (uint16_t argument = 0u; argument < phi->value.nargs; ++argument)
            if (phi->value.args && logical_value_identity(phi->value.args[argument]) == allocation)
                return NULL;
    for (uint32_t value_index = 0u; value_index < allocation_block->nvalues; ++value_index) {
        const XiValue *value = allocation_block->values[value_index];
        if (value == allocation) {
            if (saw_allocation)
                return NULL;
            saw_allocation = true;
            continue;
        }
        for (uint16_t argument = 0u; value && argument < value->nargs; ++argument) {
            if (!value->args || logical_value_identity(value->args[argument]) != allocation)
                continue;
            bool initializer = value->op == XI_AGG_SET && argument == 0u;
            if (!initializer) {
                if (!saw_allocation || initialization_count != layout->field_count)
                    return NULL;
                saw_non_initializer_use = true;
                continue;
            }
            uint32_t ordinal = value->aux_int >= 0 ? (uint32_t) value->aux_int : UINT32_MAX;
            if (!saw_allocation || saw_non_initializer_use || value->nargs != 2u ||
                !value->args[1] || value->aux != layout || ordinal >= layout->field_count ||
                (seen_fields & (UINT64_C(1) << ordinal)) != 0u || !layout->field_names[ordinal] ||
                strcmp(layout->field_names[ordinal], class_data->instance_field_names[ordinal]) !=
                    0 ||
                !xr_type_equals(value->args[1]->type, class_data->instance_field_types[ordinal]))
                return NULL;
            const XgClassFieldSummary *field = NULL;
            for (uint32_t field_index = 0u; field_index < evidence->nclass_fields; ++field_index) {
                const XgClassFieldSummary *candidate = &evidence->class_fields[field_index];
                if (candidate->owner_class_id != class_row->class_id ||
                    candidate->instance_slot != ordinal)
                    continue;
                if (field)
                    return NULL;
                field = candidate;
            }
            if (!field || field->module_id != class_row->module_id ||
                field->decl_ordinal != ordinal || (field->flags & XG_CLASS_FIELD_STATIC) != 0u ||
                field->source_node_id != class_data->instance_field_source_node_ids[ordinal] ||
                field->name_id != xg_name_id(class_data->instance_field_names[ordinal]))
                return NULL;
            seen_fields |= UINT64_C(1) << ordinal;
            ++initialization_count;
            if (field_values_out)
                field_values_out[ordinal] = value->args[1];
        }
    }
    if (allocation_block->control &&
        logical_value_identity(allocation_block->control) == allocation) {
        if (!saw_allocation || initialization_count != layout->field_count)
            return NULL;
        saw_non_initializer_use = true;
    }
    uint64_t expected_fields =
        layout->field_count == 64u ? UINT64_MAX : (UINT64_C(1) << layout->field_count) - 1u;
    if (!saw_allocation || initialization_count != layout->field_count ||
        seen_fields != expected_fields)
        return NULL;

    xi_ensure_dominators((XiFunc *) caller);
    for (uint32_t block_index = 0u; block_index < caller->nblocks; ++block_index) {
        const XiBlock *block = caller->blocks[block_index];
        if (!block || block == allocation_block)
            continue;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            for (uint16_t argument = 0u; argument < phi->value.nargs; ++argument) {
                if (!phi->value.args ||
                    logical_value_identity(phi->value.args[argument]) != allocation)
                    continue;
                if (argument >= block->npreds || !block->preds[argument] ||
                    !xi_dominates(allocation_block, block->preds[argument]))
                    return NULL;
            }
        }
        for (uint32_t value_index = 0u; value_index < block->nvalues; ++value_index) {
            const XiValue *value = block->values[value_index];
            for (uint16_t argument = 0u; value && argument < value->nargs; ++argument) {
                if (!value->args || logical_value_identity(value->args[argument]) != allocation)
                    continue;
                if (value->op == XI_AGG_SET || !xi_dominates(allocation_block, block))
                    return NULL;
            }
        }
        if (block->control && logical_value_identity(block->control) == allocation &&
            !xi_dominates(allocation_block, block))
            return NULL;
    }
    if (field_count_out)
        *field_count_out = layout->field_count;
    return class_data;
}

static bool value_aggregate_initializer_is_exact(const XrXiBuildContext *context,
                                                 const XiFunc *caller, const XiValue *initializer) {
    const XiValue *allocation = initializer && initializer->op == XI_AGG_SET &&
                                        initializer->nargs == 2u && initializer->args
                                    ? logical_value_identity(initializer->args[0])
                                    : NULL;
    XiValue *field_values[XR_MAX_AGG_FIELDS];
    uint32_t field_count = 0u;
    if (!resolved_value_aggregate_construction(context, caller, allocation, field_values,
                                               &field_count) ||
        initializer->aux_int < 0 || (uint64_t) initializer->aux_int >= field_count)
        return false;
    return field_values[initializer->aux_int] == initializer->args[1];
}

/* Field operations share Xi opcodes with namespace and enum access.  Admit a
 * canonical aggregate
 * access only when the lowered field id, exact receiver
 * nominal identity, Xglobal declaration
 * row, and detached Xi layout all name
 * the same declared instance field.  The field spelling is
 * corroboration, not
 * lookup authority. */
static bool resolved_aggregate_field_access(const XrXiBuildContext *context, const XiValue *access,
                                            XiOp expected_operation, uint32_t *field_ordinal_out) {
    if (field_ordinal_out)
        *field_ordinal_out = UINT32_MAX;
    uint16_t expected_arguments = expected_operation == XI_STORE_FIELD ? 2u : 1u;
    if (!context || !context->source || !context->source->global_evidence || !access ||
        (expected_operation != XI_LOAD_FIELD && expected_operation != XI_STORE_FIELD) ||
        access->op != expected_operation || access->nargs != expected_arguments || !access->args ||
        !access->args[0] || (expected_operation == XI_STORE_FIELD && !access->args[1]) ||
        access->xg_class_field_id == XG_NO_ID)
        return false;

    const XiValue *receiver = logical_value_identity(access->args[0]);
    if (!receiver || !receiver->type || receiver->type->kind != XR_KIND_INSTANCE ||
        receiver->type->is_nullable)
        return false;
    const XiClassData *schema = find_aggregate_schema(context, receiver->type, NULL);
    const XrClassInfo *receiver_info = nominal_info_for_type(receiver->type);
    const XgClassSummary *class_row =
        schema ? find_xg_class_by_id(context->source->global_evidence, schema->xg_class_id) : NULL;
    if (!schema || !receiver_info || !class_row ||
        schema->xg_class_id != receiver_info->xg_class_id ||
        class_row->decl_id != receiver_info->xg_decl_id || class_row->parent_class_id != XG_NO_ID ||
        class_row->field_count != schema->instance_field_count ||
        (schema->instance_field_count != 0u &&
         (!schema->instance_field_names || !schema->instance_field_types ||
          !schema->instance_field_source_node_ids)))
        return false;

    const XgClassFieldSummary *field = NULL;
    const XgGlobalEvidence *evidence = context->source->global_evidence;
    for (uint32_t index = 0u; index < evidence->nclass_fields; ++index) {
        const XgClassFieldSummary *candidate = &evidence->class_fields[index];
        if (candidate->field_id != access->xg_class_field_id)
            continue;
        if (field)
            return false;
        field = candidate;
    }
    if (!field || field->owner_class_id != class_row->class_id ||
        field->module_id != class_row->module_id || (field->flags & XG_CLASS_FIELD_STATIC) != 0u ||
        field->instance_slot >= schema->instance_field_count ||
        field->decl_ordinal != field->instance_slot ||
        field->source_node_id != schema->instance_field_source_node_ids[field->instance_slot] ||
        field->name_id != xg_name_id(schema->instance_field_names[field->instance_slot]) ||
        !access->aux ||
        strcmp((const char *) access->aux, schema->instance_field_names[field->instance_slot]) !=
            0 ||
        !xr_type_equals(expected_operation == XI_STORE_FIELD ? access->args[1]->type : access->type,
                        schema->instance_field_types[field->instance_slot]))
        return false;
    if (field_ordinal_out)
        *field_ordinal_out = field->instance_slot;
    return true;
}

static bool resolved_aggregate_field_projection(const XrXiBuildContext *context,
                                                const XiValue *access,
                                                uint32_t *field_ordinal_out) {
    return resolved_aggregate_field_access(context, access, XI_LOAD_FIELD, field_ordinal_out);
}

static bool logical_value_is_place(const XiFunc *function, const XiValue *value) {
    value = logical_value_identity(value);
    bool ref_receiver = function && function->has_receiver &&
                        function->receiver_mode == XR_PARAM_REF && function->nparams != 0u &&
                        function->params && logical_value_identity(function->params[0]) == value;
    return value && (ref_receiver || (value->op == XI_PARAM && value->param_mode == XR_PARAM_REF) ||
                     value->op == XI_LOCAL_ADDR);
}

static const XiValue *direct_projection_base_place(const XrXiBuildContext *context,
                                                   const XiFunc *function, const XiValue *candidate,
                                                   uint32_t *field_ordinal_out) {
    if (field_ordinal_out)
        *field_ordinal_out = UINT32_MAX;
    const XiValue *place = logical_value_identity(candidate);
    if (!place || place->op != XI_LOCAL_ADDR || place->nargs != 1u || !place->args ||
        !place->args[0] || (place->aux_int & XI_LOCAL_ADDR_AUX_DIRECT_PROJECTION) == 0u)
        return NULL;
    const XiValue *projection = logical_value_identity(place->args[0]);
    uint32_t field_ordinal = UINT32_MAX;
    if (!resolved_aggregate_field_projection(context, projection, &field_ordinal) ||
        !projection->type || !place->type || !xr_type_equals(projection->type, place->type))
        return NULL;
    const XiValue *receiver = logical_value_identity(projection->args[0]);
    if (!receiver || !xi_own_type_may_be_ref(receiver->type))
        return NULL;
    if (receiver->op == XI_PLACE_LOAD) {
        if (receiver->nargs != 1u || !receiver->args || !receiver->args[0] ||
            !logical_value_is_place(function, receiver->args[0]))
            return NULL;
        receiver = logical_value_identity(receiver->args[0]);
    }
    if (field_ordinal_out)
        *field_ordinal_out = field_ordinal;
    return receiver;
}

/* Direct field references mutate the field itself. Xi's subsequent writeback
 * stores a load from
 * that same place to that same field and has no further
 * semantic effect. Match both declaration
 * identity and receiver identity. */
static bool direct_projection_writeback_is_exact(const XrXiBuildContext *context,
                                                 const XiFunc *function, const XiValue *value) {
    uint32_t ordinal = UINT32_MAX;
    if (!resolved_aggregate_field_access(context, value, XI_STORE_FIELD, &ordinal))
        return false;
    const XiValue *load = logical_value_identity(value->args[1]);
    if (!load || load->op != XI_PLACE_LOAD || load->nargs != 1u || !load->args || !value->block ||
        load->block != value->block)
        return false;
    bool adjacent = false;
    for (uint32_t index = 1u; index < value->block->nvalues; ++index)
        adjacent |=
            value->block->values[index] == value && value->block->values[index - 1u] == load;
    if (!adjacent)
        return false;
    const XiValue *place = logical_value_identity(load->args[0]);
    uint32_t projected = UINT32_MAX;
    if (!direct_projection_base_place(context, function, place, &projected) || ordinal != projected)
        return false;
    const XiValue *field = logical_value_identity(place->args[0]);
    return field->xg_class_field_id == value->xg_class_field_id &&
           logical_value_identity(field->args[0]) == logical_value_identity(value->args[0]);
}

static const XiValue *aggregate_field_store_place(const XiFunc *function, const XiValue *access) {
    if (!access || access->op != XI_STORE_FIELD || access->nargs != 2u || !access->args ||
        !access->args[0])
        return NULL;
    const XiValue *receiver = logical_value_identity(access->args[0]);
    if (logical_value_is_place(function, receiver))
        return receiver;
    if (!receiver || receiver->op != XI_PLACE_LOAD || receiver->nargs != 1u || !receiver->args ||
        !receiver->args[0] || !xi_own_type_may_be_ref(receiver->type) ||
        !logical_value_is_place(function, receiver->args[0]))
        return NULL;
    return logical_value_identity(receiver->args[0]);
}

static bool resolved_aggregate_field_store(const XrXiBuildContext *context, const XiFunc *function,
                                           const XiValue *access, uint32_t *field_ordinal_out) {
    if (!resolved_aggregate_field_access(context, access, XI_STORE_FIELD, field_ordinal_out))
        return false;
    return aggregate_field_store_place(function, access) != NULL;
}
/* A const binding changes access through an instance but not the instance's
 * nominal runtime
 * identity.  Method target resolution therefore compares the
 * declaration contract and concrete
 * type arguments directly, while receiver
 * mutability remains enforced by the analyzer-owned
 * receiver mode. */
static bool nominal_instance_identity_equal(const XrXiBuildContext *context, const XrType *left,
                                            const XrType *right) {
    const XrClassInfo *left_info = nominal_info_for_type(left);
    const XrClassInfo *right_info = nominal_info_for_type(right);
    if (!left || !right || left->kind != XR_KIND_INSTANCE || right->kind != XR_KIND_INSTANCE ||
        left->is_nullable || right->is_nullable || !left_info || !right_info ||
        !nominal_contract(context, left, NULL, NULL, NULL) ||
        !nominal_contract(context, right, NULL, NULL, NULL) || left_info->xg_class_id == XG_NO_ID ||
        left_info->xg_class_id != right_info->xg_class_id ||
        left_info->xg_decl_id != right_info->xg_decl_id ||
        left_info->xg_nominal_key != right_info->xg_nominal_key ||
        left->instance.type_arg_count != right->instance.type_arg_count)
        return false;
    for (int index = 0; index < left->instance.type_arg_count; ++index) {
        if (!left->instance.type_args || !right->instance.type_args ||
            !xr_type_equals(left->instance.type_args[index], right->instance.type_args[index]))
            return false;
    }
    return true;
}

/* An empty struct literal may retain the generic constructor-shaped Xi form
 * when no runtime
 * aggregate layout was needed.  Its constructor marker is authoritative only after the shared slot,
 * Xi class id, Xglobal declaration, result nominal key agree.  Structs cannot declare constructors,
 * so the lowering marker identifies syntax rather than a dispatchable method. */
static const XiClassData *resolved_empty_struct_literal(const XrXiBuildContext *context,
                                                        const XiFunc *caller, const XiValue *call) {
    if (!context || !caller || !call || call->op != XI_CALL_METHOD || call->nargs != 1u ||
        !call->args || !xi_value_is_constructor_call(call) || call->aux_int < 0 ||
        (call->aux_int & 1) != 0)
        return NULL;
    const XiValue *receiver = logical_value_identity(call->args[0]);
    if (!receiver || receiver->op != XI_GET_SHARED || receiver->aux_int < 0)
        return NULL;
    uint32_t module_index = UINT32_MAX;
    if (!find_collected_xi_function_module(context, caller, &module_index) ||
        module_index >= context->source->module_count)
        return NULL;
    const XiFunc *root = context->source->module_roots[module_index];
    const XiModule *module = root ? root->module : NULL;
    uint32_t slot = (uint32_t) receiver->aux_int;
    if (!module || slot >= module->nslots || !module->slot_classes)
        return NULL;
    const XiClassData *class_data = module->slot_classes[slot];
    if (!class_data || class_data->xg_class_id == XG_NO_ID || class_data->needs_runtime_type ||
        class_data->instance_field_count != 0u)
        return NULL;
    const XrClassInfo *info = nominal_info_for_type(call->type);
    const XgClassSummary *class_row =
        find_xg_class_by_id(context->source->global_evidence, class_data->xg_class_id);
    uint8_t decl_kind = 0u;
    if (!info || !class_row || class_row->decl_kind != XG_DECL_STRUCT ||
        class_row->decl_id != info->xg_decl_id ||
        !nominal_contract(context, call->type, &decl_kind, NULL, NULL) ||
        decl_kind != XG_DECL_STRUCT)
        return NULL;
    return class_data;
}

/* A unit enum member is still represented in Xi as a field load from the
 * module namespace.  Program reconstructs the value only when the caller's
 * exact shared-slot enum descriptor, the runtime symbol table, the detached
 * enum layout, and the Xglobal nominal declaration all agree on one zero-
 * payload ordinal.  Names are diagnostic metadata and never select the case. */
static const XiEnumData *resolved_unit_enum_literal(const XrXiBuildContext *context,
                                                    const XiFunc *caller, const XiValue *load,
                                                    uint32_t *variant_ordinal) {
    if (variant_ordinal)
        *variant_ordinal = UINT32_MAX;
    if (!context || !caller || !load || load->op != XI_LOAD_FIELD || load->nargs != 1u ||
        !load->args || !load->args[0] || load->aux_int < 0 || !load->type ||
        load->type->kind != XR_KIND_ENUM || load->type->is_nullable)
        return NULL;
    const XiValue *receiver = logical_value_identity(load->args[0]);
    if (!receiver || receiver->op != XI_GET_SHARED || receiver->aux_int < 0)
        return NULL;

    uint32_t module_index = UINT32_MAX;
    if (!find_collected_xi_function_module(context, caller, &module_index) ||
        module_index >= context->source->module_count)
        return NULL;
    const XiFunc *root = context->source->module_roots[module_index];
    const XiModule *module = root ? root->module : NULL;
    uint32_t slot = (uint32_t) receiver->aux_int;
    if (!module || slot >= module->nslots || !module->slot_enums)
        return NULL;
    const XiEnumData *schema = module->slot_enums[slot];
    if (!variant_schema_matches_type(schema, load->type) || !schema->runtime_type)
        return NULL;

    XrEnumType *runtime_type = (XrEnumType *) schema->runtime_type;
    const XrEnumLayout *layout = load->type->enum_type.layout;
    if (!runtime_type->layout || runtime_type->member_count != schema->member_count ||
        runtime_type->layout->layout_id != schema->layout_id ||
        runtime_type->layout->variant_count != schema->member_count || !runtime_type->members)
        return NULL;
    int ordinal = xr_enum_type_find_member_index_by_symbol(runtime_type, (int) load->aux_int);
    if (ordinal < 0 || (uint32_t) ordinal >= schema->member_count)
        return NULL;
    const XiEnumMemberData *member = &schema->members[ordinal];
    const XrEnumVariantLayout *type_variant = xr_enum_layout_variant(layout, (uint32_t) ordinal);
    const XrEnumVariantLayout *runtime_variant =
        xr_enum_layout_variant(runtime_type->layout, (uint32_t) ordinal);
    uint8_t decl_kind = 0u;
    if (runtime_type->members[ordinal].symbol != (int) load->aux_int ||
        member->ordinal != (uint32_t) ordinal || member->payload_count != 0 || !type_variant ||
        !runtime_variant || type_variant->tag != (uint32_t) ordinal ||
        runtime_variant->tag != (uint32_t) ordinal || type_variant->payload_count != 0u ||
        runtime_variant->payload_count != 0u ||
        !nominal_contract(context, load->type, &decl_kind, NULL, NULL) || decl_kind != XG_DECL_ENUM)
        return NULL;
    if (variant_ordinal)
        *variant_ordinal = (uint32_t) ordinal;
    return schema;
}

/* Xi still carries the generic pending-error scaffold emitted before resolver
 * evidence
 * classifies a constructor-shaped call.  Exact nominal construction
 * has no typed-error outcome,
 * so its exclusive mechanical error continuation
 * is unreachable even when that continuation
 * contains cleanup for other
 * owners.  Preserve the closed catch/rethrow shell: an aliased or
 * non-terminal
 * error block is not constructor-private evidence. */
static const XiValue *exact_infallible_class_construction_in_block(const XrXiBuildContext *context,
                                                                   const XiFunc *function,
                                                                   const XiBlock *block) {
    if (!context || !function || !block || block->kind != XI_BLOCK_IF || !block->control ||
        block->control->op != XI_ERR_CHECK || block->control->nargs != 0u || !block->succs[0] ||
        !block->succs[1] || block->succs[0] == block->succs[1])
        return NULL;

    const XiValue *call = xi_err_check_producer(function, block->control);
    if (!call || !resolved_canonical_class_construction(context, function, call, NULL, NULL))
        return NULL;

    const XiBlock *error = block->succs[0];
    if (error->kind != XI_BLOCK_RETURN || error->phis || error->npreds != 1u || !error->preds ||
        error->preds[0] != block || error->succs[0] || error->succs[1] || !error->control ||
        error->control->op != XI_ERR_RETURN || error->control->nargs != 1u ||
        !error->control->args || !error->control->args[0])
        return NULL;

    const XiValue *caught = NULL;
    bool saw_control = false;
    for (uint32_t index = 0u; index < error->nvalues; ++index) {
        const XiValue *value = error->values[index];
        if (!value || value->block != error)
            return NULL;
        if (value == error->control)
            saw_control = true;
        if (value->op != XI_ERR_CATCH)
            continue;
        if (caught || value->nargs != 0u)
            return NULL;
        caught = value;
    }
    return caught && saw_control && logical_value_identity(error->control->args[0]) == caught
               ? call
               : NULL;
}

static bool block_is_elided_class_construction_error_continuation(const XrXiBuildContext *context,
                                                                  const XiFunc *function,
                                                                  const XiBlock *block) {
    if (!context || !function || !block)
        return false;
    for (uint32_t index = 0u; index < function->nblocks; ++index) {
        const XiBlock *predecessor = function->blocks[index];
        if (predecessor && canonical_block_is_reachable(context, function, predecessor) &&
            predecessor->succs[0] == block &&
            exact_infallible_class_construction_in_block(context, function, predecessor))
            return true;
    }
    return false;
}

static const XiFunc *resolved_callable_target(const XiFunc *caller, const XiValue *value) {
    if (!caller || !value)
        return NULL;
    if (value->op == XI_CLOSURE_NEW && value->aux)
        return (const XiFunc *) value->aux;
    return NULL;
}

static const XgInterfaceImplSummary *
find_conformance_by_id(const XgGlobalEvidence *evidence, XgInterfaceConformanceId conformance_id) {
    const XgInterfaceImplSummary *found = NULL;
    for (uint32_t index = 0u; evidence && index < evidence->ninterface_impls; ++index) {
        const XgInterfaceImplSummary *candidate = &evidence->interface_impls[index];
        if (candidate->conformance_id != conformance_id)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static const XgInterfaceObjectUseSummary *
find_interface_object_use_by_id(const XgGlobalEvidence *evidence, XgInterfaceObjectUseId use_id) {
    const XgInterfaceObjectUseSummary *found = NULL;
    if (!evidence || use_id == XG_NO_ID)
        return NULL;
    for (uint32_t index = 0u; index < evidence->ninterface_object_uses; ++index) {
        const XgInterfaceObjectUseSummary *candidate = &evidence->interface_object_uses[index];
        if (candidate->use_id != use_id)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static bool nominal_type_contract_is_exact(const XrXiBuildContext *context,
                                           XgDeclId implementor_decl_id, uint64_t nominal_key,
                                           uint8_t implementor_kind,
                                           XrCoreIrTypeOwnership ownership,
                                           XrCoreIrCopyContract copy_contract) {
    const XgGlobalEvidence *evidence = context ? context->source->global_evidence : NULL;
    for (uint32_t index = 0u; evidence && index < evidence->ninterface_impls; ++index) {
        const XgInterfaceImplSummary *row = &evidence->interface_impls[index];
        if (row->implementor_decl_id != implementor_decl_id && row->nominal_key != nominal_key)
            continue;
        XrCoreIrTypeOwnership row_ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
        XrCoreIrCopyContract row_copy = XR_CORE_IR_COPY_TRIVIAL;
        if (row->implementor_decl_id != implementor_decl_id || row->nominal_key != nominal_key ||
            row->implementor_kind != implementor_kind || !row->type_contract_complete ||
            (row->implementor_ownership != XG_NOMINAL_OWNERSHIP_TRIVIAL &&
             row->implementor_ownership != XG_NOMINAL_OWNERSHIP_AFFINE) ||
            row->implementor_copy_contract < XG_NOMINAL_COPY_TRIVIAL ||
            row->implementor_copy_contract > XG_NOMINAL_COPY_FORBIDDEN)
            return false;
        row_ownership = row->implementor_ownership == XG_NOMINAL_OWNERSHIP_AFFINE
                            ? XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
                            : XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
        row_copy = (XrCoreIrCopyContract) (row->implementor_copy_contract - 1u);
        if (row_ownership != ownership || row_copy != copy_contract)
            return false;
    }
    /* A nominal without an interface conformance has no conformance row to
     * cross-check. Every conforming nominal must publish a complete unanimous
     * contract; the derived shape is never a substitute for a missing row. */
    return true;
}

static const XgInterfaceMethodSummary *find_interface_method_by_id(const XgGlobalEvidence *evidence,
                                                                   XgInterfaceMethodId method_id) {
    const XgInterfaceMethodSummary *found = NULL;
    for (uint32_t index = 0u; evidence && index < evidence->ninterface_methods; ++index) {
        const XgInterfaceMethodSummary *candidate = &evidence->interface_methods[index];
        if (candidate->interface_method_id != method_id)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static const XgInterfaceMethodParamSummary *
interface_method_params(const XgGlobalEvidence *evidence, const XgInterfaceMethodSummary *method) {
    if (!evidence || !method)
        return NULL;
    if (method->parameter_count == 0u)
        return method->parameter_start == 0u ? evidence->interface_method_params : NULL;
    uint32_t start = method->parameter_start;
    if (start == 0u || start - 1u > evidence->ninterface_method_params ||
        method->parameter_count > evidence->ninterface_method_params - (start - 1u))
        return NULL;
    const XgInterfaceMethodParamSummary *parameters =
        &evidence->interface_method_params[start - 1u];
    for (uint32_t ordinal = 0u; ordinal < method->parameter_count; ++ordinal) {
        if (parameters[ordinal].interface_method_id != method->interface_method_id ||
            parameters[ordinal].ordinal != ordinal || parameters[ordinal].type_key == 0u ||
            !xr_param_mode_is_valid((XrParamMode) parameters[ordinal].mode))
            return NULL;
    }
    return parameters;
}

/* Xglobal and CoreSpec deliberately use different effect vocabularies. A
 * source-native witness
 * target, for example, carries MAY_CALL_NATIVE and
 * NATIVE in Xglobal, while its canonical
 * Program body contains provider.call
 * with PROVIDER_CALL, TRAP, CALL and PROVIDER_BINDING. The
 * Program contract
 * must therefore be the exact union of the already translated closed witness
 *
 * targets, just like callable target-set contracts below; mapping the source
 * bitset directly
 * both loses provider failure and rejects valid native detail.
 * Xglobal remains the authority for
 * the closed target identities and the
 * source-visible control channels. */
static bool interface_method_core_contract(const XrXiBuildContext *context,
                                           XgInterfaceId interface_id,
                                           XgInterfaceMethodId method_id,
                                           bool require_closed_contract, uint32_t *effect_mask,
                                           uint32_t *capability_mask) {
    const XgGlobalEvidence *evidence =
        context && context->source ? context->source->global_evidence : NULL;
    const XgInterfaceMethodSummary *method =
        evidence ? find_interface_method_by_id(evidence, method_id) : NULL;
    if (!evidence || !method || !effect_mask || !capability_mask || !method->contract_complete ||
        method->owner_interface_id != interface_id)
        return false;

    uint32_t effects = 0u;
    uint32_t capabilities = 0u;
    uint32_t target_count = 0u;
    for (uint32_t index = 0u; index < evidence->ninterface_witnesses; ++index) {
        const XgInterfaceWitnessSummary *witness = &evidence->interface_witnesses[index];
        if (witness->interface_id != interface_id || witness->interface_method_id != method_id)
            continue;
        const XiFunc *target = find_xi_function_by_xg_id(context, witness->implementation_func_id);
        const XrXiFunctionStorage *storage = find_xi_function(context, target, NULL, NULL);
        if (!witness->complete || !target || !storage ||
            (require_closed_contract && !storage->closed_contract_ready))
            return false;
        effects |= storage->closed_effect_mask;
        capabilities |= storage->closed_capability_mask;
        ++target_count;
    }
    if (target_count == 0u)
        return false;

    if (require_closed_contract) {
        bool source_may_error = (method->effect_bits & XG_BODY_MAY_ERROR) != 0u;
        bool source_may_panic = (method->effect_bits & XG_BODY_MAY_PANIC) != 0u;
        bool source_may_suspend = (method->effect_bits & XG_BODY_MAY_SUSPEND) != 0u;
        bool source_target_query = (method->effect_bits & XG_BODY_TARGET_QUERY) != 0u;
        if (source_may_error != ((effects & XR_CORE_EFFECT_ERROR) != 0u) ||
            source_may_panic != ((effects & XR_CORE_EFFECT_PANIC) != 0u) ||
            source_may_suspend != ((effects & XR_CORE_EFFECT_SUSPEND) != 0u) ||
            source_target_query != ((effects & XR_CORE_EFFECT_TARGET_QUERY) != 0u))
            return false;
        if ((effects & XR_CORE_EFFECT_PROVIDER_CALL) != 0u &&
            ((method->effect_bits & XG_BODY_MAY_CALL_NATIVE) == 0u ||
             (method->capability_bits & XG_CAP_NATIVE) == 0u))
            return false;
        if ((capabilities & XR_CORE_CAPABILITY_PROVIDER_BINDING) != 0u &&
            (method->capability_bits & XG_CAP_NATIVE) == 0u)
            return false;
    }

    *effect_mask = effects;
    *capability_mask = capabilities;
    return true;
}

static bool witness_call_effect_contract(const XrXiBuildContext *context, const XiFunc *caller,
                                         const XiValue *call, bool require_closed_contract,
                                         uint32_t *effect_mask, uint32_t *capability_mask) {
    const XgCallsiteSummary *callsite = resolved_witness_callsite(context, caller, call);
    return callsite &&
           interface_method_core_contract(context, callsite->receiver_static_interface_id,
                                          (XgInterfaceMethodId) callsite->method_id,
                                          require_closed_contract, effect_mask, capability_mask);
}

static XiInterfaceUseKind interface_use_for_receiver(XrParamMode mode) {
    switch (mode) {
        case XR_PARAM_READ:
            return XI_INTERFACE_USE_READ;
        case XR_PARAM_REF:
            return XI_INTERFACE_USE_REF;
        case XR_PARAM_MOVE:
            return XI_INTERFACE_USE_MOVE;
        default:
            return XI_INTERFACE_USE_NONE;
    }
}

static bool interface_use_allows_receiver(XiInterfaceUseKind carrier, XiInterfaceUseKind receiver) {
    switch (carrier) {
        case XI_INTERFACE_USE_READ:
            return receiver == XI_INTERFACE_USE_READ;
        case XI_INTERFACE_USE_REF:
            return receiver == XI_INTERFACE_USE_READ || receiver == XI_INTERFACE_USE_REF;
        case XI_INTERFACE_USE_MOVE:
            return receiver == XI_INTERFACE_USE_READ || receiver == XI_INTERFACE_USE_MOVE;
        case XI_INTERFACE_USE_OWNED_STORAGE:
            return receiver == XI_INTERFACE_USE_READ || receiver == XI_INTERFACE_USE_REF ||
                   receiver == XI_INTERFACE_USE_MOVE;
        default:
            return false;
    }
}

static bool mapped_existential_receiver_matches(const XrXiBuildContext *context,
                                                uint16_t carrier_type_id, uint16_t slot_type_id,
                                                XiInterfaceUseKind receiver_use) {
    const XrXiTypeStorage *carrier = find_dynamic_type_by_id(context, carrier_type_id);
    const XrXiTypeStorage *slot = find_dynamic_type_by_id(context, slot_type_id);
    if (!carrier || !slot || carrier->input.kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
        slot->input.kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
        !xr_core_ir_key_equal(carrier->input.existential_interface,
                              slot->input.existential_interface) ||
        slot->input.interface_use_kind != (XrCoreIrInterfaceUseKind) receiver_use)
        return false;
    return interface_use_allows_receiver((XiInterfaceUseKind) carrier->input.interface_use_kind,
                                         receiver_use);
}

static XrCoreIrNominalKind core_nominal_kind_from_xg(uint8_t kind) {
    switch (kind) {
        case XG_DECL_CLASS:
            return XR_CORE_IR_NOMINAL_CLASS;
        case XG_DECL_STRUCT:
            return XR_CORE_IR_NOMINAL_STRUCT;
        case XG_DECL_ENUM:
            return XR_CORE_IR_NOMINAL_ENUM;
        default:
            return XR_CORE_IR_NOMINAL_NONE;
    }
}

static bool xg_type_key_for_xr_type(const XrXiBuildContext *context, const XrType *type,
                                    uint32_t *type_key) {
    if (type_key)
        *type_key = 0u;
    if (!context || !type || !type_key)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
            *type_key = xg_synthetic_width_type_key(XR_TREF_SCALAR, type->scalar_rep);
            break;
        case XR_KIND_BOOL:
            *type_key = xg_synthetic_type_key(XR_TREF_BOOL);
            break;
        case XR_KIND_RUNE:
            *type_key = xg_synthetic_type_key(XR_TREF_RUNE);
            break;
        case XR_KIND_STRING:
            *type_key = xg_synthetic_type_key(XR_TREF_STRING);
            break;
        case XR_KIND_UNIT:
            *type_key = xg_synthetic_type_key(XR_TREF_UNIT);
            break;
        case XR_KIND_NULL:
            *type_key = xg_synthetic_type_key(XR_TREF_NULL);
            break;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
        case XR_KIND_ENUM: {
            const XrClassInfo *info = nominal_info_for_type(type);
            if (!info || !nominal_contract(context, type, NULL, NULL, NULL))
                return false;
            for (uint32_t index = 0u; index < context->source->global_evidence->ndecls; ++index) {
                const XgDeclSummary *declaration = &context->source->global_evidence->decls[index];
                if (declaration->decl_id == info->xg_decl_id) {
                    *type_key = declaration->type_key;
                    break;
                }
            }
            break;
        }
        default:
            return false;
    }
    return *type_key != 0u;
}

static const XrType *function_error_source_type(const XiFunc *function) {
    const XaAnalyzer *analyzer = function ? function->analyzer : NULL;
    const XaEffectSummary *effect =
        analyzer && function->analyzer_effect_id != XA_EFFECT_NONE
            ? xa_effect_db_get(analyzer->effect_db, function->analyzer_effect_id)
            : NULL;
    if (!function || function->error_effect_nothrow || !effect ||
        effect->error_set_completeness != XA_EFFECT_COMPLETE ||
        effect->error_unknown_reasons != XA_UNKNOWN_NONE || effect->escaping.count != 1u)
        return NULL;
    return xa_effect_db_error_type_handle(analyzer->effect_db, effect->escaping.types[0].type_id);
}

static bool interface_result_ownership_is_exact(const XiFunc *target,
                                                const XgReturnOwnership *ownership) {
    return target && ownership && ownership->complete && target->arc_return_ownership.complete &&
           ownership->kind == target->arc_return_ownership.kind &&
           ownership->param_index == target->arc_return_ownership.param_index;
}

static XrProgramBuildStatus build_interface_slot_signature(
    XrXiBuildContext *context, const XgInterfaceImplSummary *implementor,
    const XgInterfaceWitnessSummary *witness, const XgInterfaceMethodSummary *method,
    XrCoreIrCallableSignatureInput *signature, uint16_t **parameter_types_out,
    XrParamMode **parameter_modes_out, char *diagnostic, size_t diagnostic_size) {
    if (!context || !implementor || !witness || !method || !signature || !parameter_types_out ||
        !parameter_modes_out)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "interface slot contract is incomplete");
    const XgGlobalEvidence *evidence = context->source->global_evidence;
    const XiFunc *target = find_xi_function_by_xg_id(context, witness->implementation_func_id);
    const XrXiFunctionStorage *target_storage = find_xi_function(context, target, NULL, NULL);
    const XgInterfaceMethodParamSummary *method_parameters =
        interface_method_params(evidence, method);
    if (!target || !target_storage || !target->has_receiver || !method->contract_complete ||
        !method->has_receiver || method->owner_interface_id != implementor->interface_id ||
        witness->interface_id != implementor->interface_id ||
        witness->conformance_id != implementor->conformance_id ||
        witness->interface_method_id != method->interface_method_id ||
        witness->signature_key != method->signature_key || !witness->complete ||
        witness->receiver_mode != method->receiver_mode ||
        target->receiver_mode != (XrParamMode) method->receiver_mode ||
        target->nparams != method->parameter_count + 1u ||
        (method->parameter_count != 0u && !method_parameters) || method->result_type_key == 0u ||
        !method->result_ownership.complete ||
        method->result_ownership.kind == XG_RETURN_OWNERSHIP_UNKNOWN || !target->params ||
        !target->params[0] || !target_storage->closed_contract_ready)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "interface slot %u lacks one complete Xglobal/Xi contract", witness->slot);

    uint32_t parameter_count = target->nparams;
    const char *contract_mismatch = "unspecified contract mismatch";
    uint16_t *parameter_types = xr_calloc(parameter_count, sizeof(*parameter_types));
    XrParamMode *parameter_modes = xr_calloc(parameter_count, sizeof(*parameter_modes));
    if (!parameter_types || !parameter_modes) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    XiInterfaceUseKind receiver_use =
        interface_use_for_receiver((XrParamMode) method->receiver_mode);
    if (receiver_use == XI_INTERFACE_USE_NONE ||
        !map_existential_interface_id(context, implementor->interface_id, receiver_use,
                                      &parameter_types[0])) {
        contract_mismatch = "receiver mode has no exact existential interface type";
        goto invalid;
    }
    parameter_modes[0] = (XrParamMode) method->receiver_mode;
    for (uint32_t parameter = 1u; parameter < parameter_count; ++parameter) {
        const XiValue *source = target->params[parameter];
        const XgInterfaceMethodParamSummary *contract = &method_parameters[parameter - 1u];
        uint32_t source_type_key = 0u;
        if (!source || source->op != XI_PARAM || source->aux_int != (int64_t) parameter) {
            contract_mismatch = "Xi parameter identity disagrees with its interface ordinal";
            goto invalid;
        }
        if (source->param_mode != contract->mode) {
            contract_mismatch = "Xi parameter mode disagrees with Xglobal";
            goto invalid;
        }
        if (!xg_type_key_for_xr_type(context, source->type, &source_type_key) ||
            source_type_key != contract->type_key) {
            contract_mismatch = "Xi parameter type key disagrees with Xglobal";
            goto invalid;
        }
        if (!map_type_for_mode(context, source->type, (XrParamMode) source->param_mode,
                               &parameter_types[parameter], NULL, 0u)) {
            contract_mismatch = "Xi parameter type has no canonical Core type";
            goto invalid;
        }
        parameter_modes[parameter] = (XrParamMode) source->param_mode;
    }

    uint16_t result_type = XR_CORE_TYPE_VOID;
    uint16_t error_type = XR_CORE_TYPE_VOID;
    uint16_t panic_type = XR_CORE_TYPE_VOID;
    uint32_t effect_mask = 0u;
    uint32_t capability_mask = 0u;
    uint32_t result_type_key = 0u;
    uint32_t error_type_key = 0u;
    const XrType *error_source_type = function_error_source_type(target);
    if (!map_type(context, target->return_type, &result_type)) {
        contract_mismatch = "Xi result has no canonical Core type";
        goto invalid;
    }
    if (!xg_type_key_for_xr_type(context, target->return_type, &result_type_key) ||
        result_type_key != method->result_type_key) {
        contract_mismatch = "Xi result type key disagrees with Xglobal";
        goto invalid;
    }
    if (map_function_error_type(context, target, &error_type, diagnostic, diagnostic_size) !=
        XR_PROGRAM_BUILD_OK) {
        contract_mismatch = "Xi result error type has no canonical Core type";
        goto invalid;
    }
    if (map_function_panic_type(context, target, &panic_type, diagnostic, diagnostic_size) !=
        XR_PROGRAM_BUILD_OK) {
        contract_mismatch = "Xi result panic type has no canonical Core type";
        goto invalid;
    }
    if (error_type != XR_CORE_TYPE_VOID &&
        (!error_source_type ||
         !xg_type_key_for_xr_type(context, error_source_type, &error_type_key))) {
        contract_mismatch = "Xi error type lacks an exact Xglobal type key";
        goto invalid;
    }
    if (method->error_type_key != error_type_key) {
        contract_mismatch = "Xi error type key disagrees with Xglobal";
        goto invalid;
    }
    if (method->panic_type_key != 0u || panic_type != XR_CORE_TYPE_VOID) {
        contract_mismatch = "panic channel is not the required canonical void contract";
        goto invalid;
    }
    if (!interface_result_ownership_is_exact(target, &method->result_ownership)) {
        contract_mismatch = "Xi result ownership disagrees with Xglobal";
        goto invalid;
    }
    if (!interface_method_core_contract(context, implementor->interface_id,
                                        method->interface_method_id, true, &effect_mask,
                                        &capability_mask)) {
        contract_mismatch = "closed witness targets have no exact Program effect contract";
        goto invalid;
    }
    *signature = (XrCoreIrCallableSignatureInput) {
        .parameter_types = parameter_types,
        .parameter_modes = parameter_modes,
        .parameter_count = parameter_count,
        .has_receiver = true,
        .receiver_mode = (XrParamMode) method->receiver_mode,
        .result_type_id = result_type,
        .result_ownership = logical_ownership_for_type(context, result_type),
        .result_borrow_origins = target->view_origin_set,
        .result_borrow_origin_count = target->view_origin_count,
        .error_type_id = error_type,
        .panic_type_id = panic_type,
        .effect_mask = effect_mask,
        .capability_mask = capability_mask,
    };
    *parameter_types_out = parameter_types;
    *parameter_modes_out = parameter_modes;
    return XR_PROGRAM_BUILD_OK;

invalid:
    xr_free(parameter_types);
    xr_free(parameter_modes);
    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "interface %u method %u slot %u disagrees with its Xglobal contract: %s "
                "(effect_bits=0x%08x, capability_bits=0x%08x)",
                method->owner_interface_id, method->interface_method_id, witness->slot,
                contract_mismatch, method->effect_bits, method->capability_bits);
}

static bool interface_signatures_equal(const XrCoreIrCallableSignatureInput *left,
                                       const XrCoreIrCallableSignatureInput *right) {
    if (!left || !right || left->parameter_count != right->parameter_count ||
        left->has_receiver != right->has_receiver || left->receiver_mode != right->receiver_mode ||
        left->result_type_id != right->result_type_id ||
        left->result_ownership != right->result_ownership ||
        left->result_borrow_origin_count != right->result_borrow_origin_count ||
        left->error_type_id != right->error_type_id ||
        left->panic_type_id != right->panic_type_id || left->effect_mask != right->effect_mask ||
        left->capability_mask != right->capability_mask)
        return false;
    for (uint32_t index = 0u; index < left->parameter_count; ++index)
        if (left->parameter_types[index] != right->parameter_types[index] ||
            left->parameter_modes[index] != right->parameter_modes[index])
            return false;
    for (uint32_t index = 0u; index < left->result_borrow_origin_count; ++index)
        if (memcmp(&left->result_borrow_origins[index], &right->result_borrow_origins[index],
                   sizeof(left->result_borrow_origins[index])) != 0)
            return false;
    return true;
}

static int32_t interface_storage_index(const XrXiBuildContext *context,
                                       XgInterfaceId interface_id) {
    for (uint32_t index = 0u; context && index < context->interface_count; ++index)
        if (context->interface_storage[index].interface_id == interface_id)
            return (int32_t) index;
    return -1;
}

static const XrCoreIrCallableSignatureInput *
interface_slot_contract(const XrXiBuildContext *context, XgInterfaceId interface_id,
                        uint32_t slot) {
    int32_t index = interface_storage_index(context, interface_id);
    if (index < 0 || slot >= context->interfaces[index].slot_count)
        return NULL;
    return &context->interfaces[index].slots[slot];
}

static const XrCoreIrCallableSignatureInput *
interface_slot_contract_by_key(const XrXiBuildContext *context, XrCoreIrKey interface_key_value,
                               uint32_t slot) {
    for (uint32_t index = 0u; context && index < context->interface_count; ++index) {
        const XrCoreIrInterfaceInput *candidate = &context->interfaces[index];
        if (xr_core_ir_key_equal(candidate->key, interface_key_value))
            return slot < candidate->slot_count ? &candidate->slots[slot] : NULL;
    }
    return NULL;
}

static XrProgramBuildStatus ensure_interface_contract(XrXiBuildContext *context,
                                                      const XgInterfaceImplSummary *implementor,
                                                      char *diagnostic, size_t diagnostic_size) {
    const XgGlobalEvidence *evidence = context->source->global_evidence;
    int32_t existing = interface_storage_index(context, implementor->interface_id);
    if (implementor->witness_count == 0u || implementor->witness_count > XR_PROGRAM_LIMIT_FUNCTIONS)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "interface %u has no bounded witness contract", implementor->interface_id);

    XrCoreIrCallableSignatureInput *slots = NULL;
    uint16_t **parameter_types = NULL;
    XrParamMode **parameter_modes = NULL;
    if (existing < 0) {
        if (context->interface_count >= context->interface_capacity)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        slots = xr_calloc(implementor->witness_count, sizeof(*slots));
        parameter_types = xr_calloc(implementor->witness_count, sizeof(*parameter_types));
        parameter_modes = xr_calloc(implementor->witness_count, sizeof(*parameter_modes));
        if (!slots || !parameter_types || !parameter_modes) {
            xr_free(slots);
            xr_free(parameter_types);
            xr_free(parameter_modes);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
    } else if (context->interfaces[existing].slot_count != implementor->witness_count) {
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "interface %u witness slot count is inconsistent", implementor->interface_id);
    }

    for (uint32_t slot = 0u; slot < implementor->witness_count; ++slot) {
        const XgInterfaceWitnessSummary *witness =
            xg_global_evidence_find_interface_witness(evidence, implementor->conformance_id, slot);
        const XgInterfaceMethodSummary *method =
            witness ? find_interface_method_by_id(evidence, witness->interface_method_id) : NULL;
        XrCoreIrCallableSignatureInput candidate = {0};
        uint16_t *candidate_types = NULL;
        XrParamMode *candidate_modes = NULL;
        XrProgramBuildStatus status = build_interface_slot_signature(
            context, implementor, witness, method, &candidate, &candidate_types, &candidate_modes,
            diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK) {
            for (uint32_t prior = 0u; existing < 0 && prior < slot; ++prior) {
                xr_free(parameter_types[prior]);
                xr_free(parameter_modes[prior]);
            }
            if (existing < 0) {
                xr_free(slots);
                xr_free(parameter_types);
                xr_free(parameter_modes);
            }
            return status;
        }
        if (existing >= 0) {
            bool equal =
                interface_signatures_equal(&context->interfaces[existing].slots[slot], &candidate);
            xr_free(candidate_types);
            xr_free(candidate_modes);
            if (!equal)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "interface %u slot %u changes across conformances",
                            implementor->interface_id, slot);
        } else {
            slots[slot] = candidate;
            parameter_types[slot] = candidate_types;
            parameter_modes[slot] = candidate_modes;
        }
    }
    if (existing >= 0)
        return XR_PROGRAM_BUILD_OK;

    uint32_t index = context->interface_count++;
    context->interfaces[index] = (XrCoreIrInterfaceInput) {
        .key = interface_key(implementor->interface_id),
        .slots = slots,
        .slot_count = implementor->witness_count,
    };
    context->interface_storage[index] = (XrXiInterfaceStorage) {
        .interface_id = implementor->interface_id,
        .slots = slots,
        .parameter_types = parameter_types,
        .parameter_modes = parameter_modes,
    };
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus ensure_conformance_contract(XrXiBuildContext *context,
                                                        XgInterfaceConformanceId conformance_id,
                                                        char *diagnostic, size_t diagnostic_size) {
    for (uint32_t index = 0u; index < context->conformance_count; ++index)
        if (context->conformance_storage[index].conformance_id == conformance_id)
            return XR_PROGRAM_BUILD_OK;
    const XgGlobalEvidence *evidence = context->source->global_evidence;
    const XgInterfaceImplSummary *implementor = find_conformance_by_id(evidence, conformance_id);
    if (!implementor || !implementor->verdict_complete || !implementor->constraint_eligible ||
        !implementor->existential_eligible || implementor->implementor_decl_id == XG_NO_ID ||
        implementor->nominal_key == 0u || implementor->interface_id == XG_NO_ID ||
        !implementor->type_contract_complete ||
        (implementor->implementor_ownership != XG_NOMINAL_OWNERSHIP_TRIVIAL &&
         implementor->implementor_ownership != XG_NOMINAL_OWNERSHIP_AFFINE) ||
        implementor->implementor_copy_contract < XG_NOMINAL_COPY_TRIVIAL ||
        implementor->implementor_copy_contract > XG_NOMINAL_COPY_FORBIDDEN ||
        context->conformance_count >= context->conformance_capacity)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "conformance %u is not complete existential evidence", conformance_id);
    const XgInterfaceWitnessSummary *first =
        xg_global_evidence_find_interface_witness(evidence, implementor->conformance_id, 0u);
    const XiFunc *first_target =
        first ? find_xi_function_by_xg_id(context, first->implementation_func_id) : NULL;
    const XrType *implementor_type = first_target && first_target->params &&
                                             first_target->nparams != 0u && first_target->params[0]
                                         ? first_target->params[0]->type
                                         : NULL;
    const XrClassInfo *nominal_info = nominal_info_for_type(implementor_type);
    uint8_t decl_kind = 0u;
    XrCoreIrNominalKind nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    uint64_t nominal_key = 0u;
    uint16_t implementor_type_id = XR_CORE_TYPE_VOID;
    if (!first || !first_target || !implementor_type)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "conformance %u has no unique Xi witness receiver", conformance_id);
    if (!nominal_info)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "conformance %u Xi witness receiver has no nominal identity (kind=%u)",
                    conformance_id, (unsigned) implementor_type->kind);
    if (!nominal_contract(context, implementor_type, &decl_kind, &nominal_kind, &nominal_key))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "conformance %u Xi witness receiver nominal contract is not exact "
                    "(decl=%u key=%llu kind=%u)",
                    conformance_id, nominal_info->xg_decl_id,
                    (unsigned long long) nominal_info->xg_nominal_key,
                    (unsigned) nominal_info->nominal_kind);
    if (nominal_info->xg_decl_id != implementor->implementor_decl_id)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "conformance %u Xi/Xglobal implementor declaration disagrees (%u != %u)",
                    conformance_id, nominal_info->xg_decl_id, implementor->implementor_decl_id);
    if (decl_kind != implementor->implementor_kind)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "conformance %u Xi/Xglobal implementor kind disagrees (%u != %u)",
                    conformance_id, (unsigned) decl_kind, (unsigned) implementor->implementor_kind);
    if (nominal_key != implementor->nominal_key)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "conformance %u Xi/Xglobal implementor nominal key disagrees", conformance_id);
    if (!map_type(context, implementor_type, &implementor_type_id) ||
        implementor_type_id == XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "conformance %u Xi witness receiver type has no exact Core mapping",
                    conformance_id);
    const XrXiTypeStorage *mapped_implementor =
        find_dynamic_type_by_id(context, implementor_type_id);
    XrCoreIrTypeOwnership expected_ownership =
        implementor->implementor_ownership == XG_NOMINAL_OWNERSHIP_AFFINE
            ? XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
            : XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
    XrCoreIrCopyContract expected_copy =
        (XrCoreIrCopyContract) (implementor->implementor_copy_contract - 1u);
    if (!mapped_implementor || mapped_implementor->input.ownership != expected_ownership ||
        mapped_implementor->input.copy_contract != expected_copy)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "conformance %u implementor ownership contract is not exact", conformance_id);
    XrProgramBuildStatus status =
        ensure_interface_contract(context, implementor, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;

    XrCoreIrKey *slot_functions = xr_calloc(implementor->witness_count, sizeof(*slot_functions));
    if (!slot_functions)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    for (uint32_t slot = 0u; slot < implementor->witness_count; ++slot) {
        const XgInterfaceWitnessSummary *witness =
            xg_global_evidence_find_interface_witness(evidence, implementor->conformance_id, slot);
        const XiFunc *target =
            witness ? find_xi_function_by_xg_id(context, witness->implementation_func_id) : NULL;
        const XrXiFunctionStorage *target_storage = find_xi_function(context, target, NULL, NULL);
        if (!witness || witness->slot != slot || !target_storage) {
            xr_free(slot_functions);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                        "conformance %u slot %u has no canonical target", conformance_id, slot);
        }
        slot_functions[slot] = target_storage->key;
    }
    uint32_t index = context->conformance_count++;
    context->conformances[index] = (XrCoreIrConformanceInput) {
        .key = conformance_key(conformance_id),
        .implementor_type_id = implementor_type_id,
        .implementor_kind = nominal_kind,
        .interface_key = interface_key(implementor->interface_id),
        .slot_functions = slot_functions,
        .slot_count = implementor->witness_count,
    };
    context->conformance_storage[index] = (XrXiConformanceStorage) {
        .conformance_id = conformance_id,
        .slot_functions = slot_functions,
    };
    return XR_PROGRAM_BUILD_OK;
}

static bool existential_value_contract_is_exact(const XrXiBuildContext *context,
                                                const XiFunc *function,
                                                const XgInterfaceImplSummary *implementor,
                                                const XiValue *value) {
    const XgInterfaceObjectUseSummary *object_use =
        context && context->source && value
            ? find_interface_object_use_by_id(context->source->global_evidence,
                                              value->xg_interface_object_use_id)
            : NULL;
    XgInterfaceUseKind expected_use = XG_INTERFACE_USE_INVALID;
    if (value) {
        switch ((XiInterfaceUseKind) value->xg_interface_use_kind) {
            case XI_INTERFACE_USE_READ:
                expected_use = XG_INTERFACE_USE_READ;
                break;
            case XI_INTERFACE_USE_REF:
                expected_use = XG_INTERFACE_USE_REF;
                break;
            case XI_INTERFACE_USE_MOVE:
                expected_use = XG_INTERFACE_USE_MOVE;
                break;
            case XI_INTERFACE_USE_OWNED_STORAGE:
                expected_use = XG_INTERFACE_USE_OWNED_STORAGE;
                break;
            case XI_INTERFACE_USE_NONE:
                break;
        }
    }
    const XgInterfaceObjectUseSummary *authoritative_use =
        object_use ? xg_global_evidence_find_interface_object_use(
                         context->source->global_evidence, object_use->owner_func_id,
                         object_use->source_node_id, object_use->interface_id, object_use->reason)
                   : NULL;
    return implementor && function && value && object_use && authoritative_use == object_use &&
           object_use->source_node_id != 0u && object_use->reason != 0u &&
           function->xg_body_func_id != XG_NO_ID &&
           object_use->owner_func_id == function->xg_body_func_id &&
           object_use->interface_id == value->xg_interface_id &&
           object_use->use_kind == expected_use &&
           implementor->conformance_id == value->xg_conformance_id &&
           implementor->interface_id == value->xg_interface_id &&
           implementor->implementor_decl_id == value->xg_implementor_decl_id &&
           implementor->nominal_key == value->xg_nominal_key &&
           implementor->implementor_kind == value->xg_implementor_kind &&
           implementor->implementor_ownership == value->xg_implementor_ownership &&
           implementor->implementor_copy_contract == value->xg_implementor_copy_contract &&
           implementor->type_contract_complete && value->xg_type_contract_complete == 1u &&
           implementor->verdict_complete && implementor->existential_eligible;
}

static bool existential_reborrow_contract_is_exact(const XrXiBuildContext *context,
                                                   const XiFunc *function, const XiValue *value) {
    const XiValue *source = value && value->nargs == 1u && value->args ? value->args[0] : NULL;
    const XrClassInfo *source_interface =
        source && source->type && source->type->kind == XR_KIND_INTERFACE
            ? source->type->instance.class_ref
            : NULL;
    const XrClassInfo *result_interface =
        value && value->type && value->type->kind == XR_KIND_INTERFACE
            ? value->type->instance.class_ref
            : NULL;
    const XgInterfaceObjectUseSummary *object_use =
        context && context->source && value
            ? find_interface_object_use_by_id(context->source->global_evidence,
                                              value->xg_interface_object_use_id)
            : NULL;
    const XgInterfaceObjectUseSummary *authoritative_use =
        context && context->source && function && object_use && result_interface
            ? xg_global_evidence_find_interface_object_use(
                  context->source->global_evidence, (XgFuncId) function->xg_body_func_id,
                  object_use->source_node_id, (XgInterfaceId) result_interface->xg_interface_id,
                  XG_INTERFACE_OBJECT_USE_ARGUMENT | XG_INTERFACE_OBJECT_USE_VALUE)
            : NULL;
    bool readable_source =
        source && (source->xg_interface_use_kind == XI_INTERFACE_USE_MOVE ||
                   source->xg_interface_use_kind == XI_INTERFACE_USE_OWNED_STORAGE);
    return value && source && function && source_interface && result_interface && object_use &&
           authoritative_use == object_use && value->op == XI_COPY &&
           value->xg_existential_kind == XI_EXISTENTIAL_REBORROW_READ && readable_source &&
           function->xg_body_func_id != XG_NO_ID &&
           object_use->owner_func_id == function->xg_body_func_id &&
           object_use->source_node_id != 0u && object_use->use_kind == XG_INTERFACE_USE_READ &&
           source_interface->xg_interface_id != XG_NO_ID &&
           source_interface->xg_interface_id == result_interface->xg_interface_id &&
           source->xg_interface_id == source_interface->xg_interface_id &&
           value->xg_interface_id == result_interface->xg_interface_id &&
           object_use->interface_id == value->xg_interface_id &&
           value->xg_interface_use_kind == XI_INTERFACE_USE_READ &&
           value->xg_conformance_id == XG_NO_ID && value->xg_implementor_decl_id == XG_NO_ID &&
           value->xg_nominal_key == 0u && value->xg_implementor_kind == 0u &&
           value->xg_implementor_ownership == XG_NOMINAL_OWNERSHIP_INVALID &&
           value->xg_implementor_copy_contract == XG_NOMINAL_COPY_INVALID &&
           value->xg_type_contract_complete == 0u;
}

static bool interface_parameter_contract_is_exact(const XrXiBuildContext *context,
                                                  const XiFunc *function, const XiValue *parameter,
                                                  XrParamMode mode) {
    const XrClassInfo *interface_info =
        parameter && parameter->type && parameter->type->kind == XR_KIND_INTERFACE
            ? parameter->type->instance.class_ref
            : NULL;
    const XgInterfaceObjectUseSummary *object_use =
        context && context->source && parameter
            ? find_interface_object_use_by_id(context->source->global_evidence,
                                              parameter->xg_interface_object_use_id)
            : NULL;
    XgInterfaceUseKind expected = mode == XR_PARAM_READ   ? XG_INTERFACE_USE_READ
                                  : mode == XR_PARAM_REF  ? XG_INTERFACE_USE_REF
                                  : mode == XR_PARAM_MOVE ? XG_INTERFACE_USE_MOVE
                                                          : XG_INTERFACE_USE_INVALID;
    XiInterfaceUseKind expected_xi = mode == XR_PARAM_READ   ? XI_INTERFACE_USE_READ
                                     : mode == XR_PARAM_REF  ? XI_INTERFACE_USE_REF
                                     : mode == XR_PARAM_MOVE ? XI_INTERFACE_USE_MOVE
                                                             : XI_INTERFACE_USE_NONE;
    uint32_t source_node_id =
        function && function->xg_body_func_id != XG_NO_ID && parameter && parameter->aux_int >= 0 &&
                (uint64_t) parameter->aux_int <= UINT32_MAX
            ? xg_interface_parameter_site_id((XgFuncId) function->xg_body_func_id,
                                             (uint32_t) parameter->aux_int)
            : 0u;
    const XgInterfaceObjectUseSummary *authoritative_use =
        context && context->source && interface_info && object_use && source_node_id != 0u
            ? xg_global_evidence_find_interface_object_use(
                  context->source->global_evidence, (XgFuncId) function->xg_body_func_id,
                  source_node_id, interface_info->xg_interface_id, XG_INTERFACE_OBJECT_USE_PARAM)
            : NULL;
    return function && function->xg_body_func_id != XG_NO_ID && interface_info &&
           interface_info->xg_interface_id != XG_NO_ID && object_use &&
           authoritative_use == object_use && object_use->source_node_id == source_node_id &&
           object_use->owner_func_id == function->xg_body_func_id &&
           object_use->interface_id == interface_info->xg_interface_id &&
           (object_use->reason & XG_INTERFACE_OBJECT_USE_PARAM) != 0u &&
           object_use->use_kind == expected &&
           parameter->xg_interface_id == object_use->interface_id &&
           parameter->xg_interface_use_kind == expected_xi;
}

static XrProgramBuildStatus ensure_interface_conformances(XrXiBuildContext *context,
                                                          XgInterfaceId interface_id,
                                                          char *diagnostic,
                                                          size_t diagnostic_size) {
    const XgGlobalEvidence *evidence = context ? context->source->global_evidence : NULL;
    bool found = false;
    for (uint32_t index = 0u; evidence && index < evidence->ninterface_impls; ++index) {
        const XgInterfaceImplSummary *implementor = &evidence->interface_impls[index];
        if (implementor->interface_id != interface_id || !implementor->existential_eligible)
            continue;
        XrProgramBuildStatus status = ensure_conformance_contract(
            context, implementor->conformance_id, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
        found = true;
    }
    if (!found)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "interface %u has no complete existential conformance", interface_id);
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
prepare_existential_contracts(XrXiBuildContext *context, char *diagnostic, size_t diagnostic_size) {
    const XgGlobalEvidence *evidence = context->source->global_evidence;
    if (!evidence || evidence->ninterface_impls == 0u)
        return XR_PROGRAM_BUILD_OK;
    context->interface_capacity = evidence->ninterface_impls;
    context->conformance_capacity = evidence->ninterface_impls;
    context->interfaces = xr_calloc(context->interface_capacity, sizeof(*context->interfaces));
    context->interface_storage =
        xr_calloc(context->interface_capacity, sizeof(*context->interface_storage));
    context->conformances =
        xr_calloc(context->conformance_capacity, sizeof(*context->conformances));
    context->conformance_storage =
        xr_calloc(context->conformance_capacity, sizeof(*context->conformance_storage));
    if (!context->interfaces || !context->interface_storage || !context->conformances ||
        !context->conformance_storage)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;

    for (uint32_t module = 0u; module < context->source->module_count; ++module) {
        const XrXiModuleStorage *storage = &context->storage[module];
        for (uint32_t function = 0u; function < storage->function_count; ++function) {
            const XiFunc *xi = storage->xi_functions[function];
            for (uint32_t block = 0u; xi && block < xi->nblocks; ++block) {
                const XiBlock *row = xi->blocks[block];
                if (!canonical_block_is_reachable(context, xi, row))
                    continue;
                for (uint32_t index = 0u; row && index < row->nvalues; ++index) {
                    const XiValue *value = row->values[index];
                    if (!value || value->xg_existential_kind == XI_EXISTENTIAL_NONE)
                        continue;
                    if (value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_DIRECT ||
                        value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE) {
                        if (!resolved_witness_callsite(context, xi, value))
                            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                        "Xi witness call v%u has inconsistent stable evidence",
                                        value->id);
                        XrProgramBuildStatus status = ensure_interface_conformances(
                            context, value->xg_interface_id, diagnostic, diagnostic_size);
                        if (status != XR_PROGRAM_BUILD_OK)
                            return status;
                        continue;
                    }
                    if (value->xg_existential_kind == XI_EXISTENTIAL_REBORROW_READ) {
                        XrProgramXiSemanticProjection projection;
                        if (!xr_program_xi_semantic_projection(
                                value->op, value->xg_existential_kind, &projection) ||
                            projection.kind != XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_REBORROW_READ ||
                            !existential_reborrow_contract_is_exact(context, xi, value))
                            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                        "Xi existential READ reborrow v%u has inconsistent stable "
                                        "evidence",
                                        value->id);
                        XrProgramBuildStatus status = ensure_interface_conformances(
                            context, value->xg_interface_id, diagnostic, diagnostic_size);
                        if (status != XR_PROGRAM_BUILD_OK)
                            return status;
                        continue;
                    }
                    if (value->xg_existential_kind != XI_EXISTENTIAL_PACK &&
                        value->xg_existential_kind != XI_EXISTENTIAL_TEST &&
                        value->xg_existential_kind != XI_EXISTENTIAL_PROJECT)
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi existential v%u has an unknown semantic kind", value->id);
                    XrProgramXiSemanticProjection projection;
                    const XgInterfaceImplSummary *implementor =
                        find_conformance_by_id(evidence, value->xg_conformance_id);
                    if (!xr_program_xi_semantic_projection(value->op, value->xg_existential_kind,
                                                           &projection) ||
                        !existential_value_contract_is_exact(context, xi, implementor, value) ||
                        value->xg_interface_use_kind < XI_INTERFACE_USE_READ ||
                        value->xg_interface_use_kind > XI_INTERFACE_USE_OWNED_STORAGE)
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi existential v%u has inconsistent stable evidence",
                                    value->id);
                    XrProgramBuildStatus status = ensure_conformance_contract(
                        context, value->xg_conformance_id, diagnostic, diagnostic_size);
                    if (status != XR_PROGRAM_BUILD_OK)
                        return status;
                }
            }
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static bool resolved_callable_call_targets(const XrXiBuildContext *context, const XiFunc *caller,
                                           const XiValue *call, XrXiCallableTargetSet *target_set);

static const XiValue *block_typed_invoke_call(const XrXiBuildContext *context,
                                              const XiFunc *function, const XiBlock *block) {
    if (!function || !block || block->kind != XI_BLOCK_IF || !block->control ||
        block->control->op != XI_ERR_CHECK || !block->succs[0] || !block->succs[1])
        return NULL;
    const XiValue *producer = xi_err_check_producer(function, block->control);
    if (!producer || !canonical_block_is_reachable(context, function, producer->block))
        return NULL;
    if (producer->op == XI_CALL)
        return resolved_sealed_callee(context, function, producer) ||
                       resolved_callable_call_targets(context, function, producer, NULL)
                   ? producer
                   : NULL;
    if ((producer->op == XI_CALL_METHOD || producer->op == XI_CALL_METHOD_DIRECT) &&
        resolved_sealed_callee(context, function, producer))
        return producer;
    if ((producer->op == XI_CALL_METHOD || producer->op == XI_CALL_METHOD_DIRECT) &&
        producer->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE)
        return resolved_witness_callsite(context, function, producer) ? producer : NULL;
    return NULL;
}

static const XiBlock *typed_invoke_check_block_for_call(const XrXiBuildContext *context,
                                                        const XiFunc *function,
                                                        const XiValue *call) {
    if (!context || !function || !call)
        return NULL;
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (canonical_block_is_reachable(context, function, block) &&
            block_typed_invoke_call(context, function, block) == call)
            return block;
    }
    return NULL;
}

static const XiValue *block_error_catch(const XiBlock *block) {
    const XiValue *found = NULL;
    for (uint32_t index = 0; block && index < block->nvalues; ++index) {
        const XiValue *value = block->values[index];
        if (!value || value->op != XI_ERR_CATCH)
            continue;
        if (found)
            return NULL;
        found = value;
    }
    return found;
}

static bool value_is_invoke_scaffold(const XrXiBuildContext *context, const XiFunc *function,
                                     const XiValue *value) {
    if (!value)
        return false;
    if (value->op == XI_ERR_RETURN)
        return true;
    if (!function || !value->block)
        return false;
    if ((value->op == XI_CALL || value->op == XI_CALL_METHOD ||
         value->op == XI_CALL_METHOD_DIRECT) &&
        typed_invoke_check_block_for_call(context, function, value))
        return true;
    const XiValue *call = block_typed_invoke_call(context, function, value->block);
    if (value == call || value == value->block->control)
        return call != NULL ||
               (value == value->block->control &&
                exact_infallible_class_construction_in_block(context, function, value->block));
    if (value->op == XI_ERR_CATCH)
        return block_error_catch(value->block) == value;
    return false;
}

static XrCoreIrKey imported_value_key(const XrXiFunctionStorage *function, const XiBlock *block,
                                      const XiValue *value) {
    uint8_t material[1u + XR_CORE_IR_KEY_SIZE + 8u];
    material[0] = UINT8_C(0x41);
    memcpy(material + 1u, function->key.bytes, sizeof(function->key.bytes));
    put_u32_be(material + 1u + sizeof(function->key.bytes), block->id);
    put_u32_be(material + 5u + sizeof(function->key.bytes), value->id);
    return xr_core_ir_key(material, sizeof(material));
}

static XrXiBlockStorage *find_block_storage(const XrXiFunctionStorage *function,
                                            const XiBlock *block) {
    if (!function || !block)
        return NULL;
    for (uint32_t index = 0; index < function->xi->nblocks; ++index) {
        if (function->block_storage[index].xi == block)
            return &function->block_storage[index];
    }
    return NULL;
}

static const XrXiTrapEdge *find_trap_edge(const XrXiBuildContext *context, const XiFunc *function,
                                          const XiValue *call) {
    for (uint32_t index = 0u; context && index < context->trap_edge_count; ++index) {
        const XrXiTrapEdge *edge = &context->trap_edges[index];
        if (edge->function == function && edge->call == call)
            return edge;
    }
    return NULL;
}

static const XrXiPanicEdge *find_panic_edge(const XrXiBuildContext *context, const XiFunc *function,
                                            const XiValue *point) {
    for (uint32_t index = 0u; context && index < context->panic_edge_count; ++index) {
        const XrXiPanicEdge *edge = &context->panic_edges[index];
        if (edge->function == function && edge->point == point)
            return edge;
    }
    return NULL;
}

static const XrXiCancelEdge *find_cancel_edge(const XrXiBuildContext *context,
                                              const XiFunc *function,
                                              const XiCoroSuspendPoint *point) {
    for (uint32_t index = 0u; context && index < context->cancel_edge_count; ++index) {
        const XrXiCancelEdge *edge = &context->cancel_edges[index];
        if (edge->function == function && edge->point == point)
            return edge;
    }
    return NULL;
}

static XrXiBlockArgumentStorage *find_block_argument(XrXiBlockStorage *block,
                                                     const XiValue *source) {
    for (uint32_t index = 0; block && index < block->argument_count; ++index) {
        if (block->argument_storage[index].source == source)
            return &block->argument_storage[index];
    }
    return NULL;
}

static bool xi_type_has_logical_value_identity(const XrType *type) {
    return type && (type->kind == XR_KIND_TUPLE || type->kind == XR_KIND_ENUM ||
                    ((type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS) &&
                     type->instance.class_ref));
}

static const XiValue *logical_value_identity(const XiValue *value) {
    while (xi_copy_is_identity_alias(value) && value->nargs == 1u && value->args && value->args[0])
        value = value->args[0];
    while (
        value && value->nargs == 1u && value->args && value->args[0] && value->type &&
        ((value->op == XI_RETAIN && xi_type_has_logical_value_identity(value->type)) ||
         (xi_copy_is_value_clone(value) &&
          (value->type->kind == XR_KIND_TUPLE ||
           ((value->type->kind == XR_KIND_INSTANCE || value->type->kind == XR_KIND_CLASS) &&
            value->type->instance.class_ref && value->type->instance.class_ref->struct_layout))))) {
        value = value->args[0];
        while (xi_copy_is_identity_alias(value) && value->nargs == 1u && value->args &&
               value->args[0])
            value = value->args[0];
    }
    return value;
}

static bool resolved_callable_call_targets(const XrXiBuildContext *context, const XiFunc *caller,
                                           const XiValue *call, XrXiCallableTargetSet *target_set) {
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    const XgCallableTargetSummary *targets = NULL;
    uint32_t target_count = 0u;
    bool evidence_valid = call && row &&
                          xg_global_evidence_callable_targets(context->source->global_evidence, row,
                                                              &targets, &target_count);
    bool mirror_valid = evidence_valid &&
                        call->xg_callable_target_start == row->callable_target_start &&
                        call->xg_callable_target_count == row->callable_target_count &&
                        call->xg_callable_signature_key == row->callable_signature_key &&
                        call->xg_callable_effect_union == row->callable_effect_union &&
                        call->xg_callable_capability_union == row->callable_capability_union;
    bool error_valid = evidence_valid && (((row->callable_effect_union & XG_BODY_MAY_ERROR) !=
                                           0u) == ((row->flags & XG_CALL_MAY_ERROR) != 0u));
    bool suspend_valid = evidence_valid && (((row->callable_effect_union & XG_BODY_MAY_SUSPEND) !=
                                             0u) == ((row->flags & XG_CALL_MAY_SUSPEND) != 0u));
    bool panic_valid = evidence_valid && (((row->callable_effect_union & XG_BODY_MAY_PANIC) !=
                                           0u) == ((row->flags & XG_CALL_MAY_PANIC) != 0u));
    if (!evidence_valid || !mirror_valid || !error_valid || !suspend_valid || !panic_valid)
        return false;
    for (uint32_t index = 0u; index < target_count; ++index) {
        if (!find_xi_function_by_xg_id(context, targets[index].target_func_id))
            return false;
    }
    if (target_set) {
        target_set->callsite = row;
        target_set->targets = targets;
        target_set->target_count = target_count;
    }
    return true;
}

static bool callable_target_matches_visible_type(XrXiBuildContext *context, const XrType *visible,
                                                 const XiFunc *target, uint16_t error_type_id,
                                                 uint16_t panic_type_id) {
    if (!context || !visible || visible->kind != XR_KIND_FUNCTION || !target ||
        target->has_receiver || visible->function.param_count < 0 ||
        (uint32_t) visible->function.param_count != target->nparams)
        return false;
    uint16_t visible_result = XR_CORE_TYPE_VOID;
    uint16_t target_result = XR_CORE_TYPE_VOID;
    if (!map_type(context, visible->function.return_type, &visible_result) ||
        !map_type(context, target->return_type, &target_result) || visible_result != target_result)
        return false;
    for (uint16_t parameter = 0u; parameter < target->nparams; ++parameter) {
        const XiValue *target_parameter = target->params ? target->params[parameter] : NULL;
        const XrFunctionParam *visible_parameter = &visible->function.params[parameter];
        uint16_t visible_type = XR_CORE_TYPE_VOID;
        uint16_t target_type = XR_CORE_TYPE_VOID;
        if (!target_parameter || target_parameter->op != XI_PARAM || !visible_parameter->type ||
            !map_type(context, visible_parameter->type, &visible_type) ||
            !map_type(context, target_parameter->type, &target_type) ||
            visible_type != target_type || visible_parameter->mode != target_parameter->param_mode)
            return false;
    }
    uint16_t target_error = XR_CORE_TYPE_VOID;
    uint16_t target_panic = XR_CORE_TYPE_VOID;
    return map_function_error_type(context, target, &target_error, NULL, 0u) ==
               XR_PROGRAM_BUILD_OK &&
           map_function_panic_type(context, target, &target_panic, NULL, 0u) ==
               XR_PROGRAM_BUILD_OK &&
           target_error == error_type_id && target_panic == panic_type_id;
}

static bool callable_signature_key_for_target(const XrXiBuildContext *context, const XiFunc *target,
                                              uint64_t *structural_signature_key) {
    if (structural_signature_key)
        *structural_signature_key = 0u;
    if (!context || !target || !structural_signature_key || target->xg_body_func_id == XG_NO_ID ||
        !context->source->global_evidence)
        return false;
    const XgGlobalEvidence *evidence = context->source->global_evidence;
    bool found = false;
    for (uint32_t callsite_index = 0u; callsite_index < evidence->ncallsites; ++callsite_index) {
        const XgCallsiteSummary *callsite = &evidence->callsites[callsite_index];
        /* Xglobal also records unresolved callable-provenance sites (for
         * example, a closure value flowing out of a factory) as CLOSURE rows.
         * They are not executable closed target sets and therefore cannot
         * authenticate or contradict a target signature.  A row that claims
         * verification is authority and must remain structurally exact. */
        if (callsite->kind != XG_CALL_CLOSURE ||
            (callsite->flags & XG_CALL_TARGET_SET_VERIFIED) == 0u ||
            callsite->callable_signature_key == 0u)
            continue;
        const XgCallableTargetSummary *targets = NULL;
        uint32_t target_count = 0u;
        if (!xg_global_evidence_callable_targets(evidence, callsite, &targets, &target_count))
            return false;
        for (uint32_t target_index = 0u; target_index < target_count; ++target_index) {
            if (targets[target_index].target_func_id != target->xg_body_func_id)
                continue;
            if (found && *structural_signature_key != callsite->callable_signature_key)
                return false;
            *structural_signature_key = callsite->callable_signature_key;
            found = true;
        }
    }
    return found;
}

static const XgBodySummary *exact_function_body_contract(const XrXiBuildContext *context,
                                                         const XiFunc *function,
                                                         uint32_t module_index) {
    const XgGlobalEvidence *evidence =
        context && context->source ? context->source->global_evidence : NULL;
    const XgBodySummary *body = NULL;
    if (!evidence || !function || function->xg_body_func_id == XG_NO_ID)
        return NULL;
    for (uint32_t index = 0u; index < evidence->nbodies; ++index) {
        const XgBodySummary *candidate = &evidence->bodies[index];
        if (candidate->func_id != function->xg_body_func_id)
            continue;
        if (body)
            return NULL;
        body = candidate;
    }
    if (!body || body->module_id != (XgModuleId) (module_index + 1u) ||
        body->kind != XG_BODY_FUNCTION || body->owner_decl_id == XG_NO_ID ||
        body->signature_key == 0u)
        return NULL;
    const XgDeclSummary *declaration = NULL;
    for (uint32_t index = 0u; index < evidence->ndecls; ++index) {
        const XgDeclSummary *candidate = &evidence->decls[index];
        if (candidate->decl_id != body->owner_decl_id)
            continue;
        if (declaration)
            return NULL;
        declaration = candidate;
    }
    return declaration && declaration->kind == XG_DECL_FUNC &&
                   declaration->module_id == body->module_id &&
                   declaration->source_node_id == body->source_node_id &&
                   declaration->name_id == body->name_id &&
                   declaration->signature_key == body->signature_key
               ? body
               : NULL;
}

static bool exact_callable_target_effect_contract(const XrXiBuildContext *context,
                                                  const XiFunc *target,
                                                  uint64_t structural_signature_key,
                                                  uint32_t *effect_bits,
                                                  uint32_t *capability_bits) {
    const XgGlobalEvidence *evidence =
        context && context->source ? context->source->global_evidence : NULL;
    bool found = false;
    uint32_t exact_effects = 0u;
    uint32_t exact_capabilities = 0u;
    if (!evidence || !target || target->xg_body_func_id == XG_NO_ID ||
        structural_signature_key == 0u)
        return false;
    for (uint32_t callsite_index = 0u; callsite_index < evidence->ncallsites; ++callsite_index) {
        const XgCallsiteSummary *callsite = &evidence->callsites[callsite_index];
        const XgCallableTargetSummary *targets = NULL;
        uint32_t target_count = 0u;
        if (callsite->kind != XG_CALL_CLOSURE ||
            callsite->callable_signature_key != structural_signature_key)
            continue;
        if (!xg_global_evidence_callable_targets(evidence, callsite, &targets, &target_count))
            return false;
        for (uint32_t target_index = 0u; target_index < target_count; ++target_index) {
            const XgCallableTargetSummary *candidate = &targets[target_index];
            if (candidate->target_func_id != target->xg_body_func_id)
                continue;
            if (candidate->structural_signature_key != structural_signature_key ||
                (found && (candidate->effect_bits != exact_effects ||
                           candidate->capability_bits != exact_capabilities)))
                return false;
            exact_effects = candidate->effect_bits;
            exact_capabilities = candidate->capability_bits;
            found = true;
        }
    }
    if (found && effect_bits)
        *effect_bits = exact_effects;
    if (found && capability_bits)
        *capability_bits = exact_capabilities;
    return found;
}

static bool value_is_static_typed_catch_test(const XiValue *value) {
    return value && value->op == XI_IS && value->xg_existential_kind == XI_EXISTENTIAL_NONE &&
           value->nargs == 2u && value->args && value->args[0] && value->args[1] && value->aux &&
           value->block &&
           block_error_catch(value->block) == logical_value_identity(value->args[0]);
}

static const XiEnumData *static_typed_catch_token_schema(const XrXiBuildContext *context,
                                                         const XiValue *value,
                                                         const XrType *target) {
    const XiValue *token =
        value && value->nargs == 2u && value->args ? logical_value_identity(value->args[1]) : NULL;
    if (!context || !token || !target)
        return NULL;
    if (token->op == XI_CONST && token->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE && token->aux &&
        token->type && xr_type_equals(token->type, (XrType *) target))
        return (const XiEnumData *) token->aux;
    if (token->op != XI_GET_SHARED || token->aux_int < 0 || !value->block || !value->block->func)
        return NULL;
    uint32_t module_index = UINT32_MAX;
    if (!find_xi_function(context, value->block->func, &module_index, NULL) ||
        module_index >= context->source->module_count)
        return NULL;
    const XiFunc *root = context->source->module_roots[module_index];
    const XiModule *module = root ? root->module : NULL;
    uint32_t slot = (uint32_t) token->aux_int;
    return module && module->slot_enums && slot < module->nslots ? module->slot_enums[slot] : NULL;
}

static bool static_typed_catch_contract_is_exact(const XrXiBuildContext *context,
                                                 const XiValue *value) {
    if (!value_is_static_typed_catch_test(value))
        return false;
    const XrType *target = (const XrType *) value->aux;
    const XiEnumData *schema = static_typed_catch_token_schema(context, value, target);
    uint8_t decl_kind = 0u;
    return target && target->kind == XR_KIND_ENUM && !target->is_nullable &&
           variant_schema_matches_type(schema, target) &&
           find_variant_schema(context, target) == schema &&
           nominal_contract(context, target, &decl_kind, NULL, NULL) && decl_kind == XG_DECL_ENUM;
}

static bool function_contains_block(const XiFunc *function, const XiBlock *block) {
    for (uint32_t index = 0u; function && block && index < function->nblocks; ++index)
        if (function->blocks[index] == block)
            return true;
    return false;
}

static bool error_region_contract_is_exact(const XiFunc *function, const XiErrorRegion *region,
                                           const XiValue *caught) {
    if (!function || !region || !caught || caught->op != XI_ERR_CATCH ||
        caught->error_region != region || region->catch_value != caught ||
        caught->block != region->catch_block)
        return false;
    for (uint32_t depth = 0u; region; region = region->parent, ++depth) {
        if (depth >= function->nblocks || region->parent == region || !region->registration_block ||
            !region->body_block || !region->catch_block || !region->merge_block ||
            !region->catch_value || region->catch_value->op != XI_ERR_CATCH ||
            region->catch_value->error_region != region ||
            region->catch_value->block != region->catch_block ||
            block_error_catch(region->catch_block) != region->catch_value ||
            region->registration_block->succs[0] != region->body_block ||
            !function_contains_block(function, region->registration_block) ||
            !function_contains_block(function, region->body_block) ||
            !function_contains_block(function, region->catch_block) ||
            !function_contains_block(function, region->merge_block))
            return false;
    }
    return true;
}

static bool error_region_is_ancestor(const XiFunc *function, const XiErrorRegion *ancestor,
                                     const XiErrorRegion *region) {
    for (uint32_t depth = 0u; function && region && depth < function->nblocks;
         ++depth, region = region->parent)
        if (region == ancestor)
            return true;
    return false;
}

static XrProgramBuildStatus error_region_contains(const XiFunc *function,
                                                  const XiErrorRegion *region,
                                                  const XiValue *point_value,
                                                  const XiBlock *point_block, bool *contains) {
    if (contains)
        *contains = false;
    if (!function || !region || !contains || (!point_value && !point_block))
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint8_t *seen = xr_calloc(function->nblocks, sizeof(*seen));
    const XiBlock **work = xr_calloc(function->nblocks, sizeof(*work));
    if (!seen || !work) {
        xr_free(seen);
        xr_free(work);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    uint32_t body_index = UINT32_MAX;
    for (uint32_t index = 0u; index < function->nblocks; ++index)
        if (function->blocks[index] == region->body_block) {
            body_index = index;
            break;
        }
    if (body_index == UINT32_MAX) {
        xr_free(seen);
        xr_free(work);
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    uint32_t head = 0u;
    uint32_t tail = 0u;
    seen[body_index] = 1u;
    work[tail++] = region->body_block;
    while (head < tail && !*contains) {
        const XiBlock *block = work[head++];
        if (block == region->catch_block || block == region->merge_block)
            continue;
        if (point_block && block == point_block)
            *contains = true;
        for (uint32_t value = 0u; point_value && !*contains && value < block->nvalues; ++value)
            *contains = block->values[value] == point_value;
        for (uint32_t successor_index = 0u; successor_index < 2u && !*contains; ++successor_index) {
            const XiBlock *successor = block->succs[successor_index];
            uint32_t index = UINT32_MAX;
            for (uint32_t candidate = 0u; successor && candidate < function->nblocks; ++candidate)
                if (function->blocks[candidate] == successor) {
                    index = candidate;
                    break;
                }
            if (index != UINT32_MAX && !seen[index]) {
                seen[index] = 1u;
                work[tail++] = successor;
            }
        }
    }
    xr_free(seen);
    xr_free(work);
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus nearest_error_region(const XiFunc *function, const XiValue *point,
                                                 const XiErrorRegion **region_out, char *diagnostic,
                                                 size_t diagnostic_size) {
    if (region_out)
        *region_out = NULL;
    if (!function || !point || !region_out)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    const XiErrorRegion *selected = NULL;
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *marker = block->values[value_index];
            if (!marker || marker->op != XI_ERR_CATCH || !marker->error_region)
                continue;
            const XiErrorRegion *candidate = marker->error_region;
            if (!error_region_contract_is_exact(function, candidate, marker))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi error region has incomplete canonical structure");
            if (candidate->parent) {
                bool parent_contains_registration = false;
                XrProgramBuildStatus status = error_region_contains(
                    function, candidate->parent, NULL, candidate->registration_block,
                    &parent_contains_registration);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
                if (!parent_contains_registration)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi error region parent is outside its lexical region");
            }
            bool candidate_contains_point = false;
            XrProgramBuildStatus status =
                error_region_contains(function, candidate, point, NULL, &candidate_contains_point);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            if (!candidate_contains_point)
                continue;
            if (!selected || error_region_is_ancestor(function, selected, candidate))
                selected = candidate;
            else if (!error_region_is_ancestor(function, candidate, selected))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi error regions overlap without a lexical nesting relation");
        }
    }
    *region_out = selected;
    return XR_PROGRAM_BUILD_OK;
}

static const XiValue *routed_error_catch(const XrXiFunctionStorage *function, const XiBlock *entry,
                                         const XiBlock **catch_block_out) {
    if (catch_block_out)
        *catch_block_out = NULL;
    const XiBlock *block = entry;
    for (uint32_t depth = 0u; function && function->xi && block && depth <= function->xi->nblocks;
         ++depth) {
        const XiValue *caught = block_error_catch(block);
        if (caught) {
            if (catch_block_out)
                *catch_block_out = block;
            return caught;
        }
        const XrXiBlockStorage *storage = find_block_storage(function, block);
        if (!storage || !storage->reachable || block->kind != XI_BLOCK_PLAIN || block->control ||
            !block->succs[0] || block->succs[1])
            return NULL;
        /* A typed-error route may contain ordinary cleanup work, but it must
         * remain a single-successor, non-throwing path.  A second error
         * producer or check would make the caught value ambiguous. */
        for (uint32_t value_index = 0u; value_index < block->nvalues; ++value_index) {
            const XiValue *value = block->values[value_index];
            if (!value || value->op == XI_ERR_CHECK || value->op == XI_ERR_CATCH ||
                value->op == XI_THROW || (value->flags & XI_FLAG_MAY_THROW) != 0u)
                return NULL;
        }
        block = block->succs[0];
    }
    return NULL;
}

static const XiImportRef *imported_callable_ref(const XrXiBuildContext *context,
                                                const XiFunc *function, const XiValue *value) {
    if (!context || !function || !value || value->op != XI_GET_SHARED || value->aux_int < 0)
        return NULL;
    if (resolved_module_namespace_carrier(context, function, value))
        return NULL;
    uint32_t consumer_module_index = UINT32_MAX;
    if (!find_xi_function(context, function, &consumer_module_index, NULL) ||
        consumer_module_index >= context->source->module_count)
        return NULL;
    const XiFunc *consumer_root = context->source->module_roots[consumer_module_index];
    const XiModule *consumer_module = consumer_root ? consumer_root->module : NULL;
    uint32_t consumer_slot = (uint32_t) value->aux_int;
    if (!consumer_module || consumer_slot >= consumer_module->nslots ||
        !consumer_module->slot_imports)
        return NULL;
    return consumer_module->slot_imports[consumer_slot];
}

/* Resolve an imported callable only from the immutable resolver join and its
 * exact Xglobal body identity.  Shared/export slots are corroborating bounds;
 * neither source spelling nor a slot alone is allowed to select the target. */
static const XiFunc *resolved_imported_callable_target(const XrXiBuildContext *context,
                                                       const XiFunc *function, const XiValue *value,
                                                       uint64_t *signature_key) {
    if (signature_key)
        *signature_key = 0u;
    const XiImportRef *ref = imported_callable_ref(context, function, value);
    if (!ref || !ref->resolution_attempted || ref->resolved_mod_index < 0 ||
        (uint32_t) ref->resolved_mod_index >= context->source->module_count ||
        ref->resolved_shared_slot < 0 || ref->resolved_export_slot < 0 || !ref->resolved_module ||
        !ref->resolved_func)
        return NULL;

    uint32_t target_module_index = (uint32_t) ref->resolved_mod_index;
    const XiFunc *target_root = context->source->module_roots[target_module_index];
    const XiModule *target_module = target_root ? target_root->module : NULL;
    uint32_t target_slot = (uint32_t) ref->resolved_shared_slot;
    uint32_t export_slot = (uint32_t) ref->resolved_export_slot;
    if (!target_module || ref->resolved_module != target_module ||
        target_module->init != target_root || target_slot >= target_module->nslots ||
        export_slot >= target_module->nexports || !target_module->slot_funcs ||
        !target_module->exports || target_module->slot_funcs[target_slot] != ref->resolved_func)
        return NULL;
    const XiModuleExport *export_row = &target_module->exports[export_slot];
    if (export_row->shared_slot != target_slot || export_row->function != ref->resolved_func)
        return NULL;

    uint32_t function_module_index = UINT32_MAX;
    if (!find_xi_function(context, ref->resolved_func, &function_module_index, NULL) ||
        function_module_index != target_module_index ||
        ref->resolved_func->xg_body_func_id == XG_NO_ID)
        return NULL;
    const XgBodySummary *body =
        exact_function_body_contract(context, ref->resolved_func, target_module_index);
    if (!body)
        return NULL;
    if (signature_key) {
        uint64_t exact_signature_key = 0u;
        if (!callable_signature_key_for_target(context, ref->resolved_func, &exact_signature_key))
            return NULL;
        *signature_key = exact_signature_key;
    }
    return ref->resolved_func;
}

static const XrType *imported_callable_refined_type(const XrXiBuildContext *context,
                                                    const XiFunc *function,
                                                    const XiValue *imported);

static bool local_callable_publication_store_matches(const XiFunc *root, const XiValue *store,
                                                     uint32_t slot, const XiFunc *target) {
    if (!root || !root->module || root->module->init != root || !store || !target ||
        store->op != XI_SET_SHARED || store->aux_int < 0 || (uint32_t) store->aux_int != slot ||
        store->nargs != 1u || !store->args || !store->args[0] || target->parent_func != root)
        return false;
    const XiValue *closure = logical_value_identity(store->args[0]);
    return closure && closure->op == XI_CLOSURE_NEW && closure->nargs == 0u &&
           resolved_callable_target(root, closure) == target;
}

/* A top-level source function is immutable canonical code, not mutable module
 * state.  Xi still
 * routes its value through a shared slot.  Reconstruct the
 * callable pack only after the module
 * slot table, the unique initializer
 * publication, the Xi child body, and Xglobal's
 * body/signature identities all
 * agree on the same target. */
static const XiFunc *resolved_local_callable_target(const XrXiBuildContext *context,
                                                    const XiFunc *function, const XiValue *value,
                                                    uint64_t *signature_key) {
    if (signature_key)
        *signature_key = 0u;
    if (!context || !function || !value || value->op != XI_GET_SHARED || value->aux_int < 0 ||
        imported_callable_ref(context, function, value))
        return NULL;
    uint32_t module_index = UINT32_MAX;
    if (!find_xi_function(context, function, &module_index, NULL) ||
        module_index >= context->source->module_count)
        return NULL;
    const XiFunc *root = context->source->module_roots[module_index];
    const XiModule *module = root ? root->module : NULL;
    uint32_t slot = (uint32_t) value->aux_int;
    if (!module || module->init != root || slot >= module->nslots || !module->slot_funcs)
        return NULL;
    const XiFunc *target = module->slot_funcs[slot];
    uint32_t target_module_index = UINT32_MAX;
    if (!target || target->xg_body_func_id == XG_NO_ID ||
        !find_xi_function(context, target, &target_module_index, NULL) ||
        target_module_index != module_index ||
        !exact_function_body_contract(context, target, module_index))
        return NULL;

    uint32_t publications = 0u;
    for (uint32_t block_index = 0u; block_index < root->nblocks; ++block_index) {
        const XiBlock *block = root->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *candidate = block->values[value_index];
            if (!candidate || candidate->op != XI_SET_SHARED || candidate->aux_int < 0 ||
                (uint32_t) candidate->aux_int != slot)
                continue;
            if (!local_callable_publication_store_matches(root, candidate, slot, target))
                return NULL;
            publications++;
        }
    }
    if (publications != 1u)
        return NULL;
    if (signature_key) {
        uint64_t exact_signature_key = 0u;
        if (!callable_signature_key_for_target(context, target, &exact_signature_key))
            return NULL;
        *signature_key = exact_signature_key;
    }
    return target;
}

static const XiFunc *resolved_shared_callable_target(const XrXiBuildContext *context,
                                                     const XiFunc *function, const XiValue *value,
                                                     uint64_t *signature_key,
                                                     const XrType **visible_type) {
    if (visible_type)
        *visible_type = NULL;
    const XiFunc *target =
        resolved_imported_callable_target(context, function, value, signature_key);
    if (target) {
        const XiImportRef *ref = imported_callable_ref(context, function, value);
        if (visible_type) {
            *visible_type =
                ref && ref->has_exact_target && value->type && value->type->kind == XR_KIND_FUNCTION
                    ? value->type
                    : imported_callable_refined_type(context, function, value);
        }
        return visible_type && !*visible_type ? NULL : target;
    }
    target = resolved_local_callable_target(context, function, value, signature_key);
    if (target && visible_type)
        *visible_type = value->type;
    return target;
}

static bool imported_callable_checktype_is_only_exact_direct_callee(const XrXiBuildContext *context,
                                                                    const XiFunc *function,
                                                                    const XiValue *check,
                                                                    const XiFunc *target) {
    bool found = false;
    for (uint32_t block_index = 0u;
         context && function && check && target && block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!canonical_block_is_reachable(context, function, block))
            continue;
        if (block->control == check)
            return false;
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *consumer = block->values[value_index];
            for (uint16_t argument = 0u; consumer && argument < consumer->nargs; ++argument) {
                if (!consumer->args || consumer->args[argument] != check)
                    continue;
                const XgCallsiteSummary *row = resolved_callsite(context, function, consumer);
                if (argument != 0u || consumer->op != XI_CALL ||
                    resolved_sealed_callee(context, function, consumer) != target || !row ||
                    row->kind != XG_CALL_DIRECT_FUNC ||
                    row->static_target_func_id != target->xg_body_func_id ||
                    (row->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u)
                    return false;
                found = true;
            }
        }
    }
    return found;
}

/* CHECKTYPE is normally an observable dynamic assertion and cannot disappear
 * from canonical Program.  The sole mechanical exception is a refinement of
 * an imported shared slot whose resolver join, Xglobal body identity and
 * frozen Xi signature all prove the same non-null, infallible function type. */
static const char *imported_callable_checktype_proof_failure(const XrXiBuildContext *context,
                                                             const XiFunc *function,
                                                             const XiValue *check) {
    if (!context || !function || !check || check->op != XI_CHECKTYPE || check->nargs != 1u ||
        !check->args || !check->args[0])
        return "CHECKTYPE is not a unary imported-shared refinement";
    if (!check->type || check->type->kind != XR_KIND_FUNCTION)
        return "CHECKTYPE result is not a function type";
    if (check->type->is_nullable)
        return "CHECKTYPE function result is nullable";
    if (!xr_type_function_is_no_throw(check->type))
        return "CHECKTYPE function result is not proven no-throw";
    if (check->args[0]->op != XI_GET_SHARED)
        return "CHECKTYPE operand is not the direct imported shared slot";
    if (check->args[0]->type && !XR_TYPE_IS_UNKNOWN(check->args[0]->type))
        return "CHECKTYPE imported shared operand is not the erased internal type";
    uint8_t type_id = xr_type_to_tid(check->type);
    if (type_id == XR_TID_NULL || check->aux_int != ((int64_t) type_id << 1))
        return "CHECKTYPE runtime type id or null policy disagrees with its result type";
    const XiFunc *target =
        resolved_imported_callable_target(context, function, check->args[0], NULL);
    if (!target)
        return "CHECKTYPE imported slot has no unique resolver/body/signature join";
    uint32_t target_module_index = UINT32_MAX;
    uint64_t signature_key = 0u;
    uint32_t target_effects = 0u;
    uint32_t target_capabilities = 0u;
    const XgBodySummary *body =
        target && find_xi_function(context, target, &target_module_index, NULL)
            ? exact_function_body_contract(context, target, target_module_index)
            : NULL;
    if (!body)
        return "CHECKTYPE target has no exact Xglobal body contract";
    if (callable_signature_key_for_target(context, target, &signature_key)) {
        if (!exact_callable_target_effect_contract(context, target, signature_key, &target_effects,
                                                   &target_capabilities))
            return "CHECKTYPE target has no exact callable target-set effect contract";
    } else {
        if (!imported_callable_checktype_is_only_exact_direct_callee(context, function, check,
                                                                     target))
            return "CHECKTYPE target has neither a closed callable set nor an exact direct use";
        target_effects = body->effect_bits;
        target_capabilities = body->capability_bits;
    }
    uint32_t control_effects = XG_BODY_MAY_ERROR | XG_BODY_MAY_PANIC;
    if ((body->effect_bits & control_effects) != (target_effects & control_effects))
        return "CHECKTYPE target body and closed target-set control effects disagree";
    if ((body->capability_bits & ~target_capabilities) != 0u)
        return "CHECKTYPE target-set capability closure omits a target body capability";
    if ((target_effects & control_effects) != 0u)
        return "CHECKTYPE target-set is not proven error-free and panic-free";
    if (!callable_target_matches_visible_type((XrXiBuildContext *) context, check->type, target,
                                              XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID))
        return "CHECKTYPE target Xi signature or error/panic channels disagree with the visible "
               "type";
    return NULL;
}

static bool imported_callable_checktype_is_exact(const XrXiBuildContext *context,
                                                 const XiFunc *function, const XiValue *check) {
    return imported_callable_checktype_proof_failure(context, function, check) == NULL;
}

static bool cleanup_return_copy_is_exact(const XiValue *value) {
    return xi_copy_is_cleanup_return(value) && value->nargs == 1u && value->args &&
           value->args[0] && value->type && value->args[0]->type &&
           xr_type_equals(value->type, value->args[0]->type) &&
           value->xg_existential_kind == XI_EXISTENTIAL_NONE &&
           value->enum_metadata_owner == NULL && value->enum_metadata_kind == 0u;
}

/* A READ parameter is a value in Canonical Program. Xi may still materialize one LOCAL_ADDR
 *
 * because its executors borrow a value-struct argument through caller storage. Ordinary calls,
 *
 * static class calls and module-qualified calls use different Xi opcodes but retain the same
 *
 * leading phase-only carrier plus explicit parameter slots. Collapse that
 *
 * address only when every reachable use is the matching READ slot of one exact sealed call and
 *
 * the frozen call plan names this call-bound, non-escaping local. */
static bool read_value_call_place_is_exact(const XrXiBuildContext *context, const XiFunc *function,
                                           const XiValue *value) {
    value = logical_value_identity(value);
    const XiValue *storage =
        value && value->op == XI_LOCAL_ADDR && value->nargs == 1u && value->args
            ? logical_value_identity(value->args[0])
            : NULL;
    uint8_t decl_kind = 0u;
    XrCoreIrNominalKind nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    const XiClassData *schema =
        storage ? find_aggregate_schema(context, storage->type, NULL) : NULL;
    if (!context || !function || !value || !storage || !value->type || !storage->type ||
        !xi_local_addr_names_operand_storage(value->aux_int) ||
        !xr_type_equals(value->type, storage->type) ||
        !nominal_contract(context, storage->type, &decl_kind, &nominal_kind, NULL) ||
        decl_kind != XG_DECL_STRUCT || nominal_kind != XR_CORE_IR_NOMINAL_STRUCT || !schema ||
        !schema->struct_layout || schema->needs_runtime_type || schema->is_generic_skeleton)
        return false;

    bool found = false;
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!canonical_block_is_reachable(context, function, block))
            continue;
        if (block->control == value)
            return false;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            for (uint16_t argument = 0u; argument < phi->value.nargs; ++argument)
                if (phi->value.args && phi->value.args[argument] == value)
                    return false;
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *call = block->values[value_index];
            for (uint16_t argument = 0u; call && argument < call->nargs; ++argument) {
                if (call->args[argument] != value)
                    continue;
                const XiFunc *callee = resolved_sealed_callee(context, function, call);
                const XiCallPlan *plan = xi_call_plan(call);
                bool method_argument = callee && callee->has_receiver && argument != 0u;
                bool free_argument = callee && !callee->has_receiver && argument != 0u;
                uint16_t parameter =
                    method_argument ? argument
                                    : (free_argument ? (uint16_t) (argument - 1u) : UINT16_MAX);
                uint16_t plan_argument = method_argument ? (uint16_t) (parameter - 1u) : parameter;
                const XiCallArgPlan *argument_plan =
                    plan && plan_argument < plan->nargs ? &plan->args[plan_argument] : NULL;
                bool local_origin = argument_plan &&
                                    argument_plan->origin == XI_PLACE_ORIGIN_STACK_LOCAL &&
                                    xi_var_id_is_valid(argument_plan->origin_var_id) &&
                                    argument_plan->origin_var_id < function->source_var_count;
                bool direct_value_origin = argument_plan &&
                                           argument_plan->origin == XI_PLACE_ORIGIN_DIRECT_VALUE &&
                                           argument_plan->origin_var_id == XI_NO_VAR_ID &&
                                           xi_value_is_fresh_direct_storage(storage);
                bool sealed_call = call->op == XI_CALL || call->op == XI_CALL_METHOD ||
                                   call->op == XI_CALL_METHOD_DIRECT;
                bool exact_call_shape =
                    callee && plan &&
                    (callee->has_receiver
                         ? (plan->has_receiver && plan->nargs + 1u == callee->nparams &&
                            call->nargs == callee->nparams)
                         : (!plan->has_receiver && plan->nargs == callee->nparams &&
                            call->nargs == (uint32_t) callee->nparams + 1u));
                if (!callee || (!method_argument && !free_argument) || !sealed_call ||
                    !call->args || !plan || !plan->verified || !exact_call_shape ||
                    parameter >= callee->nparams || !callee->params ||
                    callee->params[parameter]->param_mode != XR_PARAM_READ || !argument_plan ||
                    argument_plan->param_mode != XR_PARAM_READ ||
                    argument_plan->access != XR_CALL_ARG_PLAIN ||
                    (!local_origin && !direct_value_origin) ||
                    argument_plan->lifetime != XI_PLACE_LIFETIME_CALL_BOUND ||
                    argument_plan->escape != XI_PLACE_ESCAPE_NONE || !argument_plan->addressable ||
                    argument_plan->place != value || !callee->params[parameter]->type ||
                    !xr_type_equals(callee->params[parameter]->type, storage->type))
                    return false;
                found = true;
            }
        }
    }
    return found;
}

/* Xi passes a READ value-struct receiver through caller storage. Canonical Program has no
 *
 * executor-specific call-place ABI, so collapse that address to its immutable value only when
 *
 * the complete call plan proves one non-escaping receiver use of the exact concrete method. */
static bool read_value_receiver_call_place_is_exact(const XrXiBuildContext *context,
                                                    const XiFunc *function, const XiValue *value) {
    value = logical_value_identity(value);
    const XiValue *storage =
        value && value->op == XI_LOCAL_ADDR && value->nargs == 1u && value->args
            ? logical_value_identity(value->args[0])
            : NULL;
    uint8_t decl_kind = 0u;
    XrCoreIrNominalKind nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    const XiClassData *schema =
        storage ? find_aggregate_schema(context, storage->type, NULL) : NULL;
    if (!context || !function || !value || !storage || !value->type || !storage->type ||
        !xi_local_addr_names_operand_storage(value->aux_int) ||
        !xr_type_equals(value->type, storage->type) ||
        !nominal_contract(context, storage->type, &decl_kind, &nominal_kind, NULL) ||
        decl_kind != XG_DECL_STRUCT || nominal_kind != XR_CORE_IR_NOMINAL_STRUCT || !schema ||
        !schema->struct_layout || schema->needs_runtime_type || schema->is_generic_skeleton)
        return false;

    bool found = false;
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!canonical_block_is_reachable(context, function, block))
            continue;
        if (block->control == value)
            return false;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            for (uint16_t argument = 0u; argument < phi->value.nargs; ++argument)
                if (phi->value.args && phi->value.args[argument] == value)
                    return false;
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *call = block->values[value_index];
            for (uint16_t argument = 0u; call && argument < call->nargs; ++argument) {
                if (call->args[argument] != value)
                    continue;
                const XiFunc *callee = resolved_sealed_callee(context, function, call);
                const XiCallPlan *plan = xi_call_plan(call);
                const XiCallArgPlan *receiver = plan ? &plan->receiver : NULL;
                bool local_origin = receiver && receiver->origin == XI_PLACE_ORIGIN_STACK_LOCAL &&
                                    xi_var_id_is_valid(receiver->origin_var_id) &&
                                    receiver->origin_var_id < function->source_var_count;
                bool direct_value_origin = receiver &&
                                           receiver->origin == XI_PLACE_ORIGIN_DIRECT_VALUE &&
                                           receiver->origin_var_id == XI_NO_VAR_ID &&
                                           xi_value_is_fresh_direct_storage(storage);
                bool sealed_call = call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT;
                if (!callee || !callee->has_receiver || !callee->receiver_call_place ||
                    callee->receiver_mode != XR_PARAM_READ || !sealed_call || !call->args ||
                    argument != 0u || !plan || !plan->verified || !plan->has_receiver ||
                    plan->nargs + 1u != callee->nparams || call->nargs != callee->nparams ||
                    !callee->params || !callee->params[0] ||
                    callee->params[0]->param_mode != XR_PARAM_READ || !receiver ||
                    receiver->param_mode != XR_PARAM_READ ||
                    receiver->access != XR_CALL_ARG_PLAIN ||
                    (!local_origin && !direct_value_origin) ||
                    receiver->lifetime != XI_PLACE_LIFETIME_CALL_BOUND ||
                    receiver->escape != XI_PLACE_ESCAPE_NONE || !receiver->addressable ||
                    receiver->place != value || !callee->params[0]->type ||
                    !xr_type_equals(callee->params[0]->type, storage->type))
                    return false;
                found = true;
            }
        }
    }
    return found;
}

/* Xi exposes a value-struct READ receiver as a call-bound place so its executors can borrow the
 *
 * caller's storage. Canonical Program passes READ parameters as immutable values. Erase the
 *
 * physical load only when it is the exact receiver parameter of one concrete, non-runtime struct
 *
 * declaration; REF receivers and heap classes keep their explicit place semantics. */
static bool read_value_receiver_load_is_exact(const XrXiBuildContext *context,
                                              const XiFunc *function, const XiValue *value) {
    value = logical_value_identity(value);
    const XiValue *receiver =
        value && value->op == XI_PLACE_LOAD && value->nargs == 1u && value->args
            ? logical_value_identity(value->args[0])
            : NULL;
    uint8_t decl_kind = 0u;
    XrCoreIrNominalKind nominal_kind = XR_CORE_IR_NOMINAL_NONE;
    const XiClassData *schema =
        receiver ? find_aggregate_schema(context, receiver->type, NULL) : NULL;
    return context && function && function->has_receiver &&
           function->receiver_mode == XR_PARAM_READ && function->nparams != 0u &&
           function->params && receiver == logical_value_identity(function->params[0]) &&
           receiver && receiver->op == XI_PARAM && receiver->param_mode == XR_PARAM_READ &&
           value->type && receiver->type && xr_type_equals(value->type, receiver->type) &&
           nominal_contract(context, receiver->type, &decl_kind, &nominal_kind, NULL) &&
           decl_kind == XG_DECL_STRUCT && nominal_kind == XR_CORE_IR_NOMINAL_STRUCT && schema &&
           schema->struct_layout && !schema->needs_runtime_type && !schema->is_generic_skeleton;
}

static const XiValue *exact_logical_value_identity(const XrXiBuildContext *context,
                                                   const XiFunc *function, const XiValue *value) {
    for (;;) {
        value = logical_value_identity(value);
        if (cleanup_return_copy_is_exact(value)) {
            value = value->args[0];
            continue;
        }
        if (imported_callable_checktype_is_exact(context, function, value)) {
            value = value->args[0];
            continue;
        }
        if (read_value_receiver_load_is_exact(context, function, value)) {
            value = value->args[0];
            continue;
        }
        if (read_value_call_place_is_exact(context, function, value)) {
            value = value->args[0];
            continue;
        }
        if (read_value_receiver_call_place_is_exact(context, function, value)) {
            value = value->args[0];
            continue;
        }
        return value;
    }
}

static uint32_t coroutine_point_value_occurrences(const XrXiBuildContext *context,
                                                  const XiFunc *function, XiValue *const *values,
                                                  uint32_t value_count, const XiValue *target) {
    uint32_t occurrences = 0u;
    target = exact_logical_value_identity(context, function, target);
    for (uint32_t index = 0u; target && values && index < value_count; ++index)
        occurrences +=
            exact_logical_value_identity(context, function, values[index]) == target ? 1u : 0u;
    return occurrences;
}

/* A call-bound place aliases its operand storage or an explicitly marked field
 * projection.
 * Repeated LOCAL_ADDR(PLACE_LOAD(...)) chains retain that root.
 * An ordinary local initialized
 * from a field remains an independent value copy. */
static const XiValue *local_place_storage_root(const XrXiBuildContext *context,
                                               const XiFunc *function, const XiValue *candidate) {
    const XiValue *place = exact_logical_value_identity(context, function, candidate);
    uint64_t remaining = 1u;
    for (uint32_t block = 0u; function && block < function->nblocks; ++block)
        remaining += function->blocks[block] ? function->blocks[block]->nvalues : 0u;
    while (remaining-- != 0u) {
        const XiValue *projection_base =
            direct_projection_base_place(context, function, place, NULL);
        if (projection_base) {
            place = exact_logical_value_identity(context, function, projection_base);
            if (!logical_value_is_place(function, place))
                return place;
            continue;
        }
        if (!place || place->op != XI_LOCAL_ADDR || place->nargs != 1u || !place->args ||
            !place->args[0] || !xi_local_addr_names_operand_storage(place->aux_int))
            return NULL;
        const XiValue *owner = exact_logical_value_identity(context, function, place->args[0]);
        if (!owner || owner == place || !owner->type || !place->type ||
            !xr_type_equals(owner->type, place->type))
            return NULL;
        if (owner->op != XI_PLACE_LOAD)
            return owner;
        if (owner->nargs != 1u || !owner->args || !owner->args[0])
            return NULL;
        const XiValue *next = exact_logical_value_identity(context, function, owner->args[0]);
        if (!next || next == place)
            return NULL;
        place = next;
    }
    return NULL;
}

/* A cleanup place is a derived capability, not an independently transferable
 * value. Its affine
 * source is the sole cross-block and cross-suspension
 * carrier; every continuation reconstructs a
 * fresh block-local place. */
static const XiValue *cleanup_place_storage_owner(const XrXiBuildContext *context,
                                                  const XiFunc *function,
                                                  const XiValue *candidate) {
    const XiValue *place = exact_logical_value_identity(context, function, candidate);
    const XiCoroPlan *plan = function ? function->coro_plan : NULL;
    if (!place || place->op != XI_LOCAL_ADDR || place->nargs != 1u || !place->args ||
        !place->args[0] || (place->aux_int & XI_LOCAL_ADDR_AUX_CLEANUP_LIVE) == 0)
        return NULL;
    const XiValue *owner = local_place_storage_root(context, function, place);
    if (!owner)
        return NULL;

    if (!plan || !plan->is_coroutine)
        return owner;
    if (!plan->analysis_complete || !plan->actions_materialized || !plan->cfg_rewritten ||
        !xi_coro_plan_is_current(function, plan) || !plan->points)
        return NULL;
    for (uint32_t point_index = 0u; point_index < plan->nstates; ++point_index) {
        const XiCoroSuspendPoint *point = &plan->points[point_index];
        uint32_t place_live =
            coroutine_point_value_occurrences(context, function, point->live, point->nlive, place);
        if (place_live == 0u)
            continue;
        if (place_live != 1u ||
            coroutine_point_value_occurrences(context, function, point->live, point->nlive,
                                              owner) != 1u ||
            coroutine_point_value_occurrences(context, function, point->drops, point->ndrops,
                                              owner) != 1u)
            return NULL;
    }
    return owner;
}

static bool cleanup_place_has_definition_block_use(const XrXiBuildContext *context,
                                                   const XiFunc *function,
                                                   const XiValue *candidate) {
    const XiValue *place = exact_logical_value_identity(context, function, candidate);
    const XiBlock *block = place ? place->block : NULL;
    for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
        const XiValue *user = block->values[value_index];
        if (!user || user == place)
            continue;
        for (uint16_t argument = 0u; user->args && argument < user->nargs; ++argument)
            if (exact_logical_value_identity(context, function, user->args[argument]) == place)
                return true;
    }
    return false;
}

static const XiValue *deferred_cleanup_place_owner(const XrXiBuildContext *context,
                                                   const XiFunc *function,
                                                   const XiValue *candidate) {
    const XiValue *owner = cleanup_place_storage_owner(context, function, candidate);
    return owner && !cleanup_place_has_definition_block_use(context, function, candidate) ? owner
                                                                                          : NULL;
}

static const XiValue *canonical_owner_storage_identity(const XrXiBuildContext *context,
                                                       const XrXiFunctionStorage *function,
                                                       const XiValue *owner) {
    owner = exact_logical_value_identity(context, function ? function->xi : NULL, owner);
    if (owner && owner->op == XI_PLACE_LOAD && owner->nargs == 1u && owner->args) {
        const XiValue *storage_owner =
            local_place_storage_root(context, function->xi, owner->args[0]);
        if (storage_owner)
            owner = exact_logical_value_identity(context, function->xi, storage_owner);
    }
    return owner;
}

static const XrType *imported_callable_refined_type(const XrXiBuildContext *context,
                                                    const XiFunc *function,
                                                    const XiValue *imported) {
    XrType *found = NULL;
    for (uint32_t block_index = 0u; function && block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!canonical_block_is_reachable(context, function, block))
            continue;
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *candidate = block->values[value_index];
            if (!imported_callable_checktype_is_exact(context, function, candidate) ||
                logical_value_identity(candidate->args[0]) != imported)
                continue;
            if (found && !xr_type_equals(found, candidate->type))
                return NULL;
            found = candidate->type;
        }
    }
    return found;
}

static bool shared_callable_value_is_exact(const XrXiBuildContext *context, const XiFunc *function,
                                           const XiValue *value) {
    uint64_t signature_key = 0u;
    const XrType *visible_type = NULL;
    return value && value->op == XI_GET_SHARED &&
           resolved_shared_callable_target(context, function, value, &signature_key,
                                           &visible_type) != NULL &&
           visible_type && signature_key != 0u;
}

static XrProgramBuildStatus validate_imported_callable_bindings(const XrXiBuildContext *context,
                                                                char *diagnostic,
                                                                size_t diagnostic_size) {
    for (uint32_t module_index = 0u; context && module_index < context->source->module_count;
         ++module_index) {
        const XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            const XiFunc *function = module->xi_functions[function_index];
            for (uint32_t block_index = 0u; function && block_index < function->nblocks;
                 ++block_index) {
                const XiBlock *block = function->blocks[block_index];
                if (!canonical_block_is_reachable(context, function, block))
                    continue;
                for (uint32_t value_index = 0u; block && value_index < block->nvalues;
                     ++value_index) {
                    const XiValue *value = block->values[value_index];
                    if (!value || !imported_callable_ref(context, function, value) ||
                        resolved_class_carrier(context, function, value, XG_NO_ID, NULL))
                        continue;
                    if (!resolved_imported_callable_target(context, function, value, NULL))
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi imported callable v%u has an inconsistent resolver join",
                                    value->id);
                }
            }
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

/* Every executable indirect call must cross one exact Xglobal-to-Xi evidence
 * boundary before contract closure begins.  Keep absence distinct from a
 * present-but-corrupt row: the former is an unresolved program graph, while
 * the latter is invalid canonical input and must never degrade into a later
 * CoreSpec projection failure. */
static XrProgramBuildStatus validate_callable_callsite_bindings(const XrXiBuildContext *context,
                                                                char *diagnostic,
                                                                size_t diagnostic_size) {
    for (uint32_t module_index = 0u; context && module_index < context->source->module_count;
         ++module_index) {
        const XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            const XiFunc *function = module->xi_functions[function_index];
            for (uint32_t block_index = 0u; function && block_index < function->nblocks;
                 ++block_index) {
                const XiBlock *block = function->blocks[block_index];
                if (!canonical_block_is_reachable(context, function, block))
                    continue;
                for (uint32_t value_index = 0u; block && value_index < block->nvalues;
                     ++value_index) {
                    const XiValue *call = block->values[value_index];
                    if (!call || call->op != XI_CALL ||
                        resolved_sealed_callee(context, function, call) ||
                        resolved_canonical_class_construction(context, function, call, NULL,
                                                              NULL) ||
                        resolved_provider_native_call(context, function, call) ||
                        resolved_suspension_native_call(context, function, call))
                        continue;

                    const XgCallsiteSummary *row = resolved_callsite(context, function, call);
                    if (!row)
                        return fail(diagnostic, diagnostic_size,
                                    XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                    "Xi indirect call %s:v%u module=%u block=%u has an unresolved "
                                    "callable target set",
                                    function->name ? function->name : "<anonymous>", call->id,
                                    module_index, block_index);
                    if (row->kind != XG_CALL_CLOSURE ||
                        (row->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u) {
                        const XiImportRef *reference =
                            call->nargs && call->args ? xi_value_import_ref(function, call->args[0])
                                                      : NULL;
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi indirect call %s:v%u module=%u block=%u has inconsistent "
                                    "callable effect evidence (kind=%u flags=0x%x nargs=%u "
                                    "native=%u argc=%u method=%llu/%llu metadata=%u target=%s.%s)",
                                    function->name ? function->name : "<anonymous>", call->id,
                                    module_index, block_index, (unsigned) row->kind,
                                    (unsigned) row->flags, (unsigned) call->nargs,
                                    xi_import_ref_is_grounded_native(reference) ? 1u : 0u,
                                    (unsigned) row->arg_count, (unsigned long long) row->method_id,
                                    (unsigned long long) xg_name_id(
                                        reference ? reference->member_name : NULL),
                                    reference && xr_stdlib_metadata_exact_native_suspension_call(
                                                     reference->module_path, reference->member_name,
                                                     row->arg_count)
                                        ? 1u
                                        : 0u,
                                    reference && reference->module_path ? reference->module_path
                                                                        : "<none>",
                                    reference && reference->member_name ? reference->member_name
                                                                        : "<none>");
                    }

                    const XgCallableTargetSummary *targets = NULL;
                    uint32_t target_count = 0u;
                    if (!xg_global_evidence_callable_targets(context->source->global_evidence, row,
                                                             &targets, &target_count))
                        return fail(
                            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi indirect call v%u has inconsistent callable target evidence",
                            call->id);
                    if (call->xg_callable_target_start != row->callable_target_start ||
                        call->xg_callable_target_count != row->callable_target_count ||
                        call->xg_callable_signature_key != row->callable_signature_key ||
                        call->xg_callable_effect_union != row->callable_effect_union ||
                        call->xg_callable_capability_union != row->callable_capability_union)
                        return fail(
                            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi indirect call v%u disagrees with its callable target evidence",
                            call->id);
                    if ((((row->callable_effect_union & XG_BODY_MAY_ERROR) != 0u) !=
                         ((row->flags & XG_CALL_MAY_ERROR) != 0u)) ||
                        (((row->callable_effect_union & XG_BODY_MAY_SUSPEND) != 0u) !=
                         ((row->flags & XG_CALL_MAY_SUSPEND) != 0u)) ||
                        (((row->callable_effect_union & XG_BODY_MAY_PANIC) != 0u) !=
                         ((row->flags & XG_CALL_MAY_PANIC) != 0u)))
                        return fail(
                            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi indirect call v%u has inconsistent callable control effects",
                            call->id);
                    for (uint32_t target_index = 0u; target_index < target_count; ++target_index) {
                        if (!find_xi_function_by_xg_id(context,
                                                       targets[target_index].target_func_id))
                            return fail(
                                diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi indirect call v%u targets a function outside the input graph",
                                call->id);
                    }
                }
            }
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

/* A source function type intentionally carries only structural shape and the
 * typed throw bit.  Xglobal owns the exact closed target sets.  Therefore one
 * visible callable SignatureId is built from the union of every exact set with
 * the same structural key; individual packs and callsites may not narrow that
 * contract from local flow or from a backend carrier. */
static bool map_callable_signature_contract(XrXiBuildContext *context, const XrType *visible,
                                            uint64_t structural_signature_key, uint16_t *type_id,
                                            XrXiCallableContract *contract_out) {
    if (!context || !visible || !type_id || structural_signature_key == 0u ||
        !context->source->global_evidence)
        return false;
    const XgGlobalEvidence *evidence = context->source->global_evidence;
    XrXiCallableContract contract = {
        .structural_signature_key = structural_signature_key,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
    };
    bool channel_initialized = false;
    for (uint32_t callsite_index = 0u; callsite_index < evidence->ncallsites; ++callsite_index) {
        const XgCallsiteSummary *callsite = &evidence->callsites[callsite_index];
        if (callsite->kind != XG_CALL_CLOSURE ||
            callsite->callable_signature_key != structural_signature_key)
            continue;
        const XgCallableTargetSummary *targets = NULL;
        uint32_t target_count = 0u;
        if (!xg_global_evidence_callable_targets(evidence, callsite, &targets, &target_count))
            return false;
        for (uint32_t target_index = 0u; target_index < target_count; ++target_index) {
            const XiFunc *target =
                find_xi_function_by_xg_id(context, targets[target_index].target_func_id);
            const XrXiFunctionStorage *storage = find_xi_function(context, target, NULL, NULL);
            uint16_t error_type_id = XR_CORE_TYPE_VOID;
            uint16_t panic_type_id = XR_CORE_TYPE_VOID;
            if (!target || !storage || !storage->closed_contract_ready ||
                map_function_error_type(context, target, &error_type_id, NULL, 0u) !=
                    XR_PROGRAM_BUILD_OK ||
                map_function_panic_type(context, target, &panic_type_id, NULL, 0u) !=
                    XR_PROGRAM_BUILD_OK)
                return false;
            bool target_may_error = (targets[target_index].effect_bits & XG_BODY_MAY_ERROR) != 0u;
            bool target_may_panic = (targets[target_index].effect_bits & XG_BODY_MAY_PANIC) != 0u;
            if (target_may_error != ((storage->closed_effect_mask & XR_CORE_EFFECT_ERROR) != 0u) ||
                target_may_panic != ((storage->closed_effect_mask & XR_CORE_EFFECT_PANIC) != 0u))
                return false;
            if (!channel_initialized) {
                contract.error_type_id = error_type_id;
                contract.panic_type_id = panic_type_id;
                channel_initialized = true;
            } else if (contract.error_type_id != error_type_id ||
                       contract.panic_type_id != panic_type_id) {
                return false;
            }
            if (!callable_target_matches_visible_type(context, visible, target, error_type_id,
                                                      panic_type_id))
                return false;
            contract.effect_mask |= storage->closed_effect_mask;
            contract.capability_mask |= storage->closed_capability_mask;
            ++contract.target_count;
        }
    }
    if (!channel_initialized || contract.target_count == 0u ||
        !map_callable_type_contract_recursive(context, visible, contract.effect_mask,
                                              contract.capability_mask, contract.error_type_id,
                                              contract.panic_type_id, type_id, NULL, 0u))
        return false;
    if (contract_out)
        *contract_out = contract;
    return true;
}

static bool map_callable_target_type(XrXiBuildContext *context, const XrType *type,
                                     const XiFunc *target, uint16_t *type_id) {
    uint64_t structural_signature_key = 0u;
    return callable_signature_key_for_target(context, target, &structural_signature_key) &&
           map_callable_signature_contract(context, type, structural_signature_key, type_id, NULL);
}

static bool map_callable_call_type(XrXiBuildContext *context, const XiFunc *caller,
                                   const XiValue *call, const XrType *visible, uint16_t *type_id,
                                   XrXiCallableContract *contract_out) {
    XrXiCallableTargetSet target_set = {0};
    return resolved_callable_call_targets(context, caller, call, &target_set) &&
           map_callable_signature_contract(context, visible,
                                           target_set.callsite->callable_signature_key, type_id,
                                           contract_out);
}

static bool callable_signature_key_for_value(const XrXiBuildContext *context,
                                             const XiFunc *function, const XiValue *value,
                                             uint32_t depth, uint64_t *signature_key) {
    if (signature_key)
        *signature_key = 0u;
    if (!context || !function || !value || !signature_key || depth > function->next_value_id + 1u)
        return false;
    if (imported_callable_checktype_is_exact(context, function, value))
        return callable_signature_key_for_value(context, function, value->args[0], depth + 1u,
                                                signature_key);
    value = logical_value_identity(value);
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!canonical_block_is_reachable(context, function, block))
            continue;
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *call = block->values[value_index];
            if (!call || call->op != XI_CALL || call->nargs == 0u || !call->args ||
                logical_value_identity(call->args[0]) != value)
                continue;
            XrXiCallableTargetSet targets = {0};
            if (!resolved_callable_call_targets(context, function, call, &targets))
                return false;
            *signature_key = targets.callsite->callable_signature_key;
            return *signature_key != 0u;
        }
    }
    if (value->op == XI_CLOSURE_NEW) {
        const XiFunc *target = resolved_callable_target(function, value);
        return callable_signature_key_for_target(context, target, signature_key);
    }
    if (value->op == XI_GET_SHARED) {
        const XiFunc *target =
            resolved_shared_callable_target(context, function, value, signature_key, NULL);
        return target != NULL;
    }
    if ((xi_value_forwards_identity(value) || value->op == XI_RETAIN ||
         (value->op == XI_CALL_BUILTIN && value->aux && value->aux_kind == XI_AUX_KIND_NONE &&
          strcmp((const char *) value->aux, "copy") == 0)) &&
        value->nargs == 1u && value->args && value->args[0])
        return callable_signature_key_for_value(context, function, value->args[0], depth + 1u,
                                                signature_key);
    if (value->op != XI_PHI || value->nargs == 0u || !value->args)
        return false;
    bool found = false;
    uint64_t merged = 0u;
    for (uint16_t argument = 0u; argument < value->nargs; ++argument) {
        const XiValue *incoming = logical_value_identity(value->args[argument]);
        if (!incoming || incoming == value)
            continue;
        uint64_t candidate = 0u;
        if (!callable_signature_key_for_value(context, function, incoming, depth + 1u, &candidate))
            return false;
        if (found && merged != candidate)
            return false;
        merged = candidate;
        found = true;
    }
    if (!found)
        return false;
    *signature_key = merged;
    return true;
}

static bool map_logical_value_type(XrXiBuildContext *context, const XiFunc *function,
                                   const XiValue *value, uint16_t *type_id) {
    const XrType *visible_type = value ? value->type : NULL;
    if (imported_callable_checktype_is_exact(context, function, value)) {
        uint64_t signature_key = 0u;
        return callable_signature_key_for_value(context, function, value->args[0], 1u,
                                                &signature_key) &&
               map_callable_signature_contract(context, visible_type, signature_key, type_id, NULL);
    }
    if (value && value->op == XI_GET_SHARED) {
        uint64_t signature_key = 0u;
        const XrType *visible_type = NULL;
        if (resolved_shared_callable_target(context, function, value, &signature_key,
                                            &visible_type))
            return map_callable_signature_contract(context, visible_type, signature_key, type_id,
                                                   NULL);
    }
    /* Ownership forwarding can introduce the first typed carrier around an
     * otherwise untyped
     * CLOSURE_NEW node.  Preserve that visible function type
     * and resolve its exact Xglobal
     * target set before stripping identity ops. */
    if (value && value->type && value->type->kind == XR_KIND_FUNCTION &&
        xi_op_is_identity_forward(value->op)) {
        uint64_t signature_key = 0u;
        return callable_signature_key_for_value(context, function, value, 0u, &signature_key) &&
               map_callable_signature_contract(context, visible_type, signature_key, type_id, NULL);
    }
    value = logical_value_identity(value);
    if (!value || !value->type)
        return false;
    if (value->type->kind == XR_KIND_INTERFACE) {
        if (value->xg_interface_use_kind >= XI_INTERFACE_USE_READ &&
            value->xg_interface_use_kind <= XI_INTERFACE_USE_OWNED_STORAGE)
            return map_existential_type(context, value->type,
                                        (XiInterfaceUseKind) value->xg_interface_use_kind, type_id);
        if (value->op == XI_PARAM && xr_param_mode_is_valid((XrParamMode) value->param_mode))
            return map_type_for_mode(context, value->type, (XrParamMode) value->param_mode, type_id,
                                     NULL, 0u);
    }
    if (value->type->kind != XR_KIND_FUNCTION)
        return map_type(context, value->type, type_id);
    uint64_t signature_key = 0u;
    return callable_signature_key_for_value(context, function, value, 0u, &signature_key) &&
           map_callable_signature_contract(context, value->type, signature_key, type_id, NULL);
}

static bool logical_value_produces_owner(XrXiBuildContext *context, const XiFunc *function,
                                         const XiValue *value, uint32_t depth) {
    if (!context || !function || !value || depth > function->next_value_id + 1u)
        return false;
    if (imported_callable_checktype_is_exact(context, function, value))
        return logical_value_produces_owner(context, function, value->args[0], depth + 1u);
    if (xi_copy_is_identity_alias(value) && value->nargs == 1u && value->args && value->args[0])
        return logical_value_produces_owner(context, function, value->args[0], depth + 1u);
    if (value->op == XI_PARAM)
        return value->param_mode == XR_PARAM_MOVE;
    if (value->op == XI_PHI) {
        bool found = false;
        for (uint16_t argument = 0u; argument < value->nargs; ++argument) {
            const XiValue *incoming = value->args ? value->args[argument] : NULL;
            if (!incoming || incoming == value)
                continue;
            if (!logical_value_produces_owner(context, function, incoming, depth + 1u))
                return false;
            found = true;
        }
        return found;
    }
    uint16_t type_id = XR_CORE_TYPE_VOID;
    if (!map_logical_value_type(context, function, value, &type_id) ||
        logical_ownership_for_type(context, type_id) != XR_CORE_IR_OWNER)
        return false;
    if (value->op == XI_VARIANT_PROJECT)
        return value->nargs == 1u && value->args && value->args[0] &&
               logical_value_produces_owner(context, function, value->args[0], depth + 1u);
    return (value->op == XI_CLOSURE_NEW && value->nargs != 0u) ||
           value->xg_existential_kind == XI_EXISTENTIAL_PACK ||
           value->xg_existential_kind == XI_EXISTENTIAL_PROJECT || value->op == XI_SUM_INJECT ||
           xi_copy_is_value_clone(value) || value->op == XI_SOURCE_MOVE ||
           value->op == XI_OWNER_FORWARD || value->op == XI_CALL ||
           ((value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT) &&
            resolved_sealed_callee(context, function, value)) ||
           resolved_canonical_class_construction(context, function, value, NULL, NULL) ||
           resolved_value_aggregate_construction(context, function, value, NULL, NULL) ||
           (value->op == XI_CALL_BUILTIN && value->aux && value->aux_kind == XI_AUX_KIND_NONE &&
            strcmp((const char *) value->aux, "copy") == 0);
}

/* The existential pack replaces its consumed owner as the frame carrier. */
/* Its representation and witness survive suspension and cancellation. */
/* Only a unique consuming pack is accepted; ambiguity fails closed. */
static const XiValue *canonical_coroutine_owner_successor(const XrXiBuildContext *context,
                                                          const XiFunc *function,
                                                          const XiCoroSuspendPoint *point,
                                                          const XiValue *source) {
    source = exact_logical_value_identity(context, function, source);
    if (!source ||
        !logical_value_produces_owner((XrXiBuildContext *) context, function, source, 0u))
        return NULL;
    const XiValue *successor = NULL;
    for (uint32_t block = 0u; function && block < function->nblocks; ++block) {
        const XiBlock *row = function->blocks[block];
        for (uint32_t value = 0u; row && value < row->nvalues; ++value) {
            const XiValue *candidate = row->values[value];
            bool live_at_point = false;
            for (uint32_t live = 0u; point && live < point->nlive; ++live)
                live_at_point |= point->live[live] == candidate;
            if (!candidate || candidate->xg_existential_kind != XI_EXISTENTIAL_PACK ||
                !live_at_point ||
                (candidate->xg_interface_use_kind != XI_INTERFACE_USE_MOVE &&
                 candidate->xg_interface_use_kind != XI_INTERFACE_USE_OWNED_STORAGE) ||
                candidate->nargs != 1u || !candidate->args || !candidate->args[0] ||
                exact_logical_value_identity(context, function, candidate->args[0]) != source ||
                !logical_value_produces_owner((XrXiBuildContext *) context, function, candidate,
                                              0u))
                continue;
            if (successor && successor != candidate)
                return NULL;
            successor = candidate;
        }
    }
    return successor;
}

static XrCoreIrValueCategory logical_value_category(const XiFunc *function, const XiValue *value) {
    return logical_value_is_place(function, value) ? XR_CORE_IR_PLACE : XR_CORE_IR_VALUE;
}

static XrProgramBuildStatus add_block_argument(XrXiBuildContext *context,
                                               XrXiFunctionStorage *function,
                                               XrXiBlockStorage *block, const XiValue *source,
                                               const XiPhi *phi, uint16_t type_override,
                                               uint8_t implicit_invoke_kind, bool *changed,
                                               char *diagnostic, size_t diagnostic_size) {
    const XiValue *typed_source = source;
    source = exact_logical_value_identity(context, function ? function->xi : NULL, source);
    bool capture_receiver = function && source == &function->capture_receiver;
    if (!source || (!source->type && !capture_receiver))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi block argument source is incomplete");
    uint16_t type_id = type_override;
    uint16_t source_type_id = XR_CORE_TYPE_VOID;
    bool source_type_mapped = capture_receiver;
    if (capture_receiver)
        source_type_id = function->capture_type_id;
    else
        source_type_mapped =
            map_logical_value_type(context, function->xi, typed_source, &source_type_id) &&
            source_type_id != XR_CORE_TYPE_VOID;
    bool erased_error_catch =
        source->op == XI_ERR_CATCH && source->type && source->type->kind == XR_KIND_UNKNOWN;
    bool erased_panic_catch =
        implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_PANIC && source->op == XI_CATCH;
    if (type_id != XR_CORE_TYPE_VOID && !erased_panic_catch &&
        ((source_type_mapped && source_type_id != type_id) ||
         (!source_type_mapped && !erased_error_catch)))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi block argument v%u type conflicts with its canonical continuation",
                    source->id);
    if (type_id == XR_CORE_TYPE_VOID && source_type_mapped)
        type_id = source_type_id;
    if (type_id == XR_CORE_TYPE_VOID) {
        uint64_t callable_signature_key = 0u;
        bool callable_signature = source->type && source->type->kind == XR_KIND_FUNCTION &&
                                  callable_signature_key_for_value(context, function->xi, source,
                                                                   0u, &callable_signature_key);
        return fail(
            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
            "Xi live-in v%u op%u type=%u nargs=%u source=%u/%u in b%u has no active "
            "CoreSpec value type (callable=%u signature=%llu closure=%u trap=%u panic=%u "
            "cancel=%u)",
            source->id, (unsigned) source->op,
            source->type ? (unsigned) source->type->kind : UINT32_MAX, (unsigned) source->nargs,
            source->nargs && source->args && source->args[0] ? source->args[0]->id : UINT32_MAX,
            source->nargs && source->args && source->args[0] ? (unsigned) source->args[0]->op
                                                             : UINT32_MAX,
            block && block->xi ? block->xi->id : UINT32_MAX, callable_signature ? 1u : 0u,
            (unsigned long long) callable_signature_key, changed ? 1u : 0u,
            block && block->trap_cleanup ? 1u : 0u, block && block->panic_cleanup ? 1u : 0u,
            block && block->cancel_cleanup ? 1u : 0u);
    }
    if (!phi && implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_NONE && source->op == XI_PLACE_LOAD &&
        logical_ownership_for_type(context, type_id) == XR_CORE_IR_OWNER) {
        const XiValue *storage_owner = canonical_owner_storage_identity(context, function, source);
        uint16_t storage_type = XR_CORE_TYPE_VOID;
        if (!storage_owner || storage_owner == source ||
            !map_logical_value_type(context, function->xi, storage_owner, &storage_type) ||
            storage_type != type_id)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi affine place load v%u has no exact storage owner", source->id);
        source = storage_owner;
    }
    XrCoreIrValueCategory category = logical_value_category(function ? function->xi : NULL, source);
    XrCoreIrOwnershipDisposition ownership =
        implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT ||
                implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_ERROR ||
                implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_PANIC
            ? logical_ownership_for_type(context, type_id)
        : logical_value_produces_owner(context, function->xi, source, 0u)
            ? logical_ownership_for_type(context, type_id)
            : XR_CORE_IR_NON_OWNER;
    if (!phi && implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_NONE &&
        ownership == XR_CORE_IR_OWNER) {
        const XiValue *storage_identity =
            canonical_owner_storage_identity(context, function, source);
        for (uint32_t index = 0u; storage_identity && index < block->argument_count; ++index) {
            XrXiBlockArgumentStorage *current = &block->argument_storage[index];
            if (current->phi || current->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE ||
                current->ownership != XR_CORE_IR_OWNER ||
                canonical_owner_storage_identity(context, function, current->source) !=
                    storage_identity)
                continue;
            bool source_is_current =
                logical_value_produces_owner(context, function->xi, source, 0u);
            bool existing_is_current =
                logical_value_produces_owner(context, function->xi, current->source, 0u);
            if (source_is_current && existing_is_current && current->source != source)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi block b%u has multiple current owners for one storage",
                            block->xi ? block->xi->id : UINT32_MAX);
            if (current->type_id != type_id || current->category != category)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi block b%u has conflicting owner storage contracts",
                            block->xi ? block->xi->id : UINT32_MAX);
            if (!source_is_current || existing_is_current)
                return XR_PROGRAM_BUILD_OK;
            current->source = source;
            current->key = imported_value_key(function, block->xi, source);
            if (changed)
                *changed = true;
            return XR_PROGRAM_BUILD_OK;
        }
    }
    XrXiBlockArgumentStorage *existing = find_block_argument(block, source);
    if (existing) {
        if (existing->phi != phi || existing->type_id != type_id ||
            existing->category != category || existing->ownership != ownership ||
            existing->implicit_invoke_kind != implicit_invoke_kind)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi block argument v%u has conflicting canonical contracts", source->id);
        return XR_PROGRAM_BUILD_OK;
    }
    if (block->argument_count == block->argument_capacity) {
        uint32_t capacity = block->argument_capacity ? block->argument_capacity * 2u : 4u;
        size_t allocation_size = (size_t) capacity * sizeof(*block->argument_storage);
        if (capacity < block->argument_count ||
            allocation_size / sizeof(*block->argument_storage) != capacity)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        XrXiBlockArgumentStorage *arguments = xr_realloc(block->argument_storage, allocation_size);
        if (!arguments)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        block->argument_storage = arguments;
        block->argument_capacity = capacity;
    }
    XrXiBlockArgumentStorage *argument = &block->argument_storage[block->argument_count++];
    *argument = (XrXiBlockArgumentStorage) {
        .source = source,
        .phi = phi,
        .key = phi || (source->op == XI_PARAM && block->xi == function->xi->entry)
                   ? value_key(function, source)
                   : imported_value_key(function, block->xi, source),
        .type_id = type_id,
        .category = category,
        .ownership = ownership,
        .implicit_invoke_kind = implicit_invoke_kind,
    };
    if (changed)
        *changed = true;
    return XR_PROGRAM_BUILD_OK;
}

static XrXiReconstructedPlaceStorage *find_reconstructed_place(XrXiBlockStorage *block,
                                                               const XiValue *place) {
    for (uint32_t index = 0u; block && index < block->reconstructed_place_count; ++index)
        if (block->reconstructed_places[index].place == place)
            return &block->reconstructed_places[index];
    return NULL;
}

static XrProgramBuildStatus add_reconstructed_place(XrXiBuildContext *context,
                                                    XrXiFunctionStorage *function,
                                                    XrXiBlockStorage *block, const XiValue *place,
                                                    const XiValue *owner, uint16_t type_id) {
    XrXiReconstructedPlaceStorage *existing = find_reconstructed_place(block, place);
    if (existing)
        return existing->owner == owner && existing->type_id == type_id
                   ? XR_PROGRAM_BUILD_OK
                   : XR_PROGRAM_BUILD_INVALID_INPUT;
    if (!function || !block || !block->xi || !place || !owner || type_id == XR_CORE_TYPE_VOID)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint32_t field_ordinal = UINT32_MAX;
    const XiValue *base_place =
        direct_projection_base_place(context, function->xi, place, &field_ordinal);
    if (base_place) {
        uint16_t base_type = XR_CORE_TYPE_VOID;
        const XiValue *base_owner =
            logical_value_is_place(function->xi, base_place)
                ? local_place_storage_root(context, function->xi, base_place)
                : base_place;
        if (base_owner != owner ||
            !map_logical_value_type(context, function->xi, base_place, &base_type) ||
            base_type == XR_CORE_TYPE_VOID)
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        if (logical_value_is_place(function->xi, base_place)) {
            XrProgramBuildStatus base_status =
                add_reconstructed_place(context, function, block, base_place, owner, base_type);
            if (base_status != XR_PROGRAM_BUILD_OK)
                return base_status;
        }
    }
    if (block->reconstructed_place_count == block->reconstructed_place_capacity) {
        uint32_t capacity =
            block->reconstructed_place_capacity ? block->reconstructed_place_capacity * 2u : 2u;
        if (capacity < block->reconstructed_place_count)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        XrXiReconstructedPlaceStorage *places =
            xr_realloc(block->reconstructed_places, (size_t) capacity * sizeof(*places));
        if (!places)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        block->reconstructed_places = places;
        block->reconstructed_place_capacity = capacity;
    }
    block->reconstructed_places[block->reconstructed_place_count++] =
        (XrXiReconstructedPlaceStorage) {
            .place = place,
            .owner = owner,
            .base_place = base_place,
            .key = imported_value_key(function, block->xi, place),
            .type_id = type_id,
            .field_ordinal = field_ordinal,
        };
    return XR_PROGRAM_BUILD_OK;
}

static bool value_has_canonical_materialization(const XrXiBuildContext *context,
                                                const XiFunc *function, const XiValue *value) {
    if (!value)
        return false;
    if (xr_program_xi_value_is_materialized(value->op) ||
        shared_callable_value_is_exact(context, function, value) ||
        static_typed_catch_contract_is_exact(context, value) ||
        resolved_canonical_class_construction(context, function, value, NULL, NULL) ||
        resolved_value_aggregate_construction(context, function, value, NULL, NULL) ||
        resolved_empty_struct_literal(context, function, value) ||
        resolved_unit_enum_literal(context, function, value, NULL) ||
        resolved_aggregate_field_projection(context, value, NULL) ||
        resolved_aggregate_field_store(context, function, value, NULL))
        return true;
    XrProgramXiSemanticProjection projection;
    return xr_program_xi_semantic_projection(value->op, value->xg_existential_kind, &projection);
}

static bool value_operand_key(const XrXiBuildContext *context, const XrXiFunctionStorage *function,
                              const XrXiBlockStorage *block, const XiValue *value,
                              XrCoreIrKey *key_out) {
    value = exact_logical_value_identity(context, function ? function->xi : NULL, value);
    if (!value || !key_out)
        return false;
    XrXiReconstructedPlaceStorage *place =
        find_reconstructed_place((XrXiBlockStorage *) block, value);
    if (place) {
        *key_out = place->key;
        return true;
    }
    XrXiBlockArgumentStorage *argument = find_block_argument((XrXiBlockStorage *) block, value);
    if (argument) {
        *key_out = argument->key;
        return true;
    }
    const XiValue *storage_identity =
        value->block != block->xi ? canonical_owner_storage_identity(context, function, value)
                                  : NULL;
    const XrXiBlockArgumentStorage *storage_owner = NULL;
    for (uint32_t index = 0u; storage_identity && block && index < block->argument_count; ++index) {
        const XrXiBlockArgumentStorage *candidate = &block->argument_storage[index];
        if (candidate->ownership != XR_CORE_IR_OWNER ||
            canonical_owner_storage_identity(context, function, candidate->source) !=
                storage_identity)
            continue;
        if (storage_owner)
            return false;
        storage_owner = candidate;
    }
    if (storage_owner) {
        *key_out = storage_owner->key;
        return true;
    }
    if (value->block != block->xi)
        return false;
    if (deferred_cleanup_place_owner(context, function ? function->xi : NULL, value))
        return false;
    if (!value_has_canonical_materialization(context, function ? function->xi : NULL, value))
        return false;
    *key_out = value_key(function, value);
    return true;
}

static bool edge_value_operand_key(const XrXiBuildContext *context,
                                   const XrXiFunctionStorage *function,
                                   const XrXiBlockStorage *block, const XiValue *value,
                                   const XiValue *edge_point, XrCoreIrKey *key_out) {
    if (edge_point) {
        if (!function || !block || !block->xi || !value)
            return false;
        if (edge_point->block != block->xi)
            return value->block != block->xi &&
                   value_operand_key(context, function, block, value, key_out);
        if (value->block == block->xi) {
            bool entry_value = value == &function->capture_receiver;
            for (uint16_t parameter = 0u;
                 !entry_value && function->xi->params && parameter < function->xi->nparams;
                 ++parameter)
                entry_value = function->xi->params[parameter] == value;
            for (const XiPhi *phi = block->xi->phis; !entry_value && phi; phi = phi->next)
                entry_value = &phi->value == value;
            uint32_t point_index = UINT32_MAX;
            uint32_t value_index = UINT32_MAX;
            for (uint32_t index = 0u; index < block->xi->nvalues; ++index) {
                const XiValue *candidate = block->xi->values[index];
                if (candidate == edge_point)
                    point_index = index;
                if (candidate == value)
                    value_index = index;
            }
            if (!entry_value && (point_index == UINT32_MAX || value_index == UINT32_MAX ||
                                 value_index >= point_index))
                return false;
        }
    }
    return value_operand_key(context, function, block, value, key_out);
}

static XrProgramBuildStatus add_constant(XrXiBuildContext *context, XrXiModuleStorage *module,
                                         const XiValue *value, XrCoreIrKey *constant_key_out,
                                         char *diagnostic, size_t diagnostic_size) {
    uint16_t type_id = XR_CORE_TYPE_VOID;
    if (!map_type(context, value->type, &type_id) ||
        (type_id != XR_CORE_TYPE_I64 && type_id != XR_CORE_TYPE_BOOL))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi constant v%u has no active CoreSpec type", value->id);
    XrCoreIrKey key =
        constant_key(module->source_authority->module_identity, type_id, value->aux_int);
    for (uint32_t index = 0; index < module->constant_count; ++index) {
        if (xr_core_ir_key_equal(module->constants[index].key, key)) {
            *constant_key_out = key;
            return XR_PROGRAM_BUILD_OK;
        }
    }
    if (module->constant_count == module->constant_capacity) {
        uint32_t capacity = module->constant_capacity ? module->constant_capacity * 2u : 8u;
        size_t allocation_size = (size_t) capacity * sizeof(XrCoreIrConstantInput);
        if (capacity < module->constant_count ||
            allocation_size / sizeof(XrCoreIrConstantInput) != capacity)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        XrCoreIrConstantInput *constants = xr_realloc(module->constants, allocation_size);
        if (!constants)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        module->constants = constants;
        module->constant_capacity = capacity;
    }
    XrCoreIrConstantInput *constant = &module->constants[module->constant_count++];
    memset(constant, 0, sizeof(*constant));
    constant->key = key;
    constant->type_id = type_id;
    if (type_id == XR_CORE_TYPE_I64) {
        constant->kind = XR_CORE_IR_CONSTANT_I64;
        constant->value.i64 = value->aux_int;
    } else {
        constant->kind = XR_CORE_IR_CONSTANT_BOOL;
        constant->value.boolean = value->aux_int != 0;
    }
    *constant_key_out = key;
    return XR_PROGRAM_BUILD_OK;
}

static void free_instruction_input(XrCoreIrInstructionInput *instruction) {
    xr_free((void *) instruction->operands);
    xr_free((void *) instruction->successors);
}

static void free_context(XrXiBuildContext *context) {
    if (!context)
        return;
    for (uint32_t module_index = 0;
         context->storage && module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        uint32_t function_count = module->function_count;
        for (uint32_t function_index = 0;
             module->function_storage && function_index < function_count; ++function_index) {
            XrXiFunctionStorage *function = &module->function_storage[function_index];
            for (uint32_t block_index = 0;
                 function->block_storage && function->xi && block_index < function->xi->nblocks;
                 ++block_index) {
                XrXiBlockStorage *block = &function->block_storage[block_index];
                for (uint32_t instruction = 0;
                     block->instructions && instruction < block->instruction_count; ++instruction)
                    free_instruction_input(&block->instructions[instruction]);
                xr_free(block->instructions);
                xr_free(block->arguments);
                xr_free(block->argument_storage);
                xr_free(block->reconstructed_places);
                xr_free(block->emission_spans);
            }
            xr_free(function->block_storage);
            for (uint32_t safepoint = 0u;
                 function->cancel_graphs &&
                 safepoint < module->functions[function_index].coroutine_safepoint_count;
                 ++safepoint) {
                XrXiCancelGraphStorage *cancel = &function->cancel_graphs[safepoint];
                for (uint32_t block = 0u; block < cancel->block_count; ++block) {
                    for (uint32_t instruction = 0u;
                         instruction < cancel->blocks[block].instruction_count; ++instruction)
                        free_instruction_input((XrCoreIrInstructionInput *) &cancel->blocks[block]
                                                   .instructions[instruction]);
                    xr_free((void *) cancel->blocks[block].instructions);
                    xr_free((void *) cancel->blocks[block].arguments);
                }
                xr_free(cancel->blocks);
            }
            xr_free(function->cancel_graphs);
            xr_free(function->blocks);
            for (uint32_t projection_index = 0u;
                 projection_index < function->cleanup_projection_count; ++projection_index) {
                XrXiCleanupProjectionStorage *cleanup =
                    &function->cleanup_projections[projection_index];
                for (uint32_t instruction = 0u;
                     cleanup->trap_instructions && instruction < cleanup->trap_instruction_count;
                     ++instruction)
                    free_instruction_input(&cleanup->trap_instructions[instruction]);
                xr_free(cleanup->trap_instructions);
                xr_free(cleanup->trap_arguments);
                xr_free(cleanup->trap_argument_sources);
            }
            xr_free(function->cleanup_projections);
            xr_free(function->cleanup_points);
            xr_free(function->cleanup_chain_nodes);
            xr_free(function->cleanup_handler_chain_nodes);
            xr_free(function->cleanup_registrations);
            xr_free(function->cleanup_registration_by_value);
            xr_free(function->cleanup_state_before_value);
            xr_free(function->cleanup_handler_state_before_value);
            xr_free(function->parameter_types);
            xr_free(function->parameter_modes);
            for (uint32_t safepoint = 0u;
                 function->coroutine_safepoints &&
                 safepoint < module->functions[function_index].coroutine_safepoint_count;
                 ++safepoint)
                xr_free((void *) function->coroutine_safepoints[safepoint].live_values);
            xr_free(function->coroutine_safepoints);
            xr_free(function->coroutine_states);
        }
        xr_free(module->function_storage);
        xr_free(module->functions);
        xr_free(module->function_reachable);
        xr_free(module->xi_functions);
        xr_free(module->constants);
    }
    xr_free(context->storage);
    xr_free(context->modules);
    for (uint32_t type = 0; type < context->type_count; ++type) {
        for (uint32_t variant = 0; variant < context->type_storage[type].input.variant_count;
             ++variant)
            xr_free(context->type_storage[type].variant_payload_types[variant]);
        xr_free(context->type_storage[type].variant_payload_types);
        xr_free(context->type_storage[type].variants);
        xr_free(context->type_storage[type].field_types);
        xr_free(context->type_storage[type].callable_parameter_types);
        xr_free(context->type_storage[type].callable_parameter_modes);
        xr_free(context->type_storage[type].callable_signature);
    }
    xr_free(context->type_storage);
    xr_free(context->types);
    for (uint32_t interface_index = 0u; interface_index < context->interface_count;
         ++interface_index) {
        XrXiInterfaceStorage *storage = &context->interface_storage[interface_index];
        for (uint32_t slot = 0u; slot < context->interfaces[interface_index].slot_count; ++slot) {
            xr_free(storage->parameter_types[slot]);
            xr_free(storage->parameter_modes[slot]);
        }
        xr_free(storage->parameter_types);
        xr_free(storage->parameter_modes);
        xr_free(storage->slots);
    }
    for (uint32_t conformance = 0u; conformance < context->conformance_count; ++conformance)
        xr_free(context->conformance_storage[conformance].slot_functions);
    xr_free(context->interface_storage);
    xr_free(context->interfaces);
    xr_free(context->conformance_storage);
    xr_free(context->conformances);
    for (uint32_t requirement = 0u; requirement < context->provider_requirement_count;
         ++requirement)
        xr_free((void *) context->provider_requirements[requirement].operation_ids);
    xr_free(context->provider_requirements);
    xr_free(context->provider_operation_capacities);
    xr_free(context->trap_edges);
    xr_free(context->panic_edges);
    xr_free(context->cancel_edges);
}

static XrProgramBuildStatus set_operands(const XrXiBuildContext *context,
                                         XrCoreIrInstructionInput *instruction,
                                         const XrXiFunctionStorage *function,
                                         const XrXiBlockStorage *block, XiValue *const *values,
                                         uint32_t value_count, char *diagnostic,
                                         size_t diagnostic_size) {
    if (value_count == 0)
        return XR_PROGRAM_BUILD_OK;
    XrCoreIrKey *operands = xr_calloc(value_count, sizeof(*operands));
    if (!operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    for (uint32_t index = 0; index < value_count; ++index) {
        if (!value_operand_key(context, function, block, values[index], &operands[index])) {
            xr_free(operands);
            return fail(
                diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                "Xi function %s Core operation %u operand %u (Xi v%u op=%u kind=%u) "
                "cannot be represented by active CoreSpec",
                function && function->xi && function->xi->name ? function->xi->name : "<anonymous>",
                instruction ? instruction->operation_id : 0u, index,
                values[index] ? values[index]->id : 0u, values[index] ? values[index]->op : 0u,
                values[index] && values[index]->type ? (unsigned) values[index]->type->kind
                                                     : UINT32_MAX);
        }
    }
    instruction->operands = operands;
    instruction->operand_count = value_count;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
set_trap_edge_operands(const XrXiBuildContext *context, XrCoreIrInstructionInput *instruction,
                       const XrXiFunctionStorage *function, const XrXiBlockStorage *source,
                       const XiValue *call, XiValue *const *provider_arguments,
                       uint32_t provider_argument_count, const XrXiBlockStorage *handler,
                       char *diagnostic, size_t diagnostic_size) {
    if (!handler || provider_argument_count > UINT32_MAX - handler->argument_count)
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    uint32_t count = provider_argument_count + handler->argument_count;
    XrCoreIrKey *operands = count ? xr_calloc(count, sizeof(*operands)) : NULL;
    if (count && !operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t cursor = 0u;
    for (uint32_t argument = 0u; argument < provider_argument_count; ++argument) {
        if (!value_operand_key(context, function, source, provider_arguments[argument],
                               &operands[cursor++])) {
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi trap-capable call argument %u is unavailable", argument);
        }
    }
    for (uint32_t argument = 0u; argument < handler->argument_count; ++argument) {
        const XrXiBlockArgumentStorage *edge_argument = &handler->argument_storage[argument];
        if (edge_argument->phi ||
            edge_argument->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE ||
            !edge_value_operand_key(context, function, source, edge_argument->source, call,
                                    &operands[cursor++])) {
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi trap continuation argument %u is unavailable", argument);
        }
    }
    XrCoreIrKey *successors = xr_calloc(1u, sizeof(*successors));
    if (!successors) {
        xr_free(operands);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    successors[0] = block_key(function, handler->xi);
    instruction->operands = operands;
    instruction->operand_count = count;
    instruction->successors = successors;
    instruction->successor_count = 1u;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
set_panic_edge_operands(const XrXiBuildContext *context, XrCoreIrInstructionInput *instruction,
                        const XrXiFunctionStorage *function, const XrXiBlockStorage *source,
                        const XiValue *point, const XiValue *condition,
                        const XrXiBlockStorage *handler, char *diagnostic, size_t diagnostic_size) {
    if (!handler || handler->argument_count == 0u ||
        handler->argument_storage[0].implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_PANIC)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi panic continuation lacks its implicit PanicInfo payload");
    uint32_t count = handler->argument_count;
    XrCoreIrKey *operands = xr_calloc(count, sizeof(*operands));
    if (!operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (!value_operand_key(context, function, source, condition, &operands[0])) {
        xr_free(operands);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi assertion condition is unavailable");
    }
    uint32_t cursor = 1u;
    for (uint32_t argument = 1u; argument < handler->argument_count; ++argument) {
        const XrXiBlockArgumentStorage *edge_argument = &handler->argument_storage[argument];
        if (edge_argument->phi ||
            edge_argument->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE ||
            !edge_value_operand_key(context, function, source, edge_argument->source, point,
                                    &operands[cursor++])) {
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi panic continuation argument %u is unavailable", argument);
        }
    }
    XrCoreIrKey *successors = xr_calloc(1u, sizeof(*successors));
    if (!successors) {
        xr_free(operands);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    successors[0] = block_key(function, handler->xi);
    instruction->operands = operands;
    instruction->operand_count = count;
    instruction->successors = successors;
    instruction->successor_count = 1u;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
translate_call(XrXiBuildContext *context, const XrXiModuleStorage *module,
               XrXiFunctionStorage *function, const XiValue *value, const XrXiBlockStorage *block,
               const XrProgramXiProjection *projection, XrCoreIrInstructionInput *instruction,
               char *diagnostic, size_t diagnostic_size) {
    (void) module;
    const XgCallsiteSummary *callsite = resolved_callsite(context, function->xi, value);
    if (!callsite)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi call v%u lacks matching global callsite evidence", value->id);
    XiValue *const *field_values = NULL;
    uint32_t field_count = 0u;
    if (resolved_canonical_class_construction(context, function->xi, value, &field_values,
                                              &field_count)) {
        uint16_t result_type = XR_CORE_TYPE_VOID;
        if (!map_type(context, value->type, &result_type))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi class construction v%u has no active nominal type", value->id);
        const XrXiTypeStorage *aggregate = find_dynamic_type_by_id(context, result_type);
        if (!aggregate || aggregate->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
            aggregate->input.field_count != field_count ||
            aggregate->input.nominal_kind == XR_CORE_IR_NOMINAL_NONE)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi class construction v%u is not an exact nominal aggregate", value->id);
        instruction->operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT;
        instruction->result = value_key(function, value);
        instruction->result_type_id = result_type;
        instruction->result_ownership = logical_ownership_for_type(context, result_type);
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        return set_operands(context, instruction, function, block, field_values, field_count,
                            diagnostic, diagnostic_size);
    }
    const XrStdlibDefEntry *provider_entry =
        resolved_provider_native_call(context, function->xi, value);
    if (provider_entry) {
        uint16_t result_type = XR_CORE_TYPE_VOID;
        if (!map_type(context, value->type, &result_type) ||
            value->nargs != (uint16_t) (provider_entry->argc + 1u) ||
            !((result_type == XR_CORE_TYPE_I64 && provider_entry->argc <= 1u) ||
              (result_type == XR_CORE_TYPE_BOOL && provider_entry->argc == 1u) ||
              (provider_entry->argc == 0u &&
               logical_type_is_optional_i64_pair(context, result_type))))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi provider call v%u has no admitted logical call shape", value->id);
        for (uint16_t argument = 0u; argument < provider_entry->argc; ++argument) {
            uint16_t argument_type = XR_CORE_TYPE_VOID;
            if (!map_logical_value_type(context, function->xi, value->args[argument + 1u],
                                        &argument_type) ||
                argument_type != XR_CORE_TYPE_I64)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi provider call v%u argument %u is not i64", value->id, argument);
        }
        XrStableId contract_id = {{0}};
        XrStableId operation_id = {{0}};
        XrProgramBuildStatus requirement_status = require_provider_operation(
            context, provider_entry->provider_contract_key, provider_entry->provider_operation_key,
            &contract_id, &operation_id);
        if (requirement_status != XR_PROGRAM_BUILD_OK)
            return requirement_status;
        const XrXiTrapEdge *trap_edge = find_trap_edge(context, function->xi, value);
        instruction->operation_id = XR_CORE_OP_CORE_PROVIDER_CALL;
        instruction->result = value_key(function, value);
        instruction->result_type_id = result_type;
        instruction->result_ownership = logical_ownership_for_type(context, result_type);
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION;
        instruction->immediate.provider_operation.contract_id = contract_id;
        instruction->immediate.provider_operation.operation_id = operation_id;
        if (!trap_edge)
            return set_operands(context, instruction, function, block, value->args + 1u,
                                provider_entry->argc, diagnostic, diagnostic_size);
        const XrXiBlockStorage *handler = find_block_storage(function, trap_edge->handler);
        return set_trap_edge_operands(context, instruction, function, block, value,
                                      value->args + 1u, provider_entry->argc, handler, diagnostic,
                                      diagnostic_size);
    }
    if (callsite->kind != XG_CALL_DIRECT_FUNC && callsite->kind != XG_CALL_CLOSURE &&
        callsite->kind != XG_CALL_METHOD) {
        const XiCoroSuspendPoint *point =
            value->block ? coroutine_point_for_block(function, value->block) : NULL;
        const XiFunc *resolved = resolved_sealed_callee(context, function->xi, value);
        const XiImportRef *reference =
            value->nargs && value->args ? xi_value_import_ref(function->xi, value->args[0]) : NULL;
        return fail(
            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
            "Xi call %s:v%u in b%u uses unsupported global callsite kind %u "
            "(op=%u/call=%u suspension=%u:v%u callee=%s flags=0x%x nargs=%u argc=%u "
            "native=%u method=%u metadata=%u target=%s.%s)",
            function->xi->name ? function->xi->name : "<anonymous>", value->id,
            value->block ? value->block->id : UINT32_MAX, callsite->kind, (unsigned) value->op,
            value->op == XI_CALL ? 1u : 0u, point ? (unsigned) point->kind : 0u,
            point && point->op ? point->op->id : UINT32_MAX,
            resolved && resolved->name ? resolved->name : "<none>", (unsigned) callsite->flags,
            (unsigned) value->nargs, (unsigned) callsite->arg_count,
            xi_import_ref_is_grounded_native(reference) ? 1u : 0u,
            reference && callsite->method_id == (XgMethodId) xg_name_id(reference->member_name)
                ? 1u
                : 0u,
            reference && xr_stdlib_metadata_exact_native_suspension_call(
                             reference->module_path, reference->member_name, callsite->arg_count)
                ? 1u
                : 0u,
            reference && reference->module_path ? reference->module_path : "<none>",
            reference && reference->member_name ? reference->member_name : "<none>");
    }
    if ((callsite->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi call v%u lacks verified callsite error evidence", value->id);
    const XiFunc *callee = resolved_sealed_callee(context, function->xi, value);
    uint32_t callee_module_index = UINT32_MAX;
    uint32_t callee_function_index = UINT32_MAX;
    const XrXiFunctionStorage *callee_storage =
        find_xi_function(context, callee, &callee_module_index, &callee_function_index);
    if (!callee && callsite->kind == XG_CALL_METHOD)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi method call v%u has no exact class/method/body join", value->id);
    if (!callee) {
        if ((callsite->flags & XG_CALL_MAY_ERROR) != 0u)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi indirect call v%u requires an explicit error continuation", value->id);
        const XiValue *callee_value = value->nargs ? logical_value_identity(value->args[0]) : NULL;
        uint16_t callable_type_id = XR_CORE_TYPE_VOID;
        if (!callee_value || !map_callable_call_type(context, function->xi, value,
                                                     callee_value->type, &callable_type_id, NULL))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi call v%u has no closed typed callable target set", value->id);
        const XrXiTypeStorage *callable = find_dynamic_type_by_id(context, callable_type_id);
        if (!callable || callable->input.kind != XR_CORE_IR_TYPE_CALLABLE ||
            !callable->callable_signature ||
            callable->callable_signature->error_type_id != XR_CORE_TYPE_VOID ||
            callable->callable_signature->panic_type_id != XR_CORE_TYPE_VOID)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi call v%u requires an unsupported callable continuation", value->id);
        uint16_t result_type = XR_CORE_TYPE_VOID;
        if (!map_type(context, value->type, &result_type) ||
            result_type != callable->callable_signature->result_type_id)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi call v%u result disagrees with its callable signature", value->id);
        instruction->operation_id = XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT;
        instruction->result_type_id = result_type;
        instruction->result_ownership = logical_ownership_for_type(context, result_type);
        if (result_type != XR_CORE_TYPE_VOID)
            instruction->result = value_key(function, value);
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        const XrXiTrapEdge *trap_edge = find_trap_edge(context, function->xi, value);
        if (!trap_edge)
            return set_operands(context, instruction, function, block, value->args, value->nargs,
                                diagnostic, diagnostic_size);
        const XrXiBlockStorage *handler = find_block_storage(function, trap_edge->handler);
        return set_trap_edge_operands(context, instruction, function, block, value, value->args,
                                      value->nargs, handler, diagnostic, diagnostic_size);
    }
    if (!callee_storage)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                    "Xi call v%u names a function outside the canonical graph", value->id);
    uint16_t error_type = XR_CORE_TYPE_VOID;
    XrProgramBuildStatus error_status =
        map_function_error_type(context, callee, &error_type, diagnostic, diagnostic_size);
    if (error_status != XR_PROGRAM_BUILD_OK)
        return error_status;
    if (error_type != XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "fallible Xi call v%u lacks an explicit invoke continuation", value->id);
    uint16_t panic_type = XR_CORE_TYPE_VOID;
    XrProgramBuildStatus panic_status =
        map_function_panic_type(context, callee, &panic_type, diagnostic, diagnostic_size);
    if (panic_status != XR_PROGRAM_BUILD_OK)
        return panic_status;

    const XrCoreIrFunctionInput *callee_contract =
        &context->storage[callee_module_index].functions[callee_function_index];
    uint16_t result_type = callee_contract->result_type_id;
    bool result_matches =
        value->type && callee->return_type && xr_type_equals(value->type, callee->return_type);
    if (!result_matches)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi call v%u result type disagrees with its sealed callee", value->id);
    instruction->operation_id = projection->core_operation_id;
    instruction->result_type_id = result_type;
    instruction->result_ownership = logical_ownership_for_type(context, result_type);
    if (result_type != XR_CORE_TYPE_VOID)
        instruction->result = value_key(function, value);
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
    instruction->immediate.key = callee_storage->key;
    uint16_t first_operand = callee->has_receiver ? 0u : 1u;
    const XrXiTrapEdge *trap_edge = find_trap_edge(context, function->xi, value);
    if (!trap_edge)
        return set_operands(context, instruction, function, block, value->args + first_operand,
                            value->nargs - first_operand, diagnostic, diagnostic_size);
    const XrXiBlockStorage *handler = find_block_storage(function, trap_edge->handler);
    return set_trap_edge_operands(context, instruction, function, block, value,
                                  value->args + first_operand, value->nargs - first_operand,
                                  handler, diagnostic, diagnostic_size);
}

static XrProgramBuildStatus translate_capture_construct(XrXiBuildContext *context,
                                                        XrXiFunctionStorage *function,
                                                        const XiValue *closure,
                                                        const XrXiBlockStorage *block,
                                                        XrCoreIrInstructionInput *instruction,
                                                        char *diagnostic, size_t diagnostic_size) {
    const XiFunc *target = resolved_callable_target(function->xi, closure);
    uint16_t capture_type_id = XR_CORE_TYPE_VOID;
    if (!target || target->ncaptures == 0u || closure->nargs != target->ncaptures ||
        !map_capture_type(context, target, &capture_type_id))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi callable pack v%u has no canonical capture aggregate", closure->id);
    const XrXiTypeStorage *capture = find_dynamic_type_by_id(context, capture_type_id);
    if (!capture || capture->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
        capture->input.field_count != closure->nargs)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi callable pack v%u capture aggregate is inconsistent", closure->id);
    for (uint32_t field = 0; field < closure->nargs; ++field) {
        uint16_t field_type = XR_CORE_TYPE_VOID;
        if (!closure->args[field] || !map_type(context, closure->args[field]->type, &field_type) ||
            field_type != capture->input.field_types[field])
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi callable pack v%u capture %u has an invalid type", closure->id, field);
    }
    memset(instruction, 0, sizeof(*instruction));
    instruction->operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT;
    instruction->result = closure_capture_key(function, closure);
    instruction->result_type_id = capture_type_id;
    instruction->result_ownership = XR_CORE_IR_OWNER;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
    return set_operands(context, instruction, function, block, closure->args, closure->nargs,
                        diagnostic, diagnostic_size);
}

static XrProgramBuildStatus translate_callable_pack(XrXiBuildContext *context,
                                                    XrXiFunctionStorage *function,
                                                    const XiValue *value,
                                                    XrCoreIrInstructionInput *instruction,
                                                    char *diagnostic, size_t diagnostic_size) {
    uint16_t callable_type_id = XR_CORE_TYPE_VOID;
    const XiFunc *target = resolved_callable_target(function->xi, value);
    const XrXiFunctionStorage *target_storage = find_xi_function(context, target, NULL, NULL);
    if (!target || !target_storage ||
        !map_callable_target_type(context, value->type, target, &callable_type_id))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi callable pack v%u has no exact target or signature", value->id);
    const XrXiTypeStorage *callable = find_dynamic_type_by_id(context, callable_type_id);
    if (!callable || callable->input.kind != XR_CORE_IR_TYPE_CALLABLE ||
        !callable->callable_signature || value->nargs != (target ? target->ncaptures : 0u))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi callable pack v%u has an inconsistent capture shape", value->id);
    instruction->operation_id = XR_CORE_OP_CORE_CALLABLE_PACK;
    instruction->result = value_key(function, value);
    instruction->result_type_id = callable_type_id;
    instruction->result_ownership = value->nargs == 0u ? XR_CORE_IR_NON_OWNER : XR_CORE_IR_OWNER;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
    instruction->immediate.key = target_storage->key;
    if (value->nargs != 0u) {
        XrCoreIrKey *capture = xr_calloc(1u, sizeof(*capture));
        if (!capture)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        *capture = closure_capture_key(function, value);
        instruction->operands = capture;
        instruction->operand_count = 1u;
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
translate_shared_callable_pack(XrXiBuildContext *context, XrXiFunctionStorage *function,
                               const XiValue *value, XrCoreIrInstructionInput *instruction,
                               char *diagnostic, size_t diagnostic_size) {
    uint64_t signature_key = 0u;
    const XrType *visible_type = NULL;
    const XiFunc *target = resolved_shared_callable_target(context, function->xi, value,
                                                           &signature_key, &visible_type);
    const XrXiFunctionStorage *target_storage = find_xi_function(context, target, NULL, NULL);
    uint16_t callable_type_id = XR_CORE_TYPE_VOID;
    if (!target || !target_storage || !visible_type || target->ncaptures != 0u ||
        !map_callable_signature_contract(context, visible_type, signature_key, &callable_type_id,
                                         NULL))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                    "Xi shared callable v%u has no exact target or signature", value->id);
    const XrXiTypeStorage *callable = find_dynamic_type_by_id(context, callable_type_id);
    if (!callable || callable->input.kind != XR_CORE_IR_TYPE_CALLABLE ||
        !callable->callable_signature)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi shared callable v%u has an inconsistent callable type", value->id);
    instruction->operation_id = XR_CORE_OP_CORE_CALLABLE_PACK;
    instruction->result = value_key(function, value);
    instruction->result_type_id = callable_type_id;
    instruction->result_ownership = XR_CORE_IR_NON_OWNER;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
    instruction->immediate.key = target_storage->key;
    return XR_PROGRAM_BUILD_OK;
}

static const XrCoreIrConformanceInput *
conformance_input_by_id(const XrXiBuildContext *context, XgInterfaceConformanceId conformance_id) {
    for (uint32_t index = 0u; context && index < context->conformance_count; ++index)
        if (context->conformance_storage[index].conformance_id == conformance_id)
            return &context->conformances[index];
    return NULL;
}

static XrProgramBuildStatus translate_existential_value(XrXiBuildContext *context,
                                                        XrXiFunctionStorage *function,
                                                        const XiValue *value,
                                                        const XrXiBlockStorage *block,
                                                        XrCoreIrInstructionInput *instruction,
                                                        char *diagnostic, size_t diagnostic_size) {
    XrProgramXiSemanticProjection projection;
    if (!xr_program_xi_semantic_projection(value->op, value->xg_existential_kind, &projection))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi existential v%u has no generated semantic projection", value->id);
    uint16_t expected_operation = 0u;
    switch (projection.kind) {
        case XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_PACK:
            expected_operation = XR_CORE_OP_CORE_EXISTENTIAL_PACK;
            break;
        case XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_REBORROW_READ:
            expected_operation = XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ;
            break;
        case XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_TEST:
            expected_operation = XR_CORE_OP_CORE_EXISTENTIAL_TEST;
            break;
        case XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_PROJECT:
            expected_operation = XR_CORE_OP_CORE_EXISTENTIAL_PROJECT;
            break;
        case XR_PROGRAM_XI_SEMANTIC_WITNESS_DIRECT:
            expected_operation = XR_CORE_OP_CORE_CALL_WITNESS_DIRECT;
            break;
        case XR_PROGRAM_XI_SEMANTIC_WITNESS_INVOKE:
            expected_operation = XR_CORE_OP_CORE_CALL_WITNESS_INVOKE;
            break;
        default:
            break;
    }
    if (expected_operation == 0u || projection.core_operation_id != expected_operation)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi existential v%u generated a mismatched canonical operation", value->id);
    memset(instruction, 0, sizeof(*instruction));
    instruction->operation_id = projection.core_operation_id;
    if (projection.kind == XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_REBORROW_READ) {
        uint16_t source_type_id = XR_CORE_TYPE_VOID;
        uint16_t result_type_id = XR_CORE_TYPE_VOID;
        if (!existential_reborrow_contract_is_exact(context, function->xi, value) ||
            !map_logical_value_type(context, function->xi, value->args[0], &source_type_id) ||
            !map_logical_value_type(context, function->xi, value, &result_type_id))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi existential READ reborrow v%u has no canonical carrier types",
                        value->id);
        const XrXiTypeStorage *source_type = find_dynamic_type_by_id(context, source_type_id);
        const XrXiTypeStorage *result_type = find_dynamic_type_by_id(context, result_type_id);
        if (!source_type || !result_type ||
            source_type->input.kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
            result_type->input.kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
            !xr_core_ir_key_equal(source_type->input.existential_interface,
                                  result_type->input.existential_interface) ||
            (source_type->input.interface_use_kind != XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE &&
             source_type->input.interface_use_kind !=
                 XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) ||
            result_type->input.interface_use_kind != XR_CORE_IR_INTERFACE_EXISTENTIAL_READ)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi existential READ reborrow v%u changes its interface identity",
                        value->id);
        instruction->result = value_key(function, value);
        instruction->result_type_id = result_type_id;
        instruction->result_category = XR_CORE_IR_VALUE;
        instruction->result_ownership = XR_CORE_IR_NON_OWNER;
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                            diagnostic_size);
    }
    if (projection.kind == XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_PACK ||
        projection.kind == XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_TEST ||
        projection.kind == XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_PROJECT) {
        const XgInterfaceImplSummary *implementor =
            find_conformance_by_id(context->source->global_evidence, value->xg_conformance_id);
        const XrCoreIrConformanceInput *conformance =
            conformance_input_by_id(context, value->xg_conformance_id);
        if (!existential_value_contract_is_exact(context, function->xi, implementor, value) ||
            !conformance ||
            !xr_core_ir_key_equal(conformance->interface_key,
                                  interface_key(value->xg_interface_id)) ||
            conformance->implementor_kind != core_nominal_kind_from_xg(value->xg_implementor_kind))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi existential v%u does not select one canonical conformance", value->id);

        if (projection.kind == XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_PACK) {
            uint16_t result_type = XR_CORE_TYPE_VOID;
            uint16_t operand_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 1u || !value->args || !value->args[0] ||
                !map_existential_interface_id(context, value->xg_interface_id,
                                              (XiInterfaceUseKind) value->xg_interface_use_kind,
                                              &result_type) ||
                !map_logical_value_type(context, function->xi, value->args[0], &operand_type) ||
                operand_type != conformance->implementor_type_id)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi existential pack v%u has an inconsistent operand type", value->id);
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->result_ownership = logical_ownership_for_type(context, result_type);
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }

        uint16_t operand_type = XR_CORE_TYPE_VOID;
        if (value->nargs < 1u || !value->args || !value->args[0] ||
            !map_logical_value_type(context, function->xi, value->args[0], &operand_type))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi existential v%u has no typed carrier operand", value->id);
        const XrXiTypeStorage *carrier = find_dynamic_type_by_id(context, operand_type);
        if (!carrier || carrier->input.kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
            !xr_core_ir_key_equal(carrier->input.existential_interface,
                                  conformance->interface_key) ||
            carrier->input.interface_use_kind !=
                (XrCoreIrInterfaceUseKind) value->xg_interface_use_kind)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi existential v%u carrier identity is inconsistent", value->id);
        instruction->result = value_key(function, value);
        instruction->result_category = XR_CORE_IR_VALUE;
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_TYPE;
        instruction->immediate.type_id = conformance->implementor_type_id;
        if (projection.kind == XR_PROGRAM_XI_SEMANTIC_EXISTENTIAL_TEST) {
            if (value->nargs != 2u)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi existential test v%u has invalid arity", value->id);
            instruction->result_type_id = XR_CORE_TYPE_BOOL;
            instruction->result_ownership = XR_CORE_IR_NON_OWNER;
        } else {
            if (value->nargs != 1u)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi existential project v%u has invalid arity", value->id);
            instruction->result_type_id = conformance->implementor_type_id;
            instruction->result_category = value->xg_interface_use_kind == XI_INTERFACE_USE_REF
                                               ? XR_CORE_IR_PLACE
                                               : XR_CORE_IR_VALUE;
            instruction->result_ownership =
                value->xg_interface_use_kind == XI_INTERFACE_USE_MOVE ||
                        value->xg_interface_use_kind == XI_INTERFACE_USE_OWNED_STORAGE
                    ? logical_ownership_for_type(context, conformance->implementor_type_id)
                    : XR_CORE_IR_NON_OWNER;
        }
        return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                            diagnostic_size);
    }

    if (projection.kind == XR_PROGRAM_XI_SEMANTIC_WITNESS_INVOKE)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi witness invoke v%u must be emitted as a CFG terminal", value->id);
    const XgCallsiteSummary *callsite = resolved_witness_callsite(context, function->xi, value);
    const XrCoreIrCallableSignatureInput *slot =
        interface_slot_contract(context, value->xg_interface_id, value->xg_interface_dispatch_slot);
    XiInterfaceUseKind receiver_use =
        slot ? interface_use_for_receiver(slot->receiver_mode) : XI_INTERFACE_USE_NONE;
    if (!callsite || !slot || projection.kind != XR_PROGRAM_XI_SEMANTIC_WITNESS_DIRECT ||
        slot->error_type_id != XR_CORE_TYPE_VOID || slot->panic_type_id != XR_CORE_TYPE_VOID ||
        !interface_use_allows_receiver((XiInterfaceUseKind) value->xg_interface_use_kind,
                                       receiver_use) ||
        value->nargs != slot->parameter_count)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi witness direct v%u has no exact infallible interface slot", value->id);
    for (uint32_t parameter = 0u; parameter < slot->parameter_count; ++parameter) {
        uint16_t argument_type = XR_CORE_TYPE_VOID;
        bool argument_mapped =
            map_logical_value_type(context, function->xi, value->args[parameter], &argument_type);
        bool argument_type_matches =
            argument_mapped &&
            (parameter == 0u ? mapped_existential_receiver_matches(context, argument_type,
                                                                   slot->parameter_types[parameter],
                                                                   receiver_use)
                             : argument_type == slot->parameter_types[parameter]);
        if (!argument_type_matches)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi witness direct v%u argument %u disagrees with its slot "
                        "(mapped=%u actual_type=%u expected_type=%u interface=%u "
                        "argument_interface=%u call_use=%u argument_use=%u)",
                        value->id, parameter, argument_mapped ? 1u : 0u, argument_type,
                        slot->parameter_types[parameter], value->xg_interface_id,
                        value->args[parameter] && value->args[parameter]->type &&
                                value->args[parameter]->type->kind == XR_KIND_INTERFACE &&
                                value->args[parameter]->type->instance.class_ref
                            ? value->args[parameter]->type->instance.class_ref->xg_interface_id
                            : XG_NO_ID,
                        value->xg_interface_use_kind,
                        value->args[parameter] ? value->args[parameter]->xg_interface_use_kind
                                               : XI_INTERFACE_USE_NONE);
    }
    uint16_t result_type = XR_CORE_TYPE_VOID;
    if (!map_type(context, value->type, &result_type) || result_type != slot->result_type_id)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi witness direct v%u result disagrees with its slot", value->id);
    if (result_type != XR_CORE_TYPE_VOID)
        instruction->result = value_key(function, value);
    instruction->result_type_id = result_type;
    instruction->result_ownership = slot->result_ownership;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    instruction->immediate.u32 = value->xg_interface_dispatch_slot;
    const XrXiTrapEdge *trap_edge = find_trap_edge(context, function->xi, value);
    if (!trap_edge)
        return set_operands(context, instruction, function, block, value->args, value->nargs,
                            diagnostic, diagnostic_size);
    const XrXiBlockStorage *handler = find_block_storage(function, trap_edge->handler);
    return set_trap_edge_operands(context, instruction, function, block, value, value->args,
                                  value->nargs, handler, diagnostic, diagnostic_size);
}

static XrProgramBuildStatus
translate_existential_owner_copy(XrXiBuildContext *context, XrXiFunctionStorage *function,
                                 const XiValue *pack, const XrXiBlockStorage *block,
                                 XrCoreIrInstructionInput *instruction, bool *emitted,
                                 char *diagnostic, size_t diagnostic_size) {
    if (emitted)
        *emitted = false;
    if (!context || !function || !pack || !instruction || !emitted ||
        pack->xg_existential_kind != XI_EXISTENTIAL_PACK ||
        pack->xg_interface_use_kind != XI_INTERFACE_USE_OWNED_STORAGE)
        return XR_PROGRAM_BUILD_OK;
    if (pack->nargs != 1u || !pack->args || !pack->args[0])
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi existential pack v%u has no concrete source", pack->id);

    uint16_t source_type = XR_CORE_TYPE_VOID;
    if (!map_logical_value_type(context, function->xi, pack->args[0], &source_type) ||
        source_type == XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi existential pack v%u source has no canonical type", pack->id);
    if (logical_ownership_for_type(context, source_type) != XR_CORE_IR_OWNER ||
        logical_value_produces_owner(context, function->xi, pack->args[0], 0u))
        return XR_PROGRAM_BUILD_OK;
    if (logical_copy_contract_for_type(context, source_type) != XR_CORE_IR_COPY_EXPLICIT)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi existential pack v%u cannot acquire owned storage from a borrow", pack->id);

    memset(instruction, 0, sizeof(*instruction));
    instruction->operation_id = XR_CORE_OP_CORE_OWNER_COPY;
    instruction->result = existential_owner_copy_key(function, pack);
    instruction->result_type_id = source_type;
    instruction->result_category = XR_CORE_IR_VALUE;
    instruction->result_ownership = XR_CORE_IR_OWNER;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
    XrProgramBuildStatus status = set_operands(context, instruction, function, block, pack->args,
                                               1u, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        *emitted = true;
    return status;
}

static XrProgramBuildStatus
translate_reconstructed_place(XrXiBuildContext *context, XrXiFunctionStorage *function,
                              const XrXiBlockStorage *block,
                              const XrXiReconstructedPlaceStorage *place,
                              XrCoreIrInstructionInput instructions[2], uint32_t *count) {
    if (!context || !function || !block || !place || !instructions || !count)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    *count = 0u;
    bool project = place->base_place != NULL;
    bool local_base = project && !logical_value_is_place(function->xi, place->base_place);
    if (project) {
        uint16_t aggregate_type = XR_CORE_TYPE_VOID;
        if (!map_logical_value_type(context, function->xi, place->base_place, &aggregate_type))
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        const XrXiTypeStorage *aggregate = find_dynamic_type_by_id(context, aggregate_type);
        if (!aggregate || aggregate->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
            place->field_ordinal >= aggregate->input.field_count ||
            aggregate->input.field_types[place->field_ordinal] != place->type_id)
            return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    XrCoreIrKey *operand = xr_calloc(1u, sizeof(*operand));
    if (!operand)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    const XiValue *source = project && !local_base ? place->base_place : place->owner;
    if (!value_operand_key(context, function, block, source, operand)) {
        xr_free(operand);
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    if (local_base) {
        uint16_t owner_type = XR_CORE_TYPE_VOID;
        if (!map_logical_value_type(context, function->xi, source, &owner_type)) {
            xr_free(operand);
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        }
        instructions[0] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
            .result = key_from_key_and_u32(UINT8_C(0x5a), place->key, 0u),
            .result_type_id = owner_type,
            .result_category = XR_CORE_IR_PLACE,
            .operands = operand,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
        *count = 1u;
        operand = xr_calloc(1u, sizeof(*operand));
        if (!operand)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        *operand = instructions[0].result;
    }
    instructions[(*count)++] = (XrCoreIrInstructionInput) {
        .operation_id = project ? XR_CORE_OP_CORE_PLACE_PROJECT : XR_CORE_OP_CORE_PLACE_LOCAL,
        .result = place->key,
        .result_type_id = place->type_id,
        .result_category = XR_CORE_IR_PLACE,
        .operands = operand,
        .operand_count = 1u,
        .immediate_kind = project ? XR_CORE_IR_IMMEDIATE_FIELD : XR_CORE_IR_IMMEDIATE_NONE,
    };
    if (project)
        instructions[*count - 1u].immediate.field_ordinal = place->field_ordinal;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
translate_aggregate_place_access(XrXiBuildContext *context, XrXiFunctionStorage *function,
                                 const XiValue *value, const XrXiBlockStorage *block,
                                 XrCoreIrInstructionInput instructions[2], char *diagnostic,
                                 size_t diagnostic_size) {
    uint32_t field_ordinal = UINT32_MAX;
    bool load = resolved_aggregate_field_projection(context, value, &field_ordinal);
    bool store =
        !load && resolved_aggregate_field_store(context, function->xi, value, &field_ordinal);
    const XiValue *place_source = load ? logical_value_identity(value->args[0])
                                       : aggregate_field_store_place(function->xi, value);
    if ((!load && !store) || !place_source ||
        logical_value_category(function->xi, place_source) != XR_CORE_IR_PLACE)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi field access v%u is not an exact aggregate place access", value->id);

    uint16_t aggregate_type_id = XR_CORE_TYPE_VOID;
    uint16_t field_type_id = XR_CORE_TYPE_VOID;
    const XrType *field_type = load ? value->type : value->args[1]->type;
    if (!map_type(context, value->args[0]->type, &aggregate_type_id) ||
        !map_type(context, field_type, &field_type_id) || field_type_id == XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi field access v%u has no exact logical aggregate field type", value->id);
    const XrXiTypeStorage *aggregate = find_dynamic_type_by_id(context, aggregate_type_id);
    if (!aggregate || aggregate->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
        field_ordinal >= aggregate->input.field_count ||
        aggregate->input.field_types[field_ordinal] != field_type_id)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi field access v%u has an invalid declaration ordinal or type", value->id);

    place_source = exact_logical_value_identity(context, function->xi, place_source);
    const XrXiReconstructedPlaceStorage *reconstructed =
        find_reconstructed_place((XrXiBlockStorage *) block, place_source);
    if (reconstructed && reconstructed->type_id != aggregate_type_id)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi field access v%u reconstructed place type %u disagrees with aggregate "
                    "type %u",
                    value->id, reconstructed->type_id, aggregate_type_id);

    memset(instructions, 0, 2u * sizeof(*instructions));
    XrCoreIrInstructionInput *project = &instructions[0];
    project->operation_id = XR_CORE_OP_CORE_PLACE_PROJECT;
    project->result = aggregate_field_place_key(function, value);
    project->result_type_id = field_type_id;
    project->result_category = XR_CORE_IR_PLACE;
    project->immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD;
    project->immediate.field_ordinal = field_ordinal;
    XiValue *place_operand = (XiValue *) place_source;
    XrProgramBuildStatus status = set_operands(context, project, function, block, &place_operand,
                                               1u, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;

    XrCoreIrInstructionInput *access = &instructions[1];
    access->operation_id = load ? XR_CORE_OP_CORE_PLACE_LOAD : XR_CORE_OP_CORE_PLACE_STORE;
    access->result_type_id = load ? field_type_id : XR_CORE_TYPE_VOID;
    access->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
    if (load)
        access->result = value_key(function, value);
    uint32_t operand_count = load ? 1u : 2u;
    XrCoreIrKey *operands = xr_calloc(operand_count, sizeof(*operands));
    if (!operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    operands[0] = project->result;
    if (store && !value_operand_key(context, function, block, value->args[1], &operands[1])) {
        xr_free(operands);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi field store v%u replacement has no canonical value", value->id);
    }
    access->operands = operands;
    access->operand_count = operand_count;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus translate_value(XrXiBuildContext *context, XrXiModuleStorage *module,
                                            XrXiFunctionStorage *function, const XiValue *value,
                                            const XrXiBlockStorage *block,
                                            XrCoreIrInstructionInput *instruction, char *diagnostic,
                                            size_t diagnostic_size) {
    if (value->xg_existential_kind != XI_EXISTENTIAL_NONE)
        return translate_existential_value(context, function, value, block, instruction, diagnostic,
                                           diagnostic_size);
    memset(instruction, 0, sizeof(*instruction));
    uint16_t result_type = XR_CORE_TYPE_VOID;
    bool result_mapped = map_logical_value_type(context, function->xi, value, &result_type);
    if (!result_mapped)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %s block b%u operation %s (%u) at v%u result type kind %u is not "
                    "active in CoreSpec",
                    function->xi && function->xi->name ? function->xi->name : "<anonymous>",
                    block && block->xi ? block->xi->id : UINT32_MAX, xi_op_name(value->op),
                    value->op, value->id, value->type ? (unsigned) value->type->kind : UINT32_MAX);
    if (value->op == XI_ASSERTION) {
        uint16_t condition_type = XR_CORE_TYPE_VOID;
        if (!exact_condition_assertion(value) || result_type != XR_CORE_TYPE_VOID ||
            !map_logical_value_type(context, function->xi, value->args[0], &condition_type) ||
            condition_type != XR_CORE_TYPE_BOOL)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi assertion v%u is not an exact condition assertion", value->id);
        instruction->operation_id = XR_CORE_OP_CORE_ASSERT_CONDITION;
        instruction->result_type_id = XR_CORE_TYPE_VOID;
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
        instruction->immediate.u32 = XR_ASSERTION_FAILURE_CONDITION_FALSE;
        const XrXiPanicEdge *edge = find_panic_edge(context, function->xi, value);
        if (!edge)
            return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        const XrXiBlockStorage *handler = find_block_storage(function, edge->handler);
        return set_panic_edge_operands(context, instruction, function, block, value, value->args[0],
                                       handler, diagnostic, diagnostic_size);
    }
    if (value_is_static_typed_catch_test(value)) {
        if (!static_typed_catch_contract_is_exact(context, value))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi typed catch test v%u has inconsistent nominal token evidence",
                        value->id);
        uint16_t target_type = XR_CORE_TYPE_VOID;
        const XrXiBlockArgumentStorage *caught =
            find_block_argument((XrXiBlockStorage *) block, logical_value_identity(value->args[0]));
        if (result_type != XR_CORE_TYPE_BOOL || !caught ||
            !map_type(context, (const XrType *) value->aux, &target_type) ||
            target_type == XR_CORE_TYPE_VOID || !find_dynamic_type_by_id(context, target_type) ||
            find_dynamic_type_by_id(context, target_type)->input.kind != XR_CORE_IR_TYPE_VARIANT)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi typed catch test v%u has no exact closed error contract", value->id);
        XiValue folded = *value;
        folded.aux_int = caught->type_id == target_type ? 1 : 0;
        XrCoreIrKey constant;
        XrProgramBuildStatus status =
            add_constant(context, module, &folded, &constant, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
        instruction->operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL;
        instruction->result = value_key(function, value);
        instruction->result_type_id = XR_CORE_TYPE_BOOL;
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT;
        instruction->immediate.key = constant;
        return XR_PROGRAM_BUILD_OK;
    }
    if (value->op == XI_GET_SHARED)
        return translate_shared_callable_pack(context, function, value, instruction, diagnostic,
                                              diagnostic_size);
    uint16_t target_enum_type = XR_CORE_TYPE_VOID;
    if (value->op == XI_CONST && value->nargs == 0u && value->aux == NULL && value->aux_int > 0 &&
        value->aux_int <= UINT16_MAX && map_type(context, value->type, &target_enum_type) &&
        xr_target_query_enum_value_valid(target_enum_type, (uint16_t) value->aux_int)) {
        instruction->operation_id = XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM;
        instruction->result = value_key(function, value);
        instruction->result_type_id = target_enum_type;
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
        instruction->immediate.u32 = (uint32_t) value->aux_int;
        return XR_PROGRAM_BUILD_OK;
    }
    uint32_t unit_enum_ordinal = UINT32_MAX;
    if (resolved_unit_enum_literal(context, function->xi, value, &unit_enum_ordinal)) {
        const XrXiTypeStorage *variant = find_dynamic_type_by_id(context, result_type);
        if (!variant || variant->input.kind != XR_CORE_IR_TYPE_VARIANT ||
            unit_enum_ordinal >= variant->input.variant_count ||
            variant->input.variants[unit_enum_ordinal].payload_count != 0u)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi unit enum literal v%u has no exact canonical variant", value->id);
        instruction->operation_id = XR_CORE_OP_CORE_VARIANT_CONSTRUCT;
        instruction->result = value_key(function, value);
        instruction->result_type_id = result_type;
        instruction->result_ownership = logical_ownership_for_type(context, result_type);
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT;
        instruction->immediate.variant_ordinal = unit_enum_ordinal;
        return XR_PROGRAM_BUILD_OK;
    }
    if (resolved_empty_struct_literal(context, function->xi, value)) {
        const XrXiTypeStorage *aggregate = find_dynamic_type_by_id(context, result_type);
        if (!aggregate || aggregate->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
            aggregate->input.field_count != 0u ||
            aggregate->input.nominal_kind != XR_CORE_IR_NOMINAL_STRUCT)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi empty struct literal v%u has no exact nominal aggregate", value->id);
        instruction->operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT;
        instruction->result = value_key(function, value);
        instruction->result_type_id = result_type;
        instruction->result_ownership = logical_ownership_for_type(context, result_type);
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        return XR_PROGRAM_BUILD_OK;
    }
    XiValue *aggregate_fields[XR_MAX_AGG_FIELDS];
    uint32_t aggregate_field_count = 0u;
    if (resolved_value_aggregate_construction(context, function->xi, value, aggregate_fields,
                                              &aggregate_field_count)) {
        const XrXiTypeStorage *aggregate = find_dynamic_type_by_id(context, result_type);
        if (!aggregate || aggregate->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
            aggregate->input.nominal_kind != XR_CORE_IR_NOMINAL_STRUCT ||
            aggregate->input.field_count != aggregate_field_count)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi value aggregate construction v%u has no exact nominal type", value->id);
        instruction->operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT;
        instruction->result = value_key(function, value);
        instruction->result_type_id = result_type;
        instruction->result_ownership = logical_ownership_for_type(context, result_type);
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        return set_operands(context, instruction, function, block, aggregate_fields,
                            aggregate_field_count, diagnostic, diagnostic_size);
    }
    uint32_t aggregate_field_ordinal = UINT32_MAX;
    if (resolved_aggregate_field_projection(context, value, &aggregate_field_ordinal)) {
        uint16_t aggregate_type_id = XR_CORE_TYPE_VOID;
        if (!map_type(context, value->args[0]->type, &aggregate_type_id))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi field load v%u has no exact logical aggregate source", value->id);
        const XrXiTypeStorage *aggregate = find_dynamic_type_by_id(context, aggregate_type_id);
        if (!aggregate || aggregate->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
            aggregate_field_ordinal >= aggregate->input.field_count ||
            aggregate->input.field_types[aggregate_field_ordinal] != result_type)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi field load v%u has an invalid declaration ordinal or type", value->id);
        instruction->operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT;
        instruction->result = value_key(function, value);
        instruction->result_type_id = result_type;
        instruction->result_ownership = XR_CORE_IR_NON_OWNER;
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD;
        instruction->immediate.field_ordinal = aggregate_field_ordinal;
        return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                            diagnostic_size);
    }
    XrProgramXiProjection projection;
    if (!xr_program_xi_projection(value->op, result_type, &projection))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi operation %s (%u) at v%u has no active CoreSpec projection",
                    xi_op_name(value->op), value->op, value->id);

    switch (projection.kind) {
        case XR_PROGRAM_XI_PROJECTION_CONSTANT: {
            XrCoreIrKey constant;
            XrProgramBuildStatus status =
                add_constant(context, module, value, &constant, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT;
            instruction->immediate.key = constant;
            return XR_PROGRAM_BUILD_OK;
        }
        case XR_PROGRAM_XI_PROJECTION_BINARY_ARITHMETIC:
            if (value->nargs != 2u || result_type != XR_CORE_TYPE_I64)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi arithmetic v%u is not exact i64 binary arithmetic", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = XR_CORE_TYPE_I64;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
            instruction->immediate.u32 = projection.immediate_u32;
            return set_operands(context, instruction, function, block, value->args, value->nargs,
                                diagnostic, diagnostic_size);
        case XR_PROGRAM_XI_PROJECTION_LOGICAL_UNARY:
        case XR_PROGRAM_XI_PROJECTION_LOGICAL_BINARY: {
            bool unary = projection.kind == XR_PROGRAM_XI_PROJECTION_LOGICAL_UNARY;
            uint16_t expected_arity = unary ? 1u : 2u;
            if (value->nargs != expected_arity || result_type != XR_CORE_TYPE_BOOL ||
                projection.immediate_u32 != 0u ||
                (unary && projection.core_operation_id != XR_CORE_OP_CORE_LOGICAL_NOT) ||
                (!unary && projection.core_operation_id != XR_CORE_OP_CORE_LOGICAL_AND &&
                 projection.core_operation_id != XR_CORE_OP_CORE_LOGICAL_OR))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi logical v%u has no exact bool operation contract", value->id);
            for (uint16_t operand = 0u; operand < value->nargs; ++operand) {
                uint16_t operand_type = XR_CORE_TYPE_VOID;
                if (!map_logical_value_type(context, function->xi, value->args[operand],
                                            &operand_type) ||
                    operand_type != XR_CORE_TYPE_BOOL)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi logical v%u operand %u is not exact bool", value->id, operand);
            }
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = XR_CORE_TYPE_BOOL;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(context, instruction, function, block, value->args, value->nargs,
                                diagnostic, diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_COMPARE: {
            uint16_t left_type = XR_CORE_TYPE_VOID;
            uint16_t right_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 2u || result_type != XR_CORE_TYPE_BOOL ||
                !map_type(context, value->args[0]->type, &left_type) ||
                !map_type(context, value->args[1]->type, &right_type) || left_type != right_type)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi comparison v%u operands do not have one exact logical type",
                            value->id);
            bool target_enum_compare =
                left_type >= XR_CORE_TYPE_TARGET_OS && left_type <= XR_CORE_TYPE_TARGET_ENDIAN;
            if ((!target_enum_compare && left_type != XR_CORE_TYPE_I64) ||
                (target_enum_compare && projection.immediate_u32 > 1u))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi comparison v%u is outside the active exact comparison domain",
                            value->id);
            instruction->operation_id = target_enum_compare ? XR_CORE_OP_CORE_COMPARE_TARGET_ENUM
                                                            : projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = XR_CORE_TYPE_BOOL;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
            instruction->immediate.u32 = projection.immediate_u32;
            return set_operands(context, instruction, function, block, value->args, value->nargs,
                                diagnostic, diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_SEALED_DIRECT_CALL:
            return translate_call(context, module, function, value, block, &projection, instruction,
                                  diagnostic, diagnostic_size);
        case XR_PROGRAM_XI_PROJECTION_AGGREGATE_CONSTRUCT: {
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, result_type);
            if (!type || type->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
                value->nargs != type->input.field_count || value->aux_int < 0 ||
                (uint64_t) (value->aux_int & XI_TUPLE_AUX_ARITY_MASK) != type->input.field_count)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi tuple construction v%u has no exact logical aggregate shape",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(context, instruction, function, block, value->args, value->nargs,
                                diagnostic, diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_AGGREGATE_PROJECT: {
            if (value->op == XI_LOAD_UPVAL) {
                uint32_t ordinal = value->aux_int >= 0 ? (uint32_t) value->aux_int : UINT32_MAX;
                const XrXiTypeStorage *capture =
                    find_dynamic_type_by_id(context, function->capture_type_id);
                if (!capture || capture->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
                    ordinal >= capture->input.field_count ||
                    capture->input.field_types[ordinal] != result_type)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi upvalue load v%u has an invalid capture ordinal or type",
                                value->id);
                instruction->operation_id = projection.core_operation_id;
                instruction->result = value_key(function, value);
                instruction->result_type_id = result_type;
                instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD;
                instruction->immediate.field_ordinal = ordinal;
                XiValue *receiver[] = {&function->capture_receiver};
                return set_operands(context, instruction, function, block, receiver, 1u, diagnostic,
                                    diagnostic_size);
            }
            uint16_t aggregate_type_id = XR_CORE_TYPE_VOID;
            if (value->nargs != 1u || value->aux_int < 0 ||
                !map_type(context, value->args[0]->type, &aggregate_type_id))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi tuple projection v%u has no exact logical aggregate source",
                            value->id);
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, aggregate_type_id);
            uint32_t ordinal = (uint32_t) value->aux_int;
            if (!type || type->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
                ordinal >= type->input.field_count ||
                type->input.field_types[ordinal] != result_type)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi tuple projection v%u has an invalid declaration ordinal",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD;
            instruction->immediate.field_ordinal = ordinal;
            return set_operands(context, instruction, function, block, value->args, value->nargs,
                                diagnostic, diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_AGGREGATE_UPDATE: {
            uint16_t source_type_id = XR_CORE_TYPE_VOID;
            uint16_t replacement_type_id = XR_CORE_TYPE_VOID;
            uint32_t ordinal = value->aux_int >= 0 ? (uint32_t) value->aux_int : UINT32_MAX;
            if (value->nargs != 2u || !map_type(context, value->args[0]->type, &source_type_id) ||
                !map_type(context, value->args[1]->type, &replacement_type_id))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi aggregate update v%u has no exact logical operands", value->id);
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, result_type);
            if (!type || type->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
                source_type_id != result_type || ordinal >= type->input.field_count ||
                type->input.field_types[ordinal] != replacement_type_id)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi aggregate update v%u has an invalid declaration ordinal or type",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD;
            instruction->immediate.field_ordinal = ordinal;
            return set_operands(context, instruction, function, block, value->args, value->nargs,
                                diagnostic, diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_VARIANT_CONSTRUCT: {
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, result_type);
            uint32_t variant = value->aux_int >= 0 ? (uint32_t) value->aux_int : UINT32_MAX;
            bool sum_inject = value->op == XI_SUM_INJECT;
            uint16_t operand_offset = sum_inject ? 0u : 1u;
            if (!type || type->input.kind != XR_CORE_IR_TYPE_VARIANT ||
                (!sum_inject && value->nargs < 1u) || variant >= type->input.variant_count ||
                value->nargs - operand_offset != type->input.variants[variant].payload_count)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant construction v%u has no exact logical variant shape",
                            value->id);
            if (sum_inject) {
                uint16_t payload_type_id = XR_CORE_TYPE_VOID;
                if (!value->type || !value->type->is_nullable ||
                    type->input.nominal_kind != XR_CORE_IR_NOMINAL_NONE ||
                    type->input.variant_count != 2u ||
                    type->input.variants[0].payload_count != 0u ||
                    type->input.variants[1].payload_count != 1u || variant > 1u ||
                    (variant == 1u &&
                     (!map_type(context, value->args[0]->type, &payload_type_id) ||
                      payload_type_id != type->input.variants[1].payload_types[0])))
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi sum injection v%u has no exact Optional payload contract",
                                value->id);
            }
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->result_ownership = logical_ownership_for_type(context, result_type);
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT;
            instruction->immediate.variant_ordinal = variant;
            return set_operands(context, instruction, function, block, value->args + operand_offset,
                                value->nargs - operand_offset, diagnostic, diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_VARIANT_TEST: {
            uint16_t variant_type_id = XR_CORE_TYPE_VOID;
            uint32_t variant = value->aux_int >= 0 ? (uint32_t) value->aux_int : UINT32_MAX;
            if (value->nargs != 1u || result_type != XR_CORE_TYPE_BOOL ||
                !map_type(context, value->args[0]->type, &variant_type_id))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant test v%u has no exact logical variant source", value->id);
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, variant_type_id);
            if (!type || type->input.kind != XR_CORE_IR_TYPE_VARIANT ||
                variant >= type->input.variant_count)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant test v%u has an invalid declaration ordinal", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = XR_CORE_TYPE_BOOL;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT;
            instruction->immediate.variant_ordinal = variant;
            return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_VARIANT_PROJECT: {
            uint16_t variant_type_id = XR_CORE_TYPE_VOID;
            uint32_t variant = xi_variant_projection_variant(value);
            uint32_t field = xi_variant_projection_field(value);
            if (value->nargs != 1u || !map_type(context, value->args[0]->type, &variant_type_id))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant projection v%u has no exact logical variant source",
                            value->id);
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, variant_type_id);
            if (!type || type->input.kind != XR_CORE_IR_TYPE_VARIANT ||
                variant >= type->input.variant_count ||
                field >= type->input.variants[variant].payload_count ||
                type->input.variants[variant].payload_types[field] != result_type)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant projection v%u has an invalid declaration ordinal",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->result_ownership =
                logical_value_produces_owner(context, function->xi, value, 0u)
                    ? XR_CORE_IR_OWNER
                    : XR_CORE_IR_NON_OWNER;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT_FIELD;
            instruction->immediate.variant_field.variant_ordinal = variant;
            instruction->immediate.variant_field.field_ordinal = field;
            return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_OWNER_COPY: {
            uint16_t operand_type = XR_CORE_TYPE_VOID;
            bool explicit_xi_clone = xi_copy_is_value_clone(value);
            bool builtin_copy = value->op == XI_CALL_BUILTIN && value->aux &&
                                value->aux_kind == XI_AUX_KIND_NONE &&
                                strcmp((const char *) value->aux, "copy") == 0;
            bool operand_type_mapped =
                value->nargs == 1u &&
                map_logical_value_type(context, function->xi, value->args[0], &operand_type);
            XrCoreIrCopyContract copy_contract =
                logical_copy_contract_for_type(context, result_type);
            if ((!explicit_xi_clone && !builtin_copy) || value->nargs != 1u ||
                result_type == XR_CORE_TYPE_VOID || !operand_type_mapped ||
                operand_type != result_type || copy_contract == XR_CORE_IR_COPY_FORBIDDEN) {
                const XrClassInfo *result_nominal = nominal_info_for_type(value->type);
                const XrClassInfo *operand_nominal =
                    value->nargs == 1u && value->args[0]
                        ? nominal_info_for_type(value->args[0]->type)
                        : NULL;
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi copy v%u has no exact logical copy contract "
                            "(kind=%lld nargs=%u result=%u operand-mapped=%u operand=%u copy=%u "
                            "result-kind=%u result-nominal=%u result-decl=%u operand-kind=%u "
                            "operand-nominal=%u operand-decl=%u)",
                            value->id, (long long) value->aux_int, value->nargs, result_type,
                            operand_type_mapped ? 1u : 0u, operand_type, (unsigned) copy_contract,
                            value->type ? (unsigned) value->type->kind : 0u,
                            result_nominal ? (unsigned) result_nominal->nominal_kind : 0u,
                            result_nominal ? result_nominal->xg_decl_id : 0u,
                            value->nargs == 1u && value->args[0] && value->args[0]->type
                                ? (unsigned) value->args[0]->type->kind
                                : 0u,
                            operand_nominal ? (unsigned) operand_nominal->nominal_kind : 0u,
                            operand_nominal ? operand_nominal->xg_decl_id : 0u);
            }
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->result_ownership = logical_ownership_for_type(context, result_type);
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_OWNER_MOVE: {
            uint16_t operand_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 1u || result_type == XR_CORE_TYPE_VOID ||
                !map_logical_value_type(context, function->xi, value->args[0], &operand_type) ||
                operand_type != result_type)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi owner transfer v%u has no exact logical value type", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->result_ownership = logical_ownership_for_type(context, result_type);
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            const XiValue *operand =
                exact_logical_value_identity(context, function->xi, value->args[0]);
            if (operand && operand->op == XI_PLACE_LOAD && operand->nargs == 1u && operand->args) {
                const XiValue *storage_owner =
                    local_place_storage_root(context, function->xi, operand->args[0]);
                if (storage_owner)
                    operand = storage_owner;
            }
            XrCoreIrKey *operands = xr_calloc(1u, sizeof(*operands));
            if (!operands)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            if (!value_operand_key(context, function, block, operand, operands)) {
                xr_free(operands);
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi owner transfer v%u storage owner is unavailable", value->id);
            }
            instruction->operands = operands;
            instruction->operand_count = 1u;
            return XR_PROGRAM_BUILD_OK;
        }
        case XR_PROGRAM_XI_PROJECTION_PLACE_LOCAL: {
            uint16_t operand_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 1u || result_type == XR_CORE_TYPE_VOID ||
                !map_type(context, value->args[0]->type, &operand_type) ||
                operand_type != result_type || !xi_local_addr_names_operand_storage(value->aux_int))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi local address v%u is not an exact call-bound local place",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->result_category = XR_CORE_IR_PLACE;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            const XiValue *storage_owner = local_place_storage_root(context, function->xi, value);
            XrCoreIrKey *operands = xr_calloc(1u, sizeof(*operands));
            if (!storage_owner || !operands) {
                xr_free(operands);
                return storage_owner
                           ? XR_PROGRAM_BUILD_OUT_OF_MEMORY
                           : fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                  "Xi local address v%u has no exact storage owner", value->id);
            }
            if (!value_operand_key(context, function, block, storage_owner, operands)) {
                xr_free(operands);
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi local address v%u storage owner is unavailable", value->id);
            }
            instruction->operands = operands;
            instruction->operand_count = 1u;
            return XR_PROGRAM_BUILD_OK;
        }
        case XR_PROGRAM_XI_PROJECTION_PLACE_LOAD: {
            uint16_t place_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 1u || result_type == XR_CORE_TYPE_VOID ||
                !map_type(context, value->args[0]->type, &place_type) ||
                place_type != result_type ||
                logical_value_category(function->xi, value->args[0]) != XR_CORE_IR_PLACE)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi place load v%u has no exact pointee type", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->result_ownership = XR_CORE_IR_NON_OWNER;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_PLACE_STORE: {
            uint16_t place_type = XR_CORE_TYPE_VOID;
            uint16_t value_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 2u || result_type != XR_CORE_TYPE_VOID ||
                !map_type(context, value->args[0]->type, &place_type) ||
                !map_type(context, value->args[1]->type, &value_type) || place_type != value_type ||
                logical_value_category(function->xi, value->args[0]) != XR_CORE_IR_PLACE)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi place store v%u has no exact pointee/value contract", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result_type_id = XR_CORE_TYPE_VOID;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(context, instruction, function, block, value->args, 2u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_CALLABLE_PACK:
            return translate_callable_pack(context, function, value, instruction, diagnostic,
                                           diagnostic_size);
        case XR_PROGRAM_XI_PROJECTION_TARGET_QUERY: {
            const XgTargetQuerySummary *query = xg_global_evidence_find_target_query(
                context->source->global_evidence,
                (XgTargetQueryUseId) value->xg_target_query_use_id);
            uint8_t expected_query = XG_TARGET_QUERY_NONE;
            uint16_t expected_type = XR_CORE_TYPE_VOID;
            uint16_t expected_operation = 0u;
            switch (value->op) {
                case XI_TARGET_POINTER_BITS:
                    expected_query = XG_TARGET_QUERY_POINTER_BITS;
                    expected_type = XR_CORE_TYPE_U16;
                    expected_operation = XR_CORE_OP_CORE_TARGET_POINTER_WIDTH;
                    break;
                case XI_TARGET_OPERATING_SYSTEM:
                    expected_query = XG_TARGET_QUERY_OPERATING_SYSTEM;
                    expected_type = XR_CORE_TYPE_TARGET_OS;
                    expected_operation = XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM;
                    break;
                case XI_TARGET_ARCHITECTURE:
                    expected_query = XG_TARGET_QUERY_ARCHITECTURE;
                    expected_type = XR_CORE_TYPE_TARGET_ARCH;
                    expected_operation = XR_CORE_OP_CORE_TARGET_ARCHITECTURE;
                    break;
                case XI_TARGET_NATIVE_ABI:
                    expected_query = XG_TARGET_QUERY_NATIVE_ABI;
                    expected_type = XR_CORE_TYPE_TARGET_ABI;
                    expected_operation = XR_CORE_OP_CORE_TARGET_NATIVE_ABI;
                    break;
                case XI_TARGET_ENDIANNESS:
                    expected_query = XG_TARGET_QUERY_ENDIANNESS;
                    expected_type = XR_CORE_TYPE_TARGET_ENDIAN;
                    expected_operation = XR_CORE_OP_CORE_TARGET_ENDIANNESS;
                    break;
                default:
                    break;
            }
            if (expected_query == XG_TARGET_QUERY_NONE || value->nargs != 0u ||
                result_type != expected_type ||
                projection.core_operation_id != expected_operation ||
                value->xg_target_query_use_id == XG_NO_ID ||
                value->xg_target_namespace_id != XG_TARGET_NAMESPACE_TARGET ||
                value->xg_target_query_kind != expected_query ||
                value->xg_target_result_native_type != XR_NATIVE_U16 ||
                value->xg_target_query_complete != 1u || !query ||
                query->use_id != value->xg_target_query_use_id ||
                query->owner_func_id != function->xi->xg_body_func_id ||
                query->source_node_id != value->xg_target_source_node_id ||
                query->body_ordinal != value->xg_target_body_ordinal ||
                query->namespace_id != XG_TARGET_NAMESPACE_TARGET ||
                query->query_kind != expected_query || query->result_native_type != XR_NATIVE_U16 ||
                query->contract_complete != 1u ||
                query->result_type_key != xg_target_query_result_type_key(expected_query))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi target query v%u lacks its exact Xglobal contract", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = expected_type;
            instruction->result_ownership = logical_ownership_for_type(context, result_type);
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return XR_PROGRAM_BUILD_OK;
        }
        case XR_PROGRAM_XI_PROJECTION_OUTPUT_GROUP_I64: {
            const XrPrintPlan *plan = xi_print_plan(value);
            uint16_t operand_type = XR_CORE_TYPE_VOID;
            if (value->op != XI_PRINT || value->nargs != 1u || result_type != XR_CORE_TYPE_VOID ||
                !plan || !xr_print_plan_validate(plan) || plan->arity != 1u ||
                plan->separator != XR_PRINT_SEPARATOR_SPACE ||
                plan->terminator != XR_PRINT_TERMINATOR_NEWLINE ||
                plan->required_capabilities != XR_PRINT_CAPABILITY_OUTPUT_WRITE ||
                plan->flags != XR_PRINT_PLAN_FLAG_ATOMIC_GROUP ||
                !map_logical_value_type(context, function->xi, value->args[0], &operand_type) ||
                operand_type != XR_CORE_TYPE_I64)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi print v%u is not the exact atomic i64-line output contract",
                            value->id);
            XrStableId contract_id = {{0}};
            XrStableId operation_id = {{0}};
            XrProgramBuildStatus requirement_status = require_provider_operation(
                context, XR_PROVIDER_IO_CONTRACT_KEY, XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY,
                &contract_id, &operation_id);
            if (requirement_status != XR_PROGRAM_BUILD_OK)
                return requirement_status;
            instruction->operation_id = projection.core_operation_id;
            instruction->result_type_id = XR_CORE_TYPE_VOID;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION;
            instruction->immediate.provider_operation.contract_id = contract_id;
            instruction->immediate.provider_operation.operation_id = operation_id;
            return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        default:
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi operation %u at v%u has an invalid CoreSpec projection", value->op,
                        value->id);
    }
}

/* Top-level function declarations are already represented by immutable
 * canonical function rows
 * and exact call-target keys. Their Xi shared-slot
 * publication is compiler scaffolding, not
 * runtime state. Erase it only when
 * the initializer store, closure target, and module slot table
 * all name the
 * same source function and the closure has no other use. */
static bool static_function_publication_store_is_exact(const XiFunc *function,
                                                       const XiValue *store) {
    const XiModule *module = function ? function->module : NULL;
    if (!module || module->init != function || !store || store->op != XI_SET_SHARED ||
        store->aux_int < 0 || (uint64_t) store->aux_int >= module->nslots || store->nargs != 1u ||
        !store->args || !store->args[0] || !module->slot_funcs)
        return false;
    const XiValue *closure = logical_value_identity(store->args[0]);
    const XiFunc *target = closure ? resolved_callable_target(function, closure) : NULL;
    return closure && closure->op == XI_CLOSURE_NEW && closure->nargs == 0u && target &&
           module->slot_funcs[store->aux_int] == target && target->parent_func == function;
}

static bool resolved_import_reference_is_exact(const XrXiBuildContext *context,
                                               const XiValue *import) {
    import = logical_value_identity(import);
    const XiImportRef *reference =
        import && import->op == XI_IMPORT_REF ? (const XiImportRef *) import->aux : NULL;
    if (!reference || !reference->resolution_attempted || reference->resolved_mod_index < 0 ||
        !reference->resolved_module || !context || !context->source ||
        (uint32_t) reference->resolved_mod_index >= context->source->module_count)
        return false;
    const XiFunc *target_root = context->source->module_roots[reference->resolved_mod_index];
    return target_root && target_root->module == reference->resolved_module;
}

static bool static_import_publication_store_is_exact(const XrXiBuildContext *context,
                                                     const XiFunc *function, const XiValue *store) {
    const XiModule *module = function ? function->module : NULL;
    if (!module || module->init != function || !store || store->op != XI_SET_SHARED ||
        store->aux_int < 0 || (uint64_t) store->aux_int >= module->nslots || store->nargs != 1u ||
        !store->args || !store->args[0] ||
        !resolved_import_reference_is_exact(context, store->args[0]))
        return false;
    const XiValue *import = logical_value_identity(store->args[0]);
    const XiImportRef *reference = (const XiImportRef *) import->aux;
    return !module->slot_imports || !module->slot_imports[store->aux_int] ||
           module->slot_imports[store->aux_int] == reference;
}

static bool static_function_publication_source_is_exact(const XiFunc *function,
                                                        const XiValue *value) {
    if (!function || !value || value->op != XI_CLOSURE_NEW || value->nargs != 0u)
        return false;
    bool found = false;
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (block && block->control == value)
            return false;
        for (uint32_t user_index = 0u; block && user_index < block->nvalues; ++user_index) {
            const XiValue *user = block->values[user_index];
            for (uint16_t argument = 0u; user && argument < user->nargs; ++argument) {
                if (!user->args || logical_value_identity(user->args[argument]) != value)
                    continue;
                if (argument != 0u || !static_function_publication_store_is_exact(function, user))
                    return false;
                found = true;
            }
        }
    }
    return found;
}

static bool static_function_publication_is_exact(const XiFunc *function, const XiValue *value) {
    if (static_function_publication_store_is_exact(function, value))
        return true;
    return static_function_publication_source_is_exact(function, value);
}

static bool static_import_publication_is_exact(const XrXiBuildContext *context,
                                               const XiFunc *function, const XiValue *value) {
    if (static_import_publication_store_is_exact(context, function, value))
        return true;
    return value && value->op == XI_IMPORT_REF && value->nargs == 0u &&
           resolved_import_reference_is_exact(context, value);
}

/* A source value-struct declaration publishes a phase-only class token through
 * module shared
 * storage even though instances are represented by their frozen
 * aggregate TypeId.  Erase that
 * token and its store only when one initializer,
 * one slot, one Xi descriptor and one Xglobal
 * struct declaration all agree.
 * Heap classes remain outside this rule because their runtime type
 * object is a
 * real execution value. */
static const XiClassData *static_value_struct_publication_source(const XrXiBuildContext *context,
                                                                 const XiFunc *function,
                                                                 const XiValue *source,
                                                                 uint32_t *slot_out) {
    if (slot_out)
        *slot_out = UINT32_MAX;
    source = logical_value_identity(source);
    const XiClassData *class_data =
        source && source->op == XI_CLASS_CREATE ? (const XiClassData *) source->aux : NULL;
    const XiModule *module = function ? function->module : NULL;
    if (!context || !context->source || !context->source->global_evidence || !function || !module ||
        module->init != function || !source || source->block == NULL ||
        source->block->func != function || source->nargs != 0u || !class_data ||
        class_data->needs_runtime_type || class_data->xg_class_id == XG_NO_ID)
        return NULL;
    uint32_t module_index = UINT32_MAX;
    if (!find_collected_xi_function_module(context, function, &module_index) ||
        module_index >= context->source->module_count ||
        context->source->module_roots[module_index] != function)
        return NULL;
    const XgClassSummary *class_row =
        find_xg_class_by_id(context->source->global_evidence, class_data->xg_class_id);
    const XgDeclSummary *decl_row =
        class_row ? find_xg_decl_by_id(context->source->global_evidence, class_row->decl_id) : NULL;
    bool skeleton = (class_row && (class_row->flags & XG_CLASS_GENERIC_SKELETON) != 0u);
    bool monomorphized = (class_row && (class_row->flags & XG_CLASS_MONOMORPHIZED) != 0u);
    if (!class_row || !decl_row || class_row->module_id != (XgModuleId) (module_index + 1u) ||
        decl_row->module_id != class_row->module_id || class_row->decl_kind != XG_DECL_STRUCT ||
        decl_row->kind != XG_DECL_STRUCT || class_row->parent_class_id != XG_NO_ID ||
        class_row->class_id != class_data->xg_class_id ||
        skeleton != class_data->is_generic_skeleton ||
        monomorphized != class_data->is_monomorphized || (skeleton && monomorphized) ||
        (!skeleton && !class_data->struct_layout) ||
        (class_data->struct_layout && class_data->struct_layout->kind != XR_AGG_LAYOUT_STRUCT &&
         class_data->struct_layout->kind != XR_AGG_LAYOUT_PACKED_STRUCT))
        return NULL;

    uint32_t publication_slot = UINT32_MAX;
    uint32_t publications = 0u;
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (block && block->control == source)
            return NULL;
        for (const XiPhi *phi = block ? block->phis : NULL; phi; phi = phi->next)
            for (uint16_t argument = 0u; argument < phi->value.nargs; ++argument)
                if (phi->value.args && logical_value_identity(phi->value.args[argument]) == source)
                    return NULL;
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *user = block->values[value_index];
            for (uint16_t argument = 0u; user && argument < user->nargs; ++argument) {
                if (!user->args || logical_value_identity(user->args[argument]) != source)
                    continue;
                if (user->op == XI_RETAIN || user->op == XI_RELEASE)
                    continue;
                if (user->op != XI_SET_SHARED || argument != 0u || user->aux_int < 0 ||
                    (uint64_t) user->aux_int >= module->nslots || !module->slot_classes ||
                    module->slot_classes[user->aux_int] != class_data || publications != 0u)
                    return NULL;
                publication_slot = (uint32_t) user->aux_int;
                ++publications;
            }
        }
    }
    if (publications != 1u)
        return NULL;
    if (slot_out)
        *slot_out = publication_slot;
    return class_data;
}

static bool static_value_struct_publication_is_exact(const XrXiBuildContext *context,
                                                     const XiFunc *function, const XiValue *value) {
    if (!value)
        return false;
    if (value->op == XI_CLASS_CREATE)
        return static_value_struct_publication_source(context, function, value, NULL) != NULL;
    if (value->op != XI_SET_SHARED || value->nargs != 1u || !value->args || value->aux_int < 0)
        return false;
    uint32_t slot = UINT32_MAX;
    return static_value_struct_publication_source(context, function, value->args[0], &slot) !=
               NULL &&
           slot == (uint32_t) value->aux_int;
}

static bool value_is_skipped(const XrXiBuildContext *context, const XiFunc *function,
                             const XiValue *value) {
    if (direct_projection_writeback_is_exact(context, function, value))
        return true;
    const XrXiFunctionStorage *function_storage =
        value && value->block ? find_xi_function(context, function, NULL, NULL) : NULL;
    const XrXiBlockStorage *block_storage =
        function_storage ? find_block_storage(function_storage, value->block) : NULL;
    if (deferred_cleanup_place_owner(context, function, value))
        return true;
    if (value && value->block && value->block->control == value &&
        value_is_static_typed_catch_test(value) && block_storage &&
        block_storage->static_branch_outcome != XR_XI_STATIC_BRANCH_UNKNOWN)
        return true;
    if (value_is_invoke_scaffold(context, function, value))
        return true;
    /* A defer's error-exit work is already ordinary Xi CFG.  Its TRY/END_TRY
     * pair only
     * identifies the compiler-generated static cleanup region; the
     * canonical panic path is
     * represented by an explicit Program edge. */
    if (exact_static_cleanup_try(function, value) || exact_static_cleanup_end(function, value) ||
        exact_cleanup_boundary_marker(value))
        return true;
    if (value && value->op == XI_CATCH && block_storage &&
        (block_storage->trap_cleanup || block_storage->panic_cleanup ||
         block_storage->cancel_cleanup))
        return true;
    if (static_function_publication_is_exact(function, value))
        return true;
    if (static_import_publication_is_exact(context, function, value))
        return true;
    if (static_value_struct_publication_is_exact(context, function, value))
        return true;
    if (value_aggregate_initializer_is_exact(context, function, value))
        return true;
    if (projected_native_import_reference_is_exact(context, function, value))
        return true;
    if (imported_callable_checktype_is_exact(context, function, value))
        return true;
    if (read_value_receiver_load_is_exact(context, function, value))
        return true;
    if (read_value_call_place_is_exact(context, function, value))
        return true;
    if (read_value_receiver_call_place_is_exact(context, function, value))
        return true;
    if (value->op == XI_PARAM || value->op == XI_THROW || xi_copy_is_identity_alias(value) ||
        cleanup_return_copy_is_exact(value) ||
        (xi_copy_is_value_clone(value) && logical_value_identity(value) != value))
        return true;
    /* Physical RC is executor-private representation.  Canonical ownership is
     * reconstructed from typed owner creation and semantic uses, never from
     * retain/release counts. */
    if (value->op == XI_RETAIN || value->op == XI_RELEASE)
        return true;
    if (resolved_module_namespace_carrier(context, function, value))
        return value_is_only_elided_operand(context, function, value);
    if (shared_callable_value_is_exact(context, function, value))
        return value_is_only_elided_operand(context, function, value);
    if (resolved_class_carrier(context, function, value, XG_NO_ID, NULL))
        return value_is_only_elided_operand(context, function, value);
    return (value->op == XI_GET_SHARED || value->op == XI_GET_BUILTIN) &&
           value_is_only_elided_operand(context, function, value);
}

/* Projection splits use instruction gaps rather than Xi indices because one Xi
 * value can emit
 * several CoreIR instructions. Cleanup markers are zero-width
 * gaps, so a body can be sliced
 * without manufacturing or dropping an emitted
 * operation. */
static bool emission_spans_are_exact(const XrXiBuildContext *context, const XiFunc *function,
                                     const XrXiBlockStorage *block,
                                     const XiCoroSuspendPoint *suspend_point, uint32_t body_end) {
    if (!function || !block || !block->xi || block->xi->func != function ||
        block->emission_span_count != block->xi->nvalues ||
        (block->emission_span_count != 0u && !block->emission_spans))
        return false;
    uint32_t previous_end = 0u;
    for (uint32_t index = 0u; index < block->emission_span_count; ++index) {
        const XiValue *value = block->xi->values[index];
        const XrXiEmissionSpan *span = &block->emission_spans[index];
        if (!value || span->begin < previous_end || span->begin > span->end || span->end > body_end)
            return false;
        bool zero_width = value_is_skipped(context, function, value) ||
                          (suspend_point && value == suspend_point->op);
        if ((zero_width && span->begin != span->end) || (!zero_width && span->begin == span->end) ||
            (exact_cleanup_boundary_marker(value) && span->begin != span->end))
            return false;
        previous_end = span->end;
    }
    return true;
}

static XrXiCleanupPointStorage *find_cleanup_point(XrXiFunctionStorage *function,
                                                   const XiCleanupBoundary *boundary) {
    for (uint32_t index = 0u; function && index < function->cleanup_point_count; ++index) {
        if (function->cleanup_points[index].boundary == boundary)
            return &function->cleanup_points[index];
    }
    return NULL;
}

static XrProgramBuildStatus collect_cleanup_program_identities(XrXiFunctionStorage *function,
                                                               char *diagnostic,
                                                               size_t diagnostic_size) {
    if (!function || !function->xi || function->cleanup_points)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint32_t count = 0u;
    for (uint32_t block = 0u; block < function->xi->nblocks; ++block) {
        const XiBlock *source = function->xi->blocks[block];
        for (uint32_t value = 0u; source && value < source->nvalues; ++value) {
            if (source->values[value] && source->values[value]->op == XI_CLEANUP_ENTER) {
                if (count == UINT32_MAX)
                    return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
                ++count;
            }
        }
    }
    function->cleanup_point_count = count;
    function->cleanup_points = count ? xr_calloc(count, sizeof(*function->cleanup_points)) : NULL;
    if (count && !function->cleanup_points)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t cursor = 0u;
    for (uint32_t block = 0u; block < function->xi->nblocks; ++block) {
        const XiBlock *source = function->xi->blocks[block];
        for (uint32_t value = 0u; source && value < source->nvalues; ++value) {
            const XiValue *marker = source->values[value];
            if (!marker || marker->op != XI_CLEANUP_ENTER)
                continue;
            if (cursor >= count || !marker->cleanup_boundary ||
                find_cleanup_point(function, marker->cleanup_boundary))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi cleanup enter in b%u has no unique program point", source->id);
            XrXiCleanupPointStorage *point = &function->cleanup_points[cursor++];
            point->boundary = marker->cleanup_boundary;
            point->enter_block = source;
            point->enter_value_index = value;
        }
    }
    if (cursor != count)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    for (uint32_t block = 0u; block < function->xi->nblocks; ++block) {
        const XiBlock *source = function->xi->blocks[block];
        for (uint32_t value = 0u; source && value < source->nvalues; ++value) {
            const XiValue *marker = source->values[value];
            if (!marker || marker->op != XI_CLEANUP_LEAVE)
                continue;
            XrXiCleanupPointStorage *point = find_cleanup_point(function, marker->cleanup_boundary);
            if (!point || point->leave_block)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi cleanup leave in b%u has no unique enter", source->id);
            point->leave_block = source;
            point->leave_value_index = value;
        }
    }
    for (uint32_t index = 0u; index < count; ++index) {
        XrXiCleanupPointStorage *point = &function->cleanup_points[index];
        bool closed = point->boundary->kind == XI_CLEANUP_BOUNDARY_CLOSED;
        if ((closed && !point->leave_block) || (!closed && point->leave_block))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi cleanup boundary %u has incomplete program points", index);
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus bind_cleanup_program_points(XrXiFunctionStorage *function,
                                                        char *diagnostic, size_t diagnostic_size) {
    if (!function || !function->xi ||
        (function->cleanup_point_count != 0u && !function->cleanup_points))
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    for (uint32_t index = 0u; index < function->cleanup_point_count; ++index) {
        XrXiCleanupPointStorage *point = &function->cleanup_points[index];
        XrXiBlockStorage *enter = find_block_storage(function, point->enter_block);
        XrXiBlockStorage *leave = find_block_storage(function, point->leave_block);
        if (!enter)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi cleanup enter in b%u has no block storage",
                        point->enter_block ? point->enter_block->id : UINT32_MAX);
        if (enter->emission_ready) {
            if (point->enter_value_index >= enter->emission_span_count)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi cleanup enter in b%u has no emission span", point->enter_block->id);
            const XrXiEmissionSpan *enter_span = &enter->emission_spans[point->enter_value_index];
            if (enter_span->begin != enter_span->end)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi cleanup enter in b%u is not an exact CoreIR gap",
                            point->enter_block->id);
            point->enter_gap = enter_span->begin;
            point->enter_emitted = true;
        } else if (enter->reachable) {
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "reachable Xi cleanup enter in b%u has no CoreIR location",
                        point->enter_block->id);
        }
        if (point->leave_block) {
            if (!leave)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi cleanup leave in b%u has no block storage", point->leave_block->id);
            if (leave->emission_ready) {
                if (point->leave_value_index >= leave->emission_span_count)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi cleanup leave in b%u has no emission span",
                                point->leave_block->id);
                const XrXiEmissionSpan *leave_span =
                    &leave->emission_spans[point->leave_value_index];
                if (leave_span->begin != leave_span->end)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi cleanup leave in b%u is not an exact CoreIR gap",
                                point->leave_block->id);
                point->leave_gap = leave_span->begin;
                point->leave_emitted = true;
            } else if (leave->reachable) {
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "reachable Xi cleanup leave in b%u has no CoreIR location",
                            point->leave_block->id);
            }
        }
        if (point->boundary->kind == XI_CLEANUP_BOUNDARY_CLOSED &&
            point->enter_emitted != point->leave_emitted)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi cleanup boundary %u has incomplete CoreIR program points", index);
    }
    return XR_PROGRAM_BUILD_OK;
}

typedef struct XrXiCleanupFlowState {
    uint32_t cleanup;
    uint32_t handler;
} XrXiCleanupFlowState;

static bool cleanup_flow_state_equal(XrXiCleanupFlowState left, XrXiCleanupFlowState right) {
    return left.cleanup == right.cleanup && left.handler == right.handler;
}

static XrProgramBuildStatus merge_cleanup_flow_state(const XiFunc *function,
                                                     XrXiCleanupFlowState *states, uint32_t *queue,
                                                     uint32_t *tail, const XiBlock *successor,
                                                     XrXiCleanupFlowState incoming,
                                                     char *diagnostic, size_t diagnostic_size) {
    if (!function || !states || !queue || !tail || !successor || successor->func != function ||
        successor->id >= function->nblocks || function->blocks[successor->id] != successor)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    XrXiCleanupFlowState *current = &states[successor->id];
    if (current->cleanup == UINT32_MAX && current->handler == UINT32_MAX) {
        if (*tail >= function->nblocks)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi cleanup control-flow queue overflowed");
        *current = incoming;
        queue[(*tail)++] = successor->id;
        return XR_PROGRAM_BUILD_OK;
    }
    if (!cleanup_flow_state_equal(*current, incoming))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi block b%u merges different cleanup or panic-handler stacks", successor->id);
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus verify_cleanup_control_flow(XrXiFunctionStorage *function,
                                                        char *diagnostic, size_t diagnostic_size) {
    const XiFunc *xi = function ? function->xi : NULL;
    if (!xi || (xi->nblocks != 0u && !xi->blocks) || !xi->entry || xi->entry->id >= xi->nblocks ||
        xi->blocks[xi->entry->id] != xi->entry || function->cleanup_chain_nodes ||
        function->cleanup_handler_chain_nodes || function->cleanup_registrations ||
        function->cleanup_registration_by_value || function->cleanup_state_before_value ||
        function->cleanup_handler_state_before_value)
        return XR_PROGRAM_BUILD_INVALID_INPUT;

    /* Hidden handlers can contain registrations. Initial canonical reachability includes only the

     * * normal graph, so census the complete Xi table and let registration-derived traversal prove

     * * that every entry is connected to the executable cleanup graph. */
    uint32_t registration_count = 0u;
    for (uint32_t block_index = 0u; block_index < xi->nblocks; ++block_index) {
        const XiBlock *block = xi->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            if (exact_static_cleanup_try(xi, block->values[value_index])) {
                if (registration_count == UINT32_MAX)
                    return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
                ++registration_count;
            }
        }
    }

    XrXiCleanupFlowState *states =
        xi->nblocks ? xr_malloc((size_t) xi->nblocks * sizeof(*states)) : NULL;
    uint32_t *queue = xi->nblocks ? xr_malloc((size_t) xi->nblocks * sizeof(*queue)) : NULL;
    uint8_t *processed = xi->nblocks ? xr_calloc(xi->nblocks, sizeof(*processed)) : NULL;
    uint32_t *handler_by_block =
        xi->nblocks ? xr_malloc((size_t) xi->nblocks * sizeof(*handler_by_block)) : NULL;
    function->cleanup_registration_by_value =
        xi->next_value_id ? xr_malloc((size_t) xi->next_value_id *
                                      sizeof(*function->cleanup_registration_by_value))
                          : NULL;
    function->cleanup_state_before_value =
        xi->next_value_id
            ? xr_malloc((size_t) xi->next_value_id * sizeof(*function->cleanup_state_before_value))
            : NULL;
    function->cleanup_handler_state_before_value =
        xi->next_value_id ? xr_malloc((size_t) xi->next_value_id *
                                      sizeof(*function->cleanup_handler_state_before_value))
                          : NULL;
    function->cleanup_chain_nodes =
        function->cleanup_point_count
            ? xr_calloc(function->cleanup_point_count, sizeof(*function->cleanup_chain_nodes))
            : NULL;
    function->cleanup_handler_chain_nodes =
        registration_count
            ? xr_calloc(registration_count, sizeof(*function->cleanup_handler_chain_nodes))
            : NULL;
    function->cleanup_registrations =
        registration_count ? xr_calloc(registration_count, sizeof(*function->cleanup_registrations))
                           : NULL;
    function->cleanup_registration_count = registration_count;
    if ((xi->nblocks && (!states || !queue || !processed || !handler_by_block)) ||
        (xi->next_value_id &&
         (!function->cleanup_registration_by_value || !function->cleanup_state_before_value ||
          !function->cleanup_handler_state_before_value)) ||
        (function->cleanup_point_count && !function->cleanup_chain_nodes) ||
        (registration_count &&
         (!function->cleanup_handler_chain_nodes || !function->cleanup_registrations))) {
        xr_free(handler_by_block);
        xr_free(processed);
        xr_free(queue);
        xr_free(states);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    for (uint32_t block_index = 0u; block_index < xi->nblocks; ++block_index) {
        states[block_index] = (XrXiCleanupFlowState) {.cleanup = UINT32_MAX, .handler = UINT32_MAX};
        handler_by_block[block_index] = UINT32_MAX;
    }
    for (uint32_t value_id = 0u; value_id < xi->next_value_id; ++value_id) {
        function->cleanup_registration_by_value[value_id] = UINT32_MAX;
        function->cleanup_state_before_value[value_id] = UINT32_MAX;
        function->cleanup_handler_state_before_value[value_id] = UINT32_MAX;
    }

    uint32_t registration_cursor = 0u;
    XrProgramBuildStatus status = XR_PROGRAM_BUILD_OK;
    for (uint32_t block_index = 0u; block_index < xi->nblocks && status == XR_PROGRAM_BUILD_OK;
         ++block_index) {
        const XiBlock *block = xi->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *registration = block->values[value_index];
            if (!exact_static_cleanup_try(xi, registration))
                continue;
            const XiBlock *handler = (const XiBlock *) registration->aux;
            if (registration_cursor >= registration_count ||
                registration->id >= xi->next_value_id || !handler || handler == block ||
                handler->id >= xi->nblocks || xi->blocks[handler->id] != handler ||
                function->cleanup_registration_by_value[registration->id] != UINT32_MAX ||
                handler_by_block[handler->id] != UINT32_MAX) {
                status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                              "Xi static cleanup registration v%u has no unique cold handler",
                              registration ? registration->id : UINT32_MAX);
                break;
            }
            uint32_t registration_index = registration_cursor++;
            function->cleanup_registration_by_value[registration->id] = registration_index;
            handler_by_block[handler->id] = registration_index;
            function->cleanup_registrations[registration_index].registration = registration;
            function->cleanup_registrations[registration_index].handler = handler;
            function->cleanup_handler_chain_nodes[registration_index].registration = registration;
        }
    }
    if (status == XR_PROGRAM_BUILD_OK && registration_cursor != registration_count)
        status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                      "Xi reachable cleanup registration census is inconsistent");

    uint32_t cleanup_node_count = 0u;
    states[xi->entry->id] = (XrXiCleanupFlowState) {.cleanup = 0u, .handler = 0u};
    /* Execution state is entry-scoped.  Registration edges pull private
     * cleanup handlers and
     * their successors into this one traversal; an
     * unrelated disconnected Xi component is
     * never seeded as executable. */
    do {
        if (status != XR_PROGRAM_BUILD_OK)
            break;
        uint32_t seed = xi->entry->id;
        if (handler_by_block[seed] != UINT32_MAX) {
            status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                          "Xi function entry cannot be a static cleanup handler");
            break;
        }
        while (seed < xi->nblocks && (processed[seed] || handler_by_block[seed] != UINT32_MAX))
            ++seed;
        if (seed == xi->nblocks) {
            status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                          "Xi static cleanup handler has no registration-derived entry state");
            break;
        }
        if (states[seed].cleanup == UINT32_MAX && states[seed].handler == UINT32_MAX) {
            if (handler_by_block[seed] != UINT32_MAX) {
                status =
                    fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                         "Xi static cleanup handler b%u was seeded as an empty component", seed);
                break;
            }
            states[seed] = (XrXiCleanupFlowState) {.cleanup = 0u, .handler = 0u};
        }
        uint32_t head = 0u;
        uint32_t tail = 0u;
        queue[tail++] = seed;
        while (head < tail && status == XR_PROGRAM_BUILD_OK) {
            uint32_t block_index = queue[head++];
            if (processed[block_index])
                continue;
            processed[block_index] = 1u;
            const XiBlock *block = xi->blocks[block_index];
            XrXiCleanupFlowState state = states[block_index];
            XrXiBlockStorage *block_storage = &function->block_storage[block_index];
            if (!block || block->func != xi || block->id != block_index ||
                block_storage->xi != block || state.cleanup == UINT32_MAX ||
                state.handler == UINT32_MAX || block_storage->cleanup_entry_ready) {
                status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                              "Xi cleanup traversal has invalid state at b%u", block_index);
                break;
            }
            block_storage->cleanup_entry_state = state.cleanup;
            block_storage->cleanup_handler_entry_state = state.handler;
            block_storage->cleanup_entry_ready = true;
            bool has_throw = false;
            for (uint32_t value_index = 0u; value_index < block->nvalues; ++value_index) {
                const XiValue *value = block->values[value_index];
                if (!value)
                    continue;
                if (value->id >= xi->next_value_id ||
                    function->cleanup_state_before_value[value->id] != UINT32_MAX ||
                    function->cleanup_handler_state_before_value[value->id] != UINT32_MAX) {
                    status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                  "Xi value v%u has no unique cleanup program point", value->id);
                    break;
                }
                function->cleanup_state_before_value[value->id] = state.cleanup;
                function->cleanup_handler_state_before_value[value->id] = state.handler;
                has_throw |= value->op == XI_THROW;
                if (value->op == XI_CLEANUP_ENTER) {
                    if (cleanup_node_count >= function->cleanup_point_count) {
                        status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                      "Xi cleanup enter v%u is outside the collected Program graph",
                                      value->id);
                        break;
                    }
                    function->cleanup_chain_nodes[cleanup_node_count] = (XrXiCleanupChainNode) {
                        .boundary = value->cleanup_boundary, .parent = state.cleanup};
                    state.cleanup = ++cleanup_node_count;
                    continue;
                }
                if (value->op == XI_CLEANUP_LEAVE) {
                    if (state.cleanup == 0u ||
                        function->cleanup_chain_nodes[state.cleanup - 1u].boundary !=
                            value->cleanup_boundary) {
                        status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                      "Xi cleanup leave in b%u does not close the active boundary",
                                      block->id);
                        break;
                    }
                    state.cleanup = function->cleanup_chain_nodes[state.cleanup - 1u].parent;
                    continue;
                }
                if (exact_static_cleanup_try(xi, value)) {
                    uint32_t registration_index =
                        value->id < xi->next_value_id
                            ? function->cleanup_registration_by_value[value->id]
                            : UINT32_MAX;
                    if (registration_index >= registration_count ||
                        function->cleanup_registrations[registration_index].context_ready) {
                        status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                      "Xi static cleanup registration v%u is outside the collected "
                                      "Program graph",
                                      value->id);
                        break;
                    }
                    XrXiCleanupRegistrationStorage *registration =
                        &function->cleanup_registrations[registration_index];
                    registration->cleanup_state = state.cleanup;
                    registration->outer_handler_state = state.handler;
                    registration->outer_registration =
                        state.handler == 0u
                            ? NULL
                            : function->cleanup_handler_chain_nodes[state.handler - 1u]
                                  .registration;
                    registration->context_ready = true;
                    XrXiCleanupFlowState handler_state = state;
                    status =
                        merge_cleanup_flow_state(xi, states, queue, &tail, registration->handler,
                                                 handler_state, diagnostic, diagnostic_size);
                    if (status != XR_PROGRAM_BUILD_OK)
                        break;
                    function->cleanup_handler_chain_nodes[registration_index].parent =
                        state.handler;
                    state.handler = registration_index + 1u;
                    continue;
                }
                if (exact_static_cleanup_end(xi, value)) {
                    const XiValue *registration = (const XiValue *) value->aux;
                    uint32_t registration_index =
                        registration->id < xi->next_value_id
                            ? function->cleanup_registration_by_value[registration->id]
                            : UINT32_MAX;
                    if (registration_index >= registration_count ||
                        state.handler != registration_index + 1u) {
                        status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                      "Xi end-try v%u does not close the active cleanup handler",
                                      value->id);
                        break;
                    }
                    state.handler =
                        function->cleanup_handler_chain_nodes[registration_index].parent;
                }
            }
            bool has_normal_successor = block->succs[0] || block->succs[1];
            uint8_t active_cleanup_kind =
                state.cleanup == 0u
                    ? 0u
                    : function->cleanup_chain_nodes[state.cleanup - 1u].boundary->kind;
            /* A closed boundary must complete on every normal exit. XI_THROW can explicitly

             * * abandon an outer closed boundary because it carries the fatal cleanup reason to
             * the
             * next handler or out of the function. */
            if (status == XR_PROGRAM_BUILD_OK && state.cleanup != 0u &&
                ((!has_normal_successor && !has_throw &&
                  active_cleanup_kind == XI_CLEANUP_BOUNDARY_CLOSED) ||
                 (has_normal_successor && active_cleanup_kind == XI_CLEANUP_BOUNDARY_FATAL)))
                status =
                    fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                         "Xi cleanup boundary exits b%u without its exact completion", block->id);
            for (uint32_t edge = 0u; status == XR_PROGRAM_BUILD_OK && edge < 2u; ++edge) {
                if (!block->succs[edge])
                    continue;
                status = merge_cleanup_flow_state(xi, states, queue, &tail, block->succs[edge],
                                                  state, diagnostic, diagnostic_size);
            }
            bool fatal_cleanup =
                state.cleanup != 0u && active_cleanup_kind == XI_CLEANUP_BOUNDARY_FATAL;
            if (status == XR_PROGRAM_BUILD_OK && !has_normal_successor && has_throw &&
                state.handler != 0u && !fatal_cleanup) {
                uint32_t registration_index = state.handler - 1u;
                if (registration_index >= registration_count) {
                    status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                  "Xi block b%u has an unknown active cleanup handler", block->id);
                    break;
                }
                XrXiCleanupFlowState outer = {
                    .cleanup = state.cleanup,
                    .handler = function->cleanup_handler_chain_nodes[registration_index].parent,
                };
                status = merge_cleanup_flow_state(
                    xi, states, queue, &tail,
                    function->cleanup_registrations[registration_index].handler, outer, diagnostic,
                    diagnostic_size);
            } else if (status == XR_PROGRAM_BUILD_OK && !has_normal_successor &&
                       state.handler != 0u && !fatal_cleanup) {
                status =
                    fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                         "Xi block b%u exits with an active static cleanup handler", block->id);
            }
        }
    } while (false);
    if (status == XR_PROGRAM_BUILD_OK && cleanup_node_count != function->cleanup_point_count)
        status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                      "Xi reachable cleanup boundary census is inconsistent");
    for (uint32_t registration = 0u;
         status == XR_PROGRAM_BUILD_OK && registration < registration_count; ++registration) {
        if (!function->cleanup_registrations[registration].context_ready)
            status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                          "Xi static cleanup registration v%u has no control-flow context",
                          function->cleanup_registrations[registration].registration->id);
    }
    if (status == XR_PROGRAM_BUILD_OK)
        function->cleanup_chain_node_count = cleanup_node_count;
    xr_free(handler_by_block);
    xr_free(processed);
    xr_free(queue);
    xr_free(states);
    return status;
}

static const XrXiCleanupRegistrationStorage *
cleanup_registration_storage(const XrXiFunctionStorage *function, const XiValue *registration) {
    if (!function || !function->xi || !registration ||
        registration->id >= function->xi->next_value_id || !function->cleanup_registration_by_value)
        return NULL;
    uint32_t index = function->cleanup_registration_by_value[registration->id];
    if (index >= function->cleanup_registration_count ||
        function->cleanup_registrations[index].registration != registration)
        return NULL;
    return &function->cleanup_registrations[index];
}

static bool
active_cleanup_registration_at_program_point(const XrXiFunctionStorage *function,
                                             const XiValue *point,
                                             const XrXiCleanupRegistrationStorage **registration) {
    if (registration)
        *registration = NULL;
    if (!function || !function->xi || !point || point->block == NULL ||
        point->block->func != function->xi || point->id >= function->xi->next_value_id ||
        !function->cleanup_handler_state_before_value || !registration)
        return false;
    uint32_t state = function->cleanup_handler_state_before_value[point->id];
    if (state == UINT32_MAX)
        return false;
    if (state == 0u)
        return true;
    uint32_t index = state - 1u;
    if (index >= function->cleanup_registration_count ||
        function->cleanup_handler_chain_nodes[index].registration !=
            function->cleanup_registrations[index].registration ||
        !function->cleanup_registrations[index].context_ready)
        return false;
    *registration = &function->cleanup_registrations[index];
    return true;
}

static bool active_cleanup_boundary_at_program_point(const XrXiFunctionStorage *function,
                                                     const XiValue *point,
                                                     const XiCleanupBoundary **boundary) {
    if (boundary)
        *boundary = NULL;
    if (!function || !function->xi || !point || !point->block ||
        point->block->func != function->xi || point->id >= function->xi->next_value_id ||
        !function->cleanup_state_before_value || !boundary)
        return false;
    uint32_t state = function->cleanup_state_before_value[point->id];
    if (state == UINT32_MAX)
        return false;
    if (state == 0u)
        return true;
    uint32_t index = state - 1u;
    if (index >= function->cleanup_chain_node_count ||
        !function->cleanup_chain_nodes[index].boundary)
        return false;
    *boundary = function->cleanup_chain_nodes[index].boundary;
    return true;
}

static XrProgramBuildStatus prepare_cleanup_control_flow(XrXiBuildContext *context,
                                                         char *diagnostic, size_t diagnostic_size) {
    if (!context || !context->source || !context->storage)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    for (uint32_t module_index = 0u; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            XrXiFunctionStorage *function = &module->function_storage[function_index];
            XrProgramBuildStatus status =
                collect_cleanup_program_identities(function, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            status = verify_cleanup_control_flow(function, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus require_value_available(XrXiBuildContext *context,
                                                    XrXiFunctionStorage *function,
                                                    XrXiBlockStorage *block, const XiValue *value,
                                                    bool *changed, char *diagnostic,
                                                    size_t diagnostic_size) {
    const XiValue *typed_value = value;
    value = exact_logical_value_identity(context, function ? function->xi : NULL, value);
    if (!value || !value->block)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi operand has no defining block");
    const XiValue *local_frame_owner =
        value->block != block->xi ? local_place_storage_root(context, function->xi, value) : NULL;
    if (local_frame_owner) {
        uint16_t type_id = XR_CORE_TYPE_VOID;
        if (!map_logical_value_type(context, function->xi, typed_value, &type_id) ||
            type_id == XR_CORE_TYPE_VOID)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi frame place v%u has no exact logical value type", value->id);
        if (logical_ownership_for_type(context, type_id) != XR_CORE_IR_OWNER) {
            XrProgramBuildStatus owner_status = require_value_available(
                context, function, block, local_frame_owner, changed, diagnostic, diagnostic_size);
            if (owner_status != XR_PROGRAM_BUILD_OK)
                return owner_status;
            XrProgramBuildStatus place_status = add_reconstructed_place(
                context, function, block, value, local_frame_owner, type_id);
            if (place_status != XR_PROGRAM_BUILD_OK)
                return fail(diagnostic, diagnostic_size, place_status,
                            "Xi frame place v%u cannot be reconstructed exactly once in b%u",
                            value->id, block->xi->id);
            return XR_PROGRAM_BUILD_OK;
        }
    }
    const XiValue *frame_owner = value->block == block->xi
                                     ? deferred_cleanup_place_owner(context, function->xi, value)
                                     : cleanup_place_storage_owner(context, function->xi, value);
    if (frame_owner) {
        uint16_t type_id = XR_CORE_TYPE_VOID;
        if (!map_logical_value_type(context, function->xi, typed_value, &type_id) ||
            type_id == XR_CORE_TYPE_VOID ||
            !logical_value_produces_owner(context, function->xi, frame_owner, 0u) ||
            logical_ownership_for_type(context, type_id) != XR_CORE_IR_OWNER)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi frame place v%u has no exact affine owner", value->id);
        const XiValue *current_owner = NULL;
        for (uint32_t edge = 0u; edge < context->trap_edge_count; ++edge) {
            const XrXiTrapEdge *trap = &context->trap_edges[edge];
            if (trap->function != function->xi || trap->handler != block->xi || !trap->call)
                continue;
            for (uint16_t operand = 0u; operand < trap->call->nargs; ++operand) {
                const XiValue *candidate =
                    exact_logical_value_identity(context, function->xi, trap->call->args[operand]);
                if (!candidate ||
                    canonical_owner_storage_identity(context, function, candidate) != frame_owner ||
                    !logical_value_produces_owner(context, function->xi, candidate, 0u))
                    continue;
                if (current_owner && current_owner != candidate)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi cleanup place v%u has multiple trap-edge owners", value->id);
                current_owner = candidate;
            }
        }
        for (uint32_t argument = 0u; argument < block->argument_count; ++argument) {
            const XrXiBlockArgumentStorage *candidate = &block->argument_storage[argument];
            const XiValue *origin =
                canonical_owner_storage_identity(context, function, candidate->source);
            if (candidate->ownership != XR_CORE_IR_OWNER || origin != frame_owner)
                continue;
            if (current_owner && current_owner != candidate->source)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi frame place v%u has multiple current owners in b%u", value->id,
                            block->xi->id);
            current_owner = candidate->source;
        }
        if (current_owner && !find_block_argument(block, current_owner)) {
            XrProgramBuildStatus owner_status = require_value_available(
                context, function, block, current_owner, changed, diagnostic, diagnostic_size);
            if (owner_status != XR_PROGRAM_BUILD_OK)
                return owner_status;
        } else if (!current_owner) {
            XrProgramBuildStatus owner_status = require_value_available(
                context, function, block, frame_owner, changed, diagnostic, diagnostic_size);
            if (owner_status != XR_PROGRAM_BUILD_OK)
                return owner_status;
            current_owner = frame_owner;
        }
        XrProgramBuildStatus place_status =
            add_reconstructed_place(context, function, block, value, current_owner, type_id);
        if (place_status != XR_PROGRAM_BUILD_OK)
            return fail(diagnostic, diagnostic_size, place_status,
                        "Xi frame place v%u cannot be reconstructed exactly once in b%u", value->id,
                        block->xi->id);
        return XR_PROGRAM_BUILD_OK;
    }
    if (value->block == block->xi)
        return XR_PROGRAM_BUILD_OK;
    if (value->xg_existential_kind == XI_EXISTENTIAL_REBORROW_READ) {
        if (value->nargs != 1u || !value->args || !value->args[0])
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi existential READ reborrow v%u has no owner", value->id);
        XrProgramBuildStatus owner_status = require_value_available(
            context, function, block, value->args[0], changed, diagnostic, diagnostic_size);
        if (owner_status != XR_PROGRAM_BUILD_OK)
            return owner_status;
    }
    XrXiBlockArgumentStorage *available = find_block_argument(block, value);
    if (available) {
        uint16_t source_type = XR_CORE_TYPE_VOID;
        bool capture_receiver = value == &function->capture_receiver;
        bool source_type_mapped = capture_receiver;
        if (capture_receiver)
            source_type = function->capture_type_id;
        else
            source_type_mapped =
                map_logical_value_type(context, function->xi, typed_value, &source_type) &&
                source_type != XR_CORE_TYPE_VOID;
        bool erased_error_catch =
            value->op == XI_ERR_CATCH && value->type && value->type->kind == XR_KIND_UNKNOWN;
        XrCoreIrOwnershipDisposition expected_ownership =
            available->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT ||
                    available->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_ERROR ||
                    available->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_PANIC
                ? logical_ownership_for_type(context, available->type_id)
            : logical_value_produces_owner(context, function->xi, value, 0u)
                ? logical_ownership_for_type(context, available->type_id)
                : XR_CORE_IR_NON_OWNER;
        if (available->phi || (source_type_mapped && source_type != available->type_id) ||
            (!source_type_mapped && !erased_error_catch) ||
            available->category != logical_value_category(function->xi, value) ||
            available->ownership != expected_ownership)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi live-in v%u has a conflicting existing block argument", value->id);
        return XR_PROGRAM_BUILD_OK;
    }
    if (block->xi == function->xi->entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi entry block depends on non-parameter v%u", value->id);
    uint16_t type_override = XR_CORE_TYPE_VOID;
    XrXiBlockStorage *definition = find_block_storage(function, value->block);
    XrXiBlockArgumentStorage *definition_argument = find_block_argument(definition, value);
    if (definition_argument)
        type_override = definition_argument->type_id;
    return add_block_argument(context, function, block, typed_value, NULL, type_override,
                              XR_XI_INVOKE_ARGUMENT_NONE, changed, diagnostic, diagnostic_size);
}

static XrProgramBuildStatus collect_value_live_ins(XrXiBuildContext *context,
                                                   XrXiFunctionStorage *function,
                                                   XrXiBlockStorage *block, const XiValue *value,
                                                   bool *changed, char *diagnostic,
                                                   size_t diagnostic_size) {
    if (value->op == XI_LOAD_UPVAL) {
        if (value->aux_int < 0 || value->aux_int >= function->xi->ncaptures ||
            function->capture_type_id == XR_CORE_TYPE_VOID)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi upvalue load v%u has no canonical capture receiver", value->id);
        XrProgramBuildStatus status =
            require_value_available(context, function, block, &function->capture_receiver, changed,
                                    diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    XiValue *aggregate_fields[XR_MAX_AGG_FIELDS];
    uint32_t aggregate_field_count = 0u;
    if (resolved_value_aggregate_construction(context, function->xi, value, aggregate_fields,
                                              &aggregate_field_count)) {
        for (uint32_t field = 0u; field < aggregate_field_count; ++field) {
            XrProgramBuildStatus status =
                require_value_available(context, function, block, aggregate_fields[field], changed,
                                        diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        return XR_PROGRAM_BUILD_OK;
    }
    const XiFunc *sealed_callee =
        value->op == XI_CALL || value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT
            ? resolved_sealed_callee(context, function->xi, value)
            : NULL;
    bool erased_first_operand =
        value->op == XI_VARIANT_CONSTRUCT ||
        resolved_provider_native_call(context, function->xi, value) ||
        resolved_suspension_native_call(context, function->xi, value) ||
        (sealed_callee && !sealed_callee->has_receiver) ||
        resolved_canonical_class_construction(context, function->xi, value, NULL, NULL) ||
        resolved_empty_struct_literal(context, function->xi, value) ||
        resolved_unit_enum_literal(context, function->xi, value, NULL);
    uint16_t begin = erased_first_operand ? 1u : 0u;
    for (uint16_t argument = begin; argument < value->nargs; ++argument) {
        XrProgramBuildStatus status = require_value_available(
            context, function, block, value->args[argument], changed, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    return XR_PROGRAM_BUILD_OK;
}

static int block_argument_compare(const void *left, const void *right) {
    const XrXiBlockArgumentStorage *a = left;
    const XrXiBlockArgumentStorage *b = right;
    if (a->implicit_invoke_kind != b->implicit_invoke_kind) {
        if (a->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_NONE)
            return 1;
        if (b->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_NONE)
            return -1;
        return a->implicit_invoke_kind < b->implicit_invoke_kind ? -1 : 1;
    }
    return memcmp(a->key.bytes, b->key.bytes, sizeof(a->key.bytes));
}

static const XiValue *edge_argument_value(const XrXiBlockArgumentStorage *argument,
                                          const XiBlock *predecessor, const XiBlock *successor,
                                          uint32_t predecessor_occurrence) {
    if (!argument->phi)
        return argument->source;
    uint32_t occurrence = 0u;
    for (uint16_t index = 0; index < successor->npreds; ++index) {
        if (successor->preds[index] != predecessor)
            continue;
        if (occurrence++ == predecessor_occurrence)
            return index < argument->phi->value.nargs ? argument->phi->value.args[index] : NULL;
    }
    return NULL;
}

static XrProgramBuildStatus
record_static_typed_catch_outcome(XrXiBuildContext *context, XrXiBlockStorage *catch_block,
                                  const XiValue *caught, uint16_t caught_type, char *diagnostic,
                                  size_t diagnostic_size) {
    const XiBlock *block = catch_block ? catch_block->xi : NULL;
    const XiValue *test = block ? block->control : NULL;
    if (!value_is_static_typed_catch_test(test) || logical_value_identity(test->args[0]) != caught)
        return XR_PROGRAM_BUILD_OK;
    if (!static_typed_catch_contract_is_exact(context, test))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi typed catch test v%u has inconsistent nominal token evidence", test->id);
    uint16_t target_type = XR_CORE_TYPE_VOID;
    if (!map_type(context, (const XrType *) test->aux, &target_type) ||
        target_type == XR_CORE_TYPE_VOID || !find_dynamic_type_by_id(context, target_type) ||
        find_dynamic_type_by_id(context, target_type)->input.kind != XR_CORE_IR_TYPE_VARIANT)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi typed catch test v%u has no exact closed error contract", test->id);
    uint8_t outcome =
        caught_type == target_type ? XR_XI_STATIC_BRANCH_TRUE : XR_XI_STATIC_BRANCH_FALSE;
    if (catch_block->static_branch_outcome != XR_XI_STATIC_BRANCH_UNKNOWN &&
        catch_block->static_branch_outcome != outcome)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi typed catch block b%u has conflicting exact error channels", block->id);
    catch_block->static_branch_outcome = outcome;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus prepare_invoke_arguments(XrXiBuildContext *context,
                                                     XrXiFunctionStorage *function,
                                                     char *diagnostic, size_t diagnostic_size) {
    for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
        const XiBlock *source = function->xi->blocks[block_index];
        XrXiBlockStorage *predecessor = find_block_storage(function, source);
        if (!predecessor || !predecessor->reachable)
            continue;
        const XiValue *call = block_typed_invoke_call(context, function->xi, source);
        if (!call)
            continue;
        XrProgramBuildStatus operand_status = collect_value_live_ins(
            context, function, predecessor, call, NULL, diagnostic, diagnostic_size);
        if (operand_status != XR_PROGRAM_BUILD_OK)
            return operand_status;
        const XiFunc *callee = resolved_sealed_callee(context, function->xi, call);
        bool witness = call->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE;
        bool indirect = callee == NULL && !witness;
        const XgCallsiteSummary *callsite =
            witness ? resolved_witness_callsite(context, function->xi, call)
                    : resolved_callsite(context, function->xi, call);
        if (!callsite || (callsite->flags & (XG_CALL_ERROR_EFFECT_VERIFIED | XG_CALL_MAY_ERROR)) !=
                             (XG_CALL_ERROR_EFFECT_VERIFIED | XG_CALL_MAY_ERROR))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi invoke b%u lacks an exact fallible call target set", source->id);
        uint16_t error_type = XR_CORE_TYPE_VOID;
        uint16_t panic_type = XR_CORE_TYPE_VOID;
        uint16_t callable_type = XR_CORE_TYPE_VOID;
        bool witness_receiver_consumed = false;
        XrProgramBuildStatus status = XR_PROGRAM_BUILD_OK;
        if (witness) {
            const XrCoreIrCallableSignatureInput *slot = interface_slot_contract(
                context, call->xg_interface_id, call->xg_interface_dispatch_slot);
            if (!slot || call->nargs != slot->parameter_count)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi witness invoke b%u has no exact interface slot", source->id);
            error_type = slot->error_type_id;
            panic_type = slot->panic_type_id;
            witness_receiver_consumed = slot->parameter_count != 0u && slot->parameter_modes &&
                                        slot->parameter_modes[0] == XR_PARAM_MOVE;
        } else if (indirect) {
            const XiValue *callable =
                call->nargs != 0u ? logical_value_identity(call->args[0]) : NULL;
            if (!callable || !map_callable_call_type(context, function->xi, call, callable->type,
                                                     &callable_type, NULL))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi invoke b%u has no exact callable signature", source->id);
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, callable_type);
            if (!type || !type->callable_signature)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi invoke b%u callable signature is absent", source->id);
            error_type = type->callable_signature->error_type_id;
            panic_type = type->callable_signature->panic_type_id;
        } else {
            status =
                map_function_error_type(context, callee, &error_type, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            status =
                map_function_panic_type(context, callee, &panic_type, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        if (error_type == XR_CORE_TYPE_VOID)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi error check in b%u guards an infallible call", source->id);
        if (panic_type != XR_CORE_TYPE_VOID)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi invoke b%u has no explicit panic continuation", source->id);
        XrXiBlockStorage *normal = find_block_storage(function, source->succs[1]);
        XrXiBlockStorage *error = find_block_storage(function, source->succs[0]);
        const XiBlock *catch_block_xi = NULL;
        const XiValue *caught = routed_error_catch(function, source->succs[0], &catch_block_xi);
        XrXiBlockStorage *catch_block = find_block_storage(function, catch_block_xi);
        if (!predecessor || !normal || !error || !caught || !catch_block)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi invoke b%u does not expose exact typed normal/error continuations",
                        source->id);
        const XiErrorRegion *error_region = source->control->error_region;
        const XiErrorRegion *lexical_region = NULL;
        status = nearest_error_region(function->xi, source->control, &lexical_region, diagnostic,
                                      diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
        if (error_region != lexical_region || caught->error_region != lexical_region ||
            (lexical_region &&
             (!error_region_contract_is_exact(function->xi, lexical_region, caught) ||
              lexical_region->catch_block != catch_block_xi ||
              lexical_region->catch_value != caught)))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi invoke b%u disagrees with its nearest lexical error region",
                        source->id);
        uint16_t caught_type = error_type;
        uint16_t result_type = XR_CORE_TYPE_VOID;
        if (!map_logical_value_type(context, function->xi, call, &result_type))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi invoke call v%u result type is not active in CoreSpec", call->id);
        if (witness) {
            const XrCoreIrCallableSignatureInput *slot = interface_slot_contract(
                context, call->xg_interface_id, call->xg_interface_dispatch_slot);
            if (!slot || result_type != slot->result_type_id)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi witness invoke v%u result disagrees with its interface slot",
                            call->id);
        }
        if (result_type != XR_CORE_TYPE_VOID) {
            status = add_block_argument(context, function, normal, call, NULL, result_type,
                                        XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT, NULL, diagnostic,
                                        diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        status = add_block_argument(context, function, error, caught, NULL, caught_type,
                                    XR_XI_INVOKE_ARGUMENT_ERROR, NULL, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
        if (catch_block != error) {
            status =
                add_block_argument(context, function, catch_block, caught, NULL, caught_type,
                                   XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        status = record_static_typed_catch_outcome(context, catch_block, caught, caught_type,
                                                   diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
        uint16_t first_operand = indirect || witness || (callee && callee->has_receiver) ? 0u : 1u;
        for (uint16_t operand = first_operand; operand < call->nargs; ++operand) {
            status = require_value_available(context, function, predecessor, call->args[operand],
                                             NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        if ((indirect || witness) && !witness_receiver_consumed) {
            const XiValue *owner = logical_value_identity(call->args[0]);
            if (logical_value_produces_owner(context, function->xi, owner, 0u)) {
                uint16_t owner_type = callable_type;
                if (witness && !map_logical_value_type(context, function->xi, owner, &owner_type))
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi witness invoke v%u receiver type is unavailable", call->id);
                status = add_block_argument(context, function, normal, owner, NULL, owner_type,
                                            XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic,
                                            diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
                status = add_block_argument(context, function, error, owner, NULL, owner_type,
                                            XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic,
                                            diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static uint32_t canonical_block_successor_count(const XrXiBuildContext *context,
                                                const XiFunc *function, const XiBlock *block) {
    if (!block)
        return 0u;
    if (exact_infallible_class_construction_in_block(context, function, block))
        return 1u;
    if (block->kind == XI_BLOCK_PLAIN)
        return block->succs[0] ? 1u : 0u;
    if (block->kind == XI_BLOCK_IF) {
        const XrXiFunctionStorage *function_storage =
            find_xi_function(context, function, NULL, NULL);
        const XrXiBlockStorage *block_storage =
            function_storage ? find_block_storage(function_storage, block) : NULL;
        if (block_storage && block_storage->static_branch_outcome != XR_XI_STATIC_BRANCH_UNKNOWN)
            return 1u;
        return block->succs[0] && block->succs[1] ? 2u : 0u;
    }
    return 0u;
}

static const XiBlock *canonical_block_successor(const XrXiBuildContext *context,
                                                const XiFunc *function, const XiBlock *block,
                                                uint32_t index) {
    if (exact_infallible_class_construction_in_block(context, function, block))
        return index == 0u ? block->succs[1] : NULL;
    const XrXiFunctionStorage *function_storage = find_xi_function(context, function, NULL, NULL);
    const XrXiBlockStorage *block_storage =
        function_storage ? find_block_storage(function_storage, block) : NULL;
    if (block && block->kind == XI_BLOCK_IF && block_storage && index == 0u &&
        block_storage->static_branch_outcome != XR_XI_STATIC_BRANCH_UNKNOWN)
        return block_storage->static_branch_outcome == XR_XI_STATIC_BRANCH_TRUE ? block->succs[0]
                                                                                : block->succs[1];
    return block && index < canonical_block_successor_count(context, function, block)
               ? block->succs[index]
               : NULL;
}

/* An affine owner cannot be transferred on only one side of a branch.  When
 * any successor needs it, every successor receives it; paths with no semantic
 * use close the owner with an explicit drop in that successor. */
static XrProgramBuildStatus balance_owner_successor_arguments(XrXiBuildContext *context,
                                                              XrXiFunctionStorage *function,
                                                              bool *changed, char *diagnostic,
                                                              size_t diagnostic_size) {
    for (uint32_t predecessor_index = 0u; predecessor_index < function->xi->nblocks;
         ++predecessor_index) {
        const XiBlock *predecessor = function->xi->blocks[predecessor_index];
        const XrXiBlockStorage *predecessor_storage = find_block_storage(function, predecessor);
        if (!predecessor_storage || !predecessor_storage->reachable)
            continue;
        uint32_t successor_count =
            canonical_block_successor_count(context, function->xi, predecessor);
        if (successor_count < 2u)
            continue;
        for (uint32_t source_edge = 0u; source_edge < successor_count; ++source_edge) {
            XrXiBlockStorage *source_successor =
                find_block_storage(function, canonical_block_successor(context, function->xi,
                                                                       predecessor, source_edge));
            if (!source_successor)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi CFG successor is absent");
            uint32_t argument_count = source_successor->argument_count;
            for (uint32_t argument_index = 0u; argument_index < argument_count; ++argument_index) {
                XrXiBlockArgumentStorage argument =
                    source_successor->argument_storage[argument_index];
                if (argument.implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE ||
                    argument.ownership != XR_CORE_IR_OWNER)
                    continue;
                uint32_t source_occurrence = 0u;
                for (uint32_t prior_edge = 0u; prior_edge < source_edge; ++prior_edge)
                    source_occurrence +=
                        canonical_block_successor(context, function->xi, predecessor, prior_edge) ==
                        source_successor->xi;
                const XiValue *incoming = edge_argument_value(
                    &argument, predecessor, source_successor->xi, source_occurrence);
                if (!incoming)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi affine edge argument has no incoming owner");
                for (uint32_t target_edge = 0u; target_edge < successor_count; ++target_edge) {
                    XrXiBlockStorage *target_successor = find_block_storage(
                        function,
                        canonical_block_successor(context, function->xi, predecessor, target_edge));
                    if (!target_successor)
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi CFG successor is absent");
                    XrProgramBuildStatus status = add_block_argument(
                        context, function, target_successor, incoming, NULL, argument.type_id,
                        XR_XI_INVOKE_ARGUMENT_NONE, changed, diagnostic, diagnostic_size);
                    if (status != XR_PROGRAM_BUILD_OK)
                        return status;
                }
            }
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus mark_reachable_blocks(const XrXiBuildContext *context,
                                                  XrXiFunctionStorage *function, char *diagnostic,
                                                  size_t diagnostic_size) {
    XrXiBlockStorage *entry =
        find_block_storage(function, function && function->xi ? function->xi->entry : NULL);
    if (!entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi function entry block is absent");
    entry->reachable = true;
    for (uint32_t iteration = 0u; iteration < function->xi->nblocks; ++iteration) {
        bool changed = false;
        for (uint32_t block_index = 0u; block_index < function->xi->nblocks; ++block_index) {
            XrXiBlockStorage *source = &function->block_storage[block_index];
            if (!source->reachable)
                continue;
            uint32_t successor_count =
                canonical_block_successor_count(context, function->xi, source->xi);
            for (uint32_t successor_index = 0u; successor_index < successor_count;
                 ++successor_index) {
                const XiBlock *successor_xi =
                    canonical_block_successor(context, function->xi, source->xi, successor_index);
                XrXiBlockStorage *successor = find_block_storage(function, successor_xi);
                if (!successor)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi reachable block b%u has an absent successor",
                                source->xi ? source->xi->id : UINT32_MAX);
                if (!successor->reachable) {
                    successor->reachable = true;
                    changed = true;
                }
            }
            for (uint32_t edge_index = 0u; edge_index < context->trap_edge_count; ++edge_index) {
                const XrXiTrapEdge *edge = &context->trap_edges[edge_index];
                if (edge->function != function->xi || !edge->call ||
                    edge->call->block != source->xi)
                    continue;
                XrXiBlockStorage *successor = find_block_storage(function, edge->handler);
                if (!successor)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi provider trap continuation is absent");
                if (!successor->reachable) {
                    successor->reachable = true;
                    changed = true;
                }
            }
            for (uint32_t edge_index = 0u; edge_index < context->panic_edge_count; ++edge_index) {
                const XrXiPanicEdge *edge = &context->panic_edges[edge_index];
                if (edge->function != function->xi || !edge->point ||
                    edge->point->block != source->xi)
                    continue;
                XrXiBlockStorage *successor = find_block_storage(function, edge->handler);
                if (!successor)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi panic continuation is absent");
                if (!successor->reachable) {
                    successor->reachable = true;
                    changed = true;
                }
            }
            for (uint32_t edge_index = 0u; edge_index < context->cancel_edge_count; ++edge_index) {
                const XrXiCancelEdge *edge = &context->cancel_edges[edge_index];
                if (edge->function != function->xi || !edge->point || !edge->point->op ||
                    edge->point->op->block != source->xi)
                    continue;
                XrXiBlockStorage *successor = find_block_storage(function, edge->handler);
                if (!successor)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi cancel continuation is absent");
                if (!successor->reachable) {
                    successor->reachable = true;
                    changed = true;
                }
            }
        }
        if (!changed)
            return XR_PROGRAM_BUILD_OK;
    }
    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "Xi function block reachability did not converge");
}

static bool static_cleanup_handler_graph_is_exact(const XiFunc *function,
                                                  const XiValue *registration, uint8_t *members,
                                                  uint32_t *stack, uint8_t *completes) {
    if (!exact_static_cleanup_try(function, registration) || !members || !stack || !completes)
        return false;
    const XiBlock *handler = (const XiBlock *) registration->aux;
    if (!handler || handler->id >= function->nblocks || function->blocks[handler->id] != handler)
        return false;

    const XiValue *caught = NULL;
    for (uint32_t index = 0u; index < handler->nvalues; ++index) {
        const XiValue *value = handler->values[index];
        if (!value || value->op != XI_CATCH)
            continue;
        if (caught || value->aux != registration || value->nargs != 0u)
            return false;
        caught = value;
    }
    if (!caught)
        return false;

    uint32_t stack_count = 0u;
    uint32_t member_count = 0u;
    uint32_t enter_count = 0u;
    uint32_t leave_count = 0u;
    uint32_t terminal = UINT32_MAX;
    members[handler->id] = 2u;
    stack[stack_count++] = handler->id;
    while (stack_count != 0u) {
        uint32_t block_index = stack[--stack_count];
        if (block_index >= function->nblocks || members[block_index] != 2u)
            return false;
        const XiBlock *block = function->blocks[block_index];
        if (!block || block->id != block_index || block->func != function)
            return false;
        members[block_index] = 1u;
        ++member_count;

        const XiValue *throw_value = NULL;
        for (uint32_t value_index = 0u; value_index < block->nvalues; ++value_index) {
            const XiValue *value = block->values[value_index];
            if (!value)
                continue;
            if (value->op == XI_CATCH) {
                if (block != handler || value != caught)
                    return false;
            } else if (value->op == XI_THROW) {
                if (throw_value || value->nargs != 1u || !value->args || value->args[0] != caught)
                    return false;
                throw_value = value;
            } else if (value->op == XI_CLEANUP_ENTER) {
                if (!exact_cleanup_boundary_marker(value))
                    return false;
                ++enter_count;
            } else if (value->op == XI_CLEANUP_LEAVE) {
                if (!exact_cleanup_boundary_marker(value))
                    return false;
                ++leave_count;
            }
        }

        uint32_t successor_count = 0u;
        if (block->kind == XI_BLOCK_PLAIN) {
            if (!block->succs[0] || block->succs[1])
                return false;
            successor_count = 1u;
        } else if (block->kind == XI_BLOCK_IF) {
            if (!block->control || !block->succs[0] || !block->succs[1])
                return false;
            successor_count = 2u;
        } else if (block->kind == XI_BLOCK_UNREACHABLE) {
            if (!throw_value || block->succs[0] || block->succs[1] || terminal != UINT32_MAX)
                return false;
            terminal = block_index;
        } else {
            return false;
        }
        if (throw_value && block->kind != XI_BLOCK_UNREACHABLE)
            return false;
        for (uint32_t successor = 0u; successor < successor_count; ++successor) {
            const XiBlock *target = block->succs[successor];
            if (!target || target->id >= function->nblocks ||
                function->blocks[target->id] != target ||
                (members[target->id] == 0u && stack_count >= function->nblocks))
                return false;
            if (members[target->id] == 0u) {
                members[target->id] = 2u;
                stack[stack_count++] = target->id;
            }
        }
    }
    if (member_count == 0u || terminal == UINT32_MAX || enter_count == 0u ||
        enter_count != leave_count)
        return false;

    completes[terminal] = 1u;
    for (uint32_t round = 0u; round < member_count; ++round) {
        bool changed = false;
        for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
            if (!members[block_index] || completes[block_index])
                continue;
            const XiBlock *block = function->blocks[block_index];
            uint32_t successor_count = block->kind == XI_BLOCK_IF ? 2u : 1u;
            bool complete = true;
            for (uint32_t successor = 0u; successor < successor_count; ++successor)
                complete &=
                    members[block->succs[successor]->id] && completes[block->succs[successor]->id];
            if (complete) {
                completes[block_index] = 1u;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index)
        if (members[block_index] && !completes[block_index])
            return false;
    return true;
}

static XrProgramBuildStatus
claim_static_cleanup_handler_graph(XrXiFunctionStorage *function, const XiValue *registration,
                                   XrXiCleanupReason reason, bool *private_projection,
                                   char *diagnostic, size_t diagnostic_size) {
    if (private_projection)
        *private_projection = false;
    if (!function || !function->xi || !registration || !private_projection ||
        reason < XR_XI_CLEANUP_REASON_TRAP || reason > XR_XI_CLEANUP_REASON_CANCEL)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint32_t count = function->xi->nblocks;
    uint8_t *members = count ? xr_calloc(count, sizeof(*members)) : NULL;
    uint32_t *stack = count ? xr_calloc(count, sizeof(*stack)) : NULL;
    uint8_t *completes = count ? xr_calloc(count, sizeof(*completes)) : NULL;
    if (count && (!members || !stack || !completes)) {
        xr_free(completes);
        xr_free(stack);
        xr_free(members);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    if (!static_cleanup_handler_graph_is_exact(function->xi, registration, members, stack,
                                               completes)) {
        xr_free(completes);
        xr_free(stack);
        xr_free(members);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi static cleanup handler is not an exact closed control-flow graph");
    }

    uint32_t trap_count = 0u;
    uint32_t panic_count = 0u;
    uint32_t cancel_count = 0u;
    uint32_t member_count = 0u;
    bool reachable = false;
    for (uint32_t block_index = 0u; block_index < count; ++block_index) {
        if (!members[block_index])
            continue;
        XrXiBlockStorage *block = &function->block_storage[block_index];
        ++member_count;
        trap_count += block->trap_cleanup ? 1u : 0u;
        panic_count += block->panic_cleanup ? 1u : 0u;
        cancel_count += block->cancel_cleanup ? 1u : 0u;
        reachable |= block->reachable;
    }
    bool has_trap = trap_count != 0u;
    bool has_panic = panic_count != 0u;
    bool has_cancel = cancel_count != 0u;
    if ((has_trap && trap_count != member_count) || (has_panic && panic_count != member_count) ||
        (has_cancel && cancel_count != member_count)) {
        xr_free(completes);
        xr_free(stack);
        xr_free(members);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi static cleanup graph has inconsistent reason ownership");
    }
    if (has_panic && reason != XR_XI_CLEANUP_REASON_PANIC) {
        xr_free(completes);
        xr_free(stack);
        xr_free(members);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi static cleanup graph already owns a panic reason");
    }
    if (reason == XR_XI_CLEANUP_REASON_CANCEL && (has_trap || has_cancel)) {
        *private_projection = true;
    } else if ((reason == XR_XI_CLEANUP_REASON_TRAP && (has_panic || has_cancel)) ||
               (reason == XR_XI_CLEANUP_REASON_PANIC && (has_trap || has_cancel))) {
        xr_free(completes);
        xr_free(stack);
        xr_free(members);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi static cleanup graph requires multiple reason-private owners");
    } else if (!has_trap && !has_panic && !has_cancel) {
        if (reachable) {
            xr_free(completes);
            xr_free(stack);
            xr_free(members);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi static cleanup graph is already reachable without an exact reason");
        }
        for (uint32_t block_index = 0u; block_index < count; ++block_index) {
            if (!members[block_index])
                continue;
            XrXiBlockStorage *block = &function->block_storage[block_index];
            block->trap_cleanup = reason == XR_XI_CLEANUP_REASON_TRAP;
            block->panic_cleanup = reason == XR_XI_CLEANUP_REASON_PANIC;
            block->cancel_cleanup = reason == XR_XI_CLEANUP_REASON_CANCEL;
        }
    }
    xr_free(completes);
    xr_free(stack);
    xr_free(members);
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus append_trap_edge(XrXiBuildContext *context, const XiFunc *function,
                                             const XiValue *call, const XiValue *registration) {
    if (context->trap_edge_count == context->trap_edge_capacity) {
        uint32_t capacity = context->trap_edge_capacity ? context->trap_edge_capacity * 2u : 4u;
        if (capacity < context->trap_edge_capacity)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        XrXiTrapEdge *edges = xr_realloc(context->trap_edges, (size_t) capacity * sizeof(*edges));
        if (!edges)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        context->trap_edges = edges;
        context->trap_edge_capacity = capacity;
    }
    context->trap_edges[context->trap_edge_count++] = (XrXiTrapEdge) {
        .function = function,
        .call = call,
        .registration = registration,
        .handler = (const XiBlock *) registration->aux,
    };
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus append_panic_edge(XrXiBuildContext *context, const XiFunc *function,
                                              const XiValue *point, const XiValue *registration) {
    if (context->panic_edge_count == context->panic_edge_capacity) {
        uint32_t capacity = context->panic_edge_capacity ? context->panic_edge_capacity * 2u : 4u;
        if (capacity < context->panic_edge_capacity)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        XrXiPanicEdge *edges = xr_realloc(context->panic_edges, (size_t) capacity * sizeof(*edges));
        if (!edges)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        context->panic_edges = edges;
        context->panic_edge_capacity = capacity;
    }
    context->panic_edges[context->panic_edge_count++] = (XrXiPanicEdge) {
        .function = function,
        .point = point,
        .registration = registration,
        .handler = (const XiBlock *) registration->aux,
    };
    return XR_PROGRAM_BUILD_OK;
}

static bool exact_condition_assertion(const XiValue *value) {
    const XrAssertionPlan *plan = xi_assertion_plan(value);
    return value && value->op == XI_ASSERTION && value->nargs == 1u && value->args &&
           value->args[0] && plan && xr_assertion_plan_validate(plan) &&
           plan->kind == XR_ASSERTION_KIND_CONDITION && plan->arity == 1u &&
           plan->message_operand == XR_ASSERTION_OPERAND_NONE;
}

static bool exact_indirect_direct_call(XrXiBuildContext *context, const XiFunc *function,
                                       const XiValue *call) {
    if (!call || call->op != XI_CALL || call->nargs == 0u || !call->args ||
        resolved_sealed_callee(context, function, call))
        return false;
    const XgCallsiteSummary *callsite = resolved_callsite(context, function, call);
    if (!callsite || (callsite->kind != XG_CALL_DIRECT_FUNC && callsite->kind != XG_CALL_CLOSURE) ||
        (callsite->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u ||
        (callsite->flags & XG_CALL_MAY_ERROR) != 0u)
        return false;
    /* Trap-edge discovery runs before canonical callable TypeIds are
     * materialized.  The
     * closed Xg callsite contract is the phase-correct
     * admission fact here; translation
     * later validates its exact callable
     * signature and result type. */
    return true;
}

static bool exact_indirect_invoke_call(XrXiBuildContext *context, const XiFunc *function,
                                       const XiValue *call) {
    if (!call || !call->block || call->op != XI_CALL || call->nargs == 0u || !call->args ||
        resolved_sealed_callee(context, function, call) ||
        !typed_invoke_check_block_for_call(context, function, call))
        return false;
    const XgCallsiteSummary *callsite = resolved_callsite(context, function, call);
    return callsite &&
           (callsite->kind == XG_CALL_DIRECT_FUNC || callsite->kind == XG_CALL_CLOSURE) &&
           (callsite->flags & (XG_CALL_ERROR_EFFECT_VERIFIED | XG_CALL_MAY_ERROR)) ==
               (XG_CALL_ERROR_EFFECT_VERIFIED | XG_CALL_MAY_ERROR) &&
           resolved_callable_call_targets(context, function, call, NULL);
}

static bool exact_witness_direct_call(const XrXiBuildContext *context, const XiFunc *function,
                                      const XiValue *call) {
    return call && (call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT) &&
           call->xg_existential_kind == XI_EXISTENTIAL_WITNESS_DIRECT &&
           resolved_witness_callsite(context, function, call) != NULL;
}

static bool exact_witness_invoke_call(const XrXiBuildContext *context, const XiFunc *function,
                                      const XiValue *call) {
    return call && call->block &&
           (call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT) &&
           call->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE &&
           typed_invoke_check_block_for_call(context, function, call) &&
           resolved_witness_callsite(context, function, call) != NULL;
}

static XrProgramBuildStatus prepare_trap_continuations(XrXiBuildContext *context, char *diagnostic,
                                                       size_t diagnostic_size) {
    for (uint32_t module_index = 0u; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            XrXiFunctionStorage *storage = &module->function_storage[function_index];
            const XiFunc *function = storage->xi;
            for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
                const XiBlock *block = function->blocks[block_index];
                if (!storage->block_storage[block_index].reachable &&
                    !storage->block_storage[block_index].cleanup_entry_ready)
                    continue;
                for (uint32_t value_index = 0u; value_index < block->nvalues; ++value_index) {
                    const XiValue *call = block->values[value_index];
                    bool provider_call = resolved_provider_native_call(context, function, call);
                    bool witness_invoke =
                        provider_call ? false : exact_witness_invoke_call(context, function, call);
                    bool witness_call = provider_call
                                            ? false
                                            : (witness_invoke ||
                                               exact_witness_direct_call(context, function, call));
                    const XiFunc *sealed_callee =
                        provider_call || witness_call
                            ? NULL
                            : resolved_sealed_callee(context, function, call);
                    bool sealed_invoke =
                        sealed_callee && typed_invoke_check_block_for_call(context, function, call);
                    bool indirect_invoke =
                        provider_call || witness_call || sealed_callee
                            ? false
                            : exact_indirect_invoke_call(context, function, call);
                    bool indirect_call = provider_call || witness_call || sealed_callee
                                             ? false
                                             : (indirect_invoke || exact_indirect_direct_call(
                                                                       context, function, call));
                    if ((!provider_call && !witness_call && !sealed_callee && !indirect_call) ||
                        (typed_invoke_check_block_for_call(context, function, call) &&
                         !witness_invoke && !sealed_invoke && !indirect_invoke) ||
                        resolved_canonical_class_construction(context, function, call, NULL, NULL))
                        continue;
                    const XrXiCleanupRegistrationStorage *active_context = NULL;
                    if (!active_cleanup_registration_at_program_point(storage, call,
                                                                      &active_context))
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi trap-capable call v%u has no exact cleanup program point",
                                    call->id);
                    if (!active_context)
                        continue;
                    if (active_context->outer_handler_state != 0u)
                        return fail(
                            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi trap-capable call v%u has nested static cleanup regions", call->id);
                    const XiValue *active = active_context->registration;
                    const XiBlock *handler = (const XiBlock *) active->aux;
                    XrXiBlockStorage *handler_storage = find_block_storage(storage, handler);
                    if (!handler_storage)
                        return fail(
                            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi trap-capable call v%u has no exact private cleanup", call->id);
                    bool private_projection = false;
                    XrProgramBuildStatus status = claim_static_cleanup_handler_graph(
                        storage, active, XR_XI_CLEANUP_REASON_TRAP, &private_projection, diagnostic,
                        diagnostic_size);
                    if (status != XR_PROGRAM_BUILD_OK)
                        return status;
                    if (private_projection)
                        return fail(
                            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi trap-capable call v%u has no exact private cleanup", call->id);
                    status = append_trap_edge(context, function, call, active);
                    if (status != XR_PROGRAM_BUILD_OK)
                        return status;
                }
            }
        }
    }
    for (uint32_t module_index = 0u; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            XrXiFunctionStorage *storage = &module->function_storage[function_index];
            for (uint32_t block_index = 0u; block_index < storage->xi->nblocks; ++block_index)
                storage->block_storage[block_index].reachable = false;
            XrProgramBuildStatus status =
                mark_reachable_blocks(context, storage, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus prepare_panic_continuations(XrXiBuildContext *context, char *diagnostic,
                                                        size_t diagnostic_size) {
    for (uint32_t module_index = 0u; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            XrXiFunctionStorage *storage = &module->function_storage[function_index];
            const XiFunc *function = storage->xi;
            for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
                const XiBlock *block = function->blocks[block_index];
                if (!storage->block_storage[block_index].reachable &&
                    !storage->block_storage[block_index].cleanup_entry_ready)
                    continue;
                for (uint32_t value_index = 0u; value_index < block->nvalues; ++value_index) {
                    const XiValue *point = block->values[value_index];
                    if (!exact_condition_assertion(point))
                        continue;
                    const XrXiCleanupRegistrationStorage *active_context = NULL;
                    if (!active_cleanup_registration_at_program_point(storage, point,
                                                                      &active_context))
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi assertion v%u has no exact cleanup program point",
                                    point->id);
                    if (!active_context)
                        continue;
                    if (active_context->outer_handler_state != 0u)
                        return fail(
                            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi assertion v%u has nested static cleanup regions", point->id);
                    const XiValue *active = active_context->registration;
                    const XiBlock *handler = (const XiBlock *) active->aux;
                    XrXiBlockStorage *handler_storage = find_block_storage(storage, handler);
                    if (!handler_storage)
                        return fail(diagnostic, diagnostic_size,
                                    XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                    "Xi assertion v%u has no exact panic cleanup", point->id);
                    bool private_projection = false;
                    XrProgramBuildStatus status = claim_static_cleanup_handler_graph(
                        storage, active, XR_XI_CLEANUP_REASON_PANIC, &private_projection,
                        diagnostic, diagnostic_size);
                    if (status != XR_PROGRAM_BUILD_OK)
                        return status;
                    if (private_projection)
                        return fail(diagnostic, diagnostic_size,
                                    XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                    "Xi assertion v%u has no exact panic cleanup", point->id);
                    status = append_panic_edge(context, function, point, active);
                    if (status != XR_PROGRAM_BUILD_OK)
                        return status;
                }
            }
        }
    }
    for (uint32_t module_index = 0u; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            XrXiFunctionStorage *storage = &module->function_storage[function_index];
            for (uint32_t block_index = 0u; block_index < storage->xi->nblocks; ++block_index)
                storage->block_storage[block_index].reachable = false;
            XrProgramBuildStatus status =
                mark_reachable_blocks(context, storage, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus prepare_cancel_continuations(XrXiBuildContext *context,
                                                         char *diagnostic, size_t diagnostic_size) {
    for (uint32_t module_index = 0u; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            XrXiFunctionStorage *storage = &module->function_storage[function_index];
            const XiFunc *function = storage->xi;
            const XiCoroPlan *plan = function ? function->coro_plan : NULL;
            if (!plan || !plan->is_coroutine || !plan->analysis_complete || !plan->points ||
                !plan->actions_materialized || !plan->cfg_rewritten ||
                !xi_coro_plan_is_current(function, plan))
                continue;
            const XiFunc *suspend_stack[64] = {0};
            if (!coroutine_function_has_proven_suspend(context, function, suspend_stack, 0u))
                continue;
            for (uint32_t point_index = 0u; point_index < plan->nstates; ++point_index) {
                const XiCoroSuspendPoint *point = &plan->points[point_index];
                const XrXiCleanupRegistrationStorage *active_context = NULL;
                if (!active_cleanup_registration_at_program_point(storage, point->op,
                                                                  &active_context))
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi suspension v%u has no exact cleanup program point",
                                point->op ? point->op->id : UINT32_MAX);
                if (point->active_handler_count == 0u) {
                    if (active_context)
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi suspension v%u omitted its active cleanup handler",
                                    point->op ? point->op->id : UINT32_MAX);
                    continue;
                }
                if (point->active_handler_count != 1u || !point->active_handlers ||
                    !point->active_handlers[0] || !active_context ||
                    active_context->registration != point->active_handlers[0] ||
                    active_context->outer_handler_state != 0u)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi suspension v%u has nested cancellation cleanup",
                                point->op ? point->op->id : UINT32_MAX);
                const XiValue *registration = point->active_handlers[0];
                const XrXiCleanupRegistrationStorage *registration_context =
                    cleanup_registration_storage(storage, registration);
                const XiBlock *handler = (const XiBlock *) registration->aux;
                XrXiBlockStorage *handler_storage = find_block_storage(storage, handler);
                if (!registration_context || registration_context != active_context ||
                    !handler_storage)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi suspension v%u has no exact private cancel cleanup",
                                point->op ? point->op->id : UINT32_MAX);
                bool private_projection = false;
                XrProgramBuildStatus status = claim_static_cleanup_handler_graph(
                    storage, registration, XR_XI_CLEANUP_REASON_CANCEL, &private_projection,
                    diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
                status =
                    append_cancel_edge(context, function, point, registration, private_projection);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        }
    }
    for (uint32_t module_index = 0u; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            XrXiFunctionStorage *storage = &module->function_storage[function_index];
            for (uint32_t block_index = 0u; block_index < storage->xi->nblocks; ++block_index)
                storage->block_storage[block_index].reachable = false;
            XrProgramBuildStatus status =
                mark_reachable_blocks(context, storage, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus initialize_canonical_reachability(XrXiBuildContext *context,
                                                              char *diagnostic,
                                                              size_t diagnostic_size) {
    for (uint32_t module_index = 0u; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            XrXiFunctionStorage *storage = &module->function_storage[function_index];
            const XiFunc *function = storage->xi;
            if (!function || function->stage != XI_STAGE_OPTIMIZED || function->semantic_plan ||
                function->nblocks == 0u || !function->entry)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi function %u is not an Optimized program input", function_index);
            storage->block_storage = xr_calloc(function->nblocks, sizeof(*storage->block_storage));
            if (!storage->block_storage)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            uint32_t entry_count = 0u;
            for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
                const XiBlock *block = function->blocks[block_index];
                if (!block || block->id != block_index)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi function %u block table is not dense and canonical",
                                function_index);
                for (uint32_t prior = 0u; prior < block_index; ++prior)
                    if (function->blocks[prior] == block)
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi function %u block table contains a duplicate",
                                    function_index);
                entry_count += block == function->entry;
                storage->block_storage[block_index].xi = block;
            }
            if (entry_count != 1u)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi function %u must contain its entry exactly once", function_index);
            char cleanup_error[192];
            if (!xi_cleanup_verify(function, cleanup_error, sizeof(cleanup_error)))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi function %u has invalid cleanup identities: %s", function_index,
                            cleanup_error);
            XrProgramBuildStatus status =
                mark_reachable_blocks(context, storage, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus refine_static_typed_catch_reachability(XrXiBuildContext *context,
                                                                   char *diagnostic,
                                                                   size_t diagnostic_size) {
    for (uint32_t module_index = 0u; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0u; function_index < module->function_count;
             ++function_index) {
            XrXiFunctionStorage *function = &module->function_storage[function_index];
            XrProgramBuildStatus status =
                prepare_invoke_arguments(context, function, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            for (uint32_t block_index = 0u; block_index < function->xi->nblocks; ++block_index) {
                XrXiBlockStorage *block = &function->block_storage[block_index];
                xr_free(block->argument_storage);
                block->argument_storage = NULL;
                block->argument_count = 0u;
                block->argument_capacity = 0u;
                block->reachable = false;
            }
            status = mark_reachable_blocks(context, function, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static const XiValue *canonical_coroutine_live_identity(const XrXiBuildContext *context,
                                                        const XrXiFunctionStorage *function,
                                                        const XiCoroSuspendPoint *point,
                                                        const XiValue *value);

typedef struct XrXiTrapCallContract {
    const XrParamMode *parameter_modes;
    uint32_t parameter_count;
    uint32_t first_operand;
    bool indirect;
} XrXiTrapCallContract;

static bool trap_call_contract(XrXiBuildContext *context, const XrXiFunctionStorage *function,
                               const XiValue *call, XrXiTrapCallContract *contract);
static bool trap_call_operand_consumes(const XrXiTrapCallContract *contract, uint32_t operand);

static XrProgramBuildStatus prepare_coroutine_call_arguments(XrXiBuildContext *context,
                                                             XrXiFunctionStorage *function,
                                                             char *diagnostic,
                                                             size_t diagnostic_size) {
    const XiCoroPlan *plan = function && function->xi ? function->xi->coro_plan : NULL;
    for (uint32_t point_index = 0u; plan && point_index < plan->nstates; ++point_index) {
        const XiCoroSuspendPoint *point = &plan->points[point_index];
        if (point->kind != XI_CORO_SUSP_CALL)
            continue;
        if (point->op && typed_invoke_check_block_for_call(context, function->xi, point->op))
            continue;
        XrXiBlockStorage *resume = find_block_storage(function, point->resume_block);
        uint16_t result_type = XR_CORE_TYPE_VOID;
        if (!point->op || !resume ||
            !map_logical_value_type(context, function->xi, point->op, &result_type))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi coroutine call has no exact result continuation");
        const XiValue *logical_result =
            result_type == XR_CORE_TYPE_VOID
                ? NULL
                : exact_logical_value_identity(context, function->xi, point->op);
        if (logical_result) {
            XrXiBlockArgumentStorage *argument = find_block_argument(resume, logical_result);
            if (!argument) {
                XrProgramBuildStatus status = add_block_argument(
                    context, function, resume, point->op, NULL, result_type,
                    XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT, NULL, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            } else {
                if (argument->phi || argument->type_id != result_type ||
                    argument->category != XR_CORE_IR_VALUE)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi coroutine call result continuation is inconsistent");
                argument->implicit_invoke_kind = XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT;
                argument->ownership = logical_ownership_for_type(context, result_type);
            }
        }
        /* A READ call operand can borrow an affine owner for the entire child
         * execution
         * without creating an ordinary post-resume Xi use.  The
         * parent still owns that
         * frame anchor on both normal completion and
         * cancellation, so materialize only
         * such cancellation-only operands on
         * the resume edge.  MOVE operands belong to
         * the child; unrelated drops
         * are handled by their cleanup graph. */
        XrXiTrapCallContract contract;
        if (!resolved_suspension_native_call(context, function->xi, point->op) &&
            trap_call_contract(context, function, point->op, &contract)) {
            for (uint32_t operand = contract.first_operand; operand < point->op->nargs; ++operand) {
                const XiValue *source =
                    exact_logical_value_identity(context, function->xi, point->op->args[operand]);
                if (trap_call_operand_consumes(&contract, operand) ||
                    !logical_value_produces_owner(context, function->xi, source, 0u))
                    continue;
                const XiValue *logical =
                    canonical_coroutine_live_identity(context, function, point, source);
                uint32_t drop_matches = 0u;
                for (uint32_t drop = 0u; logical && drop < point->ndrops; ++drop)
                    drop_matches += canonical_coroutine_live_identity(
                                        context, function, point, point->drops[drop]) == logical;
                if (!logical || drop_matches != 1u)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi borrowed coroutine operand has no exact cancellation owner");
                XrProgramBuildStatus status = add_block_argument(
                    context, function, resume, logical, NULL, XR_CORE_TYPE_VOID,
                    XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static bool trap_call_contract(XrXiBuildContext *context, const XrXiFunctionStorage *function,
                               const XiValue *call, XrXiTrapCallContract *contract) {
    if (!context || !function || !call || !call->args || !contract)
        return false;
    memset(contract, 0, sizeof(*contract));
    bool witness_invoke = exact_witness_invoke_call(context, function->xi, call);
    if (witness_invoke || exact_witness_direct_call(context, function->xi, call)) {
        const XrCoreIrCallableSignatureInput *slot = interface_slot_contract(
            context, call->xg_interface_id, call->xg_interface_dispatch_slot);
        if (!slot || (!witness_invoke && (slot->error_type_id != XR_CORE_TYPE_VOID ||
                                          slot->panic_type_id != XR_CORE_TYPE_VOID)))
            return false;
        contract->parameter_modes = slot->parameter_modes;
        contract->parameter_count = slot->parameter_count;
    } else {
        const XiFunc *callee = resolved_sealed_callee(context, function->xi, call);
        if (callee) {
            const XrXiFunctionStorage *callee_storage =
                find_xi_function(context, callee, NULL, NULL);
            if (!callee_storage)
                return false;
            contract->first_operand = callee->has_receiver ? 0u : 1u;
            contract->parameter_modes = callee_storage->parameter_modes;
            contract->parameter_count =
                callee->nparams + (callee_storage->capture_type_id != XR_CORE_TYPE_VOID ? 1u : 0u);
        } else if (exact_indirect_invoke_call(context, function->xi, call) ||
                   exact_indirect_direct_call(context, function->xi, call)) {
            const XiValue *callable = logical_value_identity(call->args[0]);
            uint16_t callable_type_id = XR_CORE_TYPE_VOID;
            if (!callable || !map_callable_call_type(context, function->xi, call, callable->type,
                                                     &callable_type_id, NULL))
                return false;
            const XrXiTypeStorage *callable_type =
                find_dynamic_type_by_id(context, callable_type_id);
            if (!callable_type || !callable_type->callable_signature)
                return false;
            contract->parameter_modes = callable_type->callable_signature->parameter_modes;
            contract->parameter_count = callable_type->callable_signature->parameter_count;
            contract->indirect = true;
        } else {
            return false;
        }
    }
    uint32_t prefix = contract->first_operand + (contract->indirect ? 1u : 0u);
    return call->nargs == prefix + contract->parameter_count &&
           (contract->parameter_count == 0u || contract->parameter_modes);
}

static bool trap_call_operand_consumes(const XrXiTrapCallContract *contract, uint32_t operand) {
    uint32_t prefix = contract->first_operand + (contract->indirect ? 1u : 0u);
    return operand >= prefix && operand - prefix < contract->parameter_count &&
           contract->parameter_modes[operand - prefix] == XR_PARAM_MOVE;
}

/* A direct call can borrow an affine owner without consuming it.  If the
 * private backend reports
 * provider-call-failed while that call is active,
 * the caller still owns the value and the trap
 * cleanup must close it. */
static XrProgramBuildStatus prepare_trap_borrowed_owner_arguments(XrXiBuildContext *context,
                                                                  XrXiFunctionStorage *function,
                                                                  char *diagnostic,
                                                                  size_t diagnostic_size) {
    for (uint32_t edge_index = 0u; edge_index < context->trap_edge_count; ++edge_index) {
        const XrXiTrapEdge *edge = &context->trap_edges[edge_index];
        const XiValue *call = edge->call;
        XrXiTrapCallContract contract;
        if (edge->function != function->xi ||
            !trap_call_contract(context, function, call, &contract))
            continue;
        XrXiBlockStorage *handler = find_block_storage(function, edge->handler);
        if (!handler)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi trap continuation endpoint is absent");
        for (uint32_t operand = contract.first_operand; operand < call->nargs; ++operand) {
            const XiValue *owner = logical_value_identity(call->args[operand]);
            if (trap_call_operand_consumes(&contract, operand) ||
                !logical_value_produces_owner(context, function->xi, owner, 0u))
                continue;
            uint16_t owner_type = XR_CORE_TYPE_VOID;
            if (!map_logical_value_type(context, function->xi, call->args[operand], &owner_type) ||
                owner_type == XR_CORE_TYPE_VOID)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi trap-capable call v%u borrowed owner operand %u has no CoreSpec "
                            "type",
                            call->id, operand);
            XrProgramBuildStatus status = add_block_argument(
                context, function, handler, call->args[operand], NULL, owner_type,
                XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static bool trap_call_consumes_source(XrXiBuildContext *context,
                                      const XrXiFunctionStorage *function, const XiValue *call,
                                      const XiValue *source) {
    XrXiTrapCallContract contract;
    if (!source || !trap_call_contract(context, function, call, &contract))
        return false;
    source = exact_logical_value_identity(context, function->xi, source);
    for (uint32_t operand = contract.first_operand; operand < call->nargs; ++operand) {
        const XiValue *argument =
            exact_logical_value_identity(context, function->xi, call->args[operand]);
        if (trap_call_operand_consumes(&contract, operand) && argument == source)
            return true;
    }
    return false;
}

static XrProgramBuildStatus prepare_panic_arguments(XrXiBuildContext *context,
                                                    XrXiFunctionStorage *function, char *diagnostic,
                                                    size_t diagnostic_size) {
    for (uint32_t edge_index = 0u; edge_index < context->panic_edge_count; ++edge_index) {
        const XrXiPanicEdge *edge = &context->panic_edges[edge_index];
        if (edge->function != function->xi)
            continue;
        XrXiBlockStorage *handler = find_block_storage(function, edge->handler);
        const XiValue *caught = NULL;
        for (uint32_t value_index = 0u; edge->handler && value_index < edge->handler->nvalues;
             ++value_index) {
            const XiValue *value = edge->handler->values[value_index];
            if (value && value->op == XI_CATCH && value->aux == edge->registration) {
                if (caught)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi panic cleanup has multiple typed payloads");
                caught = value;
            }
        }
        if (!handler || !caught)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi panic cleanup payload is absent");
        XrProgramBuildStatus status =
            add_block_argument(context, function, handler, caught, NULL, XR_CORE_TYPE_PANIC_INFO,
                               XR_XI_INVOKE_ARGUMENT_PANIC, NULL, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    return XR_PROGRAM_BUILD_OK;
}

static const XiValue *canonical_cancel_drop_identity(const XrXiBuildContext *context,
                                                     const XrXiFunctionStorage *function,
                                                     const XiCoroSuspendPoint *point,
                                                     const XiValue *drop) {
    drop = canonical_owner_storage_identity(context, function, drop);
    const XiValue *successor =
        canonical_coroutine_owner_successor(context, function ? function->xi : NULL, point, drop);
    return canonical_owner_storage_identity(context, function, successor ? successor : drop);
}

static XrProgramBuildStatus prepare_cancel_arguments(XrXiBuildContext *context,
                                                     XrXiFunctionStorage *function,
                                                     char *diagnostic, size_t diagnostic_size) {
    for (uint32_t edge_index = 0u; edge_index < context->cancel_edge_count; ++edge_index) {
        const XrXiCancelEdge *edge = &context->cancel_edges[edge_index];
        if (edge->function != function->xi)
            continue;
        XrXiBlockStorage *handler = find_block_storage(function, edge->handler);
        if (!handler || !edge->point)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi cancel continuation endpoint is absent");
        if (handler->trap_cleanup)
            continue;
        for (uint32_t drop = 0u; drop < edge->point->ndrops; ++drop) {
            uint16_t type_id = XR_CORE_TYPE_VOID;
            const XiValue *storage_identity = canonical_cancel_drop_identity(
                context, function, edge->point, edge->point->drops[drop]);
            const XiValue *owner = NULL;
            for (uint32_t argument = 0u; argument < handler->argument_count; ++argument) {
                const XrXiBlockArgumentStorage *candidate = &handler->argument_storage[argument];
                if (candidate->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE ||
                    candidate->ownership != XR_CORE_IR_OWNER ||
                    canonical_owner_storage_identity(context, function, candidate->source) !=
                        storage_identity)
                    continue;
                if (owner && owner != candidate->source)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi cancellation owner has multiple current values");
                owner = candidate->source;
            }
            if (!owner)
                owner = edge->point->drops[drop];
            if (!map_logical_value_type(context, function->xi, owner, &type_id) ||
                type_id == XR_CORE_TYPE_VOID)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi cancellation owner v%u has no canonical type",
                            owner ? owner->id : UINT32_MAX);
            XrProgramBuildStatus status =
                add_block_argument(context, function, handler, owner, NULL, type_id,
                                   XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static bool cancel_handler_payload_is_exact(const XrXiBuildContext *context,
                                            const XrXiFunctionStorage *function,
                                            const XrXiBlockStorage *handler,
                                            const XiCoroSuspendPoint *point) {
    if (!context || !function || !handler || !point)
        return false;
    uint32_t owner_count = 0u;
    for (uint32_t argument = 0u; argument < handler->argument_count; ++argument) {
        const XrXiBlockArgumentStorage *candidate = &handler->argument_storage[argument];
        if (candidate->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_PANIC)
            continue;
        if (candidate->phi || candidate->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE ||
            candidate->category > XR_CORE_IR_PLACE)
            return false;
        if (candidate->ownership != XR_CORE_IR_OWNER)
            continue;
        const XiValue *logical =
            canonical_owner_storage_identity(context, function, candidate->source);
        uint32_t matches = 0u;
        for (uint32_t drop = 0u; drop < point->ndrops; ++drop)
            matches += canonical_cancel_drop_identity(context, function, point,
                                                      point->drops[drop]) == logical;
        if (!logical || matches != 1u)
            return false;
        ++owner_count;
    }
    if (owner_count != point->ndrops)
        return false;
    for (uint32_t drop = 0u; drop < point->ndrops; ++drop) {
        const XiValue *logical_drop =
            canonical_cancel_drop_identity(context, function, point, point->drops[drop]);
        uint32_t matches = 0u;
        for (uint32_t argument = 0u; argument < handler->argument_count; ++argument) {
            const XrXiBlockArgumentStorage *candidate = &handler->argument_storage[argument];
            matches += candidate->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_NONE &&
                       candidate->category == XR_CORE_IR_VALUE &&
                       candidate->ownership == XR_CORE_IR_OWNER &&
                       canonical_owner_storage_identity(context, function, candidate->source) ==
                           logical_drop;
        }
        if (!logical_drop || matches != 1u)
            return false;
    }
    return true;
}

static XrProgramBuildStatus close_block_arguments(XrXiBuildContext *context,
                                                  XrXiFunctionStorage *function, char *diagnostic,
                                                  size_t diagnostic_size) {
    XrProgramBuildStatus invoke_status =
        prepare_invoke_arguments(context, function, diagnostic, diagnostic_size);
    if (invoke_status != XR_PROGRAM_BUILD_OK)
        return invoke_status;
    XrProgramBuildStatus trap_owner_status =
        prepare_trap_borrowed_owner_arguments(context, function, diagnostic, diagnostic_size);
    if (trap_owner_status != XR_PROGRAM_BUILD_OK)
        return trap_owner_status;
    XrProgramBuildStatus panic_argument_status =
        prepare_panic_arguments(context, function, diagnostic, diagnostic_size);
    if (panic_argument_status != XR_PROGRAM_BUILD_OK)
        return panic_argument_status;
    XrProgramBuildStatus cancel_argument_status =
        prepare_cancel_arguments(context, function, diagnostic, diagnostic_size);
    if (cancel_argument_status != XR_PROGRAM_BUILD_OK)
        return cancel_argument_status;
    XrXiBlockStorage *entry = find_block_storage(function, function->xi->entry);
    if (!entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi function entry block is absent");
    if (function->capture_type_id != XR_CORE_TYPE_VOID) {
        XrProgramBuildStatus status = add_block_argument(
            context, function, entry, &function->capture_receiver, NULL, function->capture_type_id,
            XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    for (uint16_t parameter = 0; parameter < function->xi->nparams; ++parameter) {
        XrProgramBuildStatus status = add_block_argument(
            context, function, entry, function->xi->params[parameter], NULL, XR_CORE_TYPE_VOID,
            XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
        XrXiBlockStorage *block = &function->block_storage[block_index];
        if (!block->reachable ||
            block_is_elided_class_construction_error_continuation(context, function->xi, block->xi))
            continue;
        for (const XiPhi *phi = block->xi->phis; phi; phi = phi->next) {
            XrProgramBuildStatus status =
                add_block_argument(context, function, block, &phi->value, phi, XR_CORE_TYPE_VOID,
                                   XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        for (uint32_t value_index = 0; value_index < block->xi->nvalues; ++value_index) {
            const XiValue *value = block->xi->values[value_index];
            if (value && value->op == XI_THROW && (block->trap_cleanup || block->cancel_cleanup))
                continue;
            if (value && (value->op == XI_ERR_RETURN || value->op == XI_THROW)) {
                if (value->nargs != 1u || !value->args || !value->args[0])
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi terminal scaffold v%u has no typed payload", value->id);
                XrProgramBuildStatus status = require_value_available(
                    context, function, block, value->args[0], NULL, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
                continue;
            }
            if (value_is_skipped(context, function->xi, value))
                continue;
            XrProgramBuildStatus status = collect_value_live_ins(context, function, block, value,
                                                                 NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        bool reason_cleanup_terminal =
            block->xi->kind == XI_BLOCK_UNREACHABLE &&
            (block->trap_cleanup || block->panic_cleanup || block->cancel_cleanup);
        if (block->xi->control && !reason_cleanup_terminal &&
            block->static_branch_outcome == XR_XI_STATIC_BRANCH_UNKNOWN &&
            !block_typed_invoke_call(context, function->xi, block->xi)) {
            XrProgramBuildStatus status = require_value_available(
                context, function, block, block->xi->control, NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }

    uint64_t limit = (uint64_t) function->xi->nblocks *
                     ((uint64_t) function->xi->next_value_id + function->xi->nparams + 1u);
    for (uint64_t iteration = 0; iteration <= limit; ++iteration) {
        bool changed = false;
        for (uint32_t edge_index = 0u; edge_index < context->trap_edge_count; ++edge_index) {
            const XrXiTrapEdge *edge = &context->trap_edges[edge_index];
            if (edge->function != function->xi)
                continue;
            const XiBlock *invoke_block =
                typed_invoke_check_block_for_call(context, function->xi, edge->call);
            XrXiBlockStorage *source =
                find_block_storage(function, invoke_block ? invoke_block : edge->call->block);
            XrXiBlockStorage *handler = find_block_storage(function, edge->handler);
            if (!source || !handler)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi provider trap continuation endpoint is absent");
            uint32_t source_argument_count = source->argument_count;
            for (uint32_t argument = 0u; argument < source_argument_count; ++argument) {
                XrXiBlockArgumentStorage owner = source->argument_storage[argument];
                const XiValue *current_owner = owner.source;
                if (owner.ownership != XR_CORE_IR_OWNER ||
                    owner.implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE ||
                    trap_call_consumes_source(context, function, edge->call, current_owner))
                    continue;
                XrProgramBuildStatus status = add_block_argument(
                    context, function, handler, current_owner, NULL, owner.type_id,
                    XR_XI_INVOKE_ARGUMENT_NONE, &changed, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
            for (uint32_t argument = 0u; argument < handler->argument_count; ++argument) {
                XrXiBlockArgumentStorage *edge_argument = &handler->argument_storage[argument];
                if (edge_argument->phi ||
                    edge_argument->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi provider trap continuation requires a phi payload");
                XrProgramBuildStatus status =
                    require_value_available(context, function, source, edge_argument->source,
                                            &changed, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        }
        for (uint32_t edge_index = 0u; edge_index < context->panic_edge_count; ++edge_index) {
            const XrXiPanicEdge *edge = &context->panic_edges[edge_index];
            if (edge->function != function->xi)
                continue;
            XrXiBlockStorage *source = find_block_storage(function, edge->point->block);
            XrXiBlockStorage *handler = find_block_storage(function, edge->handler);
            if (!source || !handler)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi panic continuation endpoint is absent");
            uint32_t source_argument_count = source->argument_count;
            for (uint32_t argument = 0u; argument < source_argument_count; ++argument) {
                XrXiBlockArgumentStorage owner = source->argument_storage[argument];
                if (owner.ownership != XR_CORE_IR_OWNER ||
                    owner.implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE)
                    continue;
                XrProgramBuildStatus status = add_block_argument(
                    context, function, handler, owner.source, NULL, owner.type_id,
                    XR_XI_INVOKE_ARGUMENT_NONE, &changed, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
            for (uint32_t argument = 0u; argument < handler->argument_count; ++argument) {
                XrXiBlockArgumentStorage *edge_argument = &handler->argument_storage[argument];
                if (edge_argument->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_PANIC)
                    continue;
                if (edge_argument->phi ||
                    edge_argument->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi panic continuation requires a phi payload");
                XrProgramBuildStatus status =
                    require_value_available(context, function, source, edge_argument->source,
                                            &changed, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        }
        for (uint32_t edge_index = 0u; edge_index < context->cancel_edge_count; ++edge_index) {
            const XrXiCancelEdge *edge = &context->cancel_edges[edge_index];
            if (edge->function != function->xi)
                continue;
            XrXiBlockStorage *source = find_block_storage(
                function, edge->point && edge->point->op ? edge->point->op->block : NULL);
            XrXiBlockStorage *handler = find_block_storage(function, edge->handler);
            if (!source || !handler || !edge->point)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi cancel continuation endpoint is absent");
            for (uint32_t argument = 0u; argument < handler->argument_count; ++argument) {
                XrXiBlockArgumentStorage *edge_argument = &handler->argument_storage[argument];
                if (edge_argument->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_PANIC)
                    continue;
                if (edge_argument->phi ||
                    edge_argument->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi cancel continuation requires an exact payload");
                XrProgramBuildStatus status =
                    require_value_available(context, function, source, edge_argument->source,
                                            &changed, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        }
        XrProgramBuildStatus balance_status = balance_owner_successor_arguments(
            context, function, &changed, diagnostic, diagnostic_size);
        if (balance_status != XR_PROGRAM_BUILD_OK)
            return balance_status;
        for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
            XrXiBlockStorage *successor = &function->block_storage[block_index];
            if (!successor->reachable || block_is_elided_class_construction_error_continuation(
                                             context, function->xi, successor->xi))
                continue;
            bool reason_cleanup =
                successor->trap_cleanup || successor->panic_cleanup || successor->cancel_cleanup;
            uint32_t argument_count = successor->argument_count;
            for (uint16_t predecessor_index = 0; predecessor_index < successor->xi->npreds;
                 ++predecessor_index) {
                const XiBlock *predecessor_xi = successor->xi->preds[predecessor_index];
                if (block_is_elided_class_construction_error_continuation(context, function->xi,
                                                                          predecessor_xi))
                    continue;
                XrXiBlockStorage *predecessor = find_block_storage(function, predecessor_xi);
                if (!predecessor)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi CFG predecessor is absent");
                if (!predecessor->reachable)
                    continue;
                if (reason_cleanup && (predecessor->trap_cleanup != successor->trap_cleanup ||
                                       predecessor->panic_cleanup != successor->panic_cleanup ||
                                       predecessor->cancel_cleanup != successor->cancel_cleanup))
                    continue;
                for (uint32_t argument_index = 0; argument_index < argument_count;
                     ++argument_index) {
                    XrXiBlockArgumentStorage argument = successor->argument_storage[argument_index];
                    if (argument.implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE)
                        continue;
                    uint32_t predecessor_occurrence = 0u;
                    for (uint16_t prior = 0u; prior < predecessor_index; ++prior)
                        predecessor_occurrence += successor->xi->preds[prior] == predecessor_xi;
                    const XiValue *incoming = edge_argument_value(
                        &argument, predecessor_xi, successor->xi, predecessor_occurrence);
                    XrProgramBuildStatus status =
                        require_value_available(context, function, predecessor, incoming, &changed,
                                                diagnostic, diagnostic_size);
                    if (status != XR_PROGRAM_BUILD_OK)
                        return status;
                }
            }
        }
        if (!changed)
            break;
        if (iteration == limit)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi block-parameter closure did not converge");
    }
    /* Cancellation exactness is a property of the closed live-in graph. Owners used only by an

     * * inner handler can arrive after the first propagation round, so checking inside the fixed

     * * point observes a valid payload while it is still incomplete. */
    for (uint32_t edge_index = 0u; edge_index < context->cancel_edge_count; ++edge_index) {
        const XrXiCancelEdge *edge = &context->cancel_edges[edge_index];
        if (edge->function != function->xi)
            continue;
        const XrXiBlockStorage *handler = find_block_storage(function, edge->handler);
        if (!handler || !edge->point ||
            !cancel_handler_payload_is_exact(context, function, handler, edge->point))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi cancel continuation has no exact owner payload");
    }
    XrProgramBuildStatus coroutine_argument_status =
        prepare_coroutine_call_arguments(context, function, diagnostic, diagnostic_size);
    if (coroutine_argument_status != XR_PROGRAM_BUILD_OK)
        return coroutine_argument_status;
    uint32_t expected_entry_arguments =
        function->xi->nparams + (function->capture_type_id != XR_CORE_TYPE_VOID ? 1u : 0u);
    if (entry->argument_count != expected_entry_arguments)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi entry block acquired non-parameter live-ins");

    for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
        XrXiBlockStorage *block = &function->block_storage[block_index];
        if (!block->reachable ||
            block_is_elided_class_construction_error_continuation(context, function->xi, block->xi))
            continue;
        if (block->xi != function->xi->entry && block->argument_count > 1u)
            qsort(block->argument_storage, block->argument_count, sizeof(*block->argument_storage),
                  block_argument_compare);
        if (block->argument_count == 0u)
            continue;
        block->arguments = xr_calloc(block->argument_count, sizeof(*block->arguments));
        if (!block->arguments)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        for (uint32_t argument = 0; argument < block->argument_count; ++argument) {
            block->arguments[argument].key = block->argument_storage[argument].key;
            block->arguments[argument].type_id = block->argument_storage[argument].type_id;
            block->arguments[argument].category = block->argument_storage[argument].category;
            block->arguments[argument].ownership = block->argument_storage[argument].ownership;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
set_edge_operands(const XrXiBuildContext *context, XrCoreIrInstructionInput *instruction,
                  const XrXiFunctionStorage *function, const XrXiBlockStorage *predecessor,
                  const XrXiBlockStorage *first, const XrXiBlockStorage *second,
                  const XiValue *control, char *diagnostic, size_t diagnostic_size) {
    uint32_t count =
        (control ? 1u : 0u) + first->argument_count + (second ? second->argument_count : 0u);
    XrCoreIrKey *operands = count ? xr_calloc(count, sizeof(*operands)) : NULL;
    if (count && !operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t cursor = 0;
    if (control) {
        if (!value_operand_key(context, function, predecessor, control, &operands[cursor++])) {
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi branch condition is unavailable");
        }
    }
    const XrXiBlockStorage *successors[2] = {first, second};
    for (uint32_t successor_index = 0; successor_index < (second ? 2u : 1u); ++successor_index) {
        const XrXiBlockStorage *successor = successors[successor_index];
        uint32_t predecessor_occurrence =
            successor_index == 1u && successors[0]->xi == successor->xi ? 1u : 0u;
        for (uint32_t argument = 0; argument < successor->argument_count; ++argument) {
            const XiValue *incoming =
                edge_argument_value(&successor->argument_storage[argument], predecessor->xi,
                                    successor->xi, predecessor_occurrence);
            if (!edge_value_operand_key(context, function, predecessor, incoming, NULL,
                                        &operands[cursor++])) {
                const XiValue *logical =
                    exact_logical_value_identity(context, function ? function->xi : NULL, incoming);
                xr_free(operands);
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi edge argument %u (v%u op%u defined in b%u; logical v%u op%u "
                            "defined in b%u) is unavailable on b%u -> b%u",
                            argument, incoming ? incoming->id : UINT32_MAX,
                            incoming ? (unsigned) incoming->op : UINT32_MAX,
                            incoming && incoming->block ? incoming->block->id : UINT32_MAX,
                            logical ? logical->id : UINT32_MAX,
                            logical ? (unsigned) logical->op : UINT32_MAX,
                            logical && logical->block ? logical->block->id : UINT32_MAX,
                            predecessor->xi->id, successor->xi->id);
            }
        }
    }
    instruction->operands = operands;
    instruction->operand_count = count;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
set_invoke_operands(const XrXiBuildContext *context, XrCoreIrInstructionInput *instruction,
                    const XrXiFunctionStorage *function, const XrXiBlockStorage *predecessor,
                    const XiValue *call, const XrXiBlockStorage *normal,
                    const XrXiBlockStorage *error, uint32_t first_operand, char *diagnostic,
                    size_t diagnostic_size) {
    uint32_t normal_implicit = call->type && call->type->kind != XR_KIND_UNIT ? 1u : 0u;
    if (normal->argument_count < normal_implicit || error->argument_count == 0u ||
        (normal_implicit != 0u &&
         normal->argument_storage[0].implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT) ||
        error->argument_storage[0].implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_ERROR)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi invoke continuations lack ordered implicit result/error arguments");
    uint32_t parameter_count = call->nargs - first_operand;
    uint32_t count =
        parameter_count + normal->argument_count - normal_implicit + error->argument_count - 1u;
    XrCoreIrKey *operands = count ? xr_calloc(count, sizeof(*operands)) : NULL;
    if (count && !operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t cursor = 0u;
    const XiValue *unavailable_value = NULL;
    for (uint16_t parameter = (uint16_t) first_operand; parameter < call->nargs; ++parameter) {
        if (!value_operand_key(context, function, predecessor, call->args[parameter],
                               &operands[cursor++])) {
            unavailable_value = call->args[parameter];
            goto unavailable;
        }
    }
    const XrXiBlockStorage *successors[2] = {normal, error};
    const uint32_t starts[2] = {normal_implicit, 1u};
    for (uint32_t edge = 0u; edge < 2u; ++edge) {
        const XrXiBlockStorage *successor = successors[edge];
        uint32_t predecessor_occurrence =
            edge == 1u && successors[0]->xi == successor->xi ? 1u : 0u;
        for (uint32_t argument = starts[edge]; argument < successor->argument_count; ++argument) {
            const XiValue *incoming =
                edge_argument_value(&successor->argument_storage[argument], predecessor->xi,
                                    successor->xi, predecessor_occurrence);
            if (!edge_value_operand_key(context, function, predecessor, incoming, call,
                                        &operands[cursor++])) {
                unavailable_value = incoming;
                goto unavailable;
            }
        }
    }
    instruction->operands = operands;
    instruction->operand_count = count;
    return XR_PROGRAM_BUILD_OK;

unavailable:
    xr_free(operands);
    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "Xi function %s invoke v%u in b%u has unavailable operand v%u op%u from b%u",
                function && function->xi && function->xi->name ? function->xi->name : "<anonymous>",
                call ? call->id : UINT32_MAX, predecessor->xi->id,
                unavailable_value ? unavailable_value->id : UINT32_MAX,
                unavailable_value ? (unsigned) unavailable_value->op : UINT32_MAX,
                unavailable_value && unavailable_value->block ? unavailable_value->block->id
                                                              : UINT32_MAX);
}

static XrProgramBuildStatus append_invoke_trap_edge_operands(
    const XrXiBuildContext *context, XrCoreIrInstructionInput *instruction,
    const XrXiFunctionStorage *function, const XrXiBlockStorage *source, const XiValue *call,
    const XrXiBlockStorage *handler, char *diagnostic, size_t diagnostic_size) {
    if (!instruction || !handler ||
        instruction->operand_count > UINT32_MAX - handler->argument_count ||
        instruction->successor_count == UINT32_MAX)
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    uint32_t operand_count = instruction->operand_count + handler->argument_count;
    XrCoreIrKey *operands = operand_count ? xr_calloc(operand_count, sizeof(*operands)) : NULL;
    if (operand_count && !operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (instruction->operand_count)
        memcpy(operands, instruction->operands,
               (size_t) instruction->operand_count * sizeof(*operands));
    uint32_t cursor = instruction->operand_count;
    for (uint32_t argument = 0u; argument < handler->argument_count; ++argument) {
        const XrXiBlockArgumentStorage *edge_argument = &handler->argument_storage[argument];
        if (edge_argument->phi ||
            edge_argument->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE ||
            !edge_value_operand_key(context, function, source, edge_argument->source, call,
                                    &operands[cursor++])) {
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi invoke trap continuation argument %u is unavailable", argument);
        }
    }
    uint32_t successor_count = instruction->successor_count + 1u;
    XrCoreIrKey *successors = xr_calloc(successor_count, sizeof(*successors));
    if (!successors) {
        xr_free(operands);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    if (instruction->successor_count)
        memcpy(successors, instruction->successors,
               (size_t) instruction->successor_count * sizeof(*successors));
    successors[instruction->successor_count] = block_key(function, handler->xi);
    xr_free((void *) instruction->operands);
    xr_free((void *) instruction->successors);
    instruction->operands = operands;
    instruction->operand_count = operand_count;
    instruction->successors = successors;
    instruction->successor_count = successor_count;
    return XR_PROGRAM_BUILD_OK;
}

static bool exact_cooperative_yield_contract(XrXiBuildContext *context,
                                             const XrXiFunctionStorage *function,
                                             const XiValue *value) {
    XrProgramXiProjection projection;
    const XgSuspendPointSummary *evidence =
        context && context->source && value
            ? xg_global_evidence_find_suspend_point(context->source->global_evidence,
                                                    value->xg_suspend_point_use_id)
            : NULL;
    uint16_t result_type = XR_CORE_TYPE_VOID;
    return context && function && function->xi && value && value->op == XI_YIELD &&
           map_logical_value_type(context, function->xi, value, &result_type) &&
           result_type == XR_CORE_TYPE_VOID &&
           xr_program_xi_projection(value->op, result_type, &projection) &&
           projection.kind == XR_PROGRAM_XI_PROJECTION_COROUTINE_YIELD &&
           projection.core_operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD && value->nargs == 0u &&
           value->aux == NULL && value->aux_int == XI_YIELD_AUX_IMMEDIATE &&
           (value->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND)) ==
               (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND) &&
           value->xg_suspend_point_use_id != XG_NO_ID && value->xg_suspend_source_node_id != 0u &&
           value->xg_suspend_body_ordinal != 0u &&
           value->xg_suspend_point_kind == XG_SUSPEND_POINT_COOPERATIVE_YIELD &&
           value->xg_suspend_may_suspend == 1u && value->xg_suspend_contract_complete == 1u &&
           evidence && evidence->use_id == value->xg_suspend_point_use_id &&
           evidence->owner_func_id == function->xi->xg_body_func_id &&
           evidence->source_node_id == value->xg_suspend_source_node_id &&
           evidence->body_ordinal == value->xg_suspend_body_ordinal &&
           evidence->kind == XG_SUSPEND_POINT_COOPERATIVE_YIELD && evidence->may_suspend == 1u &&
           evidence->contract_complete == 1u;
}

static const XiCoroSuspendPoint *coroutine_point_for_block(const XrXiFunctionStorage *function,
                                                           const XiBlock *block) {
    const XiCoroPlan *plan = function && function->xi ? function->xi->coro_plan : NULL;
    const XiCoroSuspendPoint *found = NULL;
    for (uint32_t point = 0u; plan && point < plan->nstates; ++point) {
        const XiCoroSuspendPoint *candidate = &plan->points[point];
        if (candidate->suspend_block != block)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static const XiCoroSuspendPoint *coroutine_point_for_safepoint(const XrXiFunctionStorage *function,
                                                               uint32_t safepoint_id) {
    const XiCoroPlan *plan = function && function->xi ? function->xi->coro_plan : NULL;
    const XiCoroSuspendPoint *found = NULL;
    for (uint32_t point = 0u; plan && plan->points && point < plan->nstates; ++point) {
        const XiCoroSuspendPoint *candidate = &plan->points[point];
        if (candidate->state_id == 0u || candidate->state_id - 1u != safepoint_id)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static bool coroutine_function_has_proven_suspend(const XrXiBuildContext *context,
                                                  const XiFunc *function, const XiFunc **stack,
                                                  uint32_t depth) {
    const XiCoroPlan *plan = function ? function->coro_plan : NULL;
    if (!plan || plan->nstates == 0u || !plan->points)
        return false;
    if (!stack || depth >= 64u)
        return false;
    for (uint32_t ancestor = 0u; ancestor < depth; ++ancestor)
        if (stack[ancestor] == function)
            return false;
    stack[depth] = function;
    for (uint32_t point_index = 0u; point_index < plan->nstates; ++point_index) {
        const XiCoroSuspendPoint *point = &plan->points[point_index];
        if (!point->op)
            return false;
        if (point->kind == XI_CORO_SUSP_YIELD)
            return true;
        if (point->kind != XI_CORO_SUSP_CALL)
            return false;
        if (resolved_suspension_native_call(context, function, point->op))
            return true;
        if (point->resolved_callee) {
            if (coroutine_function_has_proven_suspend(context, point->resolved_callee, stack,
                                                      depth + 1u))
                return true;
            continue;
        }
        const XiCoroEdge *child = xi_coro_point_find_edge(point, XI_CORO_EDGE_CHILD);
        XrXiCallableTargetSet target_set = {0};
        if (!child || !child->indirect_child || child->callee ||
            !resolved_callable_call_targets(context, function, point->op, &target_set) ||
            (target_set.callsite->flags & XG_CALL_MAY_SUSPEND) == 0u ||
            (target_set.callsite->callable_effect_union & XG_BODY_MAY_SUSPEND) == 0u ||
            (target_set.callsite->callable_capability_union & XG_CAP_COROUTINE) == 0u)
            continue;
        bool has_suspending_target = false;
        bool targets_proven = true;
        for (uint32_t target_index = 0u; target_index < target_set.target_count; ++target_index) {
            const XgCallableTargetSummary *target_row = &target_set.targets[target_index];
            const XiFunc *target = find_xi_function_by_xg_id(context, target_row->target_func_id);
            const XrXiFunctionStorage *target_storage =
                find_xi_function(context, target, NULL, NULL);
            bool row_suspends = (target_row->effect_bits & XG_BODY_MAY_SUSPEND) != 0u;
            bool storage_suspends = target_storage && (target_storage->closed_effect_mask &
                                                       XR_CORE_EFFECT_SUSPEND) != 0u;
            if (!target_storage || !target_storage->closed_contract_ready ||
                row_suspends != storage_suspends ||
                (row_suspends &&
                 (!coroutine_function_has_proven_suspend(context, target, stack, depth + 1u) ||
                  (target_storage->closed_capability_mask &
                   XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION) == 0u))) {
                targets_proven = false;
                break;
            }
            has_suspending_target |= row_suspends;
        }
        if (targets_proven && has_suspending_target)
            return true;
    }
    return false;
}

static bool canonical_block_is_trap_cleanup(const XrXiBuildContext *context, const XiFunc *function,
                                            const XiBlock *block) {
    const XrXiFunctionStorage *storage = find_xi_function(context, function, NULL, NULL);
    if (!storage || !storage->block_storage || !block)
        return false;
    for (uint32_t index = 0u; index < function->nblocks; ++index)
        if (storage->block_storage[index].xi == block)
            return storage->block_storage[index].trap_cleanup;
    return false;
}

static bool canonical_block_is_panic_cleanup(const XrXiBuildContext *context,
                                             const XiFunc *function, const XiBlock *block) {
    const XrXiFunctionStorage *storage = find_xi_function(context, function, NULL, NULL);
    if (!storage || !storage->block_storage || !block)
        return false;
    for (uint32_t index = 0u; index < function->nblocks; ++index)
        if (storage->block_storage[index].xi == block)
            return storage->block_storage[index].panic_cleanup;
    return false;
}

static bool canonical_block_is_cancel_cleanup(const XrXiBuildContext *context,
                                              const XiFunc *function, const XiBlock *block) {
    const XrXiFunctionStorage *storage = find_xi_function(context, function, NULL, NULL);
    if (!storage || !storage->block_storage || !block)
        return false;
    for (uint32_t index = 0u; index < function->nblocks; ++index)
        if (storage->block_storage[index].xi == block)
            return storage->block_storage[index].cancel_cleanup;
    return false;
}

static XrProgramBuildStatus prepare_coroutine_shape(XrXiBuildContext *context,
                                                    XrXiFunctionStorage *function,
                                                    XrCoreIrFunctionInput *output, char *diagnostic,
                                                    size_t diagnostic_size) {
    const XiFunc *xi = function ? function->xi : NULL;
    const XiCoroPlan *plan = xi ? xi->coro_plan : NULL;
    uint32_t yield_count = 0u;
    for (uint32_t block = 0u; xi && block < xi->nblocks; ++block)
        for (uint32_t value = 0u; xi->blocks[block] && value < xi->blocks[block]->nvalues; ++value)
            yield_count += xi->blocks[block]->values[value] &&
                           xi->blocks[block]->values[value]->op == XI_YIELD;

    if (yield_count == 0u && (!plan || plan->nstates == 0u))
        return XR_PROGRAM_BUILD_OK;

    /* Xi reserves states for open callable target sets, and propagates those
     * reservations
     * through direct callers, even when no suspension effect is
     * proven.  They keep later
     * target planning conservative but are not
     * semantic suspension points in canonical
     * Program. */
    const XiFunc *suspend_stack[64] = {0};
    if (yield_count == 0u && plan->is_coroutine && plan->analysis_complete &&
        plan->actions_materialized && plan->cfg_rewritten && xi_coro_plan_is_current(xi, plan) &&
        (function->closed_effect_mask & XR_CORE_EFFECT_SUSPEND) == 0u &&
        !coroutine_function_has_proven_suspend(context, xi, suspend_stack, 0u))
        return XR_PROGRAM_BUILD_OK;
    if (!context || !xi || !output || !plan || !plan->is_coroutine || !plan->analysis_complete ||
        !plan->actions_materialized || !plan->cfg_rewritten || !xi_coro_plan_is_current(xi, plan) ||
        !plan->points || !plan->dispatch)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi function %s has no current exact coroutine plan",
                    xi && xi->name ? xi->name : "<anonymous>");

    if (plan->nstates == 0u || plan->nstates == UINT32_MAX ||
        plan->ndispatch != plan->nstates + 1u ||
        plan->dispatch[0].state_id != XI_CORO_STATE_ENTRY || plan->dispatch[0].target != xi->entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %s coroutine shape is outside the active canonical Program slice",
                    xi->name ? xi->name : "<anonymous>");

    function->coroutine_states =
        xr_calloc((size_t) plan->nstates + 1u, sizeof(*function->coroutine_states));
    function->coroutine_safepoints =
        xr_calloc(plan->nstates, sizeof(*function->coroutine_safepoints));
    function->cancel_graphs = xr_calloc(plan->nstates, sizeof(*function->cancel_graphs));
    if (!function->coroutine_states || !function->coroutine_safepoints || !function->cancel_graphs)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    function->coroutine_states[0].state_id = XI_CORO_STATE_ENTRY;
    function->coroutine_states[0].continuation_block = block_key(function, xi->entry);
    for (uint32_t point_index = 0u; point_index < plan->nstates; ++point_index) {
        const XiCoroSuspendPoint *point = &plan->points[point_index];
        const XiCoroEdge *resume = xi_coro_point_find_edge(point, XI_CORO_EDGE_RESUME);
        const XiCoroEdge *cancel = xi_coro_point_find_edge(point, XI_CORO_EDGE_CANCEL);
        if (point->state_id != point_index + 1u || !point->op ||
            point->op->block != point->suspend_block || !point->suspend_block ||
            point->suspend_block->nvalues != 1u || point->suspend_block->values[0] != point->op ||
            point->suspend_block->kind != XI_BLOCK_PLAIN ||
            point->suspend_block->succs[0] != point->resume_block ||
            point->suspend_block->succs[1] != NULL || !point->resume_block ||
            point->continuation != point->resume_block ||
            point->store_state_id != point->state_id || point->generation != point->state_id ||
            !point->returns_to_scheduler ||
            plan->dispatch[point->state_id].state_id != point->state_id ||
            plan->dispatch[point->state_id].target != point->suspend_block || !resume ||
            resume->terminal || resume->source_state_id != point->state_id ||
            resume->target_state_id != point->state_id ||
            resume->target_block != point->resume_block || !cancel || !cancel->terminal ||
            cancel->source_state_id != point->state_id ||
            cancel->target_state_id != XI_CORO_STATE_TERMINAL || cancel->target_block != NULL ||
            cancel->drops != point->drops || cancel->ndrops != point->ndrops ||
            (point->capability_mask & XI_CORO_CAP_CANCEL_CLEANUP) == 0u)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi function %s suspension %u lacks exact logical plan closure",
                        xi->name ? xi->name : "<anonymous>", point_index);

        bool exact_yield = point->kind == XI_CORO_SUSP_YIELD &&
                           exact_cooperative_yield_contract(context, function, point->op);
        if (point->kind == XI_CORO_SUSP_YIELD && !exact_yield)
            return fail(
                diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "Xi function %s cooperative yield %u lacks exact logical plan/evidence closure",
                xi->name ? xi->name : "<anonymous>", point_index);
        bool exact_suspend = point->kind == XI_CORO_SUSP_CALL &&
                             resolved_suspension_native_call(context, xi, point->op) != NULL;
        bool exact_call = false;
        bool exact_indirect_call = false;
        if (!exact_suspend && point->kind == XI_CORO_SUSP_CALL &&
            (point->op->op == XI_CALL || point->op->op == XI_CALL_METHOD ||
             point->op->op == XI_CALL_METHOD_DIRECT)) {
            const XiFunc *callee = resolved_sealed_callee(context, xi, point->op);
            const XrXiFunctionStorage *callee_storage =
                find_xi_function(context, callee, NULL, NULL);
            const XiCoroPlan *callee_plan = callee ? callee->coro_plan : NULL;
            const XiCoroEdge *child = xi_coro_point_find_edge(point, XI_CORO_EDGE_CHILD);
            exact_call = callee && callee_storage && point->resolved_callee == callee && child &&
                         !child->terminal && !child->indirect_child && child->callee == callee &&
                         callee_plan && callee_plan->is_coroutine &&
                         callee_plan->analysis_complete && callee_plan->actions_materialized &&
                         callee_plan->cfg_rewritten &&
                         xi_coro_plan_is_current(callee, callee_plan) &&
                         callee_plan->nstates != 0u && callee_plan->points;
            const XiFunc *callee_stack[64] = {0};
            exact_call = exact_call &&
                         coroutine_function_has_proven_suspend(context, callee, callee_stack, 0u);
        }
        if (!exact_suspend && !exact_call && point->kind == XI_CORO_SUSP_CALL &&
            point->op->op == XI_CALL && !point->resolved_callee) {
            XrXiCallableTargetSet target_set = {0};
            const XiCoroEdge *child = xi_coro_point_find_edge(point, XI_CORO_EDGE_CHILD);
            exact_indirect_call =
                child && !child->terminal && child->indirect_child && !child->callee &&
                resolved_callable_call_targets(context, xi, point->op, &target_set) &&
                (target_set.callsite->flags & XG_CALL_MAY_SUSPEND) != 0u &&
                (target_set.callsite->callable_effect_union & XG_BODY_MAY_SUSPEND) != 0u &&
                (target_set.callsite->callable_capability_union & XG_CAP_COROUTINE) != 0u;
            bool has_suspending_target = false;
            uint32_t effect_union = 0u;
            uint32_t capability_union = 0u;
            for (uint32_t target_index = 0u;
                 exact_indirect_call && target_index < target_set.target_count; ++target_index) {
                const XgCallableTargetSummary *target_row = &target_set.targets[target_index];
                const XiFunc *target =
                    find_xi_function_by_xg_id(context, target_row->target_func_id);
                const XrXiFunctionStorage *target_storage =
                    find_xi_function(context, target, NULL, NULL);
                const XiFunc *target_stack[64] = {0};
                bool row_suspends = (target_row->effect_bits & XG_BODY_MAY_SUSPEND) != 0u;
                bool storage_suspends = target_storage && (target_storage->closed_effect_mask &
                                                           XR_CORE_EFFECT_SUSPEND) != 0u;
                exact_indirect_call =
                    target && target_storage && target_storage->closed_contract_ready &&
                    row_suspends == storage_suspends &&
                    (!row_suspends ||
                     (coroutine_function_has_proven_suspend(context, target, target_stack, 0u) &&
                      (target_storage->closed_capability_mask &
                       XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION) != 0u));
                if (!exact_indirect_call)
                    break;
                has_suspending_target |= row_suspends;
                effect_union |= target_row->effect_bits;
                capability_union |= target_row->capability_bits;
            }
            exact_indirect_call =
                exact_indirect_call && has_suspending_target &&
                effect_union == target_set.callsite->callable_effect_union &&
                capability_union == target_set.callsite->callable_capability_union;
        }
        if (!exact_yield && !exact_suspend && !exact_call && !exact_indirect_call)
            return fail(
                diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                "Xi function %s suspension %u is outside the active canonical Program slice",
                xi->name ? xi->name : "<anonymous>", point_index);

        XrXiBlockStorage *resume_block = find_block_storage(function, point->resume_block);
        if (!resume_block || !resume_block->reachable ||
            block_is_elided_class_construction_error_continuation(context, xi, point->resume_block))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi function %s suspension %u has no canonical resume block",
                        xi->name ? xi->name : "<anonymous>", point_index);

        function->coroutine_states[point->state_id].state_id = point->state_id;
        function->coroutine_states[point->state_id].continuation_block =
            block_key(function, exact_call || exact_indirect_call ? point->suspend_block
                                                                  : point->resume_block);
        function->coroutine_safepoints[point_index].safepoint_id = point_index;
        function->coroutine_safepoints[point_index].resume_state_id = point->state_id;
    }
    output->coroutine_states = function->coroutine_states;
    output->coroutine_state_count = plan->nstates + 1u;
    output->coroutine_safepoints = function->coroutine_safepoints;
    output->coroutine_safepoint_count = plan->nstates;
    return XR_PROGRAM_BUILD_OK;
}

/* A scalar field load is normally a snapshot. Only address-construction
 * scaffolding with no
 * independent value use can share the aggregate live row. */
static bool direct_projection_has_only_address_uses(const XrXiBuildContext *context,
                                                    const XiFunc *function, const XiValue *value) {
    for (uint32_t index = 0u; function && index < function->nblocks; ++index) {
        const XiBlock *block = function->blocks[index];
        if (!block)
            continue;
        if (block->control &&
            exact_logical_value_identity(context, function, block->control) == value)
            return false;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            for (uint32_t operand = 0u; phi->value.args && operand < phi->value.nargs; ++operand) {
                if (exact_logical_value_identity(context, function, phi->value.args[operand]) ==
                    value)
                    return false;
            }
        }
        for (uint32_t index_value = 0u; index_value < block->nvalues; ++index_value) {
            const XiValue *consumer = block->values[index_value];
            if (!consumer || exact_logical_value_identity(context, function, consumer) == value)
                continue;
            for (uint32_t operand = 0u; consumer->args && operand < consumer->nargs; ++operand) {
                if (exact_logical_value_identity(context, function, consumer->args[operand]) !=
                    value)
                    continue;
                if (operand != 0u ||
                    !direct_projection_base_place(context, function, consumer, NULL))
                    return false;
            }
        }
    }
    return function != NULL;
}

static const XiValue *coroutine_direct_projection_storage_owner(const XrXiBuildContext *context,
                                                                const XrXiFunctionStorage *function,
                                                                const XiCoroSuspendPoint *point,
                                                                const XiValue *value) {
    const XiFunc *xi = function ? function->xi : NULL;
    value = exact_logical_value_identity(context, xi, value);
    if (!value || !resolved_aggregate_field_projection(context, value, NULL) ||
        !direct_projection_has_only_address_uses(context, xi, value))
        return NULL;
    const XiValue *owner = NULL;
    for (uint32_t live = 0u; value && point && point->live && live < point->nlive; ++live) {
        const XiValue *place = exact_logical_value_identity(context, xi, point->live[live]);
        if (!place || place->op != XI_LOCAL_ADDR || place->nargs != 1u || !place->args ||
            exact_logical_value_identity(context, xi, place->args[0]) != value)
            continue;
        const XiValue *base = direct_projection_base_place(context, xi, place, NULL);
        const XiValue *candidate =
            logical_value_is_place(xi, base) ? local_place_storage_root(context, xi, base) : base;
        if (!candidate || (owner && owner != candidate))
            return NULL;
        owner = candidate;
    }
    return owner;
}

static const XiValue *canonical_coroutine_live_identity(const XrXiBuildContext *context,
                                                        const XrXiFunctionStorage *function,
                                                        const XiCoroSuspendPoint *point,
                                                        const XiValue *value) {
    const XiFunc *xi = function ? function->xi : NULL;
    value = exact_logical_value_identity(context, xi, value);
    const XiValue *frame_owner = local_place_storage_root(context, xi, value);
    if (!frame_owner && value && value->op == XI_PLACE_LOAD && value->nargs == 1u && value->args &&
        value->args[0] && xi_own_type_may_be_ref(value->type))
        frame_owner = local_place_storage_root(context, xi, value->args[0]);
    if (!frame_owner)
        frame_owner = coroutine_direct_projection_storage_owner(context, function, point, value);
    value = exact_logical_value_identity(context, xi, frame_owner ? frame_owner : value);
    const XiValue *successor = canonical_coroutine_owner_successor(context, xi, point, value);
    return exact_logical_value_identity(context, xi, successor ? successor : value);
}

/* Xi models LOAD_UPVAL without an explicit environment operand.  Canonical
 * Program makes that
 * dependency explicit as a hidden READ parameter, so the
 * closed resume CFG—not the raw Xi spill
 * list—owns whether the capture carrier
 * crosses a particular safepoint. */
static bool coroutine_capture_receiver_is_live(const XrXiFunctionStorage *function,
                                               const XiCoroSuspendPoint *point) {
    if (!function || function->capture_type_id == XR_CORE_TYPE_VOID || !point)
        return false;
    XrXiBlockStorage *resume =
        find_block_storage((XrXiFunctionStorage *) function, point->resume_block);
    const XrXiBlockArgumentStorage *argument =
        resume ? find_block_argument(resume, &function->capture_receiver) : NULL;
    return argument && !argument->phi &&
           argument->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_NONE &&
           argument->type_id == function->capture_type_id &&
           argument->category == XR_CORE_IR_VALUE && argument->ownership == XR_CORE_IR_NON_OWNER;
}

static bool coroutine_live_contract(const XrXiBuildContext *context,
                                    const XrXiFunctionStorage *function, const XiValue *logical,
                                    uint16_t *type_id, XrCoreIrOwnershipDisposition *ownership) {
    if (!context || !function || !logical || !type_id || !ownership)
        return false;
    if (logical == &function->capture_receiver) {
        *type_id = function->capture_type_id;
        *ownership = XR_CORE_IR_NON_OWNER;
        return *type_id != XR_CORE_TYPE_VOID;
    }
    if (!map_logical_value_type((XrXiBuildContext *) context, function->xi, logical, type_id) ||
        *type_id == XR_CORE_TYPE_VOID)
        return false;
    *ownership = logical_ownership_for_type(context, *type_id);
    return true;
}

static uint32_t canonical_coroutine_live_count(const XrXiBuildContext *context,
                                               const XrXiFunctionStorage *function,
                                               const XiCoroSuspendPoint *point) {
    uint32_t count = 0u;
    for (uint32_t live = 0u; point && point->live && live < point->nlive; ++live) {
        const XiValue *identity =
            canonical_coroutine_live_identity(context, function, point, point->live[live]);
        if (!identity)
            return UINT32_MAX;
        bool duplicate = false;
        for (uint32_t prior = 0u; prior < live; ++prior)
            duplicate |= canonical_coroutine_live_identity(context, function, point,
                                                           point->live[prior]) == identity;
        count += duplicate ? 0u : 1u;
    }
    if (coroutine_capture_receiver_is_live(function, point)) {
        bool duplicate = false;
        for (uint32_t live = 0u; point->live && live < point->nlive; ++live)
            duplicate |=
                canonical_coroutine_live_identity(context, function, point, point->live[live]) ==
                &function->capture_receiver;
        count += duplicate ? 0u : 1u;
    }
    return point && (point->nlive == 0u || point->live) ? count : UINT32_MAX;
}

static uint32_t canonical_coroutine_live_occurrences(const XrXiBuildContext *context,
                                                     const XrXiFunctionStorage *function,
                                                     const XiCoroSuspendPoint *point,
                                                     const XiValue *target) {
    uint32_t occurrences = 0u;
    target = canonical_coroutine_live_identity(context, function, point, target);
    for (uint32_t live = 0u; target && point && point->live && live < point->nlive; ++live) {
        const XiValue *identity =
            canonical_coroutine_live_identity(context, function, point, point->live[live]);
        bool duplicate = false;
        for (uint32_t prior = 0u; prior < live; ++prior)
            duplicate |= canonical_coroutine_live_identity(context, function, point,
                                                           point->live[prior]) == identity;
        occurrences += !duplicate && identity == target ? 1u : 0u;
    }
    occurrences +=
        coroutine_capture_receiver_is_live(function, point) && target == &function->capture_receiver
            ? 1u
            : 0u;
    return occurrences;
}

static bool coroutine_live_set_matches(XrXiBuildContext *context,
                                       const XrXiFunctionStorage *function,
                                       const XrXiBlockStorage *predecessor,
                                       const XrXiBlockStorage *successor,
                                       const XiCoroSuspendPoint *point) {
    uint32_t live_count = canonical_coroutine_live_count(context, function, point);
    if (!point || !successor || live_count == UINT32_MAX || successor->argument_count != live_count)
        return false;
    for (uint32_t argument = 0u; argument < successor->argument_count; ++argument) {
        const XiValue *incoming = edge_argument_value(&successor->argument_storage[argument],
                                                      predecessor->xi, successor->xi, 0u);
        const XiValue *logical_incoming =
            canonical_coroutine_live_identity(context, function, point, incoming);
        uint16_t type_id = XR_CORE_TYPE_VOID;
        XrCoreIrOwnershipDisposition ownership = XR_CORE_IR_NON_OWNER;
        XrCoreIrValueCategory category = logical_value_category(function->xi, logical_incoming);
        if (!logical_incoming || category > XR_CORE_IR_PLACE ||
            !coroutine_live_contract(context, function, logical_incoming, &type_id, &ownership) ||
            type_id != successor->argument_storage[argument].type_id ||
            canonical_coroutine_live_occurrences(context, function, point, logical_incoming) !=
                1u ||
            successor->argument_storage[argument].category != category ||
            successor->argument_storage[argument].ownership != ownership)
            return false;
        for (uint32_t prior = 0u; prior < argument; ++prior) {
            const XiValue *prior_incoming = edge_argument_value(&successor->argument_storage[prior],
                                                                predecessor->xi, successor->xi, 0u);
            if (canonical_coroutine_live_identity(context, function, point, prior_incoming) ==
                logical_incoming)
                return false;
        }
    }
    return true;
}

static bool coroutine_cancel_drop_set_matches(const XrXiBuildContext *context,
                                              const XrXiFunctionStorage *function,
                                              const XrXiBlockStorage *resume,
                                              uint32_t resume_argument_start, uint32_t live_count,
                                              const XiCoroSuspendPoint *point) {
    if (!context || !function || !resume || !point ||
        resume_argument_start > resume->argument_count ||
        live_count != resume->argument_count - resume_argument_start || point->ndrops > live_count)
        return false;
    uint32_t owner_count = 0u;
    for (uint32_t live = 0u; live < live_count; ++live) {
        const XrXiBlockArgumentStorage *argument =
            &resume->argument_storage[resume_argument_start + live];
        bool owner = argument->ownership == XR_CORE_IR_OWNER;
        const XiValue *logical =
            owner ? canonical_owner_storage_identity(context, function, argument->source)
                  : canonical_coroutine_live_identity(context, function, point, argument->source);
        uint16_t type_id = XR_CORE_TYPE_VOID;
        XrCoreIrOwnershipDisposition expected_ownership = XR_CORE_IR_NON_OWNER;
        uint32_t drop_matches = 0u;
        for (uint32_t drop = 0u; drop < point->ndrops; ++drop)
            drop_matches += canonical_cancel_drop_identity(context, function, point,
                                                           point->drops[drop]) == logical;
        if (!logical || argument->category > XR_CORE_IR_PLACE ||
            !coroutine_live_contract(context, function, logical, &type_id, &expected_ownership) ||
            type_id != argument->type_id || argument->ownership != expected_ownership ||
            drop_matches != (owner ? 1u : 0u))
            return false;
        owner_count += owner ? 1u : 0u;
    }
    return owner_count == point->ndrops;
}

static XrProgramBuildStatus
prepare_cancel_block(const XrXiBuildContext *context, XrXiFunctionStorage *function,
                     const XrXiBlockStorage *resume, uint32_t resume_argument_start,
                     uint32_t live_count, const XiCoroSuspendPoint *point, uint32_t safepoint_id,
                     char *diagnostic, size_t diagnostic_size) {
    if (!function || !function->cancel_graphs)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi coroutine safepoint %u has no cancellation storage", safepoint_id);
    if (!coroutine_cancel_drop_set_matches(context, function, resume, resume_argument_start,
                                           live_count, point))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi function %s safepoint %u cancellation live/drop set is inconsistent",
                    function->xi && function->xi->name ? function->xi->name : "<anonymous>",
                    safepoint_id);
    XrXiCancelGraphStorage *cancel = &function->cancel_graphs[safepoint_id];
    if (cancel->blocks || cancel->block_count != 0u)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi function %s safepoint %u cancellation block was prepared twice",
                    function->xi && function->xi->name ? function->xi->name : "<anonymous>",
                    safepoint_id);
    cancel->blocks = xr_calloc(1u, sizeof(*cancel->blocks));
    if (!cancel->blocks)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    cancel->block_count = 1u;
    XrCoreIrBlockInput *block = &cancel->blocks[0];
    block->key = cancel_block_key(function, safepoint_id);
    block->argument_count = point->ndrops;
    XrCoreIrValueInput *arguments =
        point->ndrops ? xr_calloc(point->ndrops, sizeof(*arguments)) : NULL;
    if (point->ndrops && !arguments)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    block->arguments = arguments;
    for (uint32_t drop = 0u; drop < point->ndrops; ++drop) {
        const XiValue *logical_drop =
            canonical_cancel_drop_identity(context, function, point, point->drops[drop]);
        const XrXiBlockArgumentStorage *source = NULL;
        for (uint32_t live = 0u; live < live_count; ++live) {
            const XrXiBlockArgumentStorage *candidate =
                &resume->argument_storage[resume_argument_start + live];
            if (exact_logical_value_identity(context, function->xi, candidate->source) ==
                logical_drop) {
                source = candidate;
                break;
            }
        }
        if (!source || source->category != XR_CORE_IR_VALUE ||
            source->ownership != XR_CORE_IR_OWNER)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi function %s safepoint %u cancellation owner v%u is not an exact "
                        "resume owner",
                        function->xi && function->xi->name ? function->xi->name : "<anonymous>",
                        safepoint_id, logical_drop ? logical_drop->id : UINT32_MAX);
        arguments[drop] = (XrCoreIrValueInput) {
            .key = cancel_argument_key(function, safepoint_id, drop),
            .type_id = source->type_id,
            .category = XR_CORE_IR_VALUE,
            .ownership = XR_CORE_IR_OWNER,
        };
    }
    block->instruction_count = (point->ndrops != 0u ? 1u : 0u) + point->ndrops + 1u;
    XrCoreIrInstructionInput *instructions =
        xr_calloc(block->instruction_count, sizeof(*instructions));
    if (!instructions)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    block->instructions = instructions;
    uint32_t instruction = 0u;
    if (point->ndrops != 0u) {
        XrCoreIrKey *operands = xr_calloc(point->ndrops, sizeof(*operands));
        if (!operands)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        for (uint32_t drop = 0u; drop < point->ndrops; ++drop)
            operands[drop] = arguments[drop].key;
        instructions[instruction++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = operands,
            .operand_count = point->ndrops,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    }
    for (uint32_t drop = 0u; drop < point->ndrops; ++drop) {
        XrCoreIrKey *operand = xr_calloc(1u, sizeof(*operand));
        if (!operand)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        *operand = arguments[drop].key;
        instructions[instruction++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = operand,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    }
    instructions[instruction] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    return XR_PROGRAM_BUILD_OK;
}

static bool remap_cancel_cleanup_key(const XrCoreIrKey *source_keys, const XrCoreIrKey *target_keys,
                                     uint32_t key_count, XrCoreIrKey source, XrCoreIrKey *target) {
    for (uint32_t index = 0u; index < key_count; ++index) {
        if (!xr_core_ir_key_equal(source_keys[index], source))
            continue;
        *target = target_keys[index];
        return true;
    }
    return false;
}

static XrProgramBuildStatus static_cleanup_graph_block_count(const XiFunc *function,
                                                             const XiValue *registration,
                                                             uint32_t *count_out) {
    if (count_out)
        *count_out = 0u;
    if (!function || !registration || !count_out)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint8_t *members = function->nblocks ? xr_calloc(function->nblocks, sizeof(*members)) : NULL;
    uint32_t *stack = function->nblocks ? xr_calloc(function->nblocks, sizeof(*stack)) : NULL;
    uint8_t *completes =
        function->nblocks ? xr_calloc(function->nblocks, sizeof(*completes)) : NULL;
    if (function->nblocks && (!members || !stack || !completes)) {
        xr_free(completes);
        xr_free(stack);
        xr_free(members);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    bool exact =
        static_cleanup_handler_graph_is_exact(function, registration, members, stack, completes);
    uint32_t count = 0u;
    if (exact)
        for (uint32_t block = 0u; block < function->nblocks; ++block)
            count += members[block] != 0u;
    xr_free(completes);
    xr_free(stack);
    xr_free(members);
    if (!exact || count == 0u)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    *count_out = count;
    return XR_PROGRAM_BUILD_OK;
}

static const XrCoreIrBlockInput *cancel_graph_source_block(const XrXiFunctionStorage *function,
                                                           uint32_t source_block_count,
                                                           const XiBlock *source) {
    XrCoreIrKey key = block_key(function, source);
    for (uint32_t block = 0u; function && block < source_block_count; ++block)
        if (xr_core_ir_key_equal(function->blocks[block].key, key))
            return &function->blocks[block];
    return NULL;
}

static bool cancel_graph_has_argument(const XrCoreIrBlockInput *const *blocks, uint32_t block_count,
                                      XrCoreIrKey key) {
    for (uint32_t block = 0u; block < block_count; ++block)
        for (uint32_t argument = 0u; blocks[block] && argument < blocks[block]->argument_count;
             ++argument)
            if (xr_core_ir_key_equal(blocks[block]->arguments[argument].key, key))
                return true;
    return false;
}

/* A static cleanup body can serve several termination reasons or suspension
 * points in Xi.  Each
 * extra cancellation use gets a fully private, re-keyed
 * Program subgraph.  Ordinary CFG
 * successors stay within that subgraph, while
 * trap continuations raised by cleanup operations
 * keep pointing at the already
 * closed trap projection. */
static XrProgramBuildStatus
clone_cancel_cleanup_graph(const XrXiBuildContext *context, XrXiFunctionStorage *function,
                           const XrXiBlockStorage *handler, const XiValue *registration,
                           const XiCoroSuspendPoint *point, uint32_t safepoint_id,
                           uint32_t source_block_count, char *diagnostic, size_t diagnostic_size) {
    if (!context || !function || !function->xi || !function->cancel_graphs || !handler ||
        !handler->xi || !registration || !point ||
        !cancel_handler_payload_is_exact(context, function, handler, point))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi shared cleanup has no exact cancellable Program projection");

    uint32_t xi_block_count = function->xi->nblocks;
    uint8_t *members = xi_block_count ? xr_calloc(xi_block_count, sizeof(*members)) : NULL;
    uint32_t *stack = xi_block_count ? xr_calloc(xi_block_count, sizeof(*stack)) : NULL;
    uint8_t *completes = xi_block_count ? xr_calloc(xi_block_count, sizeof(*completes)) : NULL;
    if (xi_block_count && (!members || !stack || !completes)) {
        xr_free(completes);
        xr_free(stack);
        xr_free(members);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    if (!static_cleanup_handler_graph_is_exact(function->xi, registration, members, stack,
                                               completes)) {
        xr_free(completes);
        xr_free(stack);
        xr_free(members);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi shared cleanup graph changed before cancellation projection");
    }
    xr_free(completes);
    xr_free(stack);

    uint32_t member_count = 0u;
    for (uint32_t block = 0u; block < xi_block_count; ++block)
        member_count += members[block] != 0u;
    XrXiCancelGraphStorage *cancel = &function->cancel_graphs[safepoint_id];
    if (member_count == 0u || cancel->blocks || cancel->block_count != 0u) {
        xr_free(members);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi cancellation cleanup graph was materialized twice or is empty");
    }

    const XiBlock **xi_blocks = xr_calloc(member_count, sizeof(*xi_blocks));
    const XrCoreIrBlockInput **source_blocks = xr_calloc(member_count, sizeof(*source_blocks));
    XrCoreIrKey *source_keys = NULL;
    XrCoreIrKey *target_keys = NULL;
    cancel->blocks = xr_calloc(member_count, sizeof(*cancel->blocks));
    if (!xi_blocks || !source_blocks || !cancel->blocks) {
        xr_free(source_blocks);
        xr_free(xi_blocks);
        xr_free(members);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    cancel->block_count = member_count;
    xi_blocks[0] = handler->xi;
    uint32_t cursor = 1u;
    for (uint32_t block = 0u; block < xi_block_count; ++block)
        if (members[block] && function->xi->blocks[block] != handler->xi)
            xi_blocks[cursor++] = function->xi->blocks[block];
    xr_free(members);
    if (cursor != member_count) {
        xr_free(source_blocks);
        xr_free(xi_blocks);
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }

    uint64_t map_capacity_wide = 0u;
    for (uint32_t block = 0u; block < member_count; ++block) {
        source_blocks[block] =
            cancel_graph_source_block(function, source_block_count, xi_blocks[block]);
        if (!source_blocks[block] || source_blocks[block]->instruction_count == 0u) {
            xr_free(source_blocks);
            xr_free(xi_blocks);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi shared cleanup graph has no Program source block");
        }
        uint64_t block_capacity = (uint64_t) source_blocks[block]->argument_count +
                                  source_blocks[block]->instruction_count;
        if (block_capacity > UINT64_MAX - map_capacity_wide) {
            xr_free(source_blocks);
            xr_free(xi_blocks);
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        }
        map_capacity_wide += block_capacity;
    }
    if (map_capacity_wide > UINT32_MAX || map_capacity_wide > SIZE_MAX / sizeof(XrCoreIrKey)) {
        xr_free(source_blocks);
        xr_free(xi_blocks);
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    }
    uint32_t map_capacity = (uint32_t) map_capacity_wide;
    source_keys = map_capacity ? xr_calloc(map_capacity, sizeof(*source_keys)) : NULL;
    target_keys = map_capacity ? xr_calloc(map_capacity, sizeof(*target_keys)) : NULL;
    if (map_capacity && (!source_keys || !target_keys)) {
        xr_free(target_keys);
        xr_free(source_keys);
        xr_free(source_blocks);
        xr_free(xi_blocks);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }

    XrProgramBuildStatus status = XR_PROGRAM_BUILD_OK;
    uint32_t map_count = 0u;
    for (uint32_t block = 0u; block < member_count && status == XR_PROGRAM_BUILD_OK; ++block) {
        const XrXiBlockStorage *source_storage = find_block_storage(function, xi_blocks[block]);
        const XrCoreIrBlockInput *source = source_blocks[block];
        XrCoreIrBlockInput *target = &cancel->blocks[block];
        if (!source_storage || source_storage->argument_count != source->argument_count) {
            status = XR_PROGRAM_BUILD_INVALID_INPUT;
            break;
        }
        target->key = cancel_graph_block_key(function, safepoint_id, xi_blocks[block], handler->xi);
        uint32_t argument_count = 0u;
        for (uint32_t argument = 0u; argument < source->argument_count; ++argument)
            argument_count +=
                !(block == 0u && source_storage->argument_storage[argument].implicit_invoke_kind ==
                                     XR_XI_INVOKE_ARGUMENT_PANIC);
        target->argument_count = argument_count;
        XrCoreIrValueInput *target_arguments =
            argument_count ? xr_calloc(argument_count, sizeof(*target_arguments)) : NULL;
        if (argument_count && !target_arguments) {
            status = XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            break;
        }
        target->arguments = target_arguments;
        uint32_t target_argument = 0u;
        for (uint32_t argument = 0u; argument < source->argument_count; ++argument) {
            const XrXiBlockArgumentStorage *logical = &source_storage->argument_storage[argument];
            if (!xr_core_ir_key_equal(source->arguments[argument].key, logical->key)) {
                status = XR_PROGRAM_BUILD_INVALID_INPUT;
                break;
            }
            if (block == 0u && logical->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_PANIC)
                continue;
            target_arguments[target_argument] = source->arguments[argument];
            target_arguments[target_argument].key =
                cancel_graph_argument_key(target->key, target_argument);
            source_keys[map_count] = source->arguments[argument].key;
            target_keys[map_count++] = target_arguments[target_argument++].key;
        }
        if (target_argument != argument_count)
            status = XR_PROGRAM_BUILD_INVALID_INPUT;
        for (uint32_t instruction = 0u;
             instruction < source->instruction_count && status == XR_PROGRAM_BUILD_OK;
             ++instruction) {
            if (xr_core_ir_key_is_zero(source->instructions[instruction].result))
                continue;
            source_keys[map_count] = source->instructions[instruction].result;
            target_keys[map_count++] = cancel_graph_result_key(target->key, instruction);
        }
    }

    uint32_t reason_terminal_count = 0u;
    for (uint32_t block = 0u; block < member_count && status == XR_PROGRAM_BUILD_OK; ++block) {
        const XrCoreIrBlockInput *source = source_blocks[block];
        XrCoreIrBlockInput *target = &cancel->blocks[block];
        bool has_block_arguments =
            source->instructions[0].operation_id == XR_CORE_OP_CORE_BLOCK_ARGUMENT;
        bool omit_empty_arguments = has_block_arguments && target->argument_count == 0u;
        target->instruction_count = source->instruction_count - (omit_empty_arguments ? 1u : 0u);
        XrCoreIrInstructionInput *target_instructions =
            xr_calloc(target->instruction_count, sizeof(*target_instructions));
        if (!target_instructions) {
            status = XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            break;
        }
        target->instructions = target_instructions;
        uint32_t target_instruction = 0u;
        for (uint32_t instruction = 0u; instruction < source->instruction_count; ++instruction) {
            const XrCoreIrInstructionInput *from = &source->instructions[instruction];
            if (instruction == 0u && has_block_arguments) {
                if (omit_empty_arguments)
                    continue;
                XrCoreIrKey *operands = xr_calloc(target->argument_count, sizeof(*operands));
                if (!operands) {
                    status = XR_PROGRAM_BUILD_OUT_OF_MEMORY;
                    break;
                }
                for (uint32_t argument = 0u; argument < target->argument_count; ++argument)
                    operands[argument] = target->arguments[argument].key;
                target_instructions[target_instruction++] = (XrCoreIrInstructionInput) {
                    .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
                    .result_type_id = XR_CORE_TYPE_VOID,
                    .operands = operands,
                    .operand_count = target->argument_count,
                    .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
                };
                continue;
            }
            bool reason_terminal = instruction + 1u == source->instruction_count &&
                                   from->successor_count == 0u &&
                                   (from->operation_id == XR_CORE_OP_CORE_TRAP ||
                                    from->operation_id == XR_CORE_OP_CORE_PANIC_PUBLISH ||
                                    from->operation_id == XR_CORE_OP_CORE_CANCEL_PUBLISH);
            if (reason_terminal) {
                target_instructions[target_instruction++] = (XrCoreIrInstructionInput) {
                    .operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH,
                    .result_type_id = XR_CORE_TYPE_VOID,
                    .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
                };
                ++reason_terminal_count;
                continue;
            }
            XrCoreIrInstructionInput *to = &target_instructions[target_instruction++];
            *to = *from;
            to->operands = NULL;
            to->successors = NULL;
            if (!xr_core_ir_key_is_zero(from->result) &&
                !remap_cancel_cleanup_key(source_keys, target_keys, map_count, from->result,
                                          &to->result)) {
                status = XR_PROGRAM_BUILD_INVALID_INPUT;
                break;
            }
            if (from->operand_count != 0u) {
                XrCoreIrKey *operands = xr_calloc(from->operand_count, sizeof(*operands));
                if (!operands) {
                    status = XR_PROGRAM_BUILD_OUT_OF_MEMORY;
                    break;
                }
                for (uint32_t operand = 0u; operand < from->operand_count; ++operand) {
                    if (!remap_cancel_cleanup_key(source_keys, target_keys, map_count,
                                                  from->operands[operand], &operands[operand])) {
                        if (cancel_graph_has_argument(source_blocks, member_count,
                                                      from->operands[operand])) {
                            xr_free(operands);
                            status = XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE;
                            break;
                        }
                        operands[operand] = from->operands[operand];
                    }
                }
                if (status != XR_PROGRAM_BUILD_OK)
                    break;
                to->operands = operands;
            }
            if (from->successor_count != 0u) {
                XrCoreIrKey *successors = xr_calloc(from->successor_count, sizeof(*successors));
                if (!successors) {
                    status = XR_PROGRAM_BUILD_OUT_OF_MEMORY;
                    break;
                }
                for (uint32_t successor = 0u; successor < from->successor_count; ++successor) {
                    successors[successor] = from->successors[successor];
                    for (uint32_t member = 0u; member < member_count; ++member)
                        if (xr_core_ir_key_equal(from->successors[successor],
                                                 source_blocks[member]->key)) {
                            successors[successor] = cancel->blocks[member].key;
                            break;
                        }
                }
                to->successors = successors;
            }
        }
        if (target_instruction != target->instruction_count)
            status = XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    if (status == XR_PROGRAM_BUILD_OK && reason_terminal_count != 1u)
        status = XR_PROGRAM_BUILD_INVALID_INPUT;
    xr_free(target_keys);
    xr_free(source_keys);
    xr_free(source_blocks);
    xr_free(xi_blocks);
    if (status == XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE)
        return fail(diagnostic, diagnostic_size, status,
                    "Xi shared cleanup graph depends on a reason-specific payload");
    if (status != XR_PROGRAM_BUILD_OK)
        return fail(diagnostic, diagnostic_size, status,
                    "Xi shared cleanup graph could not be projected for cancellation");
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus append_cancel_edge(XrXiBuildContext *context, const XiFunc *function,
                                               const XiCoroSuspendPoint *point,
                                               const XiValue *registration,
                                               bool private_projection) {
    if (context->cancel_edge_count == context->cancel_edge_capacity) {
        uint32_t capacity = context->cancel_edge_capacity ? context->cancel_edge_capacity * 2u : 4u;
        if (capacity < context->cancel_edge_capacity)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        XrXiCancelEdge *edges =
            xr_realloc(context->cancel_edges, (size_t) capacity * sizeof(*edges));
        if (!edges)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        context->cancel_edges = edges;
        context->cancel_edge_capacity = capacity;
    }
    context->cancel_edges[context->cancel_edge_count++] = (XrXiCancelEdge) {
        .function = function,
        .point = point,
        .registration = registration,
        .handler = (const XiBlock *) registration->aux,
        .private_projection = private_projection,
    };
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus append_cancel_drop_operands(const XrXiBuildContext *context,
                                                        XrCoreIrInstructionInput *instruction,
                                                        const XrXiFunctionStorage *function,
                                                        const XrXiBlockStorage *source,
                                                        const XiCoroSuspendPoint *point,
                                                        char *diagnostic, size_t diagnostic_size) {
    if (!context || !instruction || !function || !source || !point ||
        point->ndrops > UINT32_MAX - instruction->operand_count)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint32_t count = instruction->operand_count + point->ndrops;
    XrCoreIrKey *operands = count ? xr_calloc(count, sizeof(*operands)) : NULL;
    if (count && !operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (instruction->operand_count != 0u)
        memcpy(operands, instruction->operands,
               (size_t) instruction->operand_count * sizeof(*operands));
    for (uint32_t drop = 0u; drop < point->ndrops; ++drop) {
        const XiValue *logical_drop =
            canonical_cancel_drop_identity(context, function, point, point->drops[drop]);
        if (!logical_drop ||
            !edge_value_operand_key(context, function, source, logical_drop, point->op,
                                    &operands[instruction->operand_count + drop])) {
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi cancel owner %u is unavailable at suspension",
                        point->drops[drop] ? point->drops[drop]->id : UINT32_MAX);
        }
    }
    xr_free((void *) instruction->operands);
    instruction->operands = operands;
    instruction->operand_count = count;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
append_cancel_handler_operands(const XrXiBuildContext *context,
                               XrCoreIrInstructionInput *instruction,
                               const XrXiFunctionStorage *function, const XrXiBlockStorage *source,
                               const XrXiBlockStorage *handler, const XiCoroSuspendPoint *point,
                               char *diagnostic, size_t diagnostic_size) {
    if (!context || !instruction || !function || !source || !handler || !point ||
        !cancel_handler_payload_is_exact(context, function, handler, point))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi cancel handler has no exact owner payload");
    uint32_t handler_argument_count = 0u;
    for (uint32_t argument = 0u; argument < handler->argument_count; ++argument)
        handler_argument_count +=
            handler->argument_storage[argument].implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_PANIC;
    if (handler_argument_count > UINT32_MAX - instruction->operand_count)
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    uint32_t count = instruction->operand_count + handler_argument_count;
    XrCoreIrKey *operands = count ? xr_calloc(count, sizeof(*operands)) : NULL;
    if (count && !operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (instruction->operand_count != 0u)
        memcpy(operands, instruction->operands,
               (size_t) instruction->operand_count * sizeof(*operands));
    uint32_t cursor = instruction->operand_count;
    for (uint32_t argument = 0u; argument < handler->argument_count; ++argument) {
        const XrXiBlockArgumentStorage *edge_argument = &handler->argument_storage[argument];
        if (edge_argument->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_PANIC)
            continue;
        if (edge_argument->phi ||
            edge_argument->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE ||
            !edge_value_operand_key(context, function, source, edge_argument->source, point->op,
                                    &operands[cursor++])) {
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi cancel handler owner is unavailable at suspension");
        }
    }
    if (cursor != count) {
        xr_free(operands);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi cancel handler argument count changed during translation");
    }
    xr_free((void *) instruction->operands);
    instruction->operands = operands;
    instruction->operand_count = count;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
translate_coroutine_yield_terminator(XrXiBuildContext *context, XrXiFunctionStorage *function,
                                     XrXiBlockStorage *block, const XiCoroSuspendPoint *point,
                                     XrCoreIrInstructionInput *instruction, char *diagnostic,
                                     size_t diagnostic_size) {
    XrXiBlockStorage *resume = point ? find_block_storage(function, point->resume_block) : NULL;
    const XrXiCancelEdge *cancel_edge = find_cancel_edge(context, function->xi, point);
    XrXiBlockStorage *cancel_handler =
        cancel_edge ? find_block_storage(function, cancel_edge->handler) : NULL;
    uint32_t safepoint_id = point && point->state_id != 0u ? point->state_id - 1u : UINT32_MAX;
    const XiCoroPlan *plan = function && function->xi ? function->xi->coro_plan : NULL;
    if (!context || !function || !block || !instruction || !point || !resume || !plan ||
        safepoint_id >= plan->nstates || !function->coroutine_safepoints ||
        !exact_cooperative_yield_contract(context, function, point->op) ||
        !coroutine_live_set_matches(context, function, block, resume, point) ||
        (cancel_edge && !cancel_handler))
        return fail(
            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
            "Xi cooperative yield %s:b%u has no exact canonical resume/live set "
            "(Xi live=%u canonical=%u resume=%u drops=%u)",
            function->xi && function->xi->name ? function->xi->name : "<anonymous>",
            block && block->xi ? block->xi->id : UINT32_MAX, point ? point->nlive : UINT32_MAX,
            canonical_coroutine_live_count(context, function, point),
            resume ? resume->argument_count : UINT32_MAX, point ? point->ndrops : UINT32_MAX);
    instruction->operation_id = XR_CORE_OP_CORE_COROUTINE_YIELD;
    instruction->result_type_id = XR_CORE_TYPE_VOID;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    instruction->immediate.u32 = safepoint_id;
    XrCoreIrKey *successors = xr_calloc(2u, sizeof(*successors));
    if (!successors)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    successors[0] = block_key(function, point->resume_block);
    successors[1] = cancel_edge && !cancel_edge->private_projection
                        ? block_key(function, cancel_edge->handler)
                        : cancel_block_key(function, safepoint_id);
    instruction->successors = successors;
    instruction->successor_count = 2u;
    XrProgramBuildStatus status = set_edge_operands(context, instruction, function, block, resume,
                                                    NULL, NULL, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;

    uint32_t live_count = instruction->operand_count;
    XrCoreIrKey *live_values = live_count ? xr_calloc(live_count, sizeof(*live_values)) : NULL;
    if (live_count && !live_values)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (live_count)
        memcpy(live_values, instruction->operands, (size_t) live_count * sizeof(*live_values));
    function->coroutine_safepoints[safepoint_id].live_values = live_values;
    function->coroutine_safepoints[safepoint_id].live_value_count = live_count;
    if (cancel_edge) {
        if (!coroutine_cancel_drop_set_matches(context, function, resume, 0u, live_count, point))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi cooperative yield has no exact cancellation owner set");
        return append_cancel_handler_operands(context, instruction, function, block, cancel_handler,
                                              point, diagnostic, diagnostic_size);
    }
    status = prepare_cancel_block(context, function, resume, 0u, live_count, point, safepoint_id,
                                  diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    return append_cancel_drop_operands(context, instruction, function, block, point, diagnostic,
                                       diagnostic_size);
}

static XrProgramBuildStatus
translate_coroutine_suspend_terminator(XrXiBuildContext *context, XrXiFunctionStorage *function,
                                       XrXiBlockStorage *block, const XiCoroSuspendPoint *point,
                                       XrCoreIrInstructionInput *instruction, char *diagnostic,
                                       size_t diagnostic_size) {
    XrXiBlockStorage *resume = point ? find_block_storage(function, point->resume_block) : NULL;
    const XrXiCancelEdge *cancel_edge = find_cancel_edge(context, function->xi, point);
    XrXiBlockStorage *cancel_handler =
        cancel_edge ? find_block_storage(function, cancel_edge->handler) : NULL;
    uint32_t safepoint_id = point && point->state_id != 0u ? point->state_id - 1u : UINT32_MAX;
    const XiCoroPlan *plan = function && function->xi ? function->xi->coro_plan : NULL;
    const XrStdlibDefEntry *entry =
        point ? resolved_suspension_native_call(context, function->xi, point->op) : NULL;
    uint16_t request_type = XR_CORE_TYPE_VOID;
    if (!context || !function || !block || !instruction || !point || !resume || !plan || !entry ||
        safepoint_id >= plan->nstates || !function->coroutine_safepoints ||
        point->op->nargs != 2u || !point->op->args ||
        !map_logical_value_type(context, function->xi, point->op->args[1], &request_type) ||
        request_type != XR_CORE_TYPE_I64 ||
        !coroutine_live_set_matches(context, function, block, resume, point) ||
        (cancel_edge && !cancel_handler))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi suspension request in b%u has no exact canonical resume/live set",
                    block && block->xi ? block->xi->id : UINT32_MAX);

    instruction->operation_id = XR_CORE_OP_CORE_COROUTINE_SUSPEND;
    instruction->result_type_id = XR_CORE_TYPE_VOID;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_COROUTINE_SUSPEND;
    instruction->immediate.coroutine_suspend.safepoint_id = safepoint_id;
    instruction->immediate.coroutine_suspend.request_kind = XR_SUSPENSION_REQUEST_TIMER_AFTER_MS;
    instruction->immediate.coroutine_suspend.request_operand_count = 1u;
    XrCoreIrKey *successors = xr_calloc(2u, sizeof(*successors));
    if (!successors)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    successors[0] = block_key(function, point->resume_block);
    successors[1] = cancel_edge && !cancel_edge->private_projection
                        ? block_key(function, cancel_edge->handler)
                        : cancel_block_key(function, safepoint_id);
    instruction->successors = successors;
    instruction->successor_count = 2u;
    XrProgramBuildStatus status = set_edge_operands(context, instruction, function, block, resume,
                                                    NULL, NULL, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;

    uint32_t live_count = instruction->operand_count;
    if (live_count == UINT32_MAX)
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    XrCoreIrKey *operands = xr_calloc((size_t) live_count + 1u, sizeof(*operands));
    if (!operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (!edge_value_operand_key(context, function, block, point->op->args[1], point->op,
                                &operands[0])) {
        xr_free(operands);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi suspension request value is unavailable");
    }
    if (live_count != 0u)
        memcpy(operands + 1u, instruction->operands, (size_t) live_count * sizeof(*operands));
    xr_free((void *) instruction->operands);
    instruction->operands = operands;
    instruction->operand_count = live_count + 1u;

    XrCoreIrKey *live_values = live_count ? xr_calloc(live_count, sizeof(*live_values)) : NULL;
    if (live_count && !live_values)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (live_count)
        memcpy(live_values, operands + 1u, (size_t) live_count * sizeof(*live_values));
    function->coroutine_safepoints[safepoint_id].live_values = live_values;
    function->coroutine_safepoints[safepoint_id].live_value_count = live_count;
    if (cancel_edge) {
        if (!coroutine_cancel_drop_set_matches(context, function, resume, 0u, live_count, point))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi suspension request has no exact cancellation owner set");
        return append_cancel_handler_operands(context, instruction, function, block, cancel_handler,
                                              point, diagnostic, diagnostic_size);
    }
    status = prepare_cancel_block(context, function, resume, 0u, live_count, point, safepoint_id,
                                  diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    return append_cancel_drop_operands(context, instruction, function, block, point, diagnostic,
                                       diagnostic_size);
}

static const char *coroutine_call_live_set_mismatch(XrXiBuildContext *context,
                                                    const XrXiFunctionStorage *function,
                                                    const XrXiBlockStorage *resume,
                                                    const XiCoroSuspendPoint *point,
                                                    uint32_t implicit_result_count) {
    uint32_t live_count = canonical_coroutine_live_count(context, function, point);
    if (!context || !function || !resume || !point || live_count == UINT32_MAX ||
        resume->argument_count < implicit_result_count)
        return "live-set input is incomplete";
    if (live_count != resume->argument_count)
        return "canonical live count disagrees with resume arguments";
    if (implicit_result_count != 0u &&
        (resume->argument_storage[0].implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT ||
         exact_logical_value_identity(context, function->xi, resume->argument_storage[0].source) !=
             exact_logical_value_identity(context, function->xi, point->op) ||
         canonical_coroutine_live_occurrences(context, function, point, point->op) != 1u))
        return "implicit result identity disagrees with the call";
    for (uint32_t index = implicit_result_count; index < resume->argument_count; ++index) {
        const XrXiBlockArgumentStorage *argument = &resume->argument_storage[index];
        const XiValue *logical =
            canonical_coroutine_live_identity(context, function, point, argument->source);
        uint16_t type_id = XR_CORE_TYPE_VOID;
        XrCoreIrOwnershipDisposition ownership = XR_CORE_IR_NON_OWNER;
        if (!logical ||
            !coroutine_live_contract(context, function, logical, &type_id, &ownership) ||
            type_id != argument->type_id)
            return "resume value has no matching canonical type";
        XrCoreIrValueCategory category = logical_value_category(function->xi, logical);
        if (category > XR_CORE_IR_PLACE || argument->category != category ||
            argument->ownership != ownership)
            return "resume value category or ownership disagrees";
        if (canonical_coroutine_live_occurrences(context, function, point, logical) != 1u ||
            argument->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE)
            return "resume value does not name an exact live operand";
        for (uint32_t prior = implicit_result_count; prior < index; ++prior)
            if (canonical_coroutine_live_identity(
                    context, function, point, resume->argument_storage[prior].source) == logical)
                return "resume values repeat one canonical identity";
    }
    return NULL;
}

static XrProgramBuildStatus append_coroutine_trap_operands(XrCoreIrInstructionInput *instruction,
                                                           const XrCoreIrInstructionInput *call,
                                                           uint32_t parameter_count,
                                                           uint32_t live_count, char *diagnostic,
                                                           size_t diagnostic_size) {
    if (call->successor_count == 0u)
        return XR_PROGRAM_BUILD_OK;
    uint32_t trap_count = call->operand_count - parameter_count;
    if (trap_count > UINT32_MAX - instruction->operand_count)
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    for (uint32_t argument = 0u; argument < trap_count; ++argument) {
        uint32_t occurrences = 0u;
        for (uint32_t live = 0u; live < live_count; ++live)
            occurrences += xr_core_ir_key_equal(call->operands[parameter_count + argument],
                                                instruction->operands[parameter_count + live]);
        if (occurrences != 1u)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi coroutine trap argument %u has no unique safepoint carrier", argument);
    }
    uint32_t count = instruction->operand_count + trap_count;
    XrCoreIrKey *operands = count ? xr_calloc(count, sizeof(*operands)) : NULL;
    if (count && !operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (instruction->operand_count != 0u)
        memcpy(operands, instruction->operands,
               (size_t) instruction->operand_count * sizeof(*operands));
    if (trap_count != 0u)
        memcpy(operands + instruction->operand_count, call->operands + parameter_count,
               (size_t) trap_count * sizeof(*operands));
    xr_free((void *) instruction->operands);
    instruction->operands = operands;
    instruction->operand_count = count;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus translate_coroutine_call_terminator(
    XrXiBuildContext *context, XrXiModuleStorage *module, XrXiFunctionStorage *function,
    XrXiBlockStorage *block, const XiCoroSuspendPoint *point, XrCoreIrInstructionInput *instruction,
    char *diagnostic, size_t diagnostic_size) {
    XrXiBlockStorage *resume = point ? find_block_storage(function, point->resume_block) : NULL;
    const XrXiCancelEdge *cancel_edge = find_cancel_edge(context, function->xi, point);
    XrXiBlockStorage *cancel_handler =
        cancel_edge ? find_block_storage(function, cancel_edge->handler) : NULL;
    uint32_t safepoint_id = point && point->state_id != 0u ? point->state_id - 1u : UINT32_MAX;
    XrCoreIrInstructionInput call = {0};
    XrProgramBuildStatus status = point ? translate_value(context, module, function, point->op,
                                                          block, &call, diagnostic, diagnostic_size)
                                        : XR_PROGRAM_BUILD_INVALID_INPUT;
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    bool indirect = call.operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT;
    uint32_t implicit_result = call.result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    const XrXiFunctionStorage *callee =
        point ? find_xi_function(context, point->resolved_callee, NULL, NULL) : NULL;
    const XrXiTrapEdge *trap_edge = find_trap_edge(context, function->xi, point->op);
    const XrXiBlockStorage *trap_handler =
        trap_edge ? find_block_storage(function, trap_edge->handler) : NULL;
    uint32_t parameter_count = callee && callee->xi ? callee->xi->nparams : 0u;
    if (indirect) {
        const XiValue *carrier = point && point->op && point->op->nargs && point->op->args
                                     ? logical_value_identity(point->op->args[0])
                                     : NULL;
        uint16_t callable_type_id = XR_CORE_TYPE_VOID;
        const XrXiTypeStorage *callable_type = NULL;
        if (carrier && map_callable_call_type(context, function->xi, point->op, carrier->type,
                                              &callable_type_id, NULL))
            callable_type = find_dynamic_type_by_id(context, callable_type_id);
        if (!callable_type || !callable_type->callable_signature ||
            callable_type->callable_signature->error_type_id != XR_CORE_TYPE_VOID ||
            callable_type->callable_signature->panic_type_id != XR_CORE_TYPE_VOID ||
            (callable_type->callable_signature->effect_mask &
             (XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND)) !=
                (XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND) ||
            (callable_type->callable_signature->capability_mask &
             XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION) == 0u ||
            callable_type->callable_signature->result_type_id != call.result_type_id ||
            callable_type->callable_signature->parameter_count == UINT32_MAX) {
            xr_free((void *) call.operands);
            xr_free((void *) call.successors);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi coroutine callable has no exact infallible signature");
        }
        parameter_count = callable_type->callable_signature->parameter_count + 1u;
    }
    const XiCoroPlan *plan = function && function->xi ? function->xi->coro_plan : NULL;
    const char *live_set_failure =
        coroutine_call_live_set_mismatch(context, function, resume, point, implicit_result);
    if (live_set_failure) {
        xr_free((void *) call.operands);
        xr_free((void *) call.successors);
        return fail(
            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
            "Xi coroutine call %s:b%u v%u core-op=%u callee=%s has no exact child "
            "continuation: %s (Xi live=%u canonical=%u resume=%u drops=%u)",
            function && function->xi && function->xi->name ? function->xi->name : "<anonymous>",
            block && block->xi ? block->xi->id : UINT32_MAX,
            point && point->op ? point->op->id : UINT32_MAX, (unsigned) call.operation_id,
            callee && callee->xi && callee->xi->name ? callee->xi->name : "<indirect>",
            live_set_failure, point ? point->nlive : UINT32_MAX,
            canonical_coroutine_live_count(context, function, point),
            resume ? resume->argument_count : UINT32_MAX, point ? point->ndrops : UINT32_MAX);
    }
    const char *closure_failure = NULL;
    if (!resume)
        closure_failure = "resume block is absent";
    else if (!callee && !indirect)
        closure_failure = "sealed callee is absent";
    else if (!plan)
        closure_failure = "coroutine plan is absent";
    else if (safepoint_id >= plan->nstates)
        closure_failure = "safepoint is outside the coroutine plan";
    else if (cancel_edge && !cancel_handler)
        closure_failure = "cancel handler is absent";
    else if (!indirect && call.immediate_kind != XR_CORE_IR_IMMEDIATE_FUNCTION)
        closure_failure = "call target is not a function key";
    else if (!indirect && !xr_core_ir_key_equal(call.immediate.key, callee->key))
        closure_failure = "call target disagrees with the sealed callee";
    else if (indirect && call.immediate_kind != XR_CORE_IR_IMMEDIATE_NONE)
        closure_failure = "indirect call retained an unexpected immediate";
    else if (call.successor_count != (trap_edge ? 1u : 0u) ||
             (trap_edge &&
              (!trap_handler || !call.successors ||
               !xr_core_ir_key_equal(call.successors[0], block_key(function, trap_edge->handler)))))
        closure_failure = "call retained a non-canonical trap successor";
    else if (call.operand_count < parameter_count ||
             call.operand_count - parameter_count !=
                 (trap_handler ? trap_handler->argument_count : 0u))
        closure_failure = "call parameter and trap operand segments disagree";
    if (closure_failure) {
        xr_free((void *) call.operands);
        xr_free((void *) call.successors);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi coroutine call in b%u has no exact child continuation: %s",
                    block && block->xi ? block->xi->id : UINT32_MAX, closure_failure);
    }
    uint32_t live_count = resume->argument_count - implicit_result;
    if (parameter_count > UINT32_MAX - live_count) {
        xr_free((void *) call.operands);
        xr_free((void *) call.successors);
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    }
    uint32_t operand_count = parameter_count + live_count;
    XrCoreIrKey *operands = operand_count ? xr_calloc(operand_count, sizeof(*operands)) : NULL;
    if (operand_count && !operands) {
        xr_free((void *) call.operands);
        xr_free((void *) call.successors);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    if (parameter_count)
        memcpy(operands, call.operands, (size_t) parameter_count * sizeof(*operands));
    for (uint32_t live = 0u; live < live_count; ++live) {
        const XiValue *incoming = resume->argument_storage[implicit_result + live].source;
        if (!edge_value_operand_key(context, function, block, incoming, point->op,
                                    &operands[parameter_count + live])) {
            xr_free(operands);
            xr_free((void *) call.operands);
            xr_free((void *) call.successors);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi coroutine call live value is unavailable");
        }
    }
    uint32_t successor_count = trap_edge ? 3u : 2u;
    XrCoreIrKey *successors = xr_calloc(successor_count, sizeof(*successors));
    XrCoreIrKey *live_values = live_count ? xr_calloc(live_count, sizeof(*live_values)) : NULL;
    if (!successors || (live_count && !live_values)) {
        xr_free(live_values);
        xr_free(successors);
        xr_free(operands);
        xr_free((void *) call.operands);
        xr_free((void *) call.successors);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    if (live_count)
        memcpy(live_values, operands + parameter_count, (size_t) live_count * sizeof(*live_values));
    successors[0] = block_key(function, point->resume_block);
    successors[1] = cancel_edge && !cancel_edge->private_projection
                        ? block_key(function, cancel_edge->handler)
                        : cancel_block_key(function, safepoint_id);
    if (trap_edge)
        successors[2] = call.successors[0];
    instruction->operation_id =
        indirect ? XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT : XR_CORE_OP_CORE_COROUTINE_CALL_SEALED;
    instruction->result_type_id = XR_CORE_TYPE_VOID;
    if (indirect) {
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
        instruction->immediate.u32 = safepoint_id;
    } else {
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_COROUTINE_CALL;
        instruction->immediate.coroutine_call.callee = callee->key;
        instruction->immediate.coroutine_call.safepoint_id = safepoint_id;
    }
    instruction->operands = operands;
    instruction->operand_count = operand_count;
    instruction->successors = successors;
    instruction->successor_count = successor_count;
    function->coroutine_safepoints[safepoint_id].live_values = live_values;
    function->coroutine_safepoints[safepoint_id].live_value_count = live_count;
    if (cancel_edge) {
        if (!coroutine_cancel_drop_set_matches(context, function, resume, implicit_result,
                                               live_count, point))
            status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                          "Xi coroutine call has no exact cancellation owner set");
        else
            status =
                append_cancel_handler_operands(context, instruction, function, block,
                                               cancel_handler, point, diagnostic, diagnostic_size);
    } else {
        status = prepare_cancel_block(context, function, resume, implicit_result, live_count, point,
                                      safepoint_id, diagnostic, diagnostic_size);
        if (status == XR_PROGRAM_BUILD_OK)
            status = append_cancel_drop_operands(context, instruction, function, block, point,
                                                 diagnostic, diagnostic_size);
    }
    if (status == XR_PROGRAM_BUILD_OK)
        status = append_coroutine_trap_operands(instruction, &call, parameter_count, live_count,
                                                diagnostic, diagnostic_size);
    xr_free((void *) call.operands);
    xr_free((void *) call.successors);
    return status;
}

static const XrCoreIrFunctionInput *input_function_by_key(const XrXiBuildContext *context,
                                                          XrCoreIrKey key) {
    for (uint32_t module = 0u; context && module < context->source->module_count; ++module) {
        const XrXiModuleStorage *storage = &context->storage[module];
        for (uint32_t function = 0u; function < storage->function_count; ++function) {
            const XrCoreIrFunctionInput *candidate = &storage->functions[function];
            if (xr_core_ir_key_equal(candidate->key, key))
                return candidate;
        }
    }
    return NULL;
}

static bool input_value_type(const XrXiBlockStorage *block, uint32_t instruction_count,
                             XrCoreIrKey key, uint16_t *type_id) {
    if (type_id)
        *type_id = XR_CORE_TYPE_VOID;
    if (!block || !type_id)
        return false;
    for (uint32_t argument = 0u; argument < block->argument_count; ++argument) {
        if (xr_core_ir_key_equal(block->argument_storage[argument].key, key)) {
            *type_id = block->argument_storage[argument].type_id;
            return true;
        }
    }
    for (uint32_t instruction = 0u; instruction < instruction_count; ++instruction) {
        const XrCoreIrInstructionInput *candidate = &block->instructions[instruction];
        if (candidate->result_type_id != XR_CORE_TYPE_VOID &&
            xr_core_ir_key_equal(candidate->result, key)) {
            *type_id = candidate->result_type_id;
            return true;
        }
    }
    return false;
}

static bool input_operation_consumes_operand(const XrXiBuildContext *context,
                                             const XrXiBlockStorage *block,
                                             uint32_t instruction_count,
                                             const XrCoreIrInstructionInput *instruction,
                                             uint32_t operand_index) {
    if ((instruction->operation_id == XR_CORE_OP_CORE_OWNER_MOVE ||
         instruction->operation_id == XR_CORE_OP_CORE_OWNER_DROP ||
         instruction->operation_id == XR_CORE_OP_CORE_PLACE_TAKE) &&
        operand_index == 0u)
        return true;
    if (instruction->operation_id == XR_CORE_OP_CORE_PLACE_STORE && operand_index == 1u)
        return true;
    if (instruction->operation_id == XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT ||
        instruction->operation_id == XR_CORE_OP_CORE_VARIANT_CONSTRUCT) {
        uint16_t operand_type = XR_CORE_TYPE_VOID;
        return input_value_type(block, instruction_count, instruction->operands[operand_index],
                                &operand_type) &&
               logical_ownership_for_type(context, operand_type) == XR_CORE_IR_OWNER;
    }
    if (instruction->operation_id == XR_CORE_OP_CORE_VARIANT_PROJECT &&
        instruction->result_ownership == XR_CORE_IR_OWNER)
        return operand_index == 0u;
    if ((instruction->operation_id == XR_CORE_OP_CORE_RETURN ||
         instruction->operation_id == XR_CORE_OP_CORE_ERROR_PUBLISH ||
         instruction->operation_id == XR_CORE_OP_CORE_PANIC_PUBLISH ||
         instruction->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK) &&
        operand_index == 0u)
        return true;
    if (instruction->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PACK && operand_index == 0u) {
        const XrXiTypeStorage *existential =
            find_dynamic_type_by_id(context, instruction->result_type_id);
        return existential && existential->input.kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
               (existential->input.interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE ||
                existential->input.interface_use_kind ==
                    XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE);
    }

    const XrParamMode *parameter_modes = NULL;
    uint32_t parameter_count = 0u;
    uint32_t prefix = 0u;
    if (instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_FUNCTION) {
        const XrCoreIrFunctionInput *callee =
            input_function_by_key(context, instruction->immediate.key);
        if (!callee)
            return false;
        parameter_modes = callee->parameter_modes;
        parameter_count = callee->parameter_count;
    } else if (instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT ||
               instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE ||
               instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) {
        if (instruction->operand_count == 0u)
            return false;
        uint16_t callable_type = XR_CORE_TYPE_VOID;
        if (!input_value_type(block, instruction_count, instruction->operands[0], &callable_type))
            return false;
        const XrXiTypeStorage *type = find_dynamic_type_by_id(context, callable_type);
        if (!type || !type->callable_signature)
            return false;
        parameter_modes = type->callable_signature->parameter_modes;
        parameter_count = type->callable_signature->parameter_count;
        prefix = 1u;
    } else if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT ||
               instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) {
        if (instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_U32 ||
            instruction->operand_count == 0u)
            return false;
        uint16_t receiver_type = XR_CORE_TYPE_VOID;
        if (!input_value_type(block, instruction_count, instruction->operands[0], &receiver_type))
            return false;
        const XrXiTypeStorage *existential = find_dynamic_type_by_id(context, receiver_type);
        if (!existential || existential->input.kind != XR_CORE_IR_TYPE_EXISTENTIAL)
            return false;
        const XrCoreIrCallableSignatureInput *slot = interface_slot_contract_by_key(
            context, existential->input.existential_interface, instruction->immediate.u32);
        if (!slot)
            return false;
        parameter_modes = slot->parameter_modes;
        parameter_count = slot->parameter_count;
    } else {
        return false;
    }
    return operand_index >= prefix && operand_index - prefix < parameter_count && parameter_modes &&
           parameter_modes[operand_index - prefix] == XR_PARAM_MOVE;
}

static uint32_t owner_edge_occurrences(const XrXiBuildContext *context,
                                       const XrXiFunctionStorage *function,
                                       const XrXiBlockStorage *predecessor, XrCoreIrKey owner,
                                       const XiBlock *successor, uint32_t predecessor_occurrence) {
    const XrXiBlockStorage *target = find_block_storage(function, successor);
    uint32_t occurrences = 0u;
    for (uint32_t argument = 0u; target && argument < target->argument_count; ++argument) {
        const XiValue *incoming =
            edge_argument_value(&target->argument_storage[argument], predecessor->xi, successor,
                                predecessor_occurrence);
        XrCoreIrKey key = {{0}};
        const XiValue *edge_point = block_typed_invoke_call(context, function->xi, predecessor->xi);
        const XiCoroSuspendPoint *suspend_point =
            edge_point ? NULL : coroutine_point_for_block(function, predecessor->xi);
        if (!edge_point && suspend_point)
            edge_point = suspend_point->op;
        if (incoming &&
            edge_value_operand_key(context, function, predecessor, incoming, edge_point, &key) &&
            xr_core_ir_key_equal(key, owner))
            ++occurrences;
    }
    return occurrences;
}

static XrProgramBuildStatus
close_logical_owner_lifetimes(const XrXiBuildContext *context, const XrXiFunctionStorage *function,
                              XrXiBlockStorage *block, uint32_t instruction_count,
                              uint32_t instruction_capacity, uint32_t *closed_instruction_count,
                              char *diagnostic, size_t diagnostic_size) {
    if (!context || !function || !block || instruction_count == 0u || !closed_instruction_count)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint32_t owner_capacity = block->argument_count + instruction_count;
    XrCoreIrKey *owners = owner_capacity ? xr_calloc(owner_capacity, sizeof(*owners)) : NULL;
    if (owner_capacity && !owners)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t owner_count = 0u;
    for (uint32_t argument = 0u; argument < block->argument_count; ++argument) {
        if (block->argument_storage[argument].ownership == XR_CORE_IR_OWNER)
            owners[owner_count++] = block->argument_storage[argument].key;
    }
    for (uint32_t index = 0u; index < instruction_count; ++index) {
        const XrCoreIrInstructionInput *instruction = &block->instructions[index];
        if (instruction->result_type_id != XR_CORE_TYPE_VOID &&
            instruction->result_ownership == XR_CORE_IR_OWNER) {
            bool duplicate = false;
            for (uint32_t owner = 0u; owner < owner_count; ++owner)
                duplicate = duplicate || xr_core_ir_key_equal(owners[owner], instruction->result);
            if (!duplicate)
                owners[owner_count++] = instruction->result;
        }
    }

    bool *drops = owner_count ? xr_calloc(owner_count, sizeof(*drops)) : NULL;
    if (owner_count && !drops) {
        xr_free(owners);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    uint32_t drop_count = 0u;
    uint32_t successor_count = canonical_block_successor_count(context, function->xi, block->xi);
    for (uint32_t owner = 0u; owner < owner_count; ++owner) {
        uint32_t consuming_uses = 0u;
        for (uint32_t index = 0u; index < instruction_count; ++index) {
            const XrCoreIrInstructionInput *instruction = &block->instructions[index];
            for (uint32_t operand = 0u; operand < instruction->operand_count; ++operand) {
                if (xr_core_ir_key_equal(instruction->operands[operand], owners[owner]) &&
                    input_operation_consumes_operand(context, block, instruction_count, instruction,
                                                     operand))
                    ++consuming_uses;
            }
        }
        bool transferred = successor_count != 0u;
        bool absent = successor_count != 0u;
        for (uint32_t successor = 0u; successor < successor_count; ++successor) {
            const XiBlock *successor_block =
                canonical_block_successor(context, function->xi, block->xi, successor);
            uint32_t predecessor_occurrence = 0u;
            for (uint32_t prior = 0u; prior < successor; ++prior)
                predecessor_occurrence +=
                    canonical_block_successor(context, function->xi, block->xi, prior) ==
                    successor_block;
            uint32_t occurrences = owner_edge_occurrences(context, function, block, owners[owner],
                                                          successor_block, predecessor_occurrence);
            transferred = transferred && occurrences == 1u;
            absent = absent && occurrences == 0u;
        }
        if (consuming_uses > 1u || (consuming_uses != 0u && transferred) ||
            (consuming_uses == 0u && successor_count != 0u && !transferred && !absent)) {
            xr_free(drops);
            xr_free(owners);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi function %s block b%u has an unbalanced affine owner "
                        "(owner=%u uses=%u successors=%u transferred=%u absent=%u)",
                        function->xi && function->xi->name ? function->xi->name : "<anonymous>",
                        block->xi->id, owner, consuming_uses, successor_count,
                        transferred ? 1u : 0u, absent ? 1u : 0u);
        }
        if (consuming_uses == 0u && !transferred)
            drops[owner] = true, ++drop_count;
    }
    if (instruction_count + drop_count > instruction_capacity) {
        xr_free(drops);
        xr_free(owners);
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    }
    XrCoreIrKey **drop_operands = drop_count ? xr_calloc(drop_count, sizeof(*drop_operands)) : NULL;
    if (drop_count && !drop_operands) {
        xr_free(drops);
        xr_free(owners);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    uint32_t prepared_drop = 0u;
    for (uint32_t owner = 0u; owner < owner_count; ++owner) {
        if (!drops[owner])
            continue;
        drop_operands[prepared_drop] = xr_calloc(1u, sizeof(**drop_operands));
        if (!drop_operands[prepared_drop]) {
            for (uint32_t prior = 0u; prior < prepared_drop; ++prior)
                xr_free(drop_operands[prior]);
            xr_free(drop_operands);
            xr_free(drops);
            xr_free(owners);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        *drop_operands[prepared_drop++] = owners[owner];
    }

    /* All allocations complete before the terminator is moved.  A failed
     * allocation therefore leaves the original instruction array owned by
     * the context and fully recoverable by free_context(). */
    XrCoreIrInstructionInput terminator = block->instructions[instruction_count - 1u];
    uint32_t cursor = instruction_count - 1u;
    uint32_t drop_index = 0u;
    for (uint32_t owner = 0u; owner < owner_count; ++owner) {
        if (!drops[owner])
            continue;
        XrCoreIrInstructionInput *instruction = &block->instructions[cursor++];
        memset(instruction, 0, sizeof(*instruction));
        instruction->operation_id = XR_CORE_OP_CORE_OWNER_DROP;
        instruction->result_type_id = XR_CORE_TYPE_VOID;
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        instruction->operands = drop_operands[drop_index++];
        instruction->operand_count = 1u;
    }
    block->instructions[cursor++] = terminator;
    *closed_instruction_count = cursor;
    xr_free(drop_operands);
    xr_free(drops);
    xr_free(owners);
    return XR_PROGRAM_BUILD_OK;
}

typedef struct XrXiCleanupProjectionValueInfo {
    uint16_t type_id;
    XrCoreIrValueCategory category;
    XrCoreIrOwnershipDisposition ownership;
    uint32_t instruction;
    bool argument;
} XrXiCleanupProjectionValueInfo;

static bool cleanup_trap_capable_operation(uint16_t operation_id) {
    const XrCoreOperationSpec *operation = xr_core_spec_operation_by_id(operation_id);
    return operation && (operation->successor_mask & XR_CORE_SUCCESSOR_TRAP) != 0u;
}

static bool cleanup_call_has_exact_active_handler_edge(const XrXiBuildContext *context,
                                                       const XrXiFunctionStorage *function,
                                                       const XiValue *call,
                                                       const XrCoreIrInstructionInput *instruction,
                                                       uint32_t handler_state) {
    if (!context || !function || !call || !instruction || handler_state == 0u ||
        handler_state > function->cleanup_registration_count ||
        instruction->successor_count != 1u || !instruction->successors)
        return false;
    const XrXiTrapEdge *edge = find_trap_edge(context, function->xi, call);
    const XiValue *active = function->cleanup_handler_chain_nodes[handler_state - 1u].registration;
    return edge && edge->registration == active && edge->handler &&
           xr_core_ir_key_equal(instruction->successors[0], block_key(function, edge->handler));
}

static XrCoreIrBlockInput *find_input_block(XrXiFunctionStorage *function, uint32_t block_count,
                                            XrCoreIrKey key) {
    for (uint32_t block = 0u; function && block < block_count; ++block)
        if (xr_core_ir_key_equal(function->blocks[block].key, key))
            return &function->blocks[block];
    return NULL;
}

static uint32_t cleanup_projection_block_body_begin(const XrCoreIrBlockInput *block) {
    return block && block->instruction_count != 0u &&
                   block->instructions[0].operation_id == XR_CORE_OP_CORE_BLOCK_ARGUMENT
               ? 1u
               : 0u;
}

static bool cleanup_projection_slice_limits(XrXiFunctionStorage *function,
                                            const XrXiCleanupProjectionStorage *projection,
                                            uint32_t original_block_count,
                                            XrXiBlockStorage **source_storage_out,
                                            XrCoreIrBlockInput **source_out, uint32_t *begin_limit,
                                            uint32_t *end_limit) {
    XrXiCleanupPointStorage *point = projection ? projection->point : NULL;
    const XiBlock *source_block = projection ? projection->source_block : NULL;
    if (!point || !source_block)
        return false;
    XrXiBlockStorage *source_storage = find_block_storage(function, source_block);
    XrCoreIrBlockInput *source =
        find_input_block(function, original_block_count, block_key(function, source_block));
    if (!source_storage || !source || source->instruction_count == 0u)
        return false;
    uint32_t begin = cleanup_projection_block_body_begin(source);
    uint32_t end = source->instruction_count - 1u;
    if (source_block == point->enter_block) {
        if (!point->enter_emitted || point->enter_gap < begin || point->enter_gap > end)
            return false;
        begin = point->enter_gap;
    }
    if (source_block == point->leave_block) {
        if (!point->leave_emitted || point->leave_gap < begin || point->leave_gap > end)
            return false;
        end = point->leave_gap;
    }
    if (begin > end)
        return false;
    if (source_storage_out)
        *source_storage_out = source_storage;
    if (source_out)
        *source_out = source;
    if (begin_limit)
        *begin_limit = begin;
    if (end_limit)
        *end_limit = end;
    return true;
}

static XrXiCleanupProjectionStorage *find_cleanup_projection(XrXiFunctionStorage *function,
                                                             const XrXiCleanupPointStorage *point,
                                                             const XiBlock *source_block,
                                                             uint32_t begin_gap) {
    for (uint32_t index = 0u; function && index < function->cleanup_projection_count; ++index) {
        XrXiCleanupProjectionStorage *projection = &function->cleanup_projections[index];
        if (projection->point == point && projection->source_block == source_block &&
            projection->begin_gap == begin_gap)
            return projection;
    }
    return NULL;
}

static XrProgramBuildStatus
ensure_cleanup_projection(XrXiFunctionStorage *function, XrXiCleanupPointStorage *point,
                          const XiBlock *source_block, uint32_t begin_gap,
                          XrXiCleanupProjectionStorage **projection_out) {
    if (projection_out)
        *projection_out = NULL;
    if (!function || !point || !source_block || !projection_out)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    XrXiCleanupProjectionStorage *existing =
        find_cleanup_projection(function, point, source_block, begin_gap);
    if (existing) {
        *projection_out = existing;
        return XR_PROGRAM_BUILD_OK;
    }
    if (function->cleanup_projection_count == function->cleanup_projection_capacity) {
        uint32_t capacity =
            function->cleanup_projection_capacity ? function->cleanup_projection_capacity * 2u : 8u;
        if (capacity < function->cleanup_projection_capacity ||
            capacity > UINT32_MAX / sizeof(*function->cleanup_projections))
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        XrXiCleanupProjectionStorage *projections =
            xr_realloc(function->cleanup_projections, (size_t) capacity * sizeof(*projections));
        if (!projections)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        memset(projections + function->cleanup_projection_capacity, 0,
               (size_t) (capacity - function->cleanup_projection_capacity) * sizeof(*projections));
        function->cleanup_projections = projections;
        function->cleanup_projection_capacity = capacity;
    }
    XrXiCleanupProjectionStorage *projection =
        &function->cleanup_projections[function->cleanup_projection_count++];
    projection->point = point;
    projection->source_block = source_block;
    projection->begin_gap = begin_gap;
    projection->end_gap = begin_gap;
    *projection_out = projection;
    return XR_PROGRAM_BUILD_OK;
}

static const XiBlock *cleanup_projection_xi_block_for_key(const XrXiFunctionStorage *function,
                                                          XrCoreIrKey key) {
    for (uint32_t block = 0u; function && function->xi && block < function->xi->nblocks; ++block)
        if (xr_core_ir_key_equal(block_key(function, function->xi->blocks[block]), key))
            return function->xi->blocks[block];
    return NULL;
}

static bool cleanup_projection_block_is_in_boundary(const XrXiFunctionStorage *function,
                                                    const XiBlock *block,
                                                    const XiCleanupBoundary *boundary) {
    if (!function || !block || !boundary)
        return false;
    for (uint32_t value = 0u; value < block->nvalues; ++value) {
        const XiCleanupBoundary *active = NULL;
        if (!block->values[value] ||
            !active_cleanup_boundary_at_program_point(function, block->values[value], &active))
            return false;
        if (active == boundary)
            return true;
    }
    return false;
}

static XrProgramBuildStatus mark_cleanup_trap_projection_graph(
    XrXiFunctionStorage *function, XrXiCleanupPointStorage *point, const XiBlock *source_block,
    uint32_t begin_gap, uint32_t original_block_count, char *diagnostic, size_t diagnostic_size) {
    XrXiCleanupProjectionStorage *projection = NULL;
    XrProgramBuildStatus status =
        ensure_cleanup_projection(function, point, source_block, begin_gap, &projection);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    if (projection->flow_state == 2u || projection->flow_state == 1u)
        return XR_PROGRAM_BUILD_OK;
    projection->flow_state = 1u;

    XrCoreIrBlockInput *source = NULL;
    uint32_t begin_limit = 0u;
    uint32_t end_limit = 0u;
    if (!cleanup_projection_slice_limits(function, projection, original_block_count, NULL, &source,
                                         &begin_limit, &end_limit) ||
        begin_gap < begin_limit || begin_gap > end_limit)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "cleanup projection b%u:%u is outside its closed boundary",
                    source_block ? source_block->id : UINT32_MAX, begin_gap);

    projection->end_gap = end_limit;
    projection->ends_in_trap = false;
    for (uint32_t instruction = begin_gap; instruction < end_limit; ++instruction) {
        if (!cleanup_trap_capable_operation(source->instructions[instruction].operation_id))
            continue;
        projection->end_gap = instruction + 1u;
        projection->ends_in_trap = true;
        break;
    }

    if (projection->ends_in_trap) {
        status =
            mark_cleanup_trap_projection_graph(function, point, source_block, projection->end_gap,
                                               original_block_count, diagnostic, diagnostic_size);
    } else if (source_block == point->leave_block && projection->end_gap == point->leave_gap) {
        const XiValue *remaining = point->boundary ? point->boundary->remaining : NULL;
        if (remaining) {
            XrXiCleanupPointStorage *next =
                find_cleanup_point(function, remaining->cleanup_boundary);
            if (!next || next->boundary->enter != remaining)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "cleanup boundary has no exact remaining program point");
            status = mark_cleanup_trap_projection_graph(function, next, next->enter_block,
                                                        next->enter_gap, original_block_count,
                                                        diagnostic, diagnostic_size);
        }
    } else {
        const XrCoreIrInstructionInput *terminal =
            &source->instructions[source->instruction_count - 1u];
        if ((terminal->operation_id != XR_CORE_OP_CORE_BRANCH &&
             terminal->operation_id != XR_CORE_OP_CORE_CONDITIONAL_BRANCH) ||
            terminal->successor_count == 0u || terminal->successor_count > 2u)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "cleanup projection b%u exits its boundary before leave", source_block->id);
        for (uint32_t successor = 0u; successor < terminal->successor_count; ++successor) {
            const XiBlock *target =
                cleanup_projection_xi_block_for_key(function, terminal->successors[successor]);
            if (!target ||
                !cleanup_projection_block_is_in_boundary(function, target, point->boundary))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "cleanup projection b%u has an out-of-boundary successor",
                            source_block->id);
            XrXiCleanupProjectionStorage target_identity = {
                .point = point,
                .source_block = target,
            };
            XrCoreIrBlockInput *target_source = NULL;
            uint32_t target_begin = 0u;
            if (!cleanup_projection_slice_limits(function, &target_identity, original_block_count,
                                                 NULL, &target_source, &target_begin, NULL))
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            status = mark_cleanup_trap_projection_graph(function, point, target, target_begin,
                                                        original_block_count, diagnostic,
                                                        diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                break;
        }
    }
    projection = find_cleanup_projection(function, point, source_block, begin_gap);
    if (!projection)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    projection->flow_state = status == XR_PROGRAM_BUILD_OK ? 2u : 0u;
    return status;
}

static bool cleanup_projection_value_info(const XrCoreIrBlockInput *block, XrCoreIrKey key,
                                          XrXiCleanupProjectionValueInfo *info) {
    if (info)
        memset(info, 0, sizeof(*info));
    if (!block || !info)
        return false;
    for (uint32_t argument = 0u; argument < block->argument_count; ++argument) {
        if (!xr_core_ir_key_equal(block->arguments[argument].key, key))
            continue;
        *info = (XrXiCleanupProjectionValueInfo) {
            .type_id = block->arguments[argument].type_id,
            .category = block->arguments[argument].category,
            .ownership = block->arguments[argument].ownership,
            .instruction = UINT32_MAX,
            .argument = true,
        };
        return true;
    }
    for (uint32_t instruction = 0u; instruction < block->instruction_count; ++instruction) {
        const XrCoreIrInstructionInput *candidate = &block->instructions[instruction];
        if (candidate->result_type_id == XR_CORE_TYPE_VOID ||
            !xr_core_ir_key_equal(candidate->result, key))
            continue;
        *info = (XrXiCleanupProjectionValueInfo) {
            .type_id = candidate->result_type_id,
            .category = candidate->result_category,
            .ownership = candidate->result_ownership,
            .instruction = instruction,
            .argument = false,
        };
        return true;
    }
    return false;
}

static bool cleanup_projection_key_is_internal(const XrCoreIrBlockInput *block, XrCoreIrKey key,
                                               uint32_t begin, uint32_t end) {
    XrXiCleanupProjectionValueInfo info;
    return cleanup_projection_value_info(block, key, &info) && !info.argument &&
           info.instruction >= begin && info.instruction < end;
}

static bool cleanup_projection_place_root(const XrCoreIrBlockInput *block, XrCoreIrKey place,
                                          uint32_t before, XrCoreIrKey *root) {
    for (uint32_t depth = 0u; block && depth < block->instruction_count; ++depth) {
        XrXiCleanupProjectionValueInfo info;
        if (!cleanup_projection_value_info(block, place, &info) || info.argument ||
            info.instruction >= before || info.category != XR_CORE_IR_PLACE)
            return false;
        const XrCoreIrInstructionInput *definition = &block->instructions[info.instruction];
        if (definition->operand_count != 1u || definition->successor_count != 0u)
            return false;
        if (definition->operation_id == XR_CORE_OP_CORE_PLACE_LOCAL) {
            *root = definition->operands[0];
            return true;
        }
        if (definition->operation_id != XR_CORE_OP_CORE_PLACE_PROJECT)
            return false;
        place = definition->operands[0];
    }
    return false;
}

static bool cleanup_projection_affine_owner(const XrXiBuildContext *context,
                                            const XrCoreIrBlockInput *block, XrCoreIrKey value,
                                            uint32_t before, XrCoreIrKey *owner) {
    if (owner)
        memset(owner, 0, sizeof(*owner));
    for (uint32_t depth = 0u; context && block && owner && depth <= block->instruction_count;
         ++depth) {
        XrXiCleanupProjectionValueInfo info;
        if (!cleanup_projection_value_info(block, value, &info) ||
            (!info.argument && info.instruction >= before))
            return false;
        if (info.ownership == XR_CORE_IR_OWNER) {
            *owner = value;
            return true;
        }
        if (info.category == XR_CORE_IR_PLACE)
            return cleanup_projection_place_root(block, value, before, owner);
        if (logical_ownership_for_type(context, info.type_id) != XR_CORE_IR_OWNER || info.argument)
            return false;
        const XrCoreIrInstructionInput *definition = &block->instructions[info.instruction];
        if (definition->operand_count != 1u)
            return false;
        if (definition->operation_id == XR_CORE_OP_CORE_PLACE_LOAD) {
            return cleanup_projection_place_root(block, definition->operands[0], info.instruction,
                                                 owner);
        }
        if (definition->operation_id != XR_CORE_OP_CORE_AGGREGATE_PROJECT &&
            definition->operation_id != XR_CORE_OP_CORE_VARIANT_PROJECT &&
            definition->operation_id != XR_CORE_OP_CORE_EXISTENTIAL_PROJECT)
            return false;
        value = definition->operands[0];
    }
    return false;
}

static bool cleanup_projection_operand_consumes_owner(const XrXiBuildContext *context,
                                                      const XrXiBlockStorage *source_storage,
                                                      const XrCoreIrBlockInput *source,
                                                      const XrCoreIrInstructionInput *instruction,
                                                      uint32_t instruction_index,
                                                      uint32_t operand_index, XrCoreIrKey owner) {
    if (!input_operation_consumes_operand(context, source_storage, source->instruction_count,
                                          instruction, operand_index))
        return false;
    XrCoreIrKey consumed = {{0}};
    return cleanup_projection_affine_owner(context, source, instruction->operands[operand_index],
                                           instruction_index, &consumed) &&
           xr_core_ir_key_equal(consumed, owner);
}

static XrProgramBuildStatus
cleanup_projection_add_argument(XrXiFunctionStorage *function,
                                XrXiCleanupProjectionStorage *projection,
                                const XrCoreIrBlockInput *source, XrCoreIrKey source_key,
                                char *diagnostic, size_t diagnostic_size) {
    for (uint32_t argument = 0u; argument < projection->trap_argument_count; ++argument)
        if (xr_core_ir_key_equal(projection->trap_argument_sources[argument], source_key))
            return XR_PROGRAM_BUILD_OK;
    XrXiCleanupProjectionValueInfo info;
    if (!cleanup_projection_value_info(source, source_key, &info) ||
        info.category == XR_CORE_IR_PLACE)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "cleanup trap projection requires an unavailable non-place input");
    if (projection->trap_argument_count == UINT32_MAX)
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    uint32_t count = projection->trap_argument_count + 1u;
    if ((size_t) count > SIZE_MAX / sizeof(*projection->trap_argument_sources) ||
        (size_t) count > SIZE_MAX / sizeof(*projection->trap_arguments))
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    XrCoreIrKey *sources =
        xr_realloc(projection->trap_argument_sources, (size_t) count * sizeof(*sources));
    if (!sources)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    projection->trap_argument_sources = sources;
    XrCoreIrValueInput *arguments =
        xr_realloc(projection->trap_arguments, (size_t) count * sizeof(*arguments));
    if (!arguments)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    projection->trap_arguments = arguments;
    projection->trap_argument_sources[count - 1u] = source_key;
    projection->trap_arguments[count - 1u] = (XrCoreIrValueInput) {
        .key = cleanup_trap_argument_key(function, projection, count - 1u),
        .type_id = info.type_id,
        .category = info.category,
        .ownership = info.ownership,
    };
    projection->trap_argument_count = count;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus prepare_cleanup_trap_projection_arguments(
    const XrXiBuildContext *context, XrXiFunctionStorage *function,
    XrXiCleanupProjectionStorage *projection, uint32_t original_block_count, char *diagnostic,
    size_t diagnostic_size) {
    XrXiCleanupPointStorage *point = projection ? projection->point : NULL;
    if (!point)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    XrXiBlockStorage *source_storage = NULL;
    XrCoreIrBlockInput *source = NULL;
    uint32_t begin_limit = 0u;
    uint32_t end_limit = 0u;
    if (!cleanup_projection_slice_limits(function, projection, original_block_count,
                                         &source_storage, &source, &begin_limit, &end_limit) ||
        projection->flow_state != 2u || projection->begin_gap < begin_limit ||
        projection->begin_gap > projection->end_gap || projection->end_gap > end_limit)
        return XR_PROGRAM_BUILD_INVALID_INPUT;

    XrProgramBuildStatus status = XR_PROGRAM_BUILD_OK;
    if (projection->begin_gap == begin_limit) {
        for (uint32_t argument = 0u; argument < source->argument_count; ++argument) {
            status = cleanup_projection_add_argument(function, projection, source,
                                                     source->arguments[argument].key, diagnostic,
                                                     diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }

    uint32_t owner_definition_limit = projection->begin_gap;
    if (owner_definition_limit > begin_limit &&
        cleanup_trap_capable_operation(
            source->instructions[owner_definition_limit - 1u].operation_id))
        --owner_definition_limit;
    uint64_t owner_capacity_wide = (uint64_t) source->argument_count + owner_definition_limit;
    if (owner_capacity_wide > UINT32_MAX || owner_capacity_wide > SIZE_MAX / sizeof(XrCoreIrKey))
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    uint32_t owner_capacity = (uint32_t) owner_capacity_wide;
    XrCoreIrKey *owners = owner_capacity ? xr_calloc(owner_capacity, sizeof(*owners)) : NULL;
    if (owner_capacity && !owners)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t owner_count = 0u;
    for (uint32_t argument = 0u; argument < source->argument_count; ++argument)
        if (source->arguments[argument].ownership == XR_CORE_IR_OWNER)
            owners[owner_count++] = source->arguments[argument].key;
    for (uint32_t instruction = 0u; instruction < owner_definition_limit; ++instruction) {
        const XrCoreIrInstructionInput *candidate = &source->instructions[instruction];
        if (candidate->result_type_id == XR_CORE_TYPE_VOID ||
            candidate->result_ownership != XR_CORE_IR_OWNER)
            continue;
        bool duplicate = false;
        for (uint32_t owner = 0u; owner < owner_count; ++owner)
            duplicate |= xr_core_ir_key_equal(owners[owner], candidate->result);
        if (!duplicate)
            owners[owner_count++] = candidate->result;
    }
    for (uint32_t owner = 0u; owner < owner_count && status == XR_PROGRAM_BUILD_OK; ++owner) {
        uint32_t consumes = 0u;
        for (uint32_t instruction = 0u; instruction < projection->begin_gap; ++instruction) {
            const XrCoreIrInstructionInput *candidate = &source->instructions[instruction];
            for (uint32_t operand = 0u; operand < candidate->operand_count; ++operand)
                if (cleanup_projection_operand_consumes_owner(context, source_storage, source,
                                                              candidate, instruction, operand,
                                                              owners[owner]))
                    ++consumes;
        }
        if (consumes > 1u) {
            status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                          "cleanup projection owner is consumed more than once before its body");
        } else if (consumes == 0u) {
            status = cleanup_projection_add_argument(function, projection, source, owners[owner],
                                                     diagnostic, diagnostic_size);
        }
    }
    xr_free(owners);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;

    for (uint32_t instruction = projection->begin_gap; instruction < projection->end_gap;
         ++instruction) {
        const XrCoreIrInstructionInput *candidate = &source->instructions[instruction];
        if (candidate->successor_count != 0u)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "cleanup trap projection body already owns a control-flow edge");
        for (uint32_t operand = 0u; operand < candidate->operand_count; ++operand) {
            XrCoreIrKey dependency = candidate->operands[operand];
            if (cleanup_projection_key_is_internal(source, dependency, projection->begin_gap,
                                                   instruction))
                continue;
            XrXiCleanupProjectionValueInfo info;
            if (!cleanup_projection_value_info(source, dependency, &info))
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            if (info.category == XR_CORE_IR_PLACE) {
                XrCoreIrKey root = {{0}};
                if (!cleanup_projection_place_root(source, dependency, projection->begin_gap,
                                                   &root))
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "cleanup trap projection cannot reconstruct a borrowed place");
                dependency = root;
            }
            status = cleanup_projection_add_argument(function, projection, source, dependency,
                                                     diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    bool reaches_block_terminal = !projection->ends_in_trap && projection->end_gap == end_limit &&
                                  projection->source_block != point->leave_block;
    if (reaches_block_terminal) {
        const XrCoreIrInstructionInput *terminal =
            &source->instructions[source->instruction_count - 1u];
        for (uint32_t operand = 0u; operand < terminal->operand_count; ++operand) {
            XrCoreIrKey dependency = terminal->operands[operand];
            if (cleanup_projection_key_is_internal(source, dependency, projection->begin_gap,
                                                   projection->end_gap))
                continue;
            XrXiCleanupProjectionValueInfo info;
            if (!cleanup_projection_value_info(source, dependency, &info))
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            if (info.category == XR_CORE_IR_PLACE) {
                XrCoreIrKey root = {{0}};
                if (!cleanup_projection_place_root(source, dependency, projection->begin_gap,
                                                   &root))
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "cleanup branch cannot reconstruct a borrowed place");
                dependency = root;
            }
            status = cleanup_projection_add_argument(function, projection, source, dependency,
                                                     diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrXiCleanupProjectionStorage *
cleanup_projection_successor(XrXiFunctionStorage *function,
                             const XrXiCleanupProjectionStorage *projection,
                             bool *successor_expected) {
    if (successor_expected)
        *successor_expected = false;
    if (!function || !projection || !projection->point)
        return NULL;
    if (projection->ends_in_trap) {
        if (successor_expected)
            *successor_expected = true;
        return find_cleanup_projection(function, projection->point, projection->source_block,
                                       projection->end_gap);
    }
    if (projection->source_block != projection->point->leave_block)
        return NULL;
    const XiCleanupBoundary *boundary = projection->point->boundary;
    if (!boundary || !boundary->remaining)
        return NULL;
    if (successor_expected)
        *successor_expected = true;
    XrXiCleanupPointStorage *next_point =
        find_cleanup_point(function, boundary->remaining->cleanup_boundary);
    return next_point ? find_cleanup_projection(function, next_point, next_point->enter_block,
                                                next_point->enter_gap)
                      : NULL;
}

static XrProgramBuildStatus close_cleanup_projection_arguments(
    XrXiFunctionStorage *function, XrXiCleanupProjectionStorage *projection,
    uint32_t original_block_count, uint32_t depth, char *diagnostic, size_t diagnostic_size) {
    if (!function || !projection || !projection->point ||
        depth > function->cleanup_projection_count)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    if (projection->argument_state == 2u)
        return XR_PROGRAM_BUILD_OK;
    if (projection->argument_state != 0u)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "cleanup projection continuation graph contains a cycle");
    projection->argument_state = 1u;
    bool successor_expected = false;
    XrXiCleanupProjectionStorage *next =
        cleanup_projection_successor(function, projection, &successor_expected);
    if (successor_expected && !next)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "cleanup projection has no exact next program point");
    if (next) {
        XrProgramBuildStatus status = close_cleanup_projection_arguments(
            function, next, original_block_count, depth + 1u, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
        XrCoreIrBlockInput *source = find_input_block(
            function, original_block_count, block_key(function, projection->source_block));
        if (!source)
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        for (uint32_t argument = 0u; argument < next->trap_argument_count; ++argument) {
            XrCoreIrKey dependency = next->trap_argument_sources[argument];
            if (cleanup_projection_key_is_internal(source, dependency, projection->begin_gap,
                                                   projection->end_gap))
                continue;
            status = cleanup_projection_add_argument(function, projection, source, dependency,
                                                     diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }
    projection->argument_state = 2u;
    return XR_PROGRAM_BUILD_OK;
}

static bool cleanup_projection_remap_key(const XrCoreIrKey *source_keys,
                                         const XrCoreIrKey *target_keys, uint32_t key_count,
                                         XrCoreIrKey source, XrCoreIrKey *target) {
    for (uint32_t index = 0u; index < key_count; ++index) {
        if (!xr_core_ir_key_equal(source_keys[index], source))
            continue;
        *target = target_keys[index];
        return true;
    }
    return false;
}

static XrProgramBuildStatus append_cleanup_trap_successor(
    XrCoreIrInstructionInput *instruction, const XrXiFunctionStorage *function,
    const XrXiCleanupProjectionStorage *target, const XrCoreIrKey *source_keys,
    const XrCoreIrKey *target_keys, uint32_t key_count, char *diagnostic, size_t diagnostic_size) {
    if (!instruction || !target || instruction->successor_count != 0u ||
        !cleanup_trap_capable_operation(instruction->operation_id) ||
        target->trap_argument_count > UINT32_MAX - instruction->operand_count)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint32_t operand_count = instruction->operand_count + target->trap_argument_count;
    XrCoreIrKey *operands = operand_count ? xr_calloc(operand_count, sizeof(*operands)) : NULL;
    XrCoreIrKey *successors = xr_calloc(1u, sizeof(*successors));
    if ((operand_count && !operands) || !successors) {
        xr_free(operands);
        xr_free(successors);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    if (instruction->operand_count)
        memcpy(operands, instruction->operands,
               (size_t) instruction->operand_count * sizeof(*operands));
    for (uint32_t argument = 0u; argument < target->trap_argument_count; ++argument) {
        XrCoreIrKey mapped = target->trap_argument_sources[argument];
        if (source_keys &&
            !cleanup_projection_remap_key(source_keys, target_keys, key_count, mapped, &mapped)) {
            xr_free(successors);
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "cleanup trap edge cannot supply remaining argument %u", argument);
        }
        if (!xr_core_ir_key_is_zero(instruction->result) &&
            xr_core_ir_key_equal(mapped, instruction->result)) {
            xr_free(successors);
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "cleanup failure suffix depends on the failed operation result");
        }
        operands[instruction->operand_count + argument] = mapped;
    }
    *successors = cleanup_trap_block_key(function, target);
    xr_free((void *) instruction->operands);
    instruction->operands = operands;
    instruction->operand_count = operand_count;
    instruction->successors = successors;
    instruction->successor_count = 1u;
    return XR_PROGRAM_BUILD_OK;
}

static bool cleanup_projection_key_array_contains(const XrCoreIrKey *keys, uint32_t count,
                                                  XrCoreIrKey key) {
    for (uint32_t index = 0u; index < count; ++index)
        if (xr_core_ir_key_equal(keys[index], key))
            return true;
    return false;
}

static XrProgramBuildStatus
materialize_cleanup_trap_projection(const XrXiBuildContext *context, XrXiFunctionStorage *function,
                                    XrXiCleanupProjectionStorage *projection,
                                    uint32_t original_block_count, XrCoreIrBlockInput *output,
                                    char *diagnostic, size_t diagnostic_size) {
    XrXiCleanupPointStorage *point = projection ? projection->point : NULL;
    if (!point)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    XrXiBlockStorage *source_storage = NULL;
    XrCoreIrBlockInput *source = NULL;
    uint32_t begin_limit = 0u;
    uint32_t end_limit = 0u;
    bool exact_slice =
        cleanup_projection_slice_limits(function, projection, original_block_count, &source_storage,
                                        &source, &begin_limit, &end_limit);
    if (!source_storage || !source || !output || projection->trap_instructions || !exact_slice ||
        projection->begin_gap < begin_limit || projection->begin_gap > projection->end_gap ||
        projection->end_gap > end_limit || projection->argument_state != 2u)
        return XR_PROGRAM_BUILD_INVALID_INPUT;

    uint32_t body_count = projection->end_gap - projection->begin_gap;
    XrCoreIrKey *places =
        projection->begin_gap ? xr_calloc(projection->begin_gap, sizeof(*places)) : NULL;
    if (projection->begin_gap && !places)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t place_count = 0u;
    for (uint32_t instruction = projection->begin_gap; instruction < projection->end_gap;
         ++instruction) {
        const XrCoreIrInstructionInput *candidate = &source->instructions[instruction];
        for (uint32_t operand = 0u; operand < candidate->operand_count; ++operand) {
            XrCoreIrKey key = candidate->operands[operand];
            if (cleanup_projection_key_is_internal(source, key, projection->begin_gap, instruction))
                continue;
            XrXiCleanupProjectionValueInfo info;
            if (!cleanup_projection_value_info(source, key, &info)) {
                xr_free(places);
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            }
            if (info.category == XR_CORE_IR_PLACE &&
                !cleanup_projection_key_array_contains(places, place_count, key)) {
                if (place_count >= projection->begin_gap) {
                    xr_free(places);
                    return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
                }
                places[place_count++] = key;
            }
        }
    }
    for (uint32_t cursor = 0u; cursor < place_count; ++cursor) {
        XrXiCleanupProjectionValueInfo info;
        if (!cleanup_projection_value_info(source, places[cursor], &info) || info.argument ||
            info.instruction >= projection->begin_gap) {
            xr_free(places);
            return XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE;
        }
        const XrCoreIrInstructionInput *definition = &source->instructions[info.instruction];
        if (definition->operand_count != 1u || definition->successor_count != 0u ||
            (definition->operation_id != XR_CORE_OP_CORE_PLACE_LOCAL &&
             definition->operation_id != XR_CORE_OP_CORE_PLACE_PROJECT)) {
            xr_free(places);
            return XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE;
        }
        XrXiCleanupProjectionValueInfo dependency;
        if (!cleanup_projection_value_info(source, definition->operands[0], &dependency)) {
            xr_free(places);
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        }
        if (dependency.category == XR_CORE_IR_PLACE &&
            !cleanup_projection_key_array_contains(places, place_count, definition->operands[0])) {
            if (place_count >= projection->begin_gap) {
                xr_free(places);
                return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
            }
            places[place_count++] = definition->operands[0];
        }
    }

    uint64_t map_capacity_wide =
        (uint64_t) projection->trap_argument_count + place_count + body_count;
    uint64_t instruction_capacity_wide = (projection->trap_argument_count ? 1u : 0u) +
                                         (uint64_t) place_count + body_count +
                                         projection->trap_argument_count + body_count + 1u;
    if (map_capacity_wide > UINT32_MAX || instruction_capacity_wide > UINT32_MAX) {
        xr_free(places);
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    }
    uint32_t map_capacity = (uint32_t) map_capacity_wide;
    uint32_t instruction_capacity = (uint32_t) instruction_capacity_wide;
    XrCoreIrKey *source_keys = map_capacity ? xr_calloc(map_capacity, sizeof(*source_keys)) : NULL;
    XrCoreIrKey *target_keys = map_capacity ? xr_calloc(map_capacity, sizeof(*target_keys)) : NULL;
    projection->trap_instructions =
        instruction_capacity
            ? xr_calloc(instruction_capacity, sizeof(*projection->trap_instructions))
            : NULL;
    if ((map_capacity && (!source_keys || !target_keys)) ||
        (instruction_capacity && !projection->trap_instructions)) {
        xr_free(target_keys);
        xr_free(source_keys);
        xr_free(places);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    /* Publish initialized storage length immediately so free_context() can
     * reclaim every
     * operand allocated below if materialization fails. */
    projection->trap_instruction_count = instruction_capacity;
    uint32_t map_count = 0u;
    for (uint32_t argument = 0u; argument < projection->trap_argument_count; ++argument) {
        source_keys[map_count] = projection->trap_argument_sources[argument];
        target_keys[map_count++] = projection->trap_arguments[argument].key;
    }
    uint32_t target_instruction = 0u;
    if (projection->trap_argument_count) {
        XrCoreIrKey *operands = xr_calloc(projection->trap_argument_count, sizeof(*operands));
        if (!operands) {
            xr_free(target_keys);
            xr_free(source_keys);
            xr_free(places);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        for (uint32_t argument = 0u; argument < projection->trap_argument_count; ++argument)
            operands[argument] = projection->trap_arguments[argument].key;
        projection->trap_instructions[target_instruction++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = operands,
            .operand_count = projection->trap_argument_count,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    }

    for (uint32_t instruction = 0u; instruction < projection->begin_gap; ++instruction) {
        const XrCoreIrInstructionInput *from = &source->instructions[instruction];
        if (xr_core_ir_key_is_zero(from->result) ||
            !cleanup_projection_key_array_contains(places, place_count, from->result))
            continue;
        XrCoreIrInstructionInput *to = &projection->trap_instructions[target_instruction];
        *to = *from;
        to->operands = NULL;
        to->successors = NULL;
        to->result = cleanup_trap_result_key(function, projection, target_instruction);
        source_keys[map_count] = from->result;
        target_keys[map_count++] = to->result;
        if (from->operand_count != 1u) {
            xr_free(target_keys);
            xr_free(source_keys);
            xr_free(places);
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        }
        XrCoreIrKey *operand = xr_calloc(1u, sizeof(*operand));
        if (!operand) {
            xr_free(target_keys);
            xr_free(source_keys);
            xr_free(places);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        if (!cleanup_projection_remap_key(source_keys, target_keys, map_count, from->operands[0],
                                          operand)) {
            xr_free(operand);
            xr_free(target_keys);
            xr_free(source_keys);
            xr_free(places);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "cleanup trap projection place dependency is unavailable");
        }
        to->operands = operand;
        ++target_instruction;
    }

    for (uint32_t instruction = projection->begin_gap; instruction < projection->end_gap;
         ++instruction) {
        const XrCoreIrInstructionInput *from = &source->instructions[instruction];
        XrCoreIrInstructionInput *to = &projection->trap_instructions[target_instruction];
        *to = *from;
        to->operands = NULL;
        to->successors = NULL;
        if (!xr_core_ir_key_is_zero(from->result)) {
            to->result = cleanup_trap_result_key(function, projection, target_instruction);
            source_keys[map_count] = from->result;
            target_keys[map_count++] = to->result;
        }
        if (from->operand_count) {
            XrCoreIrKey *operands = xr_calloc(from->operand_count, sizeof(*operands));
            if (!operands) {
                xr_free(target_keys);
                xr_free(source_keys);
                xr_free(places);
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            }
            for (uint32_t operand = 0u; operand < from->operand_count; ++operand) {
                if (!cleanup_projection_remap_key(source_keys, target_keys, map_count,
                                                  from->operands[operand], &operands[operand])) {
                    xr_free(operands);
                    xr_free(target_keys);
                    xr_free(source_keys);
                    xr_free(places);
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "cleanup trap projection operand is not closed over its slice");
                }
            }
            to->operands = operands;
        }
        ++target_instruction;
    }
    xr_free(places);

    bool successor_expected = false;
    XrXiCleanupProjectionStorage *next =
        cleanup_projection_successor(function, projection, &successor_expected);
    if (successor_expected && !next) {
        xr_free(target_keys);
        xr_free(source_keys);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "cleanup projection has no exact materialized successor");
    }
    for (uint32_t instruction = 0u; instruction < body_count; ++instruction) {
        uint32_t source_instruction = projection->begin_gap + instruction;
        XrCoreIrInstructionInput *candidate =
            &projection->trap_instructions[target_instruction - body_count + instruction];
        if (!cleanup_trap_capable_operation(candidate->operation_id))
            continue;
        if (!next || !projection->ends_in_trap || source_instruction + 1u != projection->end_gap) {
            xr_free(target_keys);
            xr_free(source_keys);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "cleanup trap instruction has no exact segment successor");
        }
        XrProgramBuildStatus status =
            append_cleanup_trap_successor(candidate, function, next, source_keys, target_keys,
                                          map_count, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK) {
            xr_free(target_keys);
            xr_free(source_keys);
            return status;
        }
    }

    XrCoreIrKey *terminal_operands = NULL;
    uint32_t terminal_operand_count = 0u;
    XrCoreIrKey *terminal_successors = NULL;
    uint32_t terminal_successor_count = 0u;
    uint16_t terminal_operation = XR_CORE_OP_CORE_TRAP;
    bool crosses_block =
        !projection->ends_in_trap && projection->source_block != point->leave_block;
    if (crosses_block) {
        const XrCoreIrInstructionInput *from =
            &source->instructions[source->instruction_count - 1u];
        if ((from->operation_id != XR_CORE_OP_CORE_BRANCH &&
             from->operation_id != XR_CORE_OP_CORE_CONDITIONAL_BRANCH) ||
            from->successor_count == 0u || from->successor_count > 2u)
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        terminal_operation = from->operation_id;
        terminal_operand_count = from->operand_count;
        terminal_successor_count = from->successor_count;
        terminal_operands = terminal_operand_count
                                ? xr_calloc(terminal_operand_count, sizeof(*terminal_operands))
                                : NULL;
        terminal_successors = xr_calloc(terminal_successor_count, sizeof(*terminal_successors));
        if ((terminal_operand_count && !terminal_operands) || !terminal_successors) {
            xr_free(terminal_successors);
            xr_free(terminal_operands);
            xr_free(target_keys);
            xr_free(source_keys);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        for (uint32_t operand = 0u; operand < terminal_operand_count; ++operand) {
            if (!cleanup_projection_remap_key(source_keys, target_keys, map_count,
                                              from->operands[operand],
                                              &terminal_operands[operand])) {
                xr_free(terminal_successors);
                xr_free(terminal_operands);
                xr_free(target_keys);
                xr_free(source_keys);
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "cleanup branch operand is not closed over its slice");
            }
        }
        for (uint32_t successor = 0u; successor < terminal_successor_count; ++successor) {
            const XiBlock *target =
                cleanup_projection_xi_block_for_key(function, from->successors[successor]);
            XrXiCleanupProjectionStorage target_identity = {
                .point = point,
                .source_block = target,
            };
            uint32_t target_begin = 0u;
            if (!target ||
                !cleanup_projection_slice_limits(function, &target_identity, original_block_count,
                                                 NULL, NULL, &target_begin, NULL)) {
                xr_free(terminal_successors);
                xr_free(terminal_operands);
                xr_free(target_keys);
                xr_free(source_keys);
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            }
            XrXiCleanupProjectionStorage *target_projection =
                find_cleanup_projection(function, point, target, target_begin);
            if (!target_projection) {
                xr_free(terminal_successors);
                xr_free(terminal_operands);
                xr_free(target_keys);
                xr_free(source_keys);
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            }
            terminal_successors[successor] = cleanup_trap_block_key(function, target_projection);
        }
    } else if (next) {
        terminal_operation = XR_CORE_OP_CORE_BRANCH;
        terminal_operand_count = next->trap_argument_count;
        terminal_operands = terminal_operand_count
                                ? xr_calloc(terminal_operand_count, sizeof(*terminal_operands))
                                : NULL;
        if (terminal_operand_count && !terminal_operands) {
            xr_free(target_keys);
            xr_free(source_keys);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        for (uint32_t argument = 0u; argument < terminal_operand_count; ++argument) {
            if (!cleanup_projection_remap_key(source_keys, target_keys, map_count,
                                              next->trap_argument_sources[argument],
                                              &terminal_operands[argument])) {
                xr_free(terminal_operands);
                xr_free(target_keys);
                xr_free(source_keys);
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "cleanup trap projection cannot continue to its remaining item");
            }
        }
        terminal_successor_count = 1u;
        terminal_successors = xr_calloc(1u, sizeof(*terminal_successors));
        if (!terminal_successors) {
            xr_free(terminal_operands);
            xr_free(target_keys);
            xr_free(source_keys);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        *terminal_successors = cleanup_trap_block_key(function, next);
    }

    for (uint32_t mapped = 0u; mapped < map_count; ++mapped) {
        XrXiCleanupProjectionValueInfo info;
        if (!cleanup_projection_value_info(source, source_keys[mapped], &info) ||
            info.ownership != XR_CORE_IR_OWNER)
            continue;
        uint32_t consumes = 0u;
        for (uint32_t instruction = projection->begin_gap; instruction < projection->end_gap;
             ++instruction) {
            const XrCoreIrInstructionInput *candidate = &source->instructions[instruction];
            for (uint32_t operand = 0u; operand < candidate->operand_count; ++operand)
                if (cleanup_projection_operand_consumes_owner(context, source_storage, source,
                                                              candidate, instruction, operand,
                                                              source_keys[mapped]))
                    ++consumes;
        }
        uint32_t transfers = 0u;
        for (uint32_t operand = 0u; operand < terminal_operand_count; ++operand)
            transfers += xr_core_ir_key_equal(terminal_operands[operand], target_keys[mapped]);
        if (crosses_block && terminal_operation == XR_CORE_OP_CORE_CONDITIONAL_BRANCH &&
            transfers != 0u)
            transfers = 1u;
        if (consumes > 1u || (consumes != 0u && transfers != 0u) || transfers > 1u) {
            xr_free(terminal_successors);
            xr_free(terminal_operands);
            xr_free(target_keys);
            xr_free(source_keys);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "cleanup trap projection has an unbalanced affine owner");
        }
        if (consumes == 0u && transfers == 0u) {
            XrCoreIrKey *drop = xr_calloc(1u, sizeof(*drop));
            if (!drop) {
                xr_free(terminal_successors);
                xr_free(terminal_operands);
                xr_free(target_keys);
                xr_free(source_keys);
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            }
            *drop = target_keys[mapped];
            projection->trap_instructions[target_instruction++] = (XrCoreIrInstructionInput) {
                .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
                .result_type_id = XR_CORE_TYPE_VOID,
                .operands = drop,
                .operand_count = 1u,
                .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
            };
        }
    }
    XrCoreIrInstructionInput *terminal = &projection->trap_instructions[target_instruction++];
    terminal->operation_id = terminal_operation;
    terminal->result_type_id = XR_CORE_TYPE_VOID;
    terminal->operands = terminal_operands;
    terminal->operand_count = terminal_operand_count;
    terminal->successors = terminal_successors;
    terminal->successor_count = terminal_successor_count;
    terminal->immediate_kind =
        terminal_operation == XR_CORE_OP_CORE_TRAP ? XR_CORE_IR_IMMEDIATE_U32
        : crosses_block ? source->instructions[source->instruction_count - 1u].immediate_kind
                        : XR_CORE_IR_IMMEDIATE_NONE;
    if (terminal_operation == XR_CORE_OP_CORE_TRAP)
        terminal->immediate.u32 = 7u;
    projection->trap_instruction_count = target_instruction;
    output->key = cleanup_trap_block_key(function, projection);
    output->arguments = projection->trap_arguments;
    output->argument_count = projection->trap_argument_count;
    output->instructions = projection->trap_instructions;
    output->instruction_count = projection->trap_instruction_count;
    xr_free(target_keys);
    xr_free(source_keys);
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
materialize_cleanup_trap_projections(const XrXiBuildContext *context, XrXiFunctionStorage *function,
                                     uint32_t original_block_count, uint32_t block_capacity,
                                     uint32_t *output_block_index, uint32_t *projection_count,
                                     char *diagnostic, size_t diagnostic_size) {
    if (!context || !function || !output_block_index || !projection_count ||
        *output_block_index != original_block_count || function->cleanup_projection_count != 0u ||
        function->cleanup_projections)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    *projection_count = 0u;
    for (uint32_t block_index = 0u; block_index < function->xi->nblocks; ++block_index) {
        XrXiBlockStorage *block = &function->block_storage[block_index];
        if (!block->reachable || !block->emission_ready)
            continue;
        for (uint32_t value_index = 0u; value_index < block->xi->nvalues; ++value_index) {
            const XiValue *value = block->xi->values[value_index];
            const XiCleanupBoundary *boundary = NULL;
            if (!value || !active_cleanup_boundary_at_program_point(function, value, &boundary))
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            if (!boundary)
                continue;
            if (value_index >= block->emission_span_count)
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            const XrXiEmissionSpan *span = &block->emission_spans[value_index];
            uint32_t trap_instruction_count = 0u;
            for (uint32_t instruction = span->begin; instruction < span->end; ++instruction) {
                XrCoreIrInstructionInput *candidate = &block->instructions[instruction];
                if (!cleanup_trap_capable_operation(candidate->operation_id))
                    continue;
                if (++trap_instruction_count != 1u)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "cleanup value v%u emits multiple trap-capable operations",
                                value->id);
                XrXiCleanupPointStorage *active = find_cleanup_point(function, boundary);
                XrXiCleanupProjectionStorage identity = {
                    .point = active,
                    .source_block = block->xi,
                };
                uint32_t begin_limit = 0u;
                uint32_t end_limit = 0u;
                if (!active || boundary->kind != XI_CLEANUP_BOUNDARY_CLOSED ||
                    !active->enter_emitted || !active->leave_emitted ||
                    !cleanup_projection_slice_limits(function, &identity, original_block_count,
                                                     NULL, NULL, &begin_limit, &end_limit) ||
                    instruction < begin_limit || instruction >= end_limit)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "cleanup call v%u is outside a closed Program graph", value->id);
                uint32_t handler_state = function->cleanup_handler_state_before_value[value->id];
                if (handler_state != 0u) {
                    if (!cleanup_call_has_exact_active_handler_edge(context, function, value,
                                                                    candidate, handler_state))
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "cleanup call v%u has a stale outer-handler edge", value->id);
                    continue;
                }
                if (candidate->successor_count != 0u)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "cleanup call v%u already owns a non-handler trap edge", value->id);
                XrProgramBuildStatus status = mark_cleanup_trap_projection_graph(
                    function, active, block->xi, instruction + 1u, original_block_count, diagnostic,
                    diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        }
    }
    *projection_count = function->cleanup_projection_count;
    if (*projection_count > block_capacity - *output_block_index)
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    for (uint32_t projection_index = 0u; projection_index < *projection_count; ++projection_index) {
        XrXiCleanupProjectionStorage *projection = &function->cleanup_projections[projection_index];
        XrProgramBuildStatus status = prepare_cleanup_trap_projection_arguments(
            context, function, projection, original_block_count, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    for (uint32_t projection_index = 0u; projection_index < *projection_count; ++projection_index) {
        XrXiCleanupProjectionStorage *projection = &function->cleanup_projections[projection_index];
        XrProgramBuildStatus status = close_cleanup_projection_arguments(
            function, projection, original_block_count, 0u, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    for (uint32_t projection_index = 0u; projection_index < *projection_count; ++projection_index) {
        XrXiCleanupProjectionStorage *projection = &function->cleanup_projections[projection_index];
        XrProgramBuildStatus status = materialize_cleanup_trap_projection(
            context, function, projection, original_block_count,
            &function->blocks[(*output_block_index)++], diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    for (uint32_t block_index = 0u; block_index < function->xi->nblocks; ++block_index) {
        XrXiBlockStorage *block = &function->block_storage[block_index];
        if (!block->reachable || !block->emission_ready)
            continue;
        for (uint32_t value_index = 0u; value_index < block->xi->nvalues; ++value_index) {
            const XiValue *value = block->xi->values[value_index];
            const XiCleanupBoundary *boundary = NULL;
            if (!value || !active_cleanup_boundary_at_program_point(function, value, &boundary))
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            if (!boundary)
                continue;
            const XrXiEmissionSpan *span = &block->emission_spans[value_index];
            for (uint32_t instruction = span->begin; instruction < span->end; ++instruction) {
                XrCoreIrInstructionInput *candidate = &block->instructions[instruction];
                if (!cleanup_trap_capable_operation(candidate->operation_id))
                    continue;
                uint32_t handler_state = function->cleanup_handler_state_before_value[value->id];
                if (handler_state != 0u) {
                    if (!cleanup_call_has_exact_active_handler_edge(context, function, value,
                                                                    candidate, handler_state))
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "cleanup call v%u lost its outer-handler edge", value->id);
                    continue;
                }
                XrXiCleanupPointStorage *active = find_cleanup_point(function, boundary);
                XrXiCleanupProjectionStorage *target =
                    active ? find_cleanup_projection(function, active, block->xi, instruction + 1u)
                           : NULL;
                if (!target)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "cleanup refusal edge has no exact projected successor");
                XrProgramBuildStatus status = append_cleanup_trap_successor(
                    candidate, function, target, NULL, NULL, 0u, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static const XrCoreOperationSpec *xi_block_terminal_contract(const XrXiBuildContext *context,
                                                             const XiFunc *function,
                                                             const XiBlock *block) {
    const XiValue *invoke = block_typed_invoke_call(context, function, block);
    if (invoke && invoke->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE)
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CALL_WITNESS_INVOKE);
    if (invoke)
        return xr_core_spec_operation_by_id(resolved_sealed_callee(context, function, invoke)
                                                ? XR_CORE_OP_CORE_CALL_SEALED_INVOKE
                                                : XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE);
    if (exact_infallible_class_construction_in_block(context, function, block))
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_BRANCH);
    if (block->kind == XI_BLOCK_UNREACHABLE &&
        canonical_block_is_trap_cleanup(context, function, block))
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_TRAP);
    if (block->kind == XI_BLOCK_UNREACHABLE &&
        canonical_block_is_cancel_cleanup(context, function, block))
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CANCEL_PUBLISH);
    if (block->kind == XI_BLOCK_UNREACHABLE)
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_PANIC_PUBLISH);
    if (block->kind == XI_BLOCK_RETURN && block->control && block->control->op == XI_ERR_RETURN)
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_ERROR_PUBLISH);
    if (block->kind == XI_BLOCK_RETURN)
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_RETURN);
    if (block->kind == XI_BLOCK_PLAIN)
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_BRANCH);
    if (block->kind == XI_BLOCK_IF)
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CONDITIONAL_BRANCH);
    return NULL;
}

/* Close target-neutral function contracts before any callable TypeId is
 * materialized.  This is a CoreSpec operation-algebra pass: it reads the
 * generated Xi projection plus exact Xg call targets and never inspects a
 * backend body, source spelling, or runtime representation. */
static XrProgramBuildStatus
precompute_function_contracts(XrXiBuildContext *context, char *diagnostic, size_t diagnostic_size) {
    uint32_t total_functions = 0u;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        if (module->function_count > UINT32_MAX - total_functions)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi function contract graph is too large");
        total_functions += module->function_count;
        for (uint32_t function_index = 0; function_index < module->function_count;
             ++function_index) {
            XrXiFunctionStorage *storage = &module->function_storage[function_index];
            const XiFunc *function = storage->xi;
            uint32_t effects = 0u;
            uint32_t capabilities = 0u;
            for (uint32_t block_index = 0; function && block_index < function->nblocks;
                 ++block_index) {
                const XiBlock *block = function->blocks[block_index];
                if (!canonical_block_is_reachable(context, function, block) ||
                    block_is_elided_class_construction_error_continuation(context, function, block))
                    continue;
                for (uint32_t value_index = 0; block && value_index < block->nvalues;
                     ++value_index) {
                    const XiValue *value = block->values[value_index];
                    if (!value || value_is_skipped(context, function, value))
                        continue;
                    uint32_t value_effects = 0u;
                    uint32_t value_capabilities = 0u;
                    const XrCoreOperationSpec *shared_callable =
                        value->op == XI_GET_SHARED && resolved_shared_callable_target(
                                                          context, function, value, NULL, NULL)
                            ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CALLABLE_PACK)
                            : NULL;
                    const XrCoreOperationSpec *class_construction =
                        resolved_canonical_class_construction(context, function, value, NULL, NULL)
                            ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT)
                            : NULL;
                    const XrCoreOperationSpec *value_aggregate_construction =
                        resolved_value_aggregate_construction(context, function, value, NULL, NULL)
                            ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT)
                            : NULL;
                    const XrCoreOperationSpec *empty_struct_literal =
                        resolved_empty_struct_literal(context, function, value)
                            ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT)
                            : NULL;
                    const XrCoreOperationSpec *unit_enum_literal =
                        resolved_unit_enum_literal(context, function, value, NULL)
                            ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_VARIANT_CONSTRUCT)
                            : NULL;
                    const XrCoreOperationSpec *aggregate_field_projection =
                        resolved_aggregate_field_projection(context, value, NULL)
                            ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_AGGREGATE_PROJECT)
                            : NULL;
                    const XrCoreOperationSpec *aggregate_field_store =
                        resolved_aggregate_field_store(context, function, value, NULL)
                            ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_PLACE_STORE)
                            : NULL;
                    const XrCoreOperationSpec *condition_assertion =
                        exact_condition_assertion(value)
                            ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_ASSERT_CONDITION)
                            : NULL;
                    const XrStdlibDefEntry *provider_entry =
                        resolved_provider_native_call(context, function, value);
                    const XrCoreOperationSpec *provider_operation =
                        provider_entry ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_PROVIDER_CALL)
                                       : NULL;
                    const XrCoreOperationSpec *suspension_operation =
                        resolved_suspension_native_call(context, function, value)
                            ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_COROUTINE_SUSPEND)
                            : NULL;
                    bool has_contract = false;
                    if (value->xg_existential_kind != XI_EXISTENTIAL_NONE) {
                        has_contract = xr_program_xi_semantic_operation_contract(
                            value->op, value->xg_existential_kind, &value_effects,
                            &value_capabilities);
                    } else if (value_is_static_typed_catch_test(value)) {
                        if (!static_typed_catch_contract_is_exact(context, value))
                            return fail(
                                diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi typed catch test v%u has inconsistent nominal token evidence",
                                value->id);
                        const XrCoreOperationSpec *constant =
                            xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CONSTANT_BOOL);
                        has_contract = constant != NULL;
                        value_effects = constant ? constant->effect_mask : 0u;
                        value_capabilities = constant ? constant->capability_mask : 0u;
                    } else if (shared_callable) {
                        has_contract = true;
                        value_effects = shared_callable->effect_mask;
                        value_capabilities = shared_callable->capability_mask;
                    } else if (class_construction) {
                        has_contract = true;
                        value_effects = class_construction->effect_mask;
                        value_capabilities = class_construction->capability_mask;
                    } else if (value_aggregate_construction) {
                        has_contract = true;
                        value_effects = value_aggregate_construction->effect_mask;
                        value_capabilities = value_aggregate_construction->capability_mask;
                    } else if (empty_struct_literal) {
                        has_contract = true;
                        value_effects = empty_struct_literal->effect_mask;
                        value_capabilities = empty_struct_literal->capability_mask;
                    } else if (unit_enum_literal) {
                        has_contract = true;
                        value_effects = unit_enum_literal->effect_mask;
                        value_capabilities = unit_enum_literal->capability_mask;
                    } else if (aggregate_field_projection) {
                        has_contract = true;
                        value_effects = aggregate_field_projection->effect_mask;
                        value_capabilities = aggregate_field_projection->capability_mask;
                    } else if (aggregate_field_store) {
                        const XrCoreOperationSpec *place_projection =
                            xr_core_spec_operation_by_id(XR_CORE_OP_CORE_PLACE_PROJECT);
                        has_contract = place_projection != NULL;
                        value_effects = aggregate_field_store->effect_mask |
                                        (place_projection ? place_projection->effect_mask : 0u);
                        value_capabilities =
                            aggregate_field_store->capability_mask |
                            (place_projection ? place_projection->capability_mask : 0u);
                    } else if (condition_assertion) {
                        has_contract = true;
                        value_effects = condition_assertion->effect_mask;
                        value_capabilities = condition_assertion->capability_mask;
                    } else if (provider_operation) {
                        has_contract = true;
                        value_effects = provider_operation->effect_mask;
                        value_capabilities = provider_operation->capability_mask;
                    } else if (suspension_operation) {
                        has_contract = true;
                        value_effects = suspension_operation->effect_mask;
                        value_capabilities = suspension_operation->capability_mask;
                    } else {
                        has_contract = xr_program_xi_operation_contract(value->op, &value_effects,
                                                                        &value_capabilities);
                    }
                    if (!has_contract) {
                        if (value->op == XI_CHECKTYPE) {
                            const char *proof_failure =
                                imported_callable_checktype_proof_failure(context, function, value);
                            return fail(
                                diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi function %s CHECKTYPE at v%u cannot be mechanically erased: %s",
                                function->name ? function->name : "<anonymous>", value->id,
                                proof_failure ? proof_failure
                                              : "no canonical CHECKTYPE projection");
                        }
                        const XiValue *first_user = NULL;
                        uint16_t first_argument = UINT16_MAX;
                        for (uint32_t use_block = 0u; use_block < function->nblocks && !first_user;
                             ++use_block) {
                            const XiBlock *use_row = function->blocks[use_block];
                            for (uint32_t use_index = 0u;
                                 use_row && use_index < use_row->nvalues && !first_user;
                                 ++use_index) {
                                const XiValue *candidate = use_row->values[use_index];
                                for (uint16_t argument = 0u;
                                     candidate && argument < candidate->nargs; ++argument) {
                                    if (candidate->args && candidate->args[argument] == value) {
                                        first_user = candidate;
                                        first_argument = argument;
                                        break;
                                    }
                                }
                            }
                        }
                        return fail(diagnostic, diagnostic_size,
                                    XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                    "Xi function %s operation %s (%u) at v%u has no unique "
                                    "CoreSpec contract (aux=%lld nargs=%u first-use=%s:%u)",
                                    function->name ? function->name : "<anonymous>",
                                    xi_op_name(value->op), value->op, value->id,
                                    (long long) value->aux_int, (unsigned) value->nargs,
                                    first_user ? xi_op_name(first_user->op) : "none",
                                    (unsigned) first_argument);
                    }
                    effects |= value_effects;
                    capabilities |= value_capabilities;
                }
                const XrCoreOperationSpec *terminal =
                    block ? xi_block_terminal_contract(context, function, block) : NULL;
                if (!terminal)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi block has no active CoreSpec terminal contract");
                effects |= terminal->effect_mask;
                capabilities |= terminal->capability_mask;
            }
            storage->closed_effect_mask = effects;
            storage->closed_capability_mask = capabilities;
        }
    }

    for (uint32_t iteration = 0; iteration <= total_functions; ++iteration) {
        bool changed = false;
        for (uint32_t module_index = 0; module_index < context->source->module_count;
             ++module_index) {
            XrXiModuleStorage *module = &context->storage[module_index];
            for (uint32_t function_index = 0; function_index < module->function_count;
                 ++function_index) {
                XrXiFunctionStorage *storage = &module->function_storage[function_index];
                uint32_t effects = storage->closed_effect_mask;
                uint32_t capabilities = storage->closed_capability_mask;
                for (uint32_t block_index = 0; block_index < storage->xi->nblocks; ++block_index) {
                    const XiBlock *block = storage->xi->blocks[block_index];
                    if (!canonical_block_is_reachable(context, storage->xi, block) ||
                        block_is_elided_class_construction_error_continuation(context, storage->xi,
                                                                              block))
                        continue;
                    for (uint32_t value_index = 0; block && value_index < block->nvalues;
                         ++value_index) {
                        const XiValue *value = block->values[value_index];
                        bool witness =
                            value &&
                            (value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT) &&
                            (value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_DIRECT ||
                             value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE);
                        bool sealed_method =
                            value &&
                            (value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT) &&
                            value->xg_existential_kind == XI_EXISTENTIAL_NONE;
                        if (!value || (value->op != XI_CALL && !witness && !sealed_method))
                            continue;
                        if (resolved_canonical_class_construction(context, storage->xi, value, NULL,
                                                                  NULL) ||
                            resolved_empty_struct_literal(context, storage->xi, value) ||
                            resolved_provider_native_call(context, storage->xi, value) ||
                            resolved_suspension_native_call(context, storage->xi, value))
                            continue;
                        const XiFunc *callee = resolved_sealed_callee(context, storage->xi, value);
                        bool invoke =
                            typed_invoke_check_block_for_call(context, storage->xi, value) != NULL;
                        if (witness) {
                            uint32_t witness_effects = 0u;
                            uint32_t witness_capabilities = 0u;
                            if (!witness_call_effect_contract(context, storage->xi, value, false,
                                                              &witness_effects,
                                                              &witness_capabilities))
                                return fail(diagnostic, diagnostic_size,
                                            XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                            "Xi function contract has an unresolved witness "
                                            "slot");
                            if (invoke)
                                witness_effects &= ~XR_CORE_EFFECT_ERROR;
                            effects |= witness_effects;
                            capabilities |= witness_capabilities;
                            continue;
                        }
                        if (callee) {
                            const XrXiFunctionStorage *callee_storage =
                                find_xi_function(context, callee, NULL, NULL);
                            if (!callee_storage)
                                return fail(diagnostic, diagnostic_size,
                                            XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                            "Xi function contract has an unresolved direct target");
                            uint32_t callee_effects = callee_storage->closed_effect_mask;
                            if (invoke)
                                callee_effects &= ~XR_CORE_EFFECT_ERROR;
                            effects |= callee_effects;
                            capabilities |= callee_storage->closed_capability_mask;
                            continue;
                        }
                        if (sealed_method)
                            return fail(diagnostic, diagnostic_size,
                                        XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                        "Xi function %s call v%u has an unresolved method target",
                                        storage->xi && storage->xi->name ? storage->xi->name
                                                                         : "<anonymous>",
                                        value->id);
                        XrXiCallableTargetSet target_set = {0};
                        if (!resolved_callable_call_targets(context, storage->xi, value,
                                                            &target_set))
                            return fail(diagnostic, diagnostic_size,
                                        XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                        "Xi function contract has an unresolved callable target "
                                        "set");
                        for (uint32_t target_index = 0u; target_index < target_set.target_count;
                             ++target_index) {
                            const XiFunc *target = find_xi_function_by_xg_id(
                                context, target_set.targets[target_index].target_func_id);
                            const XrXiFunctionStorage *target_storage =
                                find_xi_function(context, target, NULL, NULL);
                            if (!target_storage)
                                return fail(diagnostic, diagnostic_size,
                                            XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                            "Xi callable contract target is outside the canonical "
                                            "graph");
                            uint32_t target_effects = target_storage->closed_effect_mask;
                            if (invoke)
                                target_effects &= ~XR_CORE_EFFECT_ERROR;
                            effects |= target_effects;
                            capabilities |= target_storage->closed_capability_mask;
                        }
                    }
                }
                if (effects != storage->closed_effect_mask ||
                    capabilities != storage->closed_capability_mask) {
                    storage->closed_effect_mask = effects;
                    storage->closed_capability_mask = capabilities;
                    changed = true;
                }
            }
        }
        if (!changed) {
            for (uint32_t module_index = 0; module_index < context->source->module_count;
                 ++module_index)
                for (uint32_t function_index = 0;
                     function_index < context->storage[module_index].function_count;
                     ++function_index)
                    context->storage[module_index]
                        .function_storage[function_index]
                        .closed_contract_ready = true;
            return XR_PROGRAM_BUILD_OK;
        }
    }
    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "Xi callable contract closure did not converge");
}

static XrProgramBuildStatus prepare_function_signature(XrXiBuildContext *context,
                                                       XrXiModuleStorage *module,
                                                       uint32_t function_index, char *diagnostic,
                                                       size_t diagnostic_size) {
    const XiFunc *xi =
        function_index < module->function_count ? module->xi_functions[function_index] : NULL;
    XrCoreIrFunctionInput *output = &module->functions[function_index];
    XrXiFunctionStorage *storage = &module->function_storage[function_index];
    XrCoreIrKey key = storage->key;
    uint32_t closed_effect_mask = storage->closed_effect_mask;
    uint32_t closed_capability_mask = storage->closed_capability_mask;
    bool closed_contract_ready = storage->closed_contract_ready;
    XrXiBlockStorage *block_storage = storage->block_storage;
    XrXiCleanupPointStorage *cleanup_points = storage->cleanup_points;
    uint32_t cleanup_point_count = storage->cleanup_point_count;
    XrXiCleanupChainNode *cleanup_chain_nodes = storage->cleanup_chain_nodes;
    uint32_t cleanup_chain_node_count = storage->cleanup_chain_node_count;
    XrXiCleanupHandlerChainNode *cleanup_handler_chain_nodes = storage->cleanup_handler_chain_nodes;
    XrXiCleanupRegistrationStorage *cleanup_registrations = storage->cleanup_registrations;
    uint32_t *cleanup_registration_by_value = storage->cleanup_registration_by_value;
    uint32_t *cleanup_state_before_value = storage->cleanup_state_before_value;
    uint32_t *cleanup_handler_state_before_value = storage->cleanup_handler_state_before_value;
    uint32_t cleanup_registration_count = storage->cleanup_registration_count;
    memset(output, 0, sizeof(*output));
    memset(storage, 0, sizeof(*storage));
    storage->xi = xi;
    storage->key = key;
    storage->closed_effect_mask = closed_effect_mask;
    storage->closed_capability_mask = closed_capability_mask;
    storage->closed_contract_ready = closed_contract_ready;
    storage->block_storage = block_storage;
    storage->cleanup_points = cleanup_points;
    storage->cleanup_point_count = cleanup_point_count;
    storage->cleanup_chain_nodes = cleanup_chain_nodes;
    storage->cleanup_chain_node_count = cleanup_chain_node_count;
    storage->cleanup_handler_chain_nodes = cleanup_handler_chain_nodes;
    storage->cleanup_registrations = cleanup_registrations;
    storage->cleanup_registration_by_value = cleanup_registration_by_value;
    storage->cleanup_state_before_value = cleanup_state_before_value;
    storage->cleanup_handler_state_before_value = cleanup_handler_state_before_value;
    storage->cleanup_registration_count = cleanup_registration_count;
    if (!xi || xi->stage != XI_STAGE_OPTIMIZED || xi->semantic_plan || xi->nblocks == 0u ||
        !xi->entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %u is not an Optimized program input", function_index);
    if (xi->ncaptures != 0u) {
        if (xi->has_receiver || !map_capture_type(context, xi, &storage->capture_type_id))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi function %u has an unsupported capture contract", function_index);
        memset(&storage->capture_receiver, 0, sizeof(storage->capture_receiver));
        storage->capture_receiver.op = XI_PARAM;
        storage->capture_receiver.id = UINT32_MAX;
        storage->capture_receiver.block = xi->entry;
        storage->capture_receiver.param_mode = XR_PARAM_READ;
    }
    output->key = storage->key;
    uint32_t parameter_offset = storage->capture_type_id != XR_CORE_TYPE_VOID ? 1u : 0u;
    output->parameter_count = (uint32_t) xi->nparams + parameter_offset;
    bool result_mapped = false;
    if (xi->return_type && xi->return_type->kind == XR_KIND_FUNCTION) {
        uint64_t signature_key = 0u;
        bool found_return = false;
        bool exact_returns = true;
        for (uint32_t block_index = 0u; block_index < xi->nblocks; ++block_index) {
            const XiBlock *block = xi->blocks[block_index];
            if (!canonical_block_is_reachable(context, xi, block) || !block ||
                block->kind != XI_BLOCK_RETURN || !block->control ||
                block->control->op == XI_ERR_RETURN)
                continue;
            uint64_t candidate = 0u;
            if (!callable_signature_key_for_value(context, xi, block->control, 0u, &candidate) ||
                (found_return && candidate != signature_key)) {
                exact_returns = false;
                break;
            }
            signature_key = candidate;
            found_return = true;
        }
        result_mapped = exact_returns && found_return &&
                        map_callable_signature_contract(context, xi->return_type, signature_key,
                                                        &output->result_type_id, NULL);
    } else {
        result_mapped = map_type(context, xi->return_type, &output->result_type_id);
    }
    if (!result_mapped)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %s (%u) result type kind %u is not active in CoreSpec",
                    xi->name ? xi->name : "<anonymous>", function_index,
                    xi->return_type ? (unsigned) xi->return_type->kind : UINT32_MAX);
    XrProgramBuildStatus signature_status =
        map_function_error_type(context, xi, &output->error_type_id, diagnostic, diagnostic_size);
    if (signature_status != XR_PROGRAM_BUILD_OK)
        return signature_status;
    signature_status =
        map_function_panic_type(context, xi, &output->panic_type_id, diagnostic, diagnostic_size);
    if (signature_status != XR_PROGRAM_BUILD_OK)
        return signature_status;
    output->result_ownership = logical_ownership_for_type(context, output->result_type_id);
    output->has_receiver = xi->has_receiver || parameter_offset != 0u;
    output->receiver_mode = xi->has_receiver ? xi->receiver_mode : XR_PARAM_READ;
    if (output->parameter_count != 0u) {
        storage->parameter_types =
            xr_calloc(output->parameter_count, sizeof(*storage->parameter_types));
        storage->parameter_modes =
            xr_calloc(output->parameter_count, sizeof(*storage->parameter_modes));
        if (!storage->parameter_types || !storage->parameter_modes)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        if (parameter_offset != 0u) {
            storage->parameter_types[0] = storage->capture_type_id;
            storage->parameter_modes[0] = XR_PARAM_READ;
        }
        for (uint16_t parameter = 0; parameter < xi->nparams; ++parameter) {
            const XiValue *parameter_value = xi->params ? xi->params[parameter] : NULL;
            XrParamMode parameter_mode =
                xi->has_receiver && parameter == 0u ? (XrParamMode) xi->receiver_mode
                : parameter_value                   ? (XrParamMode) parameter_value->param_mode
                                                    : XR_PARAM_READ;
            bool parameter_type_mapped =
                parameter_value && parameter_value->type &&
                        parameter_value->type->kind == XR_KIND_FUNCTION
                    ? map_logical_value_type(
                          context, xi, parameter_value,
                          &storage->parameter_types[parameter + parameter_offset])
                    : parameter_value &&
                          map_type_for_mode(context, parameter_value->type, parameter_mode,
                                            &storage->parameter_types[parameter + parameter_offset],
                                            NULL, 0u);
            if (!parameter_value || parameter_value->op != XI_PARAM ||
                parameter_value->aux_int != parameter ||
                (parameter_value->type && parameter_value->type->kind == XR_KIND_INTERFACE &&
                 !interface_parameter_contract_is_exact(context, xi, parameter_value,
                                                        parameter_mode)) ||
                !parameter_type_mapped ||
                storage->parameter_types[parameter + parameter_offset] == XR_CORE_TYPE_VOID)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi function %u parameter %u is not a CoreSpec value", function_index,
                            parameter);
            if (!xr_param_mode_is_valid(parameter_mode))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi function %u parameter %u has an invalid mode", function_index,
                            parameter);
            storage->parameter_modes[parameter + parameter_offset] = parameter_mode;
        }
        output->parameter_types = storage->parameter_types;
        output->parameter_modes = storage->parameter_modes;
    }
    output->flags = xi == context->source->entry_function ? XR_PROGRAM_FUNCTION_ENTRY : 0u;

    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus build_function_body(XrXiBuildContext *context,
                                                XrXiModuleStorage *module, uint32_t function_index,
                                                char *diagnostic, size_t diagnostic_size) {
    const XiFunc *xi =
        function_index < module->function_count ? module->xi_functions[function_index] : NULL;
    XrCoreIrFunctionInput *output = &module->functions[function_index];
    XrXiFunctionStorage *storage = &module->function_storage[function_index];

    if (!storage->block_storage)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    output->entry_block = block_key(storage, xi->entry);
    XrProgramBuildStatus status =
        close_block_arguments(context, storage, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    status = prepare_coroutine_shape(context, storage, output, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    uint32_t synthetic_cancel_count = 0u;
    for (uint32_t safepoint = 0u; safepoint < output->coroutine_safepoint_count; ++safepoint) {
        const XiCoroSuspendPoint *point = coroutine_point_for_safepoint(storage, safepoint);
        if (!point)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi function %u has no exact safepoint owner", function_index);
        const XrXiCancelEdge *edge = find_cancel_edge(context, xi, point);
        if (!edge) {
            if (synthetic_cancel_count == UINT32_MAX)
                return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
            ++synthetic_cancel_count;
        } else if (edge->private_projection) {
            uint32_t private_block_count = 0u;
            status = static_cleanup_graph_block_count(xi, edge->registration, &private_block_count);
            if (status != XR_PROGRAM_BUILD_OK)
                return fail(diagnostic, diagnostic_size, status,
                            "Xi private cancellation cleanup graph is invalid");
            if (private_block_count > UINT32_MAX - synthetic_cancel_count)
                return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
            synthetic_cancel_count += private_block_count;
        }
    }
    uint32_t cleanup_projection_upper_bound = storage->cleanup_point_count;
    for (uint32_t block = 0u; block < xi->nblocks; ++block) {
        const XiBlock *source_block = xi->blocks[block];
        const XrXiBlockStorage *source_storage = find_block_storage(storage, source_block);
        if (!source_storage || !source_storage->reachable)
            continue;
        for (uint32_t value = 0u; source_block && value < source_block->nvalues; ++value) {
            const XiCleanupBoundary *boundary = NULL;
            if (!source_block->values[value] ||
                !active_cleanup_boundary_at_program_point(storage, source_block->values[value],
                                                          &boundary))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "reachable Xi value has no cleanup projection bound");
            if (!boundary)
                continue;
            if (cleanup_projection_upper_bound == UINT32_MAX)
                return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
            ++cleanup_projection_upper_bound;
        }
    }
    if (synthetic_cancel_count > UINT32_MAX - xi->nblocks ||
        cleanup_projection_upper_bound > UINT32_MAX - xi->nblocks - synthetic_cancel_count)
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    uint32_t block_capacity = xi->nblocks + synthetic_cancel_count + cleanup_projection_upper_bound;
    storage->blocks = xr_calloc(block_capacity, sizeof(*storage->blocks));
    if (!storage->blocks)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    output->blocks = storage->blocks;

    output->block_count = 0u;
    for (uint32_t block_index = 0u; block_index < xi->nblocks; ++block_index)
        if (storage->block_storage[block_index].reachable &&
            !block_is_elided_class_construction_error_continuation(context, xi,
                                                                   xi->blocks[block_index]))
            ++output->block_count;
    output->block_count += synthetic_cancel_count;
    if (output->block_count == 0u || !find_block_storage(storage, xi->entry)->reachable ||
        block_is_elided_class_construction_error_continuation(context, xi, xi->entry))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi function %u has no canonical entry block", function_index);

    uint32_t output_block_index = 0u;
    for (uint32_t block_index = 0; block_index < xi->nblocks; ++block_index) {
        const XiBlock *xi_block = xi->blocks[block_index];
        if (!storage->block_storage[block_index].reachable ||
            block_is_elided_class_construction_error_continuation(context, xi, xi_block))
            continue;
        XrCoreIrBlockInput *block_output = &storage->blocks[output_block_index++];
        XrXiBlockStorage *block_storage = &storage->block_storage[block_index];
        block_output->key = block_key(storage, xi_block);
        block_output->arguments = block_storage->arguments;
        block_output->argument_count = block_storage->argument_count;
        const XiCoroSuspendPoint *suspend_point = output->coroutine_safepoint_count != 0u
                                                      ? coroutine_point_for_block(storage, xi_block)
                                                      : NULL;

        uint32_t emitted = block_storage->argument_count != 0u ? 1u : 0u;
        if (block_storage->reconstructed_place_count > UINT32_MAX - emitted)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        emitted += block_storage->reconstructed_place_count;
        for (uint32_t value_index = 0; value_index < xi_block->nvalues; ++value_index) {
            const XiValue *value = xi_block->values[value_index];
            if (value_is_skipped(context, xi, value))
                continue;
            if (suspend_point && value == suspend_point->op)
                continue;
            if (value->op == XI_CLOSURE_NEW && value->nargs != 0u)
                ++emitted;
            if (value->xg_existential_kind == XI_EXISTENTIAL_PACK &&
                value->xg_interface_use_kind == XI_INTERFACE_USE_OWNED_STORAGE)
                ++emitted;
            ++emitted;
        }
        ++emitted;
        uint64_t instruction_capacity_wide =
            (uint64_t) emitted * 2u + block_storage->argument_count;
        if (instruction_capacity_wide > UINT32_MAX)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        uint32_t instruction_capacity = (uint32_t) instruction_capacity_wide;
        block_storage->instructions =
            xr_calloc(instruction_capacity, sizeof(*block_storage->instructions));
        if (!block_storage->instructions)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        block_storage->emission_span_count = xi_block->nvalues;
        block_storage->emission_spans =
            xi_block->nvalues ? xr_calloc(xi_block->nvalues, sizeof(*block_storage->emission_spans))
                              : NULL;
        if (xi_block->nvalues && !block_storage->emission_spans)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        block_storage->instruction_count = instruction_capacity;
        block_output->instructions = block_storage->instructions;
        block_output->instruction_count = instruction_capacity;

        uint32_t instruction_index = 0;
        if (block_storage->argument_count != 0u) {
            XrCoreIrInstructionInput *arguments = &block_storage->instructions[instruction_index++];
            arguments->operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT;
            arguments->result_type_id = XR_CORE_TYPE_VOID;
            arguments->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            XrCoreIrKey *operands = xr_calloc(block_storage->argument_count, sizeof(*operands));
            if (!operands)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            for (uint32_t argument = 0; argument < block_storage->argument_count; ++argument)
                operands[argument] = block_storage->arguments[argument].key;
            arguments->operands = operands;
            arguments->operand_count = block_storage->argument_count;
        }
        for (uint32_t place_index = 0u; place_index < block_storage->reconstructed_place_count;
             ++place_index) {
            const XrXiReconstructedPlaceStorage *place =
                &block_storage->reconstructed_places[place_index];
            uint32_t count = 0u;
            status = translate_reconstructed_place(context, storage, block_storage, place,
                                                   &block_storage->instructions[instruction_index],
                                                   &count);
            if (status != XR_PROGRAM_BUILD_OK)
                return fail(diagnostic, diagnostic_size, status,
                            "Xi frame place cannot be reconstructed in b%u", xi_block->id);
            for (uint32_t emitted = 0u; emitted < count; ++emitted) {
                const XrCoreOperationSpec *operation = xr_core_spec_operation_by_id(
                    block_storage->instructions[instruction_index++].operation_id);
                if (!operation)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "reconstructed frame place has no CoreSpec operation");
                storage->local_effect_mask |= operation->effect_mask;
                storage->local_capability_mask |= operation->capability_mask;
            }
        }
        for (uint32_t value_index = 0; value_index < xi_block->nvalues; ++value_index) {
            const XiValue *value = xi_block->values[value_index];
            XrXiEmissionSpan *span = &block_storage->emission_spans[value_index];
            span->begin = instruction_index;
            if (value_is_skipped(context, xi, value)) {
                span->end = instruction_index;
                continue;
            }
            if (suspend_point && value == suspend_point->op) {
                span->end = instruction_index;
                continue;
            }
            uint32_t direct_field = UINT32_MAX;
            const XiValue *direct_base =
                direct_projection_base_place(context, xi, value, &direct_field);
            if (direct_base) {
                uint16_t place_type = XR_CORE_TYPE_VOID;
                if (!map_logical_value_type(context, xi, value, &place_type))
                    return XR_PROGRAM_BUILD_INVALID_INPUT;
                XrXiReconstructedPlaceStorage place = {
                    .place = value,
                    .owner = local_place_storage_root(context, xi, value),
                    .base_place = direct_base,
                    .field_ordinal = direct_field,
                    .key = value_key(storage, value),
                    .type_id = place_type,
                };
                uint32_t count = 0u;
                status = translate_reconstructed_place(
                    context, storage, block_storage, &place,
                    &block_storage->instructions[instruction_index], &count);
                if (status != XR_PROGRAM_BUILD_OK)
                    return fail(diagnostic, diagnostic_size, status,
                                "Xi projected address v%u has no exact storage root", value->id);
                for (uint32_t emitted_instruction = 0u; emitted_instruction < count;
                     ++emitted_instruction) {
                    const XrCoreOperationSpec *operation = xr_core_spec_operation_by_id(
                        block_storage->instructions[instruction_index++].operation_id);
                    if (!operation)
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "projected address has no CoreSpec operation");
                    storage->local_effect_mask |= operation->effect_mask;
                    storage->local_capability_mask |= operation->capability_mask;
                }
                span->end = instruction_index;
                continue;
            }
            bool aggregate_place_access =
                resolved_aggregate_field_store(context, xi, value, NULL) ||
                (resolved_aggregate_field_projection(context, value, NULL) && value->args &&
                 logical_value_category(xi, value->args[0]) == XR_CORE_IR_PLACE);
            if (aggregate_place_access) {
                if (instruction_index > instruction_capacity ||
                    instruction_capacity - instruction_index < 2u)
                    return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
                XrCoreIrInstructionInput *instructions =
                    &block_storage->instructions[instruction_index];
                status =
                    translate_aggregate_place_access(context, storage, value, block_storage,
                                                     instructions, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
                for (uint32_t emitted_instruction = 0u; emitted_instruction < 2u;
                     ++emitted_instruction) {
                    const XrCoreOperationSpec *operation = xr_core_spec_operation_by_id(
                        instructions[emitted_instruction].operation_id);
                    if (!operation)
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "translated Xi field access v%u has no CoreSpec operation",
                                    value->id);
                    storage->local_effect_mask |= operation->effect_mask;
                    storage->local_capability_mask |= operation->capability_mask;
                }
                instruction_index += 2u;
                span->end = instruction_index;
                continue;
            }
            if (value->op == XI_CLOSURE_NEW && value->nargs != 0u) {
                XrCoreIrInstructionInput *capture =
                    &block_storage->instructions[instruction_index++];
                status = translate_capture_construct(context, storage, value, block_storage,
                                                     capture, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
                storage->local_effect_mask |=
                    xr_core_spec_operation_by_id(capture->operation_id)->effect_mask;
                storage->local_capability_mask |=
                    xr_core_spec_operation_by_id(capture->operation_id)->capability_mask;
            }
            bool copied_existential_source = false;
            XrCoreIrInstructionInput *owner_copy = &block_storage->instructions[instruction_index];
            status = translate_existential_owner_copy(context, storage, value, block_storage,
                                                      owner_copy, &copied_existential_source,
                                                      diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            if (copied_existential_source) {
                ++instruction_index;
                storage->local_effect_mask |=
                    xr_core_spec_operation_by_id(owner_copy->operation_id)->effect_mask;
                storage->local_capability_mask |=
                    xr_core_spec_operation_by_id(owner_copy->operation_id)->capability_mask;
            }
            XrCoreIrInstructionInput *instruction =
                &block_storage->instructions[instruction_index++];
            status = translate_value(context, module, storage, value, block_storage, instruction,
                                     diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            const XrCoreOperationSpec *operation =
                xr_core_spec_operation_by_id(instruction->operation_id);
            if (!operation)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "translated Xi v%u has no CoreSpec operation", value->id);
            if (copied_existential_source) {
                if (instruction->operation_id != XR_CORE_OP_CORE_EXISTENTIAL_PACK ||
                    instruction->operand_count != 1u || !instruction->operands)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi existential pack v%u lost its canonical copy boundary",
                                value->id);
                ((XrCoreIrKey *) instruction->operands)[0] = owner_copy->result;
            }
            storage->local_effect_mask |= operation->effect_mask;
            storage->local_capability_mask |= operation->capability_mask;
            span->end = instruction_index;
        }
        if (!emission_spans_are_exact(context, xi, block_storage, suspend_point, instruction_index))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi block b%u has no exact CoreIR emission spans", xi_block->id);

        XrCoreIrInstructionInput *terminator = &block_storage->instructions[instruction_index++];
        terminator->result_type_id = XR_CORE_TYPE_VOID;
        terminator->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        const XiValue *invoke_call = block_typed_invoke_call(context, xi, xi_block);
        if (suspend_point && suspend_point->kind == XI_CORO_SUSP_YIELD) {
            status =
                translate_coroutine_yield_terminator(context, storage, block_storage, suspend_point,
                                                     terminator, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (suspend_point && suspend_point->kind == XI_CORO_SUSP_CALL) {
            if (resolved_suspension_native_call(context, xi, suspend_point->op))
                status = translate_coroutine_suspend_terminator(context, storage, block_storage,
                                                                suspend_point, terminator,
                                                                diagnostic, diagnostic_size);
            else
                status = translate_coroutine_call_terminator(
                    context, module, storage, block_storage, suspend_point, terminator, diagnostic,
                    diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (invoke_call) {
            const XiFunc *callee = resolved_sealed_callee(context, xi, invoke_call);
            bool witness = invoke_call->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE;
            bool indirect = callee == NULL && !witness;
            const XrXiFunctionStorage *callee_storage =
                find_xi_function(context, callee, NULL, NULL);
            XrXiBlockStorage *normal = find_block_storage(storage, xi_block->succs[1]);
            XrXiBlockStorage *error = find_block_storage(storage, xi_block->succs[0]);
            if ((!indirect && !witness && !callee_storage) ||
                (indirect && !resolved_callable_call_targets(context, xi, invoke_call, NULL)) ||
                (witness && !resolved_witness_callsite(context, xi, invoke_call)) || !normal ||
                !error)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi invoke block b%u has an unresolved continuation", xi_block->id);
            terminator->operation_id = witness    ? XR_CORE_OP_CORE_CALL_WITNESS_INVOKE
                                       : indirect ? XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE
                                                  : XR_CORE_OP_CORE_CALL_SEALED_INVOKE;
            if (witness) {
                terminator->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
                terminator->immediate.u32 = invoke_call->xg_interface_dispatch_slot;
            } else if (!indirect) {
                terminator->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
                terminator->immediate.key = callee_storage->key;
            }
            XrCoreIrKey *successors = xr_calloc(2u, sizeof(*successors));
            if (!successors)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            successors[0] = block_key(storage, normal->xi);
            successors[1] = block_key(storage, error->xi);
            terminator->successors = successors;
            terminator->successor_count = 2u;
            uint32_t first_operand =
                indirect || witness || (callee && callee->has_receiver) ? 0u : 1u;
            status = set_invoke_operands(context, terminator, storage, block_storage, invoke_call,
                                         normal, error, first_operand, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            const XrXiTrapEdge *trap_edge = find_trap_edge(context, xi, invoke_call);
            if (trap_edge) {
                const XrXiBlockStorage *handler = find_block_storage(storage, trap_edge->handler);
                status = append_invoke_trap_edge_operands(context, terminator, storage,
                                                          block_storage, invoke_call, handler,
                                                          diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        } else if (exact_infallible_class_construction_in_block(context, xi, xi_block)) {
            XrXiBlockStorage *successor = find_block_storage(storage, xi_block->succs[1]);
            if (!successor)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi infallible allocation block b%u has no normal continuation",
                            xi_block->id);
            terminator->operation_id = XR_CORE_OP_CORE_BRANCH;
            XrCoreIrKey *successors = xr_calloc(1u, sizeof(*successors));
            if (!successors)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            successors[0] = block_key(storage, successor->xi);
            terminator->successors = successors;
            terminator->successor_count = 1u;
            status = set_edge_operands(context, terminator, storage, block_storage, successor, NULL,
                                       NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (xi_block->kind == XI_BLOCK_UNREACHABLE) {
            if (block_storage->trap_cleanup) {
                terminator->operation_id = XR_CORE_OP_CORE_TRAP;
                terminator->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
                terminator->immediate.u32 = 7u;
            } else if (block_storage->cancel_cleanup) {
                terminator->operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH;
            } else {
                const XiValue *throw_value = NULL;
                for (uint32_t value_index = 0u; value_index < xi_block->nvalues; ++value_index) {
                    const XiValue *candidate = xi_block->values[value_index];
                    if (!candidate || candidate->op != XI_THROW)
                        continue;
                    if (throw_value)
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi panic block b%u has multiple throw terminals",
                                    xi_block->id);
                    throw_value = candidate;
                }
                if (!throw_value || throw_value->nargs != 1u || !throw_value->args ||
                    !throw_value->args[0])
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi unreachable block b%u has no typed panic terminal",
                                xi_block->id);
                terminator->operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH;
                XiValue *published[] = {throw_value->args[0]};
                status = set_operands(context, terminator, storage, block_storage, published, 1u,
                                      diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        } else if (xi_block->kind == XI_BLOCK_RETURN && xi_block->control &&
                   xi_block->control->op == XI_ERR_RETURN) {
            if (xi_block->control->nargs != 1u || !xi_block->control->args[0])
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi error publication in b%u has no typed payload", xi_block->id);
            terminator->operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH;
            XiValue *published[] = {xi_block->control->args[0]};
            status = set_operands(context, terminator, storage, block_storage, published, 1u,
                                  diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (xi_block->kind == XI_BLOCK_RETURN) {
            terminator->operation_id = XR_CORE_OP_CORE_RETURN;
            if (xi_block->control) {
                XiValue *returned[] = {xi_block->control};
                status = set_operands(context, terminator, storage, block_storage, returned, 1u,
                                      diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            } else if (output->result_type_id != XR_CORE_TYPE_VOID) {
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi function %u has a value-less non-void return", function_index);
            }
        } else if (xi_block->kind == XI_BLOCK_PLAIN) {
            XrXiBlockStorage *successor = find_block_storage(storage, xi_block->succs[0]);
            if (!successor || xi_block->succs[1])
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi function %s plain block b%u has invalid successors b%u/b%u",
                            xi->name ? xi->name : "<anonymous>", xi_block->id,
                            xi_block->succs[0] ? xi_block->succs[0]->id : UINT32_MAX,
                            xi_block->succs[1] ? xi_block->succs[1]->id : UINT32_MAX);
            terminator->operation_id = XR_CORE_OP_CORE_BRANCH;
            XrCoreIrKey *successors = xr_calloc(1u, sizeof(*successors));
            if (!successors)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            successors[0] = block_key(storage, successor->xi);
            terminator->successors = successors;
            terminator->successor_count = 1u;
            status = set_edge_operands(context, terminator, storage, block_storage, successor, NULL,
                                       NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (xi_block->kind == XI_BLOCK_IF &&
                   block_storage->static_branch_outcome != XR_XI_STATIC_BRANCH_UNKNOWN) {
            const XiBlock *selected = canonical_block_successor(context, xi, xi_block, 0u);
            XrXiBlockStorage *successor = find_block_storage(storage, selected);
            if (!successor)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi static typed catch block b%u has no selected successor",
                            xi_block->id);
            terminator->operation_id = XR_CORE_OP_CORE_BRANCH;
            XrCoreIrKey *successors = xr_calloc(1u, sizeof(*successors));
            if (!successors)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            successors[0] = block_key(storage, successor->xi);
            terminator->successors = successors;
            terminator->successor_count = 1u;
            status = set_edge_operands(context, terminator, storage, block_storage, successor, NULL,
                                       NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (xi_block->kind == XI_BLOCK_IF) {
            XrXiBlockStorage *true_block = find_block_storage(storage, xi_block->succs[0]);
            XrXiBlockStorage *false_block = find_block_storage(storage, xi_block->succs[1]);
            uint16_t condition_type = XR_CORE_TYPE_VOID;
            if (!true_block || !false_block || !xi_block->control ||
                !map_type(context, xi_block->control->type, &condition_type) ||
                condition_type != XR_CORE_TYPE_BOOL)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi conditional block b%u is incomplete", xi_block->id);
            terminator->operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH;
            XrCoreIrKey *successors = xr_calloc(2u, sizeof(*successors));
            if (!successors)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            successors[0] = block_key(storage, true_block->xi);
            successors[1] = block_key(storage, false_block->xi);
            terminator->successors = successors;
            terminator->successor_count = 2u;
            status = set_edge_operands(context, terminator, storage, block_storage, true_block,
                                       false_block, xi_block->control, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else {
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi unreachable block b%u requires an explicit CoreSpec terminal",
                        xi_block->id);
        }
        status = close_logical_owner_lifetimes(context, storage, block_storage, instruction_index,
                                               instruction_capacity, &instruction_index, diagnostic,
                                               diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
        block_output->instruction_count = instruction_index;
        block_storage->instruction_count = instruction_index;
        terminator = &block_storage->instructions[instruction_index - 1u];
        const XrCoreOperationSpec *terminal_operation =
            xr_core_spec_operation_by_id(terminator->operation_id);
        if (!terminal_operation)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi block b%u has no CoreSpec terminal operation", xi_block->id);
        storage->local_effect_mask |= terminal_operation->effect_mask;
        storage->local_capability_mask |= terminal_operation->capability_mask;
        block_storage->emission_ready = true;
    }
    status = bind_cleanup_program_points(storage, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    uint32_t original_block_count = output_block_index;
    uint32_t cleanup_trap_projection_count = 0u;
    status = materialize_cleanup_trap_projections(
        context, storage, original_block_count, block_capacity, &output_block_index,
        &cleanup_trap_projection_count, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    if (cleanup_trap_projection_count > UINT32_MAX - output->block_count)
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    output->block_count += cleanup_trap_projection_count;
    if (output->coroutine_safepoint_count != 0u) {
        for (uint32_t safepoint = 0u; safepoint < output->coroutine_safepoint_count; ++safepoint) {
            const XiCoroSuspendPoint *point = coroutine_point_for_safepoint(storage, safepoint);
            if (!point)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi function %u has no exact safepoint owner", function_index);
            const XrXiCancelEdge *edge = find_cancel_edge(context, xi, point);
            XrXiBlockStorage *handler = edge ? find_block_storage(storage, edge->handler) : NULL;
            if (edge && !edge->private_projection)
                continue;
            XrXiCancelGraphStorage *cancel = &storage->cancel_graphs[safepoint];
            if (edge) {
                if (!handler)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi shared cancellation cleanup has no Xi source block");
                status = clone_cancel_cleanup_graph(context, storage, handler, edge->registration,
                                                    point, safepoint, original_block_count,
                                                    diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
            if (!cancel->blocks || cancel->block_count == 0u)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi function %u has no exact cancellation cleanup", function_index);
            for (uint32_t cancel_block = 0u; cancel_block < cancel->block_count; ++cancel_block) {
                if (output_block_index >= block_capacity)
                    return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
                storage->blocks[output_block_index++] = cancel->blocks[cancel_block];
                for (uint32_t instruction = 0u;
                     instruction < cancel->blocks[cancel_block].instruction_count; ++instruction) {
                    const XrCoreOperationSpec *operation = xr_core_spec_operation_by_id(
                        cancel->blocks[cancel_block].instructions[instruction].operation_id);
                    if (!operation)
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "canonical cancellation cleanup has no CoreSpec operation");
                    storage->local_effect_mask |= operation->effect_mask;
                    storage->local_capability_mask |= operation->capability_mask;
                }
            }
        }
    }
    if (output_block_index != output->block_count)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi function %u canonical block count changed during translation",
                    function_index);
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus close_effects(XrXiBuildContext *context, char *diagnostic,
                                          size_t diagnostic_size) {
    uint32_t total_functions = 0u;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        if (module->function_count > UINT32_MAX - total_functions)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi call-effect graph is too large");
        total_functions += module->function_count;
        for (uint32_t function = 0; function < module->function_count; ++function) {
            module->functions[function].effect_mask =
                module->function_storage[function].local_effect_mask;
            module->functions[function].capability_mask =
                module->function_storage[function].local_capability_mask;
        }
    }
    for (uint32_t iteration = 0; iteration <= total_functions; ++iteration) {
        bool changed = false;
        for (uint32_t module_index = 0; module_index < context->source->module_count;
             ++module_index) {
            XrXiModuleStorage *module = &context->storage[module_index];
            for (uint32_t function_index = 0; function_index < module->function_count;
                 ++function_index) {
                XrXiFunctionStorage *function = &module->function_storage[function_index];
                uint32_t effects = module->functions[function_index].effect_mask;
                uint32_t capabilities = module->functions[function_index].capability_mask;
                for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
                    const XiBlock *block = function->xi->blocks[block_index];
                    if (!canonical_block_is_reachable(context, function->xi, block) ||
                        block_is_elided_class_construction_error_continuation(context, function->xi,
                                                                              block))
                        continue;
                    for (uint32_t value_index = 0; value_index < block->nvalues; ++value_index) {
                        const XiValue *value = block->values[value_index];
                        bool witness =
                            (value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT) &&
                            (value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_DIRECT ||
                             value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE);
                        bool sealed_method =
                            (value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT) &&
                            value->xg_existential_kind == XI_EXISTENTIAL_NONE;
                        if (value->op != XI_CALL && !witness && !sealed_method)
                            continue;
                        if (resolved_canonical_class_construction(context, function->xi, value,
                                                                  NULL, NULL) ||
                            resolved_empty_struct_literal(context, function->xi, value) ||
                            resolved_provider_native_call(context, function->xi, value) ||
                            resolved_suspension_native_call(context, function->xi, value))
                            continue;
                        const XiFunc *callee = resolved_sealed_callee(context, function->xi, value);
                        bool invoke =
                            typed_invoke_check_block_for_call(context, function->xi, value) != NULL;
                        if (witness) {
                            uint32_t witness_effects = 0u;
                            uint32_t witness_capabilities = 0u;
                            if (!witness_call_effect_contract(context, function->xi, value, true,
                                                              &witness_effects,
                                                              &witness_capabilities))
                                return fail(diagnostic, diagnostic_size,
                                            XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                            "Xi effect closure has an unresolved witness slot");
                            if (invoke)
                                witness_effects &= ~XR_CORE_EFFECT_ERROR;
                            effects |= witness_effects;
                            capabilities |= witness_capabilities;
                            continue;
                        }
                        if (callee) {
                            uint32_t callee_module = 0;
                            uint32_t callee_function = 0;
                            if (!find_xi_function(context, callee, &callee_module,
                                                  &callee_function))
                                return fail(diagnostic, diagnostic_size,
                                            XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                            "Xi effect closure has an unresolved direct target");
                            uint32_t callee_effects = context->storage[callee_module]
                                                          .functions[callee_function]
                                                          .effect_mask;
                            if (invoke)
                                callee_effects &= ~XR_CORE_EFFECT_ERROR;
                            effects |= callee_effects;
                            capabilities |= context->storage[callee_module]
                                                .functions[callee_function]
                                                .capability_mask;
                            continue;
                        }
                        if (sealed_method)
                            return fail(diagnostic, diagnostic_size,
                                        XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                        "Xi function %s call v%u has an unresolved method effect "
                                        "target",
                                        function->xi && function->xi->name ? function->xi->name
                                                                           : "<anonymous>",
                                        value->id);
                        XrXiCallableTargetSet target_set = {0};
                        if (!resolved_callable_call_targets(context, function->xi, value,
                                                            &target_set))
                            return fail(diagnostic, diagnostic_size,
                                        XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                        "Xi effect closure has an unresolved callable target set");
                        for (uint32_t target_index = 0u; target_index < target_set.target_count;
                             ++target_index) {
                            const XiFunc *target = find_xi_function_by_xg_id(
                                context, target_set.targets[target_index].target_func_id);
                            uint32_t target_module = 0u;
                            uint32_t target_function = 0u;
                            if (!target || !find_xi_function(context, target, &target_module,
                                                             &target_function))
                                return fail(diagnostic, diagnostic_size,
                                            XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                            "Xi emitted callable target is outside the canonical "
                                            "graph");
                            uint32_t target_effects = context->storage[target_module]
                                                          .functions[target_function]
                                                          .effect_mask;
                            if (invoke)
                                target_effects &= ~XR_CORE_EFFECT_ERROR;
                            effects |= target_effects;
                            capabilities |= context->storage[target_module]
                                                .functions[target_function]
                                                .capability_mask;
                        }
                    }
                }
                if (effects != module->functions[function_index].effect_mask ||
                    capabilities != module->functions[function_index].capability_mask) {
                    module->functions[function_index].effect_mask = effects;
                    module->functions[function_index].capability_mask = capabilities;
                    changed = true;
                }
            }
        }
        if (!changed) {
            for (uint32_t module_index = 0; module_index < context->source->module_count;
                 ++module_index) {
                XrXiModuleStorage *module = &context->storage[module_index];
                for (uint32_t function_index = 0; function_index < module->function_count;
                     ++function_index) {
                    XrXiFunctionStorage *storage = &module->function_storage[function_index];
                    if (!storage->closed_contract_ready ||
                        module->functions[function_index].effect_mask !=
                            storage->closed_effect_mask ||
                        module->functions[function_index].capability_mask !=
                            storage->closed_capability_mask)
                        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                    "Xi precomputed and emitted function contracts disagree");
                }
            }
            return XR_PROGRAM_BUILD_OK;
        }
    }
    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "Xi call-effect closure did not converge");
}

static uint32_t input_value_occurrences(const XrCoreIrFunctionInput *function, XrCoreIrKey key) {
    uint32_t occurrences = 0;
    for (uint32_t block = 0; block < function->block_count; ++block) {
        const XrCoreIrBlockInput *row = &function->blocks[block];
        for (uint32_t argument = 0; argument < row->argument_count; ++argument)
            occurrences += xr_core_ir_key_equal(row->arguments[argument].key, key);
        for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
            XrCoreIrKey result = row->instructions[instruction].result;
            occurrences += !xr_core_ir_key_is_zero(result) && xr_core_ir_key_equal(result, key);
        }
    }
    return occurrences;
}

static XrProgramBuildStatus validate_input_value_identities(const XrXiBuildContext *context,
                                                            char *diagnostic,
                                                            size_t diagnostic_size) {
    for (uint32_t module = 0; module < context->source->module_count; ++module) {
        const XrCoreIrModuleInput *module_row = &context->modules[module];
        for (uint32_t function = 0; function < module_row->function_count; ++function) {
            const XrCoreIrFunctionInput *function_row = &module_row->functions[function];
            for (uint32_t block = 0; block < function_row->block_count; ++block) {
                const XrCoreIrBlockInput *block_row = &function_row->blocks[block];
                for (uint32_t argument = 0; argument < block_row->argument_count; ++argument) {
                    XrCoreIrKey key = block_row->arguments[argument].key;
                    uint32_t occurrences = input_value_occurrences(function_row, key);
                    if (occurrences != 1u)
                        return fail(diagnostic, diagnostic_size,
                                    XR_PROGRAM_BUILD_DUPLICATE_IDENTITY,
                                    "Xi function %u block %u argument %u has %u definitions",
                                    function, block, argument, occurrences);
                }
            }
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus finalize_type_inputs(XrXiBuildContext *context) {
    if (context->type_count == 0u)
        return XR_PROGRAM_BUILD_OK;
    context->types = xr_calloc(context->type_count, sizeof(*context->types));
    if (!context->types)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    for (uint32_t type = 0; type < context->type_count; ++type)
        context->types[type] = context->type_storage[type].input;
    return XR_PROGRAM_BUILD_OK;
}

static bool count_nested_functions(const XiFunc *function, uint32_t *count) {
    if (!function || !count)
        return false;
    for (uint16_t child = 0; child < function->nchildren; ++child) {
        if (*count == UINT32_MAX)
            return false;
        ++*count;
        if (!count_nested_functions(function->children[child], count))
            return false;
    }
    return true;
}

static bool append_nested_functions(const XiFunc *function, const XiFunc **functions,
                                    uint32_t capacity, uint32_t *cursor) {
    if (!function || !functions || !cursor)
        return false;
    for (uint16_t child = 0; child < function->nchildren; ++child) {
        if (*cursor >= capacity || !function->children[child])
            return false;
        functions[(*cursor)++] = function->children[child];
        if (!append_nested_functions(function->children[child], functions, capacity, cursor))
            return false;
    }
    return true;
}

static XrProgramBuildStatus collect_module_functions(XrXiModuleStorage *storage,
                                                     bool include_initializer, char *diagnostic,
                                                     size_t diagnostic_size) {
    const XiModule *module = storage && storage->root ? storage->root->module : NULL;
    if (!module || module->init != storage->root || (module->nfuncs != 0u && !module->functions))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi module has no exact initializer function tree");
    uint32_t count = include_initializer ? 1u : module->nfuncs;
    if (include_initializer) {
        if (!count_nested_functions(storage->root, &count))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi module function tree is malformed or too large");
    } else {
        for (uint16_t function = 0u; function < module->nfuncs; ++function)
            if (!module->functions[function] ||
                !count_nested_functions(module->functions[function], &count))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi module function tree is malformed or too large");
    }
    const XiFunc **functions = xr_calloc(count, sizeof(*functions));
    if (!functions)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t cursor = 0u;
    if (include_initializer) {
        functions[cursor++] = storage->root;
        if (!append_nested_functions(storage->root, functions, count, &cursor)) {
            xr_free(functions);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi module function tree cannot be flattened");
        }
    } else {
        for (uint16_t function = 0u; function < module->nfuncs; ++function)
            functions[cursor++] = module->functions[function];
        for (uint16_t function = 0u; function < module->nfuncs; ++function) {
            if (!append_nested_functions(module->functions[function], functions, count, &cursor)) {
                xr_free(functions);
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi module function tree cannot be flattened");
            }
        }
    }
    if (cursor != count) {
        xr_free(functions);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi module function count is inconsistent");
    }
    storage->xi_functions = functions;
    storage->function_count = count;
    return XR_PROGRAM_BUILD_OK;
}

/* An executable Program owns one entry/build-target closure.  Importing a
 * source module grants
 * access to its exports; it does not re-export every
 * declaration from that module through the
 * executable artifact.  Close the
 * function roots from Xglobal's exact target identities before
 * assigning
 * Program-local FunctionIds.  Unknown dynamic targets are deliberately not
 * guessed
 * here: the existing callable validation rejects them after the
 * closure has been materialized.
 */
static bool program_function_is_open_generic(const XrXiBuildContext *context,
                                             const XiFunc *function) {
    const XgGlobalEvidence *evidence =
        context && context->source ? context->source->global_evidence : NULL;
    if (!evidence || !function || function->xg_body_func_id == XG_NO_ID)
        return false;
    for (uint32_t body_index = 0u; body_index < evidence->nbodies; ++body_index) {
        const XgBodySummary *body = &evidence->bodies[body_index];
        if (body->func_id != function->xg_body_func_id || body->owner_decl_id == XG_NO_ID)
            continue;
        for (uint32_t decl_index = 0u; decl_index < evidence->ndecls; ++decl_index)
            if (evidence->decls[decl_index].decl_id == body->owner_decl_id &&
                (evidence->decls[decl_index].flags & XG_DECL_GENERIC_TEMPLATE) != 0u)
                return true;
    }
    return false;
}

static bool mark_program_function(XrXiBuildContext *context, const XiFunc *function,
                                  bool *changed) {
    /* An open generic body has no executable logical type or representation.
     *
     * Monomorphization must select a concrete clone before XrProgram closure;
     * retaining the
     * template would create an erased fallback path. */
    if (!context || !function || function->is_generic_template ||
        program_function_is_open_generic(context, function))
        return false;
    for (uint32_t module = 0u; module < context->source->module_count; ++module) {
        XrXiModuleStorage *storage = &context->storage[module];
        for (uint32_t index = 0u; index < storage->function_count; ++index) {
            if (storage->xi_functions[index] != function)
                continue;
            if (!storage->function_reachable[index]) {
                storage->function_reachable[index] = true;
                if (changed)
                    *changed = true;
            }
            return true;
        }
    }
    return false;
}

static bool mark_program_function_id(XrXiBuildContext *context, XgFuncId function_id,
                                     bool *changed) {
    if (!context || function_id == XG_NO_ID)
        return false;
    for (uint32_t module = 0u; module < context->source->module_count; ++module) {
        XrXiModuleStorage *storage = &context->storage[module];
        for (uint32_t index = 0u; index < storage->function_count; ++index) {
            const XiFunc *function = storage->xi_functions[index];
            if (function && function->xg_body_func_id == function_id)
                return mark_program_function(context, function, changed);
        }
    }
    return false;
}

static void mark_program_method_targets(XrXiBuildContext *context,
                                        const XgCallsiteSummary *callsite, bool *changed) {
    const XgGlobalEvidence *evidence = context ? context->source->global_evidence : NULL;
    if (!evidence || !callsite || callsite->method_id == XG_NO_ID)
        return;
    for (uint32_t method_index = 0u; method_index < evidence->nmethods; ++method_index) {
        const XgMethodSummary *method = &evidence->methods[method_index];
        if (method->method_id != callsite->method_id &&
            method->root_method_id != callsite->method_id)
            continue;
        for (uint32_t body_index = 0u; body_index < evidence->nbodies; ++body_index) {
            const XgBodySummary *body = &evidence->bodies[body_index];
            if (body->kind == XG_BODY_METHOD && body->owner_method_id == method->method_id)
                (void) mark_program_function_id(context, body->func_id, changed);
        }
    }
}

static void mark_program_interface_targets(XrXiBuildContext *context,
                                           const XgCallsiteSummary *callsite, bool *changed) {
    const XgGlobalEvidence *evidence = context ? context->source->global_evidence : NULL;
    if (!evidence || !callsite || callsite->receiver_static_interface_id == XG_NO_ID ||
        callsite->method_id == XG_NO_ID)
        return;
    for (uint32_t witness = 0u; witness < evidence->ninterface_witnesses; ++witness) {
        const XgInterfaceWitnessSummary *row = &evidence->interface_witnesses[witness];
        if (row->interface_id == callsite->receiver_static_interface_id &&
            row->interface_method_id == (XgInterfaceMethodId) callsite->method_id && row->complete)
            (void) mark_program_function_id(context, row->implementation_func_id, changed);
    }
}

static void mark_program_conformance_targets(XrXiBuildContext *context,
                                             XgInterfaceConformanceId conformance_id,
                                             bool *changed) {
    const XgGlobalEvidence *evidence = context ? context->source->global_evidence : NULL;
    if (!evidence || conformance_id == XG_NO_ID)
        return;
    for (uint32_t witness = 0u; witness < evidence->ninterface_witnesses; ++witness) {
        const XgInterfaceWitnessSummary *row = &evidence->interface_witnesses[witness];
        if (row->conformance_id == conformance_id && row->complete)
            (void) mark_program_function_id(context, row->implementation_func_id, changed);
    }
}

static void mark_program_callsite_targets(XrXiBuildContext *context,
                                          const XgCallsiteSummary *callsite, bool *changed) {
    const XgGlobalEvidence *evidence = context ? context->source->global_evidence : NULL;
    if (!evidence || !callsite)
        return;
    if (callsite->static_target_func_id != XG_NO_ID)
        (void) mark_program_function_id(context, callsite->static_target_func_id, changed);
    if (callsite->kind == XG_CALL_METHOD)
        mark_program_method_targets(context, callsite, changed);
    if (callsite->kind == XG_CALL_INTERFACE)
        mark_program_interface_targets(context, callsite, changed);
    if (callsite->kind == XG_CALL_CLOSURE) {
        const XgCallableTargetSummary *targets = NULL;
        uint32_t target_count = 0u;
        if (xg_global_evidence_callable_targets(evidence, callsite, &targets, &target_count))
            for (uint32_t target = 0u; target < target_count; ++target)
                (void) mark_program_function_id(context, targets[target].target_func_id, changed);
    }
    for (uint32_t instance = 0u; instance < evidence->ngeneric_insts; ++instance) {
        const XgGenericInstSummary *generic = &evidence->generic_insts[instance];
        if (generic->root_callsite_id == callsite->callsite_id &&
            generic->specialized_func_id != XG_NO_ID)
            (void) mark_program_function_id(context, generic->specialized_func_id, changed);
    }
}

static void mark_program_value_targets(XrXiBuildContext *context, const XiFunc *function,
                                       const XiValue *value, bool *changed) {
    if (!context || !function || !value)
        return;
    if (value->xg_callsite_id != XG_NO_ID &&
        !resolved_canonical_class_construction(context, function, value, NULL, NULL)) {
        const XgCallsiteSummary *callsite = xg_global_evidence_find_callsite(
            context->source->global_evidence, (XgCallsiteId) value->xg_callsite_id);
        if (callsite && callsite->owner_func_id == function->xg_body_func_id)
            mark_program_callsite_targets(context, callsite, changed);
    }
    if (value->op == XI_CLOSURE_NEW) {
        const XiFunc *target = resolved_callable_target(function, value);
        if (target)
            (void) mark_program_function(context, target, changed);
    }
    if (value->op == XI_GET_SHARED && value->aux_int >= 0 && function->module) {
        uint32_t slot = (uint32_t) value->aux_int;
        if (slot < function->module->nslots) {
            if (function->module->slot_funcs && function->module->slot_funcs[slot])
                (void) mark_program_function(context, function->module->slot_funcs[slot], changed);
            const XiImportRef *reference =
                function->module->slot_imports ? function->module->slot_imports[slot] : NULL;
            if (reference && reference->resolution_attempted && reference->resolved_func)
                (void) mark_program_function(context, reference->resolved_func, changed);
        }
    }
    if (value->xg_conformance_id != XG_NO_ID)
        mark_program_conformance_targets(context, value->xg_conformance_id, changed);
}

static XrProgramBuildStatus retain_program_function_closure(XrXiBuildContext *context,
                                                            char *diagnostic,
                                                            size_t diagnostic_size) {
    if (!context || !context->source || !context->source->global_evidence)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint32_t total = 0u;
    for (uint32_t module = 0u; module < context->source->module_count; ++module) {
        XrXiModuleStorage *storage = &context->storage[module];
        if (storage->function_count > UINT32_MAX - total)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        total += storage->function_count;
        storage->function_reachable = xr_calloc(storage->function_count, sizeof(bool));
        if (storage->function_count != 0u && !storage->function_reachable)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    bool changed = false;
    if (!mark_program_function(context, context->source->entry_function, &changed))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "canonical-program entry is not a source function in the input graph");
    for (uint32_t iteration = 0u; changed && iteration <= total; ++iteration) {
        changed = false;
        for (uint32_t module = 0u; module < context->source->module_count; ++module) {
            XrXiModuleStorage *storage = &context->storage[module];
            for (uint32_t function_index = 0u; function_index < storage->function_count;
                 ++function_index) {
                const XiFunc *function = storage->xi_functions[function_index];
                if (!storage->function_reachable[function_index] || !function)
                    continue;
                for (uint32_t block = 0u; block < function->nblocks; ++block) {
                    const XiBlock *row = function->blocks[block];
                    for (uint32_t value = 0u; row && value < row->nvalues; ++value)
                        mark_program_value_targets(context, function, row->values[value], &changed);
                }
            }
        }
    }
    if (changed)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "canonical-program function reachability did not converge");

    for (uint32_t module = 0u; module < context->source->module_count; ++module) {
        XrXiModuleStorage *storage = &context->storage[module];
        uint32_t retained_count = 0u;
        for (uint32_t index = 0u; index < storage->function_count; ++index)
            retained_count += storage->function_reachable[index] ? 1u : 0u;
        const XiFunc **retained =
            retained_count ? xr_calloc(retained_count, sizeof(*retained)) : NULL;
        if (retained_count != 0u && !retained)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        uint32_t cursor = 0u;
        for (uint32_t index = 0u; index < storage->function_count; ++index)
            if (storage->function_reachable[index])
                retained[cursor++] = storage->xi_functions[index];
        xr_free(storage->xi_functions);
        xr_free(storage->function_reachable);
        storage->xi_functions = retained;
        storage->function_reachable = NULL;
        storage->function_count = retained_count;
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus build_context(XrXiBuildContext *context, char *diagnostic,
                                          size_t diagnostic_size) {
    context->modules = xr_calloc(context->source->module_count, sizeof(*context->modules));
    context->storage = xr_calloc(context->source->module_count, sizeof(*context->storage));
    if (!context->modules || !context->storage)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;

    /* Collect the complete compiler graph first so source-local and imported
     * target
     * identities can close the entry-scoped function set independent
     * of module order. */
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        const XiFunc *root = context->source->module_roots[module_index];
        XrXiModuleStorage *storage = &context->storage[module_index];
        XrCoreIrModuleInput *output = &context->modules[module_index];
        storage->root = root;
        if (!root || root->stage != XI_STAGE_OPTIMIZED || root->semantic_plan || !root->module ||
            root->module->init != root || !root->module->source_semantic_module_present)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi module %u is not verified canonical-program input", module_index);
        storage->source_authority = &root->module->source_semantic_module;
        XrProgramBuildStatus collect_status = collect_module_functions(
            storage, root == context->source->entry_function, diagnostic, diagnostic_size);
        if (collect_status != XR_PROGRAM_BUILD_OK)
            return collect_status;
        output->key = key_from_stable_id(UINT8_C(0x4d), storage->source_authority->module_identity);
    }

    XrProgramBuildStatus closure_status =
        retain_program_function_closure(context, diagnostic, diagnostic_size);
    if (closure_status != XR_PROGRAM_BUILD_OK)
        return closure_status;

    /* Publish every retained function identity before translating a body.
     * This makes forward
     * and cross-module calls independent of input order. */
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *storage = &context->storage[module_index];
        XrCoreIrModuleInput *output = &context->modules[module_index];
        storage->functions = storage->function_count
                                 ? xr_calloc(storage->function_count, sizeof(*storage->functions))
                                 : NULL;
        storage->function_storage =
            storage->function_count
                ? xr_calloc(storage->function_count, sizeof(*storage->function_storage))
                : NULL;
        if (storage->function_count != 0u && (!storage->functions || !storage->function_storage))
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        output->functions = storage->functions;
        output->function_count = storage->function_count;
        for (uint32_t function = 0; function < storage->function_count; ++function) {
            storage->function_storage[function].xi = storage->xi_functions[function];
            storage->function_storage[function].key =
                function_key(storage->source_authority->module_identity, function);
        }
    }

    XrProgramBuildStatus reachability_status =
        initialize_canonical_reachability(context, diagnostic, diagnostic_size);
    if (reachability_status != XR_PROGRAM_BUILD_OK)
        return reachability_status;
    XrProgramBuildStatus cleanup_status =
        prepare_cleanup_control_flow(context, diagnostic, diagnostic_size);
    if (cleanup_status != XR_PROGRAM_BUILD_OK)
        return cleanup_status;
    XrProgramBuildStatus trap_status =
        prepare_trap_continuations(context, diagnostic, diagnostic_size);
    if (trap_status != XR_PROGRAM_BUILD_OK)
        return trap_status;
    XrProgramBuildStatus panic_status =
        prepare_panic_continuations(context, diagnostic, diagnostic_size);
    if (panic_status != XR_PROGRAM_BUILD_OK)
        return panic_status;
    XrProgramBuildStatus cancel_status =
        prepare_cancel_continuations(context, diagnostic, diagnostic_size);
    if (cancel_status != XR_PROGRAM_BUILD_OK)
        return cancel_status;

    XrProgramBuildStatus imported_callable_status =
        validate_imported_callable_bindings(context, diagnostic, diagnostic_size);
    if (imported_callable_status != XR_PROGRAM_BUILD_OK)
        return imported_callable_status;

    XrProgramBuildStatus callable_callsite_status =
        validate_callable_callsite_bindings(context, diagnostic, diagnostic_size);
    if (callable_callsite_status != XR_PROGRAM_BUILD_OK)
        return callable_callsite_status;

    XrProgramBuildStatus contract_status =
        precompute_function_contracts(context, diagnostic, diagnostic_size);
    if (contract_status != XR_PROGRAM_BUILD_OK)
        return contract_status;
    XrProgramBuildStatus existential_status =
        prepare_existential_contracts(context, diagnostic, diagnostic_size);
    if (existential_status != XR_PROGRAM_BUILD_OK)
        return existential_status;
    XrProgramBuildStatus catch_reachability_status =
        refine_static_typed_catch_reachability(context, diagnostic, diagnostic_size);
    if (catch_reachability_status != XR_PROGRAM_BUILD_OK)
        return catch_reachability_status;
    contract_status = precompute_function_contracts(context, diagnostic, diagnostic_size);
    if (contract_status != XR_PROGRAM_BUILD_OK)
        return contract_status;

    if (!find_xi_function(context, context->source->entry_function, NULL, NULL))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "canonical-program entry is not a source function in the input graph");

    uint32_t entry_count = 0;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *storage = &context->storage[module_index];
        for (uint32_t function = 0; function < storage->function_count; ++function) {
            XrProgramBuildStatus status =
                prepare_function_signature(context, storage, function, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            entry_count += (storage->functions[function].flags & XR_PROGRAM_FUNCTION_ENTRY) != 0u;
        }
    }
    if (entry_count != 1u)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi input must identify exactly one program entry");

    /* Publish every canonical function signature before translating any body.
     * Ownership closure for a forward or cross-module direct call must consume
     * the callee's exact parameter modes independently of source order. */
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *storage = &context->storage[module_index];
        XrCoreIrModuleInput *output = &context->modules[module_index];
        for (uint32_t function = 0; function < storage->function_count; ++function) {
            XrProgramBuildStatus status =
                build_function_body(context, storage, function, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        output->constants = storage->constants;
        output->constant_count = storage->constant_count;
    }
    XrProgramBuildStatus status = close_effects(context, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        context->modules[module_index].constants = context->storage[module_index].constants;
        context->modules[module_index].constant_count =
            context->storage[module_index].constant_count;
    }
    status = finalize_type_inputs(context);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    return validate_input_value_identities(context, diagnostic, diagnostic_size);
}

XrProgramBuildStatus xr_program_write_from_xi(const XrProgramFromXiInput *input,
                                              XrProgramArtifact *artifact_out, char *diagnostic,
                                              size_t diagnostic_size) {
    if (artifact_out)
        memset(artifact_out, 0, sizeof(*artifact_out));
    if (diagnostic && diagnostic_size != 0)
        diagnostic[0] = '\0';
    if (!input || !artifact_out || !input->module_roots || input->module_count == 0u ||
        !input->entry_function || !input->global_evidence || !input->semantic_profile_fingerprint)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi program producer input is incomplete");
    XrXiBuildContext context = {.source = input};
    XrProgramBuildStatus status = build_context(&context, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK && (!diagnostic || diagnostic_size == 0u || !diagnostic[0]))
        (void) fail(diagnostic, diagnostic_size, status,
                    "Xi canonical context construction failed: %s",
                    xr_program_build_status_name(status));
    XrCoreIrProgram *program = NULL;
    if (status == XR_PROGRAM_BUILD_OK) {
        uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
        XrCoreIrProgramInput core_input = {
            .semantic_profile_fingerprint = input->semantic_profile_fingerprint,
            .required_features = &feature,
            .required_feature_count = 1u,
            .types = context.types,
            .type_count = context.type_count,
            .interfaces = context.interface_count ? context.interfaces : NULL,
            .interface_count = context.interface_count,
            .conformances = context.conformance_count ? context.conformances : NULL,
            .conformance_count = context.conformance_count,
            .provider_requirements =
                context.provider_requirement_count ? context.provider_requirements : NULL,
            .provider_requirement_count = context.provider_requirement_count,
            .modules = context.modules,
            .module_count = input->module_count,
        };
        status = xr_core_ir_program_build(&core_input, &program, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK &&
            (!diagnostic || diagnostic_size == 0u || !diagnostic[0]))
            (void) fail(diagnostic, diagnostic_size, status,
                        "Xi canonical Core IR construction failed: %s",
                        xr_program_build_status_name(status));
    }
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact_out, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK && (!diagnostic || diagnostic_size == 0u || !diagnostic[0]))
        (void) fail(diagnostic, diagnostic_size, status,
                    "Xi canonical Program serialization failed: %s",
                    xr_program_build_status_name(status));
    if (status == XR_PROGRAM_BUILD_OK) {
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic verify_diagnostic;
        XrProgramVerifyStatus verify = xr_program_validate(artifact_out->bytes, artifact_out->size,
                                                           NULL, &validated, &verify_diagnostic);
        xr_validated_program_free(validated);
        if (verify != XR_PROGRAM_VERIFY_OK) {
            const XrCoreIrFunction *failed_function = NULL;
            const XrCoreIrInstruction *failed_instruction = NULL;
            uint32_t flat_function = 0u;
            for (uint32_t module = 0u;
                 program && module < program->module_count && !failed_instruction; ++module) {
                const XrCoreIrModule *module_row = &program->modules[module];
                for (uint32_t function = 0u; function < module_row->function_count;
                     ++function, ++flat_function) {
                    if (flat_function != verify_diagnostic.location.function_id)
                        continue;
                    const XrCoreIrFunction *function_row = &module_row->functions[function];
                    failed_function = function_row;
                    if (verify_diagnostic.location.block_id < function_row->block_count) {
                        const XrCoreIrBlock *block =
                            &function_row->blocks[verify_diagnostic.location.block_id];
                        if (verify_diagnostic.location.instruction_id < block->instruction_count)
                            failed_instruction =
                                &block->instructions[verify_diagnostic.location.instruction_id];
                    }
                    break;
                }
            }
            xr_program_artifact_free(artifact_out);
            status = fail(
                diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "Xi-produced XrProgram failed semantic verification: %s/%s "
                "at function=%u block=%u instruction=%u value=%u operation=%u "
                "result_type=%u category=%u ownership=%u result_zero=%u "
                "function_effects=%u function_capabilities=%u states=%u safepoints=%u",
                xr_program_verify_status_name(verify),
                xr_program_diagnostic_kind_name(verify_diagnostic.kind),
                verify_diagnostic.location.function_id, verify_diagnostic.location.block_id,
                verify_diagnostic.location.instruction_id, verify_diagnostic.location.value_id,
                failed_instruction ? failed_instruction->operation_id : 0u,
                failed_instruction ? failed_instruction->result_type_id : 0u,
                failed_instruction ? failed_instruction->result_category : 0u,
                failed_instruction ? failed_instruction->result_ownership : 0u,
                failed_instruction ? (unsigned) xr_core_ir_key_is_zero(failed_instruction->result)
                                   : 1u,
                failed_function ? failed_function->effect_mask : 0u,
                failed_function ? failed_function->capability_mask : 0u,
                failed_function ? failed_function->coroutine_state_count : 0u,
                failed_function ? failed_function->coroutine_safepoint_count : 0u);
        }
    }
    xr_core_ir_program_free(program);
    free_context(&context);
    return status;
}
