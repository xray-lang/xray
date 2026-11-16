/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_module_query.inc.c - Exclusive module and class field loans from owned effects
 *
 * KEY CONCEPT:
 *   A ref argument lends either a module binding or a field of a class object.
 *   The call conflicts when the code it runs may touch the same target by
 *   another path. A Slice argument of a module binding conflicts only with a
 *   write to that binding, a MutSlice with any access; the callee's own body
 *   may then treat module storage as apart from its view parameters. A module binding is named, so such an access is a proven
 *   conflict. A class field is reached through an object that the callee may
 *   also hold, and only a run-time identity check could tell the objects
 *   apart; until compiled code has one, every such call is refused.
 */
#include "xxir_effect_module_export_internal.h"

struct XirModuleEffectExport { XirModuleEffectExportView view; };
XR_FUNC void xir_effects_module_export_free(XirModuleEffectExport *owned) {
    if (!owned) return;
    xr_compile_resources_free((void *)owned->view.functions);
    xr_compile_resources_free((void *)owned->view.values);
    xr_compile_resources_free((void *)owned->view.terms);
    xr_compile_resources_free((void *)owned->view.modules);
    xr_compile_resources_free((void *)owned->view.dependencies);
    xr_compile_resources_free((void *)owned->view.slots);
    xr_compile_resources_free(owned);
}
XR_FUNC XrXirStatus xir_effects_module_export_view(const XirModuleEffectExport *owned, XirModuleEffectExportView *output) {
    if (!owned || !output) return XR_XIR_BAD_STRUCTURE;
    *output = owned->view; return XR_XIR_OK;
}
static bool effect_module_export_kind(uint8_t kind, XirModuleEffectTermKind *output) {
    switch (kind) {
    case EFFECT_MODULE_SLOT: *output=XIR_MODULE_EFFECT_SLOT; return true;
    case EFFECT_MODULE_CALLBACK: *output=XIR_MODULE_EFFECT_CALLBACK; return true;
    case EFFECT_MODULE_CAPTURE: *output=XIR_MODULE_EFFECT_CAPTURE; return true;
    case EFFECT_MODULE_UNKNOWN: *output=XIR_MODULE_EFFECT_UNKNOWN; return true;
    case EFFECT_MODULE_FIELD: *output=XIR_MODULE_EFFECT_FIELD; return true;
    case EFFECT_MODULE_PARAMETER: *output=XIR_MODULE_EFFECT_PARAMETER; return true;
    default: return false;
    }
}
typedef struct EffectModuleProjection {
    const XrXirArtifact *actual, *producer;
    const XrXirProvenance *instance;
    const EffectCheckedRoots *roots;
    uint32_t *witnesses;
    size_t term_limit;
} EffectModuleProjection;

/* The specializing producer retains its exact Source derivation rather than
 * solving the same facts again for each physical function. Its real origins
 * relocate rows; no absent equation is replaced with an empty summary. */
static XrXirStatus effect_module_export_projection(const XrXirArtifact *artifact,
    EffectModuleProjection *projection) {
    *projection = (EffectModuleProjection){.actual=artifact,.producer=artifact};
    if (!artifact->program_final && !artifact->checked_effects) {
        const XrXirProvenance *instance=artifact->module.provenance;
        if (!instance || instance->kind!=XR_XIR_EVIDENCE_INSTANCE || !instance->source ||
            !instance->origins || instance->count!=artifact->module.function_count ||
            instance->source==artifact || instance->source->context.resources!=artifact->context.resources)
            return XR_XIR_BAD_STRUCTURE;
        projection->producer=instance->source; projection->instance=instance;
    }
    const XrXirDeclarations *actual=artifact->module.declarations;
    const XrXirDeclarations *source=projection->producer->module.declarations;
    if (!actual || !source || actual->module_count!=source->module_count || actual->slot_count!=source->slot_count)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t slot=0;slot<actual->slot_count;++slot)
        if (actual->slots[slot].module!=source->slots[slot].module) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t module=0;module<actual->module_count;++module) {
        const XrXirSourceModule *a=&actual->modules[module], *b=&source->modules[module];
        if (a->dependency_count!=b->dependency_count || (a->dependency_count &&
            memcmp(a->dependencies,b->dependencies,(size_t)a->dependency_count*sizeof(*a->dependencies))))
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_module_export_function(const EffectModuleProjection *projection,
    uint32_t function, uint32_t *origin) {
    *origin=projection->instance ? projection->instance->origins[function].function : function;
    if (*origin>=projection->producer->module.function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *actual=&projection->actual->module.functions[function];
    const XrXirFunction *source=&projection->producer->module.functions[*origin];
    const XrXirFunctionIdentity *a=&projection->actual->module.declarations->functions[function];
    const XrXirFunctionIdentity *b=&projection->producer->module.declarations->functions[*origin];
    return actual->parameter_count==source->parameter_count && actual->instruction_count==source->instruction_count &&
        a->module==b->module && a->capture_count==b->capture_count ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus effect_module_export_set(const EffectModuleProjection *projection, uint32_t node, uint8_t property,
    XirModuleEffectSpan *span, size_t *used, XirModuleEffectTerm *terms) {
    const EffectCheckedRoots *roots=projection->roots;
    if (node >= roots->nodes) return XR_XIR_BAD_STRUCTURE;
    if (!roots->module_access) { *span = (XirModuleEffectSpan){0}; return XR_XIR_OK; }
    size_t first = *used; uint32_t visited = 0;
    for (uint32_t m = roots->node[node].module_sets[property]; m != UINT32_MAX;) {
        if (m >= roots->module_members || visited++ >= roots->module_members || *used == SIZE_MAX) return XR_XIR_BAD_STRUCTURE;
        EffectModuleMember member = roots->module_member[m]; XirModuleEffectTermKind kind;
        if (member.node != node || member.property != property || !effect_module_export_kind(member.kind, &kind)) return XR_XIR_BAD_STRUCTURE;
        if (terms) {
            if (*used>=projection->term_limit) return XR_XIR_BAD_STRUCTURE;
            uint32_t witness=member.function, instruction=member.instruction;
            if (projection->witnesses) {
                if (witness!=UINT32_MAX && witness>=projection->producer->module.function_count) return XR_XIR_BAD_STRUCTURE;
                witness=witness==UINT32_MAX ? UINT32_MAX : projection->witnesses[witness];
                if (witness==UINT32_MAX) instruction=UINT32_MAX;
            }
            terms[*used] = (XirModuleEffectTerm){kind,member.identity,witness,instruction};
        }
        ++*used; m = member.next;
    }
    *span = (XirModuleEffectSpan){*used == first ? 0 : first,*used - first}; return XR_XIR_OK;
}
static XrXirStatus effect_module_export_terms(const EffectCheckedRoots *roots, const EffectModuleProjection *projection,
    XirModuleEffectExportView *view, bool copy) {
    size_t used = 0, value = 0; XrXirStatus status = XR_XIR_OK;
    for (uint32_t f = 0; f < view->function_count && status == XR_XIR_OK; ++f) {
        uint32_t declaration=0; status=effect_module_export_function(projection,f,&declaration);
        if (status!=XR_XIR_OK || declaration>=roots->templates || declaration>=roots->functions) return XR_XIR_BAD_STRUCTURE;
        EffectCheckFunction source = roots->function[declaration]; XirModuleEffectFunction row = {
            .module=source.module,.parameter_count=source.parameters,.capture_count=source.capture_count,
            .first_value=value,.value_count=source.values};
        if (source.first > roots->nodes || source.values > roots->nodes - source.first ||
            (copy && (value>view->value_count || source.values>view->value_count-value))) return XR_XIR_BAD_STRUCTURE;
        status = effect_module_export_set(projection, source.root, EFFECT_MODULE_ACCESS, &row.access, &used, copy ? (XirModuleEffectTerm *)view->terms : NULL);
        if (status == XR_XIR_OK) status = effect_module_export_set(projection, source.root, EFFECT_MODULE_WRITE, &row.write, &used, copy ? (XirModuleEffectTerm *)view->terms : NULL);
        if (status == XR_XIR_OK) status = effect_module_export_set(projection, source.result, EFFECT_MODULE_ORIGIN, &row.result_origin, &used, copy ? (XirModuleEffectTerm *)view->terms : NULL);
        if (copy) ((XirModuleEffectFunction *)view->functions)[f] = row;
        if (source.values > SIZE_MAX - value) return XR_XIR_OVERFLOW;
        for (uint32_t v = 0; v < source.values && status == XR_XIR_OK; ++v) {
            XirModuleEffectSpan origin;
            status = effect_module_export_set(projection, source.first + v, EFFECT_MODULE_ORIGIN, &origin, &used, copy ? (XirModuleEffectTerm *)view->terms : NULL);
            if (copy && status == XR_XIR_OK) ((XirModuleEffectValue *)view->values)[value + v].origin = origin;
        }
        value += source.values;
    }
    if (copy && (used != view->term_count || value != view->value_count)) return XR_XIR_BAD_STRUCTURE;
    if (!copy) { view->term_count = used; view->value_count = value; }
    return status;
}
static XrXirStatus effect_module_export_allocate(const XrXirCompileContext *context,
    const XrXirDeclarations *declarations, XirModuleEffectExportView *view) {
    XrXirStatus status = XR_XIR_OK;
    view->module_count = declarations->module_count; view->slot_count = declarations->slot_count;
    for (uint32_t m = 0; m < view->module_count; ++m) {
        uint32_t count = declarations->modules[m].dependency_count;
        if (count > SIZE_MAX - view->dependency_count) return XR_XIR_OVERFLOW;
        view->dependency_count += count;
    }
    view->functions = xir_compile_calloc(context, view->function_count, sizeof(*view->functions), &status);
    view->values = xir_compile_calloc(context, view->value_count, sizeof(*view->values), &status);
    view->terms = xir_compile_calloc(context, view->term_count, sizeof(*view->terms), &status);
    view->modules = xir_compile_calloc(context, view->module_count, sizeof(*view->modules), &status);
    view->dependencies = xir_compile_calloc(context, view->dependency_count, sizeof(*view->dependencies), &status);
    view->slots = xir_compile_calloc(context, view->slot_count, sizeof(*view->slots), &status);
    return status;
}
XR_FUNC XrXirStatus xir_effects_module_export_copy(const XrXirArtifact *artifact, XirModuleEffectExport **output) {
    if (!artifact || !output || *output) return XR_XIR_BAD_STRUCTURE;
    if (artifact->module.stage != XR_XIR_CHECKED && artifact->module.stage != XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    const XrXirCompileContext *context = &artifact->context;
    EffectModuleProjection projection={0};
    XrXirStatus status=effect_module_export_projection(artifact,&projection);
    if (status!=XR_XIR_OK) return status;
    const XrXirArtifact *producer=projection.producer;
    XirEffectInput input = {&producer->module,producer->construction,NULL};
    const XrXirEffects *effects = producer->program_final ? producer->program_final : producer->checked_effects;
    if (!effects || !effect_final_outcomes(effects) || !effects->checked_roots)
        return XR_XIR_BAD_STRUCTURE;
    projection.roots=effects->checked_roots;
    status = xir_effects_source_body_match_input(context, input, effects);
    if (status != XR_XIR_OK) return status;
    if (!input.module->declarations) return XR_XIR_BAD_STRUCTURE;
    XirModuleEffectExport *owned = xir_compile_calloc(context, 1, sizeof(*owned), &status);
    if (!owned) return status;
    XirModuleEffectExportView *view = &owned->view;
    view->function_count = artifact->module.function_count;
    view->module_access_complete = effects->checked_roots->module_access;
    if (projection.instance) {
        uint64_t bytes=(uint64_t)producer->module.function_count*sizeof(*projection.witnesses);
        if (bytes>SIZE_MAX) status=XR_XIR_OVERFLOW;
        else projection.witnesses=xir_compile_alloc(context,(size_t)bytes,&status);
        if (projection.witnesses) {
            for (uint32_t f=0;f<producer->module.function_count;++f) projection.witnesses[f]=UINT32_MAX;
            for (uint32_t f=0;f<view->function_count && status==XR_XIR_OK;++f) {
                uint32_t origin=0; status=effect_module_export_function(&projection,f,&origin);
                if (status==XR_XIR_OK && projection.witnesses[origin]==UINT32_MAX) projection.witnesses[origin]=f;
            }
        }
    }
    if (status==XR_XIR_OK) status = effect_module_export_terms(effects->checked_roots, &projection, view, false);
    projection.term_limit=view->term_count;
    if (status == XR_XIR_OK) status = effect_module_export_allocate(context, artifact->module.declarations, view);
    if (status == XR_XIR_OK) status = xir_effects_source_body_match_input(context, input, effects);
    if (status == XR_XIR_OK) status = effect_module_export_terms(effects->checked_roots, &projection, view, true);
    if (status == XR_XIR_OK) {
        const XrXirDeclarations *declarations = artifact->module.declarations; size_t next = 0;
        if (view->module_count != declarations->module_count || view->slot_count != declarations->slot_count) status = XR_XIR_BAD_STRUCTURE;
        for (uint32_t m = 0; m < view->module_count && status == XR_XIR_OK; ++m) {
            XrXirSourceModule source = declarations->modules[m];
            if (source.dependency_count > view->dependency_count - next) { status = XR_XIR_BAD_STRUCTURE; break; }
            ((XirModuleEffectDependency *)view->modules)[m] = (XirModuleEffectDependency){source.dependency_count ? next : 0,source.dependency_count};
            if (source.dependency_count) memcpy((uint32_t *)view->dependencies + next, source.dependencies,
                (size_t)source.dependency_count * sizeof(*source.dependencies));
            next += source.dependency_count;
        }
        if (next != view->dependency_count) status = XR_XIR_BAD_STRUCTURE;
        for (uint32_t s = 0; s < view->slot_count && status == XR_XIR_OK; ++s)
            ((XirModuleEffectSlot *)view->slots)[s].module = declarations->slots[s].module;
    }
    if (status == XR_XIR_OK) status = xir_effects_source_body_match_input(context, input, effects);
    xr_compile_resources_free(projection.witnesses);
    if (status != XR_XIR_OK) { xir_effects_module_export_free(owned); return status; }
    *output = owned; return XR_XIR_OK;
}

/* property selects ACCESS or WRITE terms of the code a call runs. */
typedef struct EffectModuleTarget {
    uint8_t kind, property;
    uint32_t identity;
} EffectModuleTarget;

static bool effect_module_target_matches(EffectModuleTarget target, EffectModuleMember member) {
    if (member.kind != target.kind) return false;
    if (target.kind != EFFECT_MODULE_FIELD) return member.identity == target.identity || target.identity == UINT32_MAX;
    return member.identity == target.identity || member.identity == EFFECT_MODULE_FIELD_ANY ||
        target.identity == EFFECT_MODULE_FIELD_ANY;
}

static void effect_module_conflict_target(EffectModuleTarget target, XirModuleBorrowConflict *conflict) {
    if (target.kind == EFFECT_MODULE_FIELD) conflict->field = target.identity;
    else conflict->slot = target.identity;
}

static bool effect_module_conflict_member(const EffectCheckedRoots *roots, EffectModuleMember member,
    uint32_t function, EffectModuleTarget target, XirModuleBorrowConflict *conflict) {
    if (member.kind == EFFECT_MODULE_CALLBACK) {
        EffectCheckFunction row = roots->function[function];
        if (member.identity >= row.parameters) {
            effect_module_conflict_target(target, conflict);
            conflict->unknown = true;
            return true;
        }
        if (!effect_module_unknown(&roots->node[row.first + member.identity])) return false;
        conflict->unknown = true;
    } else if (member.kind == EFFECT_MODULE_UNKNOWN) {
        conflict->unknown = true;
    } else if (!effect_module_target_matches(target, member)) {
        return false;
    }
    effect_module_conflict_target(target, conflict);
    conflict->access_function = member.function;
    conflict->access_instruction = member.instruction;
    return true;
}

static bool effect_module_conflict_set(const EffectCheckedRoots *roots, uint32_t node, uint8_t property,
    uint32_t function, EffectModuleTarget target, XirModuleBorrowConflict *conflict) {
    for (uint32_t m = roots->node[node].module_sets[property]; m != UINT32_MAX; m = roots->module_member[m].next)
        if (effect_module_conflict_member(roots, roots->module_member[m], function, target, conflict)) return true;
    return false;
}

static bool effect_module_conflict_call(const EffectCheckedRoots *roots, EffectCheckCall call,
    EffectModuleTarget target, XirModuleBorrowConflict *conflict) {
    if (call.fixed_effects) return false;
    EffectCheckFunction callee = roots->function[roots->node[call.target].function];
    uint32_t caller = roots->node[call.output].function;
    conflict->callee = callee.declaration;
    for (uint32_t m = roots->node[call.target].module_sets[target.property]; m != UINT32_MAX;) {
        EffectModuleMember member = roots->module_member[m];
        m = member.next;
        if (member.kind == EFFECT_MODULE_CALLBACK || member.kind == EFFECT_MODULE_CAPTURE) {
            if (member.identity >= call.count) continue;
            uint32_t actual = roots->argument[call.first + member.identity];
            uint8_t property = member.kind == EFFECT_MODULE_CALLBACK ? target.property : EFFECT_MODULE_ORIGIN;
            if (effect_module_conflict_set(roots, actual, property, caller, target, conflict)) return true;
        } else if (effect_module_conflict_member(roots, member, caller, target, conflict)) {
            return true;
        }
    }
    return false;
}

/* Checks the code one call instruction runs against one lent target. */
static bool effect_module_borrow_conflict(const EffectCheckedRoots *roots, const XrXirTypes *types,
    const XrXirInstruction *op, uint32_t function, uint32_t instruction, uint32_t argument, EffectModuleTarget target,
    XirModuleBorrowConflict *conflict) {
    EffectCheckFunction row = roots->function[function];
    for (uint32_t c = roots->node[row.root].calls; c != UINT32_MAX; c = roots->call[c].next) {
        EffectCheckCall call = roots->call[c];
        EffectCheckFunction callee = roots->function[roots->node[call.target].function];
        if (call.instruction != instruction || call.target != callee.root || call.protocol != 2 ||
            ((op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE) && argument < callee.capture_count)) continue;
        if (effect_module_conflict_call(roots, call, target, conflict)) return true;
    }
    for (uint32_t p = 0; p < row.pending_count; ++p) {
        EffectCheckPending pending = roots->pending[row.pending_first + p];
        if (pending.instruction != instruction) continue;
        if (pending.kind == EFFECT_CHECK_HIGHER_ORDER) {
            for (uint32_t e = roots->node[row.root].aliases; e != UINT32_MAX; e = roots->alias[e].next) {
                EffectCheckAlias alias = roots->alias[e];
                if (alias.instruction != instruction || alias.source != pending.receiver ||
                    !(alias.module_transfer & EFFECT_MODULE_TRANSFER_EXECUTE)) continue;
                EffectCheckNode receiver = roots->node[pending.receiver];
                if ((receiver.erased || receiver.callable_opaque) && effect_module_unknown(&receiver)) {
                    effect_module_conflict_target(target, conflict);
                    conflict->callee = UINT32_MAX;
                    conflict->unknown = true;
                    return true;
                }
                if (!receiver.erased && !receiver.callable_opaque &&
                    effect_module_conflict_set(roots, pending.receiver, target.property,
                        function, target, conflict)) return true;
            }
        } else if (!pending.linked && effect_module_requirement_unknown(types, pending)) {
            effect_module_conflict_target(target, conflict);
            conflict->callee = UINT32_MAX;
            conflict->unknown = true;
            return true;
        }
    }
    return false;
}

XR_FUNC XrXirStatus xir_effects_module_borrow_site(const XrXirCompileContext *context,
    const XrXirEffects *effects, const XrXirModule *module, uint32_t function, uint32_t instruction,
    XirModuleBorrowConflict *output) {
    if (!output || !module || !effects || !effects->checked_roots || function >= module->function_count ||
        !xir_effects_context_matches(context, effects, module->function_count) ||
        instruction >= module->functions[function].instruction_count) return XR_XIR_BAD_STRUCTURE;
    XirModuleBorrowConflict conflict = {.slot = UINT32_MAX, .field = EFFECT_MODULE_FIELD_NONE,
        .callee = UINT32_MAX, .access_function = UINT32_MAX, .access_instruction = UINT32_MAX};
    const EffectCheckedRoots *roots = effects->checked_roots;
    const XrXirFunction *body = &module->functions[function];
    const XrXirInstruction *op = &body->instructions[instruction];
    if (!effect_module_borrow_op(op->op)) {
        *output = conflict;
        return XR_XIR_OK;
    }
    if (op->args[0] > body->operand_count || op->args[1] > body->operand_count - op->args[0])
        return XR_XIR_BAD_STRUCTURE;
    EffectCheckFunction row = roots->function[function];
    for (uint32_t a = 0; a < op->args[1]; ++a) {
        uint32_t operand = body->operands[op->args[0] + a];
        if (operand >= row.values) return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *type = xr_xir_type_node(module->types, xr_xir_operand_type(body, operand));
        uint8_t view = !type ? 0 : type->kind == XR_XIR_TYPE_SLICE ? 1 : type->kind == XR_XIR_TYPE_MUT_SLICE ? 2 : 0;
        if (view) {
            for (uint32_t m = roots->node[row.first + operand].module_sets[EFFECT_MODULE_ORIGIN]; m != UINT32_MAX;
                m = roots->module_member[m].next) {
                EffectModuleMember origin = roots->module_member[m];
                if (origin.kind != EFFECT_MODULE_SLOT && origin.kind != EFFECT_MODULE_UNKNOWN) continue;
                bool unknown = origin.kind == EFFECT_MODULE_UNKNOWN;
                EffectModuleTarget target = {EFFECT_MODULE_SLOT, view == 1 ? EFFECT_MODULE_WRITE : EFFECT_MODULE_ACCESS,
                    unknown ? UINT32_MAX : origin.identity};
                if (effect_module_borrow_conflict(roots, module->types, op, function, instruction, a, target, &conflict)) {
                    conflict.view = view;
                    conflict.unknown_owner = unknown;
                    goto done;
                }
            }
            continue;
        }
        if (!effect_module_loan_operand(module->types, body, operand)) continue;
        for (uint32_t m = roots->node[row.first + operand].module_sets[EFFECT_MODULE_ORIGIN]; m != UINT32_MAX;) {
            EffectModuleMember origin = roots->module_member[m];
            m = origin.next;
            if (origin.kind != EFFECT_MODULE_SLOT) continue;
            EffectModuleTarget target = {EFFECT_MODULE_SLOT, EFFECT_MODULE_ACCESS, origin.identity};
            if (effect_module_borrow_conflict(roots, module->types, op, function, instruction, a, target, &conflict)) goto done;
        }
        uint32_t field = EFFECT_MODULE_FIELD_NONE;
        if (effect_module_lent_field(module->types, body, operand, &field)) {
            EffectModuleTarget target = {EFFECT_MODULE_FIELD, EFFECT_MODULE_ACCESS, field};
            if (effect_module_borrow_conflict(roots, module->types, op, function, instruction, a, target, &conflict)) goto done;
        }
    }
done:
    *output = conflict;
    return XR_XIR_OK;
}

static XrXirStatus effect_module_borrows_verify(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirEffects *effects, XrXirDiagnostic *diagnostic) {
    if (!effects || !effects->checked_roots) return XR_XIR_BAD_STRUCTURE;
    if (!effects->checked_roots->module_access) return XR_XIR_OK;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            XirModuleBorrowConflict conflict = {0};
            XrXirStatus status = xir_effects_module_borrow_site(context, effects, module, f, i, &conflict);
            if (status != XR_XIR_OK) return status;
            if (conflict.field != EFFECT_MODULE_FIELD_NONE) {
                *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, f, UINT32_MAX, i,
                    XR_XIR_DIAGNOSTIC_FIELD_BORROW};
                return XR_XIR_BAD_TYPE;
            }
            if (conflict.slot == UINT32_MAX && !conflict.unknown_owner) continue;
            *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, f, UINT32_MAX, i,
                conflict.unknown ? XR_XIR_DIAGNOSTIC_MODULE_BORROW_UNKNOWN : XR_XIR_DIAGNOSTIC_MODULE_BORROW_ACCESS};
            return XR_XIR_BAD_TYPE;
        }
    }
    return XR_XIR_OK;
}
