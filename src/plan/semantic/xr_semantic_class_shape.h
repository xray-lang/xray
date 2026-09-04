/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_class_shape.h - Shared exactness judgement for source class objects
 *
 * KEY CONCEPT:
 *   A declared class lowers to one allocation whose result carries no class
 *   type: the value is typed `any`, so nothing in the type table names the
 *   declaration. The only authority that binds the allocation to a declaration
 *   is the plan's own source-class table matched by the operation's own class
 *   name. Every layer that has to answer "is this value a source class object"
 *   asks this one judgement, so the target builder, the target verifier and the
 *   AOT representation oracle cannot drift into three similar-looking rules.
 */

#ifndef XR_SEMANTIC_CLASS_SHAPE_H
#define XR_SEMANTIC_CLASS_SHAPE_H

#include "xr_semantic_plan.h"
#include "xr_semantic_type_admission_shape.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../ir/xi_own.h"
#include "../../runtime/value/xtype.h"
#include <string.h>

/* The class object value is a freshly owned module-level allocation. Its type
 * is the erased `any` reference, so the type row proves only that the value is
 * a reference-capable ownership root carrying no aggregate geometry. */
static inline bool xr_semantic_class_object_type_is_exact(const XrSemanticTypeRecord *type) {
    return type && type->kind == XR_KIND_UNKNOWN && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
}

/* The operation shape of a class allocation: no operands, generated effects,
 * flags and ownership, a fresh allocation identity, and at least the class name
 * in its metadata. Anything that deviates is not this family's to claim. */
static inline bool
xr_semantic_class_object_operation_is_exact(const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    return operation && operation->opcode == XI_CLASS_CREATE && operation->operand_count == 0 &&
           operation->metadata_count >= 1 && operation->allocation_key &&
           !xr_stable_id_equal(operation->allocation_id, zero) &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->auxiliary_kind == 0 && operation->semantic_immediate == 0 &&
           operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE &&
           operation->effects == xi_generated_op_effects(XI_CLASS_CREATE) &&
           operation->flags == xi_generated_op_default_flags(XI_CLASS_CREATE) &&
           operation->ownership_use == xi_generated_op_own_use(XI_CLASS_CREATE) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_OWNED &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           operation->view_complete == 0 && operation->view_source_operand == -1 &&
           operation->view_source_parameter == -1;
}

/* Whether the plan's source-class table names one frozen declaration at this
 * index. A generic skeleton or a monomorphized instantiation has no single
 * frozen object identity, and a row whose own ordinal disagrees with the index
 * it sits at names nothing, so neither is any caller's to claim. Every judgement
 * below asks this one question rather than restating it, so a declaration one
 * layer accepts cannot be a declaration another layer refuses. */
static inline bool xr_semantic_class_declaration_is_frozen(const XrSemanticPlan *plan,
                                                           uint32_t source_class) {
    if (!plan || source_class == XR_SEMANTIC_INDEX_NONE ||
        source_class >= (uint32_t) xr_semantic_plan_source_class_count(plan))
        return false;
    const XrSemanticSourceClassRecord *record = xr_semantic_plan_source_class(plan, source_class);
    return record && (record->flags & XR_SEM_SOURCE_CLASS_GENERIC) == 0 &&
           record->ordinal == source_class && record->canonical_key && record->module_path;
}

/* The declaration this allocation builds, or XR_SEMANTIC_INDEX_NONE when the
 * plan cannot name exactly one. The match is by class name because that is the
 * only class identity the operation retains; a name that names two declarations
 * or none names nothing, and the caller must refuse rather than guess. */
static inline uint32_t
xr_semantic_class_object_source_class(const XrSemanticPlan *plan,
                                      const XrSemanticOperationRecord *operation) {
    if (!plan || !xr_semantic_class_object_operation_is_exact(operation))
        return XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_class_object_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type)))
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!metadata || operation->metadata_begin >= metadata_count ||
        operation->metadata_count > metadata_count - operation->metadata_begin)
        return XR_SEMANTIC_INDEX_NONE;
    const char *name = metadata[operation->metadata_begin];
    if (!name || !name[0])
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t class_count = (uint32_t) xr_semantic_plan_source_class_count(plan);
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < class_count; i++) {
        const XrSemanticSourceClassRecord *record = xr_semantic_plan_source_class(plan, i);
        if (!record || !record->name || strcmp(record->name, name) != 0)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE)
            return XR_SEMANTIC_INDEX_NONE;
        match = i;
    }
    return xr_semantic_class_declaration_is_frozen(plan, match) ? match : XR_SEMANTIC_INDEX_NONE;
}

/* Whether this class object belongs to a generic template rather than to a
 * class instances can be of.
 *
 * A generic declaration and each of its specialisations both lower to a class
 * object, and both reach the plan. The specialisations are ordinary frozen
 * classes -- `Cell$i64` names one class, holds one layout, and every instance
 * of `Cell<int>` is of it. The template is not a class in that sense: nothing
 * is ever an instance of `Cell<T>` itself, and the storage family that binds a
 * class object has nothing to bind for it.
 *
 * The family therefore has to tell "this one is not mine" apart from "this one
 * should be mine and I cannot name it", which is the same distinction the
 * scalar family draws when it declines an address. Both go through the same
 * name lookup, so the two answers cannot disagree about which declaration is
 * behind a given class object. */
static inline bool
xr_semantic_class_object_is_generic_template(const XrSemanticPlan *plan,
                                             const XrSemanticOperationRecord *operation) {
    if (!plan || !xr_semantic_class_object_operation_is_exact(operation))
        return false;
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!metadata || operation->metadata_begin >= metadata_count)
        return false;
    const char *name = metadata[operation->metadata_begin];
    if (!name || !name[0])
        return false;
    uint32_t class_count = (uint32_t) xr_semantic_plan_source_class_count(plan);
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < class_count; i++) {
        const XrSemanticSourceClassRecord *record = xr_semantic_plan_source_class(plan, i);
        if (!record || !record->name || strcmp(record->name, name) != 0)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE)
            return false;
        match = i;
    }
    if (match == XR_SEMANTIC_INDEX_NONE)
        return false;
    const XrSemanticSourceClassRecord *record = xr_semantic_plan_source_class(plan, match);
    return record && (record->flags & XR_SEM_SOURCE_CLASS_GENERIC) != 0;
}

static inline bool xr_semantic_class_object_is_exact(const XrSemanticPlan *plan,
                                                     const XrSemanticOperationRecord *operation) {
    return xr_semantic_class_object_source_class(plan, operation) != XR_SEMANTIC_INDEX_NONE;
}

/* The declaration an instance type names, or XR_SEMANTIC_INDEX_NONE. Unlike the
 * class object, an instance keeps its declaration in the type row itself, so
 * the judgement is the row: the reference-capable ownership root that names one
 * frozen declaration and carries no aggregate, enum or scalar geometry. The
 * row's own class identity must agree with the declaration it indexes, so a
 * table whose index and identity disagree names nothing. */
static inline uint32_t
xr_semantic_class_instance_type_source_class(const XrSemanticPlan *plan,
                                             const XrSemanticTypeRecord *type) {
    if (!plan || !type || type->kind != XR_KIND_INSTANCE || type->builtin_type != XR_TID_NULL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->enum_member_count != 0 ||
        type->enum_flags != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        !xr_semantic_class_declaration_is_frozen(plan, type->source_class))
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticSourceClassRecord *record =
        xr_semantic_plan_source_class(plan, type->source_class);
    if (!record || !xr_stable_id_equal(type->source_class_identity, record->id))
        return XR_SEMANTIC_INDEX_NONE;
    return type->source_class;
}

/* The anonymous instance shape a constructor receiver carries. It is the
 * instance row stripped of its class identity: the frontend types `this` as a
 * bare instance that names no declaration, so this judgement proves only that
 * the row is a reference-capable ownership root with no aggregate, enum or
 * scalar geometry, and deliberately requires the class name to be absent. A row
 * that does name a declaration is the instance judgement's to answer, not this
 * one's, and a row carrying geometry is neither's. */
static inline bool
xr_semantic_class_anonymous_instance_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_INSTANCE && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
}

/* The declaration whose instance a declared receiver binds, or NONE. Every other
 * value in this family keeps its declaration in its own type row; a receiver
 * cannot, because the frontend types `this` as a bare instance naming no
 * declaration at all, so the type table has no answer to give. The authority is
 * the function's identity instead: a function the plan records as the
 * `source_kind` member of one frozen declaration receives that declaration's
 * instance, and the receiver is the parameter its own parameter range starts
 * with rather than any parameter that merely claims ordinal zero. The row must
 * still be the anonymous instance shape, so a member whose receiver carries
 * geometry or already names a class stays outside this family.
 *
 * The two member kinds differ only in what the plan records on the parameter: a
 * constructor receives the instance it is building and carries no borrow
 * annotation, while an instance method receives a borrowed receiver and the
 * plan states that borrow on the parameter itself. The annotation is admitted
 * only where the member kind can carry it, and only when the ownership it
 * claims is the ownership the plan recorded, so the flag can never widen the
 * ownership fact it is supposed to restate. */
static inline uint32_t xr_semantic_class_declared_receiver_source_class(const XrSemanticPlan *plan,
                                                                        uint32_t parameter_index,
                                                                        uint8_t source_kind) {
    if (!plan || parameter_index == XR_SEMANTIC_INDEX_NONE ||
        parameter_index >= (uint32_t) xr_semantic_plan_parameter_count(plan))
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, parameter_index);
    uint8_t receiver_flags = (uint8_t) (XR_SEM_PARAMETER_REQUIRED |
                                        (source_kind == XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD
                                             ? XR_SEM_PARAMETER_RECEIVER_BORROWED
                                             : 0));
    if (!parameter || parameter->value == XR_SEMANTIC_INDEX_NONE || parameter->ordinal != 0 ||
        !xr_param_mode_is_valid((XrParamMode) parameter->mode) ||
        (source_kind == XR_SEM_SOURCE_FUNCTION_CONSTRUCTOR ? parameter->mode != XR_PARAM_READ
                                                           : false) ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->flags & ~receiver_flags) != 0 || parameter->reserved != 0 ||
        parameter->function >= (uint32_t) xr_semantic_plan_function_count(plan))
        return XR_SEMANTIC_INDEX_NONE;
    /* The receiver is bound by reference either way, but which of the two the
     * plan recorded is the plan's fact to state, not this judgement's to pick. */
    bool borrowed = (parameter->flags & XR_SEM_PARAMETER_RECEIVER_BORROWED) != 0;
    if (source_kind == XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD) {
        if (parameter->mode == XR_PARAM_MOVE) {
            if (borrowed || parameter->ownership != XI_OWN_OWNED)
                return XR_SEMANTIC_INDEX_NONE;
        } else if (!borrowed || parameter->ownership != XI_OWN_BORROWED) {
            return XR_SEMANTIC_INDEX_NONE;
        }
    } else {
        /* Constructor lowering does not publish a method receiver contract:
         * parameter zero is the freshly allocated instance carrier. Depending
         * on where allocation ownership is frozen, that carrier may already be
         * borrowed or still owned. A borrowed marker, when present, must agree
         * with the ownership row; otherwise either exact ownership is valid. */
        if (parameter->ownership != XI_OWN_OWNED && parameter->ownership != XI_OWN_BORROWED)
            return XR_SEMANTIC_INDEX_NONE;
        if (borrowed && parameter->ownership != XI_OWN_BORROWED)
            return XR_SEMANTIC_INDEX_NONE;
    }
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, parameter->function);
    /* Parameter zero of the function is the one its range starts with. Trusting
     * the ordinal alone would let a parameter belonging to another function's
     * range answer for this one. */
    if (!function || function->source_kind != source_kind || function->parameter_count == 0 ||
        function->parameter_begin != parameter_index ||
        !xr_semantic_class_declaration_is_frozen(plan, function->source_class))
        return XR_SEMANTIC_INDEX_NONE;
    /* A receiver row may name its own declaration, and when it does that is a
     * stronger statement than the anonymous shape, not a weaker one: the type
     * table and the function identity then agree about which declaration is
     * being received.  Requiring anonymity alone refused every declared member,
     * because the front end does name `this` -- the row carries the class and
     * its source-class identity.  A receiver naming some *other* declaration is
     * still refused, which is the case the anonymous requirement was guarding
     * against. */
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, parameter->type);
    if (!xr_semantic_class_anonymous_instance_type_is_exact(receiver_type) &&
        xr_semantic_class_instance_type_source_class(plan, receiver_type) != function->source_class)
        return XR_SEMANTIC_INDEX_NONE;
    return function->source_class;
}

/* The declaration whose instance a constructor receives, or NONE. */
static inline uint32_t
xr_semantic_class_constructor_receiver_source_class(const XrSemanticPlan *plan,
                                                    uint32_t parameter_index) {
    return xr_semantic_class_declared_receiver_source_class(plan, parameter_index,
                                                            XR_SEM_SOURCE_FUNCTION_CONSTRUCTOR);
}

/* The declaration whose instance an instance method receives, or NONE. */
static inline uint32_t xr_semantic_class_method_receiver_source_class(const XrSemanticPlan *plan,
                                                                      uint32_t parameter_index) {
    return xr_semantic_class_declared_receiver_source_class(plan, parameter_index,
                                                            XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD);
}

/* The declaration whose instance an ordinary parameter binds, or NONE. Unlike a
 * receiver, such a parameter is written in the source with the class as its
 * declared type, so its own type row names the declaration and the function's
 * identity says nothing about it. The parameter may either take the owning
 * reference or borrow it; both bind the same tagged carrier, and the call-site
 * judgement below proves the matching consume or borrow action. The parameter
 * must also sit inside its own function's range at the ordinal that range gives
 * it, so a row belonging to another function can never answer for this one. */
static inline uint32_t xr_semantic_class_argument_source_class(const XrSemanticPlan *plan,
                                                               uint32_t parameter_index) {
    if (!plan || parameter_index == XR_SEMANTIC_INDEX_NONE ||
        parameter_index >= (uint32_t) xr_semantic_plan_parameter_count(plan))
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, parameter_index);
    if (!parameter || parameter->value == XR_SEMANTIC_INDEX_NONE ||
        parameter->mode != XR_PARAM_READ || parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->ownership != XI_OWN_OWNED && parameter->ownership != XI_OWN_BORROWED) ||
        (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 || parameter->reserved != 0 ||
        parameter->function >= (uint32_t) xr_semantic_plan_function_count(plan))
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, parameter->function);
    if (!function || function->parameter_count == 0 ||
        parameter_index < function->parameter_begin ||
        parameter_index - function->parameter_begin >= function->parameter_count ||
        parameter->ordinal != parameter_index - function->parameter_begin)
        return XR_SEMANTIC_INDEX_NONE;
    /* Parameter zero of a declared member is owned by the receiver judgement,
     * even when its type row also names the declaration. Later parameters of
     * that member remain ordinary arguments. */
    if (parameter_index == function->parameter_begin &&
        (function->source_kind == XR_SEM_SOURCE_FUNCTION_CONSTRUCTOR ||
         function->source_kind == XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD))
        return XR_SEMANTIC_INDEX_NONE;
    return xr_semantic_class_instance_type_source_class(
        plan, xr_semantic_plan_type(plan, parameter->type));
}

/* The class-level conditions for a method call to name one body, and which
 * call-target kind that binding takes.
 *
 * A runtime type and a non-generic declaration are what make a body nameable at
 * all. Being final is a different question: it says no subclass can exist
 * anywhere, so the binding stands without seeing the whole module graph. A
 * class that is not final can still name one body -- it just names it under an
 * obligation the graph-holding layer has to discharge. So final does not gate
 * the binding; it selects between stating a conclusion and stating an
 * obligation. Both the builder and the verifier ask here, or one would refuse
 * rows the other produces. */
static inline bool xr_semantic_source_class_can_name_one_method(uint8_t flags) {
    return (flags & XR_SEM_SOURCE_CLASS_RUNTIME_TYPE) != 0 &&
           (flags & XR_SEM_SOURCE_CLASS_GENERIC) == 0;
}

/* A generic declaration cannot freeze one runtime class-instance identity,
 * but its method body can still name another method body when the call's
 * receiver is proven separately to be the caller's exact self parameter. This
 * predicate deliberately answers only the declaration half of that proof;
 * builder and verifier must also establish exact self, selector and arity. */
static inline bool xr_semantic_source_class_can_name_template_method(uint8_t flags) {
    return (flags & XR_SEM_SOURCE_CLASS_GENERIC) != 0;
}

static inline uint16_t xr_semantic_source_instance_method_call_kind(uint8_t flags) {
    return (flags & XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL) != 0
               ? (uint16_t) XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL
               : (uint16_t) XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_SEALED_CANDIDATE;
}

/* One judgement for every way a class instance crosses a parameter boundary:
 * the receiver a constructor builds, the receiver an instance method borrows,
 * and an ordinary parameter declared with the class as its type. All three bind
 * the same outer tagged value for the same reason the construction result does,
 * so the layers that only need to know "is this parameter a proved class
 * instance" ask this one question instead of restating the three. The three are
 * disjoint by construction: a declared member's first parameter belongs to a
 * receiver judgement, while the ordinary-argument judgement excludes that
 * position even when the receiver's type row names its declaration. */
static inline uint32_t xr_semantic_class_instance_parameter_source_class(const XrSemanticPlan *plan,
                                                                         uint32_t parameter_index) {
    uint32_t source_class =
        xr_semantic_class_constructor_receiver_source_class(plan, parameter_index);
    if (source_class != XR_SEMANTIC_INDEX_NONE)
        return source_class;
    source_class = xr_semantic_class_method_receiver_source_class(plan, parameter_index);
    if (source_class != XR_SEMANTIC_INDEX_NONE)
        return source_class;
    return xr_semantic_class_argument_source_class(plan, parameter_index);
}

/* A class parameter's ownership declaration and the call operand's action are
 * one transfer fact. An owned parameter receives the caller's reference and
 * therefore requires CONSUME; a borrowed parameter leaves responsibility with
 * the caller and therefore requires BORROW. Both use the shared READ carrier,
 * so a ref/writeback spelling cannot be admitted by an ownership match alone.
 * The class-instance judgement is part of this question: an ordinary scalar
 * with the same ownership numbers must not enter the class storage family. */
static inline bool xr_semantic_class_parameter_call_transfer_is_exact(
    const XrSemanticPlan *plan, uint32_t parameter_index, const XrSemanticOperandRecord *operand) {
    const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, parameter_index);
    if (!parameter || !operand ||
        xr_semantic_class_instance_parameter_source_class(plan, parameter_index) ==
            XR_SEMANTIC_INDEX_NONE ||
        parameter->mode != operand->parameter_mode ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        operand->transfer_mode != XR_TRANSFER_SHARE || operand->origin != XI_PLACE_ORIGIN_NONE ||
        operand->lifetime != XI_PLACE_LIFETIME_NONE || operand->escape != XI_PLACE_ESCAPE_NONE ||
        operand->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    bool method_receiver = xr_semantic_class_method_receiver_source_class(plan, parameter_index) !=
                           XR_SEMANTIC_INDEX_NONE;
    if (!method_receiver &&
        (parameter->mode != XR_PARAM_READ || operand->access != XR_CALL_ARG_PLAIN))
        return false;
    if (method_receiver && parameter->mode != XR_PARAM_MOVE && operand->access != XR_CALL_ARG_PLAIN)
        return false;
    if (method_receiver && parameter->mode == XR_PARAM_MOVE &&
        operand->access != XR_CALL_ARG_PLAIN && operand->access != XR_CALL_ARG_MOVE)
        return false;
    return (parameter->ownership == XI_OWN_OWNED &&
            operand->ownership_action == XR_SEM_OPERAND_CONSUME) ||
           (parameter->ownership == XI_OWN_BORROWED &&
            operand->ownership_action == XR_SEM_OPERAND_BORROW);
}

/* The one function the plan records as the constructor of `source_class`, or
 * NONE when the plan records none or more than one. A declaration that declares
 * no constructor has none, and that is not a failure: its construction then
 * carries no argument at all. Two constructors for one declaration name no
 * single body, so the caller must refuse rather than pick. */
static inline uint32_t xr_semantic_class_constructor_function(const XrSemanticPlan *plan,
                                                              uint32_t source_class) {
    if (!xr_semantic_class_declaration_is_frozen(plan, source_class))
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(plan);
    uint32_t found = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < function_count; i++) {
        const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, i);
        if (!function || function->source_kind != XR_SEM_SOURCE_FUNCTION_CONSTRUCTOR ||
            function->source_class != source_class)
            continue;
        if (found != XR_SEMANTIC_INDEX_NONE)
            return XR_SEMANTIC_INDEX_NONE;
        found = i;
    }
    return found;
}

/* A constructor call may omit only a trailing suffix of parameters that the
 * declaration marks optional. The supplied arguments are checked separately;
 * this proves that accepting fewer than the full arity does not invent a
 * default or skip a required parameter. */
static inline bool
xr_semantic_class_constructor_arity_is_exact(const XrSemanticPlan *plan, uint32_t constructor,
                                             const XrSemanticFunctionRecord *function,
                                             uint16_t supplied_arguments) {
    if (!plan || !function || function->parameter_count == 0 ||
        supplied_arguments > function->parameter_count - 1u)
        return false;
    for (uint16_t ordinal = (uint16_t) (supplied_arguments + 1u);
         ordinal < function->parameter_count; ordinal++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, function->parameter_begin + ordinal);
        if (!parameter || parameter->value == XR_SEMANTIC_INDEX_NONE ||
            parameter->function != constructor || parameter->ordinal != ordinal ||
            parameter->mode != XR_PARAM_READ ||
            (parameter->ownership != XI_OWN_NONE && parameter->ownership != XI_OWN_OWNED &&
             parameter->ownership != XI_OWN_BORROWED) ||
            parameter->reserved != 0 || parameter->flags != 0)
            return false;
    }
    return true;
}

/* The one parameter in the module bound to `value`, or NONE when the module
 * binds it zero times or more than once. A value two parameters claim carries
 * no single receiver identity, so it names nothing. */
static inline uint32_t xr_semantic_class_parameter_for_value(const XrSemanticPlan *plan,
                                                             uint32_t value) {
    if (!plan || value == XR_SEMANTIC_INDEX_NONE)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(plan);
    uint32_t found = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, i);
        if (!parameter || parameter->value != value)
            continue;
        if (found != XR_SEMANTIC_INDEX_NONE)
            return XR_SEMANTIC_INDEX_NONE;
        found = i;
    }
    return found;
}

/* The declaration whose instance a receiver operand names when the operand
 * reads a parameter, or NONE when it reads anything else. Every field access
 * inside a class member or inside a function that takes an instance reaches its
 * object this way, so the read, the write and the storage authority all ask
 * this one question rather than each restating which parameter shapes count.
 * The operand must read the parameter as its own function's binding and with
 * the very type row the parameter carries, so a row belonging to another
 * function or to another type can never answer for this access. */
static inline uint32_t
xr_semantic_class_receiver_parameter_source_class(const XrSemanticPlan *plan, uint32_t function,
                                                  const XrSemanticOperandRecord *receiver) {
    if (!plan || !receiver)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t parameter_index = xr_semantic_class_parameter_for_value(plan, receiver->value);
    const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, parameter_index);
    uint32_t source_class =
        xr_semantic_class_instance_parameter_source_class(plan, parameter_index);
    if (source_class == XR_SEMANTIC_INDEX_NONE || !parameter || parameter->type != receiver->type ||
        parameter->function != function)
        return XR_SEMANTIC_INDEX_NONE;
    return source_class;
}

/* The declaration whose instance a field store writes through, or NONE. The
 * store is the generated field write: a borrowed receiver, one consumed value
 * and one metadata name, with no allocation, constant, callee or view of its
 * own. Which field the name selects is the frontend's proof; what this
 * judgement adds is that the receiver is an instance this family named through
 * a parameter, so the write lands in a proved allocation rather than in an open
 * object. A store through a computed receiver stays outside the family: the
 * write would then have to be accounted for against an allocation this
 * judgement did not follow to its parameter. The stored value's own storage is
 * deliberately not this judgement's question: a field whose type has no storage
 * row must still be refused by the caller that asks for it. */
static inline uint32_t
xr_semantic_class_field_store_source_class(const XrSemanticPlan *plan,
                                           const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    if (!plan || xr_semantic_plan_source_class_count(plan) == 0 || !operation ||
        operation->opcode != XI_STORE_FIELD || operation->operand_count != 2 ||
        operation->metadata_count != 1 || operation->semantic_immediate < 0 ||
        operation->allocation_key || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_STORE_FIELD) ||
        operation->flags != xi_generated_op_default_flags(XI_STORE_FIELD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_STORE_FIELD) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_STORE_FIELD) ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->view_complete != 0 ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count ||
        operand_count - operation->operand_begin < 2)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *stored = &operands[operation->operand_begin + 1];
    if (receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->transfer_mode != 0 || receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != 0 || receiver->access != 0 || receiver->origin != 0 ||
        receiver->lifetime != 0 || receiver->escape != 0 || receiver->flags != 0)
        return XR_SEMANTIC_INDEX_NONE;
    if (stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
        stored->transfer_mode != 0 || stored->ownership_action != XR_SEM_OPERAND_CONSUME ||
        stored->parameter_mode != 0 || stored->access != 0 || stored->origin != 0 ||
        stored->lifetime != 0 || stored->escape != 0 || stored->flags != 0)
        return XR_SEMANTIC_INDEX_NONE;
    /* The store must run inside the very function that binds the instance; a
     * store reading another function's parameter names nothing. */
    return xr_semantic_class_receiver_parameter_source_class(plan, operation->function, receiver);
}

/* The generated shape of a module-level shared load: a borrowed static read of
 * one slot with no operand, metadata, constant, callee or allocation of its
 * own. Which slot it reads is the caller's question; that it is nothing but a
 * shared read is this judgement's. */
static inline bool
xr_semantic_class_shared_read_shape_is_exact(const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    return operation && operation->opcode == XI_GET_SHARED && operation->operand_count == 0 &&
           operation->metadata_count == 0 && operation->semantic_immediate >= 0 &&
           operation->semantic_immediate <= UINT16_MAX && !operation->allocation_key &&
           xr_stable_id_equal(operation->allocation_id, zero) &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->auxiliary_kind == 0 && operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE &&
           operation->effects == xi_generated_op_effects(XI_GET_SHARED) &&
           operation->flags == xi_generated_op_default_flags(XI_GET_SHARED) &&
           operation->ownership_use == xi_generated_op_own_use(XI_GET_SHARED) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
           operation->result_ownership == xi_generated_op_result_ownership(XI_GET_SHARED) &&
           operation->transfer_mode == 0 && operation->parameter_mode == 0 &&
           operation->parameter_ownership == 0 && operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           operation->view_complete == 0 && operation->view_source_operand == -1 &&
           operation->view_source_parameter == -1;
}

/* The generated shape of the store that fills a class shared slot: one consumed
 * plain value and no result of its own. A non-exported class carries no store
 * metadata. An exported class carries the exact source-class export annotation
 * that the explicit source-export table independently freezes; admitting that
 * one schema row keeps the class-object proof aligned with the producer without
 * accepting arbitrary legacy annotations. */
static inline bool
xr_semantic_class_shared_store_shape_is_exact(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    if (!plan || !operation)
        return false;
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    bool metadata_is_exact = operation->metadata_count == 0;
    if (operation->metadata_count == 2 && metadata && operation->metadata_begin < metadata_count &&
        metadata_count - operation->metadata_begin >= 2) {
        const char *tag = metadata[operation->metadata_begin];
        const char *name = metadata[operation->metadata_begin + 1u];
        metadata_is_exact = tag && name && name[0] && strcmp(tag, "source-class-export-v1") == 0;
    }
    if (operation->opcode != XI_SET_SHARED || operation->operand_count != 1 || !metadata_is_exact ||
        operation->semantic_immediate < 0 || operation->semantic_immediate > UINT16_MAX ||
        operation->allocation_key || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_SET_SHARED) ||
        operation->flags != xi_generated_op_default_flags(XI_SET_SHARED) ||
        operation->ownership_use != xi_generated_op_own_use(XI_SET_SHARED) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_SET_SHARED) ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->view_complete != 0 ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count)
        return false;
    const XrSemanticOperandRecord *stored = &operands[operation->operand_begin];
    return stored->role == XR_SEM_OPERAND_VALUE && stored->parameter == -1 &&
           stored->transfer_mode == 0 && stored->ownership_action == XR_SEM_OPERAND_CONSUME &&
           stored->parameter_mode == 0 && stored->access == 0 && stored->origin == 0 &&
           stored->lifetime == 0 && stored->escape == 0 && stored->flags == 0;
}

/* The single function a direct-local call target names for `operation`, or NULL
 * when the plan names none or more than one.
 *
 * The operation is given as a record rather than an index because every caller
 * in this header holds one, and the index it needs is recovered by the same
 * identity scan the rest of the header uses. Keeping the lookup here is what
 * lets the judgements below ask about a call's callee without each of their
 * four callers having to supply it, which is how the same question ends up
 * answered differently in different layers. */
static inline const XrSemanticFunctionRecord *
xr_semantic_class_direct_local_callee(const XrSemanticPlan *plan,
                                      const XrSemanticOperationRecord *operation) {
    if (!plan || !operation)
        return NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < operation_count; i++) {
        if (xr_semantic_plan_operation(plan, i) != operation)
            continue;
        if (operation_index != XR_SEMANTIC_INDEX_NONE)
            return NULL;
        operation_index = i;
    }
    if (operation_index == XR_SEMANTIC_INDEX_NONE)
        return NULL;
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(plan);
    const XrSemanticFunctionRecord *callee = NULL;
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, i);
        if (!target || target->operation != operation_index ||
            target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (callee)
            return NULL;
        callee = xr_semantic_plan_function(plan, target->function);
        if (!callee)
            return NULL;
    }
    return callee;
}

/* The one operation in the module whose result is `value`, or NULL when the
 * module defines it zero times or more than once. */
static inline const XrSemanticOperationRecord *
xr_semantic_class_value_definition(const XrSemanticPlan *plan, uint32_t value) {
    if (!plan || value == XR_SEMANTIC_INDEX_NONE)
        return NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    const XrSemanticOperationRecord *found = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != value)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

/* Shared slot numbers are lexical-owner local. Resolve a class-object read from
 * its own function outward: a same-function binding shadows the module binding,
 * while an absent local binding may reach only the root initializer. A root
 * store must precede every activation boundary; a local store must precede the
 * read in the same block. The latter is deliberately fail-closed for cross-
 * block locals until SemanticPlan freezes a reusable dominance authority. */
static inline bool
xr_semantic_class_operation_can_activate(const XrSemanticOperationRecord *operation) {
    return operation &&
           (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL ||
            operation->opcode == XI_CALL_METHOD || operation->opcode == XI_CALL_METHOD_DIRECT ||
            operation->opcode == XI_CALL_BUILTIN || operation->opcode == XI_GO);
}

static inline const XrSemanticOperationRecord *
xr_semantic_class_shared_read_store(const XrSemanticPlan *plan,
                                    const XrSemanticOperationRecord *load) {
    const XrSemanticFunctionRecord *root = xr_semantic_plan_function(plan, 0);
    if (!plan || !load || !root || root->parent != XR_SEMANTIC_INDEX_NONE ||
        !root->is_module_initializer || root->block_count == 0 ||
        load->function >= xr_semantic_plan_function_count(plan))
        return NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    uint32_t load_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < operation_count; i++)
        if (xr_semantic_plan_operation(plan, i) == load) {
            if (load_index != XR_SEMANTIC_INDEX_NONE)
                return NULL;
            load_index = i;
        }
    if (load_index == XR_SEMANTIC_INDEX_NONE)
        return NULL;
    uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(plan);
    for (uint32_t owner = load->function, depth = 0;
         owner != XR_SEMANTIC_INDEX_NONE && depth < function_count; depth++) {
        const XrSemanticFunctionRecord *owner_function = xr_semantic_plan_function(plan, owner);
        if (!owner_function)
            return NULL;
        const XrSemanticOperationRecord *found = NULL;
        uint32_t found_index = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t i = 0; i < operation_count; i++) {
            const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
            if (!candidate || candidate->function != owner || candidate->opcode != XI_SET_SHARED ||
                candidate->semantic_immediate != load->semantic_immediate)
                continue;
            if (found)
                return NULL;
            found = candidate;
            found_index = i;
        }
        if (!found) {
            owner = owner_function->parent;
            continue;
        }
        if (!xr_semantic_class_shared_store_shape_is_exact(plan, found))
            return NULL;
        if (owner == load->function && found->block == load->block && found_index < load_index)
            return found;
        if (owner != 0)
            return NULL;
        const XrSemanticBlockRecord *entry = xr_semantic_plan_block(plan, root->block_begin);
        if (!entry || found->block != root->block_begin || found_index < entry->operation_begin ||
            found_index >= entry->operation_begin + entry->operation_count)
            return NULL;
        for (uint32_t i = entry->operation_begin; i < found_index; i++)
            if (xr_semantic_class_operation_can_activate(xr_semantic_plan_operation(plan, i)))
                return NULL;
        return found;
    }
    return NULL;
}

/* The value a shared read loads, proved through the module's single store into
 * the same slot, together with the store's own operand row. */
static inline const XrSemanticOperationRecord *
xr_semantic_class_shared_read_definition(const XrSemanticPlan *plan,
                                         const XrSemanticOperationRecord *operation) {
    if (!xr_semantic_class_shared_read_shape_is_exact(operation))
        return NULL;
    const XrSemanticOperationRecord *store = xr_semantic_class_shared_read_store(plan, operation);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        store ? xr_semantic_plan_operands(plan, &operand_count) : NULL;
    if (!store || !operands || store->operand_begin >= operand_count)
        return NULL;
    const XrSemanticOperandRecord *stored = &operands[store->operand_begin];
    /* The slot carries one type: the store's operand row and the read's result
     * must agree, or the read is not reading what the store wrote. */
    if (stored->type != operation->result_type)
        return NULL;
    const XrSemanticOperationRecord *definition =
        xr_semantic_class_value_definition(plan, stored->value);
    return definition && definition->result_value == stored->value &&
                   definition->result_type == stored->type
               ? definition
               : NULL;
}

/* The declaration whose class object this shared read loads, or NONE. */
static inline uint32_t
xr_semantic_class_object_read_source_class(const XrSemanticPlan *plan,
                                           const XrSemanticOperationRecord *operation) {
    if (!plan || xr_semantic_plan_source_class_count(plan) == 0 || !operation ||
        operation->opcode != XI_GET_SHARED ||
        !xr_semantic_class_object_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type)))
        return XR_SEMANTIC_INDEX_NONE;
    return xr_semantic_class_object_source_class(
        plan, xr_semantic_class_shared_read_definition(plan, operation));
}

/* The operation facts common to local and imported source-class construction.
 * Import resolution is deliberately not part of this predicate: local
 * construction proves a local class-object load, while imported construction
 * proves a source import and a dependency export. Sharing only the generated
 * call facts prevents either route from widening the other. */
static inline bool
xr_semantic_class_construction_operation_is_exact(const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    return operation && operation->opcode == XI_CALL && operation->operand_count >= 1 &&
           operation->metadata_count == 0 && operation->semantic_immediate == 0 &&
           !operation->allocation_key && xr_stable_id_equal(operation->allocation_id, zero) &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->auxiliary_kind == 0 && operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE &&
           operation->effects == xi_generated_op_effects(XI_CALL) &&
           (uint8_t) (operation->flags & (uint8_t) ~XI_FLAG_SPEC_CONST) ==
               xi_generated_op_default_flags(XI_CALL) &&
           operation->ownership_use == xi_generated_op_own_use(XI_CALL) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           operation->transfer_mode == 0 && operation->parameter_mode == 0 &&
           operation->parameter_ownership == 0 && operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_OWNED &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           operation->view_complete == 0 && operation->view_source_operand == -1 &&
           operation->view_source_parameter == -1;
}

/* The declaration a construction call builds, or NONE. The call names no callee
 * function: what it constructs is proved from the instance type it returns and
 * from the class object its callee operand loads, and the two must name the
 * same declaration. Any argument it carries is proved against the declaration's
 * own constructor, so the arity and the contract of a construction can never be
 * taken on the call's word alone. */
static inline uint32_t
xr_semantic_class_construction_source_class(const XrSemanticPlan *plan,
                                            const XrSemanticOperationRecord *operation) {
    if (!plan || xr_semantic_plan_source_class_count(plan) == 0 || !operation ||
        !xr_semantic_class_construction_operation_is_exact(operation))
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t source_class = xr_semantic_class_instance_type_source_class(
        plan, xr_semantic_plan_type(plan, operation->result_type));
    if (source_class == XR_SEMANTIC_INDEX_NONE)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count ||
        operand_count - operation->operand_begin < operation->operand_count)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperandRecord *callee = &operands[operation->operand_begin];
    if (callee->role != XR_SEM_OPERAND_CALLEE || callee->parameter != -1 ||
        callee->transfer_mode != 0 || callee->ownership_action != XR_SEM_OPERAND_BORROW ||
        callee->parameter_mode != 0 || callee->access != 0 || callee->origin != 0 ||
        callee->lifetime != 0 || callee->escape != 0 || callee->flags != 0)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *load = xr_semantic_class_value_definition(plan, callee->value);
    if (!load || load->result_type != callee->type ||
        xr_semantic_class_object_read_source_class(plan, load) != source_class)
        return XR_SEMANTIC_INDEX_NONE;
    /* The arguments the construction passes. A declaration that declares no
     * constructor takes none. One that declares a constructor takes exactly the
     * parameters that follow its receiver, matched one for one and in order:
     * the receiver is not among them, because the construction supplies it
     * itself, so argument ordinal zero binds the constructor's parameter
     * ordinal one. Ownership is exact: trivial and owned parameters consume
     * their argument, while borrowed parameters borrow it. */
    uint16_t argument_count = (uint16_t) (operation->operand_count - 1u);
    uint32_t constructor = xr_semantic_class_constructor_function(plan, source_class);
    const XrSemanticFunctionRecord *function =
        constructor != XR_SEMANTIC_INDEX_NONE ? xr_semantic_plan_function(plan, constructor) : NULL;
    if (!function)
        return argument_count == 0 ? source_class : XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_class_constructor_arity_is_exact(plan, constructor, function,
                                                      argument_count) ||
        xr_semantic_class_constructor_receiver_source_class(plan, function->parameter_begin) !=
            source_class)
        return XR_SEMANTIC_INDEX_NONE;
    for (uint16_t i = 0; i < argument_count; i++) {
        const XrSemanticOperandRecord *argument = &operands[operation->operand_begin + 1u + i];
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, function->parameter_begin + 1u + i);
        const XrSemanticTypeRecord *argument_type = xr_semantic_plan_type(plan, argument->type);
        const XrSemanticTypeRecord *parameter_type =
            parameter ? xr_semantic_plan_type(plan, parameter->type) : NULL;
        if (!parameter || !argument_type || !parameter_type ||
            parameter->value == XR_SEMANTIC_INDEX_NONE || parameter->function != constructor ||
            parameter->ordinal != i + 1u || parameter->mode != XR_PARAM_READ ||
            (parameter->ownership != XI_OWN_NONE && parameter->ownership != XI_OWN_OWNED &&
             parameter->ownership != XI_OWN_BORROWED) ||
            parameter->reserved != 0 || (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 ||
            argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) i ||
            !xr_semantic_parameter_type_admits_argument(plan, parameter_type, argument_type,
                                                        parameter->mode) ||
            argument->parameter_mode != parameter->mode ||
            argument->transfer_mode != parameter->transfer_mode ||
            argument->access != XR_CALL_ARG_PLAIN || argument->origin != 0 ||
            argument->lifetime != 0 || argument->escape != 0 ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            (parameter->ownership == XI_OWN_BORROWED
                 ? argument->ownership_action != XR_SEM_OPERAND_BORROW
                 : argument->ownership_action != XR_SEM_OPERAND_CONSUME))
            return XR_SEMANTIC_INDEX_NONE;
    }
    return source_class;
}

/* An instance type imported from another source module carries the declaration
 * identity but cannot index the caller's local source-class table. This is the
 * exact external counterpart of xr_semantic_class_instance_type_source_class. */
static inline bool
xr_semantic_external_class_instance_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_INSTANCE && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           !xr_stable_id_equal(type->source_class_identity, zero) &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
}

/* The declaration frozen by one class-export row. The semantic builder and
 * verifier have already proved the root initializer independently; downstream
 * module-set, Target and AOT consumers use only this explicit authority. */
static inline uint32_t
xr_semantic_source_class_export_source_class(const XrSemanticPlan *plan,
                                             const XrSemanticSourceExportRecord *source_export) {
    if (!plan || !source_export || source_export->kind != XR_SEM_SOURCE_EXPORT_SOURCE_CLASS ||
        source_export->function != XR_SEMANTIC_INDEX_NONE || !source_export->name ||
        !source_export->name[0] || source_export->reserved[0] != 0 ||
        source_export->reserved[1] != 0 || source_export->reserved[2] != 0 ||
        !xr_semantic_class_declaration_is_frozen(plan, source_export->source_class))
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticSourceClassRecord *record =
        xr_semantic_plan_source_class(plan, source_export->source_class);
    return record && record->name && record->name[0] &&
                   xr_stable_id_equal(record->id, source_export->exported_entity)
               ? source_export->source_class
               : XR_SEMANTIC_INDEX_NONE;
}

/* The source import behind the class-object callee. It is not a local shared
 * class read: the shared slot is only the caller's import binding, and this
 * proof must reach the root SOURCE_MODULE import row with its non-empty member. */
static inline bool
xr_semantic_imported_class_callee_is_exact(const XrSemanticPlan *plan,
                                           const XrSemanticOperationRecord *operation,
                                           const char **module_path, const char **member) {
    if (!plan || !xr_semantic_class_construction_operation_is_exact(operation))
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    const XrSemanticOperandRecord *callee = &operands[operation->operand_begin];
    if (callee->role != XR_SEM_OPERAND_CALLEE || callee->parameter != -1 ||
        callee->transfer_mode != 0 || callee->ownership_action != XR_SEM_OPERAND_BORROW ||
        callee->parameter_mode != 0 || callee->access != 0 || callee->origin != 0 ||
        callee->lifetime != 0 || callee->escape != 0 || callee->flags != 0)
        return false;
    const XrSemanticOperationRecord *load = xr_semantic_class_value_definition(plan, callee->value);
    if (!load || load->result_type != callee->type ||
        !xr_semantic_class_shared_read_shape_is_exact(load))
        return false;
    const XrSemanticOperationRecord *store = xr_semantic_class_shared_read_store(plan, load);
    if (!store || store->function != 0 || store->operand_count != 1 ||
        store->operand_begin >= operand_count)
        return false;
    uint32_t value = operands[store->operand_begin].value;
    const XrSemanticOperationRecord *import = NULL;
    for (uint32_t depth = 0; depth < (uint32_t) xr_semantic_plan_operation_count(plan); depth++) {
        import = xr_semantic_class_value_definition(plan, value);
        if (!import || import->function != 0)
            return false;
        if (import->opcode != XI_COPY || import->semantic_immediate != XI_COPY_KIND_IDENTITY ||
            import->operand_count != 1 || import->result_alias_operand != 0)
            break;
        if (import->operand_begin >= operand_count)
            return false;
        value = operands[import->operand_begin].value;
    }
    XrStableId zero = {{0}};
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *import_type =
        import ? xr_semantic_plan_type(plan, import->result_type) : NULL;
    if (!import || !metadata || !import_type || import->opcode != XI_IMPORT_REF ||
        import->operand_count != 0 || import->metadata_count != 2 ||
        import->metadata_begin + 1u >= metadata_count ||
        import->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
        import->semantic_immediate < -1 || import->semantic_immediate > UINT16_MAX ||
        import->allocation_key || !xr_stable_id_equal(import->allocation_id, zero) ||
        import->constant != XR_SEMANTIC_INDEX_NONE ||
        import->callable_function != XR_SEMANTIC_INDEX_NONE || import->auxiliary_kind != 0 ||
        import->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        import->effects != xi_generated_op_effects(XI_IMPORT_REF) ||
        import->flags != xi_generated_op_default_flags(XI_IMPORT_REF) ||
        import->ownership_use != xi_generated_op_own_use(XI_IMPORT_REF) ||
        import->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        import->result_alias_operand != -1 ||
        import->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        import->return_parameter != -1 || import->return_complete != 1 ||
        !xr_semantic_class_object_type_is_exact(import_type) ||
        import->result_type != load->result_type)
        return false;
    const char *path = metadata[import->metadata_begin];
    const char *name = metadata[import->metadata_begin + 1u];
    if (!path || !path[0] || !name || !name[0])
        return false;
    if (module_path)
        *module_path = path;
    if (member)
        *member = name;
    return true;
}

/* The exact dependency declaration frozen by imported-constructor authority.
 * This mechanical form consumes only explicit plan rows: it deliberately does
 * not rediscover the import by walking the caller operation graph. Semantic's
 * module-set verifier performs that independent graph proof before downstream
 * Target and AOT are allowed to consume this form. */
static inline uint32_t xr_semantic_imported_class_construction_authority_source_class(
    const XrSemanticPlan *caller, const XrSemanticPlan *dependency,
    const XrSemanticDependencyRecord *dependency_record,
    const XrSemanticSourceExportRecord *source_export, const XrSemanticOperationRecord *operation,
    uint32_t *out_constructor) {
    if (out_constructor)
        *out_constructor = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(caller, operation->result_type) : NULL;
    uint32_t source_class = xr_semantic_source_class_export_source_class(dependency, source_export);
    const XrSemanticSourceClassRecord *source_class_record =
        xr_semantic_plan_source_class(dependency, source_class);
    if (!caller || !dependency || !dependency_record || !source_export || !operation ||
        !xr_semantic_class_construction_operation_is_exact(operation) ||
        !xr_semantic_external_class_instance_type_is_exact(result_type) ||
        !dependency_record->module_path || !dependency_record->module_path[0] ||
        !source_class_record ||
        !xr_stable_id_equal(result_type->source_class_identity, source_class_record->id))
        return XR_SEMANTIC_INDEX_NONE;
    uint16_t argument_count = (uint16_t) (operation->operand_count - 1u);
    uint32_t constructor = xr_semantic_class_constructor_function(dependency, source_class);
    const XrSemanticFunctionRecord *function =
        constructor != XR_SEMANTIC_INDEX_NONE ? xr_semantic_plan_function(dependency, constructor)
                                              : NULL;
    if (!function)
        return argument_count == 0 ? source_class : XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_class_constructor_arity_is_exact(dependency, constructor, function,
                                                      argument_count) ||
        xr_semantic_class_constructor_receiver_source_class(
            dependency, function->parameter_begin) != source_class)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t caller_operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(caller, &caller_operand_count);
    for (uint16_t i = 0; i < argument_count; i++) {
        uint32_t operand_index = operation->operand_begin + 1u + i;
        const XrSemanticOperandRecord *argument =
            operands && operand_index < caller_operand_count ? &operands[operand_index] : NULL;
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(dependency, function->parameter_begin + 1u + i);
        const XrSemanticTypeRecord *argument_type =
            argument ? xr_semantic_plan_type(caller, argument->type) : NULL;
        const XrSemanticTypeRecord *parameter_type =
            parameter ? xr_semantic_plan_type(dependency, parameter->type) : NULL;
        if (!argument || !parameter || !argument_type || !parameter_type ||
            parameter->value == XR_SEMANTIC_INDEX_NONE || parameter->function != constructor ||
            parameter->ordinal != i + 1u || parameter->mode != XR_PARAM_READ ||
            (parameter->ownership != XI_OWN_NONE && parameter->ownership != XI_OWN_OWNED &&
             parameter->ownership != XI_OWN_BORROWED) ||
            parameter->reserved != 0 || (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 ||
            argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) i ||
            !xr_semantic_parameter_type_admits_argument(dependency, parameter_type, argument_type,
                                                        parameter->mode) ||
            argument->parameter_mode != parameter->mode ||
            argument->transfer_mode != parameter->transfer_mode ||
            argument->access != XR_CALL_ARG_PLAIN || argument->origin != 0 ||
            argument->lifetime != 0 || argument->escape != 0 ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            (parameter->ownership == XI_OWN_BORROWED
                 ? argument->ownership_action != XR_SEM_OPERAND_BORROW
                 : argument->ownership_action != XR_SEM_OPERAND_CONSUME))
            return XR_SEMANTIC_INDEX_NONE;
    }
    if (out_constructor)
        *out_constructor = constructor;
    return source_class;
}

/* Semantic's full independent proof additionally reaches the root import and
 * matches its module/member spelling to the frozen dependency and export. */
static inline uint32_t xr_semantic_imported_class_construction_source_class(
    const XrSemanticPlan *caller, const XrSemanticPlan *dependency,
    const XrSemanticDependencyRecord *dependency_record,
    const XrSemanticSourceExportRecord *source_export, const XrSemanticOperationRecord *operation,
    uint32_t *out_constructor) {
    const char *module_path = NULL;
    const char *member = NULL;
    if (!dependency_record || !source_export ||
        !xr_semantic_imported_class_callee_is_exact(caller, operation, &module_path, &member) ||
        !module_path || !member || !dependency_record->module_path ||
        strcmp(module_path, dependency_record->module_path) != 0 ||
        strcmp(member, source_export->name) != 0) {
        if (out_constructor)
            *out_constructor = XR_SEMANTIC_INDEX_NONE;
        return XR_SEMANTIC_INDEX_NONE;
    }
    return xr_semantic_imported_class_construction_authority_source_class(
        caller, dependency, dependency_record, source_export, operation, out_constructor);
}

/* The declaration whose instance a direct-local call hands back, or NONE.
 *
 * An instance is a reference-capable allocation, not a slot the caller writes
 * into, so the result is a transfer: the callee hands back the allocation and
 * the caller takes ownership of the outer tagged value, exactly as it does for
 * an owned String or Array. The return must be fresh and whole -- an aliased or
 * parameter-forwarded return would hand back a borrow whose extent this plan
 * cannot state -- and the callee's declared return contract has to say the same
 * thing the call site reads. */
static inline uint32_t
xr_semantic_class_instance_result_source_class(const XrSemanticPlan *plan,
                                               const XrSemanticOperationRecord *operation) {
    const XrSemanticFunctionRecord *callee = xr_semantic_class_direct_local_callee(plan, operation);
    if (!plan || !operation || !callee ||
        (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL) ||
        operation->result_type != callee->return_type ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_complete != 1 || operation->return_provenance != XR_SEM_RETURN_OWNED ||
        callee->return_parameter != -1 || callee->return_provenance != XR_SEM_RETURN_OWNED)
        return XR_SEMANTIC_INDEX_NONE;
    return xr_semantic_class_instance_type_source_class(
        plan, xr_semantic_plan_type(plan, operation->result_type));
}

/* The declaration whose instance this shared read loads, or NONE.
 *
 * The read is proved through the one value its slot holds, and that value is an
 * instance for either of the two reasons an instance exists at all: a
 * construction made it, or a direct-local call handed it back. Asking only
 * about the construction would make a read of `var k = mk()` unnameable while
 * the same read of `var k = K()` is fine, which is a distinction about how the
 * allocation arrived rather than about what the slot holds. */
static inline uint32_t
xr_semantic_class_instance_read_source_class(const XrSemanticPlan *plan,
                                             const XrSemanticOperationRecord *operation) {
    if (!plan || xr_semantic_plan_source_class_count(plan) == 0 || !operation ||
        operation->opcode != XI_GET_SHARED)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t source_class = xr_semantic_class_instance_type_source_class(
        plan, xr_semantic_plan_type(plan, operation->result_type));
    const XrSemanticOperationRecord *definition =
        xr_semantic_class_shared_read_definition(plan, operation);
    uint32_t stored_class = xr_semantic_class_construction_source_class(plan, definition);
    if (stored_class == XR_SEMANTIC_INDEX_NONE)
        stored_class = xr_semantic_class_instance_result_source_class(plan, definition);
    if (source_class == XR_SEMANTIC_INDEX_NONE || stored_class != source_class)
        return XR_SEMANTIC_INDEX_NONE;
    return source_class;
}

/* One judgement for every value in the construction family: the borrowed reads
 * of a class object out of its module slot, the owned instance a construction
 * returns, the owned instance a direct-local call returns, and the borrowed
 * reads of an instance out of its own module slot. All of them are the outer
 * tagged value, because the IR gives an instance no machine geometry a bare
 * object pointer could state, and that is why they are one family rather than
 * four. Ownership is never assumed from the shape: it is the operation's own
 * result ownership, owned for either kind of call and borrowed for a read. */
static inline bool
xr_semantic_class_instance_value_is_exact(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *operation,
                                          uint32_t *out_source_class) {
    uint32_t source_class = XR_SEMANTIC_INDEX_NONE;
    if (!operation)
        return false;
    if (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL) {
        source_class = xr_semantic_class_construction_source_class(plan, operation);
        if (source_class == XR_SEMANTIC_INDEX_NONE)
            source_class = xr_semantic_class_instance_result_source_class(plan, operation);
    } else if (operation->opcode == XI_GET_SHARED) {
        source_class = xr_semantic_class_object_read_source_class(plan, operation);
        if (source_class == XR_SEMANTIC_INDEX_NONE)
            source_class = xr_semantic_class_instance_read_source_class(plan, operation);
    }
    if (source_class == XR_SEMANTIC_INDEX_NONE)
        return false;
    if (out_source_class)
        *out_source_class = source_class;
    return true;
}

/* The source declaration reached by loading one exact `ref Class` parameter,
 * or NONE. A ref parameter is a place, so field lowering first emits
 * PLACE_LOAD and only then LOAD_FIELD. The place and load rows together prove
 * that this value is the borrowed instance named by that parameter; no call,
 * alias or arbitrary instance-typed operation is admitted. */
static inline uint32_t
xr_semantic_class_ref_parameter_load_source_class(const XrSemanticPlan *plan,
                                                  const XrSemanticOperationRecord *load) {
    if (!plan || !load || load->opcode != XI_PLACE_LOAD || load->operand_count != 1 ||
        load->result_value == XR_SEMANTIC_INDEX_NONE ||
        load->effects != xi_generated_op_effects(XI_PLACE_LOAD) ||
        load->flags != xi_generated_op_default_flags(XI_PLACE_LOAD) ||
        load->ownership_use != xi_generated_op_own_use(XI_PLACE_LOAD) ||
        load->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        load->result_alias_operand != -1 || load->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        load->metadata_count != 0 || load->auxiliary_kind != XI_AUX_KIND_NONE ||
        load->semantic_immediate != 0 || load->constant != XR_SEMANTIC_INDEX_NONE ||
        load->callable_function != XR_SEMANTIC_INDEX_NONE ||
        load->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE || load->transfer_mode != 0 ||
        load->parameter_mode != 0 || load->parameter_ownership != 0 ||
        load->return_parameter != -1 || load->view_complete != 0 ||
        load->view_source_operand != -1 || load->view_source_parameter != -1)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || load->operand_begin >= operand_count)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperandRecord *place = &operands[load->operand_begin];
    if (place->type != load->result_type || place->role != XR_SEM_OPERAND_VALUE ||
        place->parameter != -1 || place->transfer_mode != 0 ||
        place->ownership_action != XR_SEM_OPERAND_BORROW ||
        place->parameter_mode != XR_PARAM_READ || place->access != XR_CALL_ARG_PLAIN ||
        place->origin != XI_PLACE_ORIGIN_NONE || place->lifetime != XI_PLACE_LIFETIME_NONE ||
        place->escape != XI_PLACE_ESCAPE_NONE || place->flags != 0)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticParameterRecord *parameter = NULL;
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(plan);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *candidate = xr_semantic_plan_parameter(plan, i);
        if (!candidate || candidate->function != load->function || candidate->value != place->value)
            continue;
        if (parameter)
            return XR_SEMANTIC_INDEX_NONE;
        parameter = candidate;
    }
    if (!parameter || parameter->type != load->result_type || parameter->mode != XR_PARAM_REF ||
        parameter->ownership != XI_OWN_BORROWED || parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 || parameter->reserved != 0)
        return XR_SEMANTIC_INDEX_NONE;
    return xr_semantic_class_instance_type_source_class(
        plan, xr_semantic_plan_type(plan, parameter->type));
}

/* The declaration whose instance a field read borrows from, or NONE. The read
 * is the generated field load: one borrowed plain operand, one metadata name,
 * and no allocation, constant, callee or view of its own. Which field the name
 * selects is the frontend's proof and is already frozen in the result type; what
 * this judgement adds is that the receiver is an instance this family named, so
 * the read borrows from a proved allocation rather than from an open object. */
static inline uint32_t
xr_semantic_class_field_read_source_class(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    if (!plan || xr_semantic_plan_source_class_count(plan) == 0 || !operation ||
        operation->opcode != XI_LOAD_FIELD || operation->operand_count != 1 ||
        operation->metadata_count != 1 || operation->semantic_immediate < 0 ||
        operation->allocation_key || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_LOAD_FIELD) ||
        operation->flags != xi_generated_op_default_flags(XI_LOAD_FIELD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_LOAD_FIELD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_LOAD_FIELD) ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->view_complete != 0 ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->transfer_mode != 0 || receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != 0 || receiver->access != 0 || receiver->origin != 0 ||
        receiver->lifetime != 0 || receiver->escape != 0 || receiver->flags != 0)
        return XR_SEMANTIC_INDEX_NONE;
    /* A receiver bound on entry is proved from the parameter table: the
     * declaration is either the member's own identity or the row the parameter
     * was declared with, and neither reaches this operand through a defining
     * operation. */
    if (xr_semantic_class_parameter_for_value(plan, receiver->value) != XR_SEMANTIC_INDEX_NONE)
        return xr_semantic_class_receiver_parameter_source_class(plan, operation->function,
                                                                 receiver);
    uint32_t source_class = xr_semantic_class_instance_type_source_class(
        plan, xr_semantic_plan_type(plan, receiver->type));
    const XrSemanticOperationRecord *definition =
        xr_semantic_class_value_definition(plan, receiver->value);
    uint32_t receiver_class = XR_SEMANTIC_INDEX_NONE;
    if (source_class == XR_SEMANTIC_INDEX_NONE || !definition ||
        definition->result_type != receiver->type)
        return XR_SEMANTIC_INDEX_NONE;
    receiver_class = xr_semantic_class_ref_parameter_load_source_class(plan, definition);
    if (receiver_class == XR_SEMANTIC_INDEX_NONE &&
        !xr_semantic_class_instance_value_is_exact(plan, definition, &receiver_class))
        return XR_SEMANTIC_INDEX_NONE;
    if (receiver_class != source_class)
        return XR_SEMANTIC_INDEX_NONE;
    return source_class;
}

#endif  // XR_SEMANTIC_CLASS_SHAPE_H
