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
 *   ACCESS holds what running a value or function may read or write by name,
 *   WRITE the part it may modify, and ORIGIN the bindings a value denotes.
 *   WRITE follows ACCESS through calls, callbacks and executed values, and
 *   every unknown access is an unknown write as well.
 */
enum {
    EFFECT_MODULE_ACCESS, EFFECT_MODULE_ORIGIN, EFFECT_MODULE_WRITE,
    EFFECT_MODULE_TRANSFER_ACCESS = 1, EFFECT_MODULE_TRANSFER_ORIGIN = 2,
    EFFECT_MODULE_TRANSFER_CAPTURE = 4, EFFECT_MODULE_TRANSFER_EXECUTE = 8
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
        roots->node[n].module_sets[0] = roots->node[n].module_sets[1] = roots->node[n].module_sets[2] = UINT32_MAX;
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

/* Packed field identities stay below ANY; ANY stands for every field and
 * NONE for no field at all. */
#define EFFECT_MODULE_FIELD_ANY (UINT32_MAX - 1u)
#define EFFECT_MODULE_FIELD_NONE UINT32_MAX

static bool effect_module_nominal(const XrXirNominalTable *table, uint32_t declaration,
    uint32_t *parent, uint32_t *fields) {
    if (!table || declaration >= table->count) return false;
    if (table->declarations) {
        *parent = table->declarations[declaration].parent;
        *fields = table->declarations[declaration].field_count;
    } else if (table->identities) {
        *parent = table->identities[declaration].parent;
        *fields = table->identities[declaration].field_count;
    } else {
        return false;
    }
    return true;
}

/* A subclass repeats its ancestors' fields as a prefix, so one field keeps one
 * identity whichever class reaches it: the oldest ancestor whose fields still
 * cover the ordinal declares it. An identity that does not pack is ANY. */
static uint32_t effect_module_field_identity(const XrXirTypes *types, XrXirType container,
    uint32_t ordinal) {
    const XrXirTypeNode *node = xr_xir_type_node(types, container);
    const XrXirNominalTable *table = types ? types->nominals : NULL;
    uint32_t parent = 0, fields = 0;
    if (!node || node->kind != XR_XIR_TYPE_NOMINAL ||
        !effect_module_nominal(table, node->nominal.declaration, &parent, &fields) || ordinal >= fields)
        return EFFECT_MODULE_FIELD_ANY;
    uint32_t declaration = node->nominal.declaration;
    for (uint32_t depth = 0; parent && depth < table->count; ++depth) {
        uint32_t above = 0, above_fields = 0;
        if (!effect_module_nominal(table, parent - 1, &above, &above_fields)) return EFFECT_MODULE_FIELD_ANY;
        if (above_fields <= ordinal) break;
        declaration = parent - 1;
        parent = above;
    }
    if (declaration >= (1u << 20) - 1u || ordinal >= (1u << 12)) return EFFECT_MODULE_FIELD_ANY;
    return declaration << 12 | ordinal;
}

/* The class field an instruction reads, writes or projects, if any. */
static bool effect_module_field_access(const XrXirTypes *types, const XrXirFunction *body,
    const XrXirInstruction *op, uint32_t *field) {
    if (op->op != XR_XIR_CLASS_GET && op->op != XR_XIR_CLASS_SET && op->op != XR_XIR_FIELD_PLACE) return false;
    if (op->args[0] >= (uint64_t)body->parameter_count + body->instruction_count || op->immediate < 0)
        return false;
    XrXirType container = xr_xir_operand_type(body, op->args[0]);
    if (!xr_xir_type_is_class(types, container)) return false;
    *field = op->immediate > UINT32_MAX ? EFFECT_MODULE_FIELD_ANY :
        effect_module_field_identity(types, container, (uint32_t)op->immediate);
    return true;
}

/* The class field a ref place argument lends. A ref path crosses at most the
 * class object at its root, so the first class field met walking back from
 * the place is the one whose storage the callee receives. */
static bool effect_module_lent_field(const XrXirTypes *types, const XrXirFunction *body,
    uint32_t value, uint32_t *field) {
    for (uint32_t steps = 0; steps <= body->instruction_count; ++steps) {
        if (value < body->parameter_count || value - body->parameter_count >= body->instruction_count)
            return false;
        const XrXirInstruction *op = &body->instructions[value - body->parameter_count];
        if (effect_module_field_access(types, body, op, field)) return op->op == XR_XIR_FIELD_PLACE;
        switch (op->op) {
        case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE: case XR_XIR_CELL_PROJECT:
        case XR_XIR_COPY: case XR_XIR_OWNED_RETAIN:
            value = op->args[0];
            break;
        default:
            return false;
        }
    }
    return false;
}

/* A ref argument reaches the callee as a Cell projection or as a place. */
static bool effect_module_loan_operand(const XrXirTypes *types, const XrXirFunction *body, uint32_t operand) {
    return xr_xir_type_is_cell(types, xr_xir_operand_type(body, operand)) ||
        xr_xir_place_kind(body, operand) != XR_XIR_PLACE_NONE;
}

static bool effect_module_borrow_op(XrXirOp op) {
    return op == XR_XIR_CALL || op == XR_XIR_INVOKE || op == XR_XIR_CALL_INDIRECT ||
        op == XR_XIR_INVOKE_INDIRECT || op == XR_XIR_CALL_REQUIREMENT;
}

static bool effect_module_field_lent(const EffectCheckedRoots *roots, uint32_t field) {
    uint32_t count = roots->lent_field_count;
    if (!count) return false;
    if (field == EFFECT_MODULE_FIELD_ANY || roots->lent_fields[count - 1] == EFFECT_MODULE_FIELD_ANY) return true;
    uint32_t low = 0, high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (roots->lent_fields[middle] < field) low = middle + 1;
        else high = middle;
    }
    return low < count && roots->lent_fields[low] == field;
}

static bool effect_module_lend_field(EffectCheckBuild *build, uint32_t field) {
    EffectCheckedRoots *roots = build->roots;
    uint32_t low = 0, high = roots->lent_field_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (roots->lent_fields[middle] < field) low = middle + 1;
        else high = middle;
    }
    if (low < roots->lent_field_count && roots->lent_fields[low] == field) return true;
    if (roots->lent_field_count == roots->lent_field_capacity) {
        uint32_t *fields = xir_compile_array_grow(build->memory, roots->lent_fields,
            roots->lent_field_count, sizeof(*fields), &roots->lent_field_capacity, &build->status);
        if (!fields) return false;
        roots->lent_fields = fields;
    }
    memmove(&roots->lent_fields[low + 1], &roots->lent_fields[low],
        (size_t)(roots->lent_field_count - low) * sizeof(*roots->lent_fields));
    roots->lent_fields[low] = field;
    ++roots->lent_field_count;
    return true;
}

/* Collects every class field some call lends by ref. Only these fields take
 * part in access sets: a field nobody lends cannot conflict with a loan. */
static bool effect_module_lent_fields(EffectCheckBuild *build) {
    const XrXirModule *module = build->module;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *body = &module->functions[f];
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            if (!(i % 256) && !effect_check_poll(build)) return false;
            const XrXirInstruction *op = &body->instructions[i];
            if (!effect_module_borrow_op(op->op) || op->args[0] > body->operand_count ||
                op->args[1] > body->operand_count - op->args[0]) continue;
            for (uint32_t a = 0; a < op->args[1]; ++a) {
                uint32_t operand = body->operands[op->args[0] + a], field = EFFECT_MODULE_FIELD_NONE;
                if (!effect_module_loan_operand(module->types, body, operand) ||
                    !effect_module_lent_field(module->types, body, operand, &field)) continue;
                if (!effect_module_lend_field(build, field)) return false;
            }
        }
    }
    return true;
}

static bool effect_module_first_operand(const XrXirFunction *body, const XrXirInstruction *op,
    uint32_t *value) {
    if (!xr_xir_op_uses_operand_table(op->op)) {
        *value = op->args[0];
        return true;
    }
    if (!op->args[1] || op->args[0] >= body->operand_count) return false;
    *value = body->operands[op->args[0]];
    return true;
}

XR_FUNC bool xir_module_slot_root(const XrXirModule *module, const XrXirFunction *body, uint32_t value,
    uint32_t *slot) {
    const XrXirDeclarations *declarations = module ? module->declarations : NULL;
    if (!body || !slot || !declarations || !declarations->slots) return false;
    for (uint32_t steps = 0; steps <= body->instruction_count; ++steps) {
        if (value < body->parameter_count || value - body->parameter_count >= body->instruction_count)
            return false;
        const XrXirInstruction *op = &body->instructions[value - body->parameter_count];
        switch (op->op) {
        case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_PLACE:
            if (op->immediate < 0 || (uint64_t)op->immediate >= declarations->slot_count ||
                !declarations->slots[op->immediate].mutable ||
                (op->op == XR_XIR_SLOT_LOAD && !xr_xir_type_is_cell(module->types, op->type))) return false;
            *slot = (uint32_t)op->immediate;
            return true;
        case XR_XIR_FIELD_PLACE:
            if (op->args[0] >= (uint64_t)body->parameter_count + body->instruction_count ||
                xr_xir_type_is_class(module->types, xr_xir_operand_type(body, op->args[0]))) return false;
            value = op->args[0];
            break;
        case XR_XIR_CELL_PLACE: case XR_XIR_CELL_PROJECT: case XR_XIR_INDEX_PLACE:
        case XR_XIR_COPY: case XR_XIR_OWNED_RETAIN: case XR_XIR_VIEW_NEW: case XR_XIR_VIEW_REBORROW:
            if (!effect_module_first_operand(body, op, &value)) return false;
            break;
        default:
            return false;
        }
    }
    return false;
}

static const uint8_t effect_module_operand_counts[XR_XIR_OP_COUNT] = {
    0,
#define XR_XIR_OP(name, stages, result, operands, edges, terminal) operands,
#include "xxir_ops.def"
#undef XR_XIR_OP
};

/* Instructions that only read the storage their operands denote, and the
 * projections whose result stands for that storage further on. Every other
 * instruction given module storage is taken to write it. */
static bool effect_module_storage_read_only(XrXirOp op) {
    switch (op) {
    case XR_XIR_CELL_READ: case XR_XIR_PLACE_READ: case XR_XIR_ARRAY_GET: case XR_XIR_ARRAY_LEN:
    case XR_XIR_ARRAY_CAPACITY: case XR_XIR_FIXED_ARRAY_GET: case XR_XIR_FIXED_ARRAY_LEN:
    case XR_XIR_MAP_GET: case XR_XIR_MAP_INDEX: case XR_XIR_MAP_ENTRIES: case XR_XIR_HASH_KEYS:
    case XR_XIR_HASH_VALUES: case XR_XIR_HASH_CONTAINS: case XR_XIR_HASH_LEN: case XR_XIR_HASH_KEY_AT:
    case XR_XIR_VIEW_LEN: case XR_XIR_VIEW_GET: case XR_XIR_VIEW_COPY: case XR_XIR_VIEW_END:
    case XR_XIR_VIEW_CLEANUP:
    case XR_XIR_CELL_PLACE: case XR_XIR_CELL_PROJECT: case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE:
    case XR_XIR_COPY: case XR_XIR_OWNED_RETAIN: case XR_XIR_VIEW_NEW: case XR_XIR_VIEW_REBORROW:
        return true;
    default:
        return op >= XR_XIR_SET_UNION && op <= XR_XIR_MAP_VALUE_AT;
    }
}

/* Seeds the module bindings a function may write in its own body. A Cell,
 * place or MutSlice handed to a call may be written by the callee, so the
 * caller records that write itself; this covers native callees and forwarded
 * ref parameters without per-parameter summaries. A Slice argument is a read. */
static bool effect_module_write_seeds(EffectCheckBuild *build, uint32_t f, uint32_t i) {
    const XrXirModule *module = build->module;
    const XrXirFunction *body = &module->functions[f];
    const XrXirInstruction *op = &body->instructions[i];
    if (op->op >= XR_XIR_OP_COUNT || effect_module_storage_read_only(op->op)) return true;
    bool table = xr_xir_op_uses_operand_table(op->op), call = effect_module_borrow_op(op->op);
    uint32_t count = table ? op->args[1] : effect_module_operand_counts[op->op];
    if (table && (op->args[0] > body->operand_count || count > body->operand_count - op->args[0])) return true;
    if (!table && count > sizeof(op->args) / sizeof(op->args[0])) count = sizeof(op->args) / sizeof(op->args[0]);
    uint32_t root = build->roots->function[f].root;
    for (uint32_t a = 0; a < count; ++a) {
        uint32_t operand = table ? body->operands[op->args[0] + a] : op->args[a], slot = UINT32_MAX;
        if (call) {
            const XrXirTypeNode *type = xr_xir_type_node(module->types, xr_xir_operand_type(body, operand));
            if (type && type->kind == XR_XIR_TYPE_SLICE) continue;
        }
        if (xir_module_slot_root(module, body, operand, &slot) &&
            !effect_module_seed(build, root, EFFECT_MODULE_WRITE, EFFECT_MODULE_SLOT, slot, i)) return false;
    }
    return true;
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

/* Unknown code may read and write any binding. */
static bool effect_module_add_unknown(EffectCheckBuild *build, uint32_t node, uint32_t function,
    uint32_t instruction) {
    bool changed = effect_module_add(build, node, EFFECT_MODULE_ACCESS, EFFECT_MODULE_UNKNOWN, 0,
        function, instruction);
    return effect_module_add(build, node, EFFECT_MODULE_WRITE, EFFECT_MODULE_UNKNOWN, 0,
        function, instruction) || changed;
}

/* Running `source` touches what running it was found to touch. */
static bool effect_module_merge_run(EffectCheckBuild *build, uint32_t target, uint32_t source) {
    bool changed = effect_module_merge(build, target, EFFECT_MODULE_ACCESS, source, EFFECT_MODULE_ACCESS);
    return effect_module_merge(build, target, EFFECT_MODULE_WRITE, source, EFFECT_MODULE_WRITE) || changed;
}

static bool effect_module_execute(EffectCheckBuild *build, uint32_t output, uint32_t source,
    uint32_t instruction) {
    EffectCheckNode node = build->roots->node[source];
    if (node.erased || node.callable_opaque)
        return effect_module_unknown(&node) && effect_module_add_unknown(build, output, node.function, instruction);
    return effect_module_merge_run(build, output, source);
}

static bool effect_module_apply(EffectCheckBuild *build, uint32_t output, EffectCheckCall call) {
    EffectCheckedRoots *roots = build->roots;
    EffectCheckFunction callee = roots->function[roots->node[call.target].function];
    bool origin = call.target == callee.result &&
        effect_module_view_type(effect_check_types_read(build), roots->node[call.target].type);
    if (call.fixed_effects && !origin) return false;
    bool changed = false;
    for (uint8_t property = origin ? EFFECT_MODULE_ORIGIN : EFFECT_MODULE_ACCESS;
        property <= (origin ? EFFECT_MODULE_ORIGIN : EFFECT_MODULE_WRITE); property += 2) {
        for (uint32_t m = roots->node[call.target].module_sets[property]; m != UINT32_MAX;) {
            EffectModuleMember member = roots->module_member[m];
            m = member.next;
            if (member.kind == EFFECT_MODULE_CALLBACK || member.kind == EFFECT_MODULE_CAPTURE ||
                member.kind == EFFECT_MODULE_PARAMETER) {
                uint32_t parameter = member.identity;
                if (parameter < call.count) {
                    uint32_t actual = roots->argument[call.first + parameter];
                    changed |= effect_module_merge(build, output, property, actual,
                        member.kind == EFFECT_MODULE_CALLBACK ? property : EFFECT_MODULE_ORIGIN);
                } else if (origin) {
                    changed |= effect_module_add(build, output, EFFECT_MODULE_ORIGIN,
                        EFFECT_MODULE_UNKNOWN, 0, callee.declaration, call.instruction);
                } else if (call.protocol == 1 && parameter < callee.parameters &&
                    effect_module_unknown(&roots->node[callee.first + parameter])) {
                    changed |= effect_module_add(build, output, property,
                        EFFECT_MODULE_UNKNOWN, 0, callee.declaration, call.instruction);
                }
            } else {
                changed |= effect_module_add(build, output, property, member.kind,
                    member.identity, member.function, member.instruction);
            }
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
            changed |= effect_module_add_unknown(build, id, function.declaration, UINT32_MAX);
    } else if (id >= function.first && id - function.first < function.parameters) {
        uint32_t parameter = id - function.first;
        if (node.callable_input) {
            changed |= effect_module_add(build, id, EFFECT_MODULE_ACCESS, EFFECT_MODULE_CALLBACK,
                parameter, function.declaration, UINT32_MAX);
            changed |= effect_module_add(build, id, EFFECT_MODULE_WRITE, EFFECT_MODULE_CALLBACK,
                parameter, function.declaration, UINT32_MAX);
        }
        bool view = effect_module_view_type(effect_check_types_read(build), node.type);
        if ((parameter < function.capture_count && node.cell) || view)
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
        changed |= effect_module_add_unknown(build, id, function.declaration, UINT32_MAX);
    }
    if (effect_module_view_type(effect_check_types_read(build), node.type) &&
        id >= function.first + function.parameters && node.aliases == UINT32_MAX && node.calls == UINT32_MAX &&
        (id < function.first + function.values || id == function.result))
        changed |= effect_module_add(build, id, EFFECT_MODULE_ORIGIN, EFFECT_MODULE_UNKNOWN,
            0, function.declaration, UINT32_MAX);
    for (uint32_t a = node.aliases; a != UINT32_MAX; a = roots->alias[a].next) {
        EffectCheckAlias alias = roots->alias[a];
        if (!erased && (alias.module_transfer & EFFECT_MODULE_TRANSFER_ACCESS))
            changed |= effect_module_merge_run(build, id, alias.source);
        if (alias.module_transfer & EFFECT_MODULE_TRANSFER_ORIGIN)
            changed |= effect_module_merge(build, id, EFFECT_MODULE_ORIGIN, alias.source, EFFECT_MODULE_ORIGIN);
        if (alias.module_transfer & EFFECT_MODULE_TRANSFER_CAPTURE) {
            changed |= effect_module_merge(build, id, EFFECT_MODULE_ACCESS, alias.source, EFFECT_MODULE_ORIGIN);
            changed |= effect_module_merge(build, id, EFFECT_MODULE_WRITE, alias.source, EFFECT_MODULE_ORIGIN);
        }
        if (alias.module_transfer & EFFECT_MODULE_TRANSFER_EXECUTE)
            changed |= effect_module_execute(build, id, alias.source, alias.instruction);
    }
    for (uint32_t c = node.calls; !erased && c != UINT32_MAX; c = roots->call[c].next)
        changed |= effect_module_apply(build, id, roots->call[c]);
    if (!erased && effect_module_stored_callable(build, &roots->node[id]))
        changed |= effect_module_add_unknown(build, id, function.declaration, UINT32_MAX);
    if (id == function.root) {
        for (uint32_t p = 0; p < function.pending_count; ++p) {
            EffectCheckPending pending = roots->pending[function.pending_first + p];
            if (pending.linked || pending.root_linked) continue;
            if (pending.kind != EFFECT_CHECK_HIGHER_ORDER && pending.kind != EFFECT_CHECK_REQUIREMENT) continue;
            if (pending.kind == EFFECT_CHECK_REQUIREMENT &&
                !effect_module_requirement_unknown(effect_check_types_read(build), pending)) continue;
            changed |= effect_module_add_unknown(build, id, function.declaration, pending.instruction);
        }
    }
    return changed;
}

static bool effect_module_origin_link(EffectCheckBuild *build, uint32_t target, uint32_t source) {
    if (source == UINT32_MAX || !effect_check_alias(build, target, source)) return false;
    EffectCheckAlias *alias = &build->roots->alias[build->roots->node[target].aliases];
    alias->module_transfer = EFFECT_MODULE_TRANSFER_ORIGIN;
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
        if (op->op == XR_XIR_SLOT_STORE &&
            !effect_module_seed(build, function.root, EFFECT_MODULE_WRITE, EFFECT_MODULE_SLOT, slot, i)) return false;
        if (op->op != XR_XIR_SLOT_STORE &&
            !effect_module_seed(build, value, EFFECT_MODULE_ORIGIN, EFFECT_MODULE_SLOT, slot, i)) return false;
    }
    if (!effect_module_write_seeds(build, f, i)) return false;
    if (op->op == XR_XIR_VIEW_NEW || op->op == XR_XIR_VIEW_REBORROW) {
        const XrXirFunction *body = &build->module->functions[f];
        if (!op->args[1] || op->args[0] >= body->operand_count) {
            build->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        uint32_t source = effect_check_value(build, f, body->operands[op->args[0]]);
        if (!effect_module_origin_link(build, value, source)) return false;
    }
    if (op->op == XR_XIR_RETURN &&
        effect_module_view_type(effect_check_types_read(build), build->module->functions[f].result)) {
        uint32_t source = effect_check_value(build, f, op->args[0]);
        if (!effect_module_origin_link(build, function.result, source)) return false;
    }
    uint32_t field = EFFECT_MODULE_FIELD_NONE;
    if (roots->lent_field_count &&
        effect_module_field_access(build->module->types, &build->module->functions[f], op, &field) &&
        effect_module_field_lent(roots, field) &&
        !effect_module_seed(build, function.root, EFFECT_MODULE_ACCESS, EFFECT_MODULE_FIELD, field, i))
        return false;
    if (op->op == XR_XIR_FIELD_PLACE || op->op == XR_XIR_INDEX_PLACE || op->op == XR_XIR_OBJECT_PLACE ||
        op->op == XR_XIR_CELL_READ || op->op == XR_XIR_CELL_TAKE || op->op == XR_XIR_PLACE_READ) {
        uint32_t source = effect_check_value(build, f, op->args[0]);
        if (source == UINT32_MAX || !effect_check_alias(build, value, source)) return false;
        roots->alias[roots->node[value].aliases].module_transfer = EFFECT_MODULE_TRANSFER_ORIGIN;
        roots->alias[roots->node[value].aliases].root = false;
        roots->alias[roots->node[value].aliases].suspension = false;
        roots->alias[roots->node[value].aliases].callable_origin = false;
    }
    return true;
}
