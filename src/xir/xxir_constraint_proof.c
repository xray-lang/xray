/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_constraint_proof.c - Bounded proofs from explicit declaration facts
 *
 * KEY CONCEPT:
 *   Matching methods do not establish conformance, and pending proofs are facts
 *   only when seeded by an authentic parameter declaration.
 */
#include "xxir_constraint_proof.h"
#include "xxir_interface_members.h"
#include "xxir_types.h"

static bool constraint_charge(XrXirBudget *budget, uint64_t bytes, uint64_t work) {
    if (bytes > SIZE_MAX || bytes > budget->metadata_bytes || work > budget->work) return false;
    budget->metadata_bytes -= bytes; budget->work -= work; return true;
}
static bool constraint_argument_shape(const XrXirTypes *types, XrXirType type, uint32_t count) {
    if (type == XR_XIR_UNIT || xr_xir_type_is_cell(types, type)) return false;
    uint32_t id = (uint32_t)type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
        return id - XR_XIR_TYPE_PARAMETER_BASE < count;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node ? node->parameter_span <= count : type == XR_XIR_BOOL || xr_xir_type_is_number(type) ||
        type == XR_XIR_STRING || type == XR_XIR_ATOMIC_I64 || type == XR_XIR_ERROR || type == XR_XIR_PANIC_INFO;
}
XrXirStatus xr_xir_constraint_structure(const XrXirTypes *types, XrXirConstraint constraint,
    uint32_t count, XrXirBudget *budget) {
    if (!budget || (!!constraint.interfaces != !!constraint.interface_count)) return XR_XIR_BAD_STRUCTURE;
    if (constraint.markers & ~XR_XIR_CONSTRAINT_MASK) return XR_XIR_BAD_TYPE;
    if (!constraint_charge(budget, (uint64_t)constraint.interface_count * sizeof(*constraint.interfaces),
        (uint64_t)constraint.interface_count + 1)) return XR_XIR_BUDGET;
    const XrXirInterfaceTable *table = types ? types->interfaces : NULL;
    for (uint32_t i = 0; i < constraint.interface_count; ++i) {
        const XrXirInterfaceApplication *app = &constraint.interfaces[i];
        if (!table || !table->declarations || app->declaration >= table->count ||
            (!!app->arguments != !!app->argument_count)) return XR_XIR_BAD_STRUCTURE;
        if (app->argument_count != table->declarations[app->declaration].parameter_count) return XR_XIR_BAD_TYPE;
        if (!constraint_charge(budget, (uint64_t)app->argument_count * sizeof(*app->arguments), app->argument_count))
            return XR_XIR_BUDGET;
        for (uint32_t a = 0; a < app->argument_count; ++a)
            if (!constraint_argument_shape(types, app->arguments[a], count)) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static XrXirStatus constraint_application_match(const XrXirConstraintSubstitution *use,
    const XrXirInterfaceApplication *required, const XrXirInterfaceClosure *closure,
    const XrXirInterfaceApplication *fact, XrXirBudget *budget) {
    if (required->declaration != fact->declaration || required->argument_count != fact->argument_count)
        return XR_XIR_BAD_TYPE;
    const XrXirTypes *types = xr_xir_interface_closure_types(closure);
    for (uint32_t a = 0; a < fact->argument_count; ++a) {
        XrXirStatus status = xr_xir_type_substitution_matches_between(use->types, types,
            use->arguments, use->argument_count, required->arguments[a], fact->arguments[a], budget);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_constraint_entails(const XrXirConstraintEnvironment *environment,
    const XrXirConstraintSubstitution *use, XrXirBudget *budget) {
    if (!environment || !use || !budget || (environment->parameter_count && !environment->constraints) ||
        (use->argument_count && !use->arguments)) return XR_XIR_BAD_STRUCTURE;
    if (!constraint_argument_shape(environment->types, use->subject, environment->parameter_count)) return XR_XIR_BAD_TYPE;
    XrXirStatus status = xr_xir_constraint_structure(use->types, use->requirement, use->argument_count, budget);
    if (status != XR_XIR_OK) return status;
    if (!constraint_charge(budget, 0, 1)) return XR_XIR_BUDGET;
    status = xr_xir_type_markers(environment->types, use->subject, use->requirement.markers,
        environment->constraints, environment->parameter_count, &budget->work);
    if (status != XR_XIR_OK || !use->requirement.interface_count) return status;
    uint32_t id = (uint32_t)use->subject;
    /* Explicit nominal implementation maps are required before concrete proofs. */
    if (id < XR_XIR_TYPE_PARAMETER_BASE || id >= XR_XIR_TYPE_PARAMETER_LIMIT ||
        id - XR_XIR_TYPE_PARAMETER_BASE >= environment->parameter_count) return XR_XIR_BAD_TYPE;
    const XrXirConstraint *facts = &environment->constraints[id - XR_XIR_TYPE_PARAMETER_BASE];
    status = xr_xir_constraint_structure(environment->types, *facts, environment->parameter_count, budget);
    if (status != XR_XIR_OK) return status;
    const XrXirInterfaceTable *table = environment->types ? environment->types->interfaces : NULL;
    if (!facts->interface_count || !table) return XR_XIR_BAD_TYPE;
    XrXirInterfaceClosure *closure = NULL;
    status = xr_xir_interface_closure_build(table, environment->types, facts->interfaces,
        facts->interface_count, budget, &closure);
    for (uint32_t r = 0; status == XR_XIR_OK && r < use->requirement.interface_count; ++r) {
        const XrXirInterfaceApplication *required = &use->requirement.interfaces[r];
        bool proved = false;
        uint32_t count = xr_xir_interface_closure_application_count(closure);
        for (uint32_t a = 0; a < count; ++a) {
            if (!constraint_charge(budget, 0, 1)) { status = XR_XIR_BUDGET; break; }
            const XrXirInterfaceApplication *fact = xr_xir_interface_closure_application(closure, a);
            XrXirStatus match = constraint_application_match(use, required, closure, fact, budget);
            if (match == XR_XIR_OK) { proved = true; break; }
            if (match != XR_XIR_BAD_TYPE) { status = match; break; }
        }
        if (status == XR_XIR_OK && !proved) status = XR_XIR_BAD_TYPE;
    }
    xr_xir_interface_closure_free(closure); return status;
}
XrXirStatus xr_xir_constraint_arguments(const XrXirConstraintEnvironment *environment,
    const XrXirConstraint *requirements, const XrXirType *arguments, uint32_t count, XrXirBudget *budget) {
    if (count && (!requirements || !arguments)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p = 0; p < count; ++p) {
        XrXirConstraintSubstitution use = {environment->types, requirements[p], arguments, count, arguments[p]};
        XrXirStatus status = xr_xir_constraint_entails(environment, &use, budget);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_constraint_environment_verify(const XrXirConstraintEnvironment *environment,
    XrXirBudget *budget) {
    if (!environment || !budget || (environment->parameter_count && !environment->constraints))
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceTable *table = environment->types ? environment->types->interfaces : NULL;
    for (uint32_t p = 0; p < environment->parameter_count; ++p) {
        const XrXirConstraint *constraint = &environment->constraints[p];
        XrXirStatus status = xr_xir_constraint_structure(environment->types, *constraint,
            environment->parameter_count, budget);
        if (status != XR_XIR_OK) return status;
        if (!constraint->interface_count) continue;
        for (uint32_t i = 0; i < constraint->interface_count; ++i) {
            const XrXirInterfaceApplication *app = &constraint->interfaces[i];
            const XrXirInterfaceDeclaration *declaration = &table->declarations[app->declaration];
            for (uint32_t a = 0; a < app->argument_count; ++a) {
                status = xr_xir_type_context_verify(environment->types, app->arguments[a],
                    environment->constraints, environment->parameter_count, budget);
                if (status != XR_XIR_OK) return status;
            }
            status = xr_xir_constraint_arguments(environment, declaration->constraints,
                app->arguments, app->argument_count, budget);
            if (status != XR_XIR_OK) return status;
        }
        XrXirInterfaceClosure *closure = NULL;
        status = xr_xir_interface_closure_build(table, environment->types, constraint->interfaces,
            constraint->interface_count, budget, &closure);
        xr_xir_interface_closure_free(closure);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus constraint_environment(const XrXirModule *module, XrXirDeclarationContext owner,
    XrXirConstraintEnvironment *environment) {
    if (!module) return XR_XIR_BAD_STRUCTURE;
    *environment = (XrXirConstraintEnvironment){module->types, NULL, 0};
    if (owner.kind == XR_XIR_CONTEXT_FUNCTION) {
        if (owner.declaration >= module->function_count) return XR_XIR_BAD_STRUCTURE;
        if (module->generics) {
            const XrXirGeneric *g = &module->generics[owner.declaration];
            environment->constraints = g->constraints; environment->parameter_count = g->parameter_count;
        }
    } else if (owner.kind == XR_XIR_CONTEXT_NOMINAL) {
        const XrXirNominalTable *table = module->types ? module->types->nominals : NULL;
        if (!table || !table->declarations || owner.declaration >= table->count) return XR_XIR_BAD_STRUCTURE;
        const XrXirNominalDeclaration *d = &table->declarations[owner.declaration];
        environment->constraints = d->constraints; environment->parameter_count = d->parameter_count;
    } else if (owner.kind == XR_XIR_CONTEXT_INTERFACE) {
        const XrXirInterfaceTable *table = module->types ? module->types->interfaces : NULL;
        if (!table || !table->declarations || owner.declaration >= table->count) return XR_XIR_BAD_STRUCTURE;
        const XrXirInterfaceDeclaration *d = &table->declarations[owner.declaration];
        environment->constraints = d->constraints; environment->parameter_count = d->parameter_count;
    } else return XR_XIR_BAD_STRUCTURE;
    return environment->parameter_count && !environment->constraints ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
XrXirStatus xr_xir_constraints_prove(const XrXirProofContext *context,
    const XrXirConstraintUse *use, XrXirBudget *budget) {
    if (!context || !use || !budget) return XR_XIR_BAD_STRUCTURE;
    XrXirConstraintEnvironment environment = {0}, declaration = {0};
    XrXirStatus status = constraint_environment(context->module, context->owner, &environment);
    if (status == XR_XIR_OK) status = constraint_environment(context->module, use->declaration, &declaration);
    if (status != XR_XIR_OK) return status;
    if (use->argument_count != declaration.parameter_count || use->parameter >= use->argument_count || !use->arguments)
        return XR_XIR_BAD_STRUCTURE;
    XrXirConstraintSubstitution substituted = {context->module->types, declaration.constraints[use->parameter],
        use->arguments, use->argument_count, use->arguments[use->parameter]};
    for (uint32_t a = 0; a < use->argument_count; ++a)
        if (!constraint_argument_shape(environment.types, use->arguments[a], environment.parameter_count))
            return XR_XIR_BAD_TYPE;
    return xr_xir_constraint_entails(&environment, &substituted, budget);
}
