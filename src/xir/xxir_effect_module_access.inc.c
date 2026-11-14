/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_module_access.inc.c - Qualified module access on effect equations
 *
 * KEY CONCEPT:
 *   Sparse binding and callback terms close on the existing dependency graph.
 *   A reference parameter's own loan is distinct from a captured binding.
 */
enum {
    EFFECT_MODULE_ACCESS, EFFECT_MODULE_ORIGIN,
    EFFECT_MODULE_TRANSFER_ACCESS = 1, EFFECT_MODULE_TRANSFER_ORIGIN = 2,
    EFFECT_MODULE_TRANSFER_CAPTURE = 4, EFFECT_MODULE_TRANSFER_EXECUTE = 8,
    EFFECT_MODULE_TRANSFER_WRITE = 16
};

static bool effect_module_view_type(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && (node->kind == XR_XIR_TYPE_SLICE || node->kind == XR_XIR_TYPE_MUT_SLICE);
}

static uint32_t effect_module_hash(uint32_t node, uint32_t identity, uint8_t kind, uint8_t property) {
    uint64_t pair = ((uint64_t)node << 32) | identity;
    return xr_hash_bytes(&pair, sizeof(pair)) ^ xr_hash_int((uint32_t)kind * 2u + property);
}

static bool effect_module_index(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    if (roots->module_members < roots->module_index_capacity / 2) return true;
    uint64_t capacity = roots->module_index_capacity ? (uint64_t)roots->module_index_capacity * 2 : 32;
    if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof(uint32_t)) {
        build->status = XR_XIR_OVERFLOW;
        return false;
    }
    uint32_t *index = xir_compile_calloc(build->memory, (size_t)capacity, sizeof(*index), &build->status);
    if (!index) return false;
    for (uint32_t m = 0; m < roots->module_members; ++m) {
        EffectModuleMember member = roots->module_member[m];
        uint32_t at = effect_module_hash(member.node, member.identity, member.kind, member.property) &
            ((uint32_t)capacity - 1);
        while (index[at]) at = (at + 1) & ((uint32_t)capacity - 1);
        index[at] = m + 1;
    }
    xr_compile_resources_free(roots->module_index);
    roots->module_index = index;
    roots->module_index_capacity = (uint32_t)capacity;
    return true;
}

static bool effect_module_add(EffectCheckBuild *build, uint32_t node, uint8_t property,
    uint8_t kind, uint32_t identity, uint32_t function, uint32_t instruction) {
    EffectCheckedRoots *roots = build->roots;
    if (build->status != XR_XIR_OK || !effect_module_index(build)) return false;
    uint32_t mask = roots->module_index_capacity - 1;
    uint32_t at = effect_module_hash(node, identity, kind, property) & mask;
    while (roots->module_index[at]) {
        EffectModuleMember member = roots->module_member[roots->module_index[at] - 1];
        if (member.node == node && member.property == property && member.kind == kind && member.identity == identity)
            return false;
        at = (at + 1) & mask;
    }
    if (roots->module_members == roots->module_member_capacity) {
        EffectModuleMember *members = xir_compile_array_grow(build->memory, roots->module_member,
            roots->module_members, sizeof(*members), &roots->module_member_capacity, &build->status);
        if (!members) return false;
        roots->module_member = members;
    }
    uint32_t member = roots->module_members++;
    roots->module_member[member] = (EffectModuleMember){node, identity,
        roots->node[node].module_sets[property], function, instruction, kind, property};
    roots->node[node].module_sets[property] = member;
    roots->module_index[at] = member + 1;
    return true;
}

static bool effect_module_seed(EffectCheckBuild *build, uint32_t node, uint8_t property,
    uint8_t kind, uint32_t identity, uint32_t instruction) {
    EffectCheckedRoots *roots = build->roots;
    if (roots->module_seeds == roots->module_seed_capacity) {
        EffectModuleSeed *seeds = xir_compile_array_grow(build->memory, roots->module_seed,
            roots->module_seeds, sizeof(*seeds), &roots->module_seed_capacity, &build->status);
        if (!seeds) return false;
        roots->module_seed = seeds;
    }
    uint32_t seed = roots->module_seeds++;
    roots->module_seed[seed] = (EffectModuleSeed){identity, roots->node[node].module_seeds,
        instruction, kind, property};
    roots->node[node].module_seeds = seed;
    return true;
}

static void effect_module_reset(EffectCheckedRoots *roots) {
    roots->module_members = 0;
    if (roots->module_index_capacity)
        memset(roots->module_index, 0, (size_t)roots->module_index_capacity * sizeof(*roots->module_index));
    for (uint32_t n = 0; n < roots->nodes; ++n)
        roots->node[n].module_sets[0] = roots->node[n].module_sets[1] = UINT32_MAX;
}

static bool effect_module_merge(EffectCheckBuild *build, uint32_t target, uint8_t property,
    uint32_t source, uint8_t source_property) {
    bool changed = false;
    for (uint32_t m = build->roots->node[source].module_sets[source_property]; m != UINT32_MAX;) {
        EffectModuleMember member = build->roots->module_member[m];
        changed |= effect_module_add(build, target, property, member.kind, member.identity,
            member.function, member.instruction);
        m = member.next;
    }
    return changed;
}

static bool effect_module_write(EffectCheckBuild *build, uint32_t target, uint32_t source,
    uint32_t function, uint32_t instruction) {
    bool changed = false;
    for (uint32_t m = build->roots->node[source].module_sets[EFFECT_MODULE_ORIGIN]; m != UINT32_MAX;) {
        EffectModuleMember member = build->roots->module_member[m];
        changed |= effect_module_add(build, target, EFFECT_MODULE_ACCESS,
            (uint8_t)(member.kind | EFFECT_MODULE_WRITE), member.identity, function, instruction);
        m = member.next;
    }
    return changed;
}

static bool effect_module_unknown(const EffectCheckNode *node) {
    return (node->bound | node->root_seed) != 0;
}

static bool effect_module_requirement_unknown(const XrXirTypes *types, EffectCheckPending pending) {
    const XrXirInterfaceTable *table = types ? types->interfaces : NULL;
    if (!table || pending.selector >= table->count ||
        pending.member >= table->declarations[pending.selector].method_count) return true;
    return effect_check_type_mask(types, table->declarations[pending.selector].methods[pending.member].signature) != 0;
}

/* A function value read from module storage may be whatever was last stored
 * there. Its type's ROOT promise is the only bound on what it touches; a type
 * that promises nothing root-free leaves the executed code unknown. */
static bool effect_module_stored_callable(EffectCheckBuild *build, const EffectCheckNode *node) {
    const XrXirTypes *types = effect_check_types_read(build);
    if (!xr_xir_type_is_callable(types, node->type) || !effect_check_type_mask(types, node->type)) return false;
    for (uint32_t m = node->module_sets[EFFECT_MODULE_ORIGIN]; m != UINT32_MAX; m = build->roots->module_member[m].next)
        if (build->roots->module_member[m].kind == EFFECT_MODULE_SLOT) return true;
    return false;
}

static bool effect_module_execute(EffectCheckBuild *build, uint32_t output, uint32_t source,
    uint32_t instruction) {
    EffectCheckNode node = build->roots->node[source];
    if (node.erased || node.callable_opaque) {
        return effect_module_unknown(&node) && effect_module_add(build, output, EFFECT_MODULE_ACCESS,
            EFFECT_MODULE_UNKNOWN, 0, node.function, instruction);
    }
    return effect_module_merge(build, output, EFFECT_MODULE_ACCESS, source, EFFECT_MODULE_ACCESS);
}

static bool effect_module_apply(EffectCheckBuild *build, uint32_t output, EffectCheckCall call) {
    EffectCheckedRoots *roots = build->roots;
    EffectCheckFunction callee = roots->function[roots->node[call.target].function];
    bool origin = call.target == callee.result && effect_module_view_type(effect_check_types_read(build),
        roots->node[call.target].type);
    if (call.fixed_effects && !origin) return false;
    uint8_t property = origin ? EFFECT_MODULE_ORIGIN : EFFECT_MODULE_ACCESS;
    bool changed = false;
    for (uint32_t m = roots->node[call.target].module_sets[property]; m != UINT32_MAX;) {
        EffectModuleMember member = roots->module_member[m];
        m = member.next;
        uint8_t kind = (uint8_t)(member.kind & ~EFFECT_MODULE_WRITE);
        if (kind == EFFECT_MODULE_CALLBACK || kind == EFFECT_MODULE_CAPTURE || kind == EFFECT_MODULE_PARAMETER) {
            uint32_t parameter = member.identity;
            if (parameter < call.count) {
                uint32_t actual = roots->argument[call.first + parameter];
                if (member.kind & EFFECT_MODULE_WRITE)
                    changed |= effect_module_write(build, output, actual, member.function, member.instruction);
                else changed |= effect_module_merge(build, output, property, actual,
                    kind == EFFECT_MODULE_CALLBACK ? EFFECT_MODULE_ACCESS : EFFECT_MODULE_ORIGIN);
            } else if (call.protocol == 1 && parameter < callee.parameters &&
                effect_module_unknown(&roots->node[callee.first + parameter])) {
                changed |= effect_module_add(build, output, EFFECT_MODULE_ACCESS,
                    EFFECT_MODULE_UNKNOWN, 0, callee.declaration, call.instruction);
            }
        } else {
            changed |= effect_module_add(build, output, property, member.kind,
                member.identity, member.function, member.instruction);
        }
    }
    return changed;
}

static bool effect_module_evaluate(EffectCheckBuild *build, uint32_t id) {
    EffectCheckedRoots *roots = build->roots;
    if (!roots->module_access) return false;
    EffectCheckNode node = roots->node[id];
    EffectCheckFunction function = roots->function[node.function];
    bool erased = node.erased;
    bool changed = false;
    for (uint32_t s = node.module_seeds; s != UINT32_MAX; s = roots->module_seed[s].next) {
        EffectModuleSeed seed = roots->module_seed[s];
        changed |= effect_module_add(build, id, seed.property, seed.kind, seed.identity,
            function.declaration, seed.instruction);
    }
    if (erased) {
        if (effect_module_unknown(&node))
            changed |= effect_module_add(build, id, EFFECT_MODULE_ACCESS, EFFECT_MODULE_UNKNOWN,
                0, function.declaration, UINT32_MAX);
    } else if (id >= function.first && id - function.first < function.parameters) {
        uint32_t parameter = id - function.first;
        if (node.callable_input)
            changed |= effect_module_add(build, id, EFFECT_MODULE_ACCESS, EFFECT_MODULE_CALLBACK,
                parameter, function.declaration, UINT32_MAX);
        bool view = effect_module_view_type(effect_check_types_read(build), node.type);
        if (node.cell || view)
            changed |= effect_module_add(build, id, EFFECT_MODULE_ORIGIN,
                parameter < function.capture_count ? EFFECT_MODULE_CAPTURE : EFFECT_MODULE_PARAMETER,
                parameter, function.declaration, UINT32_MAX);
        if (parameter < function.capture_count && node.cell) {
            for (uint32_t b = node.bindings; b != UINT32_MAX; b = roots->binding[b].next)
                changed |= effect_module_merge(build, id, EFFECT_MODULE_ORIGIN,
                    roots->binding[b].source, EFFECT_MODULE_ORIGIN);
        }
    } else if ((node.callable_opaque || (node.aliases == UINT32_MAX && node.calls == UINT32_MAX)) &&
        effect_module_unknown(&node) && (node.contents_value ||
            xr_xir_type_is_callable(effect_check_types_read(build), node.type))) {
        changed |= effect_module_add(build, id, EFFECT_MODULE_ACCESS, EFFECT_MODULE_UNKNOWN,
            0, function.declaration, UINT32_MAX);
    }
    if (effect_module_view_type(effect_check_types_read(build), node.type) &&
        id >= function.first + function.parameters && node.aliases == UINT32_MAX && node.calls == UINT32_MAX)
        changed |= effect_module_add(build, id, EFFECT_MODULE_ORIGIN, EFFECT_MODULE_UNKNOWN,
            0, function.declaration, UINT32_MAX);
    for (uint32_t a = node.aliases; a != UINT32_MAX; a = roots->alias[a].next) {
        EffectCheckAlias alias = roots->alias[a];
        if (!erased && (alias.module_transfer & EFFECT_MODULE_TRANSFER_ACCESS))
            changed |= effect_module_merge(build, id, EFFECT_MODULE_ACCESS, alias.source, EFFECT_MODULE_ACCESS);
        if (alias.module_transfer & EFFECT_MODULE_TRANSFER_ORIGIN)
            changed |= effect_module_merge(build, id, EFFECT_MODULE_ORIGIN, alias.source, EFFECT_MODULE_ORIGIN);
        if (alias.module_transfer & EFFECT_MODULE_TRANSFER_CAPTURE)
            changed |= effect_module_merge(build, id, EFFECT_MODULE_ACCESS, alias.source, EFFECT_MODULE_ORIGIN);
        if (alias.module_transfer & EFFECT_MODULE_TRANSFER_EXECUTE)
            changed |= effect_module_execute(build, id, alias.source, alias.instruction);
        if (alias.module_transfer & EFFECT_MODULE_TRANSFER_WRITE)
            changed |= effect_module_write(build, id, alias.source, function.declaration, alias.instruction);
    }
    for (uint32_t c = node.calls; !erased && c != UINT32_MAX; c = roots->call[c].next)
        changed |= effect_module_apply(build, id, roots->call[c]);
    if (!erased && effect_module_stored_callable(build, &roots->node[id]))
        changed |= effect_module_add(build, id, EFFECT_MODULE_ACCESS, EFFECT_MODULE_UNKNOWN,
            0, function.declaration, UINT32_MAX);
    if (id == function.root) {
        for (uint32_t p = 0; p < function.pending_count; ++p) {
            EffectCheckPending pending = roots->pending[function.pending_first + p];
            if (pending.linked || pending.root_linked) continue;
            if (pending.kind != EFFECT_CHECK_HIGHER_ORDER && pending.kind != EFFECT_CHECK_REQUIREMENT) continue;
            if (pending.kind == EFFECT_CHECK_REQUIREMENT &&
                !effect_module_requirement_unknown(effect_check_types_read(build), pending)) continue;
            changed |= effect_module_add(build, id, EFFECT_MODULE_ACCESS, EFFECT_MODULE_UNKNOWN,
                0, function.declaration, pending.instruction);
        }
    }
    return changed;
}

static bool effect_module_link(EffectCheckBuild *build, uint32_t target, uint32_t source,
    uint8_t transfer, uint32_t instruction) {
    if (source == UINT32_MAX || !effect_check_alias(build, target, source)) return false;
    EffectCheckAlias *alias = &build->roots->alias[build->roots->node[target].aliases];
    alias->module_transfer = transfer;
    alias->instruction = instruction;
    alias->root = alias->suspension = alias->callable_origin = false;
    return true;
}

static bool effect_module_instruction(EffectCheckBuild *build, uint32_t f, uint32_t i) {
    EffectCheckedRoots *roots = build->roots;
    if (!roots->module_access) return true;
    const XrXirInstruction *op = &build->module->functions[f].instructions[i];
    EffectCheckFunction function = roots->function[f];
    uint32_t value = function.first + function.parameters + i;
    if (op->op == XR_XIR_SLOT_LOAD || op->op == XR_XIR_SLOT_PLACE || op->op == XR_XIR_SLOT_STORE) {
        const XrXirDeclarations *declarations = build->module->declarations;
        if (op->immediate < 0 || (uint64_t)op->immediate >= declarations->slot_count) {
            build->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        uint32_t slot = (uint32_t)op->immediate;
        if (!declarations->slots[slot].mutable) return true;
        if (!effect_module_seed(build, function.root, EFFECT_MODULE_ACCESS, EFFECT_MODULE_SLOT, slot, i)) return false;
        if (op->op != XR_XIR_SLOT_STORE &&
            !effect_module_seed(build, value, EFFECT_MODULE_ORIGIN, EFFECT_MODULE_SLOT, slot, i)) return false;
        if (op->op == XR_XIR_SLOT_STORE && !effect_module_seed(build, function.root, EFFECT_MODULE_ACCESS,
            EFFECT_MODULE_SLOT | EFFECT_MODULE_WRITE, slot, i)) return false;
    }
    if (op->op == XR_XIR_FIELD_PLACE || op->op == XR_XIR_INDEX_PLACE || op->op == XR_XIR_OBJECT_PLACE ||
        op->op == XR_XIR_CELL_READ || op->op == XR_XIR_CELL_TAKE || op->op == XR_XIR_PLACE_READ ||
        op->op == XR_XIR_CELL_PROJECT || op->op == XR_XIR_VIEW_NEW || op->op == XR_XIR_VIEW_REBORROW) {
        uint32_t operand = op->args[0];
        if (op->op == XR_XIR_VIEW_NEW || op->op == XR_XIR_VIEW_REBORROW) {
            const XrXirFunction *body = &build->module->functions[f];
            if (!op->args[1] || operand >= body->operand_count) { build->status = XR_XIR_BAD_STRUCTURE; return false; }
            operand = body->operands[operand];
        }
        uint32_t source = effect_check_value(build, f, operand);
        if (!effect_module_link(build, value, source, EFFECT_MODULE_TRANSFER_ORIGIN, i)) return false;
    }
    if (op->op == XR_XIR_RETURN &&
        effect_module_view_type(effect_check_types_read(build), build->module->functions[f].result)) {
        uint32_t source = effect_check_value(build, f, op->args[0]);
        if (!effect_module_link(build, function.result, source, EFFECT_MODULE_TRANSFER_ORIGIN, i)) return false;
    }
    bool write = xr_xir_operand_role(op->op, 0) == XR_XIR_OPERAND_WRITE ||
        op->op == XR_XIR_CELL_WRITE || op->op == XR_XIR_CELL_WRITE_TAKE || op->op == XR_XIR_CELL_LOCAL_WRITE ||
        op->op == XR_XIR_CELL_STRING_APPEND || op->op == XR_XIR_CLASS_SET || op->op == XR_XIR_VIEW_SET;
    if (write) {
        uint32_t operand = op->args[0];
        if (xr_xir_op_uses_operand_table(op->op)) {
            const XrXirFunction *body = &build->module->functions[f];
            if (!op->args[1] || operand >= body->operand_count) { build->status = XR_XIR_BAD_STRUCTURE; return false; }
            operand = body->operands[operand];
        }
        uint32_t source = effect_check_value(build, f, operand);
        if (!effect_module_link(build, function.root, source, EFFECT_MODULE_TRANSFER_WRITE, i)) return false;
    }
    bool call = op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE || op->op == XR_XIR_CALL_INDIRECT ||
        op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_CALL_REQUIREMENT;
    if (call) {
        const XrXirFunction *body = &build->module->functions[f];
        if (op->args[0] > body->operand_count || op->args[1] > body->operand_count - op->args[0]) {
            build->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        uint32_t captures = 0;
        if ((op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE) && op->immediate >= 0 &&
            (uint64_t)op->immediate < build->module->function_count)
            captures = roots->function[op->immediate].capture_count;
        for (uint32_t p = captures; p < op->args[1]; ++p) {
            uint32_t operand = body->operands[op->args[0] + p];
            XrXirType type = xr_xir_operand_type(body, operand);
            if (!xr_xir_type_is_cell(effect_check_types_read(build), type) &&
                xr_xir_place_kind(body, operand) == XR_XIR_PLACE_NONE) continue;
            uint32_t source = effect_check_value(build, f, operand);
            if (!effect_module_link(build, function.root, source, EFFECT_MODULE_TRANSFER_WRITE, i)) return false;
        }
    }
    return true;
}
