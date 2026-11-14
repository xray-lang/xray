/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_module_query.inc.c - Exclusive module loans from owned effects
 */
static bool effect_module_conflict_member(const EffectCheckedRoots *roots, EffectModuleMember member,
    uint32_t function, uint32_t slot, bool write_only, XirModuleBorrowConflict *conflict) {
    uint8_t kind = (uint8_t)(member.kind & ~EFFECT_MODULE_WRITE);
    if (kind == EFFECT_MODULE_CALLBACK) {
        EffectCheckFunction row = roots->function[function];
        if (member.identity >= row.parameters) {
            conflict->slot = slot;
            conflict->unknown = true;
            return true;
        }
        if (!effect_module_unknown(&roots->node[row.first + member.identity])) return false;
        conflict->unknown = true;
    } else if (kind == EFFECT_MODULE_UNKNOWN) {
        conflict->unknown = true;
    } else if (kind != EFFECT_MODULE_SLOT || (slot != UINT32_MAX && member.identity != slot) ||
        (write_only && !(member.kind & EFFECT_MODULE_WRITE))) {
        return false;
    }
    conflict->slot = slot == UINT32_MAX && kind == EFFECT_MODULE_SLOT ? member.identity : slot;
    conflict->access_function = member.function;
    conflict->access_instruction = member.instruction;
    return true;
}

static bool effect_module_conflict_set(const EffectCheckedRoots *roots, uint32_t node, uint8_t property,
    uint32_t function, uint32_t slot, bool write_only, XirModuleBorrowConflict *conflict) {
    for (uint32_t m = roots->node[node].module_sets[property]; m != UINT32_MAX; m = roots->module_member[m].next)
        if (effect_module_conflict_member(roots, roots->module_member[m], function, slot, write_only, conflict)) return true;
    return false;
}

static bool effect_module_conflict_call(const EffectCheckedRoots *roots, EffectCheckCall call,
    uint32_t slot, uint32_t borrowed, bool write_only, XirModuleBorrowConflict *conflict) {
    if (call.fixed_effects) return false;
    EffectCheckFunction callee = roots->function[roots->node[call.target].function];
    uint32_t caller = roots->node[call.output].function;
    conflict->callee = callee.declaration;
    for (uint32_t m = roots->node[call.target].module_sets[EFFECT_MODULE_ACCESS]; m != UINT32_MAX;) {
        EffectModuleMember member = roots->module_member[m];
        m = member.next;
        uint8_t kind = (uint8_t)(member.kind & ~EFFECT_MODULE_WRITE);
        if (kind == EFFECT_MODULE_CALLBACK || kind == EFFECT_MODULE_CAPTURE || kind == EFFECT_MODULE_PARAMETER) {
            if (member.identity >= call.count) continue;
            uint32_t actual = roots->argument[call.first + member.identity];
            if (kind == EFFECT_MODULE_PARAMETER && actual == borrowed) continue;
            if (write_only && kind != EFFECT_MODULE_CALLBACK && !(member.kind & EFFECT_MODULE_WRITE)) continue;
            uint8_t property = kind == EFFECT_MODULE_CALLBACK ? EFFECT_MODULE_ACCESS : EFFECT_MODULE_ORIGIN;
            if (effect_module_conflict_set(roots, actual, property, caller, slot,
                kind == EFFECT_MODULE_CALLBACK && write_only, conflict)) return true;
        } else if (effect_module_conflict_member(roots, member, caller, slot, write_only, conflict)) {
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
        XrXirType type = xr_xir_operand_type(body, operand);
        const XrXirTypeNode *node = xr_xir_type_node(module->types, type);
        bool view = node && (node->kind == XR_XIR_TYPE_SLICE || node->kind == XR_XIR_TYPE_MUT_SLICE);
        bool write_only = view && node->kind == XR_XIR_TYPE_SLICE;
        if (!view && !xr_xir_type_is_cell(module->types, type) &&
            xr_xir_place_kind(body, operand) == XR_XIR_PLACE_NONE) continue;
        conflict.view = view;
        for (uint32_t m = roots->node[row.first + operand].module_sets[EFFECT_MODULE_ORIGIN]; m != UINT32_MAX;) {
            EffectModuleMember origin = roots->module_member[m];
            m = origin.next;
            if (origin.kind != EFFECT_MODULE_SLOT && !(view && origin.kind == EFFECT_MODULE_UNKNOWN)) continue;
            conflict.unknown_owner = origin.kind == EFFECT_MODULE_UNKNOWN;
            uint32_t slot = conflict.unknown_owner ? UINT32_MAX : origin.identity;
            for (uint32_t c = roots->node[row.root].calls; c != UINT32_MAX; c = roots->call[c].next) {
                EffectCheckCall call = roots->call[c];
                EffectCheckFunction callee = roots->function[roots->node[call.target].function];
                if (call.instruction != instruction || call.target != callee.root || call.protocol != 2 ||
                    ((op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE) && a < callee.capture_count)) continue;
                if (effect_module_conflict_call(roots, call, slot, row.first + operand, write_only, &conflict)) goto done;
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
                                function, slot, write_only, &conflict)) goto done;
                    }
                } else if (!pending.linked && effect_module_requirement_unknown(module->types, pending)) {
                    conflict.slot = slot;
                    conflict.callee = UINT32_MAX;
                    conflict.unknown = true;
                    goto done;
                }
            }
            conflict.unknown_owner = false;
        }
    }
done:
    *output = conflict;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_module_origin(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, uint32_t value, uint32_t slot, bool *output) {
    if (!output || !effects || !effects->checked_roots || function >= effects->count ||
        !xir_effects_context_matches(context, effects, effects->count)) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckedRoots *roots = effects->checked_roots;
    EffectCheckFunction row = roots->function[function];
    if (value >= row.values) return XR_XIR_BAD_STRUCTURE;
    *output = false;
    for (uint32_t m = roots->node[row.first + value].module_sets[EFFECT_MODULE_ORIGIN]; m != UINT32_MAX;
        m = roots->module_member[m].next) {
        EffectModuleMember member = roots->module_member[m];
        *output |= member.kind == EFFECT_MODULE_SLOT && member.identity == slot;
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_view_module_entry(const XrXirCompileContext *context,
    const XrXirEffects *effects, const XrXirModule *module, uint32_t function,
    uint32_t parameter, uint32_t accessed, bool *output) {
    if (!output || !module || !effects || !effects->checked_roots ||
        !xir_effects_context_matches(context, effects, module->function_count) ||
        function >= module->function_count || !module->declarations ||
        parameter >= module->functions[function].parameter_count) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckedRoots *roots = effects->checked_roots;
    EffectCheckFunction row = roots->function[function];
    if (accessed >= row.values) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *view = xr_xir_type_node(module->types, module->functions[function].parameters[parameter]);
    XrXirType storage = xr_xir_operand_type(&module->functions[function], accessed);
    if (xr_xir_type_is_cell(module->types, storage)) storage = xr_xir_cell_element(module->types, storage);
    const XrXirTypeNode *array = xr_xir_type_node(module->types, storage);
    bool present = false;
    if (view && view->kind == XR_XIR_TYPE_SLICE && array && array->kind == XR_XIR_TYPE_ARRAY &&
        view->element == array->element && roots->module_access) {
        for (uint32_t o = roots->node[row.first + accessed].module_sets[EFFECT_MODULE_ORIGIN]; o != UINT32_MAX;
            o = roots->module_member[o].next) {
            EffectModuleMember origin = roots->module_member[o];
            if (origin.kind != EFFECT_MODULE_SLOT) continue;
            for (uint32_t m = roots->node[row.root].module_sets[EFFECT_MODULE_ACCESS]; m != UINT32_MAX;
                m = roots->module_member[m].next) {
                EffectModuleMember member = roots->module_member[m];
                present |= member.kind == (EFFECT_MODULE_SLOT | EFFECT_MODULE_WRITE) && member.identity == origin.identity;
            }
        }
    }
    *output = present;
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
            if (conflict.slot == UINT32_MAX && !conflict.unknown_owner) continue;
            *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, f, UINT32_MAX, i,
                conflict.unknown ? XR_XIR_DIAGNOSTIC_MODULE_BORROW_UNKNOWN : XR_XIR_DIAGNOSTIC_MODULE_BORROW_ACCESS};
            return XR_XIR_BAD_TYPE;
        }
    }
    return XR_XIR_OK;
}
