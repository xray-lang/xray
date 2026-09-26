/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_dynamic_value_shape.h - Values carried in generic tagged reference storage
 */

#ifndef XR_SEMANTIC_DYNAMIC_VALUE_SHAPE_H
#define XR_SEMANTIC_DYNAMIC_VALUE_SHAPE_H

#include "../../ir/xi.h"
#include "../../frontend/analyzer/xa_intrinsic_registry.h"
#include "../../shared/xr_obj_header.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_class_shape.h"
#include "xr_semantic_range_shape.h"
#include "xr_semantic_plan.h"
#include "xr_semantic_local_addr_shape.h"
#include "xr_semantic_task_shape.h"
#include "xr_semantic_type_admission_shape.h"
#include "xr_semantic_value_aggregate_shape.h"
#include "xr_semantic_allocation_shape.h"
#include "../../base/xglobal_indices.h"
#include <stdio.h>
#include <string.h>

/* Runtime constructors share a fresh tagged owner contract. The finite
 * signatures below identify actual runtime primitives, not ordinary library
 * functions. Physical layout and emission bindings are verified separately. */
typedef enum {
    XR_SEM_RUNTIME_CONSTRUCTOR_NONE,
    XR_SEM_RUNTIME_CONSTRUCTOR_STRINGBUILDER,
    XR_SEM_RUNTIME_CONSTRUCTOR_ATOMIC_I64,
    XR_SEM_RUNTIME_CONSTRUCTOR_ATOMIC_F64,
    XR_SEM_RUNTIME_CONSTRUCTOR_ATOMIC_BOOL,
} XrSemanticRuntimeConstructorKind;

typedef struct {
    XrSemanticRuntimeConstructorKind kind;
    uint32_t argument_value;
} XrSemanticRuntimeConstructorShape;

static inline bool xr_semantic_runtime_constructor_type_is_exact(const XrSemanticTypeRecord *type,
                                                                 uint16_t kind, uint32_t builtin,
                                                                 const char *name,
                                                                 const char *child_key) {
    XrStableId zero = {{0}};
    char key[512];
    int written =
        snprintf(key, sizeof(key), "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:%zu:%s[%u%s%s]",
                 (unsigned) kind, (unsigned) builtin, (unsigned) XR_SCALAR_REP_NONE, strlen(name),
                 name, child_key ? 1u : 0u, child_key ? ";" : "", child_key ? child_key : "");
    return type && written > 0 && (size_t) written < sizeof(key) && type->kind == kind &&
           type->builtin_type == builtin && type->child_count == (child_key ? 1u : 0u) &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->canonical_key &&
           strcmp(type->canonical_key, key) == 0;
}

static inline bool
xr_semantic_runtime_constructor_signature(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *operation,
                                          XrSemanticRuntimeConstructorShape *out) {
    if (!plan || !operation ||
        (operation->opcode != XI_CALL_BUILTIN && operation->opcode != XI_CALL) ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->semantic_immediate != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(operation->opcode) ||
        operation->flags != xi_generated_op_default_flags(operation->opcode) ||
        operation->ownership_use != xi_generated_op_own_use(operation->opcode) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, operation->result_type);
    XrSemanticRuntimeConstructorShape shape = {
        .kind = XR_SEM_RUNTIME_CONSTRUCTOR_STRINGBUILDER,
        .argument_value = XR_SEMANTIC_INDEX_NONE,
    };
    if (operation->opcode == XI_CALL_BUILTIN) {
        if (operation->operand_count != 0 || operation->metadata_count != 1 ||
            operation->metadata_begin >= metadata_count || !metadata ||
            strcmp(metadata[operation->metadata_begin], "StringBuilder") != 0 ||
            !xr_semantic_runtime_constructor_type_is_exact(
                type, XR_KIND_INSTANCE, XR_TID_STRINGBUILDER, "StringBuilder", NULL))
            return false;
    } else {
        uint32_t operand_count = 0, child_count = 0;
        const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
        const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
        if (!type || !children || type->child_count != 1 || type->child_begin >= child_count ||
            !operands || operation->operand_count != 2 ||
            operation->operand_begin >= operand_count ||
            operand_count - operation->operand_begin < 2 || operation->metadata_count != 0)
            return false;
        const XrSemanticOperandRecord *callee = &operands[operation->operand_begin];
        const XrSemanticOperandRecord *argument = callee + 1;
        const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, argument->type);
        if (!element || element->flags != 0 || element->child_count != 0 ||
            element->builtin_type != XR_TID_NULL || element->aggregate_extent != 0 ||
            element->aggregate_align != 0 || !element->canonical_key ||
            children[type->child_begin] != argument->type ||
            !xr_semantic_runtime_constructor_type_is_exact(type, XR_KIND_INSTANCE, XR_TID_NULL,
                                                           "Atomic", element->canonical_key) ||
            !xr_semantic_runtime_constructor_type_is_exact(
                xr_semantic_plan_type(plan, callee->type), XR_KIND_CLASS, XR_TID_NULL, "Atomic",
                NULL) ||
            callee->role != XR_SEM_OPERAND_CALLEE || callee->parameter != -1 ||
            callee->flags != 0 || callee->ownership_action != XR_SEM_OPERAND_BORROW ||
            argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            argument->ownership_action != XR_SEM_OPERAND_CONSUME ||
            argument->parameter_mode != XR_PARAM_READ ||
            argument->transfer_mode != XR_TRANSFER_SHARE)
            return false;
        if (element->kind == XR_KIND_INT && element->scalar_rep == XR_NATIVE_I64)
            shape.kind = XR_SEM_RUNTIME_CONSTRUCTOR_ATOMIC_I64;
        else if (element->kind == XR_KIND_FLOAT && element->scalar_rep == XR_NATIVE_F64)
            shape.kind = XR_SEM_RUNTIME_CONSTRUCTOR_ATOMIC_F64;
        else if (element->kind == XR_KIND_BOOL && element->scalar_rep == XR_SCALAR_REP_NONE)
            shape.kind = XR_SEM_RUNTIME_CONSTRUCTOR_ATOMIC_BOOL;
        else
            return false;
        const XrSemanticOperationRecord *definition = NULL;
        for (uint32_t i = 0; i < xr_semantic_plan_operation_count(plan); ++i) {
            const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
            if (!candidate || candidate->result_value != callee->value)
                continue;
            if (definition)
                return false;
            definition = candidate;
        }
        if (!definition || definition->function != operation->function ||
            definition->result_type != callee->type || definition->opcode != XI_GET_BUILTIN ||
            definition->semantic_immediate != XR_GLOBAL_VAR_ATOMIC ||
            definition->operand_count != 0 || definition->metadata_count != 1 || !metadata ||
            definition->metadata_begin >= metadata_count ||
            strcmp(metadata[definition->metadata_begin], "Atomic") != 0 ||
            definition->effects != xi_generated_op_effects(XI_GET_BUILTIN) ||
            definition->flags != xi_generated_op_default_flags(XI_GET_BUILTIN) ||
            definition->auxiliary_kind != XI_AUX_KIND_NONE ||
            definition->constant != XR_SEMANTIC_INDEX_NONE ||
            definition->callable_function != XR_SEMANTIC_INDEX_NONE ||
            definition->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
            definition->result_alias_operand != -1)
            return false;
        shape.argument_value = argument->value;
    }
    if (out)
        *out = shape;
    return true;
}

static inline bool xr_semantic_runtime_constructor_shape(const XrSemanticPlan *plan,
                                                         const XrSemanticOperationRecord *operation,
                                                         XrSemanticRuntimeConstructorShape *out) {
    return xr_semantic_allocation_identity_is_canonical(operation) &&
           xr_semantic_runtime_constructor_signature(plan, operation, out);
}

static inline bool
xr_semantic_runtime_constructor_is_exact(const XrSemanticPlan *plan,
                                         const XrSemanticOperationRecord *operation) {
    return xr_semantic_runtime_constructor_shape(plan, operation, NULL);
}

/* The compiler's own "unknown" reference type: source can neither write it nor
 * name it, so a record carrying it was produced by the compiler and nowhere
 * else.  A join over differently shaped arms leaves one behind; so does an enum
 * declaration, whose namespace descriptor has no surface type to carry.
 *
 * Whatever produced it, the value is a reference on every path that can reach
 * it, so it is held the one way every untyped reference is held -- tagged.  The
 * scalar family classifies this type as not-applicable and moves on without
 * binding anything, which is correct, but it leaves the value with no storage
 * at all and refuses its readers with a diagnostic naming the reader rather
 * than the producer. */
static inline bool xr_semantic_dynamic_value_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_UNKNOWN && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && type->source_enum_key == NULL &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0;
}

/* The wider type test, for a producer whose storage fact does not come from the
 * value's type at all.  A module-level slot is one XrValue whatever it holds,
 * so a read of one is tagged because of where it was read, not because of what
 * was in it -- a Task, a Json, a nullable String all come out of the same slot
 * the same way.
 *
 * A native scalar is excluded: an int in a slot has machine storage of its own
 * and the families that name it answer first.  So are borrowed views and exact
 * aggregates, whose storage is a shape rather than a carrier.
 *
 * So is an enum.  Its layout and member identity are facts a value of it
 * carries, and the plan verifier holds a binding for one to the family that
 * knows them; a slot read cannot answer for those by pointing at the slot. */
static inline bool
xr_semantic_dynamic_value_carrier_type_is_exact(const XrSemanticTypeRecord *type) {
    return type && type->kind != XR_KIND_ENUM && type->scalar_rep == XR_SCALAR_REP_NONE &&
           (type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) != 0 &&
           (type->flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0 &&
           (type->flags & (XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT)) == 0 &&
           type->aggregate_align == 0 &&
           (type->kind == XR_KIND_STRUCT_OBJECT
                ? (type->child_count != 0 && type->aggregate_extent == type->child_count &&
                   (type->flags & XR_SEM_TYPE_VALUE) == 0)
                : type->aggregate_extent == 0);
}

/* The facts every producer of an untyped reference must state the same way,
 * whatever the opcode: it results in a value, it is not a call, not an import,
 * not an intrinsic, not a view, and does not alias an operand or a parameter.
 * What differs between producers -- whether a constant backs the value, whether
 * metadata describes it, whether an immediate names where it lives, whether it
 * resolves an import -- is left to the roster below. */
static inline bool
xr_semantic_dynamic_value_common_is_exact(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *operation) {
    return plan && operation && operation->result_value != XR_SEMANTIC_INDEX_NONE &&
           operation->function < xr_semantic_plan_function_count(plan) &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE &&

           operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
           operation->view_source_value == XR_SEMANTIC_INDEX_NONE &&
           operation->view_element_type == XR_SEMANTIC_INDEX_NONE &&
           operation->view_source_operand == -1 && operation->view_source_parameter == -1 &&
           operation->view_origin == XI_VIEW_ORIGIN_NONE && operation->view_capability == 0 &&
           operation->view_lifetime == 0 && operation->view_complete == 0;
}

/* Whether the producer allocated what it holds.  A producer that allocates
 * carries the identity of its allocation and a producer that does not carries
 * none, and the roster states which of the two each one is rather than letting
 * either pass unexamined. */
static inline bool xr_semantic_dynamic_value_allocates(const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    return operation && operation->allocation_key != NULL &&
           !xr_stable_id_equal(operation->allocation_id, zero);
}

static inline bool
xr_semantic_dynamic_value_allocates_nothing(const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    return operation && operation->allocation_key == NULL &&
           xr_stable_id_equal(operation->allocation_id, zero);
}

/* The roster of producers admitted to this family, and what each must state.
 * A producer is admitted only after its own shape has been measured, so the
 * list grows one measured opcode at a time rather than by opening the family
 * to whatever carries the untyped type.
 *
 * XI_PHI       a join of reference-capable values. It merges tagged carriers its
 *              incoming edges already own, so it names no constant or metadata.
 *              Unlike the producer-only cases below, a join may preserve an
 *              exact source type such as Array<T> or a class instance; storage
 *              is still the same tagged reference carrier.
 * XI_CONST     an enum declaration's namespace descriptor, marked as such by
 *              lowering.  A constant backs it and the member table describes
 *              it, so both are required to be present rather than absent.
 * XI_GET_SHARED a read of a module-level slot, which holds a tagged value and
 *              nothing else.  Its immediate names which slot, so unlike the
 *              other producers it is expected to carry one, and the read
 *              borrows what the slot owns rather than owning it.
 * XI_AWAIT     the value a finished task handed back, which crosses the
 *              coroutine boundary as one XrValue whatever its type -- so like a
 *              slot read it is judged by the carrier rather than by the type.
 * XI_CELL_NEW  a fresh cell holding a captured binding.  It allocates, which
 *              every other producer here does not, and says so.
 * XI_AS        a checked conversion whose target the plan records as one piece
 *              of metadata; the immediate names which type.
 * XI_IMPORT_REF a reference to something another module owns.  It is the one
 *              producer here that resolves an import, and it borrows what the
 *              other module owns rather than owning it.
 * XI_CORO_OP   a coroutine primitive's result, which leaves the coroutine the
 *              same way an awaited value does -- so it too is judged by the
 *              carrier rather than by the type. */
static inline bool
xr_semantic_dynamic_value_producer_is_exact(const XrSemanticOperationRecord *operation) {
    if (!operation)
        return false;
    switch (operation->opcode) {
        case XI_PHI:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->semantic_immediate == 0 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_CONST:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_ENUM_NAMESPACE &&
                   operation->metadata_count != 0 && operation->semantic_immediate == 0 &&
                   operation->constant != XR_SEMANTIC_INDEX_NONE &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_GET_SHARED:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->operand_count == 0 &&
                   operation->semantic_immediate <= UINT16_MAX &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
                   operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_AWAIT:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->operand_count == 1 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                   operation->return_provenance == XR_SEM_RETURN_OWNED &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_CELL_NEW:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->operand_count == 1 &&
                   operation->semantic_immediate == 0 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                   operation->return_provenance == XR_SEM_RETURN_OWNED &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates(operation);
        case XI_AS:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 1 && operation->operand_count == 1 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                   operation->return_provenance == XR_SEM_RETURN_OWNED &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_IMPORT_REF:
            return operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 2 && operation->operand_count == 0 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
                   operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_CORO_OP:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 &&
                   operation->effects == xi_generated_op_effects(XI_CORO_OP) &&
                   operation->flags == xi_generated_op_default_flags(XI_CORO_OP) &&
                   operation->ownership_use == xi_generated_op_own_use(XI_CORO_OP) &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                   operation->return_provenance == XR_SEM_RETURN_OWNED &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        default:
            return false;
    }
}

/* Whether a producer of this family owns what it holds or only borrows it.
 * A join and a descriptor own their value; a read of a shared slot borrows the
 * one the slot owns, and releasing it would drop a reference the reader never
 * took.  The builder writes this into the row and both verifiers check it, so
 * like the slot role it is answered here once. */
static inline bool
xr_semantic_dynamic_value_is_borrowed(const XrSemanticOperationRecord *operation) {
    return operation && operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED;
}

/* Which slot role a producer of this family takes.  A join is held in the slot
 * role joins use; every other producer is a temporary.  The builder writes the
 * role, the plan verifier checks it, and the AOT oracle checks it again, so the
 * question is asked in three places and answered here once. */
static inline bool xr_semantic_dynamic_value_is_join(const XrSemanticOperationRecord *operation) {
    return operation && operation->opcode == XI_PHI;
}

/* Every checked conversion freezes the same operation kernel before its
 * target family is interpreted. Keeping this base in one judgement prevents
 * scalar/string and source-class `as` from drifting into separate authorities
 * for effects, ownership, evidence or auxiliary storage metadata. */
static inline bool
xr_semantic_checked_as_base_is_exact(const XrSemanticPlan *plan,
                                     const XrSemanticOperationRecord *operation) {
    return xr_semantic_dynamic_value_common_is_exact(plan, operation) &&
           xr_semantic_dynamic_value_producer_is_exact(operation) && operation->opcode == XI_AS &&
           operation->effects == xi_generated_op_effects(XI_AS) &&
           operation->flags == xi_generated_op_default_flags(XI_AS) &&
           operation->ownership_use == xi_generated_op_own_use(XI_AS) &&
           operation->result_ownership == xi_generated_op_result_ownership(XI_AS) &&
           operation->transfer_mode == XR_TRANSFER_SHARE &&
           operation->parameter_mode == XR_PARAM_READ &&
           operation->parameter_ownership == XI_OWN_NONE && operation->result_alias_operand == -1 &&
           operation->evidence[0] == 0 && operation->evidence[1] == 0 &&
           operation->evidence[2] == 0 && operation->evidence[3] == 0 &&
           operation->evidence[4] == 0 && operation->evidence[5] == 0 &&
           operation->evidence[6] == 0 && operation->evidence[7] == XR_SEMANTIC_INDEX_NONE &&
           operation->array_element_storage == 0 &&
           operation->array_hof_kind == XR_SEM_ARRAY_HOF_NONE &&
           operation->array_result_element_storage == 0 && operation->reserved_view[0] == 0 &&
           operation->reserved_view[1] == 0;
}

/* Re-prove the typed SSA source behind one incoming edge. Target consumers call
 * this only after the whole SemanticPlan verifier has accepted CFG dominance;
 * this local judgement still checks the identity facts that decide whether the
 * incoming value can occupy the same tagged carrier as the PHI result. */
static inline bool xr_semantic_reference_phi_input_is_exact(const XrSemanticPlan *plan,
                                                            uint32_t value, uint32_t type,
                                                            uint32_t function) {
    const XrSemanticOperationRecord *definition = NULL;
    const XrSemanticParameterRecord *parameter = NULL;
    size_t operation_count = xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != value)
            continue;
        if (definition || candidate->result_type != type || candidate->function != function)
            return false;
        definition = candidate;
    }
    size_t parameter_count = xr_semantic_plan_parameter_count(plan);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *candidate = xr_semantic_plan_parameter(plan, i);
        if (!candidate || candidate->value != value)
            continue;
        if (parameter || candidate->type != type || candidate->function != function)
            return false;
        parameter = candidate;
    }
    /* PARAM and its signature record describe one definition. A non-parameter
     * operation must never also claim a signature value. */
    return definition ? (definition->opcode == XI_PARAM ? parameter != NULL : parameter == NULL)
                      : parameter != NULL;
}

/* An immutable copied capture borrows the closure's retained managed value. */
static inline bool xr_semantic_copied_capture_load_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    const XrSemanticFunctionRecord *function = operation
        ? xr_semantic_plan_function(plan, operation->function) : NULL;
    if (!function || !operation || operation->opcode != XI_LOAD_UPVAL ||
        !xr_semantic_dynamic_value_common_is_exact(plan, operation) ||
        !xr_semantic_dynamic_value_allocates_nothing(operation) ||
        !xr_semantic_dynamic_value_carrier_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type)) ||
        operation->operand_count != 0 || operation->metadata_count != 0 ||
        operation->semantic_immediate < 0 ||
        (uint64_t) operation->semantic_immediate >= function->capture_count ||
        function->capture_begin > xr_semantic_plan_capture_count(plan) ||
        function->capture_count > xr_semantic_plan_capture_count(plan) - function->capture_begin ||
        operation->constant != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_LOAD_UPVAL) ||
        operation->flags != xi_generated_op_default_flags(XI_LOAD_UPVAL) ||
        operation->ownership_use != xi_generated_op_own_use(XI_LOAD_UPVAL) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED)
        return false;
    const XrSemanticCaptureRecord *capture = xr_semantic_plan_capture(
        plan, function->capture_begin + (uint32_t) operation->semantic_immediate);
    return capture && capture->function == operation->function &&
           capture->ordinal == operation->semantic_immediate &&
           capture->source_function == function->parent &&
           capture->source == XR_SEM_CAPTURE_LOCAL_VALUE && capture->kind == XR_SEM_CAPTURE_BY_COPY &&
           capture->flags == 0 && capture->reserved[0] == 0 &&
           capture->type == operation->result_type && capture->source_type == capture->type &&
           capture->source_capture == XR_SEMANTIC_INDEX_NONE &&
           xr_semantic_reference_phi_input_is_exact(
               plan, capture->source_value, capture->source_type, capture->source_function);
}

/* Read the current local through the cleanup's frozen frame address. */
static inline bool xr_semantic_cleanup_reference_load_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    uint32_t count = 0u;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &count);
    if (!operation || !operands || operation->opcode != XI_PLACE_LOAD ||
        !xr_semantic_dynamic_value_common_is_exact(plan, operation) ||
        !xr_semantic_dynamic_value_allocates_nothing(operation) ||
        !xr_semantic_dynamic_value_carrier_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type)) ||
        operation->operand_count != 1u || operation->operand_begin >= count ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_PLACE_LOAD) ||
        operation->flags != xi_generated_op_default_flags(XI_PLACE_LOAD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_PLACE_LOAD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED)
        return false;
    const XrSemanticOperandRecord *place = &operands[operation->operand_begin];
    const XrSemanticOperationRecord *address =
        xr_semantic_unique_value_definition(plan, place->value);
    return xr_semantic_local_addr_is_exact(plan, address, NULL) &&
           address->function == operation->function && address->result_type == place->type &&
           place->type == operation->result_type && place->role == XR_SEM_OPERAND_VALUE &&
           place->parameter == -1 && place->transfer_mode == XR_TRANSFER_SHARE &&
           place->ownership_action == XR_SEM_OPERAND_BORROW &&
           place->parameter_mode == XR_PARAM_READ && place->access == XR_CALL_ARG_PLAIN &&
           place->origin == XI_PLACE_ORIGIN_NONE && place->lifetime == XI_PLACE_LIFETIME_NONE &&
           place->escape == XI_PLACE_ESCAPE_NONE && place->flags == 0;
}

/* A PHI may preserve an exact source type while merging reference-capable
 * values. Each incoming edge carries the same type, or an exact null constant
 * when the result is nullable, in the same function. The operation has the generated,
 * ownership-consuming PHI shape. This keeps the broad carrier reusable without
 * admitting a mixed or forged join. */
static inline bool xr_semantic_reference_phi_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticBlockRecord *block =
        operation ? xr_semantic_plan_block(plan, operation->block) : NULL;
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!operation || !block || !operands || operation->opcode != XI_PHI ||
        !xr_semantic_dynamic_value_common_is_exact(plan, operation) ||
        !xr_semantic_dynamic_value_producer_is_exact(operation) ||
        !xr_semantic_dynamic_value_carrier_type_is_exact(type) ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE || operation->operand_count == 0 ||
        operation->operand_count != block->predecessor_count ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->effects != xi_generated_op_effects(XI_PHI) ||
        operation->flags != xi_generated_op_default_flags(XI_PHI) ||
        operation->ownership_use != xi_generated_op_own_use(XI_PHI) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_PHI) ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->evidence[0] != 0 ||
        operation->evidence[1] != 0 || operation->evidence[2] != 0 || operation->evidence[3] != 0 ||
        operation->evidence[4] != 0 || operation->evidence[5] != 0 || operation->evidence[6] != 0 ||
        operation->evidence[7] != XR_SEMANTIC_INDEX_NONE || operation->array_element_storage != 0 ||
        operation->array_hof_kind != XR_SEM_ARRAY_HOF_NONE ||
        operation->array_result_element_storage != 0 || operation->reserved_view[0] != 0 ||
        operation->reserved_view[1] != 0)
        return false;
    for (uint16_t i = 0; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *operand = &operands[operation->operand_begin + i];
        const XrSemanticTypeRecord *input_type = xr_semantic_plan_type(plan, operand->type);
        bool exact_type = operand->type == operation->result_type;
        if (!exact_type && (type->flags & XR_SEM_TYPE_NULLABLE) != 0 && input_type &&
            input_type->kind == XR_KIND_NULL) {
            const XrSemanticOperationRecord *definition =
                xr_semantic_unique_value_definition(plan, operand->value);
            const XrSemanticConstantRecord *constant = definition
                ? xr_semantic_plan_constant(plan, definition->constant) : NULL;
            exact_type = definition && definition->opcode == XI_CONST &&
                         definition->operand_count == 0 && constant &&
                         constant->kind == XR_SEM_CONST_NULL && constant->type == operand->type;
        }
        if (!exact_type || operand->role != XR_SEM_OPERAND_VALUE ||
            operand->parameter != -1 || operand->transfer_mode != XR_TRANSFER_SHARE ||
            operand->ownership_action != XR_SEM_OPERAND_CONSUME ||
            operand->parameter_mode != XR_PARAM_READ || operand->access != XR_CALL_ARG_PLAIN ||
            operand->origin != XI_PLACE_ORIGIN_NONE ||
            operand->lifetime != XI_PLACE_LIFETIME_NONE ||
            operand->escape != XI_PLACE_ESCAPE_NONE || operand->flags != 0 ||
            !xr_semantic_reference_phi_input_is_exact(plan, operand->value, operand->type,
                                                      operation->function))
            return false;
    }
    return true;
}

/* A checked union narrowing to one exact source class still produces the
 * ordinary tagged carrier.  XI lowering freezes the narrowed result type; the
 * SemanticPlan must independently prove that it is one unique member of the
 * source union and that the runtime target spelling names the same frozen
 * source-class row.  This admits the carrier, not an arbitrary class-typed
 * operation, and leaves scalar/string conversions to their narrower family. */
static inline bool
xr_semantic_dynamic_source_class_as_is_exact(const XrSemanticPlan *plan,
                                             const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    uint32_t child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    const XrSemanticTypeRecord *result =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    uint32_t source_class = xr_semantic_class_instance_type_source_class(plan, result);
    const XrSemanticSourceClassRecord *declaration =
        source_class != XR_SEMANTIC_INDEX_NONE ? xr_semantic_plan_source_class(plan, source_class)
                                               : NULL;
    if (!plan || !operation || !operands || !metadata || !children || !declaration ||
        !xr_semantic_checked_as_base_is_exact(plan, operation) ||
        !xr_semantic_dynamic_value_carrier_type_is_exact(result) ||
        operation->operand_begin >= operand_count || operation->metadata_begin >= metadata_count ||
        !metadata[operation->metadata_begin] || !declaration->name ||
        strcmp(metadata[operation->metadata_begin], declaration->name) != 0 ||
        operation->semantic_immediate != ((int64_t) UINT32_MAX << 1))
        return false;

    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(plan, source->type);
    if (!source_type || source_type->kind != XR_KIND_UNION || source_type->child_count < 2 ||
        !xr_semantic_dynamic_value_carrier_type_is_exact(source_type) ||
        source_type->child_begin > child_count ||
        source_type->child_count > child_count - source_type->child_begin ||
        source->value == XR_SEMANTIC_INDEX_NONE || source->role != XR_SEM_OPERAND_VALUE ||
        source->parameter != -1 || source->transfer_mode != XR_TRANSFER_SHARE ||
        source->ownership_action != XR_SEM_OPERAND_BORROW ||
        source->parameter_mode != XR_PARAM_READ || source->access != XR_CALL_ARG_PLAIN ||
        source->origin != XI_PLACE_ORIGIN_NONE || source->lifetime != XI_PLACE_LIFETIME_NONE ||
        source->escape != XI_PLACE_ESCAPE_NONE || source->flags != 0)
        return false;
    uint32_t matches = 0;
    for (uint16_t i = 0; i < source_type->child_count; i++)
        matches += children[source_type->child_begin + i] == operation->result_type ? 1u : 0u;
    return matches == 1u;
}

/* Optional references preserve the payload's carrier. The nullable bit is
 * the only type difference; declaration identity and every child remain exact. */
static inline bool
xr_semantic_optional_reference_pair_is_exact(const XrSemanticPlan *plan,
                                             const XrSemanticTypeRecord *payload,
                                             const XrSemanticTypeRecord *optional) {
    if ((!xr_semantic_dynamic_value_carrier_type_is_exact(payload) &&
         !xr_semantic_tagged_tuple_type_is_exact(plan, payload)) ||
        !xr_semantic_type_is_nullable_widening(payload, optional) ||
        (optional->flags & (uint8_t) ~XR_SEM_TYPE_NULLABLE) != payload->flags ||
        !xr_semantic_type_same_structure(plan, payload, optional))
        return false;
    return true;
}

/* None has no payload; Some consumes one exact payload without allocating a
 * second object. Both produce the same owned tagged carrier used by the
 * projection and identity-forwarding paths. */
static inline bool
xr_semantic_optional_reference_inject_is_exact(const XrSemanticPlan *plan,
                                               const XrSemanticOperationRecord *operation) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!xr_semantic_dynamic_value_common_is_exact(plan, operation) ||
        (!xr_semantic_dynamic_value_carrier_type_is_exact(type) &&
         !xr_semantic_tagged_tuple_type_is_exact(plan, type)) ||
        (type->flags & XR_SEM_TYPE_NULLABLE) == 0 || operation->opcode != XI_SUM_INJECT ||
        operation->semantic_immediate < 0 || operation->semantic_immediate > 1 ||
        operation->operand_count != (uint16_t) operation->semantic_immediate ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->metadata_count != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_SUM_INJECT) ||
        operation->flags != xi_generated_op_default_flags(XI_SUM_INJECT) ||
        operation->ownership_use != xi_generated_op_own_use(XI_SUM_INJECT) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance !=
            (operation->operand_count == 0 ? XR_SEM_RETURN_BORROWED_STATIC : XR_SEM_RETURN_OWNED) ||
        operation->return_complete != 1 ||
        !xr_semantic_dynamic_value_allocates_nothing(operation) ||
        xr_semantic_unique_value_definition(plan, operation->result_value) != operation)
        return false;
    if (operation->operand_count == 0)
        return true;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count)
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    return xr_semantic_optional_reference_pair_is_exact(
               plan, xr_semantic_plan_type(plan, source->type), type) &&
           source->role == XR_SEM_OPERAND_VALUE && source->parameter == -1 &&
           source->transfer_mode == XR_TRANSFER_SHARE &&
           source->ownership_action == XR_SEM_OPERAND_CONSUME &&
           source->parameter_mode == XR_PARAM_READ && source->access == XR_CALL_ARG_PLAIN &&
           source->origin == XI_PLACE_ORIGIN_NONE && source->lifetime == XI_PLACE_LIFETIME_NONE &&
           source->escape == XI_PLACE_ESCAPE_NONE && source->flags == 0;
}

/* A closed Optional<T> projection borrows T's tagged carrier. The type relation
 * and payload ordinal establish storage independently of the source syntax or
 * the consumer. Control-flow and runtime checks govern whether Some is present;
 * they do not change the representation of its payload. */
static inline bool
xr_semantic_optional_reference_project_is_exact(const XrSemanticPlan *plan,
                                                const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!xr_semantic_dynamic_value_common_is_exact(plan, operation) || !operands ||
        operation->opcode != XI_VARIANT_PROJECT || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        operation->semantic_immediate != xi_variant_pack_projection(1u, 0u) ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->metadata_count != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        !xr_semantic_dynamic_value_allocates_nothing(operation) ||
        operation->effects != xi_generated_op_effects(XI_VARIANT_PROJECT) ||
        operation->flags != xi_generated_op_default_flags(XI_VARIANT_PROJECT) ||
        operation->ownership_use != xi_generated_op_own_use(XI_VARIANT_PROJECT) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED)
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(plan, source->type);
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, operation->result_type);
    if (!xr_semantic_optional_reference_pair_is_exact(plan, result_type, source_type) ||
        source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 ||
        source->transfer_mode != XR_TRANSFER_SHARE ||
        source->ownership_action != XR_SEM_OPERAND_BORROW ||
        source->parameter_mode != XR_PARAM_READ || source->access != XR_CALL_ARG_PLAIN ||
        source->origin != XI_PLACE_ORIGIN_NONE || source->lifetime != XI_PLACE_LIFETIME_NONE ||
        source->escape != XI_PLACE_ESCAPE_NONE || source->flags != 0 ||
        !xr_semantic_reference_phi_input_is_exact(plan, source->value, source->type,
                                                  operation->function) ||
        xr_semantic_unique_value_definition(plan, operation->result_value) != operation)
        return false;

    return true;
}

/* The field table describes the allocation's contents, not an inline caller
 * slot. Construction creates one owned tagged root with exact field ordinals. */
static inline bool
xr_semantic_structural_allocation_is_exact(const XrSemanticPlan *plan,
                                           const XrSemanticOperationRecord *operation) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!type || !operation || operation->opcode != XI_OBJECT_NEW ||
        !xr_semantic_source_structural_shape_is_exact(plan, operation->result_type) ||
        !xr_semantic_dynamic_value_carrier_type_is_exact(type) ||
        !xr_semantic_dynamic_value_common_is_exact(plan, operation) ||
        operation->operand_count != 0 ||
        (operation->semantic_immediate & XI_OBJECT_AUX_FIELD_MASK) != type->child_count ||
        ((uint64_t) operation->semantic_immediate >> XI_OBJECT_AUX_STORAGE_SHIFT) >
            XR_OBJ_STORAGE_TRANSFER ||
        operation->metadata_count != type->child_count || !metadata ||
        operation->metadata_begin > metadata_count ||
        operation->metadata_count > metadata_count - operation->metadata_begin ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_OBJECT_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_OBJECT_NEW) ||
        operation->ownership_use != xi_generated_op_own_use(XI_OBJECT_NEW) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_complete != 1 ||
        !xr_semantic_allocation_identity_is_canonical(operation))
        return false;
    for (uint32_t i = 0; i < operation->metadata_count; ++i)
        if (!metadata[operation->metadata_begin + i] || !metadata[operation->metadata_begin + i][0])
            return false;
    return true;
}

/* Tuple construction publishes one tagged root after consuming its lanes. */
static inline bool xr_semantic_tuple_allocation_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!operation || operation->opcode != XI_TUPLE_NEW ||
        !xr_semantic_tagged_tuple_type_is_exact(plan, type) ||
        (type->flags & XR_SEM_TYPE_NULLABLE) != 0 ||
        !xr_semantic_dynamic_value_common_is_exact(plan, operation) ||
        operation->operand_count != type->child_count || operation->metadata_count != 0 ||
        (operation->semantic_immediate & XI_TUPLE_AUX_ARITY_MASK) != type->child_count ||
        ((uint64_t) operation->semantic_immediate >> XI_TUPLE_AUX_STORAGE_SHIFT) >
            XR_OBJ_STORAGE_TRANSFER ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_TUPLE_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_TUPLE_NEW) ||
        operation->ownership_use != xi_generated_op_own_use(XI_TUPLE_NEW) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_complete != 1 ||
        !xr_semantic_allocation_identity_is_canonical(operation))
        return false;
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    for (uint16_t i = 0; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *operand = &operands[operation->operand_begin + i];
        if (operand->type != children[type->child_begin + i] ||
            operand->role != XR_SEM_OPERAND_VALUE || operand->parameter != -1 ||
            operand->flags != 0 || operand->ownership_action != XR_SEM_OPERAND_CONSUME)
            return false;
    }
    return true;
}

static inline bool xr_semantic_atomic_type_is_exact(
    const XrSemanticPlan *plan, const XrSemanticTypeRecord *type) {
    uint32_t count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &count);
    if (!type || !children || type->child_count != 1 || type->child_begin >= count)
        return false;
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, children[type->child_begin]);
    return element && element->flags == 0 && element->child_count == 0 &&
        ((element->kind == XR_KIND_INT && element->scalar_rep == XR_NATIVE_I64) ||
         (element->kind == XR_KIND_FLOAT && element->scalar_rep == XR_NATIVE_F64) ||
         (element->kind == XR_KIND_BOOL && element->scalar_rep == XR_SCALAR_REP_NONE)) &&
        xr_semantic_runtime_constructor_type_is_exact(type, XR_KIND_INSTANCE, XR_TID_NULL,
                                                      "Atomic", element->canonical_key);
}

/* Compare-exchange returns an owned pair with the observed scalar and success. */
static inline bool xr_semantic_atomic_compare_result_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    const XrSemanticTypeRecord *type = operation
        ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!operation || operation->opcode != XI_ATOMIC_RMW ||
        operation->evidence[1] != XA_INTRINSIC_ATOMIC_COMPARE_EXCHANGE ||
        !xr_semantic_dynamic_value_common_is_exact(plan, operation) ||
        !xr_semantic_tagged_tuple_type_is_exact(plan, type) ||
        (type->flags & XR_SEM_TYPE_NULLABLE) || type->child_count != 2 ||
        (operation->operand_count != 3 && operation->operand_count != 4) ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->semantic_immediate < 0 || operation->semantic_immediate > 4 ||
        operation->effects != xi_generated_op_effects(XI_ATOMIC_RMW) ||
        operation->flags != xi_generated_op_default_flags(XI_ATOMIC_RMW) ||
        operation->ownership_use != xi_generated_op_own_use(XI_ATOMIC_RMW) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_complete != 1)
        return false;
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (!operands || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    const XrSemanticOperandRecord *args = operands + operation->operand_begin;
    const XrSemanticTypeRecord *receiver = xr_semantic_plan_type(plan, args[0].type);
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, children[type->child_begin]);
    const XrSemanticTypeRecord *success = xr_semantic_plan_type(plan, children[type->child_begin + 1]);
    if (!receiver || !element || !success || element->flags != 0 || element->child_count != 0 ||
        !((element->kind == XR_KIND_INT && element->scalar_rep == XR_NATIVE_I64) ||
          (element->kind == XR_KIND_FLOAT && element->scalar_rep == XR_NATIVE_F64) ||
          (element->kind == XR_KIND_BOOL && element->scalar_rep == XR_SCALAR_REP_NONE)) ||
        success->kind != XR_KIND_BOOL || success->flags != 0 || success->child_count != 0 ||
        success->scalar_rep != XR_SCALAR_REP_NONE ||
        !xr_semantic_runtime_constructor_type_is_exact(receiver, XR_KIND_INSTANCE, XR_TID_NULL,
                                                       "Atomic", element->canonical_key) ||
        receiver->child_begin >= child_count ||
        children[receiver->child_begin] != children[type->child_begin] ||
        args[1].type != children[type->child_begin] || args[2].type != args[1].type)
        return false;
    for (uint16_t i = 0; i < operation->operand_count; i++)
        if (args[i].role != XR_SEM_OPERAND_VALUE || args[i].parameter != -1 ||
            args[i].flags != 0 || args[i].ownership_action != XR_SEM_OPERAND_BORROW)
            return false;
    if (operation->operand_count == 4) {
        const XrSemanticTypeRecord *ordering = xr_semantic_plan_type(plan, args[3].type);
        const XrSemanticOperationRecord *definition = NULL;
        if (!ordering || ordering->kind != XR_KIND_INT || ordering->flags != 0)
            return false;
        for (uint32_t i = 0; i < xr_semantic_plan_operation_count(plan); i++) {
            const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
            if (candidate && candidate->result_value == args[3].value) {
                if (definition) return false;
                definition = candidate;
            }
        }
        if (!definition || definition->function != operation->function ||
            definition->opcode != XI_CONST || definition->operand_count != 0 ||
            definition->semantic_immediate < 0 || definition->semantic_immediate > 4)
            return false;
    }
    return true;
}

static inline bool xr_semantic_dynamic_value_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (xr_semantic_yieldable_enum_result_is_exact(plan, operation))
        return true;
    if (operation && operation->opcode == XI_ATOMIC_RMW)
        return xr_semantic_atomic_compare_result_is_exact(plan, operation);
    if (operation && operation->opcode == XI_TUPLE_NEW)
        return xr_semantic_tuple_allocation_is_exact(plan, operation);
    if (operation && operation->opcode == XI_OBJECT_NEW)
        return xr_semantic_structural_allocation_is_exact(plan, operation);
    if (operation && operation->opcode == XI_VARIANT_PROJECT)
        return xr_semantic_optional_reference_project_is_exact(plan, operation);
    if (operation && operation->opcode == XI_SUM_INJECT)
        return xr_semantic_optional_reference_inject_is_exact(plan, operation);
    if (operation && operation->opcode == XI_LOAD_UPVAL)
        return xr_semantic_copied_capture_load_is_exact(plan, operation);
    if (operation && operation->opcode == XI_PLACE_LOAD)
        return xr_semantic_cleanup_reference_load_is_exact(plan, operation);
    if (operation && operation->opcode == XI_PHI)
        return xr_semantic_reference_phi_is_exact(plan, operation);
    if (operation && operation->opcode == XI_AS &&
        xr_semantic_class_instance_type_source_class(plan, type) != XR_SEMANTIC_INDEX_NONE)
        return xr_semantic_dynamic_source_class_as_is_exact(plan, operation);
    if (operation && operation->opcode == XI_RANGE)
        return xr_semantic_range_value_is_exact(plan, operation);
    if (!xr_semantic_dynamic_value_producer_is_exact(operation) ||
        !xr_semantic_dynamic_value_common_is_exact(plan, operation))
        return false;
    /* A module slot borrows the same tagged tuple root as construction and
     * direct-call results; reading it must not invent inline aggregate storage. */
    if (operation->opcode == XI_GET_SHARED && operation->semantic_immediate >= 0 &&
        xr_semantic_tagged_tuple_type_is_exact(plan, type))
        return true;
    /* A join, slot read, await or coroutine result is tagged because of the
     * carrier it propagates. The other producers manufacture the compiler's
     * untyped reference value and are held to that narrower type. */
    return (operation->opcode == XI_GET_SHARED || operation->opcode == XI_AWAIT ||
            operation->opcode == XI_CORO_OP)
               ? xr_semantic_dynamic_value_carrier_type_is_exact(type)
               : xr_semantic_dynamic_value_type_is_exact(type);
}

#endif /* XR_SEMANTIC_DYNAMIC_VALUE_SHAPE_H */
