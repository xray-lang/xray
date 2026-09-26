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
#include "../ir/xi_core_api.h"
#include "../ir/xi_module.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/class/xenum.h"
#include "../runtime/value/xenum_layout.h"
#include "../runtime/value/xtype.h"
#include "../shared/xr_target_query_registry_gen.h"
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

enum {
    XR_XI_INVOKE_ARGUMENT_NONE = 0,
    XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT = 1,
    XR_XI_INVOKE_ARGUMENT_ERROR = 2,
};

enum {
    XR_XI_STATIC_BRANCH_UNKNOWN = 0,
    XR_XI_STATIC_BRANCH_TRUE = 1,
    XR_XI_STATIC_BRANCH_FALSE = 2,
};

typedef struct XrXiBlockStorage {
    const XiBlock *xi;
    bool reachable;
    XrXiBlockArgumentStorage *argument_storage;
    uint32_t argument_count;
    uint32_t argument_capacity;
    XrCoreIrValueInput *arguments;
    XrCoreIrInstructionInput *instructions;
    uint32_t instruction_count;
    uint8_t static_branch_outcome;
} XrXiBlockStorage;

typedef struct XrXiFunctionStorage {
    const XiFunc *xi;
    XrCoreIrKey key;
    XiValue capture_receiver;
    uint16_t capture_type_id;
    uint16_t *parameter_types;
    XrParamMode *parameter_modes;
    XrCoreIrBlockInput *blocks;
    XrXiBlockStorage *block_storage;
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
        case XR_KIND_FLOAT:
            if (type->scalar_rep == XR_NATIVE_F64) {
                *type_id = XR_CORE_TYPE_F64;
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
            if (type->instance.class_name && strcmp(type->instance.class_name, "Atomic") == 0) {
                const XrClassInfo *atomic = type->instance.class_ref;
                if (!atomic || !atomic->declaration_symbol ||
                    !atomic->declaration_symbol->is_builtin || type->instance.type_arg_count != 1 ||
                    !type->instance.type_args || !type->instance.type_args[0] ||
                    type->instance.type_args[0]->is_nullable)
                    return false;
                const XrType *element = type->instance.type_args[0];
                if (element->kind == XR_KIND_INT && element->scalar_rep == XR_NATIVE_I64)
                    *type_id = XR_CORE_TYPE_ATOMIC_I64;
                else if (element->kind == XR_KIND_BOOL)
                    *type_id = XR_CORE_TYPE_ATOMIC_BOOL;
                else if (element->kind == XR_KIND_FLOAT && element->scalar_rep == XR_NATIVE_F64)
                    *type_id = XR_CORE_TYPE_ATOMIC_F64;
                else
                    return false;
                return true;
            }
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
    if (!context || !type || !type_id || type->kind != XR_KIND_FUNCTION || type->is_nullable ||
        type->function.param_count < 0 || type->function.param_count > UINT16_MAX ||
        (type->function.param_count != 0 && !type->function.params) ||
        !type->function.return_type || type->function.is_variadic || type->function.is_c_abi ||
        type->function.type_param_count != 0 || type->function.view_origin_count != 0 ||
        depth >= 64u || type_stack_contains(stack, depth, type) ||
        (error_type_id == XR_CORE_TYPE_VOID &&
         type->function.throw_effect != XR_FN_EFFECT_NO_THROW) ||
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

static const XiFunc *resolved_direct_callee(const XrXiBuildContext *context, const XiFunc *caller,
                                            const XiValue *call);

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
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (!canonical_block_is_reachable(context, function, block))
            continue;
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *value = block->values[value_index];
            if (!value)
                continue;
            if (value->op == XI_TRY || value->op == XI_CATCH || value->op == XI_END_TRY)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi function %s still uses implicit panic-handler state",
                            function->name ? function->name : "<anonymous>");
            if (value->op == XI_THROW) {
                uint16_t payload_type = XR_CORE_TYPE_VOID;
                if (value->nargs != 1u || !value->args || !value->args[0] ||
                    !map_type(context, value->args[0]->type, &payload_type) ||
                    payload_type != XR_CORE_TYPE_PANIC_INFO)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi throw v%u does not publish typed PanicInfo", value->id);
                found = true;
            }
            if (value->op == XI_CALL) {
                const XiFunc *callee = resolved_direct_callee(context, function, value);
                if (!callee)
                    continue;
                bool callee_panic = false;
                XrProgramBuildStatus status =
                    function_has_uncaught_panic(context, callee, stack, depth + 1u, capacity,
                                                &callee_panic, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
                found = found || callee_panic;
            }
        }
    }
    *has_panic = found;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus map_function_panic_type(XrXiBuildContext *context,
                                                    const XiFunc *function, uint16_t *panic_type_id,
                                                    char *diagnostic, size_t diagnostic_size) {
    if (!context || !function || !panic_type_id)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint32_t capacity = 1u;
    for (uint32_t module = 0u; module < context->source->module_count; ++module) {
        const XiFunc *root = context->source->module_roots[module];
        if (root && root->module)
            capacity += root->module->nfuncs;
    }
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

static bool map_callable_target_type(XrXiBuildContext *context, const XrType *type,
                                     const XiFunc *target, uint16_t *type_id);

static const XiFunc *resolved_direct_callee(const XrXiBuildContext *context, const XiFunc *caller,
                                            const XiValue *call);
static const XiValue *logical_value_identity(const XiValue *value);
static const XiClassData *resolved_empty_class_allocation(const XrXiBuildContext *context,
                                                          const XiFunc *caller,
                                                          const XiValue *call);
static const XiClassData *resolved_empty_struct_literal(const XrXiBuildContext *context,
                                                        const XiFunc *caller, const XiValue *call);
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
        for (uint32_t value_index = 0; block && value_index < block->nvalues; ++value_index) {
            const XiValue *consumer = block->values[value_index];
            for (uint16_t argument = 0; consumer && argument < consumer->nargs; ++argument) {
                if (consumer->args[argument] != value)
                    continue;
                bool elided = false;
                if (argument == 0u &&
                    (consumer->op == XI_VARIANT_CONSTRUCT ||
                     (consumer->op == XI_CALL &&
                      (resolved_direct_callee(context, function, consumer) ||
                       resolved_empty_class_allocation(context, function, consumer))) ||
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
    return found;
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

static const XiFunc *resolved_direct_callee(const XrXiBuildContext *context, const XiFunc *caller,
                                            const XiValue *call) {
    if (!call || call->op != XI_CALL || call->nargs == 0u)
        return NULL;
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    if (!row || row->kind != XG_CALL_DIRECT_FUNC || row->static_target_func_id == XG_NO_ID)
        return NULL;
    return find_xi_function_by_xg_id(context, row->static_target_func_id);
}

/* A source `C()` with no declared constructor still uses the runtime shared
 * namespace carrier in Xi.  That carrier is phase-only in Program, but it may
 * disappear only when the Xi allocation marker, the caller module's exact
 * shared-slot class table, and the Xglobal class-allocation row all name the
 * same declaration.  Restrict this normalization to fieldless declarations;
 * defaults need a future explicit value graph and must not be guessed here. */
static const XiClassData *resolved_empty_class_allocation(const XrXiBuildContext *context,
                                                          const XiFunc *caller,
                                                          const XiValue *call) {
    if (!context || !caller || !call || call->op != XI_CALL || call->nargs != 1u || !call->args)
        return NULL;
    const XiValue *callee = logical_value_identity(call->args[0]);
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    if (!callee || callee->op != XI_GET_SHARED || callee->aux_int < 0 || !row ||
        row->kind != XG_CALL_CLASS_ALLOC || row->receiver_static_class_id == XG_NO_ID)
        return NULL;
    uint32_t module_index = UINT32_MAX;
    if (!find_xi_function(context, caller, &module_index, NULL) ||
        module_index >= context->source->module_count)
        return NULL;
    const XiFunc *root = context->source->module_roots[module_index];
    const XiModule *module = root ? root->module : NULL;
    uint32_t slot = (uint32_t) callee->aux_int;
    if (!module || slot >= module->nslots || !module->slot_classes)
        return NULL;
    const XiClassData *class_data = module->slot_classes[slot];
    if (!class_data || class_data->xg_class_id != row->receiver_static_class_id ||
        class_data->instance_field_count != 0u)
        return NULL;
    for (uint16_t method = 0u; method < class_data->nmethod; ++method)
        if (class_data->methods && class_data->methods[method].is_constructor &&
            !class_data->methods[method].is_static)
            return NULL;
    const XrClassInfo *info = nominal_info_for_type(call->type);
    const XgClassSummary *class_row =
        find_xg_class_by_id(context->source->global_evidence, row->receiver_static_class_id);
    if (!info || !class_row || info->xg_decl_id != class_row->decl_id ||
        (class_row->decl_kind != XG_DECL_CLASS && class_row->decl_kind != XG_DECL_STRUCT) ||
        !nominal_contract(context, call->type, NULL, NULL, NULL))
        return NULL;
    return class_data;
}

/* An empty struct literal may retain the generic constructor-shaped Xi form
 * when no runtime aggregate layout was needed.  Its constructor marker is
 * authoritative only after the shared slot, Xi class id, Xglobal declaration,
 * result nominal key agree.  Structs cannot declare constructors, so the
 * lowering marker identifies syntax rather than a dispatchable method. */
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
    if (!find_xi_function(context, caller, &module_index, NULL) ||
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
    if (!find_xi_function(context, caller, &module_index, NULL) ||
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
 * evidence classifies a constructor-shaped call.  Program may erase that CFG
 * edge only for an exact, fieldless class allocation and only when the error
 * continuation is the isolated mechanical catch/rethrow block created for the
 * same call.  Any cleanup or user-visible work outside that shape remains
 * unsupported instead of being silently discarded. */
static const XiValue *
exact_infallible_empty_class_allocation_in_block(const XrXiBuildContext *context,
                                                 const XiFunc *function, const XiBlock *block) {
    if (!context || !function || !block || block->kind != XI_BLOCK_IF || !block->control ||
        block->control->op != XI_ERR_CHECK || block->control->nargs != 0u || !block->succs[0] ||
        !block->succs[1] || block->succs[0] == block->succs[1])
        return NULL;

    const XiValue *call = xi_err_check_producer(function, block->control);
    if (!call || !resolved_empty_class_allocation(context, function, call))
        return NULL;

    const XiBlock *error = block->succs[0];
    if (error->kind != XI_BLOCK_RETURN || error->phis || error->npreds != 1u || !error->preds ||
        error->preds[0] != block || error->succs[0] || error->succs[1] || !error->control ||
        error->control->op != XI_ERR_RETURN || error->control->nargs != 1u ||
        !error->control->args || !error->control->args[0])
        return NULL;

    const XiValue *caught = NULL;
    for (uint32_t index = 0u; index < error->nvalues; ++index) {
        const XiValue *value = error->values[index];
        if (!value)
            return NULL;
        if (value->op == XI_ERR_CATCH) {
            if (caught || value->nargs != 0u)
                return NULL;
            caught = value;
            continue;
        }
        if (value == error->control)
            continue;
        if (value->op != XI_RELEASE || value->nargs != 1u || !value->args ||
            logical_value_identity(value->args[0]) != call)
            return NULL;
    }
    return caught && logical_value_identity(error->control->args[0]) == caught ? call : NULL;
}

static bool block_is_elided_empty_class_error_continuation(const XrXiBuildContext *context,
                                                           const XiFunc *function,
                                                           const XiBlock *block) {
    if (!context || !function || !block)
        return false;
    for (uint32_t index = 0u; index < function->nblocks; ++index) {
        const XiBlock *predecessor = function->blocks[index];
        if (predecessor && canonical_block_is_reachable(context, function, predecessor) &&
            predecessor->succs[0] == block &&
            exact_infallible_empty_class_allocation_in_block(context, function, predecessor))
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

static bool xg_effect_contract_to_core(uint32_t xg_effects, uint32_t *core_effects) {
    uint32_t supported = XG_BODY_MAY_ERROR | XG_BODY_MAY_PANIC | XG_BODY_MAY_CALL |
                         XG_BODY_MAY_TRAP | XG_BODY_TARGET_QUERY;
    if (!core_effects || (xg_effects & ~supported) != 0u)
        return false;
    uint32_t mapped = 0u;
    if ((xg_effects & XG_BODY_MAY_ERROR) != 0u)
        mapped |= XR_CORE_EFFECT_ERROR;
    if ((xg_effects & XG_BODY_MAY_PANIC) != 0u)
        mapped |= XR_CORE_EFFECT_PANIC;
    if ((xg_effects & XG_BODY_MAY_CALL) != 0u)
        mapped |= XR_CORE_EFFECT_CALL;
    if ((xg_effects & XG_BODY_MAY_TRAP) != 0u)
        mapped |= XR_CORE_EFFECT_TRAP;
    if ((xg_effects & XG_BODY_TARGET_QUERY) != 0u)
        mapped |= XR_CORE_EFFECT_TARGET_QUERY;
    *core_effects = mapped;
    return true;
}

static bool xg_capability_contract_to_core(uint32_t xg_capabilities,
                                           uint32_t *core_capabilities) {
    const uint32_t supported = XG_CAP_PROFILE_POINTER_WIDTH |
                               XG_CAP_PROFILE_OPERATING_SYSTEM |
                               XG_CAP_PROFILE_ARCHITECTURE |
                               XG_CAP_PROFILE_NATIVE_ABI |
                               XG_CAP_PROFILE_ENDIANNESS;
    if (!core_capabilities || (xg_capabilities & ~supported) != 0u)
        return false;
    uint32_t mapped = 0u;
    if ((xg_capabilities & XG_CAP_PROFILE_POINTER_WIDTH) != 0u)
        mapped |= XR_CORE_CAPABILITY_PROFILE_POINTER_WIDTH;
    if ((xg_capabilities & XG_CAP_PROFILE_OPERATING_SYSTEM) != 0u)
        mapped |= XR_CORE_CAPABILITY_PROFILE_OPERATING_SYSTEM;
    if ((xg_capabilities & XG_CAP_PROFILE_ARCHITECTURE) != 0u)
        mapped |= XR_CORE_CAPABILITY_PROFILE_ARCHITECTURE;
    if ((xg_capabilities & XG_CAP_PROFILE_NATIVE_ABI) != 0u)
        mapped |= XR_CORE_CAPABILITY_PROFILE_NATIVE_ABI;
    if ((xg_capabilities & XG_CAP_PROFILE_ENDIANNESS) != 0u)
        mapped |= XR_CORE_CAPABILITY_PROFILE_ENDIANNESS;
    *core_capabilities = mapped;
    return true;
}

static bool witness_call_effect_contract(const XrXiBuildContext *context,
                                         const XiFunc *caller, const XiValue *call,
                                         uint32_t *effect_mask,
                                         uint32_t *capability_mask) {
    const XgCallsiteSummary *callsite = resolved_witness_callsite(context, caller, call);
    const XgInterfaceMethodSummary *method =
        callsite ? find_interface_method_by_id(context->source->global_evidence,
                                               callsite->method_id)
                 : NULL;
    return method && method->contract_complete &&
           method->owner_interface_id == callsite->receiver_static_interface_id &&
           xg_effect_contract_to_core(method->effect_bits, effect_mask) &&
           xg_capability_contract_to_core(method->capability_bits, capability_mask);
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
    if (!xg_effect_contract_to_core(method->effect_bits, &effect_mask)) {
        contract_mismatch = "Xglobal effect contract contains an unsupported effect";
        goto invalid;
    }
    if (!xg_capability_contract_to_core(method->capability_bits, &capability_mask)) {
        contract_mismatch = "Xglobal capability contract contains an unsupported capability";
        goto invalid;
    }
    if ((target_storage->closed_effect_mask & ~effect_mask) != 0u) {
        contract_mismatch = "Xi closed effects exceed the Xglobal effect contract";
        goto invalid;
    }
    if ((target_storage->closed_capability_mask & ~capability_mask) != 0u) {
        contract_mismatch = "Xi closed capabilities exceed the Xglobal capability contract";
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
        return resolved_direct_callee(context, function, producer) ||
                       resolved_callable_call_targets(context, function, producer, NULL)
                   ? producer
                   : NULL;
    if ((producer->op == XI_CALL_METHOD || producer->op == XI_CALL_METHOD_DIRECT) &&
        producer->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE)
        return resolved_witness_callsite(context, function, producer) ? producer : NULL;
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
    const XiValue *call = block_typed_invoke_call(context, function, value->block);
    if (value == call || value == value->block->control)
        return call != NULL ||
               (value == value->block->control &&
                exact_infallible_empty_class_allocation_in_block(context, function, value->block));
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
    bool panic_valid = evidence_valid && (((row->callable_effect_union & XG_BODY_MAY_PANIC) !=
                                           0u) == ((row->flags & XG_CALL_MAY_PANIC) != 0u));
    if (!evidence_valid || !mirror_valid || !error_valid || !panic_valid)
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
                    resolved_direct_callee(context, function, consumer) != target || !row ||
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

static const XiValue *exact_logical_value_identity(const XrXiBuildContext *context,
                                                   const XiFunc *function, const XiValue *value) {
    value = logical_value_identity(value);
    while (imported_callable_checktype_is_exact(context, function, value)) {
        value = logical_value_identity(value->args[0]);
    }
    return value;
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

static bool imported_callable_value_is_exact(const XrXiBuildContext *context,
                                             const XiFunc *function, const XiValue *value) {
    uint64_t signature_key = 0u;
    return value && value->op == XI_GET_SHARED &&
           imported_callable_refined_type(context, function, value) != NULL &&
           resolved_imported_callable_target(context, function, value, &signature_key) != NULL &&
           signature_key != 0u;
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
                    if (!value || !imported_callable_ref(context, function, value))
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
                        resolved_direct_callee(context, function, call) ||
                        resolved_empty_class_allocation(context, function, call))
                        continue;

                    const XgCallsiteSummary *row = resolved_callsite(context, function, call);
                    if (!row)
                        return fail(
                            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                            "Xi indirect call v%u has an unresolved callable target set", call->id);
                    if (row->kind != XG_CALL_CLOSURE ||
                        (row->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u)
                        return fail(
                            diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi indirect call v%u has inconsistent callable effect evidence",
                            call->id);

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
            resolved_imported_callable_target(context, function, value, signature_key);
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
        const XrType *refined_type = imported_callable_refined_type(context, function, value);
        if (refined_type &&
            resolved_imported_callable_target(context, function, value, &signature_key))
            return map_callable_signature_contract(context, refined_type, signature_key, type_id,
                                                   NULL);
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
    return value->op == XI_CLOSURE_NEW ||
           (value->op == XI_GET_SHARED &&
            resolved_imported_callable_target(context, function, value, NULL)) ||
           value->xg_existential_kind == XI_EXISTENTIAL_PACK ||
           value->xg_existential_kind == XI_EXISTENTIAL_PROJECT || value->op == XI_SUM_INJECT ||
           xi_copy_is_value_clone(value) || value->op == XI_SOURCE_MOVE ||
           value->op == XI_OWNER_FORWARD || value->op == XI_CALL ||
           (value->op == XI_CALL_BUILTIN && value->aux && value->aux_kind == XI_AUX_KIND_NONE &&
            strcmp((const char *) value->aux, "copy") == 0);
}

static XrCoreIrValueCategory logical_value_category(const XiValue *value) {
    value = logical_value_identity(value);
    return value && ((value->op == XI_PARAM && value->param_mode == XR_PARAM_REF) ||
                     value->op == XI_LOCAL_ADDR)
               ? XR_CORE_IR_PLACE
               : XR_CORE_IR_VALUE;
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
    if (type_id != XR_CORE_TYPE_VOID && ((source_type_mapped && source_type_id != type_id) ||
                                         (!source_type_mapped && !erased_error_catch)))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi block argument v%u type conflicts with its canonical continuation",
                    source->id);
    if (type_id == XR_CORE_TYPE_VOID && source_type_mapped)
        type_id = source_type_id;
    if (type_id == XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi live-in v%u has no active CoreSpec value type", source->id);
    XrCoreIrValueCategory category = logical_value_category(source);
    XrCoreIrOwnershipDisposition ownership =
        implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT ||
                implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_ERROR
            ? logical_ownership_for_type(context, type_id)
        : logical_value_produces_owner(context, function->xi, source, 0u)
            ? logical_ownership_for_type(context, type_id)
            : XR_CORE_IR_NON_OWNER;
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

static bool value_has_canonical_materialization(const XrXiBuildContext *context,
                                                const XiFunc *function, const XiValue *value) {
    if (!value)
        return false;
    if (xr_program_xi_value_is_materialized(value->op) ||
        imported_callable_value_is_exact(context, function, value) ||
        static_typed_catch_contract_is_exact(context, value) ||
        resolved_empty_class_allocation(context, function, value) ||
        resolved_empty_struct_literal(context, function, value) ||
        resolved_unit_enum_literal(context, function, value, NULL))
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
    XrXiBlockArgumentStorage *argument = find_block_argument((XrXiBlockStorage *) block, value);
    if (argument) {
        *key_out = argument->key;
        return true;
    }
    if (value->block != block->xi)
        return false;
    if (!value_has_canonical_materialization(context, function ? function->xi : NULL, value))
        return false;
    *key_out = value_key(function, value);
    return true;
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
            }
            xr_free(function->block_storage);
            xr_free(function->blocks);
            xr_free(function->parameter_types);
            xr_free(function->parameter_modes);
        }
        xr_free(module->function_storage);
        xr_free(module->functions);
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
translate_call(XrXiBuildContext *context, const XrXiModuleStorage *module,
               XrXiFunctionStorage *function, const XiValue *value, const XrXiBlockStorage *block,
               const XrProgramXiProjection *projection, XrCoreIrInstructionInput *instruction,
               char *diagnostic, size_t diagnostic_size) {
    (void) module;
    const XgCallsiteSummary *callsite = resolved_callsite(context, function->xi, value);
    if (!callsite)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi call v%u lacks matching global callsite evidence", value->id);
    if (resolved_empty_class_allocation(context, function->xi, value)) {
        uint16_t result_type = XR_CORE_TYPE_VOID;
        if (!map_type(context, value->type, &result_type))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi class allocation v%u has no active nominal type", value->id);
        const XrXiTypeStorage *aggregate = find_dynamic_type_by_id(context, result_type);
        if (!aggregate || aggregate->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
            aggregate->input.field_count != 0u ||
            aggregate->input.nominal_kind == XR_CORE_IR_NOMINAL_NONE)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi class allocation v%u is not an exact empty nominal aggregate",
                        value->id);
        instruction->operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT;
        instruction->result = value_key(function, value);
        instruction->result_type_id = result_type;
        instruction->result_ownership = logical_ownership_for_type(context, result_type);
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        return XR_PROGRAM_BUILD_OK;
    }
    if (callsite->kind != XG_CALL_DIRECT_FUNC && callsite->kind != XG_CALL_CLOSURE)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi call v%u uses unsupported global callsite kind %u", value->id,
                    callsite->kind);
    if ((callsite->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi call v%u lacks verified callsite error evidence", value->id);
    const XiFunc *callee = resolved_direct_callee(context, function->xi, value);
    uint32_t callee_module_index = UINT32_MAX;
    uint32_t callee_function_index = UINT32_MAX;
    const XrXiFunctionStorage *callee_storage =
        find_xi_function(context, callee, &callee_module_index, &callee_function_index);
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
        return set_operands(context, instruction, function, block, value->args, value->nargs,
                            diagnostic, diagnostic_size);
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
    if (panic_type != XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "panic-capable Xi call v%u lacks an explicit panic continuation", value->id);

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
    return set_operands(context, instruction, function, block, value->args + 1u, value->nargs - 1u,
                        diagnostic, diagnostic_size);
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
    instruction->result_ownership = XR_CORE_IR_OWNER;
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
translate_imported_callable_pack(XrXiBuildContext *context, XrXiFunctionStorage *function,
                                 const XiValue *value, XrCoreIrInstructionInput *instruction,
                                 char *diagnostic, size_t diagnostic_size) {
    uint64_t signature_key = 0u;
    const XiFunc *target =
        resolved_imported_callable_target(context, function->xi, value, &signature_key);
    const XrXiFunctionStorage *target_storage = find_xi_function(context, target, NULL, NULL);
    const XrType *refined_type = imported_callable_refined_type(context, function->xi, value);
    uint16_t callable_type_id = XR_CORE_TYPE_VOID;
    if (!target || !target_storage || !refined_type || target->ncaptures != 0u ||
        !map_callable_signature_contract(context, refined_type, signature_key, &callable_type_id,
                                         NULL))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                    "Xi imported callable v%u has no exact target or signature", value->id);
    const XrXiTypeStorage *callable = find_dynamic_type_by_id(context, callable_type_id);
    if (!callable || callable->input.kind != XR_CORE_IR_TYPE_CALLABLE ||
        !callable->callable_signature)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi imported callable v%u has an inconsistent callable type", value->id);
    instruction->operation_id = XR_CORE_OP_CORE_CALLABLE_PACK;
    instruction->result = value_key(function, value);
    instruction->result_type_id = callable_type_id;
    instruction->result_ownership = XR_CORE_IR_OWNER;
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
    return set_operands(context, instruction, function, block, value->args, value->nargs,
                        diagnostic, diagnostic_size);
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
                    "Xi operation %u at v%u result type kind %u is not active in CoreSpec",
                    value->op, value->id, value->type ? (unsigned) value->type->kind : UINT32_MAX);
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
        return translate_imported_callable_pack(context, function, value, instruction, diagnostic,
                                                diagnostic_size);
    uint16_t target_enum_type = XR_CORE_TYPE_VOID;
    if (value->op == XI_CONST && value->nargs == 0u && value->aux == NULL &&
        value->aux_int > 0 && value->aux_int <= UINT16_MAX &&
        map_type(context, value->type, &target_enum_type) &&
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
        case XR_PROGRAM_XI_PROJECTION_COMPARE: {
            uint16_t left_type = XR_CORE_TYPE_VOID;
            uint16_t right_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 2u || result_type != XR_CORE_TYPE_BOOL ||
                !map_type(context, value->args[0]->type, &left_type) ||
                !map_type(context, value->args[1]->type, &right_type) ||
                left_type != right_type)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi comparison v%u operands do not have one exact logical type",
                            value->id);
            bool target_enum_compare = left_type >= XR_CORE_TYPE_TARGET_OS &&
                                       left_type <= XR_CORE_TYPE_TARGET_ENDIAN;
            if ((!target_enum_compare && left_type != XR_CORE_TYPE_I64) ||
                (target_enum_compare && projection.immediate_u32 > 1u))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi comparison v%u is outside the active exact comparison domain",
                            value->id);
            instruction->operation_id = target_enum_compare
                                            ? XR_CORE_OP_CORE_COMPARE_TARGET_ENUM
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
                type->input.variants[variant].payload_types[field] != result_type ||
                logical_ownership_for_type(context, result_type) == XR_CORE_IR_OWNER)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant projection v%u has an invalid declaration ordinal",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
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
            return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
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
            return set_operands(context, instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_PLACE_LOAD: {
            uint16_t place_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 1u || result_type == XR_CORE_TYPE_VOID ||
                !map_type(context, value->args[0]->type, &place_type) ||
                place_type != result_type ||
                logical_value_category(value->args[0]) != XR_CORE_IR_PLACE)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi place load v%u has no exact pointee type", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
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
                logical_value_category(value->args[0]) != XR_CORE_IR_PLACE)
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
                result_type != expected_type || projection.core_operation_id != expected_operation ||
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
                query->query_kind != expected_query ||
                query->result_native_type != XR_NATIVE_U16 || query->contract_complete != 1u ||
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
        default:
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi operation %u at v%u has an invalid CoreSpec projection", value->op,
                        value->id);
    }
}

static bool value_is_skipped(const XrXiBuildContext *context, const XiFunc *function,
                             const XiValue *value) {
    const XrXiFunctionStorage *function_storage =
        value && value->block ? find_xi_function(context, function, NULL, NULL) : NULL;
    const XrXiBlockStorage *block_storage =
        function_storage ? find_block_storage(function_storage, value->block) : NULL;
    if (value && value->block && value->block->control == value &&
        value_is_static_typed_catch_test(value) && block_storage &&
        block_storage->static_branch_outcome != XR_XI_STATIC_BRANCH_UNKNOWN)
        return true;
    if (value_is_invoke_scaffold(context, function, value))
        return true;
    if (imported_callable_checktype_is_exact(context, function, value))
        return true;
    if (value->op == XI_PARAM || value->op == XI_THROW || xi_copy_is_identity_alias(value) ||
        (xi_copy_is_value_clone(value) && logical_value_identity(value) != value))
        return true;
    /* Physical RC is executor-private representation.  Canonical ownership is
     * reconstructed from typed owner creation and semantic uses, never from
     * retain/release counts. */
    if (value->op == XI_RETAIN || value->op == XI_RELEASE)
        return true;
    if (imported_callable_value_is_exact(context, function, value))
        return false;
    return (value->op == XI_GET_SHARED || value->op == XI_GET_BUILTIN) &&
           value_is_only_elided_operand(context, function, value);
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
    if (value->block == block->xi)
        return XR_PROGRAM_BUILD_OK;
    XrXiBlockArgumentStorage *available = find_block_argument(block, value);
    if (available) {
        uint16_t source_type = XR_CORE_TYPE_VOID;
        bool source_type_mapped =
            map_logical_value_type(context, function->xi, typed_value, &source_type) &&
            source_type != XR_CORE_TYPE_VOID;
        bool erased_error_catch =
            value->op == XI_ERR_CATCH && value->type && value->type->kind == XR_KIND_UNKNOWN;
        XrCoreIrOwnershipDisposition expected_ownership =
            available->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT ||
                    available->implicit_invoke_kind == XR_XI_INVOKE_ARGUMENT_ERROR
                ? logical_ownership_for_type(context, available->type_id)
            : logical_value_produces_owner(context, function->xi, value, 0u)
                ? logical_ownership_for_type(context, available->type_id)
                : XR_CORE_IR_NON_OWNER;
        if (available->phi || (source_type_mapped && source_type != available->type_id) ||
            (!source_type_mapped && !erased_error_catch) ||
            available->category != logical_value_category(value) ||
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
    uint16_t begin = value->op == XI_VARIANT_CONSTRUCT ||
                             (value->op == XI_CALL &&
                              (resolved_direct_callee(context, function->xi, value) != NULL ||
                               resolved_empty_class_allocation(context, function->xi, value))) ||
                             resolved_empty_struct_literal(context, function->xi, value) ||
                             resolved_unit_enum_literal(context, function->xi, value, NULL)
                         ? 1u
                         : 0u;
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
        const XiFunc *callee = resolved_direct_callee(context, function->xi, call);
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
        uint16_t first_operand = indirect || witness ? 0u : 1u;
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
    if (exact_infallible_empty_class_allocation_in_block(context, function, block))
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
    if (exact_infallible_empty_class_allocation_in_block(context, function, block))
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
        }
        if (!changed)
            return XR_PROGRAM_BUILD_OK;
    }
    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "Xi function block reachability did not converge");
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

static XrProgramBuildStatus close_block_arguments(XrXiBuildContext *context,
                                                  XrXiFunctionStorage *function, char *diagnostic,
                                                  size_t diagnostic_size) {
    XrProgramBuildStatus invoke_status =
        prepare_invoke_arguments(context, function, diagnostic, diagnostic_size);
    if (invoke_status != XR_PROGRAM_BUILD_OK)
        return invoke_status;
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
            block_is_elided_empty_class_error_continuation(context, function->xi, block->xi))
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
        if (block->xi->control && block->static_branch_outcome == XR_XI_STATIC_BRANCH_UNKNOWN) {
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
        XrProgramBuildStatus balance_status = balance_owner_successor_arguments(
            context, function, &changed, diagnostic, diagnostic_size);
        if (balance_status != XR_PROGRAM_BUILD_OK)
            return balance_status;
        for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
            XrXiBlockStorage *successor = &function->block_storage[block_index];
            if (!successor->reachable || block_is_elided_empty_class_error_continuation(
                                             context, function->xi, successor->xi))
                continue;
            uint32_t argument_count = successor->argument_count;
            for (uint16_t predecessor_index = 0; predecessor_index < successor->xi->npreds;
                 ++predecessor_index) {
                const XiBlock *predecessor_xi = successor->xi->preds[predecessor_index];
                if (block_is_elided_empty_class_error_continuation(context, function->xi,
                                                                   predecessor_xi))
                    continue;
                XrXiBlockStorage *predecessor = find_block_storage(function, predecessor_xi);
                if (!predecessor)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi CFG predecessor is absent");
                if (!predecessor->reachable)
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
    uint32_t expected_entry_arguments =
        function->xi->nparams + (function->capture_type_id != XR_CORE_TYPE_VOID ? 1u : 0u);
    if (entry->argument_count != expected_entry_arguments)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi entry block acquired non-parameter live-ins");

    for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
        XrXiBlockStorage *block = &function->block_storage[block_index];
        if (!block->reachable ||
            block_is_elided_empty_class_error_continuation(context, function->xi, block->xi))
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
            if (!value_operand_key(context, function, predecessor, incoming, &operands[cursor++])) {
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

static XrProgramBuildStatus set_invoke_operands(const XrXiBuildContext *context,
                                                XrCoreIrInstructionInput *instruction,
                                                const XrXiFunctionStorage *function,
                                                const XrXiBlockStorage *predecessor,
                                                const XiValue *call, const XrXiBlockStorage *normal,
                                                const XrXiBlockStorage *error, bool indirect,
                                                char *diagnostic, size_t diagnostic_size) {
    uint32_t normal_implicit = call->type && call->type->kind != XR_KIND_UNIT ? 1u : 0u;
    if (normal->argument_count < normal_implicit || error->argument_count == 0u ||
        (normal_implicit != 0u &&
         normal->argument_storage[0].implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT) ||
        error->argument_storage[0].implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_ERROR)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi invoke continuations lack ordered implicit result/error arguments");
    uint32_t first_operand = indirect ? 0u : 1u;
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
            if (!value_operand_key(context, function, predecessor, incoming, &operands[cursor++])) {
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
         instruction->operation_id == XR_CORE_OP_CORE_PLACE_LOCAL) &&
        operand_index == 0u)
        return true;
    if (instruction->operation_id == XR_CORE_OP_CORE_PLACE_STORE && operand_index == 1u)
        return true;
    if (instruction->operation_id == XR_CORE_OP_CORE_VARIANT_CONSTRUCT) {
        uint16_t operand_type = XR_CORE_TYPE_VOID;
        return input_value_type(block, instruction_count, instruction->operands[operand_index],
                                &operand_type) &&
               logical_ownership_for_type(context, operand_type) == XR_CORE_IR_OWNER;
    }
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
               instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) {
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
        if (incoming && value_operand_key(context, function, predecessor, incoming, &key) &&
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
                        "Xi block b%u has an unbalanced affine owner", block->xi->id);
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

static const XrCoreOperationSpec *xi_block_terminal_contract(const XrXiBuildContext *context,
                                                             const XiFunc *function,
                                                             const XiBlock *block) {
    const XiValue *invoke = block_typed_invoke_call(context, function, block);
    if (invoke && invoke->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE)
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CALL_WITNESS_INVOKE);
    if (invoke)
        return xr_core_spec_operation_by_id(resolved_direct_callee(context, function, invoke)
                                                ? XR_CORE_OP_CORE_CALL_SEALED_INVOKE
                                                : XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE);
    if (exact_infallible_empty_class_allocation_in_block(context, function, block))
        return xr_core_spec_operation_by_id(XR_CORE_OP_CORE_BRANCH);
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
                    block_is_elided_empty_class_error_continuation(context, function, block))
                    continue;
                for (uint32_t value_index = 0; block && value_index < block->nvalues;
                     ++value_index) {
                    const XiValue *value = block->values[value_index];
                    if (!value || value_is_skipped(context, function, value))
                        continue;
                    uint32_t value_effects = 0u;
                    uint32_t value_capabilities = 0u;
                    const XrCoreOperationSpec *imported_callable =
                        value->op == XI_GET_SHARED &&
                                resolved_imported_callable_target(context, function, value, NULL)
                            ? xr_core_spec_operation_by_id(XR_CORE_OP_CORE_CALLABLE_PACK)
                            : NULL;
                    const XrCoreOperationSpec *empty_class_allocation =
                        resolved_empty_class_allocation(context, function, value)
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
                    } else if (imported_callable) {
                        has_contract = true;
                        value_effects = imported_callable->effect_mask;
                        value_capabilities = imported_callable->capability_mask;
                    } else if (empty_class_allocation) {
                        has_contract = true;
                        value_effects = empty_class_allocation->effect_mask;
                        value_capabilities = empty_class_allocation->capability_mask;
                    } else if (empty_struct_literal) {
                        has_contract = true;
                        value_effects = empty_struct_literal->effect_mask;
                        value_capabilities = empty_struct_literal->capability_mask;
                    } else if (unit_enum_literal) {
                        has_contract = true;
                        value_effects = unit_enum_literal->effect_mask;
                        value_capabilities = unit_enum_literal->capability_mask;
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
                        return fail(diagnostic, diagnostic_size,
                                    XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                    "Xi function %s operation %s (%u) at v%u has no unique "
                                    "CoreSpec contract",
                                    function->name ? function->name : "<anonymous>",
                                    xi_op_name(value->op), value->op, value->id);
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
                        block_is_elided_empty_class_error_continuation(context, storage->xi, block))
                        continue;
                    for (uint32_t value_index = 0; block && value_index < block->nvalues;
                         ++value_index) {
                        const XiValue *value = block->values[value_index];
                        bool witness =
                            value &&
                            (value->op == XI_CALL_METHOD || value->op == XI_CALL_METHOD_DIRECT) &&
                            (value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_DIRECT ||
                             value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE);
                        if (!value || (value->op != XI_CALL && !witness))
                            continue;
                        if (resolved_empty_class_allocation(context, storage->xi, value))
                            continue;
                        const XiFunc *callee = resolved_direct_callee(context, storage->xi, value);
                        bool invoke = block_typed_invoke_call(context, storage->xi, block) == value;
                        if (witness) {
                            uint32_t witness_effects = 0u;
                            uint32_t witness_capabilities = 0u;
                            if (!witness_call_effect_contract(context, storage->xi, value,
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
    memset(output, 0, sizeof(*output));
    memset(storage, 0, sizeof(*storage));
    storage->xi = xi;
    storage->key = key;
    storage->closed_effect_mask = closed_effect_mask;
    storage->closed_capability_mask = closed_capability_mask;
    storage->closed_contract_ready = closed_contract_ready;
    storage->block_storage = block_storage;
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
            if (!xi->params || !xi->params[parameter] || xi->params[parameter]->op != XI_PARAM ||
                xi->params[parameter]->aux_int != parameter ||
                (xi->params[parameter]->type &&
                 xi->params[parameter]->type->kind == XR_KIND_INTERFACE &&
                 !interface_parameter_contract_is_exact(
                     context, xi, xi->params[parameter],
                     (XrParamMode) xi->params[parameter]->param_mode)) ||
                !map_type_for_mode(context, xi->params[parameter]->type,
                                   (XrParamMode) xi->params[parameter]->param_mode,
                                   &storage->parameter_types[parameter + parameter_offset], NULL,
                                   0u) ||
                storage->parameter_types[parameter + parameter_offset] == XR_CORE_TYPE_VOID)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi function %u parameter %u is not a CoreSpec value", function_index,
                            parameter);
            if (!xr_param_mode_is_valid((XrParamMode) xi->params[parameter]->param_mode))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi function %u parameter %u has an invalid mode", function_index,
                            parameter);
            storage->parameter_modes[parameter + parameter_offset] =
                (XrParamMode) xi->params[parameter]->param_mode;
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

    storage->blocks = xr_calloc(xi->nblocks, sizeof(*storage->blocks));
    if (!storage->blocks || !storage->block_storage)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    output->blocks = storage->blocks;
    output->entry_block = block_key(storage, xi->entry);
    XrProgramBuildStatus status =
        close_block_arguments(context, storage, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;

    output->block_count = 0u;
    for (uint32_t block_index = 0u; block_index < xi->nblocks; ++block_index)
        if (storage->block_storage[block_index].reachable &&
            !block_is_elided_empty_class_error_continuation(context, xi, xi->blocks[block_index]))
            ++output->block_count;
    if (output->block_count == 0u || !find_block_storage(storage, xi->entry)->reachable ||
        block_is_elided_empty_class_error_continuation(context, xi, xi->entry))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi function %u has no canonical entry block", function_index);

    uint32_t output_block_index = 0u;
    for (uint32_t block_index = 0; block_index < xi->nblocks; ++block_index) {
        const XiBlock *xi_block = xi->blocks[block_index];
        if (!storage->block_storage[block_index].reachable ||
            block_is_elided_empty_class_error_continuation(context, xi, xi_block))
            continue;
        XrCoreIrBlockInput *block_output = &storage->blocks[output_block_index++];
        XrXiBlockStorage *block_storage = &storage->block_storage[block_index];
        block_output->key = block_key(storage, xi_block);
        block_output->arguments = block_storage->arguments;
        block_output->argument_count = block_storage->argument_count;

        uint32_t emitted = block_storage->argument_count != 0u ? 1u : 0u;
        for (uint32_t value_index = 0; value_index < xi_block->nvalues; ++value_index) {
            const XiValue *value = xi_block->values[value_index];
            if (value_is_skipped(context, xi, value))
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
        for (uint32_t value_index = 0; value_index < xi_block->nvalues; ++value_index) {
            const XiValue *value = xi_block->values[value_index];
            if (value_is_skipped(context, xi, value))
                continue;
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
        }

        XrCoreIrInstructionInput *terminator = &block_storage->instructions[instruction_index++];
        terminator->result_type_id = XR_CORE_TYPE_VOID;
        terminator->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        const XiValue *invoke_call = block_typed_invoke_call(context, xi, xi_block);
        if (invoke_call) {
            const XiFunc *callee = resolved_direct_callee(context, xi, invoke_call);
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
            status = set_invoke_operands(context, terminator, storage, block_storage, invoke_call,
                                         normal, error, indirect || witness, diagnostic,
                                         diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (exact_infallible_empty_class_allocation_in_block(context, xi, xi_block)) {
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
            const XiValue *throw_value = NULL;
            for (uint32_t value_index = 0u; value_index < xi_block->nvalues; ++value_index) {
                const XiValue *candidate = xi_block->values[value_index];
                if (!candidate || candidate->op != XI_THROW)
                    continue;
                if (throw_value)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi panic block b%u has multiple throw terminals", xi_block->id);
                throw_value = candidate;
            }
            if (!throw_value || throw_value->nargs != 1u || !throw_value->args ||
                !throw_value->args[0])
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi unreachable block b%u has no typed panic terminal", xi_block->id);
            terminator->operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH;
            XiValue *published[] = {throw_value->args[0]};
            status = set_operands(context, terminator, storage, block_storage, published, 1u,
                                  diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
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
                        block_is_elided_empty_class_error_continuation(context, function->xi,
                                                                       block))
                        continue;
                    for (uint32_t value_index = 0; value_index < block->nvalues; ++value_index) {
                        const XiValue *value = block->values[value_index];
                        bool witness =
                            (value->op == XI_CALL_METHOD ||
                             value->op == XI_CALL_METHOD_DIRECT) &&
                            (value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_DIRECT ||
                             value->xg_existential_kind == XI_EXISTENTIAL_WITNESS_INVOKE);
                        if (value->op != XI_CALL && !witness)
                            continue;
                        if (resolved_empty_class_allocation(context, function->xi, value))
                            continue;
                        const XiFunc *callee = resolved_direct_callee(context, function->xi, value);
                        bool invoke =
                            block_typed_invoke_call(context, function->xi, block) == value;
                        if (witness) {
                            uint32_t witness_effects = 0u;
                            uint32_t witness_capabilities = 0u;
                            if (!witness_call_effect_contract(context, function->xi, value,
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

static XrProgramBuildStatus collect_module_functions(XrXiModuleStorage *storage, char *diagnostic,
                                                     size_t diagnostic_size) {
    const XiModule *module = storage && storage->root ? storage->root->module : NULL;
    if (!module || module->nfuncs == 0u || !module->functions)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi module has no source functions");
    uint32_t count = module->nfuncs;
    for (uint16_t function = 0; function < module->nfuncs; ++function)
        if (!module->functions[function] ||
            !count_nested_functions(module->functions[function], &count))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi module function tree is malformed or too large");
    const XiFunc **functions = xr_calloc(count, sizeof(*functions));
    if (!functions)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t cursor = 0u;
    for (uint16_t function = 0; function < module->nfuncs; ++function)
        functions[cursor++] = module->functions[function];
    for (uint16_t function = 0; function < module->nfuncs; ++function) {
        if (!append_nested_functions(module->functions[function], functions, count, &cursor)) {
            xr_free(functions);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi module function tree cannot be flattened");
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

static XrProgramBuildStatus build_context(XrXiBuildContext *context, char *diagnostic,
                                          size_t diagnostic_size) {
    context->modules = xr_calloc(context->source->module_count, sizeof(*context->modules));
    context->storage = xr_calloc(context->source->module_count, sizeof(*context->storage));
    if (!context->modules || !context->storage)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;

    /* Publish every module/function identity before translating a body. This
     * makes forward and cross-module calls independent of input order. */
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
        XrProgramBuildStatus collect_status =
            collect_module_functions(storage, diagnostic, diagnostic_size);
        if (collect_status != XR_PROGRAM_BUILD_OK)
            return collect_status;
        output->key = key_from_stable_id(UINT8_C(0x4d), storage->source_authority->module_identity);
        storage->functions = xr_calloc(storage->function_count, sizeof(*storage->functions));
        storage->function_storage =
            xr_calloc(storage->function_count, sizeof(*storage->function_storage));
        if (!storage->functions || !storage->function_storage)
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
            .modules = context.modules,
            .module_count = input->module_count,
        };
        status = xr_core_ir_program_build(&core_input, &program, diagnostic, diagnostic_size);
    }
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact_out, diagnostic, diagnostic_size);
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
                "function_effects=%u function_capabilities=%u",
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
                failed_function ? failed_function->capability_mask : 0u);
        }
    }
    xr_core_ir_program_free(program);
    free_context(&context);
    return status;
}
