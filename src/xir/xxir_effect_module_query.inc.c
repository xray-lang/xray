/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_module_query.inc.c - Exclusive module loans from owned effects
 */
static bool effect_module_conflict_member(const EffectCheckedRoots *roots, EffectModuleMember member,
    uint32_t function, uint32_t slot, XirModuleBorrowConflict *conflict) {
    if (member.kind == EFFECT_MODULE_CALLBACK) {
        EffectCheckFunction row = roots->function[function];
        if (member.identity >= row.parameters) {
            conflict->slot = slot;
            conflict->unknown = true;
            return true;
        }
        if (!effect_module_unknown(&roots->node[row.first + member.identity])) return false;
        conflict->unknown = true;
    } else if (member.kind == EFFECT_MODULE_UNKNOWN) {
        conflict->unknown = true;
    } else if (member.kind != EFFECT_MODULE_SLOT || member.identity != slot) {
        return false;
    }
    conflict->slot = slot;
    conflict->access_function = member.function;
    conflict->access_instruction = member.instruction;
    return true;
}

static bool effect_module_conflict_set(const EffectCheckedRoots *roots, uint32_t node, uint8_t property,
    uint32_t function, uint32_t slot, XirModuleBorrowConflict *conflict) {
    for (uint32_t m = roots->node[node].module_sets[property]; m != UINT32_MAX; m = roots->module_member[m].next)
        if (effect_module_conflict_member(roots, roots->module_member[m], function, slot, conflict)) return true;
    return false;
}

static bool effect_module_conflict_call(const EffectCheckedRoots *roots, EffectCheckCall call,
    uint32_t slot, XirModuleBorrowConflict *conflict) {
    if (call.fixed_effects) return false;
    EffectCheckFunction callee = roots->function[roots->node[call.target].function];
    uint32_t caller = roots->node[call.output].function;
    conflict->callee = callee.declaration;
    for (uint32_t m = roots->node[call.target].module_sets[EFFECT_MODULE_ACCESS]; m != UINT32_MAX;) {
        EffectModuleMember member = roots->module_member[m];
        m = member.next;
        if (member.kind == EFFECT_MODULE_CALLBACK || member.kind == EFFECT_MODULE_CAPTURE) {
            if (member.identity >= call.count) continue;
            uint32_t actual = roots->argument[call.first + member.identity];
            uint8_t property = member.kind == EFFECT_MODULE_CALLBACK ? EFFECT_MODULE_ACCESS : EFFECT_MODULE_ORIGIN;
            if (effect_module_conflict_set(roots, actual, property, caller, slot, conflict)) return true;
        } else if (effect_module_conflict_member(roots, member, caller, slot, conflict)) {
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
    XirModuleBorrowConflict conflict = {.slot = UINT32_MAX, .callee = UINT32_MAX,
        .access_function = UINT32_MAX, .access_instruction = UINT32_MAX};
    const EffectCheckedRoots *roots = effects->checked_roots;
    const XrXirFunction *body = &module->functions[function];
    const XrXirInstruction *op = &body->instructions[instruction];
    if (op->op != XR_XIR_CALL && op->op != XR_XIR_INVOKE && op->op != XR_XIR_CALL_INDIRECT &&
        op->op != XR_XIR_INVOKE_INDIRECT && op->op != XR_XIR_CALL_REQUIREMENT) {
        *output = conflict;
        return XR_XIR_OK;
    }
    if (op->args[0] > body->operand_count || op->args[1] > body->operand_count - op->args[0])
        return XR_XIR_BAD_STRUCTURE;
    EffectCheckFunction row = roots->function[function];
    for (uint32_t a = 0; a < op->args[1]; ++a) {
        uint32_t operand = body->operands[op->args[0] + a];
        if (operand >= row.values) return XR_XIR_BAD_STRUCTURE;
        if (!xr_xir_type_is_cell(module->types, xr_xir_operand_type(body, operand)) &&
            xr_xir_place_kind(body, operand) == XR_XIR_PLACE_NONE) continue;
        for (uint32_t m = roots->node[row.first + operand].module_sets[EFFECT_MODULE_ORIGIN]; m != UINT32_MAX;) {
            EffectModuleMember origin = roots->module_member[m];
            m = origin.next;
            if (origin.kind != EFFECT_MODULE_SLOT) continue;
            uint32_t slot = origin.identity;
            for (uint32_t c = roots->node[row.root].calls; c != UINT32_MAX; c = roots->call[c].next) {
                EffectCheckCall call = roots->call[c];
                EffectCheckFunction callee = roots->function[roots->node[call.target].function];
                if (call.instruction != instruction || call.target != callee.root || call.protocol != 2 ||
                    ((op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE) && a < callee.capture_count)) continue;
                if (effect_module_conflict_call(roots, call, slot, &conflict)) goto done;
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
                            conflict.slot = slot;
                            conflict.callee = UINT32_MAX;
                            conflict.unknown = true;
                            goto done;
                        }
                        if (!receiver.erased && !receiver.callable_opaque &&
                            effect_module_conflict_set(roots, pending.receiver, EFFECT_MODULE_ACCESS,
                                function, slot, &conflict)) goto done;
                    }
                } else if (!pending.linked && effect_module_requirement_unknown(module->types, pending)) {
                    conflict.slot = slot;
                    conflict.callee = UINT32_MAX;
                    conflict.unknown = true;
                    goto done;
                }
            }
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
            if (conflict.slot == UINT32_MAX) continue;
            *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, f, UINT32_MAX, i,
                conflict.unknown ? XR_XIR_DIAGNOSTIC_MODULE_BORROW_UNKNOWN : XR_XIR_DIAGNOSTIC_MODULE_BORROW_ACCESS};
            return XR_XIR_BAD_TYPE;
        }
    }
    return XR_XIR_OK;
}
