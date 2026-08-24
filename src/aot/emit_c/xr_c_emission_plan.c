/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_emission_plan.c - Immutable TargetPlan-backed C emission plan
 */

#include "xr_c_emission_plan.h"
#include "xr_c_emission_rule_runtime.h"
#include "../xaot_layout_gen.h"
#include "../xr_target_aggregate_c_projection.h"
#include "../../plan/target/xr_target_plan.h"
#include "../../plan/semantic/xr_semantic_allocation_shape.h"
#include "../../plan/semantic/xr_semantic_class_shape.h"
#include "../../plan/semantic/xr_semantic_panic_catch_shape.h"
#include "../../plan/semantic/xr_semantic_enum_shape.h"
#include "../../plan/semantic/xr_semantic_string_shape.h"
#include "../../plan/semantic/xr_semantic_container_copy_shape.h"
#include "../../plan/semantic/xr_semantic_string_runes_shape.h"
#include "../../plan/semantic/xr_semantic_iterator_rune_has_next_shape.h"
#include "../../plan/semantic/xr_semantic_iterator_rune_next_shape.h"
#include "../../plan/semantic/xr_semantic_iterator_rune_nth_shape.h"
#include "../../plan/semantic/xr_semantic_rune_to_uint32_shape.h"
#include "../../plan/semantic/xr_semantic_rune_to_string_shape.h"
#include "../../plan/semantic/xr_semantic_rune_is_whitespace_shape.h"
#include "../../plan/semantic/xr_semantic_string_slice_shape.h"
#include "../../plan/semantic/xr_semantic_local_addr_shape.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../ir/xi_own.h"
#include "../../frontend/analyzer/xa_intrinsic_registry.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_C_EMISSION_PLAN_SCHEMA_VERSION UINT32_C(37)
#define XR_C_CHANNEL_NEW_SYMBOL "xr_aot_channel_new"
#define XR_C_STRINGBUILDER_NEW_SYMBOL "xrt_strbuf_new"
#define XR_C_CHANNEL_RECV_INT_SYMBOL "XR_TO_INT"
#define XR_C_CHANNEL_RECV_FLOAT_SYMBOL "XR_TO_FLOAT"
#define XR_C_CHANNEL_RECV_BOOL_SYMBOL "XR_TO_BOOL"
#define XR_C_CHANNEL_RECV_RUNE_SYMBOL "XR_TO_RUNE"
#define XR_C_STRING_BYTE_SLICE_VIEW_SYMBOL "xrt_span_from_string_bytes"
#define XR_C_STRINGBUILDER_APPEND_RUNE_SYMBOL "xrt_strbuf_append"
#define XR_C_STRINGBUILDER_TO_STRING_SYMBOL "xrt_strbuf_finish"
#define XR_C_STRINGBUILDER_APPEND_STRING_SYMBOL "xrt_strbuf_append"
#define XR_C_STRING_CONCAT_SYMBOL "xrt_str_concat_parts"
#define XR_C_ADT_ENUM_CONSTRUCTOR_SYMBOL "xrt_enum_aggregate_box"
#define XR_C_ARRAY_WITH_CAPACITY_SYMBOL "xrt_array_with_capacity_value"
#define XR_C_ARRAY_FILLED_NEW_SYMBOL "xrt_array_new_filled_value"
#define XR_C_STRING_RUNES_SYMBOL "xrt_string_runes"
#define XR_C_ITERATOR_RUNE_HAS_NEXT_SYMBOL "xrt_iterator_rune_has_next"
#define XR_C_ITERATOR_RUNE_NEXT_SYMBOL "xrt_iterator_rune_next"
#define XR_C_ITERATOR_RUNE_NTH_SYMBOL "xrt_iterator_rune_nth"
#define XR_C_RUNE_TO_UINT32_SYMBOL "xrt_rune_to_uint32"
#define XR_C_RUNE_TO_STRING_SYMBOL "xrt_rune_to_string"
#define XR_C_RUNE_IS_WHITESPACE_SYMBOL "xrt_rune_is_whitespace"
#define XR_C_ARRAY_NEW_SYMBOL "xrt_array_new_typed"
#define XR_C_RELEASE_SYMBOL "xrt_release"
#define XR_C_STRING_SLICE_RANGE_SYMBOL "xrt_string_slice_range"

#include "xr_c_emission_rule_rows.inc.c"

struct XrCEmissionPlan {
    XrCValueEmissionView *values;
    uint32_t value_count;
    XrCCallArgumentEmissionView *call_arguments;
    uint32_t call_argument_count;
    XrCRecipeArgumentView *recipe_arguments;
    uint32_t recipe_argument_count;
    XrCCleanupEmissionView *cleanups;
    uint32_t cleanup_count;
    XrCFunctionAbiEmissionView *function_abis;
    uint32_t function_abi_count;
    uint32_t schema_version;
    XrFingerprint target_fingerprint;
    XrFingerprint profile_fingerprint;
    XrFingerprint fingerprint;
    bool verified;
};

static bool emission_error(char *error, size_t error_size, const char *code, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", code, detail);
    return false;
}

static const XrSemanticTypeRecord *emission_semantic_value_type(const XrTargetPlan *target_plan,
                                                                uint32_t semantic_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticTypeRecord *match = NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->result_value != semantic_value)
            continue;
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, operation->result_type);
        if (!type || (match && match != type))
            return NULL;
        match = type;
    }
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(semantic);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(semantic, i);
        if (!parameter || parameter->value != semantic_value)
            continue;
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, parameter->type);
        if (!type || (match && match != type))
            return NULL;
        match = type;
    }
    return match;
}

static const char *emission_raw_pointer_c_type(const XrTargetPlan *target_plan,
                                               uint32_t semantic_value) {
    const XrSemanticTypeRecord *type = emission_semantic_value_type(target_plan, semantic_value);
    unsigned kind = 0, semantic_type = 0, builtin_type = 0;
    unsigned nullable = 0, is_const = 0, is_value = 0, is_literal = 0;
    unsigned cycle_candidate = 0, pointer_mutable = 0, scalar_rep = 0;
    size_t alias_length = 0;
    int consumed = 0;
    if (!type || !type->canonical_key || type->kind != XR_KIND_POINTER || type->flags != 0 ||
        type->child_count != 0 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        sscanf(type->canonical_key, "type-v3:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%zu:%n", &kind,
               &semantic_type, &builtin_type, &nullable, &is_const, &is_value, &is_literal,
               &cycle_candidate, &pointer_mutable, &scalar_rep, &alias_length, &consumed) != 11 ||
        consumed <= 0 || (size_t) consumed != strlen(type->canonical_key) ||
        kind != XR_KIND_POINTER || semantic_type != 0 || builtin_type != XR_TID_NULL ||
        nullable != 0 || is_const != 0 || is_value != 0 || is_literal != 0 ||
        cycle_candidate != 0 || pointer_mutable > 1 || scalar_rep != XR_SCALAR_REP_NONE ||
        alias_length != 0)
        return NULL;
    return pointer_mutable ? "void *" : "const void *";
}

/* A SOURCE_EXPORT ref argument freezes one additional C pointer level on the
 * caller's LOCAL_ADDR result. The base pointee spelling still comes from the
 * semantic raw-pointer identity above; the extra level is admitted only by an
 * exact TargetPlan call-argument row, never by a source name or Xi type. */
static const char *emission_source_ref_place_c_type(const XrTargetPlan *target_plan,
                                                    uint32_t semantic_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *definition = NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->result_value != semantic_value)
            continue;
        if (definition)
            return NULL;
        definition = operation;
    }
    if (!definition || definition->opcode != XI_LOCAL_ADDR || definition->operand_count != 1)
        return NULL;
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(target_plan, semantic_value);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target_plan, &argument_count);
    bool matched = false;
    for (uint32_t i = 0; i < argument_count; i++) {
        const XrTargetCallArgumentRecord *argument = &arguments[i];
        if (argument->semantic_value != semantic_value)
            continue;
        const XrTargetCallRecord *call =
            argument->call < call_count ? &calls[argument->call] : NULL;
        if (!binding || !call || call->target_kind != XR_TARGET_CALL_TARGET_SOURCE_EXPORT ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT ||
            argument->mode != XR_TARGET_CALL_REFERENCE ||
            argument->ownership != XR_TARGET_CALL_WRITEBACK ||
            argument->flags != XR_TARGET_CALL_ARGUMENT_ADDRESSABLE ||
            argument->caller_slot != binding->slot ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
            argument->register_rep != binding->register_rep ||
            argument->memory_rep != binding->memory_rep ||
            argument->callee_register_rep != binding->register_rep ||
            argument->callee_memory_rep != binding->memory_rep)
            return NULL;
        matched = true;
    }
    if (!matched)
        return NULL;
    const char *base = emission_raw_pointer_c_type(target_plan, semantic_value);
    return base && strcmp(base, "const void *") == 0 ? "const void * *"
           : base && strcmp(base, "void *") == 0     ? "void * *"
                                                     : NULL;
}

static bool exact_direct_local_tagged_ref_parameter_recipe(
    const XrTargetPlan *target_plan, const XrTargetValueRepRecord *binding,
    uint8_t *out_storage);

/* The C boundary a function states for itself, used only where no call in this
 * plan states one for it.
 *
 * A boundary representation is normally a fact about the call rather than about
 * the declared type: measured over the whole corpus, 6 of 125 comparable
 * returns cross tagged while their declaration names a scalar. So a declaration
 * never overrides a call that names a representation -- this is the answer of
 * last resort, for a function no call in this module reaches.
 *
 * Only non-nullable scalars answer. An aggregate's or a reference's boundary
 * genuinely depends on the convention its caller uses and cannot be read off
 * the declaration, so those keep the fail-closed refusal. */
static bool declared_scalar_c_rep(const XrSemanticTypeRecord *type, XrCValueRep *out,
                                  const char **c_type) {
    if (!type || !out || !c_type || (type->flags & XR_SEM_TYPE_NULLABLE) != 0)
        return false;
    uint8_t native = type->scalar_rep;
    if (native == XR_SCALAR_REP_NONE) {
        switch (type->kind) {
            case XR_KIND_INT:
                native = XR_NATIVE_I64;
                break;
            case XR_KIND_FLOAT:
                native = XR_NATIVE_F64;
                break;
            case XR_KIND_BOOL:
                native = XR_NATIVE_BOOL;
                break;
            default:
                return false;
        }
    }
    switch ((XrNativeType) native) {
        case XR_NATIVE_I8:
            *out = XR_C_VALUE_REP_I8;
            *c_type = "int8_t";
            return true;
        case XR_NATIVE_U8:
            *out = XR_C_VALUE_REP_U8;
            *c_type = "uint8_t";
            return true;
        case XR_NATIVE_I16:
            *out = XR_C_VALUE_REP_I16;
            *c_type = "int16_t";
            return true;
        case XR_NATIVE_U16:
            *out = XR_C_VALUE_REP_U16;
            *c_type = "uint16_t";
            return true;
        case XR_NATIVE_I32:
            *out = XR_C_VALUE_REP_I32;
            *c_type = "int32_t";
            return true;
        case XR_NATIVE_U32:
            *out = XR_C_VALUE_REP_U32;
            *c_type = "uint32_t";
            return true;
        case XR_NATIVE_I64:
            *out = XR_C_VALUE_REP_I64;
            *c_type = "int64_t";
            return true;
        case XR_NATIVE_U64:
            *out = XR_C_VALUE_REP_U64;
            *c_type = "uint64_t";
            return true;
        case XR_NATIVE_ISIZE:
            *out = XR_C_VALUE_REP_ISIZE;
            *c_type = "ptrdiff_t";
            return true;
        case XR_NATIVE_USIZE:
            *out = XR_C_VALUE_REP_USIZE;
            *c_type = "size_t";
            return true;
        case XR_NATIVE_F32:
            *out = XR_C_VALUE_REP_F32;
            *c_type = "float";
            return true;
        case XR_NATIVE_F64:
            *out = XR_C_VALUE_REP_F64;
            *c_type = "double";
            return true;
        case XR_NATIVE_BOOL:
            *out = XR_C_VALUE_REP_BOOL;
            *c_type = "uint8_t";
            return true;
        default:
            return false;
    }
}

/* Whether this function's boundary may be stated by its own declaration.
 * An extern's convention comes from the foreign contract it binds to, never
 * from the Xray types written for it, so it is excluded. */
static bool function_states_own_boundary(const XrSemanticFunctionRecord *function) {
    return function && (function->flags & XR_SEM_FUNCTION_EXTERN) == 0;
}

/* The C spelling of the address of a local: untyped, because the plan records
 * the subject's type on the address itself and a spelling taken from that type
 * would name what the address points at. Each read and write through it states
 * the width it wants at the access.
 *
 * The projection is decided in one pass and checked in another, and the two
 * ask through different functions. Both call here so the answer cannot differ
 * between them. */
static const char *emission_local_address_c_type(const XrTargetPlan *target_plan,
                                                 uint32_t semantic_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    for (uint32_t i = 0; semantic && i < (uint32_t) xr_semantic_plan_operation_count(semantic);
         i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != semantic_value)
            continue;
        return xr_semantic_local_addr_is_exact(semantic, candidate, NULL) ? "void *" : NULL;
    }
    return NULL;
}

static bool machine_kind_to_c_rep(const XrTargetPlan *target_plan, uint32_t semantic_value,
                                  uint16_t kind, XrCValueRep *out, const char **c_type) {
    if (!out || !c_type)
        return false;
    switch (kind) {
        case XR_MACHINE_REP_VOID:
            *out = XR_C_VALUE_REP_VOID;
            *c_type = "void";
            return true;
        case XR_MACHINE_REP_I1:
            *out = XR_C_VALUE_REP_BOOL;
            *c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I8:
            *out = XR_C_VALUE_REP_I8;
            *c_type = "int8_t";
            return true;
        case XR_MACHINE_REP_U8:
            *out = XR_C_VALUE_REP_U8;
            *c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I16:
            *out = XR_C_VALUE_REP_I16;
            *c_type = "int16_t";
            return true;
        case XR_MACHINE_REP_U16:
            *out = XR_C_VALUE_REP_U16;
            *c_type = "uint16_t";
            return true;
        case XR_MACHINE_REP_I32:
            *out = XR_C_VALUE_REP_I32;
            *c_type = "int32_t";
            return true;
        case XR_MACHINE_REP_U32:
            *out = XR_C_VALUE_REP_U32;
            *c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_ENUM_ORDINAL:
            *out = XR_C_VALUE_REP_I64;
            *c_type = "int64_t";
            return true;
        case XR_MACHINE_REP_U64:
            *out = XR_C_VALUE_REP_U64;
            *c_type = "uint64_t";
            return true;
        case XR_MACHINE_REP_ISIZE:
            *out = XR_C_VALUE_REP_ISIZE;
            *c_type = "ptrdiff_t";
            return true;
        case XR_MACHINE_REP_USIZE:
            *out = XR_C_VALUE_REP_USIZE;
            *c_type = "size_t";
            return true;
        case XR_MACHINE_REP_F32:
            *out = XR_C_VALUE_REP_F32;
            *c_type = "float";
            return true;
        case XR_MACHINE_REP_F64:
            *out = XR_C_VALUE_REP_F64;
            *c_type = "double";
            return true;
        case XR_MACHINE_REP_RUNE:
            *out = XR_C_VALUE_REP_RUNE;
            *c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_DYN_VALUE:
            *out = XR_C_VALUE_REP_TAGGED;
            *c_type = "XrValue";
            return true;
        case XR_MACHINE_REP_VIEW:
            *out = XR_C_VALUE_REP_VIEW;
            *c_type = "xr_span_t";
            return true;
        case XR_MACHINE_REP_RAW_PTR:
            *out = XR_C_VALUE_REP_RAW_PTR;
            *c_type = emission_source_ref_place_c_type(target_plan, semantic_value);
            if (!*c_type)
                *c_type = emission_raw_pointer_c_type(target_plan, semantic_value);
            if (!*c_type) {
                const XrTargetValueRepRecord *binding =
                    xr_target_plan_value_rep(target_plan, semantic_value);
                uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
                if (exact_direct_local_tagged_ref_parameter_recipe(target_plan, binding, &storage))
                    *c_type = "XrValue *";
            }
            if (!*c_type)
                *c_type = emission_local_address_c_type(target_plan, semantic_value);
            return *c_type != NULL;
        default:
            return false;
    }
}

static bool emission_stable_id_is_zero(XrStableId id) {
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++) {
        if (id.bytes[i] != 0)
            return false;
    }
    return true;
}

static bool emission_identity_from_pair(const char *domain, XrStableId first, XrStableId second,
                                        uint32_t ordinal, XrStableId *out) {
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrFingerprint digest;
    if (!domain || !out)
        return false;
    xr_stable_id_hex(first, first_hex);
    xr_stable_id_hex(second, second_hex);
    int written = snprintf(key, sizeof(key), "%s:first=%s:second=%s:ordinal=%u", domain, first_hex,
                           second_hex, ordinal);
    return written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static const XrSemanticOperationRecord *binding_operation(const XrTargetPlan *target_plan,
                                                          const XrTargetValueRepRecord *binding) {
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    if (!binding || binding->slot >= slot_count || !slots)
        return NULL;
    uint32_t operation = slots[binding->slot].semantic_operation;
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    return operation < xr_semantic_plan_operation_count(semantic)
               ? xr_semantic_plan_operation(semantic, operation)
               : NULL;
}

static bool c_rep_is_addressable_scalar(uint16_t kind) {
    switch ((XrMachineRepKind) kind) {
        case XR_MACHINE_REP_I1:
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
        case XR_MACHINE_REP_RUNE:
        case XR_MACHINE_REP_ENUM_ORDINAL:
            return true;
        case XR_MACHINE_REP_VOID:
        case XR_MACHINE_REP_OBJECT_REF:
        case XR_MACHINE_REP_RAW_PTR:
        case XR_MACHINE_REP_CODE_REF:
        case XR_MACHINE_REP_DYN_VALUE:
        case XR_MACHINE_REP_AGGREGATE:
        case XR_MACHINE_REP_VECTOR:
        case XR_MACHINE_REP_VIEW:
        case XR_MACHINE_REP_COUNT:
            return false;
    }
    return false;
}

/* A scalar UNBOX that is representation-identical to its frozen source may
 * normally share that source's immutable C local.  An ordinary LOCAL_ADDR use
 * instead creates an exact storage obligation: the alias remains a distinct C
 * object, initialized directly from the same frozen scalar value.  This recipe
 * is derived only from SemanticPlan operation/value identities and TargetPlan
 * machine rows.  Raw/direct projections, aggregates, views, pointers, code and
 * vectors are deliberately outside this family. */
/* The address of a local. The subject it points at is the one operand, and the
 * emitter needs its identity rather than its type: the plan records the
 * subject's type on the address too, so the recipe carries the value and lets
 * each access state its own width. */
static bool exact_local_address_recipe(const XrTargetPlan *target_plan,
                                       const XrTargetValueRepRecord *binding,
                                       uint32_t *out_source_value) {
    if (out_source_value)
        *out_source_value = UINT32_MAX;
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *address = binding_operation(target_plan, binding);
    const XrSemanticOperandRecord *source = NULL;
    if (!semantic || !binding || !out_source_value ||
        !xr_semantic_local_addr_is_exact(semantic, address, &source) ||
        address->result_value != binding->semantic_value)
        return false;
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    if (!register_rep || !memory_rep || register_rep->kind != XR_MACHINE_REP_RAW_PTR ||
        memory_rep->kind != XR_MACHINE_REP_RAW_PTR ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        register_rep->root_kind != XR_TARGET_ROOT_NONE ||
        memory_rep->root_kind != XR_TARGET_ROOT_NONE)
        return false;
    *out_source_value = source->value;
    return true;
}

static bool exact_scalar_addressable_alias_recipe(const XrTargetPlan *target_plan,
                                                  const XrTargetValueRepRecord *binding,
                                                  uint32_t *out_source_value) {
    if (out_source_value)
        *out_source_value = UINT32_MAX;
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *alias = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    if (!semantic || !binding || !alias || !operands || !out_source_value ||
        alias->opcode != XI_UNBOX || alias->result_value != binding->semantic_value ||
        alias->result_value == XR_SEMANTIC_INDEX_NONE ||
        alias->function >= xr_semantic_plan_function_count(semantic) || alias->operand_count != 1 ||
        alias->metadata_count != 0 || alias->operand_begin >= operand_count ||
        alias->auxiliary_kind != XI_AUX_KIND_NONE || alias->semantic_immediate != 0 ||
        alias->effects != xi_generated_op_effects(XI_UNBOX) ||
        alias->flags != xi_generated_op_default_flags(XI_UNBOX))
        return false;

    uint32_t source_value = operands[alias->operand_begin].value;
    const XrTargetValueRepRecord *source = xr_target_plan_value_rep(target_plan, source_value);
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    if (!source || !register_rep || !memory_rep || source_value == alias->result_value ||
        binding->register_rep != source->register_rep ||
        binding->memory_rep != source->memory_rep || register_rep->kind != memory_rep->kind ||
        !c_rep_is_addressable_scalar(register_rep->kind))
        return false;

    bool address_taken = false;
    size_t operation_count = xr_semantic_plan_operation_count(semantic);
    if (operation_count > UINT32_MAX)
        return false;
    for (uint32_t i = 0; i < (uint32_t) operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(semantic, i);
        if (!use || use->opcode != XI_LOCAL_ADDR || use->function != alias->function ||
            use->operand_count != 1 || use->metadata_count != 0 ||
            use->operand_begin >= operand_count || use->auxiliary_kind != XI_AUX_KIND_NONE ||
            use->semantic_immediate != 0 ||
            use->effects != xi_generated_op_effects(XI_LOCAL_ADDR) ||
            use->flags != xi_generated_op_default_flags(XI_LOCAL_ADDR))
            continue;
        if (operands[use->operand_begin].value == alias->result_value) {
            address_taken = true;
            break;
        }
    }
    if (!address_taken)
        return false;
    *out_source_value = source_value;
    return true;
}

static bool exact_panic_catch_recipe(const XrTargetPlan *target_plan,
                                     const XrTargetValueRepRecord *binding) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    /* The operation and its result type are judged by the shared predicate the
     * builder, the target verifier, and the refinement oracle all read, so the
     * four layers cannot answer "is this a caught payload" differently.  What
     * stays here is the part only this layer knows: that the binding under
     * inspection is the one this very operation defines. */
    if (!binding || !xr_semantic_panic_catch_is_exact(semantic, operation) ||
        operation->result_value != binding->semantic_value)
        return false;
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetSlotRecord *slot = binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    uint32_t operation_index = slot ? slot->semantic_operation : XR_SEMANTIC_INDEX_NONE;
    return register_rep && memory_rep && slot && layout &&
           operation_index < xr_semantic_plan_operation_count(semantic) &&
           xr_semantic_plan_operation(semantic, operation_index) == operation &&
           register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           register_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           memory_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           register_rep->ownership == XR_TARGET_OWNERSHIP_OWNED &&
           memory_rep->ownership == XR_TARGET_OWNERSHIP_OWNED &&
           register_rep->null_encoding == XR_TARGET_NULL_TAGGED &&
           memory_rep->null_encoding == XR_TARGET_NULL_TAGGED &&
           register_rep->memory_size == memory_rep->memory_size &&
           register_rep->memory_align == memory_rep->memory_align &&
           layout->kind == XR_TARGET_LAYOUT_DYNAMIC && layout->field_count == 0 &&
           layout->root_field_count == 0 && layout->fixed_prefix_size == memory_rep->memory_size &&
           layout->align == memory_rep->memory_align &&
           slot->semantic_value == binding->semantic_value &&
           slot->function == operation->function && slot->role == XR_TARGET_SLOT_TEMPORARY &&
           slot->register_rep == binding->register_rep && slot->memory_rep == binding->memory_rep &&
           slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           slot->ownership == XR_TARGET_OWNERSHIP_OWNED;
}

static bool exact_string_concat_recipe(const XrTargetPlan *target_plan,
                                       const XrTargetValueRepRecord *binding,
                                       const XrSemanticOperandRecord **arguments,
                                       uint16_t *argument_count) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    if (arguments)
        *arguments = NULL;
    if (argument_count)
        *argument_count = 0;
    if (!semantic || !operation || !binding || !operands ||
        !xr_semantic_string_concat_is_exact(semantic, operation) ||
        operation->result_value != binding->semantic_value ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    if (arguments)
        *arguments = &operands[operation->operand_begin];
    if (argument_count)
        *argument_count = operation->operand_count;
    return true;
}

/* Freeze one ordered concat operand and the exact C value that renders it.
 * SemanticPlan retains the logical interpolation operand and its exact display
 * shape. TargetPlan independently owns that value's machine representation, so
 * native-u64 selection is an O(1) value-rep lookup rather than an
 * IR/type/selector guess. */
static bool string_concat_argument_recipe(const XrTargetPlan *target_plan, uint32_t semantic_value,
                                          uint8_t *kind, uint32_t *source_semantic_value) {
    if (kind)
        *kind = XR_C_RECIPE_ARGUMENT_INVALID;
    if (source_semantic_value)
        *source_semantic_value = UINT32_MAX;
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(target_plan, binding->memory_rep) : NULL;
    if (!target_plan || !kind || !source_semantic_value || !binding || !register_rep ||
        !memory_rep || register_rep->kind != memory_rep->kind)
        return false;
    switch ((XrMachineRepKind) register_rep->kind) {
        case XR_MACHINE_REP_DYN_VALUE:
            *kind = XR_C_RECIPE_ARGUMENT_STRING_VALUE;
            break;
        case XR_MACHINE_REP_U64:
            *kind = XR_C_RECIPE_ARGUMENT_STRING_DIRECT_U64;
            break;
        case XR_MACHINE_REP_I64:
            *kind = XR_C_RECIPE_ARGUMENT_STRING_DIRECT_I64;
            break;
        default:
            return false;
    }
    *source_semantic_value = semantic_value;
    return true;
}

static bool exact_adt_enum_constructor_recipe(const XrTargetPlan *target_plan,
                                              const XrTargetValueRepRecord *binding,
                                              XrSemanticAdtEnumConstructorShape *shape,
                                              const XrSemanticOperandRecord **payloads) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    XrSemanticAdtEnumConstructorShape derived = {0};
    const XrTargetCallRecord *call = NULL;

    if (shape)
        memset(shape, 0, sizeof(*shape));
    if (payloads)
        *payloads = NULL;
    if (!target_plan || !binding || !semantic || !operation || !operands || !calls ||
        operation->result_value != binding->semantic_value ||
        !xr_semantic_adt_enum_constructor_is_exact(semantic, operation, &derived) ||
        derived.payload_operand_begin > operand_count ||
        derived.payload_count > operand_count - derived.payload_operand_begin)
        return false;
    for (uint32_t i = 0; i < call_count; i++) {
        if (xr_semantic_plan_operation(semantic, calls[i].semantic_operation) != operation)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    if (!call || call->result_value != binding->semantic_value ||
        call->result_slot != binding->slot || call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep || call->argument_count != 0 ||
        call->adapter_count != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_ADT_ENUM_CONSTRUCTOR ||
        call->target_kind != XR_TARGET_CALL_TARGET_ADT_ENUM_CONSTRUCTOR ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_RETURN_OWNED || !register_rep || !memory_rep ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE)
        return false;
    if (shape)
        *shape = derived;
    if (payloads)
        *payloads = &operands[derived.payload_operand_begin];
    return true;
}

static bool c_array_storage_from_semantic(uint8_t semantic_storage, uint8_t *target_storage) {
    if (!target_storage)
        return false;
    switch (semantic_storage) {
        case XR_ELEM_I8:
            *target_storage = XR_TARGET_ARRAY_STORAGE_I8;
            return true;
        case XR_ELEM_U8:
            *target_storage = XR_TARGET_ARRAY_STORAGE_U8;
            return true;
        case XR_ELEM_I16:
            *target_storage = XR_TARGET_ARRAY_STORAGE_I16;
            return true;
        case XR_ELEM_U16:
            *target_storage = XR_TARGET_ARRAY_STORAGE_U16;
            return true;
        case XR_ELEM_I32:
            *target_storage = XR_TARGET_ARRAY_STORAGE_I32;
            return true;
        case XR_ELEM_U32:
            *target_storage = XR_TARGET_ARRAY_STORAGE_U32;
            return true;
        case XR_ELEM_I64:
            *target_storage = XR_TARGET_ARRAY_STORAGE_I64;
            return true;
        case XR_ELEM_U64:
            *target_storage = XR_TARGET_ARRAY_STORAGE_U64;
            return true;
        case XR_ELEM_F32:
            *target_storage = XR_TARGET_ARRAY_STORAGE_F32;
            return true;
        case XR_ELEM_F64:
            *target_storage = XR_TARGET_ARRAY_STORAGE_F64;
            return true;
        case XR_ELEM_BOOL:
            *target_storage = XR_TARGET_ARRAY_STORAGE_BOOL;
            return true;
        case XR_ELEM_RUNE:
            *target_storage = XR_TARGET_ARRAY_STORAGE_RUNE;
            return true;
        default:
            return false;
    }
}

static bool c_array_storage_projection(uint8_t storage, uint16_t *machine_kind, XrCValueRep *c_rep,
                                       const char **c_type) {
    uint16_t kind = XR_MACHINE_REP_COUNT;
    XrCValueRep rep = XR_C_VALUE_REP_COUNT;
    const char *type = NULL;
    switch (storage) {
        case XR_TARGET_ARRAY_STORAGE_I8:
            kind = XR_MACHINE_REP_I8;
            rep = XR_C_VALUE_REP_I8;
            type = "int8_t";
            break;
        case XR_TARGET_ARRAY_STORAGE_U8:
            kind = XR_MACHINE_REP_U8;
            rep = XR_C_VALUE_REP_U8;
            type = "uint8_t";
            break;
        case XR_TARGET_ARRAY_STORAGE_I16:
            kind = XR_MACHINE_REP_I16;
            rep = XR_C_VALUE_REP_I16;
            type = "int16_t";
            break;
        case XR_TARGET_ARRAY_STORAGE_U16:
            kind = XR_MACHINE_REP_U16;
            rep = XR_C_VALUE_REP_U16;
            type = "uint16_t";
            break;
        case XR_TARGET_ARRAY_STORAGE_I32:
            kind = XR_MACHINE_REP_I32;
            rep = XR_C_VALUE_REP_I32;
            type = "int32_t";
            break;
        case XR_TARGET_ARRAY_STORAGE_U32:
            kind = XR_MACHINE_REP_U32;
            rep = XR_C_VALUE_REP_U32;
            type = "uint32_t";
            break;
        case XR_TARGET_ARRAY_STORAGE_I64:
            kind = XR_MACHINE_REP_I64;
            rep = XR_C_VALUE_REP_I64;
            type = "int64_t";
            break;
        case XR_TARGET_ARRAY_STORAGE_U64:
            kind = XR_MACHINE_REP_U64;
            rep = XR_C_VALUE_REP_U64;
            type = "uint64_t";
            break;
        case XR_TARGET_ARRAY_STORAGE_F32:
            kind = XR_MACHINE_REP_F32;
            rep = XR_C_VALUE_REP_F32;
            type = "float";
            break;
        case XR_TARGET_ARRAY_STORAGE_F64:
            kind = XR_MACHINE_REP_F64;
            rep = XR_C_VALUE_REP_F64;
            type = "double";
            break;
        case XR_TARGET_ARRAY_STORAGE_BOOL:
            kind = XR_MACHINE_REP_I1;
            rep = XR_C_VALUE_REP_BOOL;
            type = "uint8_t";
            break;
        case XR_TARGET_ARRAY_STORAGE_RUNE:
            kind = XR_MACHINE_REP_RUNE;
            rep = XR_C_VALUE_REP_RUNE;
            type = "uint32_t";
            break;
        default:
            return false;
    }
    if (machine_kind)
        *machine_kind = kind;
    if (c_rep)
        *c_rep = rep;
    if (c_type)
        *c_type = type;
    return true;
}

static bool c_array_storage_from_type(const XrSemanticTypeRecord *type, uint8_t *target_storage) {
    if (!type || !target_storage || type->builtin_type != XR_TID_NULL || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->flags != 0)
        return false;
    if (type->kind == XR_KIND_BOOL && type->scalar_rep == XR_SCALAR_REP_NONE) {
        *target_storage = XR_TARGET_ARRAY_STORAGE_BOOL;
        return true;
    }
    if (type->kind == XR_KIND_RUNE && type->scalar_rep == XR_SCALAR_REP_NONE) {
        *target_storage = XR_TARGET_ARRAY_STORAGE_RUNE;
        return true;
    }
    if (type->kind != XR_KIND_INT && type->kind != XR_KIND_FLOAT)
        return false;
    switch (type->scalar_rep) {
        case XR_NATIVE_I8:
            *target_storage = XR_TARGET_ARRAY_STORAGE_I8;
            return true;
        case XR_NATIVE_U8:
            *target_storage = XR_TARGET_ARRAY_STORAGE_U8;
            return true;
        case XR_NATIVE_I16:
            *target_storage = XR_TARGET_ARRAY_STORAGE_I16;
            return true;
        case XR_NATIVE_U16:
            *target_storage = XR_TARGET_ARRAY_STORAGE_U16;
            return true;
        case XR_NATIVE_I32:
            *target_storage = XR_TARGET_ARRAY_STORAGE_I32;
            return true;
        case XR_NATIVE_U32:
            *target_storage = XR_TARGET_ARRAY_STORAGE_U32;
            return true;
        case XR_NATIVE_I64:
            *target_storage = XR_TARGET_ARRAY_STORAGE_I64;
            return true;
        case XR_NATIVE_U64:
            *target_storage = XR_TARGET_ARRAY_STORAGE_U64;
            return true;
        case XR_NATIVE_F32:
            *target_storage = XR_TARGET_ARRAY_STORAGE_F32;
            return true;
        case XR_NATIVE_F64:
            *target_storage = XR_TARGET_ARRAY_STORAGE_F64;
            return true;
        default:
            return false;
    }
}

static bool c_array_fill_type_is_exact(const XrSemanticTypeRecord *type, uint8_t element_storage) {
    uint8_t ignored_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!type)
        return false;
    if (element_storage == XR_TARGET_ARRAY_STORAGE_RUNE)
        return type->kind == XR_KIND_RUNE && c_array_storage_from_type(type, &ignored_storage);
    return element_storage > XR_TARGET_ARRAY_STORAGE_NONE &&
           element_storage < XR_TARGET_ARRAY_STORAGE_RUNE &&
           (type->kind == XR_KIND_INT || type->kind == XR_KIND_FLOAT ||
            type->kind == XR_KIND_BOOL) &&
           c_array_storage_from_type(type, &ignored_storage);
}

/* A source Array<T>(count) allocation is a distinct family from the compiler
 * Array intrinsics below.  Its C recipe is admitted only when the ordered
 * scalar count operand, owned allocation result, Target slot, and the Array
 * layout's dedicated element-storage byte all agree. */
static bool exact_array_allocation_recipe(const XrTargetPlan *target_plan,
                                          const XrTargetValueRepRecord *binding, uint8_t *storage,
                                          uint32_t *count_value, const char **symbol) {
    const XrSemanticPlan *semantic = target_plan ? xr_target_plan_semantic_plan(target_plan) : NULL;
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    uint32_t child_count = 0;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    const XrSemanticTypeRecord *array =
        operation && semantic ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    const XrSemanticTypeRecord *element =
        array && children && array->child_count == 1 && array->child_begin < child_count
            ? xr_semantic_plan_type(semantic, children[array->child_begin])
            : NULL;
    const XrSemanticOperandRecord *count = operation && operands && operation->operand_count == 1 &&
                                                   operation->operand_begin < operand_count
                                               ? &operands[operation->operand_begin]
                                               : NULL;
    const XrSemanticTypeRecord *count_type =
        count && semantic ? xr_semantic_plan_type(semantic, count->type) : NULL;
    uint8_t semantic_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t type_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    bool source_class_element =
        xr_semantic_class_instance_type_source_class(semantic, element) != XR_SEMANTIC_INDEX_NONE;
    bool storage_exact = false;
    if (source_class_element) {
        semantic_storage = XR_TARGET_ARRAY_STORAGE_TAGGED;
        type_storage = XR_TARGET_ARRAY_STORAGE_TAGGED;
        storage_exact = operation && operation->array_element_storage == XR_ELEM_ANY;
    } else {
        storage_exact = operation &&
                        c_array_storage_from_semantic(operation->array_element_storage,
                                                      &semantic_storage) &&
                        c_array_storage_from_type(element, &type_storage) &&
                        semantic_storage == type_storage;
    }
    if (!target_plan || !binding || !semantic || !operation || !operands || !children || !array ||
        !element || !count || !count_type || operation->opcode != XI_ARRAY_NEW ||
        operation->result_value != binding->semantic_value ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE || operation->operand_count != 1 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->metadata_count != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->semantic_immediate != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_ARRAY_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_ARRAY_NEW) ||
        operation->ownership_use != xi_generated_op_own_use(XI_ARRAY_NEW) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 ||
        !xr_semantic_allocation_identity_is_canonical(operation) ||
        !storage_exact ||
        array->kind != XR_KIND_ARRAY || array->builtin_type != XR_TID_NULL ||
        array->scalar_rep != XR_SCALAR_REP_NONE || array->aggregate_extent != 0 ||
        array->aggregate_align != 0 ||
        array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        count_type->kind != XR_KIND_INT || count_type->builtin_type != XR_TID_NULL ||
        count_type->child_count != 0 || count_type->aggregate_extent != 0 ||
        count_type->aggregate_align != 0 || count_type->scalar_rep != XR_NATIVE_I64 ||
        count_type->flags != 0 || count->role != XR_SEM_OPERAND_VALUE || count->parameter != -1 ||
        count->flags != 0 || count->ownership_action != XR_SEM_OPERAND_CONSUME)
        return false;
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetSlotRecord *slot = binding->slot < slot_count ? &slots[binding->slot] : NULL;
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; layouts && i < layout_count; i++) {
        if (layouts[i].semantic_type != operation->result_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
    }
    if (!register_rep || !memory_rep || !slot || !layout ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        slot->semantic_value != binding->semantic_value ||
        slot->semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
        xr_semantic_plan_operation(semantic, slot->semantic_operation) != operation ||
        slot->function != operation->function || slot->role != XR_TARGET_SLOT_TEMPORARY ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC || slot->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        layout->kind != XR_TARGET_LAYOUT_DYNAMIC ||
        layout->array_element_storage != semantic_storage || layout->field_count != 0 ||
        layout->root_field_count != 0 || layout->fixed_prefix_size != memory_rep->memory_size ||
        layout->align != memory_rep->memory_align)
        return false;
    if (storage)
        *storage = semantic_storage;
    if (count_value)
        *count_value = count->value;
    if (symbol)
        *symbol = XR_C_ARRAY_NEW_SYMBOL;
    return true;
}

/* Frozen Target authority for the two Array allocation recipes.  The recipe
 * contains the ordered semantic operands plus the exact storage enum; it does
 * not admit selector, result-type, or packed-immediate reconstruction. */
static bool exact_array_intrinsic_recipe(const XrTargetPlan *target_plan,
                                         const XrTargetValueRepRecord *binding,
                                         uint8_t *materialization, uint8_t *storage,
                                         uint32_t *count_value, uint32_t *fill_value,
                                         const char **symbol) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    uint32_t child_count = 0;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    bool with_capacity =
        operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_WITH_CAPACITY;
    bool filled = operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_FILLED_NEW;
    uint16_t expected_count = with_capacity ? 1u : 2u;
    uint8_t expected_kind = with_capacity ? XR_TARGET_ARRAY_INTRINSIC_WITH_CAPACITY
                                          : XR_TARGET_ARRAY_INTRINSIC_FILLED_NEW;
    uint8_t expected_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t semantic_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    const XrSemanticTypeRecord *array =
        operation && semantic ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    const XrSemanticTypeRecord *element =
        array && children && array->child_count == 1 && array->child_begin < child_count
            ? xr_semantic_plan_type(semantic, children[array->child_begin])
            : NULL;
    if (!target_plan || !binding || !semantic || !operation || !operands || !children || !array ||
        !element || (!with_capacity && !filled) || operation->opcode != XI_CALL_BUILTIN ||
        operation->result_value != binding->semantic_value ||
        operation->operand_count != expected_count || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->semantic_immediate != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 ||
        !xr_semantic_allocation_identity_is_canonical(operation) ||
        !c_array_storage_from_semantic(operation->array_element_storage, &semantic_storage) ||
        !c_array_storage_from_type(element, &expected_storage) ||
        expected_storage != semantic_storage || array->kind != XR_KIND_ARRAY ||
        array->builtin_type != XR_TID_NULL || array->scalar_rep != XR_SCALAR_REP_NONE ||
        array->aggregate_extent != 0 || array->aggregate_align != 0 ||
        array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const XrSemanticOperandRecord *count = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *fill = filled ? count + 1 : NULL;
    const XrSemanticTypeRecord *count_type = xr_semantic_plan_type(semantic, count->type);
    const XrSemanticTypeRecord *fill_type =
        fill ? xr_semantic_plan_type(semantic, fill->type) : NULL;
    if (!count_type || count_type->kind != XR_KIND_INT || count_type->builtin_type != XR_TID_NULL ||
        count_type->child_count != 0 || count_type->aggregate_extent != 0 ||
        count_type->aggregate_align != 0 || count_type->scalar_rep != XR_NATIVE_I64 ||
        count_type->flags != 0 || count->role != XR_SEM_OPERAND_ARGUMENT || count->parameter != 0 ||
        count->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        count->ownership_action != XR_SEM_OPERAND_CONSUME ||
        (filled && (!c_array_fill_type_is_exact(fill_type, expected_storage) ||
                    fill->role != XR_SEM_OPERAND_ARGUMENT || fill->parameter != 1 ||
                    fill->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
                    fill->ownership_action != XR_SEM_OPERAND_CONSUME)))
        return false;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (xr_semantic_plan_operation(semantic, calls[i].semantic_operation) != operation)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target_plan, &argument_count);
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    XrStableId expected_call;
    uint32_t discriminator = ((uint32_t) expected_kind << 8) | expected_storage;
    if (!call || !arguments || !register_rep || !memory_rep ||
        !emission_identity_from_pair("xray-target-array-intrinsic-v1", operation->id,
                                     operation->allocation_id, discriminator, &expected_call) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !emission_stable_id_is_zero(call->source_export_identity) ||
        !emission_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != binding->semantic_value || call->result_slot != binding->slot ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep || call->argument_count != expected_count ||
        call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin || call->adapter_count != 0 ||
        call->flags != 0 || call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_INTRINSIC ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_INTRINSIC ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
        call->array_intrinsic_kind != expected_kind ||
        call->array_element_storage != expected_storage || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0 ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE)
        return false;
    for (uint16_t ordinal = 0; ordinal < expected_count; ordinal++) {
        uint32_t semantic_operand = operation->operand_begin + ordinal;
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, operand->type);
        const XrTargetValueRepRecord *caller =
            xr_target_plan_value_rep(target_plan, operand->value);
        const XrTargetCallArgumentRecord *argument = &arguments[call->argument_begin + ordinal];
        XrStableId expected_argument;
        if (!type || !caller ||
            !emission_identity_from_pair("xray-target-array-intrinsic-argument-v1", operation->id,
                                         type->id, ordinal, &expected_argument) ||
            !xr_stable_id_equal(argument->identity, expected_argument) ||
            argument->call != call->id || argument->semantic_operand != semantic_operand ||
            argument->semantic_value != operand->value ||
            argument->callee_parameter != XR_SEMANTIC_INDEX_NONE ||
            argument->caller_slot != caller->slot ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
            argument->register_rep != caller->register_rep ||
            argument->memory_rep != caller->memory_rep ||
            argument->callee_register_rep != caller->register_rep ||
            argument->callee_memory_rep != caller->memory_rep || argument->ordinal != ordinal ||
            argument->mode != XR_TARGET_CALL_VALUE ||
            argument->ownership != XR_TARGET_CALL_CONSUME ||
            argument->transfer_mode != operand->transfer_mode || argument->flags != 0)
            return false;
    }
    if (materialization)
        *materialization = with_capacity ? XR_C_VALUE_MATERIALIZATION_ARRAY_WITH_CAPACITY
                                         : XR_C_VALUE_MATERIALIZATION_ARRAY_FILLED_NEW;
    if (storage)
        *storage = expected_storage;
    if (count_value)
        *count_value = count->value;
    if (fill_value)
        *fill_value = fill ? fill->value : UINT32_MAX;
    if (symbol)
        *symbol = with_capacity ? XR_C_ARRAY_WITH_CAPACITY_SYMBOL : XR_C_ARRAY_FILLED_NEW_SYMBOL;
    return true;
}

/* Array.fill retains XI_CALL_METHOD for VM execution, but its C recipe is
 * admitted only by the dedicated Semantic and Target identities. Metadata is
 * structurally checked and never inspected for a selector spelling. */
static bool exact_array_fill_scalar_recipe(const XrTargetPlan *target_plan,
                                           const XrTargetValueRepRecord *binding, uint8_t *storage,
                                           uint32_t *receiver_value, uint32_t *fill_value) {
    const XrSemanticPlan *semantic = target_plan ? xr_target_plan_semantic_plan(target_plan) : NULL;
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0, child_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    if (semantic)
        (void) xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!target_plan || !binding || !semantic || !operation || !operands || !children ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_FILL_SCALAR ||
        operation->opcode != XI_CALL_METHOD || operation->result_value != binding->semantic_value ||
        operation->operand_count != 2 || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->semantic_immediate != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *fill = receiver + 1;
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(semantic, receiver->type);
    if (!array || array->kind != XR_KIND_ARRAY || array->builtin_type != XR_TID_NULL ||
        array->child_count != 1 || array->child_begin >= child_count ||
        array->scalar_rep != XR_SCALAR_REP_NONE || array->aggregate_extent != 0 ||
        array->aggregate_align != 0 ||
        array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    uint32_t element_index = children[array->child_begin];
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(semantic, element_index);
    uint8_t expected_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t semantic_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!element || operation->result_type != receiver->type || fill->type != element_index ||
        !c_array_storage_from_type(element, &expected_storage) ||
        !c_array_storage_from_semantic(operation->array_element_storage, &semantic_storage) ||
        expected_storage != semantic_storage || receiver->role != XR_SEM_OPERAND_RECEIVER ||
        receiver->parameter != -1 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        fill->role != XR_SEM_OPERAND_ARGUMENT || fill->parameter != 0 ||
        fill->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        fill->ownership_action != XR_SEM_OPERAND_CONSUME)
        return false;
    uint32_t call_count = 0, argument_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target_plan, &argument_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (xr_semantic_plan_operation(semantic, calls[i].semantic_operation) != operation)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    XrStableId expected_call;
    if (!call || !arguments || !register_rep || !memory_rep ||
        !emission_identity_from_pair("xray-target-array-fill-scalar-v1", operation->id, array->id,
                                     expected_storage, &expected_call) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !emission_stable_id_is_zero(call->source_export_identity) ||
        !emission_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != binding->semantic_value || call->result_slot != binding->slot ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep || call->argument_count != 2 ||
        call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin || call->adapter_count != 0 ||
        call->flags != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_FILL_SCALAR ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_FILL_SCALAR ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != expected_storage || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0 ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE)
        return false;
    for (uint16_t ordinal = 0; ordinal < 2; ordinal++) {
        uint32_t semantic_operand = operation->operand_begin + ordinal;
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, operand->type);
        const XrTargetValueRepRecord *caller =
            xr_target_plan_value_rep(target_plan, operand->value);
        const XrTargetCallArgumentRecord *argument = &arguments[call->argument_begin + ordinal];
        XrStableId expected_argument;
        if (!type || !caller ||
            !emission_identity_from_pair("xray-target-array-fill-scalar-argument-v1", operation->id,
                                         type->id, ordinal, &expected_argument) ||
            !xr_stable_id_equal(argument->identity, expected_argument) ||
            argument->call != call->id || argument->semantic_operand != semantic_operand ||
            argument->semantic_value != operand->value ||
            argument->callee_parameter != XR_SEMANTIC_INDEX_NONE ||
            argument->caller_slot != caller->slot ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
            argument->register_rep != caller->register_rep ||
            argument->memory_rep != caller->memory_rep ||
            argument->callee_register_rep != caller->register_rep ||
            argument->callee_memory_rep != caller->memory_rep || argument->ordinal != ordinal ||
            argument->mode != XR_TARGET_CALL_VALUE ||
            argument->ownership !=
                (ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME) ||
            argument->transfer_mode != operand->transfer_mode || argument->flags != 0 ||
            argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
            argument->reserved8[0] != 0 || argument->reserved8[1] != 0 ||
            argument->reserved8[2] != 0)
            return false;
    }
    if (storage)
        *storage = expected_storage;
    if (receiver_value)
        *receiver_value = receiver->value;
    if (fill_value)
        *fill_value = fill->value;
    return true;
}

typedef struct XrCArrayHofDirectRecipe {
    uint32_t callee_function;
    uint32_t receiver_value;
    uint32_t callback_value;
    uint32_t seed_value;
    uint8_t kind;
    uint8_t source_storage;
    uint8_t result_storage;
    uint8_t parameter_reps[2];
    uint8_t return_rep;
} XrCArrayHofDirectRecipe;

/* A direct Array HOF recipe is admitted only after independently rebuilding
 * the complete SemanticPlan -> TargetPlan identity.  The callback remains an
 * ordinary tagged closure in the VM-facing IR; CGen may elide that object only
 * because this row freezes one same-module, uncaptured, pure native callee. */
static bool exact_array_hof_direct_recipe(const XrTargetPlan *target_plan,
                                          const XrTargetValueRepRecord *binding,
                                          XrCArrayHofDirectRecipe *out) {
    XrCArrayHofDirectRecipe recipe = {
        .callee_function = UINT32_MAX,
        .receiver_value = UINT32_MAX,
        .callback_value = UINT32_MAX,
        .seed_value = UINT32_MAX,
        .kind = XR_C_ARRAY_HOF_NONE,
        .source_storage = XR_TARGET_ARRAY_STORAGE_NONE,
        .result_storage = XR_TARGET_ARRAY_STORAGE_NONE,
        .parameter_reps = {XR_C_VALUE_REP_VOID, XR_C_VALUE_REP_VOID},
        .return_rep = XR_C_VALUE_REP_VOID,
    };
    const XrSemanticPlan *semantic = target_plan ? xr_target_plan_semantic_plan(target_plan) : NULL;
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0, child_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    if (semantic)
        (void) xr_semantic_plan_metadata(semantic, &metadata_count);
    uint8_t kind = XR_C_ARRAY_HOF_NONE;
    if (operation) {
        switch (operation->array_hof_kind) {
            case XR_SEM_ARRAY_HOF_MAP:
                kind = XR_C_ARRAY_HOF_MAP;
                break;
            case XR_SEM_ARRAY_HOF_FILTER:
                kind = XR_C_ARRAY_HOF_FILTER;
                break;
            case XR_SEM_ARRAY_HOF_REDUCE:
                kind = XR_C_ARRAY_HOF_REDUCE;
                break;
            default:
                break;
        }
    }
    uint16_t expected_operands = kind == XR_C_ARRAY_HOF_REDUCE ? 3u : 2u;
    if (!target_plan || !binding || !semantic || !operation || !operands || !children ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_HOF || kind == XR_C_ARRAY_HOF_NONE ||
        operation->opcode != XI_CALL_METHOD || operation->result_value != binding->semantic_value ||
        operation->operand_count != expected_operands || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->semantic_immediate != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function >= xr_semantic_plan_function_count(semantic) ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1)
        return false;

    const XrSemanticOperandRecord *rows = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *source_array = xr_semantic_plan_type(semantic, rows[0].type);
    if (!source_array || source_array->kind != XR_KIND_ARRAY ||
        source_array->builtin_type != XR_TID_NULL || source_array->child_count != 1 ||
        source_array->child_begin >= child_count ||
        source_array->scalar_rep != XR_SCALAR_REP_NONE || source_array->aggregate_extent != 0 ||
        source_array->aggregate_align != 0 ||
        source_array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    uint32_t source_element = children[source_array->child_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(semantic, source_element);
    uint8_t source_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t frozen_source = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!source_type || !c_array_storage_from_type(source_type, &source_storage) ||
        !c_array_storage_from_semantic(operation->array_element_storage, &frozen_source) ||
        source_storage != frozen_source)
        return false;

    uint32_t result_element = operation->result_type;
    if (kind != XR_C_ARRAY_HOF_REDUCE) {
        const XrSemanticTypeRecord *result_array =
            xr_semantic_plan_type(semantic, operation->result_type);
        if (!result_array || result_array->kind != XR_KIND_ARRAY ||
            result_array->builtin_type != XR_TID_NULL || result_array->child_count != 1 ||
            result_array->child_begin >= child_count ||
            result_array->scalar_rep != XR_SCALAR_REP_NONE || result_array->aggregate_extent != 0 ||
            result_array->aggregate_align != 0 ||
            result_array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
            return false;
        result_element = children[result_array->child_begin];
        if (kind == XR_C_ARRAY_HOF_FILTER &&
            (operation->result_type != rows[0].type || result_element != source_element))
            return false;
    } else if (rows[2].type != operation->result_type) {
        return false;
    }
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(semantic, result_element);
    uint8_t result_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t frozen_result = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!result_type || !c_array_storage_from_type(result_type, &result_storage) ||
        !c_array_storage_from_semantic(operation->array_result_element_storage, &frozen_result) ||
        result_storage != frozen_result)
        return false;

    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(semantic, operation->callable_function);
    uint16_t expected_parameters = kind == XR_C_ARRAY_HOF_REDUCE ? 2u : 1u;
    if (!callee || callee->parent != operation->function || callee->capture_count != 0 ||
        callee->parameter_count != expected_parameters ||
        (callee->semantic_effects & (XI_EFFECT_SIDE_EFFECT | XI_EFFECT_MEMORY_WRITE |
                                     XI_EFFECT_MAY_THROW | XI_EFFECT_MAY_SUSPEND)) != 0 ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(semantic) ||
        callee->parameter_count >
            xr_semantic_plan_parameter_count(semantic) - callee->parameter_begin)
        return false;
    const XrSemanticParameterRecord *first =
        xr_semantic_plan_parameter(semantic, callee->parameter_begin);
    const XrSemanticParameterRecord *second =
        expected_parameters == 2u
            ? xr_semantic_plan_parameter(semantic, callee->parameter_begin + 1u)
            : NULL;
    if (!first || first->function != operation->callable_function || first->ordinal != 0 ||
        first->type != (kind == XR_C_ARRAY_HOF_REDUCE ? result_element : source_element) ||
        (second && (second->function != operation->callable_function || second->ordinal != 1 ||
                    second->type != source_element)))
        return false;
    if (kind == XR_C_ARRAY_HOF_FILTER) {
        const XrSemanticTypeRecord *return_type =
            xr_semantic_plan_type(semantic, callee->return_type);
        uint16_t return_kind = XR_MACHINE_REP_COUNT;
        XrCValueRep return_c_rep = XR_C_VALUE_REP_COUNT;
        const char *return_c_type = NULL;
        if (!return_type || return_type->kind != XR_KIND_BOOL ||
            return_type->scalar_rep != XR_SCALAR_REP_NONE ||
            !c_array_storage_projection(XR_TARGET_ARRAY_STORAGE_BOOL, &return_kind, &return_c_rep,
                                        &return_c_type) ||
            return_kind != XR_MACHINE_REP_I1 || return_c_rep != XR_C_VALUE_REP_BOOL)
            return false;
    } else if (callee->return_type != result_element) {
        return false;
    }
    const XrSemanticTypeRecord *callback_type = xr_semantic_plan_type(semantic, rows[1].type);
    if (!callback_type || callback_type->kind != XR_KIND_FUNCTION ||
        callback_type->child_count != (uint32_t) expected_parameters + 1u ||
        callback_type->child_begin > child_count ||
        callback_type->child_count > child_count - callback_type->child_begin)
        return false;
    for (uint16_t i = 0; i < expected_parameters; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, callee->parameter_begin + i);
        if (!parameter || children[callback_type->child_begin + i] != parameter->type)
            return false;
    }
    if (children[callback_type->child_begin + expected_parameters] != callee->return_type ||
        rows[0].role != XR_SEM_OPERAND_RECEIVER || rows[0].parameter != -1 ||
        rows[0].flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        rows[0].ownership_action != XR_SEM_OPERAND_BORROW ||
        rows[1].role != XR_SEM_OPERAND_ARGUMENT || rows[1].parameter != 0 ||
        rows[1].flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        rows[1].ownership_action != XR_SEM_OPERAND_CONSUME ||
        (kind == XR_C_ARRAY_HOF_REDUCE &&
         (rows[2].role != XR_SEM_OPERAND_ARGUMENT || rows[2].parameter != 1 ||
          rows[2].flags != XR_SEM_OPERAND_CALL_CONTRACT ||
          rows[2].ownership_action != XR_SEM_OPERAND_CONSUME)))
        return false;
    bool result_exact = kind == XR_C_ARRAY_HOF_REDUCE
                            ? operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
                                  operation->return_provenance == XR_SEM_RETURN_NONE &&
                                  operation->return_complete == 0
                            : operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                                  operation->return_provenance == XR_SEM_RETURN_OWNED &&
                                  operation->return_complete == 1;
    const XrSemanticOperationRecord *producer = NULL;
    uint32_t callback_uses = 0;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; result_exact && i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->function != operation->function ||
            candidate->result_value != rows[1].value)
            continue;
        if (producer)
            return false;
        producer = candidate;
    }
    for (uint32_t i = 0; result_exact && i < operand_count; i++) {
        if (operands[i].value == rows[1].value && callback_uses != UINT32_MAX)
            callback_uses++;
    }
    if (!result_exact || !producer || producer >= operation || callback_uses != 1 ||
        (producer->opcode != XI_CLOSURE_NEW &&
         (producer->opcode != XI_STACK_ALLOC || producer->semantic_immediate != XI_CLOSURE_NEW)) ||
        producer->callable_function != operation->callable_function ||
        producer->result_type != rows[1].type)
        return false;

    uint32_t call_count = 0, argument_count = 0, slot_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target_plan, &argument_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (xr_semantic_plan_operation(semantic, calls[i].semantic_operation) != operation)
            continue;
        if (call)
            return false;
        call = &calls[i];
    }
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    const XrTargetSlotRecord *result_slot =
        slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    XrStableId expected_call;
    uint32_t discriminator =
        ((uint32_t) kind << 16) | ((uint32_t) source_storage << 8) | result_storage;
    uint8_t target_kind = kind == XR_C_ARRAY_HOF_MAP      ? XR_TARGET_ARRAY_HOF_MAP
                          : kind == XR_C_ARRAY_HOF_FILTER ? XR_TARGET_ARRAY_HOF_FILTER
                                                          : XR_TARGET_ARRAY_HOF_REDUCE;
    if (!call || !arguments || !slots || !register_rep || !memory_rep || !result_slot ||
        !emission_identity_from_pair("xray-target-array-hof-v1", operation->id, callee->id,
                                     discriminator, &expected_call) ||
        !xr_stable_id_equal(call->identity, expected_call) ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->caller_function != operation->function ||
        call->callee_function != operation->callable_function ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !emission_stable_id_is_zero(call->source_export_identity) ||
        !emission_stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != binding->semantic_value || call->result_slot != binding->slot ||
        call->result_register_rep != binding->register_rep ||
        call->result_memory_rep != binding->memory_rep ||
        call->argument_count != expected_operands || call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin || call->adapter_count != 0 ||
        call->flags != 0 || call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_HOF ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_HOF ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership !=
            (kind == XR_C_ARRAY_HOF_REDUCE ? XR_TARGET_CALL_NONE : XR_TARGET_CALL_RETURN_OWNED) ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != source_storage || call->array_hof_kind != target_kind ||
        call->array_result_element_storage != result_storage || call->reserved8[0] != 0 ||
        call->reserved8[1] != 0 || call->reserved8[2] != 0 ||
        result_slot->semantic_value != binding->semantic_value ||
        result_slot->function != operation->function ||
        (kind == XR_C_ARRAY_HOF_REDUCE ? register_rep->kind == XR_MACHINE_REP_DYN_VALUE ||
                                             memory_rep->kind == XR_MACHINE_REP_DYN_VALUE ||
                                             result_slot->root_kind != XR_TARGET_ROOT_NONE ||
                                             result_slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL
                                       : register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
                                             memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
                                             result_slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
                                             result_slot->ownership != XR_TARGET_OWNERSHIP_OWNED))
        return false;
    for (uint16_t ordinal = 0; ordinal < expected_operands; ordinal++) {
        uint32_t semantic_operand = operation->operand_begin + ordinal;
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        const XrSemanticTypeRecord *operand_type = xr_semantic_plan_type(semantic, operand->type);
        const XrTargetValueRepRecord *caller =
            xr_target_plan_value_rep(target_plan, operand->value);
        const XrTargetCallArgumentRecord *argument = &arguments[call->argument_begin + ordinal];
        XrStableId expected_argument;
        if (!operand_type || !caller ||
            !emission_identity_from_pair("xray-target-array-hof-argument-v1", operation->id,
                                         operand_type->id, ordinal, &expected_argument) ||
            !xr_stable_id_equal(argument->identity, expected_argument) ||
            argument->call != call->id || argument->semantic_operand != semantic_operand ||
            argument->semantic_value != operand->value ||
            argument->callee_parameter != XR_SEMANTIC_INDEX_NONE ||
            argument->caller_slot != caller->slot ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
            argument->register_rep != caller->register_rep ||
            argument->memory_rep != caller->memory_rep ||
            argument->callee_register_rep != caller->register_rep ||
            argument->callee_memory_rep != caller->memory_rep || argument->ordinal != ordinal ||
            argument->mode != XR_TARGET_CALL_VALUE ||
            argument->ownership !=
                (ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME) ||
            argument->transfer_mode != operand->transfer_mode || argument->flags != 0 ||
            argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
            argument->reserved8[0] != 0 || argument->reserved8[1] != 0 ||
            argument->reserved8[2] != 0)
            return false;
    }

    uint16_t source_machine_kind = XR_MACHINE_REP_COUNT;
    uint16_t result_machine_kind = XR_MACHINE_REP_COUNT;
    XrCValueRep source_rep = XR_C_VALUE_REP_COUNT;
    XrCValueRep result_rep = XR_C_VALUE_REP_COUNT;
    const char *ignored_type = NULL;
    if (!c_array_storage_projection(source_storage, &source_machine_kind, &source_rep,
                                    &ignored_type) ||
        !c_array_storage_projection(result_storage, &result_machine_kind, &result_rep,
                                    &ignored_type))
        return false;
    recipe.callee_function = operation->callable_function;
    recipe.receiver_value = rows[0].value;
    recipe.callback_value = rows[1].value;
    recipe.seed_value = kind == XR_C_ARRAY_HOF_REDUCE ? rows[2].value : UINT32_MAX;
    recipe.kind = kind;
    recipe.source_storage = source_storage;
    recipe.result_storage = result_storage;
    recipe.parameter_reps[0] =
        kind == XR_C_ARRAY_HOF_REDUCE ? (uint8_t) result_rep : (uint8_t) source_rep;
    recipe.parameter_reps[1] =
        kind == XR_C_ARRAY_HOF_REDUCE ? (uint8_t) source_rep : (uint8_t) XR_C_VALUE_REP_VOID;
    recipe.return_rep =
        kind == XR_C_ARRAY_HOF_FILTER ? (uint8_t) XR_C_VALUE_REP_BOOL : (uint8_t) result_rep;
    if (out)
        *out = recipe;
    return true;
}

static bool exact_direct_local_tagged_ref_parameter_prior(
    const XrTargetPlan *target_plan, const XrSemanticParameterRecord *parameter,
    uint8_t *out_storage) {
    const XrSemanticPlan *semantic = target_plan ? xr_target_plan_semantic_plan(target_plan) : NULL;
    uint32_t child_count = 0;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    const XrSemanticTypeRecord *array =
        parameter ? xr_semantic_plan_type(semantic, parameter->type) : NULL;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!semantic || !parameter || !array ||
        parameter->function >= xr_semantic_plan_function_count(semantic) ||
        parameter->mode != XR_PARAM_REF || parameter->ownership != XI_OWN_BORROWED ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 || parameter->reserved != 0)
        return false;
    if (xr_semantic_class_instance_type_source_class(semantic, array) != XR_SEMANTIC_INDEX_NONE) {
        if (out_storage)
            *out_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        return true;
    }
    if (!children ||
        array->kind != XR_KIND_ARRAY || array->builtin_type != XR_TID_NULL ||
        array->child_count != 1 || array->child_begin >= child_count ||
        array->aggregate_extent != 0 || array->aggregate_align != 0 ||
        array->scalar_rep != XR_SCALAR_REP_NONE ||
        array->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        !c_array_storage_from_type(xr_semantic_plan_type(semantic, children[array->child_begin]),
                                   &storage))
        return false;
    if (out_storage)
        *out_storage = storage;
    return true;
}

/* C spelling for a direct-local tagged ref parameter is admitted only after
 * rebuilding the complete callee-place boundary.  The semantic Array and
 * parameter rows determine element storage; Target call rows must then prove
 * that every matching argument is an addressable borrow from a DYN caller
 * slot into this RAW_PTR callee slot. */
static bool exact_direct_local_tagged_ref_parameter_recipe(
    const XrTargetPlan *target_plan, const XrTargetValueRepRecord *binding,
    uint8_t *out_storage) {
    const XrSemanticPlan *semantic = target_plan ? xr_target_plan_semantic_plan(target_plan) : NULL;
    if (!semantic || !binding)
        return false;
    const XrSemanticParameterRecord *parameter = NULL;
    uint32_t parameter_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_parameter_count(semantic); i++) {
        const XrSemanticParameterRecord *candidate = xr_semantic_plan_parameter(semantic, i);
        if (!candidate || candidate->value != binding->semantic_value)
            continue;
        if (parameter)
            return false;
        parameter = candidate;
        parameter_index = i;
    }
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!exact_direct_local_tagged_ref_parameter_prior(target_plan, parameter, &storage))
        return false;
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetSlotRecord *slot = binding->slot < slot_count ? &slots[binding->slot] : NULL;
    if (!register_rep || !memory_rep || !slot || register_rep->kind != XR_MACHINE_REP_RAW_PTR ||
        memory_rep->kind != XR_MACHINE_REP_RAW_PTR ||
        register_rep->root_kind != XR_TARGET_ROOT_NONE ||
        memory_rep->root_kind != XR_TARGET_ROOT_NONE ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        slot->semantic_value != binding->semantic_value ||
        slot->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
        slot->function != parameter->function || slot->role != XR_TARGET_SLOT_PARAMETER ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_NONE || slot->ownership != XR_TARGET_OWNERSHIP_BORROWED)
        return false;

    uint32_t argument_count = 0, call_count = 0, operand_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target_plan, &argument_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t matches = 0;
    for (uint32_t i = 0; arguments && calls && operands && i < argument_count; i++) {
        const XrTargetCallArgumentRecord *argument = &arguments[i];
        if (argument->callee_parameter != parameter_index)
            continue;
        matches++;
        const XrTargetCallRecord *call =
            argument->call < call_count ? &calls[argument->call] : NULL;
        const XrSemanticOperationRecord *call_operation =
            call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
        const XrSemanticOperandRecord *operand = argument->semantic_operand < operand_count
                                                     ? &operands[argument->semantic_operand]
                                                     : NULL;
        const XrTargetValueRepRecord *caller =
            xr_target_plan_value_rep(target_plan, argument->caller_slot < slot_count
                                                      ? slots[argument->caller_slot].semantic_value
                                                      : XR_SEMANTIC_INDEX_NONE);
        const XrTargetMachineRepRecord *caller_register =
            caller ? xr_target_plan_machine_rep(target_plan, caller->register_rep) : NULL;
        const XrTargetMachineRepRecord *caller_memory =
            caller ? xr_target_plan_machine_rep(target_plan, caller->memory_rep) : NULL;
        if (!call || !call_operation || !operand || !caller ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
            call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
            call->callee_function != parameter->function ||
            argument->semantic_operand < call_operation->operand_begin + 1u ||
            argument->semantic_operand >=
                call_operation->operand_begin + call_operation->operand_count ||
            operand->value != argument->semantic_value || operand->type != parameter->type ||
            operand->role != XR_SEM_OPERAND_ARGUMENT ||
            operand->parameter != (int16_t) argument->ordinal ||
            operand->parameter_mode != XR_PARAM_REF || operand->access != XR_CALL_ARG_REF ||
            operand->origin == XI_PLACE_ORIGIN_NONE ||
            operand->lifetime != XI_PLACE_LIFETIME_CALL_BOUND ||
            operand->escape != XI_PLACE_ESCAPE_NONE ||
            operand->ownership_action != XR_SEM_OPERAND_BORROW ||
            operand->transfer_mode != XR_TRANSFER_SHARE ||
            operand->flags != (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE) ||
            argument->callee_slot != binding->slot ||
            argument->callee_register_rep != binding->register_rep ||
            argument->callee_memory_rep != binding->memory_rep ||
            argument->mode != XR_TARGET_CALL_REFERENCE ||
            argument->ownership != XR_TARGET_CALL_BORROW ||
            argument->transfer_mode != XR_TRANSFER_SHARE ||
            argument->flags != XR_TARGET_CALL_ARGUMENT_ADDRESSABLE ||
            argument->array_element_storage != storage || argument->reserved8[0] != 0 ||
            argument->reserved8[1] != 0 || argument->reserved8[2] != 0 ||
            argument->caller_slot != caller->slot ||
            argument->register_rep != caller->register_rep ||
            argument->memory_rep != caller->memory_rep || !caller_register || !caller_memory ||
            caller_register->kind != XR_MACHINE_REP_DYN_VALUE ||
            caller_memory->kind != XR_MACHINE_REP_DYN_VALUE ||
            caller_register->ownership != caller_memory->ownership ||
            (caller_register->ownership != XR_TARGET_OWNERSHIP_OWNED &&
             caller_register->ownership != XR_TARGET_OWNERSHIP_BORROWED))
            return false;
    }
    if (matches == 0)
        return false;
    if (out_storage)
        *out_storage = storage;
    return true;
}

typedef enum XrDirectLocalTaggedRefArgumentMatch {
    XR_C_TAGGED_REF_ARGUMENT_NOT_THIS_FAMILY = 0,
    XR_C_TAGGED_REF_ARGUMENT_EXACT,
    XR_C_TAGGED_REF_ARGUMENT_MALFORMED,
} XrDirectLocalTaggedRefArgumentMatch;

/* Producer reconstruction for the immutable call-argument projection.  The
 * verified Target row supplies storage indexes; Semantic rows independently
 * bind the call result, ordered operand, and ref ownership contract. */
static bool build_direct_local_tagged_ref_argument_view(
    const XrTargetPlan *target_plan, const XrTargetCallArgumentRecord *argument,
    XrCCallArgumentEmissionView *out) {
    const XrSemanticPlan *semantic = target_plan ? xr_target_plan_semantic_plan(target_plan) : NULL;
    uint32_t call_count = 0, slot_count = 0, operand_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    const XrTargetCallRecord *call =
        argument && argument->call < call_count ? &calls[argument->call] : NULL;
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    const XrSemanticCallTargetRecord *semantic_target =
        call ? xr_semantic_plan_call_target(semantic, call->semantic_call_target) : NULL;
    const XrSemanticFunctionRecord *callee =
        call ? xr_semantic_plan_function(semantic, call->callee_function) : NULL;
    const XrSemanticOperandRecord *operand = argument && argument->semantic_operand < operand_count
                                                 ? &operands[argument->semantic_operand]
                                                 : NULL;
    const XrSemanticParameterRecord *parameter =
        argument ? xr_semantic_plan_parameter(semantic, argument->callee_parameter) : NULL;
    const XrTargetSlotRecord *caller_slot =
        argument && argument->caller_slot < slot_count ? &slots[argument->caller_slot] : NULL;
    const XrTargetSlotRecord *callee_slot =
        argument && argument->callee_slot < slot_count ? &slots[argument->callee_slot] : NULL;
    const XrTargetMachineRepRecord *caller_register =
        argument ? xr_target_plan_machine_rep(target_plan, argument->register_rep) : NULL;
    const XrTargetMachineRepRecord *caller_memory =
        argument ? xr_target_plan_machine_rep(target_plan, argument->memory_rep) : NULL;
    const XrTargetMachineRepRecord *callee_register =
        argument ? xr_target_plan_machine_rep(target_plan, argument->callee_register_rep) : NULL;
    const XrTargetMachineRepRecord *callee_memory =
        argument ? xr_target_plan_machine_rep(target_plan, argument->callee_memory_rep) : NULL;
    const XrTargetValueRepRecord *parameter_binding =
        parameter ? xr_target_plan_value_rep(target_plan, parameter->value) : NULL;
    uint8_t parameter_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    XrStableId expected_identity = {{0}};
    if (!semantic || !argument || !out || !call || !operation || !callee || !operand ||
        !semantic_target || !parameter || !parameter_binding || !caller_slot || !callee_slot ||
        !caller_register || !caller_memory || !callee_register || !callee_memory ||
        semantic_target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
        semantic_target->operation != call->semantic_operation ||
        semantic_target->function != call->callee_function ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL) ||
        operation->result_value != call->result_value ||
        operation->function != call->caller_function ||
        argument->semantic_operand != operation->operand_begin + argument->ordinal + 1u ||
        argument->semantic_value != operand->value ||
        argument->callee_parameter != callee->parameter_begin + argument->ordinal ||
        parameter->function != call->callee_function || parameter->ordinal != argument->ordinal ||
        parameter->type != operand->type || parameter->mode != XR_PARAM_REF ||
        parameter->ownership != XI_OWN_BORROWED || parameter->transfer_mode != XR_TRANSFER_SHARE ||
        operand->role != XR_SEM_OPERAND_ARGUMENT ||
        operand->parameter != (int16_t) argument->ordinal ||
        operand->parameter_mode != XR_PARAM_REF || operand->access != XR_CALL_ARG_REF ||
        operand->origin == XI_PLACE_ORIGIN_NONE ||
        operand->lifetime != XI_PLACE_LIFETIME_CALL_BOUND ||
        operand->escape != XI_PLACE_ESCAPE_NONE ||
        operand->ownership_action != XR_SEM_OPERAND_BORROW ||
        operand->transfer_mode != XR_TRANSFER_SHARE ||
        operand->flags != (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE) ||
        argument->mode != XR_TARGET_CALL_REFERENCE ||
        argument->ownership != XR_TARGET_CALL_BORROW ||
        argument->transfer_mode != XR_TRANSFER_SHARE ||
        argument->flags != XR_TARGET_CALL_ARGUMENT_ADDRESSABLE ||
        argument->array_element_storage >= XR_TARGET_ARRAY_STORAGE_COUNT ||
        argument->reserved8[0] != 0 || argument->reserved8[1] != 0 || argument->reserved8[2] != 0 ||
        caller_slot->register_rep != argument->register_rep ||
        caller_slot->memory_rep != argument->memory_rep ||
        caller_slot->ownership != caller_register->ownership ||
        caller_register->ownership != caller_memory->ownership ||
        (caller_register->ownership != XR_TARGET_OWNERSHIP_OWNED &&
         caller_register->ownership != XR_TARGET_OWNERSHIP_BORROWED) ||
        parameter_binding->slot != argument->callee_slot ||
        parameter_binding->register_rep != argument->callee_register_rep ||
        parameter_binding->memory_rep != argument->callee_memory_rep ||
        callee_slot->semantic_value != parameter->value ||
        callee_slot->register_rep != argument->callee_register_rep ||
        callee_slot->memory_rep != argument->callee_memory_rep ||
        callee_slot->role != XR_TARGET_SLOT_PARAMETER ||
        callee_slot->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        caller_register->kind != XR_MACHINE_REP_DYN_VALUE ||
        caller_memory->kind != XR_MACHINE_REP_DYN_VALUE ||
        callee_register->kind != XR_MACHINE_REP_RAW_PTR ||
        callee_memory->kind != XR_MACHINE_REP_RAW_PTR ||
        !exact_direct_local_tagged_ref_parameter_prior(target_plan, parameter,
                                                       &parameter_storage) ||
        parameter_storage != argument->array_element_storage ||
        !emission_identity_from_pair("xray-target-direct-tagged-ref-argument-v2",
                                     semantic_target->id, parameter->id, argument->ordinal,
                                     &expected_identity) ||
        !xr_stable_id_equal(argument->identity, expected_identity))
        return false;
    memset(out, 0, sizeof(*out));
    out->semantic_call_value = operation->result_value;
    out->semantic_operand = argument->semantic_operand;
    out->semantic_value = argument->semantic_value;
    out->callee_parameter = argument->callee_parameter;
    out->ordinal = argument->ordinal;
    out->caller_register_kind = caller_register->kind;
    out->caller_memory_kind = caller_memory->kind;
    out->callee_register_kind = callee_register->kind;
    out->callee_memory_kind = callee_memory->kind;
    out->mode = argument->mode;
    out->ownership = argument->ownership;
    out->transfer_mode = argument->transfer_mode;
    out->flags = argument->flags;
    out->array_element_storage = argument->array_element_storage;
    out->c_type = "XrValue *";
    return true;
}

/* A call-argument row belongs to this consumer only when its call is in the
 * direct-local family and it also carries tagged-ref boundary evidence.
 * Array member and intrinsic calls reuse the storage discriminant, so storage
 * alone must never let this projection claim those independent families.
 * Once a row claims this family, every Semantic and Target prior fact,
 * including its stable identity, must reconstruct exactly or the projection
 * fails closed. */
static XrDirectLocalTaggedRefArgumentMatch
classify_direct_local_tagged_ref_argument(const XrTargetPlan *target_plan,
                                          const XrTargetCallArgumentRecord *argument,
                                          XrCCallArgumentEmissionView *out) {
    if (!target_plan || !argument || !out)
        return XR_C_TAGGED_REF_ARGUMENT_MALFORMED;
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    const XrTargetCallRecord *call =
        argument && calls && argument->call < call_count ? &calls[argument->call] : NULL;
    const XrSemanticCallTargetRecord *semantic_target =
        call && semantic ? xr_semantic_plan_call_target(semantic, call->semantic_call_target)
                         : NULL;
    const XrSemanticParameterRecord *parameter =
        argument && semantic ? xr_semantic_plan_parameter(semantic, argument->callee_parameter)
                             : NULL;
    bool semantic_tagged_prior =
        exact_direct_local_tagged_ref_parameter_prior(target_plan, parameter, NULL);
    XrStableId expected_identity = {{0}};
    bool exact_prior_identity =
        semantic_target && parameter && semantic_target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
        emission_identity_from_pair("xray-target-direct-tagged-ref-argument-v2",
                                    semantic_target->id,
                                    parameter->id, argument->ordinal, &expected_identity) &&
        xr_stable_id_equal(argument->identity, expected_identity);
    bool direct_local_claim =
        call && (call->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
                 call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
                 (semantic_target &&
                  semantic_target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL));
    bool tagged_ref_claim =
        semantic_tagged_prior || exact_prior_identity ||
        argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE;
    bool claimed = argument && direct_local_claim && tagged_ref_claim;
    if (!claimed)
        return XR_C_TAGGED_REF_ARGUMENT_NOT_THIS_FAMILY;
    return build_direct_local_tagged_ref_argument_view(target_plan, argument, out)
               ? XR_C_TAGGED_REF_ARGUMENT_EXACT
               : XR_C_TAGGED_REF_ARGUMENT_MALFORMED;
}

static int compare_c_call_argument_view(const void *left, const void *right) {
    const XrCCallArgumentEmissionView *a = (const XrCCallArgumentEmissionView *) left;
    const XrCCallArgumentEmissionView *b = (const XrCCallArgumentEmissionView *) right;
    if (a->semantic_call_value != b->semantic_call_value)
        return a->semantic_call_value < b->semantic_call_value ? -1 : 1;
    if (a->ordinal != b->ordinal)
        return a->ordinal < b->ordinal ? -1 : 1;
    return 0;
}

static bool adt_enum_payload_has_exact_projection(const XrTargetPlan *target_plan,
                                                  uint32_t semantic_value) {
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(target_plan, binding->memory_rep) : NULL;
    XrCValueRep register_c_rep = XR_C_VALUE_REP_COUNT;
    XrCValueRep memory_c_rep = XR_C_VALUE_REP_COUNT;
    const char *register_c_type = NULL;
    const char *memory_c_type = NULL;
    return binding && register_rep && memory_rep &&
           machine_kind_to_c_rep(target_plan, semantic_value, register_rep->kind, &register_c_rep,
                                 &register_c_type) &&
           machine_kind_to_c_rep(target_plan, semantic_value, memory_rep->kind, &memory_c_rep,
                                 &memory_c_type) &&
           register_rep->kind == memory_rep->kind && register_c_rep == memory_c_rep &&
           strcmp(register_c_type, memory_c_type) == 0;
}

/* Builder-side collector. The recipe is admitted only by the frozen
 * intrinsic evidence and its exact Target call row; no Xi name survives at
 * this boundary. */
static bool build_exact_string_byte_slice_view_recipe(const XrTargetPlan *target_plan,
                                                      const XrTargetValueRepRecord *binding,
                                                      uint32_t *source_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    if (!semantic || !operation || !binding || !operands || !slots || binding->slot >= slot_count ||
        operation->opcode != XI_CALL_BUILTIN ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        operation->evidence[1] != XA_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        operation->result_value != binding->semantic_value || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->view_source_operand != 0 ||
        operation->view_source_parameter != -1 ||
        operation->view_source_value == XR_SEMANTIC_INDEX_NONE ||
        operation->view_origin != XI_VIEW_ORIGIN_RECEIVER || operation->view_capability != 1 ||
        operation->view_lifetime != 1 || operation->view_complete != 1 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0 ||
        operands[operation->operand_begin].value != operation->view_source_value ||
        operands[operation->operand_begin].parameter != 0 ||
        operands[operation->operand_begin].role != XR_SEM_OPERAND_ARGUMENT ||
        (operands[operation->operand_begin].flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0)
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(semantic, source->type);
    const XrSemanticTypeRecord *element_type =
        xr_semantic_plan_type(semantic, operation->view_element_type);
    const XrSemanticTypeRecord *view_type = xr_semantic_plan_type(semantic, operation->result_type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    const uint8_t string_required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    const uint8_t string_allowed = string_required | XR_SEM_TYPE_CONST | XR_SEM_TYPE_NULLABLE;
    if (!source_type || source_type->kind != XR_KIND_STRING ||
        source_type->builtin_type != XR_TID_NULL || source_type->child_count != 0 ||
        source_type->aggregate_extent != 0 || source_type->aggregate_align != 0 ||
        source_type->scalar_rep != XR_SCALAR_REP_NONE ||
        (source_type->flags & string_required) != string_required ||
        (source_type->flags & ~string_allowed) != 0 || !element_type ||
        element_type->kind != XR_KIND_INT || element_type->builtin_type != XR_TID_NULL ||
        element_type->child_count != 0 || element_type->aggregate_extent != 0 ||
        element_type->aggregate_align != 0 || element_type->scalar_rep != XR_NATIVE_U8 ||
        element_type->flags != 0 || !view_type || view_type->kind != XR_KIND_SLICE ||
        view_type->builtin_type != XR_TID_NULL || view_type->child_count != 1 ||
        view_type->child_begin >= child_count ||
        children[view_type->child_begin] != operation->view_element_type ||
        view_type->aggregate_extent != 0 || view_type->aggregate_align != 0 ||
        view_type->scalar_rep != XR_SCALAR_REP_NONE ||
        view_type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW))
        return false;
    uint32_t semantic_operation = slots[binding->slot].semantic_operation;
    XrStableId expected_identity;
    if (!emission_identity_from_pair("xray-target-string-byte-slice-view-v1", operation->id,
                                     view_type->id, 0, &expected_identity))
        return false;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->semantic_operation != semantic_operation)
            continue;
        if (match || !xr_stable_id_equal(call->identity, expected_identity) ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_value != binding->semantic_value || call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != 0 ||
            call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_BORROW ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_STRING_BYTE_SLICE_VIEW ||
            call->target_kind != XR_TARGET_CALL_TARGET_STRING_BYTE_SLICE_VIEW)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (source_value)
        *source_value = operation->view_source_value;
    return true;
}

/* Verifier-side proof is deliberately separate from the collector above. */
static bool verify_exact_string_byte_slice_view_recipe(const XrTargetPlan *target_plan,
                                                       const XrTargetValueRepRecord *binding,
                                                       uint32_t *source_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *op = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operand_rows =
        xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!semantic || !binding || !op || !operand_rows || op->opcode != XI_CALL_BUILTIN ||
        op->intrinsic_kind != XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        op->evidence[1] != XA_INTRINSIC_STRING_BYTE_SLICE_VIEW || op->operand_count != 1 ||
        op->operand_begin >= operand_count || op->result_value != binding->semantic_value ||
        op->view_source_operand != 0 || op->view_source_parameter != -1 ||
        op->view_origin != XI_VIEW_ORIGIN_RECEIVER || op->view_capability != 1 ||
        op->view_lifetime != 1 || op->view_complete != 1 || op->reserved_view[0] != 0 ||
        op->reserved_view[1] != 0)
        return false;
    const XrSemanticOperandRecord *source = &operand_rows[op->operand_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(semantic, source->type);
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(semantic, op->view_element_type);
    const XrSemanticTypeRecord *view = xr_semantic_plan_type(semantic, op->result_type);
    const uint8_t source_required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    const uint8_t source_allowed = source_required | XR_SEM_TYPE_CONST | XR_SEM_TYPE_NULLABLE;
    if (source->value != op->view_source_value || source->parameter != 0 ||
        source->role != XR_SEM_OPERAND_ARGUMENT ||
        (source->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 || !source_type ||
        source_type->kind != XR_KIND_STRING || source_type->builtin_type != XR_TID_NULL ||
        source_type->child_count != 0 || source_type->aggregate_extent != 0 ||
        source_type->aggregate_align != 0 || source_type->scalar_rep != XR_SCALAR_REP_NONE ||
        (source_type->flags & source_required) != source_required ||
        (source_type->flags & ~source_allowed) != 0 || !element || element->kind != XR_KIND_INT ||
        element->builtin_type != XR_TID_NULL || element->child_count != 0 ||
        element->aggregate_extent != 0 || element->aggregate_align != 0 ||
        element->scalar_rep != XR_NATIVE_U8 || element->flags != 0 || !view ||
        view->kind != XR_KIND_SLICE || view->builtin_type != XR_TID_NULL ||
        view->child_count != 1 || view->child_begin >= child_count ||
        children[view->child_begin] != op->view_element_type || view->aggregate_extent != 0 ||
        view->aggregate_align != 0 || view->scalar_rep != XR_SCALAR_REP_NONE ||
        view->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW))
        return false;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    if (!slots || binding->slot >= slot_count)
        return false;
    XrStableId expected;
    if (!emission_identity_from_pair("xray-target-string-byte-slice-view-v1", op->id, view->id, 0,
                                     &expected))
        return false;
    uint32_t semantic_operation = slots[binding->slot].semantic_operation;
    uint32_t matches = 0;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->semantic_operation != semantic_operation)
            continue;
        matches++;
        if (!xr_stable_id_equal(call->identity, expected) ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != op->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_value != binding->semantic_value || call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != 0 ||
            call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_BORROW ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_STRING_BYTE_SLICE_VIEW ||
            call->target_kind != XR_TARGET_CALL_TARGET_STRING_BYTE_SLICE_VIEW)
            return false;
    }
    if (matches != 1)
        return false;
    if (source_value)
        *source_value = source->value;
    return true;
}

static const XrTargetCallRecord *
stringbuilder_constructor_call(const XrTargetPlan *target_plan,
                               const XrTargetValueRepRecord *binding) {
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    if (!operation || !binding || !slots || binding->slot >= slot_count)
        return NULL;
    uint32_t semantic_operation = slots[binding->slot].semantic_operation;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->semantic_operation != semantic_operation)
            continue;
        if (match || call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_value != binding->semantic_value || call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != 0 ||
            call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR ||
            call->target_kind != XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR)
            return NULL;
        match = call;
    }
    return match;
}

static bool exact_stringbuilder_new_recipe(const XrTargetPlan *target_plan,
                                           const XrTargetValueRepRecord *binding) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    char expected_type_key[160];
    int written = snprintf(expected_type_key, sizeof(expected_type_key),
                           "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:13:StringBuilder[0]",
                           (unsigned) XR_KIND_INSTANCE, (unsigned) XR_TID_STRINGBUILDER,
                           (unsigned) XR_SCALAR_REP_NONE);
    return operation && type && written > 0 && (size_t) written < sizeof(expected_type_key) &&
           stringbuilder_constructor_call(target_plan, binding) &&
           operation->opcode == XI_CALL_BUILTIN &&
           operation->result_value == binding->semantic_value && operation->operand_count == 0 &&
           operation->metadata_count == 1 && operation->metadata_begin < metadata_count &&
           metadata && strcmp(metadata[operation->metadata_begin], "StringBuilder") == 0 &&
           operation->auxiliary_kind == XI_AUX_KIND_NONE && operation->semantic_immediate == 0 &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           operation->effects == xi_generated_op_effects(XI_CALL_BUILTIN) &&
           operation->flags == xi_generated_op_default_flags(XI_CALL_BUILTIN) &&
           operation->ownership_use == xi_generated_op_own_use(XI_CALL_BUILTIN) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           operation->transfer_mode == XR_TRANSFER_SHARE &&
           operation->parameter_mode == XR_PARAM_READ &&
           operation->parameter_ownership == XI_OWN_NONE && operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_OWNED &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           xr_semantic_allocation_identity_is_canonical(operation) &&
           type->kind == XR_KIND_INSTANCE && type->builtin_type == XR_TID_STRINGBUILDER &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key && strcmp(type->canonical_key, expected_type_key) == 0;
}

static bool exact_stringbuilder_append_rune_recipe(const XrTargetPlan *target_plan,
                                                   const XrTargetValueRepRecord *binding,
                                                   uint32_t *receiver_value,
                                                   uint32_t *argument_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    if (!semantic || !operation || !binding || !operands ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_RUNE ||
        operation->opcode != XI_CALL_METHOD || operation->operand_count != 2 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->result_value != binding->semantic_value || operation->result_alias_operand != 0)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_RUNE)
            continue;
        if (call->result_value != binding->semantic_value || match ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != 0 ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_RUNE ||
            call->target_kind != XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_RUNE ||
            call->result_ownership != XR_TARGET_CALL_RETURN_OWNED)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    if (argument_value)
        *argument_value = argument->value;
    return true;
}

static bool exact_stringbuilder_to_string_recipe(const XrTargetPlan *target_plan,
                                                 const XrTargetValueRepRecord *binding,
                                                 uint32_t *receiver_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operands_count = 0, calls_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operands_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &calls_count);
    if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_TO_STRING ||
        operation->operand_count != 1 || operation->operand_begin >= operands_count ||
        operation->result_value != binding->semantic_value || operation->result_alias_operand != -1)
        return false;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < calls_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_STRINGBUILDER_TO_STRING)
            continue;
        if (match || call->target_kind != XR_TARGET_CALL_TARGET_STRINGBUILDER_TO_STRING ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != 0 ||
            call->result_ownership != XR_TARGET_CALL_RETURN_OWNED)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (receiver_value)
        *receiver_value = operands[operation->operand_begin].value;
    return true;
}

static bool exact_string_runes_recipe(const XrTargetPlan *target_plan,
                                      const XrTargetValueRepRecord *binding,
                                      uint32_t *receiver_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t receiver = UINT32_MAX;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    if (!semantic || !operation || !binding || operation->result_value != binding->semantic_value ||
        !xr_semantic_string_runes_is_exact(semantic, operation, &receiver))
        return false;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_STRING_RUNES)
            continue;
        if (match || call->semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
            xr_semantic_plan_operation(semantic, call->semantic_operation) != operation ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != 0 ||
            call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
            call->target_kind != XR_TARGET_CALL_TARGET_STRING_RUNES)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    return true;
}

static bool exact_iterator_rune_has_next_recipe(const XrTargetPlan *target_plan,
                                                const XrTargetValueRepRecord *binding,
                                                uint32_t *receiver_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t receiver = UINT32_MAX;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    if (!semantic || !operation || !binding || operation->result_value != binding->semantic_value ||
        !xr_semantic_iterator_rune_has_next_is_exact(semantic, operation, &receiver))
        return false;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_HAS_NEXT)
            continue;
        if (match || call->semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
            xr_semantic_plan_operation(semantic, call->semantic_operation) != operation ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != 0 ||
            call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_NONE ||
            call->target_kind != XR_TARGET_CALL_TARGET_ITERATOR_RUNE_HAS_NEXT)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    return true;
}

static bool exact_iterator_rune_next_recipe(const XrTargetPlan *target_plan,
                                            const XrTargetValueRepRecord *binding,
                                            uint32_t *receiver_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t receiver = UINT32_MAX;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    if (!semantic || !operation || !binding || operation->result_value != binding->semantic_value ||
        !xr_semantic_iterator_rune_next_is_exact(semantic, operation, &receiver))
        return false;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NEXT)
            continue;
        if (match || call->semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
            xr_semantic_plan_operation(semantic, call->semantic_operation) != operation ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != 0 ||
            call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_NONE ||
            call->target_kind != XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NEXT)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    return true;
}

static bool exact_iterator_rune_nth_recipe(const XrTargetPlan *target_plan,
                                           const XrTargetValueRepRecord *binding,
                                           uint32_t *receiver_value, uint32_t *index_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t receiver = UINT32_MAX;
    uint32_t index = UINT32_MAX;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    if (!semantic || !operation || !binding || operation->result_value != binding->semantic_value ||
        !xr_semantic_iterator_rune_nth_is_exact(semantic, operation, &receiver, &index) ||
        operation->operand_begin + 1u >= operand_count)
        return false;
    uint32_t semantic_operand = operation->operand_begin + 1u;
    const XrSemanticOperandRecord *operand = &operands[semantic_operand];
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(semantic, operation->result_type);
    const XrSemanticTypeRecord *index_type = xr_semantic_plan_type(semantic, operand->type);
    const XrTargetValueRepRecord *caller = xr_target_plan_value_rep(target_plan, index);
    uint32_t call_count = 0;
    uint32_t argument_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target_plan, &argument_count);
    const XrTargetCallRecord *match = NULL;
    const XrTargetCallArgumentRecord *argument = NULL;
    XrStableId expected_call;
    XrStableId expected_argument;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NTH)
            continue;
        if (match || call->semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
            xr_semantic_plan_operation(semantic, call->semantic_operation) != operation ||
            call->id != i || call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep || call->argument_count != 1 ||
            call->argument_begin >= argument_count || call->adapter_count != 0 ||
            call->flags != 0 || call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_NONE ||
            call->target_kind != XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NTH)
            return false;
        match = call;
        argument = arguments ? &arguments[call->argument_begin] : NULL;
    }
    const XrTargetMachineRepRecord *caller_register =
        caller ? xr_target_plan_machine_rep(target_plan, caller->register_rep) : NULL;
    const XrTargetMachineRepRecord *caller_memory =
        caller ? xr_target_plan_machine_rep(target_plan, caller->memory_rep) : NULL;
    if (!match || !argument || !result_type || !index_type || !caller || !caller_register ||
        !caller_memory || operand->value != index ||
        !emission_identity_from_pair("xray-target-iterator-rune-nth-v1", operation->id,
                                     result_type->id, receiver, &expected_call) ||
        !xr_stable_id_equal(match->identity, expected_call) ||
        !emission_identity_from_pair("xray-target-iterator-rune-nth-argument-v1", operation->id,
                                     index_type->id, 0, &expected_argument) ||
        !xr_stable_id_equal(argument->identity, expected_argument) || argument->call != match->id ||
        argument->semantic_operand != semantic_operand || argument->semantic_value != index ||
        argument->callee_parameter != XR_SEMANTIC_INDEX_NONE ||
        argument->caller_slot != caller->slot || argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
        argument->register_rep != caller->register_rep ||
        argument->memory_rep != caller->memory_rep ||
        argument->callee_register_rep != caller->register_rep ||
        argument->callee_memory_rep != caller->memory_rep || argument->ordinal != 0 ||
        argument->mode != XR_TARGET_CALL_VALUE || argument->ownership != XR_TARGET_CALL_CONSUME ||
        argument->transfer_mode != XR_TRANSFER_SHARE || argument->flags != 0 ||
        argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        argument->reserved8[0] != 0 || argument->reserved8[1] != 0 || argument->reserved8[2] != 0 ||
        caller_register->kind != XR_MACHINE_REP_I64 || caller_memory->kind != XR_MACHINE_REP_I64)
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    if (index_value)
        *index_value = index;
    return true;
}

static bool exact_rune_to_uint32_recipe(const XrTargetPlan *target_plan,
                                        const XrTargetValueRepRecord *binding,
                                        uint32_t *receiver_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t receiver = UINT32_MAX;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    if (!semantic || !operation || !binding || operation->result_value != binding->semantic_value ||
        !xr_semantic_rune_to_uint32_is_exact(semantic, operation, &receiver))
        return false;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_RUNE_TO_UINT32)
            continue;
        if (match || call->semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
            xr_semantic_plan_operation(semantic, call->semantic_operation) != operation ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != 0 ||
            call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_NONE ||
            call->target_kind != XR_TARGET_CALL_TARGET_RUNE_TO_UINT32)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    return true;
}

static bool exact_rune_to_string_recipe(const XrTargetPlan *target_plan,
                                        const XrTargetValueRepRecord *binding,
                                        uint32_t *receiver_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t receiver = UINT32_MAX;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    if (!semantic || !operation || !binding || operation->result_value != binding->semantic_value ||
        !xr_semantic_rune_to_string_is_exact(semantic, operation, &receiver))
        return false;
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(semantic, operation->result_type);
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding->slot < slot_count ? &slots[binding->slot] : NULL;
    const XrTargetCallRecord *match = NULL;
    uint32_t match_index = UINT32_MAX;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_RUNE_TO_STRING)
            continue;
        if (match)
            return false;
        match = call;
        match_index = i;
    }
    XrStableId expected_call;
    if (!result_type || !register_rep || !memory_rep || !slot || !match ||
        !emission_identity_from_pair("xray-target-rune-to-string-v1", operation->id,
                                     result_type->id, receiver, &expected_call) ||
        !xr_stable_id_equal(match->identity, expected_call) || match->id != match_index ||
        match->semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
        xr_semantic_plan_operation(semantic, match->semantic_operation) != operation ||
        match->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        match->caller_function != operation->function ||
        match->callee_function != XR_SEMANTIC_INDEX_NONE ||
        match->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        match->source_export != XR_SEMANTIC_INDEX_NONE ||
        !emission_stable_id_is_zero(match->source_export_identity) ||
        !emission_stable_id_is_zero(match->source_callee_identity) ||
        match->result_slot != binding->slot ||
        match->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        match->error_slot != XR_SEMANTIC_INDEX_NONE ||
        match->result_register_rep != binding->register_rep ||
        match->result_memory_rep != binding->memory_rep || match->argument_count != 0 ||
        match->adapter_count != 0 || match->flags != 0 ||
        match->result_mode != XR_TARGET_CALL_VALUE ||
        match->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
        match->target_kind != XR_TARGET_CALL_TARGET_RUNE_TO_STRING ||
        match->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL ||
        match->reserved8[0] != 0 || match->reserved8[1] != 0 || match->reserved8[2] != 0 ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        register_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        memory_rep->null_encoding != XR_TARGET_NULL_TAGGED ||
        slot->function != operation->function || slot->semantic_value != binding->semantic_value ||
        slot->semantic_operation != match->semantic_operation ||
        slot->logical_slot != XR_SEMANTIC_INDEX_NONE || slot->role != XR_TARGET_SLOT_TEMPORARY ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED || slot->reserved != 0 ||
        slot->debug_variable != XR_SEMANTIC_INDEX_NONE)
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    return true;
}

static bool exact_rune_is_whitespace_recipe(const XrTargetPlan *target_plan,
                                            const XrTargetValueRepRecord *binding,
                                            uint32_t *receiver_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t receiver = UINT32_MAX;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    if (!semantic || !operation || !binding || operation->result_value != binding->semantic_value ||
        !xr_semantic_rune_is_whitespace_is_exact(semantic, operation, &receiver))
        return false;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_RUNE_IS_WHITESPACE)
            continue;
        if (match || call->semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
            xr_semantic_plan_operation(semantic, call->semantic_operation) != operation ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != 0 ||
            call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_NONE ||
            call->target_kind != XR_TARGET_CALL_TARGET_RUNE_IS_WHITESPACE)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    return true;
}

static bool exact_string_slice_range_recipe(const XrTargetPlan *target_plan,
                                            const XrTargetValueRepRecord *binding,
                                            uint32_t *receiver_value, uint32_t *start_value,
                                            uint32_t *end_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t receiver = UINT32_MAX;
    uint32_t start = UINT32_MAX;
    uint32_t end = UINT32_MAX;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    if (!semantic || !operation || !binding || operation->result_value != binding->semantic_value ||
        !xr_semantic_string_slice_range_is_exact(semantic, operation, &receiver, &start, &end))
        return false;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_STRING_SLICE_RANGE)
            continue;
        if (match || call->semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
            xr_semantic_plan_operation(semantic, call->semantic_operation) != operation ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep || call->argument_count != 0 ||
            call->adapter_count != 0 || call->flags != XR_TARGET_CALL_TAIL ||
            call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
            call->target_kind != XR_TARGET_CALL_TARGET_STRING_SLICE_RANGE)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    if (start_value)
        *start_value = start;
    if (end_value)
        *end_value = end;
    return true;
}

static bool string_slice_bound_has_exact_projection(const XrTargetPlan *target_plan,
                                                    uint32_t semantic_value) {
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(target_plan, semantic_value);
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(target_plan, binding->memory_rep) : NULL;
    return binding && register_rep && memory_rep && register_rep->kind == XR_MACHINE_REP_I64 &&
           memory_rep->kind == XR_MACHINE_REP_I64;
}

static bool exact_stringbuilder_append_string_recipe(const XrTargetPlan *target_plan,
                                                     const XrTargetValueRepRecord *binding,
                                                     uint32_t *receiver_value,
                                                     uint32_t *argument_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t oc = 0, cc = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &oc);
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &cc);
    if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_STRING ||
        operation->operand_count != 2 || operation->operand_begin + 1u >= oc ||
        operation->result_value != binding->semantic_value)
        return false;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < cc; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->result_value != binding->semantic_value ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_STRING)
            continue;
        if (match || call->target_kind != XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_STRING ||
            call->argument_count != 0 || call->flags != 0 ||
            call->result_ownership != XR_TARGET_CALL_RETURN_OWNED)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (receiver_value)
        *receiver_value = operands[operation->operand_begin].value;
    if (argument_value)
        *argument_value = operands[operation->operand_begin + 1u].value;
    return true;
}

/* Builder-side recipe collector. It reconstructs the literal authority from
 * frozen SemanticPlan rows; it never reads Xi values or source types. */
static const char *build_exact_string_literal(const XrTargetPlan *target_plan,
                                              const XrTargetValueRepRecord *binding) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    if (!semantic || !operation || operation->opcode != XI_CONST || operation->operand_count != 0 ||
        operation->result_value != binding->semantic_value ||
        operation->constant >= xr_semantic_plan_constant_count(semantic) ||
        operation->allocation_key || !emission_stable_id_is_zero(operation->allocation_id) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1)
        return NULL;
    const XrSemanticConstantRecord *constant =
        xr_semantic_plan_constant(semantic, operation->constant);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, operation->result_type);
    uint8_t forbidden = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT;
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    return constant && constant->kind == XR_SEM_CONST_STRING &&
                   constant->type == operation->result_type && constant->string && type &&
                   type->kind == XR_KIND_STRING && type->child_count == 0 &&
                   type->scalar_rep == XR_SCALAR_REP_NONE && type->aggregate_extent == 0 &&
                   type->aggregate_align == 0 && (type->flags & forbidden) == 0 &&
                   (type->flags & required) == required
               ? constant->string
               : NULL;
}

/* Verifier-side predicate is intentionally independent of the collector
 * above. It proves the emitted recipe from the immutable rows again. */
static const char *verify_expected_string_literal(const XrTargetPlan *target_plan,
                                                  const XrTargetValueRepRecord *binding) {
    const XrSemanticPlan *plan = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *op = binding_operation(target_plan, binding);
    if (!plan || !op || op->opcode != XI_CONST || op->operand_count != 0 ||
        op->result_value != binding->semantic_value || op->allocation_key != NULL ||
        !emission_stable_id_is_zero(op->allocation_id) || op->return_complete != 1 ||
        op->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        op->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        op->constant >= xr_semantic_plan_constant_count(plan))
        return NULL;
    const XrSemanticConstantRecord *literal = xr_semantic_plan_constant(plan, op->constant);
    const XrSemanticTypeRecord *string_type = xr_semantic_plan_type(plan, op->result_type);
    if (!literal || literal->kind != XR_SEM_CONST_STRING || literal->type != op->result_type ||
        !literal->string || !string_type || string_type->kind != XR_KIND_STRING ||
        string_type->child_count != 0 || string_type->scalar_rep != XR_SCALAR_REP_NONE ||
        string_type->aggregate_extent != 0 || string_type->aggregate_align != 0)
        return NULL;
    const uint8_t disallowed = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                               XR_SEM_TYPE_AGGREGATE_EXACT;
    const uint8_t mandatory = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    return (string_type->flags & disallowed) == 0 && (string_type->flags & mandatory) == mandatory
               ? literal->string
               : NULL;
}

static bool emission_channel_type_is_exact(const XrSemanticPlan *semantic, uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    return type && type->kind == XR_KIND_CHANNEL && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->child_count == 1 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           (type->flags & required) == required &&
           (type->flags & ~(required | XR_SEM_TYPE_CONST)) == 0 &&
           type->child_begin < child_count &&
           children[type->child_begin] < xr_semantic_plan_type_count(semantic);
}

static bool exact_channel_new_recipe(const XrTargetPlan *target_plan,
                                     const XrTargetValueRepRecord *binding,
                                     uint32_t *capacity_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    static const char suffix[] = "/allocation";
    size_t canonical_length =
        operation && operation->canonical_key ? strlen(operation->canonical_key) : 0;
    size_t allocation_length =
        operation && operation->allocation_key ? strlen(operation->allocation_key) : 0;
    XrStableId expected_allocation;
    XrFingerprint allocation_digest;
    if (!semantic || !operation || operation->opcode != XI_CHAN_NEW ||
        operation->result_value != binding->semantic_value || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        !xr_semantic_allocation_identity_is_canonical(operation) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_CHAN_NEW) ||
        operation->effects != xi_generated_op_effects(XI_CHAN_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_NEW) ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_complete != 1 ||
        !emission_channel_type_is_exact(semantic, operation->result_type))
        return false;
    const XrSemanticOperandRecord *capacity = &operands[operation->operand_begin];
    const XrTargetValueRepRecord *capacity_binding =
        xr_target_plan_value_rep(target_plan, capacity->value);
    const XrTargetMachineRepRecord *capacity_rep =
        capacity_binding ? xr_target_plan_machine_rep(target_plan, capacity_binding->register_rep)
                         : NULL;
    if (!capacity_binding || !capacity_rep || capacity->parameter != -1 ||
        capacity->role != XR_SEM_OPERAND_VALUE || capacity->flags != 0 ||
        capacity_rep->kind < XR_MACHINE_REP_I8 || capacity_rep->kind > XR_MACHINE_REP_USIZE)
        return false;
    if (capacity_value)
        *capacity_value = capacity->value;
    return true;
}

static const char *channel_receive_symbol(uint16_t machine_kind) {
    switch ((XrMachineRepKind) machine_kind) {
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
            return XR_C_CHANNEL_RECV_FLOAT_SYMBOL;
        case XR_MACHINE_REP_I1:
            return XR_C_CHANNEL_RECV_BOOL_SYMBOL;
        case XR_MACHINE_REP_RUNE:
            return XR_C_CHANNEL_RECV_RUNE_SYMBOL;
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
            return XR_C_CHANNEL_RECV_INT_SYMBOL;
        default:
            return NULL;
    }
}

static const char *exact_channel_receive_recipe(const XrTargetPlan *target_plan,
                                                const XrTargetValueRepRecord *binding,
                                                uint32_t *receiver_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    if (!semantic || !operation || operation->opcode != XI_CHAN_TRY_RECV ||
        operation->result_value != binding->semantic_value || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->allocation_key ||
        !emission_stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_TRY_RECV) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_TRY_RECV) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CHAN_TRY_RECV) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_CHAN_TRY_RECV) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return NULL;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *channel = xr_semantic_plan_type(semantic, receiver->type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    const XrTargetValueRepRecord *receiver_binding =
        xr_target_plan_value_rep(target_plan, receiver->value);
    const XrTargetMachineRepRecord *receiver_rep =
        receiver_binding ? xr_target_plan_machine_rep(target_plan, receiver_binding->register_rep)
                         : NULL;
    const XrTargetMachineRepRecord *result_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    if (!channel || channel->kind != XR_KIND_CHANNEL || channel->scalar_rep != XR_SCALAR_REP_NONE ||
        channel->child_count != 1 || channel->aggregate_extent != 0 ||
        channel->aggregate_align != 0 || (channel->flags & required) != required ||
        (channel->flags & ~(required | XR_SEM_TYPE_CONST)) != 0 ||
        channel->child_begin >= child_count ||
        children[channel->child_begin] != operation->result_type ||
        receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != XR_PARAM_READ || receiver->access != XR_CALL_ARG_PLAIN ||
        receiver->origin != XI_PLACE_ORIGIN_NONE || receiver->lifetime != XI_PLACE_LIFETIME_NONE ||
        receiver->escape != XI_PLACE_ESCAPE_NONE || receiver->flags != 0 || !receiver_binding ||
        !receiver_rep || receiver_rep->kind != XR_MACHINE_REP_DYN_VALUE || !result_rep)
        return NULL;
    const char *symbol = channel_receive_symbol(result_rep->kind);
    if (symbol && receiver_value)
        *receiver_value = receiver->value;
    return symbol;
}

static const char *verify_channel_receive_recipe(const XrTargetPlan *target_plan,
                                                 const XrTargetValueRepRecord *binding,
                                                 uint32_t *receiver_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    if (!semantic || !operation || operation->opcode != XI_CHAN_TRY_RECV ||
        operation->result_value != binding->semantic_value || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->allocation_key ||
        !emission_stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_TRY_RECV) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_TRY_RECV) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CHAN_TRY_RECV) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_CHAN_TRY_RECV) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return NULL;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *channel = xr_semantic_plan_type(semantic, receiver->type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    const XrTargetValueRepRecord *receiver_binding =
        xr_target_plan_value_rep(target_plan, receiver->value);
    const XrTargetMachineRepRecord *receiver_rep =
        receiver_binding ? xr_target_plan_machine_rep(target_plan, receiver_binding->register_rep)
                         : NULL;
    const XrTargetMachineRepRecord *result_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    if (!channel || channel->kind != XR_KIND_CHANNEL || channel->scalar_rep != XR_SCALAR_REP_NONE ||
        channel->child_count != 1 || channel->aggregate_extent != 0 ||
        channel->aggregate_align != 0 || (channel->flags & required) != required ||
        (channel->flags & ~(required | XR_SEM_TYPE_CONST)) != 0 ||
        channel->child_begin >= child_count ||
        children[channel->child_begin] != operation->result_type ||
        receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != XR_PARAM_READ || receiver->access != XR_CALL_ARG_PLAIN ||
        receiver->origin != XI_PLACE_ORIGIN_NONE || receiver->lifetime != XI_PLACE_LIFETIME_NONE ||
        receiver->escape != XI_PLACE_ESCAPE_NONE || receiver->flags != 0 || !receiver_binding ||
        !receiver_rep || receiver_rep->kind != XR_MACHINE_REP_DYN_VALUE || !result_rep)
        return NULL;
    const char *symbol = NULL;
    switch ((XrMachineRepKind) result_rep->kind) {
        case XR_MACHINE_REP_I1:
            symbol = XR_C_CHANNEL_RECV_BOOL_SYMBOL;
            break;
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
            symbol = XR_C_CHANNEL_RECV_FLOAT_SYMBOL;
            break;
        case XR_MACHINE_REP_RUNE:
            symbol = XR_C_CHANNEL_RECV_RUNE_SYMBOL;
            break;
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
            symbol = XR_C_CHANNEL_RECV_INT_SYMBOL;
            break;
        default:
            return NULL;
    }
    if (receiver_value)
        *receiver_value = receiver->value;
    return symbol;
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t encoded[8];
    for (uint32_t i = 0; i < sizeof(encoded); i++)
        encoded[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(ctx, encoded, sizeof(encoded));
}

/* Whether any call in this plan names a result representation for `callee`. */
static bool target_plan_has_call_result_rep(const XrTargetPlan *target_plan, uint32_t callee) {
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    for (uint32_t i = 0; calls && i < call_count; i++)
        if (calls[i].callee_function == callee)
            return true;
    return false;
}

static void compute_fingerprint(const XrCEmissionPlan *plan, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-c-emission-plan-v26\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    hash_u64(&ctx, plan->schema_version);
    xr_sha256_update(&ctx, plan->target_fingerprint.bytes, sizeof(plan->target_fingerprint.bytes));
    xr_sha256_update(&ctx, plan->profile_fingerprint.bytes,
                     sizeof(plan->profile_fingerprint.bytes));
    hash_u64(&ctx, plan->value_count);
    hash_u64(&ctx, plan->call_argument_count);
    hash_u64(&ctx, plan->recipe_argument_count);
    hash_u64(&ctx, plan->cleanup_count);
    hash_u64(&ctx, plan->function_abi_count);
    for (uint32_t i = 0; i < plan->function_abi_count; i++) {
        const XrCFunctionAbiEmissionView *abi = &plan->function_abis[i];
        hash_u64(&ctx, abi->semantic_function);
        hash_u64(&ctx, abi->semantic_value);
        hash_u64(&ctx, abi->ordinal);
        hash_u64(&ctx, abi->parameter_count);
        hash_u64(&ctx, abi->target_register_kind);
        hash_u64(&ctx, abi->target_memory_kind);
        hash_u64(&ctx, abi->slot_class);
        hash_u64(&ctx, abi->rep);
        hash_u64(&ctx, abi->pointee_rep);
        hash_u64(&ctx, abi->aggregate_class);
    }
    for (uint32_t i = 0; i < plan->value_count; i++) {
        const XrCValueEmissionView *value = &plan->values[i];
        hash_u64(&ctx, value->semantic_value);
        hash_u64(&ctx, value->target_register_rep);
        hash_u64(&ctx, value->target_memory_rep);
        hash_u64(&ctx, value->target_register_kind);
        hash_u64(&ctx, value->target_memory_kind);
        hash_u64(&ctx, value->register_bits);
        hash_u64(&ctx, value->memory_align);
        hash_u64(&ctx, value->memory_size);
        hash_u64(&ctx, value->rep);
        hash_u64(&ctx, value->materialization);
        hash_u64(&ctx, value->reserved);
        hash_u64(&ctx, value->literal_byte_length);
        hash_u64(&ctx, value->recipe_operand_value);
        hash_u64(&ctx, value->recipe_argument_value);
        hash_u64(&ctx, value->recipe_layout_id);
        hash_u64(&ctx, value->recipe_discriminant);
        hash_u64(&ctx, value->recipe_argument_count);
        hash_u64(&ctx, value->recipe_rule_id);
        hash_u64(&ctx, value->recipe_callee_function);
        hash_u64(&ctx, value->recipe_hof_kind);
        hash_u64(&ctx, value->recipe_hof_source_storage);
        hash_u64(&ctx, value->recipe_hof_result_storage);
        hash_u64(&ctx, value->recipe_hof_callback_parameter_reps[0]);
        hash_u64(&ctx, value->recipe_hof_callback_parameter_reps[1]);
        hash_u64(&ctx, value->recipe_hof_callback_return_rep);
        hash_u64(&ctx, value->recipe_hof_reserved);
        hash_u64(&ctx, value->backing_value);
        hash_u64(&ctx, value->backing_element_count);
        hash_u64(&ctx, value->address_projection);
        hash_u64(&ctx, value->backing_native_type);
        hash_u64(&ctx, value->projection_reserved);
        size_t c_type_length = strlen(value->c_type);
        hash_u64(&ctx, c_type_length);
        xr_sha256_update(&ctx, (const uint8_t *) value->c_type, c_type_length);
        size_t backing_c_type_length = value->backing_c_type ? strlen(value->backing_c_type) : 0;
        hash_u64(&ctx, backing_c_type_length);
        if (backing_c_type_length)
            xr_sha256_update(&ctx, (const uint8_t *) value->backing_c_type, backing_c_type_length);
        if (value->literal_byte_length)
            xr_sha256_update(&ctx, (const uint8_t *) value->literal_bytes,
                             value->literal_byte_length);
        size_t recipe_symbol_length = value->recipe_symbol ? strlen(value->recipe_symbol) : 0;
        hash_u64(&ctx, recipe_symbol_length);
        if (recipe_symbol_length)
            xr_sha256_update(&ctx, (const uint8_t *) value->recipe_symbol, recipe_symbol_length);
        size_t recipe_type_name_length =
            value->recipe_type_name ? strlen(value->recipe_type_name) : 0;
        hash_u64(&ctx, recipe_type_name_length);
        if (recipe_type_name_length)
            xr_sha256_update(&ctx, (const uint8_t *) value->recipe_type_name,
                             recipe_type_name_length);
        size_t recipe_member_name_length =
            value->recipe_member_name ? strlen(value->recipe_member_name) : 0;
        hash_u64(&ctx, recipe_member_name_length);
        if (recipe_member_name_length)
            xr_sha256_update(&ctx, (const uint8_t *) value->recipe_member_name,
                             recipe_member_name_length);
    }
    for (uint32_t i = 0; i < plan->call_argument_count; i++) {
        const XrCCallArgumentEmissionView *argument = &plan->call_arguments[i];
        hash_u64(&ctx, argument->semantic_call_value);
        hash_u64(&ctx, argument->semantic_operand);
        hash_u64(&ctx, argument->semantic_value);
        hash_u64(&ctx, argument->callee_parameter);
        hash_u64(&ctx, argument->ordinal);
        hash_u64(&ctx, argument->caller_register_kind);
        hash_u64(&ctx, argument->caller_memory_kind);
        hash_u64(&ctx, argument->callee_register_kind);
        hash_u64(&ctx, argument->callee_memory_kind);
        hash_u64(&ctx, argument->mode);
        hash_u64(&ctx, argument->ownership);
        hash_u64(&ctx, argument->transfer_mode);
        hash_u64(&ctx, argument->flags);
        hash_u64(&ctx, argument->array_element_storage);
        for (uint32_t r = 0; r < sizeof(argument->reserved); r++)
            hash_u64(&ctx, argument->reserved[r]);
        size_t c_type_length = argument->c_type ? strlen(argument->c_type) : 0;
        hash_u64(&ctx, c_type_length);
        if (c_type_length)
            xr_sha256_update(&ctx, (const uint8_t *) argument->c_type, c_type_length);
    }
    for (uint32_t i = 0; i < plan->recipe_argument_count; i++) {
        const XrCRecipeArgumentView *argument = &plan->recipe_arguments[i];
        hash_u64(&ctx, argument->semantic_value);
        hash_u64(&ctx, argument->source_semantic_value);
        hash_u64(&ctx, argument->kind);
        hash_u64(&ctx, argument->reserved[0]);
        hash_u64(&ctx, argument->reserved[1]);
        hash_u64(&ctx, argument->reserved[2]);
    }
    for (uint32_t i = 0; i < plan->cleanup_count; i++) {
        const XrCCleanupEmissionView *cleanup = &plan->cleanups[i];
        hash_u64(&ctx, cleanup->semantic_operation);
        hash_u64(&ctx, cleanup->semantic_value);
        hash_u64(&ctx, cleanup->target_slot);
        hash_u64(&ctx, cleanup->action);
        hash_u64(&ctx, cleanup->flags);
        hash_u64(&ctx, cleanup->reserved);
        size_t symbol_length = cleanup->recipe_symbol ? strlen(cleanup->recipe_symbol) : 0;
        hash_u64(&ctx, symbol_length);
        if (symbol_length)
            xr_sha256_update(&ctx, (const uint8_t *) cleanup->recipe_symbol, symbol_length);
    }
    xr_sha256_final(&ctx, out->bytes);
}

static bool verify_value(const XrCValueEmissionView *value) {
    XrCValueRep expected_rep = XR_C_VALUE_REP_COUNT;
    const char *expected_c_type = NULL;
    if (value->target_register_kind != value->target_memory_kind)
        return false;
    switch (value->target_register_kind) {
        case XR_MACHINE_REP_VOID:
            expected_rep = XR_C_VALUE_REP_VOID;
            expected_c_type = "void";
            break;
        case XR_MACHINE_REP_I1:
            expected_rep = XR_C_VALUE_REP_BOOL;
            expected_c_type = "uint8_t";
            break;
        case XR_MACHINE_REP_I8:
            expected_rep = XR_C_VALUE_REP_I8;
            expected_c_type = "int8_t";
            break;
        case XR_MACHINE_REP_U8:
            expected_rep = XR_C_VALUE_REP_U8;
            expected_c_type = "uint8_t";
            break;
        case XR_MACHINE_REP_I16:
            expected_rep = XR_C_VALUE_REP_I16;
            expected_c_type = "int16_t";
            break;
        case XR_MACHINE_REP_U16:
            expected_rep = XR_C_VALUE_REP_U16;
            expected_c_type = "uint16_t";
            break;
        case XR_MACHINE_REP_I32:
            expected_rep = XR_C_VALUE_REP_I32;
            expected_c_type = "int32_t";
            break;
        case XR_MACHINE_REP_U32:
            expected_rep = XR_C_VALUE_REP_U32;
            expected_c_type = "uint32_t";
            break;
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_ENUM_ORDINAL:
            expected_rep = XR_C_VALUE_REP_I64;
            expected_c_type = "int64_t";
            break;
        case XR_MACHINE_REP_U64:
            expected_rep = XR_C_VALUE_REP_U64;
            expected_c_type = "uint64_t";
            break;
        case XR_MACHINE_REP_ISIZE:
            expected_rep = XR_C_VALUE_REP_ISIZE;
            expected_c_type = "ptrdiff_t";
            break;
        case XR_MACHINE_REP_USIZE:
            expected_rep = XR_C_VALUE_REP_USIZE;
            expected_c_type = "size_t";
            break;
        case XR_MACHINE_REP_F32:
            expected_rep = XR_C_VALUE_REP_F32;
            expected_c_type = "float";
            break;
        case XR_MACHINE_REP_F64:
            expected_rep = XR_C_VALUE_REP_F64;
            expected_c_type = "double";
            break;
        case XR_MACHINE_REP_RUNE:
            expected_rep = XR_C_VALUE_REP_RUNE;
            expected_c_type = "uint32_t";
            break;
        case XR_MACHINE_REP_DYN_VALUE:
            expected_rep = XR_C_VALUE_REP_TAGGED;
            expected_c_type = "XrValue";
            break;
        case XR_MACHINE_REP_VIEW:
            expected_rep = XR_C_VALUE_REP_VIEW;
            expected_c_type = "xr_span_t";
            break;
        case XR_MACHINE_REP_AGGREGATE:
            expected_rep = XR_C_VALUE_REP_AGGREGATE;
            expected_c_type = value->c_type;
            if (!expected_c_type)
                return false;
            if (value->address_projection == XR_C_ADDRESS_PROJECTION_NAMED_AGGREGATE) {
                if (strncmp(expected_c_type, "xrt_struct_abi_", 15) != 0 ||
                    strlen(expected_c_type) != 31 || value->backing_value != 0 ||
                    value->backing_element_count != 0 || value->backing_native_type != 0 ||
                    value->backing_c_type != NULL)
                    return false;
            } else if (value->address_projection == XR_C_ADDRESS_PROJECTION_FIXED_ARRAY_BACKING) {
                /* Every lane of a fixed array carries the same scalar type, and
                 * the tag naming that type is zero for int64, so the tag cannot
                 * be tested for presence against zero. What the tag names is
                 * the test: a lane tag has to resolve to a scalar layout, and
                 * the backing has to spell that layout's C type and no other. */
                const XaotLayoutInfo *lane =
                    xaot_layout_for_native_type(value->backing_native_type);
                if (strcmp(expected_c_type, "XrValue") != 0 ||
                    value->backing_value != value->semantic_value ||
                    value->backing_element_count == 0 || !lane ||
                    lane->field_kind != XAOT_LAYOUT_FIELD_SCALAR || !lane->c_type ||
                    !value->backing_c_type || strcmp(value->backing_c_type, lane->c_type) != 0)
                    return false;
            } else if (value->address_projection == XR_C_ADDRESS_PROJECTION_TUPLE_BACKING) {
                /* Tuple lanes may each carry their own type, so the backing
                 * names no single element type a fixed array would. */
                if (strcmp(expected_c_type, "XrValue") != 0 ||
                    value->backing_value != value->semantic_value ||
                    value->backing_element_count == 0 || value->backing_native_type != 0 ||
                    value->backing_c_type != NULL)
                    return false;
            } else {
                return false;
            }
            break;
        case XR_MACHINE_REP_RAW_PTR:
            expected_rep = XR_C_VALUE_REP_RAW_PTR;
            if (!value->c_type ||
                (strcmp(value->c_type, "const void *") != 0 &&
                 strcmp(value->c_type, "void *") != 0 &&
                 strcmp(value->c_type, "const void * *") != 0 &&
                 strcmp(value->c_type, "void * *") != 0 && strcmp(value->c_type, "XrValue *") != 0))
                return false;
            expected_c_type = value->c_type;
            break;
        default:
            return false;
    }
    if (value->projection_reserved != 0 ||
        (value->target_register_kind != XR_MACHINE_REP_AGGREGATE &&
         (value->address_projection != XR_C_ADDRESS_PROJECTION_NONE || value->backing_value != 0 ||
          value->backing_element_count != 0 || value->backing_native_type != 0 ||
          value->backing_c_type != NULL)))
        return false;
    bool recipe_valid =
        value->materialization == XR_C_VALUE_MATERIALIZATION_NONE
            ? value->literal_byte_length == 0 && value->literal_bytes == NULL &&
                  value->recipe_operand_value == UINT32_MAX &&
                  value->recipe_argument_value == UINT32_MAX && value->recipe_symbol == NULL
            : value->materialization == XR_C_VALUE_MATERIALIZATION_STRING_LITERAL_VIEW &&
                  value->rep == XR_C_VALUE_REP_TAGGED && value->literal_bytes != NULL &&
                  strlen(value->literal_bytes) == value->literal_byte_length &&
                  value->recipe_operand_value == UINT32_MAX &&
                  value->recipe_argument_value == UINT32_MAX && value->recipe_symbol == NULL;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_CHANNEL_NEW)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_CHANNEL_NEW_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_CHANNEL_RECV_PAYLOAD)
        recipe_valid =
            value->rep != XR_C_VALUE_REP_VOID && value->rep != XR_C_VALUE_REP_TAGGED &&
            value->literal_byte_length == 0 && value->literal_bytes == NULL &&
            value->recipe_operand_value != UINT32_MAX &&
            value->recipe_argument_value == UINT32_MAX &&
            channel_receive_symbol(value->target_register_kind) && value->recipe_symbol &&
            strcmp(value->recipe_symbol, channel_receive_symbol(value->target_register_kind)) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_NEW)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value == UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_STRINGBUILDER_NEW_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_STRING_BYTE_SLICE_VIEW)
        recipe_valid = value->rep == XR_C_VALUE_REP_VIEW && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_STRING_BYTE_SLICE_VIEW_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_APPEND_RUNE)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value != UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_STRINGBUILDER_APPEND_RUNE_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_TO_STRING)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_STRINGBUILDER_TO_STRING_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_STRING_RUNES)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_STRING_RUNES_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_HAS_NEXT)
        recipe_valid = value->rep == XR_C_VALUE_REP_BOOL && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_ITERATOR_RUNE_HAS_NEXT_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_NEXT)
        recipe_valid = value->rep == XR_C_VALUE_REP_RUNE && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_ITERATOR_RUNE_NEXT_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_NTH)
        recipe_valid = value->rep == XR_C_VALUE_REP_RUNE && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value != UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_ITERATOR_RUNE_NTH_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_RUNE_TO_UINT32)
        recipe_valid = value->rep == XR_C_VALUE_REP_U32 && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_RUNE_TO_UINT32_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_RUNE_TO_STRING)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_RUNE_TO_STRING_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_RUNE_IS_WHITESPACE)
        recipe_valid = value->rep == XR_C_VALUE_REP_BOOL && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_RUNE_IS_WHITESPACE_SYMBOL) == 0;
    bool typed_rule =
        value->materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_PUSH_TAGGED;
    if (typed_rule)
        recipe_valid = value->recipe_rule_id != XR_C_EMISSION_RULE_NONE &&
                       value->rep == XR_C_VALUE_REP_VOID && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value != UINT32_MAX &&
                       value->recipe_discriminant > XR_TARGET_ARRAY_STORAGE_NONE &&
                       value->recipe_discriminant < XR_TARGET_ARRAY_STORAGE_COUNT &&
                       value->recipe_symbol != NULL;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_STRING_SLICE_RANGE) {
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX &&
                       value->recipe_argument_count == 2 && value->recipe_arguments != NULL &&
                       value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_STRING_SLICE_RANGE_SYMBOL) == 0;
        for (uint16_t i = 0; recipe_valid && i < 2; i++) {
            const XrCRecipeArgumentView *argument = &value->recipe_arguments[i];
            recipe_valid = argument->semantic_value != UINT32_MAX &&
                           argument->source_semantic_value == argument->semantic_value &&
                           argument->kind == XR_C_RECIPE_ARGUMENT_STRING_SLICE_BOUND &&
                           argument->reserved[0] == 0 && argument->reserved[1] == 0 &&
                           argument->reserved[2] == 0;
        }
    }
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_APPEND_STRING)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED &&
                       value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value != UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_STRINGBUILDER_APPEND_STRING_SYMBOL) == 0;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_STRING_SLICE_RANGE) {
        /* The complete range recipe was checked above. */
    } else if (value->materialization == XR_C_VALUE_MATERIALIZATION_STRING_CONCAT) {
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value == UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX &&
                       value->recipe_argument_count >= 2u && value->recipe_arguments != NULL &&
                       value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_STRING_CONCAT_SYMBOL) == 0;
        for (uint16_t i = 0; recipe_valid && i < value->recipe_argument_count; i++) {
            const XrCRecipeArgumentView *argument = &value->recipe_arguments[i];
            recipe_valid = argument->semantic_value != UINT32_MAX &&
                           argument->source_semantic_value != UINT32_MAX &&
                           (argument->kind == XR_C_RECIPE_ARGUMENT_STRING_VALUE ||
                            xr_c_recipe_argument_is_direct_scalar(argument->kind)) &&
                           argument->reserved[0] == 0 && argument->reserved[1] == 0 &&
                           argument->reserved[2] == 0;
        }
    } else if (value->materialization == XR_C_VALUE_MATERIALIZATION_ADT_ENUM_CONSTRUCTOR) {
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_layout_id != 0 &&
                       value->recipe_argument_count > 0 && value->recipe_arguments != NULL &&
                       value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_ADT_ENUM_CONSTRUCTOR_SYMBOL) == 0 &&
                       value->recipe_type_name && value->recipe_type_name[0] &&
                       value->recipe_member_name && value->recipe_member_name[0];
        for (uint16_t i = 0; recipe_valid && i < value->recipe_argument_count; i++) {
            const XrCRecipeArgumentView *argument = &value->recipe_arguments[i];
            recipe_valid = argument->semantic_value != UINT32_MAX &&
                           argument->source_semantic_value == argument->semantic_value &&
                           argument->kind == XR_C_RECIPE_ARGUMENT_ENUM_PAYLOAD &&
                           argument->reserved[0] == 0 && argument->reserved[1] == 0 &&
                           argument->reserved[2] == 0;
        }
    } else {
        recipe_valid =
            recipe_valid && value->recipe_argument_count == 0 && value->recipe_arguments == NULL;
    }
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_PANIC_CATCH)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value == UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX &&
                       value->recipe_argument_count == 0 && value->recipe_arguments == NULL &&
                       value->recipe_symbol == NULL;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_LOCAL_ADDRESS)
        recipe_valid = value->rep == XR_C_VALUE_REP_RAW_PTR &&
                       value->target_register_kind == XR_MACHINE_REP_RAW_PTR &&
                       value->target_memory_kind == XR_MACHINE_REP_RAW_PTR &&
                       value->literal_byte_length == 0 && value->literal_bytes == NULL &&
                       value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_operand_value != value->semantic_value &&
                       value->recipe_argument_value == UINT32_MAX &&
                       value->recipe_argument_count == 0 && value->recipe_arguments == NULL &&
                       value->recipe_symbol == NULL;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_SCALAR_ADDRESSABLE_ALIAS)
        recipe_valid = c_rep_is_addressable_scalar(value->target_register_kind) &&
                       value->target_register_kind == value->target_memory_kind &&
                       value->rep != XR_C_VALUE_REP_VOID && value->rep != XR_C_VALUE_REP_TAGGED &&
                       value->rep != XR_C_VALUE_REP_VIEW &&
                       value->rep != XR_C_VALUE_REP_AGGREGATE && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_operand_value != value->semantic_value &&
                       value->recipe_argument_value == UINT32_MAX &&
                       value->recipe_argument_count == 0 && value->recipe_arguments == NULL &&
                       value->recipe_symbol == NULL;
    bool array_recipe = value->materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_WITH_CAPACITY ||
                        value->materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_FILLED_NEW;
    if (array_recipe)
        recipe_valid =
            value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
            value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
            (value->materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_WITH_CAPACITY
                 ? value->recipe_argument_value == UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_ARRAY_WITH_CAPACITY_SYMBOL) == 0
                 : value->recipe_argument_value != UINT32_MAX && value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_ARRAY_FILLED_NEW_SYMBOL) == 0) &&
            value->recipe_discriminant > XR_TARGET_ARRAY_STORAGE_NONE &&
            value->recipe_discriminant < XR_TARGET_ARRAY_STORAGE_TAGGED &&
            value->recipe_argument_count == 0 && value->recipe_arguments == NULL;
    bool array_fill_member = value->materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_FILL_SCALAR;
    if (array_fill_member)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value != UINT32_MAX && value->recipe_layout_id == 0 &&
                       value->recipe_discriminant > XR_TARGET_ARRAY_STORAGE_NONE &&
                       value->recipe_discriminant < XR_TARGET_ARRAY_STORAGE_TAGGED &&
                       value->recipe_argument_count == 0 && value->recipe_arguments == NULL &&
                       value->recipe_symbol == NULL && value->recipe_type_name == NULL &&
                       value->recipe_member_name == NULL;
    bool array_allocation = value->materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_NEW;
    if (array_allocation)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_layout_id == 0 &&
                       value->recipe_discriminant > XR_TARGET_ARRAY_STORAGE_NONE &&
                       value->recipe_discriminant <= XR_TARGET_ARRAY_STORAGE_TAGGED &&
                       value->recipe_argument_count == 0 && value->recipe_arguments == NULL &&
                       value->recipe_symbol &&
                       strcmp(value->recipe_symbol, XR_C_ARRAY_NEW_SYMBOL) == 0 &&
                       value->recipe_type_name == NULL && value->recipe_member_name == NULL;
    bool direct_tagged_ref_parameter =
        value->materialization == XR_C_VALUE_MATERIALIZATION_DIRECT_LOCAL_TAGGED_REF_PARAMETER;
    if (direct_tagged_ref_parameter)
        recipe_valid = value->rep == XR_C_VALUE_REP_RAW_PTR && value->c_type &&
                       strcmp(value->c_type, "XrValue *") == 0 && value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL && value->recipe_operand_value == UINT32_MAX &&
                       value->recipe_argument_value == UINT32_MAX && value->recipe_layout_id == 0 &&
                       value->recipe_discriminant < XR_TARGET_ARRAY_STORAGE_COUNT &&
                       value->recipe_argument_count == 0 && value->recipe_arguments == NULL &&
                       value->recipe_symbol == NULL && value->recipe_type_name == NULL &&
                       value->recipe_member_name == NULL;
    bool array_hof_direct = value->materialization == XR_C_VALUE_MATERIALIZATION_ARRAY_HOF_DIRECT;
    if (array_hof_direct) {
        uint16_t source_machine_kind = XR_MACHINE_REP_COUNT;
        uint16_t result_machine_kind = XR_MACHINE_REP_COUNT;
        XrCValueRep source_rep = XR_C_VALUE_REP_COUNT;
        XrCValueRep result_rep = XR_C_VALUE_REP_COUNT;
        const char *ignored_type = NULL;
        bool storage_exact =
            c_array_storage_projection(value->recipe_hof_source_storage, &source_machine_kind,
                                       &source_rep, &ignored_type) &&
            c_array_storage_projection(value->recipe_hof_result_storage, &result_machine_kind,
                                       &result_rep, &ignored_type);
        uint16_t expected_arguments = value->recipe_hof_kind == XR_C_ARRAY_HOF_REDUCE ? 3u : 2u;
        uint8_t expected_parameter0 = value->recipe_hof_kind == XR_C_ARRAY_HOF_REDUCE
                                          ? (uint8_t) result_rep
                                          : (uint8_t) source_rep;
        uint8_t expected_parameter1 = value->recipe_hof_kind == XR_C_ARRAY_HOF_REDUCE
                                          ? (uint8_t) source_rep
                                          : (uint8_t) XR_C_VALUE_REP_VOID;
        uint8_t expected_return = value->recipe_hof_kind == XR_C_ARRAY_HOF_FILTER
                                      ? (uint8_t) XR_C_VALUE_REP_BOOL
                                      : (uint8_t) result_rep;
        recipe_valid =
            storage_exact && value->recipe_callee_function != UINT32_MAX &&
            value->recipe_hof_kind > XR_C_ARRAY_HOF_NONE &&
            value->recipe_hof_kind < XR_C_ARRAY_HOF_COUNT && value->recipe_hof_reserved == 0 &&
            value->rep == (value->recipe_hof_kind == XR_C_ARRAY_HOF_REDUCE
                               ? result_rep
                               : XR_C_VALUE_REP_TAGGED) &&
            value->literal_byte_length == 0 && value->literal_bytes == NULL &&
            value->recipe_operand_value == UINT32_MAX &&
            value->recipe_argument_value == UINT32_MAX && value->recipe_layout_id == 0 &&
            value->recipe_discriminant == 0 && value->recipe_symbol == NULL &&
            value->recipe_type_name == NULL && value->recipe_member_name == NULL &&
            value->recipe_hof_callback_parameter_reps[0] == expected_parameter0 &&
            value->recipe_hof_callback_parameter_reps[1] == expected_parameter1 &&
            value->recipe_hof_callback_return_rep == expected_return &&
            value->recipe_argument_count == expected_arguments && value->recipe_arguments != NULL;
        for (uint16_t i = 0; recipe_valid && i < expected_arguments; i++) {
            const XrCRecipeArgumentView *argument = &value->recipe_arguments[i];
            uint8_t expected_kind = i == 0   ? XR_C_RECIPE_ARGUMENT_ARRAY_HOF_RECEIVER
                                    : i == 1 ? XR_C_RECIPE_ARGUMENT_ARRAY_HOF_CALLBACK
                                             : XR_C_RECIPE_ARGUMENT_ARRAY_HOF_SEED;
            recipe_valid = argument->semantic_value != UINT32_MAX &&
                           argument->source_semantic_value == argument->semantic_value &&
                           argument->kind == expected_kind && argument->reserved[0] == 0 &&
                           argument->reserved[1] == 0 && argument->reserved[2] == 0;
        }
    }
    if (value->materialization != XR_C_VALUE_MATERIALIZATION_ADT_ENUM_CONSTRUCTOR &&
        !array_recipe && !array_fill_member && !array_allocation && !direct_tagged_ref_parameter &&
        !array_hof_direct && !typed_rule)
        recipe_valid = recipe_valid && value->recipe_layout_id == 0 &&
                       value->recipe_discriminant == 0 && value->recipe_type_name == NULL &&
                       value->recipe_member_name == NULL;
    if (array_recipe || array_allocation)
        recipe_valid = recipe_valid && value->recipe_layout_id == 0 &&
                       value->recipe_type_name == NULL && value->recipe_member_name == NULL;
    if (value->materialization != XR_C_VALUE_MATERIALIZATION_ARRAY_HOF_DIRECT)
        recipe_valid = recipe_valid && value->recipe_callee_function == UINT32_MAX &&
                       value->recipe_hof_kind == XR_C_ARRAY_HOF_NONE &&
                       value->recipe_hof_source_storage == 0 &&
                       value->recipe_hof_result_storage == 0 &&
                       value->recipe_hof_callback_parameter_reps[0] == XR_C_VALUE_REP_VOID &&
                       value->recipe_hof_callback_parameter_reps[1] == XR_C_VALUE_REP_VOID &&
                       value->recipe_hof_callback_return_rep == XR_C_VALUE_REP_VOID &&
                       value->recipe_hof_reserved == 0;
    if (!typed_rule && value->recipe_rule_id != XR_C_EMISSION_RULE_NONE)
        return false;
    return expected_rep == (XrCValueRep) value->rep && value->c_type && value->reserved == 0 &&
           recipe_valid &&
           strcmp(value->c_type, expected_c_type) == 0 &&
           (value->rep == XR_C_VALUE_REP_VOID
                ? value->register_bits == 0 && value->memory_size == 0 && value->memory_align == 0
                : value->register_bits != 0 && value->memory_size != 0 && value->memory_align != 0);
}

static bool verify_plan(const XrCEmissionPlan *plan) {
    if (!plan || plan->schema_version != XR_C_EMISSION_PLAN_SCHEMA_VERSION ||
        (plan->value_count && !plan->values) ||
        (plan->call_argument_count && !plan->call_arguments) ||
        (!plan->call_argument_count && plan->call_arguments) ||
        (plan->recipe_argument_count && !plan->recipe_arguments) ||
        (!plan->recipe_argument_count && plan->recipe_arguments) ||
        (plan->cleanup_count && !plan->cleanups) || (!plan->cleanup_count && plan->cleanups))
        return false;
    uint32_t next_argument = 0;
    for (uint32_t i = 0; i < plan->value_count; i++) {
        const XrCValueEmissionView *value = &plan->values[i];
        if (!verify_value(value) ||
            (i && plan->values[i - 1u].semantic_value >= plan->values[i].semantic_value))
            return false;
        if (value->recipe_argument_count) {
            if (value->recipe_argument_count > plan->recipe_argument_count - next_argument ||
                value->recipe_arguments != &plan->recipe_arguments[next_argument])
                return false;
            next_argument += value->recipe_argument_count;
        }
    }
    if (next_argument != plan->recipe_argument_count)
        return false;
    for (uint32_t i = 0; i < plan->call_argument_count; i++) {
        const XrCCallArgumentEmissionView *argument = &plan->call_arguments[i];
        if (!argument->c_type || strcmp(argument->c_type, "XrValue *") != 0 ||
            argument->caller_register_kind != XR_MACHINE_REP_DYN_VALUE ||
            argument->caller_memory_kind != XR_MACHINE_REP_DYN_VALUE ||
            argument->callee_register_kind != XR_MACHINE_REP_RAW_PTR ||
            argument->callee_memory_kind != XR_MACHINE_REP_RAW_PTR ||
            argument->mode != XR_TARGET_CALL_REFERENCE ||
            argument->ownership != XR_TARGET_CALL_BORROW ||
            argument->transfer_mode != XR_TRANSFER_SHARE ||
            argument->flags != XR_TARGET_CALL_ARGUMENT_ADDRESSABLE ||
            argument->array_element_storage >= XR_TARGET_ARRAY_STORAGE_COUNT ||
            argument->reserved[0] != 0 || argument->reserved[1] != 0 ||
            argument->reserved[2] != 0 ||
            (i &&
             (plan->call_arguments[i - 1u].semantic_call_value > argument->semantic_call_value ||
              (plan->call_arguments[i - 1u].semantic_call_value == argument->semantic_call_value &&
               plan->call_arguments[i - 1u].ordinal >= argument->ordinal))))
            return false;
    }
    for (uint32_t i = 0; i < plan->cleanup_count; i++) {
        const XrCCleanupEmissionView *cleanup = &plan->cleanups[i];
        if (cleanup->semantic_operation == UINT32_MAX || cleanup->semantic_value == UINT32_MAX ||
            cleanup->target_slot == UINT32_MAX || cleanup->action != XR_C_CLEANUP_RELEASE ||
            cleanup->flags != 0 || cleanup->reserved != 0 || !cleanup->recipe_symbol ||
            strcmp(cleanup->recipe_symbol, XR_C_RELEASE_SYMBOL) != 0 ||
            (i && plan->cleanups[i - 1u].semantic_operation >= cleanup->semantic_operation))
            return false;
    }
    XrFingerprint actual = {{0}};
    compute_fingerprint(plan, &actual);
    return xr_fingerprint_equal(actual, plan->fingerprint);
}

static bool verify_target_kind_projection(const XrTargetPlan *target_plan, uint32_t semantic_value,
                                          uint16_t kind, XrCValueRep *out_rep,
                                          const char **out_c_type) {
    if (!out_rep || !out_c_type)
        return false;
    switch ((XrMachineRepKind) kind) {
        case XR_MACHINE_REP_VOID:
            *out_rep = XR_C_VALUE_REP_VOID;
            *out_c_type = "void";
            return true;
        case XR_MACHINE_REP_I1:
            *out_rep = XR_C_VALUE_REP_BOOL;
            *out_c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I8:
            *out_rep = XR_C_VALUE_REP_I8;
            *out_c_type = "int8_t";
            return true;
        case XR_MACHINE_REP_U8:
            *out_rep = XR_C_VALUE_REP_U8;
            *out_c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I16:
            *out_rep = XR_C_VALUE_REP_I16;
            *out_c_type = "int16_t";
            return true;
        case XR_MACHINE_REP_U16:
            *out_rep = XR_C_VALUE_REP_U16;
            *out_c_type = "uint16_t";
            return true;
        case XR_MACHINE_REP_I32:
            *out_rep = XR_C_VALUE_REP_I32;
            *out_c_type = "int32_t";
            return true;
        case XR_MACHINE_REP_U32:
            *out_rep = XR_C_VALUE_REP_U32;
            *out_c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_ENUM_ORDINAL:
            *out_rep = XR_C_VALUE_REP_I64;
            *out_c_type = "int64_t";
            return true;
        case XR_MACHINE_REP_U64:
            *out_rep = XR_C_VALUE_REP_U64;
            *out_c_type = "uint64_t";
            return true;
        case XR_MACHINE_REP_ISIZE:
            *out_rep = XR_C_VALUE_REP_ISIZE;
            *out_c_type = "ptrdiff_t";
            return true;
        case XR_MACHINE_REP_USIZE:
            *out_rep = XR_C_VALUE_REP_USIZE;
            *out_c_type = "size_t";
            return true;
        case XR_MACHINE_REP_F32:
            *out_rep = XR_C_VALUE_REP_F32;
            *out_c_type = "float";
            return true;
        case XR_MACHINE_REP_F64:
            *out_rep = XR_C_VALUE_REP_F64;
            *out_c_type = "double";
            return true;
        case XR_MACHINE_REP_RUNE:
            *out_rep = XR_C_VALUE_REP_RUNE;
            *out_c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_DYN_VALUE:
            *out_rep = XR_C_VALUE_REP_TAGGED;
            *out_c_type = "XrValue";
            return true;
        case XR_MACHINE_REP_VIEW:
            *out_rep = XR_C_VALUE_REP_VIEW;
            *out_c_type = "xr_span_t";
            return true;
        case XR_MACHINE_REP_RAW_PTR:
            *out_rep = XR_C_VALUE_REP_RAW_PTR;
            *out_c_type = emission_source_ref_place_c_type(target_plan, semantic_value);
            if (!*out_c_type)
                *out_c_type = emission_raw_pointer_c_type(target_plan, semantic_value);
            if (!*out_c_type) {
                const XrTargetValueRepRecord *binding =
                    xr_target_plan_value_rep(target_plan, semantic_value);
                uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
                if (exact_direct_local_tagged_ref_parameter_recipe(target_plan, binding, &storage))
                    *out_c_type = "XrValue *";
            }
            if (!*out_c_type)
                *out_c_type = emission_local_address_c_type(target_plan, semantic_value);
            return *out_c_type != NULL;
        default:
            return false;
    }
}

static bool verify_container_copy_call_storage(const XrTargetPlan *target_plan) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t operation_index = 0; operation_index < operation_count; operation_index++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, operation_index);
        uint8_t semantic_storage = XR_ELEM_ANY;
        if (!xr_semantic_container_copy_is_exact(semantic, operation, NULL, &semantic_storage))
            continue;
        uint8_t expected_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        if (semantic_storage == XR_ELEM_ANY)
            expected_storage = XR_TARGET_ARRAY_STORAGE_TAGGED;
        else if (!c_array_storage_from_semantic(semantic_storage, &expected_storage))
            return false;
        const XrTargetCallRecord *match = NULL;
        for (uint32_t call_index = 0; calls && call_index < call_count; call_index++) {
            if (calls[call_index].semantic_operation != operation_index)
                continue;
            if (match)
                return false;
            match = &calls[call_index];
        }
        if (!match ||
            match->calling_convention != XR_TARGET_CALL_CONVENTION_CONTAINER_COPY ||
            match->target_kind != XR_TARGET_CALL_TARGET_CONTAINER_COPY ||
            match->array_element_storage != expected_storage)
            return false;
    }
    for (uint32_t call_index = 0; calls && call_index < call_count; call_index++) {
        const XrTargetCallRecord *call = &calls[call_index];
        if (call->calling_convention != XR_TARGET_CALL_CONVENTION_CONTAINER_COPY &&
            call->target_kind != XR_TARGET_CALL_TARGET_CONTAINER_COPY)
            continue;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, call->semantic_operation);
        if (!xr_semantic_container_copy_is_exact(semantic, operation, NULL, NULL))
            return false;
    }
    return true;
}

bool xr_c_emission_plan_verify(const XrCEmissionPlan *plan, const XrTargetPlan *target_plan,
                               XrFingerprint expected_profile_fingerprint, char *error,
                               size_t error_size) {
    if (!plan || !target_plan || !xr_target_plan_is_verified(target_plan) ||
        !verify_container_copy_call_storage(target_plan))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission verification authority is missing");
    XrFingerprint target_fingerprint = xr_target_plan_fingerprint(target_plan);
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    if (!profile || !xr_fingerprint_equal(profile_fingerprint, expected_profile_fingerprint) ||
        !xr_fingerprint_equal(plan->profile_fingerprint, expected_profile_fingerprint))
        return emission_error(error, error_size, "XR_TARGET_1000",
                              "C emission profile fingerprint is stale");
    if (!xr_fingerprint_equal(plan->target_fingerprint, target_fingerprint))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission TargetPlan fingerprint is stale");
    if (plan->schema_version != XR_C_EMISSION_PLAN_SCHEMA_VERSION ||
        (plan->value_count && !plan->values) ||
        (plan->call_argument_count && !plan->call_arguments) ||
        (!plan->call_argument_count && plan->call_arguments) ||
        (plan->recipe_argument_count && !plan->recipe_arguments) ||
        (!plan->recipe_argument_count && plan->recipe_arguments) ||
        (plan->cleanup_count && !plan->cleanups) || (!plan->cleanup_count && plan->cleanups))
        return emission_error(error, error_size, "XR_TARGET_1001", "C emission schema is invalid");

    uint32_t value_count = 0;
    const XrTargetValueRepRecord *values = xr_target_plan_value_reps(target_plan, &value_count);
    if (value_count && !values)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "TargetPlan value-representation table is missing");
    uint32_t projected = 0;
    uint32_t projected_recipe_arguments = 0;
    for (uint32_t i = 0; i < value_count; i++) {
        const XrTargetValueRepRecord *binding = &values[i];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        XrCValueRep expected_register = XR_C_VALUE_REP_COUNT;
        XrCValueRep expected_memory = XR_C_VALUE_REP_COUNT;
        const char *register_c_type = NULL;
        const char *memory_c_type = NULL;
        bool register_supported =
            register_rep &&
            verify_target_kind_projection(target_plan, binding->semantic_value, register_rep->kind,
                                          &expected_register, &register_c_type);
        bool memory_supported =
            memory_rep &&
            verify_target_kind_projection(target_plan, binding->semantic_value, memory_rep->kind,
                                          &expected_memory, &memory_c_type);
        XrCAggregateProjection aggregate = {0};
        bool aggregate_supported = false;
        if (register_rep && memory_rep && register_rep->kind == XR_MACHINE_REP_AGGREGATE &&
            memory_rep->kind == XR_MACHINE_REP_AGGREGATE &&
            xr_c_aggregate_projection(target_plan, binding, &aggregate)) {
            aggregate_supported = true;
            expected_register = expected_memory = XR_C_VALUE_REP_AGGREGATE;
            register_c_type = memory_c_type = aggregate.c_type;
            register_supported = memory_supported = true;
        }
        if (!register_supported && !memory_supported)
            continue;
        if (!register_supported || !memory_supported || register_rep->kind != memory_rep->kind ||
            expected_register != expected_memory || strcmp(register_c_type, memory_c_type) != 0)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "TargetPlan C projection is inconsistent");
        if (projected >= plan->value_count)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission projection is missing a TargetPlan row");
        const XrCValueEmissionView *row = &plan->values[projected++];
        if (row->semantic_value > binding->semantic_value)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission projection is missing a TargetPlan row");
        if (row->semantic_value < binding->semantic_value)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission projection has an extra row");
        if (row->semantic_value != binding->semantic_value ||
            row->target_register_rep != binding->register_rep ||
            row->target_memory_rep != binding->memory_rep ||
            row->target_register_kind != register_rep->kind ||
            row->target_memory_kind != memory_rep->kind ||
            row->register_bits != register_rep->register_bits ||
            row->memory_size != memory_rep->memory_size ||
            row->memory_align != memory_rep->memory_align ||
            row->rep != (uint8_t) expected_register || !row->c_type ||
            strcmp(row->c_type, register_c_type) != 0 ||
            (aggregate_supported &&
             (row->address_projection != aggregate.kind ||
              row->backing_value != aggregate.backing_value ||
              row->backing_element_count != aggregate.element_count ||
              row->backing_native_type != aggregate.element_native_type ||
              ((aggregate.element_c_type[0] == '\0') != (row->backing_c_type == NULL)) ||
              (aggregate.element_c_type[0] != '\0' &&
               strcmp(row->backing_c_type, aggregate.element_c_type) != 0))) ||
            (!aggregate_supported && row->address_projection != XR_C_ADDRESS_PROJECTION_NONE))
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission row disagrees with TargetPlan authority");
        XrCEmissionRuleLocation typed_location = {0};
        XrCEmissionRuleMatch typed_location_match =
            xr_c_emission_rule_locate(target_plan, binding, &typed_location);
        if (typed_location_match == XR_C_EMISSION_RULE_MALFORMED)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "typed C emission structural projection is malformed");
        XrCEmissionRuleDecision typed_actual = {
            row->recipe_rule_id, row->materialization, row->rep,
            row->recipe_discriminant, row->recipe_symbol,
        };
        const char *typed_diagnostic = NULL;
        XrCEmissionRuleMatch typed_rule_match =
            typed_location_match == XR_C_EMISSION_RULE_EXACT
                ? xr_c_emission_rule_verify(&typed_location.facts, &typed_actual,
                                            &typed_diagnostic)
                : XR_C_EMISSION_RULE_NOT_APPLICABLE;
        if (typed_rule_match == XR_C_EMISSION_RULE_MALFORMED)
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  typed_diagnostic ? typed_diagnostic
                                                   : "typed C emission rule mismatch");
        bool expected_typed_rule = typed_rule_match == XR_C_EMISSION_RULE_EXACT;
        const char *expected_literal = verify_expected_string_literal(target_plan, binding);
        uint32_t expected_capacity = UINT32_MAX;
        bool expected_channel = exact_channel_new_recipe(target_plan, binding, &expected_capacity);
        uint32_t expected_receiver = UINT32_MAX;
        const char *expected_receive_symbol =
            verify_channel_receive_recipe(target_plan, binding, &expected_receiver);
        bool expected_stringbuilder = exact_stringbuilder_new_recipe(target_plan, binding);
        uint32_t expected_view_source = UINT32_MAX;
        bool expected_string_byte_slice_view =
            verify_exact_string_byte_slice_view_recipe(target_plan, binding, &expected_view_source);
        uint32_t expected_append_receiver = UINT32_MAX;
        uint32_t expected_append_argument = UINT32_MAX;
        bool expected_stringbuilder_append = exact_stringbuilder_append_rune_recipe(
            target_plan, binding, &expected_append_receiver, &expected_append_argument);
        uint32_t expected_finish_receiver = UINT32_MAX;
        bool expected_stringbuilder_finish =
            exact_stringbuilder_to_string_recipe(target_plan, binding, &expected_finish_receiver);
        uint32_t expected_string_runes_receiver = UINT32_MAX;
        bool expected_string_runes =
            exact_string_runes_recipe(target_plan, binding, &expected_string_runes_receiver);
        uint32_t expected_iterator_rune_has_next_receiver = UINT32_MAX;
        bool expected_iterator_rune_has_next = exact_iterator_rune_has_next_recipe(
            target_plan, binding, &expected_iterator_rune_has_next_receiver);
        uint32_t expected_iterator_rune_next_receiver = UINT32_MAX;
        bool expected_iterator_rune_next = exact_iterator_rune_next_recipe(
            target_plan, binding, &expected_iterator_rune_next_receiver);
        uint32_t expected_iterator_rune_nth_receiver = UINT32_MAX;
        uint32_t expected_iterator_rune_nth_index = UINT32_MAX;
        bool expected_iterator_rune_nth = exact_iterator_rune_nth_recipe(
            target_plan, binding, &expected_iterator_rune_nth_receiver,
            &expected_iterator_rune_nth_index);
        uint32_t expected_rune_to_uint32_receiver = UINT32_MAX;
        bool expected_rune_to_uint32 =
            exact_rune_to_uint32_recipe(target_plan, binding, &expected_rune_to_uint32_receiver);
        uint32_t expected_rune_to_string_receiver = UINT32_MAX;
        bool expected_rune_to_string =
            exact_rune_to_string_recipe(target_plan, binding, &expected_rune_to_string_receiver);
        uint32_t expected_rune_is_whitespace_receiver = UINT32_MAX;
        bool expected_rune_is_whitespace = exact_rune_is_whitespace_recipe(
            target_plan, binding, &expected_rune_is_whitespace_receiver);
        uint32_t expected_string_slice_receiver = UINT32_MAX;
        uint32_t expected_string_slice_start = UINT32_MAX;
        uint32_t expected_string_slice_end = UINT32_MAX;
        bool expected_string_slice_range = exact_string_slice_range_recipe(
            target_plan, binding, &expected_string_slice_receiver, &expected_string_slice_start,
            &expected_string_slice_end);
        uint32_t expected_append_string_receiver = UINT32_MAX,
                 expected_append_string_argument = UINT32_MAX;
        bool expected_append_string = exact_stringbuilder_append_string_recipe(
            target_plan, binding, &expected_append_string_receiver,
            &expected_append_string_argument);
        const XrSemanticOperandRecord *expected_concat_arguments = NULL;
        uint16_t expected_concat_argument_count = 0;
        bool expected_string_concat = exact_string_concat_recipe(
            target_plan, binding, &expected_concat_arguments, &expected_concat_argument_count);
        bool expected_panic_catch = exact_panic_catch_recipe(target_plan, binding);
        XrSemanticAdtEnumConstructorShape expected_enum = {0};
        const XrSemanticOperandRecord *expected_enum_payloads = NULL;
        bool expected_adt_enum = exact_adt_enum_constructor_recipe(
            target_plan, binding, &expected_enum, &expected_enum_payloads);
        uint8_t expected_array_recipe = XR_C_VALUE_MATERIALIZATION_NONE;
        uint8_t expected_array_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        uint32_t expected_array_count = UINT32_MAX;
        uint32_t expected_array_fill = UINT32_MAX;
        const char *expected_array_symbol = NULL;
        bool expected_array = exact_array_intrinsic_recipe(
            target_plan, binding, &expected_array_recipe, &expected_array_storage,
            &expected_array_count, &expected_array_fill, &expected_array_symbol);
        uint32_t expected_scalar_source = UINT32_MAX;
        bool expected_scalar_alias =
            exact_scalar_addressable_alias_recipe(target_plan, binding, &expected_scalar_source);
        uint8_t expected_array_fill_member_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        uint32_t expected_array_fill_receiver = UINT32_MAX;
        uint32_t expected_array_fill_argument = UINT32_MAX;
        bool expected_array_fill_member = exact_array_fill_scalar_recipe(
            target_plan, binding, &expected_array_fill_member_storage,
            &expected_array_fill_receiver, &expected_array_fill_argument);
        uint8_t expected_array_allocation_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        uint32_t expected_array_allocation_count = UINT32_MAX;
        const char *expected_array_allocation_symbol = NULL;
        bool expected_array_allocation = exact_array_allocation_recipe(
            target_plan, binding, &expected_array_allocation_storage,
            &expected_array_allocation_count, &expected_array_allocation_symbol);
        uint8_t expected_direct_tagged_ref_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        bool expected_direct_tagged_ref_parameter =
            exact_direct_local_tagged_ref_parameter_recipe(
                target_plan, binding, &expected_direct_tagged_ref_storage);
        XrCArrayHofDirectRecipe expected_array_hof = {0};
        bool expected_array_hof_direct =
            exact_array_hof_direct_recipe(target_plan, binding, &expected_array_hof);
        uint32_t expected_local_address_source = UINT32_MAX;
        bool expected_local_address =
            exact_local_address_recipe(target_plan, binding, &expected_local_address_source);
        uint8_t expected_recipe =
            expected_typed_rule      ? row->materialization
            : expected_scalar_alias  ? XR_C_VALUE_MATERIALIZATION_SCALAR_ADDRESSABLE_ALIAS
            : expected_local_address ? XR_C_VALUE_MATERIALIZATION_LOCAL_ADDRESS
            : expected_direct_tagged_ref_parameter
                ? XR_C_VALUE_MATERIALIZATION_DIRECT_LOCAL_TAGGED_REF_PARAMETER
            : expected_adt_enum               ? XR_C_VALUE_MATERIALIZATION_ADT_ENUM_CONSTRUCTOR
            : expected_panic_catch            ? XR_C_VALUE_MATERIALIZATION_PANIC_CATCH
            : expected_literal                ? XR_C_VALUE_MATERIALIZATION_STRING_LITERAL_VIEW
            : expected_channel                ? XR_C_VALUE_MATERIALIZATION_CHANNEL_NEW
            : expected_receive_symbol         ? XR_C_VALUE_MATERIALIZATION_CHANNEL_RECV_PAYLOAD
            : expected_stringbuilder          ? XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_NEW
            : expected_string_byte_slice_view ? XR_C_VALUE_MATERIALIZATION_STRING_BYTE_SLICE_VIEW
            : expected_stringbuilder_append   ? XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_APPEND_RUNE
            : expected_iterator_rune_has_next ? XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_HAS_NEXT
            : expected_iterator_rune_next     ? XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_NEXT
            : expected_iterator_rune_nth      ? XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_NTH
            : expected_rune_to_uint32         ? XR_C_VALUE_MATERIALIZATION_RUNE_TO_UINT32
            : expected_rune_to_string         ? XR_C_VALUE_MATERIALIZATION_RUNE_TO_STRING
            : expected_string_slice_range     ? XR_C_VALUE_MATERIALIZATION_STRING_SLICE_RANGE
            : expected_rune_is_whitespace     ? XR_C_VALUE_MATERIALIZATION_RUNE_IS_WHITESPACE
            : expected_string_runes           ? XR_C_VALUE_MATERIALIZATION_STRING_RUNES
            : expected_stringbuilder_finish   ? XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_TO_STRING
            : expected_append_string ? XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_APPEND_STRING
            : expected_string_concat ? XR_C_VALUE_MATERIALIZATION_STRING_CONCAT
                                     : XR_C_VALUE_MATERIALIZATION_NONE;
        uint32_t expected_operand =
            expected_typed_rule                ? typed_location.receiver_value
            : expected_scalar_alias            ? expected_scalar_source
            : expected_local_address          ? expected_local_address_source
            : expected_adt_enum               ? expected_enum.receiver_value
            : expected_channel                ? expected_capacity
            : expected_receive_symbol         ? expected_receiver
            : expected_string_byte_slice_view ? expected_view_source
            : expected_stringbuilder_append   ? expected_append_receiver
            : expected_iterator_rune_has_next ? expected_iterator_rune_has_next_receiver
            : expected_iterator_rune_next     ? expected_iterator_rune_next_receiver
            : expected_iterator_rune_nth      ? expected_iterator_rune_nth_receiver
            : expected_rune_to_uint32         ? expected_rune_to_uint32_receiver
            : expected_rune_to_string         ? expected_rune_to_string_receiver
            : expected_string_slice_range     ? expected_string_slice_receiver
            : expected_rune_is_whitespace     ? expected_rune_is_whitespace_receiver
            : expected_string_runes           ? expected_string_runes_receiver
            : expected_stringbuilder_finish   ? expected_finish_receiver
            : expected_append_string          ? expected_append_string_receiver
                                              : UINT32_MAX;
        uint32_t expected_argument =
            expected_typed_rule ? typed_location.element_value
            : expected_stringbuilder_append ? expected_append_argument
            : expected_append_string        ? expected_append_string_argument
            : expected_iterator_rune_nth    ? expected_iterator_rune_nth_index
                                            : UINT32_MAX;
        const char *expected_symbol =
            expected_typed_rule               ? row->recipe_symbol
            : expected_adt_enum               ? XR_C_ADT_ENUM_CONSTRUCTOR_SYMBOL
            : expected_channel                ? XR_C_CHANNEL_NEW_SYMBOL
            : expected_receive_symbol         ? expected_receive_symbol
            : expected_stringbuilder          ? XR_C_STRINGBUILDER_NEW_SYMBOL
            : expected_string_byte_slice_view ? XR_C_STRING_BYTE_SLICE_VIEW_SYMBOL
            : expected_stringbuilder_append   ? XR_C_STRINGBUILDER_APPEND_RUNE_SYMBOL
            : expected_iterator_rune_has_next ? XR_C_ITERATOR_RUNE_HAS_NEXT_SYMBOL
            : expected_iterator_rune_next     ? XR_C_ITERATOR_RUNE_NEXT_SYMBOL
            : expected_iterator_rune_nth      ? XR_C_ITERATOR_RUNE_NTH_SYMBOL
            : expected_rune_to_uint32         ? XR_C_RUNE_TO_UINT32_SYMBOL
            : expected_rune_to_string         ? XR_C_RUNE_TO_STRING_SYMBOL
            : expected_string_slice_range     ? XR_C_STRING_SLICE_RANGE_SYMBOL
            : expected_rune_is_whitespace     ? XR_C_RUNE_IS_WHITESPACE_SYMBOL
            : expected_string_runes           ? XR_C_STRING_RUNES_SYMBOL
            : expected_stringbuilder_finish   ? XR_C_STRINGBUILDER_TO_STRING_SYMBOL
            : expected_append_string          ? XR_C_STRINGBUILDER_APPEND_STRING_SYMBOL
            : expected_string_concat          ? XR_C_STRING_CONCAT_SYMBOL
                                              : NULL;
        if (expected_array_fill_member) {
            expected_recipe = XR_C_VALUE_MATERIALIZATION_ARRAY_FILL_SCALAR;
            expected_operand = expected_array_fill_receiver;
            expected_argument = expected_array_fill_argument;
            expected_symbol = NULL;
        } else if (expected_array) {
            expected_recipe = expected_array_recipe;
            expected_operand = expected_array_count;
            expected_argument = expected_array_fill;
            expected_symbol = expected_array_symbol;
        } else if (expected_array_allocation) {
            expected_recipe = XR_C_VALUE_MATERIALIZATION_ARRAY_NEW;
            expected_operand = expected_array_allocation_count;
            expected_symbol = expected_array_allocation_symbol;
        } else if (expected_array_hof_direct) {
            expected_recipe = XR_C_VALUE_MATERIALIZATION_ARRAY_HOF_DIRECT;
            expected_operand = UINT32_MAX;
            expected_argument = UINT32_MAX;
            expected_symbol = NULL;
        }
        size_t expected_length = expected_literal ? strlen(expected_literal) : 0;
        if (row->materialization != expected_recipe || row->reserved != 0 ||
            row->recipe_rule_id !=
                (expected_typed_rule ? typed_actual.rule_id : XR_C_EMISSION_RULE_NONE) ||
            row->literal_byte_length != expected_length ||
            row->recipe_operand_value != expected_operand ||
            row->recipe_argument_value != expected_argument ||
            row->recipe_layout_id != (expected_adt_enum ? expected_enum.layout_id : 0) ||
            row->recipe_discriminant !=
                (expected_typed_rule                   ? typed_actual.storage
                 : expected_adt_enum                   ? expected_enum.member_ordinal
                 : expected_array_fill_member          ? expected_array_fill_member_storage
                 : expected_array                      ? expected_array_storage
                 : expected_array_allocation           ? expected_array_allocation_storage
                 : expected_direct_tagged_ref_parameter ? expected_direct_tagged_ref_storage
                                                       : 0) ||
            (expected_adt_enum
                 ? (!row->recipe_type_name ||
                    strcmp(row->recipe_type_name, expected_enum.enum_name) != 0 ||
                    !row->recipe_member_name ||
                    strcmp(row->recipe_member_name, expected_enum.member_name) != 0)
                 : row->recipe_type_name != NULL || row->recipe_member_name != NULL) ||
            (expected_symbol
                 ? (!row->recipe_symbol || strcmp(row->recipe_symbol, expected_symbol) != 0)
                 : row->recipe_symbol != NULL) ||
            (expected_literal ? (!row->literal_bytes || memcmp(row->literal_bytes, expected_literal,
                                                               expected_length + 1u) != 0)
                              : row->literal_bytes != NULL))
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission materialization recipe is not exact");
        if (expected_array_hof_direct) {
            uint16_t expected_count = expected_array_hof.kind == XR_C_ARRAY_HOF_REDUCE ? 3u : 2u;
            if (row->recipe_callee_function != expected_array_hof.callee_function ||
                row->recipe_hof_kind != expected_array_hof.kind ||
                row->recipe_hof_source_storage != expected_array_hof.source_storage ||
                row->recipe_hof_result_storage != expected_array_hof.result_storage ||
                row->recipe_hof_callback_parameter_reps[0] !=
                    expected_array_hof.parameter_reps[0] ||
                row->recipe_hof_callback_parameter_reps[1] !=
                    expected_array_hof.parameter_reps[1] ||
                row->recipe_hof_callback_return_rep != expected_array_hof.return_rep ||
                row->recipe_hof_reserved != 0 || row->recipe_argument_count != expected_count ||
                !row->recipe_arguments ||
                expected_count > plan->recipe_argument_count - projected_recipe_arguments ||
                row->recipe_arguments != &plan->recipe_arguments[projected_recipe_arguments])
                return emission_error(error, error_size, "XR_TARGET_1001",
                                      "C emission Array HOF recipe is not exact");
            const uint32_t expected_values[3] = {expected_array_hof.receiver_value,
                                                 expected_array_hof.callback_value,
                                                 expected_array_hof.seed_value};
            const uint8_t expected_kinds[3] = {XR_C_RECIPE_ARGUMENT_ARRAY_HOF_RECEIVER,
                                               XR_C_RECIPE_ARGUMENT_ARRAY_HOF_CALLBACK,
                                               XR_C_RECIPE_ARGUMENT_ARRAY_HOF_SEED};
            for (uint16_t argument = 0; argument < expected_count; argument++) {
                const XrCRecipeArgumentView *actual = &row->recipe_arguments[argument];
                if (actual->semantic_value != expected_values[argument] ||
                    actual->source_semantic_value != expected_values[argument] ||
                    actual->kind != expected_kinds[argument] || actual->reserved[0] != 0 ||
                    actual->reserved[1] != 0 || actual->reserved[2] != 0)
                    return emission_error(error, error_size, "XR_TARGET_1001",
                                          "C emission Array HOF argument is not exact");
            }
            projected_recipe_arguments += expected_count;
        } else if (row->recipe_callee_function != UINT32_MAX ||
                   row->recipe_hof_kind != XR_C_ARRAY_HOF_NONE ||
                   row->recipe_hof_source_storage != 0 || row->recipe_hof_result_storage != 0 ||
                   row->recipe_hof_callback_parameter_reps[0] != XR_C_VALUE_REP_VOID ||
                   row->recipe_hof_callback_parameter_reps[1] != XR_C_VALUE_REP_VOID ||
                   row->recipe_hof_callback_return_rep != XR_C_VALUE_REP_VOID ||
                   row->recipe_hof_reserved != 0) {
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission row has unexpected Array HOF authority");
        }
        if (expected_string_slice_range) {
            if (row->recipe_argument_count != 2 || !row->recipe_arguments ||
                2u > plan->recipe_argument_count - projected_recipe_arguments ||
                row->recipe_arguments != &plan->recipe_arguments[projected_recipe_arguments])
                return emission_error(error, error_size, "XR_TARGET_1001",
                                      "C emission String.slice bound partition is invalid");
            const uint32_t expected_bounds[2] = {expected_string_slice_start,
                                                 expected_string_slice_end};
            for (uint16_t argument = 0; argument < 2; argument++) {
                const XrCRecipeArgumentView *actual = &row->recipe_arguments[argument];
                if (actual->semantic_value != expected_bounds[argument] ||
                    actual->source_semantic_value != expected_bounds[argument] ||
                    !string_slice_bound_has_exact_projection(target_plan, actual->semantic_value) ||
                    actual->kind != XR_C_RECIPE_ARGUMENT_STRING_SLICE_BOUND ||
                    actual->reserved[0] != 0 || actual->reserved[1] != 0 ||
                    actual->reserved[2] != 0)
                    return emission_error(error, error_size, "XR_TARGET_1001",
                                          "C emission String.slice bound is not exact");
            }
            projected_recipe_arguments += 2;
        } else if (expected_adt_enum) {
            if (!expected_enum_payloads ||
                row->recipe_argument_count != expected_enum.payload_count ||
                !row->recipe_arguments ||
                expected_enum.payload_count >
                    plan->recipe_argument_count - projected_recipe_arguments ||
                row->recipe_arguments != &plan->recipe_arguments[projected_recipe_arguments])
                return emission_error(error, error_size, "XR_TARGET_1001",
                                      "C emission ADT enum payload partition is invalid");
            for (uint16_t argument = 0; argument < expected_enum.payload_count; argument++) {
                const XrCRecipeArgumentView *actual = &row->recipe_arguments[argument];
                if (actual->semantic_value != expected_enum_payloads[argument].value ||
                    actual->source_semantic_value != expected_enum_payloads[argument].value ||
                    !adt_enum_payload_has_exact_projection(target_plan, actual->semantic_value) ||
                    actual->kind != XR_C_RECIPE_ARGUMENT_ENUM_PAYLOAD || actual->reserved[0] != 0 ||
                    actual->reserved[1] != 0 || actual->reserved[2] != 0)
                    return emission_error(error, error_size, "XR_TARGET_1001",
                                          "C emission ADT enum payload is not exact");
            }
            projected_recipe_arguments += expected_enum.payload_count;
        } else if (expected_string_concat) {
            if (!expected_concat_arguments ||
                row->recipe_argument_count != expected_concat_argument_count ||
                !row->recipe_arguments ||
                expected_concat_argument_count >
                    plan->recipe_argument_count - projected_recipe_arguments ||
                row->recipe_arguments != &plan->recipe_arguments[projected_recipe_arguments])
                return emission_error(error, error_size, "XR_TARGET_1001",
                                      "C emission string concat argument partition is invalid");
            for (uint16_t argument = 0; argument < expected_concat_argument_count; argument++) {
                const XrCRecipeArgumentView *actual = &row->recipe_arguments[argument];
                uint8_t expected_kind = XR_C_RECIPE_ARGUMENT_INVALID;
                uint32_t expected_source_semantic_value = UINT32_MAX;
                if (!string_concat_argument_recipe(
                        target_plan, expected_concat_arguments[argument].value, &expected_kind,
                        &expected_source_semantic_value) ||
                    actual->semantic_value != expected_concat_arguments[argument].value ||
                    actual->source_semantic_value != expected_source_semantic_value ||
                    actual->kind != expected_kind || actual->reserved[0] != 0 ||
                    actual->reserved[1] != 0 || actual->reserved[2] != 0)
                    return emission_error(error, error_size, "XR_TARGET_1001",
                                          "C emission string concat argument is not exact");
            }
            projected_recipe_arguments += expected_concat_argument_count;
        } else if (expected_array_hof_direct) {
            /* The exact HOF partition was checked with its ABI fields above. */
        } else if (row->recipe_argument_count != 0 || row->recipe_arguments != NULL) {
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission row has unexpected recipe arguments");
        }
    }
    uint32_t target_argument_count = 0;
    const XrTargetCallArgumentRecord *target_arguments =
        xr_target_plan_call_arguments(target_plan, &target_argument_count);
    uint32_t projected_call_arguments = 0;
    for (uint32_t i = 0; i < target_argument_count; i++) {
        const XrTargetCallArgumentRecord *target_argument = &target_arguments[i];
        XrCCallArgumentEmissionView expected = {0};
        XrDirectLocalTaggedRefArgumentMatch match =
            classify_direct_local_tagged_ref_argument(target_plan, target_argument, &expected);
        if (match == XR_C_TAGGED_REF_ARGUMENT_NOT_THIS_FAMILY)
            continue;
        if (match != XR_C_TAGGED_REF_ARGUMENT_EXACT ||
            projected_call_arguments >= plan->call_argument_count)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission direct-local tagged ref argument is missing");
        const XrCCallArgumentEmissionView *actual = NULL;
        for (uint32_t a = 0; a < plan->call_argument_count; a++) {
            if (plan->call_arguments[a].semantic_call_value == expected.semantic_call_value &&
                plan->call_arguments[a].ordinal == expected.ordinal) {
                if (actual)
                    return emission_error(error, error_size, "XR_TARGET_1001",
                                          "C emission call argument identity is duplicated");
                actual = &plan->call_arguments[a];
            }
        }
        if (!actual || actual->semantic_operand != expected.semantic_operand ||
            actual->semantic_value != expected.semantic_value ||
            actual->callee_parameter != expected.callee_parameter ||
            actual->caller_register_kind != expected.caller_register_kind ||
            actual->caller_memory_kind != expected.caller_memory_kind ||
            actual->callee_register_kind != expected.callee_register_kind ||
            actual->callee_memory_kind != expected.callee_memory_kind ||
            actual->mode != expected.mode || actual->ownership != expected.ownership ||
            actual->transfer_mode != expected.transfer_mode || actual->flags != expected.flags ||
            actual->array_element_storage != expected.array_element_storage ||
            actual->reserved[0] != 0 || actual->reserved[1] != 0 || actual->reserved[2] != 0 ||
            !actual->c_type || strcmp(actual->c_type, expected.c_type) != 0)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission call argument disagrees with Target authority");
        projected_call_arguments++;
    }
    if (projected_call_arguments != plan->call_argument_count)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission call argument table has an extra row");
    uint32_t target_cleanup_count = 0;
    const XrTargetCleanupRecord *target_cleanups =
        xr_target_plan_cleanups(target_plan, &target_cleanup_count);
    uint32_t target_slot_count = 0;
    const XrTargetSlotRecord *target_slots = xr_target_plan_slots(target_plan, &target_slot_count);
    if (target_cleanup_count != plan->cleanup_count ||
        (target_cleanup_count && (!target_cleanups || !target_slots)))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C cleanup projection is incomplete");
    for (uint32_t i = 0; i < target_cleanup_count; i++) {
        const XrTargetCleanupRecord *target = &target_cleanups[i];
        const XrCCleanupEmissionView *actual = &plan->cleanups[i];
        const XrTargetSlotRecord *slot =
            target->slot < target_slot_count ? &target_slots[target->slot] : NULL;
        if (!slot || target->action != XR_TARGET_CLEANUP_RELEASE || target->flags != 0 ||
            target->provider != 0 || actual->semantic_operation != target->semantic_operation ||
            actual->semantic_value != slot->semantic_value || actual->target_slot != target->slot ||
            actual->action != XR_C_CLEANUP_RELEASE || actual->flags != 0 || actual->reserved != 0 ||
            !actual->recipe_symbol || strcmp(actual->recipe_symbol, XR_C_RELEASE_SYMBOL) != 0)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C cleanup projection disagrees with Target authority");
    }
    if (projected != plan->value_count)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission projection has an extra row");
    if (projected_recipe_arguments != plan->recipe_argument_count)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission recipe argument table has an extra row");
    if (!verify_plan(plan))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission fingerprint or canonical form is invalid");
    return true;
}

bool xr_c_emission_plan_build(const XrTargetPlan *target_plan,
                              XrFingerprint expected_profile_fingerprint, XrCEmissionPlan **out,
                              char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!target_plan || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan input is missing");
    if (!xr_target_plan_is_verified(target_plan))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan requires a verified TargetPlan");
    const uint64_t required_value_families =
        XR_TARGET_FAMILY_SCALAR | XR_TARGET_FAMILY_CLOSURE_STORAGE |
        XR_TARGET_FAMILY_STRING_LITERAL_STORAGE | XR_TARGET_FAMILY_STRING_RUNES_RESULT_STORAGE |
        XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE |
        XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE |
        XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE | XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE |
        XR_TARGET_FAMILY_SOURCE_IMPORT_STORAGE | XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE |
        XR_TARGET_FAMILY_STRINGBUILDER_APPEND_RUNE_STORAGE |
        XR_TARGET_FAMILY_STRINGBUILDER_TO_STRING_STORAGE |
        XR_TARGET_FAMILY_STRINGBUILDER_APPEND_STRING_STORAGE |
        XR_TARGET_FAMILY_NATIVE_MODULE_NAMESPACE_STORAGE |
        XR_TARGET_FAMILY_JSON_NAMESPACE_VALUE_STORAGE |
        XR_TARGET_FAMILY_DIRECT_LOCAL_STRING_BOUNDARY_STORAGE |
        XR_TARGET_FAMILY_ARRAY_ALLOCATION_STORAGE | XR_TARGET_FAMILY_ARRAY_INTRINSIC_STORAGE |
        XR_TARGET_FAMILY_DIRECT_LOCAL_TAGGED_REF_ARGUMENT_STORAGE |
        XR_TARGET_FAMILY_NULLABLE_SCALAR_STORAGE | XR_TARGET_FAMILY_ARRAY_MEMBER_RESULT_STORAGE |
        XR_TARGET_FAMILY_STRING_CONCAT_RESULT_STORAGE |
        XR_TARGET_FAMILY_DIRECT_LOCAL_GO_TASK_RESULT_STORAGE |
        XR_TARGET_FAMILY_PANIC_CATCH_STORAGE | XR_TARGET_FAMILY_ADT_ENUM_STORAGE |
        XR_TARGET_FAMILY_STRING_SLICE_RANGE_RESULT_STORAGE |
        XR_TARGET_FAMILY_RUNE_TO_STRING_RESULT_STORAGE |
        XR_TARGET_FAMILY_ARRAY_HOF_RESULT_STORAGE | XR_TARGET_FAMILY_AGGREGATE |
        XR_TARGET_FAMILY_CALL_ADAPTER;
    if ((xr_target_plan_completed_family_mask(target_plan) & required_value_families) !=
        required_value_families)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan requires completed value-storage families");
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    XrFingerprint actual_profile_fingerprint = xr_target_profile_fingerprint(profile);
    if (!profile || !xr_fingerprint_equal(actual_profile_fingerprint, expected_profile_fingerprint))
        return emission_error(error, error_size, "XR_TARGET_1000",
                              "C emission target profile fingerprint does not match");
    uint32_t target_value_count = 0;
    const XrTargetValueRepRecord *values =
        xr_target_plan_value_reps(target_plan, &target_value_count);
    if (target_value_count && !values)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "TargetPlan value-representation table is missing");
    uint32_t emission_value_count = 0;
    uint32_t target_call_argument_count = 0;
    const XrTargetCallArgumentRecord *target_call_arguments =
        xr_target_plan_call_arguments(target_plan, &target_call_argument_count);
    uint32_t target_cleanup_count = 0;
    const XrTargetCleanupRecord *target_cleanups =
        xr_target_plan_cleanups(target_plan, &target_cleanup_count);
    uint32_t target_slot_count = 0;
    const XrTargetSlotRecord *target_slots = xr_target_plan_slots(target_plan, &target_slot_count);
    if ((target_cleanup_count && (!target_cleanups || !target_slots)) ||
        target_cleanup_count > SIZE_MAX / sizeof(XrCCleanupEmissionView))
        return emission_error(error, error_size, "XR_EXEC_5003",
                              "C cleanup projection budget is invalid");
    uint32_t emission_call_argument_count = 0;
    uint32_t recipe_argument_count = 0;
    for (uint32_t i = 0; i < target_value_count; i++) {
        const XrTargetValueRepRecord *binding = &values[i];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        XrCValueRep register_c_rep = XR_C_VALUE_REP_COUNT;
        XrCValueRep memory_c_rep = XR_C_VALUE_REP_COUNT;
        const char *register_c_type = NULL;
        const char *memory_c_type = NULL;
        bool register_is_value =
            register_rep &&
            machine_kind_to_c_rep(target_plan, binding->semantic_value, register_rep->kind,
                                  &register_c_rep, &register_c_type);
        bool memory_is_value =
            memory_rep && machine_kind_to_c_rep(target_plan, binding->semantic_value,
                                                memory_rep->kind, &memory_c_rep, &memory_c_type);
        XrCAggregateProjection aggregate = {0};
        if (register_rep && memory_rep && register_rep->kind == XR_MACHINE_REP_AGGREGATE &&
            memory_rep->kind == XR_MACHINE_REP_AGGREGATE &&
            xr_c_aggregate_projection(target_plan, binding, &aggregate)) {
            register_c_rep = memory_c_rep = XR_C_VALUE_REP_AGGREGATE;
            register_c_type = memory_c_type = aggregate.c_type;
            register_is_value = memory_is_value = true;
        }
        if (!register_is_value && !memory_is_value)
            continue;
        if (!register_is_value || !memory_is_value || register_rep->kind != memory_rep->kind ||
            register_c_rep != memory_c_rep || strcmp(register_c_type, memory_c_type) != 0) {
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "TargetPlan value binding has no exact C projection");
        }
        uint8_t direct_tagged_ref_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        uint32_t local_address_source = UINT32_MAX;
        if (register_rep->kind == XR_MACHINE_REP_RAW_PTR &&
            !exact_direct_local_tagged_ref_parameter_recipe(target_plan, binding,
                                                            &direct_tagged_ref_storage) &&
            !exact_local_address_recipe(target_plan, binding, &local_address_source) &&
            !emission_source_ref_place_c_type(target_plan, binding->semantic_value) &&
            !emission_raw_pointer_c_type(target_plan, binding->semantic_value))
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "raw-pointer value has no exact C projection authority");
        const XrSemanticOperandRecord *concat_arguments = NULL;
        uint16_t concat_argument_count = 0;
        XrSemanticAdtEnumConstructorShape enum_shape = {0};
        const XrSemanticOperandRecord *enum_payloads = NULL;
        uint32_t string_slice_start = UINT32_MAX;
        uint32_t string_slice_end = UINT32_MAX;
        if (exact_string_slice_range_recipe(target_plan, binding, NULL, &string_slice_start,
                                            &string_slice_end)) {
            if (!string_slice_bound_has_exact_projection(target_plan, string_slice_start) ||
                !string_slice_bound_has_exact_projection(target_plan, string_slice_end))
                return emission_error(error, error_size, "XR_TARGET_1001",
                                      "String.slice bound has no exact C projection");
            if (recipe_argument_count > UINT32_MAX - 2u)
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "C emission recipe argument budget overflow");
            recipe_argument_count += 2u;
        }
        if (exact_adt_enum_constructor_recipe(target_plan, binding, &enum_shape, &enum_payloads)) {
            if (!enum_payloads || enum_shape.payload_count > UINT32_MAX - recipe_argument_count)
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "ADT enum recipe argument budget overflow");
            for (uint16_t argument = 0; argument < enum_shape.payload_count; argument++) {
                if (!adt_enum_payload_has_exact_projection(target_plan,
                                                           enum_payloads[argument].value))
                    return emission_error(error, error_size, "XR_TARGET_1001",
                                          "ADT enum payload has no exact C projection");
            }
            recipe_argument_count += enum_shape.payload_count;
        }
        if (exact_string_concat_recipe(target_plan, binding, &concat_arguments,
                                       &concat_argument_count)) {
            if (!concat_arguments || concat_argument_count > UINT32_MAX - recipe_argument_count)
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "C emission recipe argument budget overflow");
            for (uint16_t argument = 0; argument < concat_argument_count; argument++) {
                uint8_t ignored_kind = XR_C_RECIPE_ARGUMENT_INVALID;
                uint32_t ignored_semantic_value = UINT32_MAX;
                if (!string_concat_argument_recipe(target_plan, concat_arguments[argument].value,
                                                   &ignored_kind, &ignored_semantic_value))
                    return emission_error(error, error_size, "XR_TARGET_1001",
                                          "string concat argument has no exact C projection");
            }
            recipe_argument_count += concat_argument_count;
        }
        XrCArrayHofDirectRecipe array_hof = {0};
        if (exact_array_hof_direct_recipe(target_plan, binding, &array_hof)) {
            uint32_t argument_count = array_hof.kind == XR_C_ARRAY_HOF_REDUCE ? 3u : 2u;
            if (argument_count > UINT32_MAX - recipe_argument_count)
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "Array HOF recipe argument budget overflow");
            recipe_argument_count += argument_count;
        }
        emission_value_count++;
    }
    for (uint32_t i = 0; i < target_call_argument_count; i++) {
        const XrTargetCallArgumentRecord *argument = &target_call_arguments[i];
        XrCCallArgumentEmissionView projected = {0};
        XrDirectLocalTaggedRefArgumentMatch match =
            classify_direct_local_tagged_ref_argument(target_plan, argument, &projected);
        if (match == XR_C_TAGGED_REF_ARGUMENT_NOT_THIS_FAMILY)
            continue;
        if (match != XR_C_TAGGED_REF_ARGUMENT_EXACT)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "direct-local tagged ref argument has no exact C projection");
        emission_call_argument_count++;
    }
    if (emission_value_count > SIZE_MAX / sizeof(XrCValueEmissionView))
        return emission_error(error, error_size, "XR_EXEC_5003",
                              "C emission value record budget overflow");
    XrCEmissionPlan *plan = (XrCEmissionPlan *) xr_calloc(1, sizeof(*plan));
    if (!plan)
        return emission_error(error, error_size, "XR_EXEC_5003",
                              "C emission plan allocation failed");
    if (emission_value_count) {
        plan->values =
            (XrCValueEmissionView *) xr_calloc(emission_value_count, sizeof(*plan->values));
        if (!plan->values) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C emission value record allocation failed");
        }
    }
    if (emission_call_argument_count) {
        if (emission_call_argument_count > SIZE_MAX / sizeof(*plan->call_arguments)) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C call-argument record budget overflow");
        }
        plan->call_arguments = (XrCCallArgumentEmissionView *) xr_calloc(
            emission_call_argument_count, sizeof(*plan->call_arguments));
        if (!plan->call_arguments) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C call-argument plan allocation failed");
        }
    }
    if (recipe_argument_count) {
        if (recipe_argument_count > SIZE_MAX / sizeof(*plan->recipe_arguments)) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C emission recipe argument budget overflow");
        }
        plan->recipe_arguments = (XrCRecipeArgumentView *) xr_calloc(
            recipe_argument_count, sizeof(*plan->recipe_arguments));
        if (!plan->recipe_arguments) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C emission recipe argument allocation failed");
        }
    }
    if (target_cleanup_count) {
        plan->cleanups =
            (XrCCleanupEmissionView *) xr_calloc(target_cleanup_count, sizeof(*plan->cleanups));
        if (!plan->cleanups) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C cleanup projection allocation failed");
        }
    }
    plan->value_count = emission_value_count;
    plan->call_argument_count = emission_call_argument_count;
    plan->recipe_argument_count = recipe_argument_count;
    plan->cleanup_count = target_cleanup_count;
    plan->schema_version = XR_C_EMISSION_PLAN_SCHEMA_VERSION;
    plan->target_fingerprint = xr_target_plan_fingerprint(target_plan);
    plan->profile_fingerprint = actual_profile_fingerprint;
    uint32_t value_index = 0;
    uint32_t call_argument_index = 0;
    uint32_t recipe_argument_index = 0;
    for (uint32_t i = 0; i < target_value_count; i++) {
        const XrTargetValueRepRecord *binding = &values[i];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        XrCValueRep c_rep = XR_C_VALUE_REP_COUNT;
        const char *c_type = NULL;
        XrCAggregateProjection aggregate = {0};
        bool aggregate_supported = register_rep && memory_rep &&
                                   register_rep->kind == XR_MACHINE_REP_AGGREGATE &&
                                   memory_rep->kind == XR_MACHINE_REP_AGGREGATE &&
                                   xr_c_aggregate_projection(target_plan, binding, &aggregate);
        if (aggregate_supported) {
            c_rep = XR_C_VALUE_REP_AGGREGATE;
            c_type = aggregate.c_type;
        }
        if (!register_rep || !memory_rep || register_rep->kind != memory_rep->kind ||
            (!aggregate_supported && !machine_kind_to_c_rep(target_plan, binding->semantic_value,
                                                            register_rep->kind, &c_rep, &c_type)))
            continue;
        XrCValueEmissionView *value = &plan->values[value_index++];
        value->semantic_value = binding->semantic_value;
        value->target_register_rep = binding->register_rep;
        value->target_memory_rep = binding->memory_rep;
        value->target_register_kind = register_rep->kind;
        value->target_memory_kind = memory_rep->kind;
        value->register_bits = register_rep->register_bits;
        value->memory_align = memory_rep->memory_align;
        value->memory_size = memory_rep->memory_size;
        value->rep = (uint8_t) c_rep;
        value->recipe_operand_value = UINT32_MAX;
        value->recipe_argument_value = UINT32_MAX;
        value->recipe_callee_function = UINT32_MAX;
        value->c_type = aggregate_supported ? xr_strdup(c_type) : c_type;
        if (aggregate_supported) {
            value->address_projection = aggregate.kind;
            value->backing_value = aggregate.backing_value;
            value->backing_element_count = aggregate.element_count;
            value->backing_native_type = aggregate.element_native_type;
            value->backing_c_type =
                aggregate.element_c_type[0] ? xr_strdup(aggregate.element_c_type) : NULL;
        }
        if (!value->c_type || (aggregate.element_c_type[0] && !value->backing_c_type)) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "aggregate C type allocation failed");
        }
        XrCEmissionRuleLocation typed_location = {0};
        XrCEmissionRuleDecision typed_decision = {0};
        XrCEmissionRuleMatch typed_location_match =
            xr_c_emission_rule_locate(target_plan, binding, &typed_location);
        if (typed_location_match == XR_C_EMISSION_RULE_MALFORMED) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "typed C emission structural projection is malformed");
        }
        XrCEmissionRuleMatch typed_rule_match =
            typed_location_match == XR_C_EMISSION_RULE_EXACT
                ? xr_c_emission_rule_build(&typed_location.facts, &typed_decision)
                : XR_C_EMISSION_RULE_NOT_APPLICABLE;
        if (typed_rule_match == XR_C_EMISSION_RULE_MALFORMED) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "typed C emission rule rejected frozen target facts");
        }
        bool typed_rule = typed_rule_match == XR_C_EMISSION_RULE_EXACT;
        if (typed_rule &&
            (typed_decision.rep != value->rep ||
             typed_location.receiver_value == UINT32_MAX ||
             typed_location.element_value == UINT32_MAX)) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "typed C emission recipe result is inconsistent");
        }
        const char *literal = build_exact_string_literal(target_plan, binding);
        const XrSemanticOperandRecord *concat_arguments = NULL;
        uint16_t concat_argument_count = 0;
        bool string_concat = exact_string_concat_recipe(target_plan, binding, &concat_arguments,
                                                        &concat_argument_count);
        bool panic_catch = exact_panic_catch_recipe(target_plan, binding);
        XrSemanticAdtEnumConstructorShape enum_shape = {0};
        const XrSemanticOperandRecord *enum_payloads = NULL;
        bool adt_enum =
            exact_adt_enum_constructor_recipe(target_plan, binding, &enum_shape, &enum_payloads);
        uint8_t array_recipe = XR_C_VALUE_MATERIALIZATION_NONE;
        uint8_t array_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        uint32_t array_count = UINT32_MAX;
        uint32_t array_fill = UINT32_MAX;
        const char *array_symbol = NULL;
        bool array_intrinsic =
            exact_array_intrinsic_recipe(target_plan, binding, &array_recipe, &array_storage,
                                         &array_count, &array_fill, &array_symbol);
        uint32_t scalar_alias_source = UINT32_MAX;
        bool scalar_addressable_alias =
            exact_scalar_addressable_alias_recipe(target_plan, binding, &scalar_alias_source);
        uint8_t array_fill_member_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        uint32_t array_fill_receiver = UINT32_MAX;
        uint32_t array_fill_argument = UINT32_MAX;
        bool array_fill_member =
            exact_array_fill_scalar_recipe(target_plan, binding, &array_fill_member_storage,
                                           &array_fill_receiver, &array_fill_argument);
        uint8_t array_allocation_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        uint32_t array_allocation_count = UINT32_MAX;
        const char *array_allocation_symbol = NULL;
        bool array_allocation =
            exact_array_allocation_recipe(target_plan, binding, &array_allocation_storage,
                                          &array_allocation_count, &array_allocation_symbol);
        uint8_t direct_tagged_ref_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        bool direct_tagged_ref_parameter = exact_direct_local_tagged_ref_parameter_recipe(
            target_plan, binding, &direct_tagged_ref_storage);
        XrCArrayHofDirectRecipe array_hof = {0};
        bool array_hof_direct = exact_array_hof_direct_recipe(target_plan, binding, &array_hof);
        uint32_t string_slice_receiver = UINT32_MAX;
        uint32_t string_slice_start = UINT32_MAX;
        uint32_t string_slice_end = UINT32_MAX;
        bool string_slice_range = exact_string_slice_range_recipe(
            target_plan, binding, &string_slice_receiver, &string_slice_start, &string_slice_end);
        uint32_t local_address_source = UINT32_MAX;
        bool local_address =
            exact_local_address_recipe(target_plan, binding, &local_address_source);
        if (typed_rule) {
            value->recipe_symbol = xr_strdup(typed_decision.symbol);
            if (!value->recipe_symbol) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "typed C emission recipe allocation failed");
            }
            value->recipe_rule_id = typed_decision.rule_id;
            value->materialization = typed_decision.recipe;
            value->recipe_operand_value = typed_location.receiver_value;
            value->recipe_argument_value = typed_location.element_value;
            value->recipe_discriminant = typed_decision.storage;
        } else if (scalar_addressable_alias) {
            value->materialization = XR_C_VALUE_MATERIALIZATION_SCALAR_ADDRESSABLE_ALIAS;
            value->recipe_operand_value = scalar_alias_source;
        } else if (local_address) {
            value->materialization = XR_C_VALUE_MATERIALIZATION_LOCAL_ADDRESS;
            value->recipe_operand_value = local_address_source;
        } else if (direct_tagged_ref_parameter) {
            value->materialization = XR_C_VALUE_MATERIALIZATION_DIRECT_LOCAL_TAGGED_REF_PARAMETER;
            value->recipe_discriminant = direct_tagged_ref_storage;
        } else if (array_hof_direct) {
            uint16_t argument_count = array_hof.kind == XR_C_ARRAY_HOF_REDUCE ? 3u : 2u;
            if (argument_count > plan->recipe_argument_count - recipe_argument_index) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_TARGET_1001",
                                      "Array HOF recipe argument partition is invalid");
            }
            value->materialization = XR_C_VALUE_MATERIALIZATION_ARRAY_HOF_DIRECT;
            value->recipe_callee_function = array_hof.callee_function;
            value->recipe_hof_kind = array_hof.kind;
            value->recipe_hof_source_storage = array_hof.source_storage;
            value->recipe_hof_result_storage = array_hof.result_storage;
            value->recipe_hof_callback_parameter_reps[0] = array_hof.parameter_reps[0];
            value->recipe_hof_callback_parameter_reps[1] = array_hof.parameter_reps[1];
            value->recipe_hof_callback_return_rep = array_hof.return_rep;
            value->recipe_argument_count = argument_count;
            value->recipe_arguments = &plan->recipe_arguments[recipe_argument_index];
            const uint32_t semantic_values[3] = {array_hof.receiver_value, array_hof.callback_value,
                                                 array_hof.seed_value};
            const uint8_t kinds[3] = {XR_C_RECIPE_ARGUMENT_ARRAY_HOF_RECEIVER,
                                      XR_C_RECIPE_ARGUMENT_ARRAY_HOF_CALLBACK,
                                      XR_C_RECIPE_ARGUMENT_ARRAY_HOF_SEED};
            for (uint16_t argument = 0; argument < argument_count; argument++) {
                XrCRecipeArgumentView *recipe_argument =
                    &plan->recipe_arguments[recipe_argument_index++];
                recipe_argument->semantic_value = semantic_values[argument];
                recipe_argument->source_semantic_value = semantic_values[argument];
                recipe_argument->kind = kinds[argument];
            }
        } else if (string_slice_range) {
            if (2u > plan->recipe_argument_count - recipe_argument_index) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_TARGET_1001",
                                      "String.slice recipe argument partition is invalid");
            }
            value->recipe_symbol = xr_strdup(XR_C_STRING_SLICE_RANGE_SYMBOL);
            if (!value->recipe_symbol) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "String.slice recipe allocation failed");
            }
            value->materialization = XR_C_VALUE_MATERIALIZATION_STRING_SLICE_RANGE;
            value->recipe_operand_value = string_slice_receiver;
            value->recipe_argument_count = 2;
            value->recipe_arguments = &plan->recipe_arguments[recipe_argument_index];
            const uint32_t bounds[2] = {string_slice_start, string_slice_end};
            for (uint16_t argument = 0; argument < 2; argument++) {
                XrCRecipeArgumentView *recipe_argument =
                    &plan->recipe_arguments[recipe_argument_index++];
                recipe_argument->semantic_value = bounds[argument];
                recipe_argument->source_semantic_value = bounds[argument];
                recipe_argument->kind = XR_C_RECIPE_ARGUMENT_STRING_SLICE_BOUND;
            }
        } else if (array_fill_member) {
            value->materialization = XR_C_VALUE_MATERIALIZATION_ARRAY_FILL_SCALAR;
            value->recipe_operand_value = array_fill_receiver;
            value->recipe_argument_value = array_fill_argument;
            value->recipe_discriminant = array_fill_member_storage;
        } else if (array_intrinsic) {
            value->recipe_symbol = xr_strdup(array_symbol);
            if (!value->recipe_symbol) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "Array intrinsic recipe allocation failed");
            }
            value->materialization = array_recipe;
            value->recipe_operand_value = array_count;
            value->recipe_argument_value = array_fill;
            value->recipe_discriminant = array_storage;
        } else if (array_allocation) {
            value->recipe_symbol = xr_strdup(array_allocation_symbol);
            if (!value->recipe_symbol) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "Array allocation recipe allocation failed");
            }
            value->materialization = XR_C_VALUE_MATERIALIZATION_ARRAY_NEW;
            value->recipe_operand_value = array_allocation_count;
            value->recipe_discriminant = array_allocation_storage;
        } else if (adt_enum) {
            if (!enum_payloads ||
                enum_shape.payload_count > plan->recipe_argument_count - recipe_argument_index) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_TARGET_1001",
                                      "ADT enum recipe argument partition is invalid");
            }
            value->recipe_symbol = xr_strdup(XR_C_ADT_ENUM_CONSTRUCTOR_SYMBOL);
            value->recipe_type_name = xr_strdup(enum_shape.enum_name);
            value->recipe_member_name = xr_strdup(enum_shape.member_name);
            if (!value->recipe_symbol || !value->recipe_type_name || !value->recipe_member_name) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "ADT enum recipe allocation failed");
            }
            value->materialization = XR_C_VALUE_MATERIALIZATION_ADT_ENUM_CONSTRUCTOR;
            value->recipe_operand_value = enum_shape.receiver_value;
            value->recipe_layout_id = enum_shape.layout_id;
            value->recipe_discriminant = enum_shape.member_ordinal;
            value->recipe_argument_count = enum_shape.payload_count;
            value->recipe_arguments = &plan->recipe_arguments[recipe_argument_index];
            for (uint16_t argument = 0; argument < enum_shape.payload_count; argument++) {
                XrCRecipeArgumentView *recipe_argument =
                    &plan->recipe_arguments[recipe_argument_index++];
                recipe_argument->semantic_value = enum_payloads[argument].value;
                recipe_argument->source_semantic_value = enum_payloads[argument].value;
                recipe_argument->kind = XR_C_RECIPE_ARGUMENT_ENUM_PAYLOAD;
            }
        } else if (panic_catch) {
            value->materialization = XR_C_VALUE_MATERIALIZATION_PANIC_CATCH;
        } else if (string_concat) {
            size_t symbol_length = sizeof(XR_C_STRING_CONCAT_SYMBOL);
            char *owned = (char *) xr_malloc(symbol_length);
            if (!owned) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "string concat recipe allocation failed");
            }
            if (!concat_arguments ||
                concat_argument_count > plan->recipe_argument_count - recipe_argument_index) {
                xr_free(owned);
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_TARGET_1001",
                                      "string concat recipe argument partition is invalid");
            }
            memcpy(owned, XR_C_STRING_CONCAT_SYMBOL, symbol_length);
            value->materialization = XR_C_VALUE_MATERIALIZATION_STRING_CONCAT;
            value->recipe_argument_count = concat_argument_count;
            value->recipe_arguments = &plan->recipe_arguments[recipe_argument_index];
            value->recipe_symbol = owned;
            for (uint16_t argument = 0; argument < concat_argument_count; argument++) {
                XrCRecipeArgumentView *recipe_argument =
                    &plan->recipe_arguments[recipe_argument_index++];
                recipe_argument->semantic_value = concat_arguments[argument].value;
                if (!string_concat_argument_recipe(target_plan, concat_arguments[argument].value,
                                                   &recipe_argument->kind,
                                                   &recipe_argument->source_semantic_value)) {
                    xr_c_emission_plan_free(plan);
                    return emission_error(error, error_size, "XR_TARGET_1001",
                                          "string concat argument recipe is not exact");
                }
            }
        } else if (literal) {
            size_t length = strlen(literal);
            if (length > UINT32_MAX) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "String literal exceeds C emission budget");
            }
            char *owned = (char *) xr_malloc(length + 1u);
            if (!owned) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "String literal C emission allocation failed");
            }
            memcpy(owned, literal, length + 1u);
            value->materialization = XR_C_VALUE_MATERIALIZATION_STRING_LITERAL_VIEW;
            value->literal_byte_length = (uint32_t) length;
            value->literal_bytes = owned;
        } else {
            uint32_t capacity = UINT32_MAX;
            if (exact_channel_new_recipe(target_plan, binding, &capacity)) {
                size_t symbol_length = sizeof(XR_C_CHANNEL_NEW_SYMBOL);
                char *owned = (char *) xr_malloc(symbol_length);
                if (!owned) {
                    xr_c_emission_plan_free(plan);
                    return emission_error(error, error_size, "XR_EXEC_5003",
                                          "channel recipe symbol allocation failed");
                }
                memcpy(owned, XR_C_CHANNEL_NEW_SYMBOL, symbol_length);
                value->materialization = XR_C_VALUE_MATERIALIZATION_CHANNEL_NEW;
                value->recipe_operand_value = capacity;
                value->recipe_symbol = owned;
            } else {
                uint32_t receiver = UINT32_MAX;
                const char *symbol = exact_channel_receive_recipe(target_plan, binding, &receiver);
                if (symbol) {
                    size_t symbol_length = strlen(symbol) + 1u;
                    char *owned = (char *) xr_malloc(symbol_length);
                    if (!owned) {
                        xr_c_emission_plan_free(plan);
                        return emission_error(error, error_size, "XR_EXEC_5003",
                                              "channel receive recipe allocation failed");
                    }
                    memcpy(owned, symbol, symbol_length);
                    value->materialization = XR_C_VALUE_MATERIALIZATION_CHANNEL_RECV_PAYLOAD;
                    value->recipe_operand_value = receiver;
                    value->recipe_symbol = owned;
                } else if (exact_stringbuilder_new_recipe(target_plan, binding)) {
                    size_t symbol_length = sizeof(XR_C_STRINGBUILDER_NEW_SYMBOL);
                    char *owned = (char *) xr_malloc(symbol_length);
                    if (!owned) {
                        xr_c_emission_plan_free(plan);
                        return emission_error(error, error_size, "XR_EXEC_5003",
                                              "StringBuilder recipe symbol allocation failed");
                    }
                    memcpy(owned, XR_C_STRINGBUILDER_NEW_SYMBOL, symbol_length);
                    value->materialization = XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_NEW;
                    value->recipe_symbol = owned;
                } else {
                    uint32_t source_value = UINT32_MAX;
                    if (build_exact_string_byte_slice_view_recipe(target_plan, binding,
                                                                  &source_value)) {
                        size_t symbol_length = sizeof(XR_C_STRING_BYTE_SLICE_VIEW_SYMBOL);
                        char *owned = (char *) xr_malloc(symbol_length);
                        if (!owned) {
                            xr_c_emission_plan_free(plan);
                            return emission_error(
                                error, error_size, "XR_EXEC_5003",
                                "string byte-slice view recipe symbol allocation failed");
                        }
                        memcpy(owned, XR_C_STRING_BYTE_SLICE_VIEW_SYMBOL, symbol_length);
                        value->materialization = XR_C_VALUE_MATERIALIZATION_STRING_BYTE_SLICE_VIEW;
                        value->recipe_operand_value = source_value;
                        value->recipe_symbol = owned;
                    } else {
                        uint32_t receiver = UINT32_MAX;
                        uint32_t argument = UINT32_MAX;
                        if (exact_stringbuilder_append_rune_recipe(target_plan, binding, &receiver,
                                                                   &argument)) {
                            size_t symbol_length = sizeof(XR_C_STRINGBUILDER_APPEND_RUNE_SYMBOL);
                            char *owned = (char *) xr_malloc(symbol_length);
                            if (!owned) {
                                xr_c_emission_plan_free(plan);
                                return emission_error(
                                    error, error_size, "XR_EXEC_5003",
                                    "StringBuilder append recipe allocation failed");
                            }
                            memcpy(owned, XR_C_STRINGBUILDER_APPEND_RUNE_SYMBOL, symbol_length);
                            value->materialization =
                                XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_APPEND_RUNE;
                            value->recipe_operand_value = receiver;
                            value->recipe_argument_value = argument;
                            value->recipe_symbol = owned;
                        }
                        if (value->materialization == XR_C_VALUE_MATERIALIZATION_NONE) {
                            uint32_t receiver = UINT32_MAX;
                            if (exact_string_runes_recipe(target_plan, binding, &receiver)) {
                                size_t symbol_length = sizeof(XR_C_STRING_RUNES_SYMBOL);
                                char *owned = (char *) xr_malloc(symbol_length);
                                if (!owned) {
                                    xr_c_emission_plan_free(plan);
                                    return emission_error(error, error_size, "XR_EXEC_5003",
                                                          "String.runes recipe allocation failed");
                                }
                                memcpy(owned, XR_C_STRING_RUNES_SYMBOL, symbol_length);
                                value->materialization = XR_C_VALUE_MATERIALIZATION_STRING_RUNES;
                                value->recipe_operand_value = receiver;
                                value->recipe_symbol = owned;
                            }
                        }
                        if (value->materialization == XR_C_VALUE_MATERIALIZATION_NONE) {
                            uint32_t receiver = UINT32_MAX;
                            if (exact_iterator_rune_has_next_recipe(target_plan, binding,
                                                                    &receiver)) {
                                size_t symbol_length = sizeof(XR_C_ITERATOR_RUNE_HAS_NEXT_SYMBOL);
                                char *owned = (char *) xr_malloc(symbol_length);
                                if (!owned) {
                                    xr_c_emission_plan_free(plan);
                                    return emission_error(
                                        error, error_size, "XR_EXEC_5003",
                                        "Iterator<rune>.hasNext recipe allocation failed");
                                }
                                memcpy(owned, XR_C_ITERATOR_RUNE_HAS_NEXT_SYMBOL, symbol_length);
                                value->materialization =
                                    XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_HAS_NEXT;
                                value->recipe_operand_value = receiver;
                                value->recipe_symbol = owned;
                            }
                        }
                        if (value->materialization == XR_C_VALUE_MATERIALIZATION_NONE) {
                            uint32_t receiver = UINT32_MAX;
                            if (exact_iterator_rune_next_recipe(target_plan, binding, &receiver)) {
                                size_t symbol_length = sizeof(XR_C_ITERATOR_RUNE_NEXT_SYMBOL);
                                char *owned = (char *) xr_malloc(symbol_length);
                                if (!owned) {
                                    xr_c_emission_plan_free(plan);
                                    return emission_error(
                                        error, error_size, "XR_EXEC_5003",
                                        "Iterator<rune>.next recipe allocation failed");
                                }
                                memcpy(owned, XR_C_ITERATOR_RUNE_NEXT_SYMBOL, symbol_length);
                                value->materialization =
                                    XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_NEXT;
                                value->recipe_operand_value = receiver;
                                value->recipe_symbol = owned;
                            }
                        }
                        if (value->materialization == XR_C_VALUE_MATERIALIZATION_NONE) {
                            uint32_t receiver = UINT32_MAX;
                            uint32_t index = UINT32_MAX;
                            if (exact_iterator_rune_nth_recipe(target_plan, binding, &receiver,
                                                               &index)) {
                                size_t symbol_length = sizeof(XR_C_ITERATOR_RUNE_NTH_SYMBOL);
                                char *owned = (char *) xr_malloc(symbol_length);
                                if (!owned) {
                                    xr_c_emission_plan_free(plan);
                                    return emission_error(
                                        error, error_size, "XR_EXEC_5003",
                                        "Iterator<rune>.nth recipe allocation failed");
                                }
                                memcpy(owned, XR_C_ITERATOR_RUNE_NTH_SYMBOL, symbol_length);
                                value->materialization =
                                    XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_NTH;
                                value->recipe_operand_value = receiver;
                                value->recipe_argument_value = index;
                                value->recipe_symbol = owned;
                            }
                        }
                        if (value->materialization == XR_C_VALUE_MATERIALIZATION_NONE) {
                            uint32_t receiver = UINT32_MAX;
                            if (exact_rune_to_uint32_recipe(target_plan, binding, &receiver)) {
                                size_t symbol_length = sizeof(XR_C_RUNE_TO_UINT32_SYMBOL);
                                char *owned = (char *) xr_malloc(symbol_length);
                                if (!owned) {
                                    xr_c_emission_plan_free(plan);
                                    return emission_error(error, error_size, "XR_EXEC_5003",
                                                          "rune.toUInt32 recipe allocation failed");
                                }
                                memcpy(owned, XR_C_RUNE_TO_UINT32_SYMBOL, symbol_length);
                                value->materialization = XR_C_VALUE_MATERIALIZATION_RUNE_TO_UINT32;
                                value->recipe_operand_value = receiver;
                                value->recipe_symbol = owned;
                            }
                        }
                        if (value->materialization == XR_C_VALUE_MATERIALIZATION_NONE) {
                            uint32_t receiver = UINT32_MAX;
                            if (exact_rune_to_string_recipe(target_plan, binding, &receiver)) {
                                size_t symbol_length = sizeof(XR_C_RUNE_TO_STRING_SYMBOL);
                                char *owned = (char *) xr_malloc(symbol_length);
                                if (!owned) {
                                    xr_c_emission_plan_free(plan);
                                    return emission_error(error, error_size, "XR_EXEC_5003",
                                                          "rune.toString recipe allocation failed");
                                }
                                memcpy(owned, XR_C_RUNE_TO_STRING_SYMBOL, symbol_length);
                                value->materialization = XR_C_VALUE_MATERIALIZATION_RUNE_TO_STRING;
                                value->recipe_operand_value = receiver;
                                value->recipe_symbol = owned;
                            }
                        }
                        if (value->materialization == XR_C_VALUE_MATERIALIZATION_NONE) {
                            uint32_t receiver = UINT32_MAX;
                            if (exact_rune_is_whitespace_recipe(target_plan, binding, &receiver)) {
                                size_t symbol_length = sizeof(XR_C_RUNE_IS_WHITESPACE_SYMBOL);
                                char *owned = (char *) xr_malloc(symbol_length);
                                if (!owned) {
                                    xr_c_emission_plan_free(plan);
                                    return emission_error(
                                        error, error_size, "XR_EXEC_5003",
                                        "rune.isWhitespace recipe allocation failed");
                                }
                                memcpy(owned, XR_C_RUNE_IS_WHITESPACE_SYMBOL, symbol_length);
                                value->materialization =
                                    XR_C_VALUE_MATERIALIZATION_RUNE_IS_WHITESPACE;
                                value->recipe_operand_value = receiver;
                                value->recipe_symbol = owned;
                            }
                        }
                        if (value->materialization == XR_C_VALUE_MATERIALIZATION_NONE) {
                            uint32_t receiver = UINT32_MAX;
                            if (exact_stringbuilder_to_string_recipe(target_plan, binding,
                                                                     &receiver)) {
                                size_t symbol_length = sizeof(XR_C_STRINGBUILDER_TO_STRING_SYMBOL);
                                char *owned = (char *) xr_malloc(symbol_length);
                                if (!owned) {
                                    xr_c_emission_plan_free(plan);
                                    return emission_error(
                                        error, error_size, "XR_EXEC_5003",
                                        "StringBuilder finish recipe allocation failed");
                                }
                                memcpy(owned, XR_C_STRINGBUILDER_TO_STRING_SYMBOL, symbol_length);
                                value->materialization =
                                    XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_TO_STRING;
                                value->recipe_operand_value = receiver;
                                value->recipe_symbol = owned;
                            }
                        }
                        if (value->materialization == XR_C_VALUE_MATERIALIZATION_NONE) {
                            uint32_t receiver = UINT32_MAX, argument = UINT32_MAX;
                            if (exact_stringbuilder_append_string_recipe(target_plan, binding,
                                                                         &receiver, &argument)) {
                                size_t n = sizeof(XR_C_STRINGBUILDER_APPEND_STRING_SYMBOL);
                                char *owned = (char *) xr_malloc(n);
                                if (!owned) {
                                    xr_c_emission_plan_free(plan);
                                    return emission_error(
                                        error, error_size, "XR_EXEC_5003",
                                        "StringBuilder string append recipe allocation failed");
                                }
                                memcpy(owned, XR_C_STRINGBUILDER_APPEND_STRING_SYMBOL, n);
                                value->materialization =
                                    XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_APPEND_STRING;
                                value->recipe_operand_value = receiver;
                                value->recipe_argument_value = argument;
                                value->recipe_symbol = owned;
                            }
                        }
                    }
                }
            }
        }
    }
    for (uint32_t i = 0; i < target_cleanup_count; i++) {
        const XrTargetCleanupRecord *cleanup = &target_cleanups[i];
        const XrTargetSlotRecord *slot =
            cleanup->slot < target_slot_count ? &target_slots[cleanup->slot] : NULL;
        XrCCleanupEmissionView *view = &plan->cleanups[i];
        if (!slot || cleanup->id != i || cleanup->action != XR_TARGET_CLEANUP_RELEASE ||
            cleanup->flags != 0 || cleanup->provider != 0) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "Target cleanup has no exact C projection");
        }
        view->semantic_operation = cleanup->semantic_operation;
        view->semantic_value = slot->semantic_value;
        view->target_slot = cleanup->slot;
        view->action = XR_C_CLEANUP_RELEASE;
        view->recipe_symbol = xr_strdup(XR_C_RELEASE_SYMBOL);
        if (!view->recipe_symbol) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C cleanup recipe allocation failed");
        }
    }
    for (uint32_t i = 0; i < target_call_argument_count; i++) {
        XrCCallArgumentEmissionView projected = {0};
        XrDirectLocalTaggedRefArgumentMatch match = classify_direct_local_tagged_ref_argument(
            target_plan, &target_call_arguments[i], &projected);
        if (match == XR_C_TAGGED_REF_ARGUMENT_NOT_THIS_FAMILY)
            continue;
        if (match != XR_C_TAGGED_REF_ARGUMENT_EXACT ||
            call_argument_index >= emission_call_argument_count) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C call-argument projection is not exact");
        }
        plan->call_arguments[call_argument_index++] = projected;
    }
    if (emission_call_argument_count > 1)
        qsort(plan->call_arguments, emission_call_argument_count, sizeof(*plan->call_arguments),
              compare_c_call_argument_view);
    if (value_index != emission_value_count) {
        xr_c_emission_plan_free(plan);
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission value partition is not exact");
    }
    if (recipe_argument_index != recipe_argument_count) {
        xr_c_emission_plan_free(plan);
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission recipe argument partition is not exact");
    }
    if (call_argument_index != emission_call_argument_count) {
        xr_c_emission_plan_free(plan);
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C call-argument partition is not exact");
    }
    /* One slot row per signature position, assembled from the frozen semantic
     * parameter list and the target representation each of those subjects
     * already carries. A function whose return or a parameter has no bound
     * representation contributes no rows at all rather than a partial
     * signature: a caller must not be able to read half a boundary. */
    {
        const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
        uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(semantic);
        uint64_t slot_budget = 0;
        for (uint32_t f = 0; f < function_count; f++) {
            const XrSemanticFunctionRecord *function = xr_semantic_plan_function(semantic, f);
            slot_budget += function ? (uint64_t) function->parameter_count + 1u : 0u;
        }
        if (slot_budget > 4000000u) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C function-ABI partition budget exhausted");
        }
        if (slot_budget) {
            plan->function_abis = (XrCFunctionAbiEmissionView *) xr_calloc(
                (size_t) slot_budget, sizeof(*plan->function_abis));
            if (!plan->function_abis) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "C function-ABI partition allocation failed");
            }
        }
        uint32_t abi_index = 0;
        for (uint32_t f = 0; f < function_count; f++) {
            const XrSemanticFunctionRecord *function = xr_semantic_plan_function(semantic, f);
            if (!function)
                continue;
            uint32_t written = abi_index;
            bool complete = true;
            /* One answer for the whole signature, settled after the rows are
             * written because it reads one of them. Three facts belong to the
             * function itself -- a module initializer, a capturing function and
             * a coroutine each take the tagged boundary however their slots are
             * shaped -- and the fourth is the return row: a return that reaches
             * C tagged makes the whole boundary tagged. */
            uint32_t signature_begin = abi_index;
            for (uint16_t ordinal = 0; complete && ordinal <= function->parameter_count;
                 ordinal++) {
                uint32_t subject = XR_SEMANTIC_INDEX_NONE;
                uint32_t declared_type = XR_SEMANTIC_INDEX_NONE;
                uint8_t slot_class = XR_C_ABI_SLOT_VALUE;
                if (ordinal > 0) {
                    const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(
                        semantic, function->parameter_begin + (uint32_t) (ordinal - 1u));
                    if (!parameter || parameter->function != f) {
                        complete = false;
                        break;
                    }
                    subject = parameter->value;
                    declared_type = parameter->type;
                    slot_class = parameter->mode == XR_PARAM_REF ? XR_C_ABI_SLOT_BORROWED_PLACE
                                                                 : XR_C_ABI_SLOT_VALUE;
                }
                const XrTargetValueRepRecord *binding =
                    subject == XR_SEMANTIC_INDEX_NONE
                        ? NULL
                        : xr_target_plan_value_rep(target_plan, subject);
                const XrTargetMachineRepRecord *register_rep =
                    binding ? xr_target_plan_machine_rep(target_plan, binding->register_rep) : NULL;
                const XrTargetMachineRepRecord *memory_rep =
                    binding ? xr_target_plan_machine_rep(target_plan, binding->memory_rep) : NULL;
                XrCValueRep rep = XR_C_VALUE_REP_VOID;
                const char *c_type = NULL;
                XrCValueRep pointee_rep = XR_C_VALUE_REP_VOID;
                const char *pointee_c_type = NULL;
                if (ordinal == 0) {
                    /* A callee's return is not a value inside the callee -- the
                     * target plan freezes a representation per value, and the
                     * result of a call is a value in each caller. The call rows
                     * carry that result representation next to the callee they
                     * name, so the return is read from the calls that reach this
                     * function rather than re-derived from its type.
                     *
                     * Callers that disagree, and a function no call in this plan
                     * reaches, leave the signature unwritten: a boundary two
                     * sides describe differently is not one this plan can state,
                     * and neither is one nothing pins down. A unit return needs
                     * no representation and is stated directly. */
                    const XrSemanticTypeRecord *return_type =
                        xr_semantic_plan_type(semantic, function->return_type);
                    /* A unit return still crosses whatever boundary its calls
                     * carry: a function on a tagged convention hands back the
                     * carrier even when the source says nothing, so the unit
                     * case reads the call rows like every other slot and only
                     * falls back to void when no call names a representation. */
                    if (function->is_module_initializer) {
                        /* The runtime enters an initializer, so no call site
                         * witnesses its boundary. The function itself says what
                         * that boundary is: an initializer is entered on the
                         * dynamic convention and hands the carrier back, which
                         * is a fact about the function rather than one inferred
                         * from the callers it does not have. */
                        rep = XR_C_VALUE_REP_TAGGED;
                        c_type = "XrValue";
                    } else if (return_type && return_type->kind == XR_KIND_UNIT &&
                               target_plan_has_call_result_rep(target_plan, f)) {
                        rep = XR_C_VALUE_REP_VOID;
                        c_type = "void";
                    } else {
                        uint32_t call_count = 0;
                        const XrTargetCallRecord *calls =
                            xr_target_plan_calls(target_plan, &call_count);
                        const XrTargetCallRecord *agreed = NULL;
                        for (uint32_t c = 0; calls && c < call_count; c++) {
                            if (calls[c].callee_function != f)
                                continue;
                            if (agreed &&
                                (agreed->result_register_rep != calls[c].result_register_rep ||
                                 agreed->result_memory_rep != calls[c].result_memory_rep)) {
                                agreed = NULL;
                                break;
                            }
                            agreed = &calls[c];
                        }
                        const XrTargetMachineRepRecord *return_rep =
                            agreed ? xr_target_plan_machine_rep(target_plan,
                                                                agreed->result_register_rep)
                                   : NULL;
                        if (!agreed && function_states_own_boundary(function) &&
                            declared_scalar_c_rep(
                                xr_semantic_plan_type(semantic, function->return_type), &rep,
                                &c_type)) {
                            /* Stated by the function, not inferred from callers
                             * it does not have. */
                        } else if (!return_rep ||
                                   !machine_kind_to_c_rep(target_plan, agreed->result_value,
                                                          return_rep->kind, &rep, &c_type) ||
                                   !c_type) {
                            complete = false;
                            break;
                        }
                    }
                } else {
                    /* A parameter's ABI representation is not the one its value
                     * carries inside the body: how a subject is held and how it
                     * crosses a boundary are two facts, and a String is held
                     * tagged while it is passed as a pointer. The boundary fact
                     * lives on the call-argument rows, beside the callee slot
                     * they name, so parameters are read from the calls that
                     * reach this function for the same reason the return is.
                     * Disagreeing callers leave the signature unwritten. */
                    uint32_t call_count = 0;
                    uint32_t argument_count = 0;
                    const XrTargetCallRecord *calls =
                        xr_target_plan_calls(target_plan, &call_count);
                    const XrTargetCallArgumentRecord *arguments =
                        xr_target_plan_call_arguments(target_plan, &argument_count);
                    const XrTargetCallArgumentRecord *agreed = NULL;
                    for (uint32_t a = 0; calls && arguments && a < argument_count; a++) {
                        if (arguments[a].ordinal != (uint16_t) (ordinal - 1u) ||
                            arguments[a].call >= call_count ||
                            calls[arguments[a].call].callee_function != f)
                            continue;
                        if (agreed &&
                            (agreed->callee_register_rep != arguments[a].callee_register_rep ||
                             agreed->callee_memory_rep != arguments[a].callee_memory_rep)) {
                            agreed = NULL;
                            break;
                        }
                        agreed = &arguments[a];
                    }
                    const XrTargetMachineRepRecord *boundary =
                        agreed
                            ? xr_target_plan_machine_rep(target_plan, agreed->callee_register_rep)
                            : NULL;
                    if (!agreed && slot_class == XR_C_ABI_SLOT_VALUE &&
                        function_states_own_boundary(function) &&
                        declared_scalar_c_rep(xr_semantic_plan_type(semantic, declared_type), &rep,
                                              &c_type)) {
                        /* Same reason as the return row. A borrowed place is
                         * excluded: how a place crosses is a property of the
                         * call, never of the declared type. */
                    } else if (!boundary ||
                               !machine_kind_to_c_rep(target_plan, subject, boundary->kind, &rep,
                                                      &c_type) ||
                               !c_type) {
                        complete = false;
                        break;
                    } else {
                        register_rep = boundary;
                        memory_rep =
                            xr_target_plan_machine_rep(target_plan, agreed->callee_memory_rep);
                        /* A place crosses as a pointer, so the callee side of the row
                         * describes the pointer and cannot say what is pointed at.
                         * The caller side can: the caller holds the thing and passes
                         * its address, so its representation IS the pointee's. The
                         * parameter's own value representation is not a substitute --
                         * inside the callee a `ref` parameter is held as the pointer
                         * too, so it answers the same thing the callee side does. */
                        if (slot_class == XR_C_ABI_SLOT_BORROWED_PLACE) {
                            const XrTargetMachineRepRecord *pointee =
                                xr_target_plan_machine_rep(target_plan, agreed->register_rep);
                            XrCValueRep held_rep = XR_C_VALUE_REP_VOID;
                            const char *held_c_type = NULL;
                            if (pointee &&
                                machine_kind_to_c_rep(target_plan, agreed->semantic_value,
                                                      pointee->kind, &held_rep, &held_c_type) &&
                                held_c_type) {
                                pointee_rep = held_rep;
                                pointee_c_type = held_c_type;
                            }
                        }
                    }
                }
                /* Which aggregate this is, read from the frozen semantic type
                 * rather than recognised from the C spelling later. */
                uint8_t aggregate_class = XR_C_ABI_AGGREGATE_NONE;
                if (rep == XR_C_VALUE_REP_AGGREGATE || rep == XR_C_VALUE_REP_VIEW) {
                    const XrSemanticTypeRecord *slot_type = xr_semantic_plan_type(
                        semantic, ordinal == 0 ? function->return_type
                                               : xr_semantic_plan_parameter(
                                                     semantic, function->parameter_begin +
                                                                   (uint32_t) (ordinal - 1u))
                                                     ->type);
                    if (slot_type) {
                        if (slot_type->kind == XR_KIND_SLICE)
                            aggregate_class = XR_C_ABI_AGGREGATE_SLICE;
                        else if (slot_type->kind == XR_KIND_ENUM)
                            aggregate_class = XR_C_ABI_AGGREGATE_ADT_ENUM;
                        else
                            aggregate_class = XR_C_ABI_AGGREGATE_STRUCT;
                    }
                }
                plan->function_abis[abi_index++] = (XrCFunctionAbiEmissionView) {
                    .semantic_function = f,
                    .semantic_value = subject,
                    .ordinal = ordinal,
                    .parameter_count = function->parameter_count,
                    .target_register_kind = register_rep ? register_rep->kind : 0u,
                    .target_memory_kind = memory_rep ? memory_rep->kind : 0u,
                    .slot_class = slot_class,
                    .rep = (uint8_t) rep,
                    .pointee_rep = (uint8_t) pointee_rep,
                    .aggregate_class = aggregate_class,
                    .c_type = c_type,
                    .pointee_c_type = pointee_c_type,
                };
            }
            if (!complete) {
                abi_index = written;
            } else {
                uint8_t boundary_kind =
                    (function->semantic_effects & XI_EFFECT_MAY_SUSPEND) != 0
                        ? (uint8_t) XR_C_ABI_BOUNDARY_COROUTINE
                    : (function->is_module_initializer || function->capture_count != 0 ||
                       (function->semantic_effects & XI_EFFECT_MAY_THROW) != 0 ||
                       (signature_begin < abi_index &&
                        plan->function_abis[signature_begin].c_type &&
                        strcmp(plan->function_abis[signature_begin].c_type, "XrValue") == 0))
                        ? (uint8_t) XR_C_ABI_BOUNDARY_TAGGED
                        : (uint8_t) XR_C_ABI_BOUNDARY_NATIVE;
                for (uint32_t row = signature_begin; row < abi_index; row++)
                    plan->function_abis[row].boundary_kind = boundary_kind;
            }
        }
        plan->function_abi_count = abi_index;
    }
    compute_fingerprint(plan, &plan->fingerprint);
    if (!xr_c_emission_plan_verify(plan, target_plan, expected_profile_fingerprint, error,
                                   error_size)) {
        xr_c_emission_plan_free(plan);
        return false;
    }
    plan->verified = true;
    *out = plan;
    return true;
}

void xr_c_emission_plan_free(XrCEmissionPlan *plan) {
    if (!plan)
        return;
    for (uint32_t i = 0; i < plan->value_count; i++)
        xr_free((void *) plan->values[i].literal_bytes);
    for (uint32_t i = 0; i < plan->value_count; i++)
        if (plan->values[i].rep == XR_C_VALUE_REP_AGGREGATE)
            xr_free((void *) plan->values[i].c_type);
    for (uint32_t i = 0; i < plan->value_count; i++)
        xr_free((void *) plan->values[i].backing_c_type);
    for (uint32_t i = 0; i < plan->value_count; i++)
        xr_free((void *) plan->values[i].recipe_symbol);
    for (uint32_t i = 0; i < plan->value_count; i++) {
        xr_free((void *) plan->values[i].recipe_type_name);
        xr_free((void *) plan->values[i].recipe_member_name);
    }
    for (uint32_t i = 0; i < plan->cleanup_count; i++)
        xr_free((void *) plan->cleanups[i].recipe_symbol);
    xr_free(plan->cleanups);
    xr_free(plan->function_abis);
    xr_free(plan->recipe_arguments);
    xr_free(plan->call_arguments);
    xr_free(plan->values);
    xr_free(plan);
}

bool xr_c_emission_plan_is_verified(const XrCEmissionPlan *plan) {
    return plan && plan->verified;
}

uint32_t xr_c_emission_plan_value_count(const XrCEmissionPlan *plan) {
    return plan ? plan->value_count : 0;
}

uint32_t xr_c_emission_plan_call_argument_count(const XrCEmissionPlan *plan) {
    return plan ? plan->call_argument_count : 0;
}

uint32_t xr_c_emission_plan_cleanup_count(const XrCEmissionPlan *plan) {
    return plan ? plan->cleanup_count : 0;
}

XrFingerprint xr_c_emission_plan_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->fingerprint : zero;
}

XrFingerprint xr_c_emission_plan_target_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->target_fingerprint : zero;
}

XrFingerprint xr_c_emission_plan_profile_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->profile_fingerprint : zero;
}

bool xr_c_emission_plan_value_view(const XrCEmissionPlan *plan, uint32_t semantic_value,
                                   XrCValueEmissionView *out, char *error, size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!plan || !plan->verified || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "verified C emission plan input is missing");
    uint32_t begin = 0;
    uint32_t end = plan->value_count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2u;
        const XrCValueEmissionView *candidate = &plan->values[middle];
        if (candidate->semantic_value == semantic_value) {
            *out = *candidate;
            return true;
        }
        if (candidate->semantic_value < semantic_value)
            begin = middle + 1u;
        else
            end = middle;
    }
    return emission_error(error, error_size, "XR_TARGET_1001",
                          "semantic C value has no immutable C emission binding");
}

uint32_t xr_c_emission_plan_function_abi_count(const XrCEmissionPlan *plan) {
    return plan ? plan->function_abi_count : 0u;
}

/* Row by position, for callers that sweep every signature rather than asking
 * about one function -- emitting the typedefs a module's boundaries mention,
 * for instance. Keyed lookup stays the way to answer a question about a known
 * function; this is for the walks. */
bool xr_c_emission_plan_function_abi_at(const XrCEmissionPlan *plan, uint32_t index,
                                        XrCFunctionAbiEmissionView *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!plan || !plan->verified || !out || index >= plan->function_abi_count)
        return false;
    *out = plan->function_abis[index];
    return true;
}

/* One slot of one signature, keyed the way it is stored: ordinal 0 is the
 * return and 1..N the parameters, so a caller asking for a parameter by
 * declaration index adds one and never has to know where the return sits. */
bool xr_c_emission_plan_function_abi_view(const XrCEmissionPlan *plan, uint32_t semantic_function,
                                          uint16_t ordinal, XrCFunctionAbiEmissionView *out,
                                          char *error, size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!plan || !plan->verified || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "verified C function-ABI plan input is missing");
    uint32_t begin = 0;
    uint32_t end = plan->function_abi_count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2u;
        const XrCFunctionAbiEmissionView *candidate = &plan->function_abis[middle];
        if (candidate->semantic_function == semantic_function && candidate->ordinal == ordinal) {
            *out = *candidate;
            return true;
        }
        if (candidate->semantic_function < semantic_function ||
            (candidate->semantic_function == semantic_function && candidate->ordinal < ordinal))
            begin = middle + 1u;
        else
            end = middle;
    }
    return emission_error(error, error_size, "XR_TARGET_1001",
                          "semantic function slot has no immutable C emission binding");
}

bool xr_c_emission_plan_call_argument_view(const XrCEmissionPlan *plan,
                                           uint32_t semantic_call_value, uint16_t ordinal,
                                           XrCCallArgumentEmissionView *out, char *error,
                                           size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!plan || !plan->verified || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "verified C call-argument plan input is missing");
    uint32_t begin = 0;
    uint32_t end = plan->call_argument_count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2u;
        const XrCCallArgumentEmissionView *candidate = &plan->call_arguments[middle];
        if (candidate->semantic_call_value == semantic_call_value &&
            candidate->ordinal == ordinal) {
            *out = *candidate;
            return true;
        }
        if (candidate->semantic_call_value < semantic_call_value ||
            (candidate->semantic_call_value == semantic_call_value && candidate->ordinal < ordinal))
            begin = middle + 1u;
        else
            end = middle;
    }
    return emission_error(error, error_size, "XR_TARGET_1001",
                          "semantic call argument has no immutable C emission binding");
}

bool xr_c_emission_plan_cleanup_view(const XrCEmissionPlan *plan, uint32_t semantic_operation,
                                     XrCCleanupEmissionView *out, char *error, size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!plan || !plan->verified || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "verified C cleanup plan input is missing");
    uint32_t begin = 0;
    uint32_t end = plan->cleanup_count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2u;
        const XrCCleanupEmissionView *candidate = &plan->cleanups[middle];
        if (candidate->semantic_operation == semantic_operation) {
            *out = *candidate;
            return true;
        }
        if (candidate->semantic_operation < semantic_operation)
            begin = middle + 1u;
        else
            end = middle;
    }
    return emission_error(error, error_size, "XR_TARGET_1001",
                          "semantic cleanup has no immutable C emission binding");
}
