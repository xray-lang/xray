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
