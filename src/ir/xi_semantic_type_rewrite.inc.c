/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_semantic_type_rewrite.inc.c - Atomic owned Xi type-graph replacement
 *
 * Included by xi_semantic_snapshot.c to reuse its deep type copier. No
 * analyzer declaration, dependency module or published artifact is written.
 */

typedef struct XiTypeRewriteSlot {
    void *address;
    void *before;
    void *after;
} XiTypeRewriteSlot;

typedef struct XiTypeRewriteJournal {
    XiSemanticSnapshot snapshot;
    XiTypeRewriteSlot *slots;
    size_t count;
    size_t capacity;
} XiTypeRewriteJournal;

static bool type_rewrite_stage(XiTypeRewriteJournal *journal, void *address) {
    XrType *source = NULL;
    if (!address)
        return false;
    memcpy(&source, address, sizeof(source));
    if (!source)
        return true;
    for (size_t i = 0; i < journal->count; i++)
        if (journal->slots[i].address == address)
            return journal->slots[i].before == source;
    XrType *owned = snapshot_type(&journal->snapshot, source);
    if (!owned || journal->snapshot.failed)
        return false;
    if (owned == source)
        return true;
    if (journal->count == journal->capacity) {
        size_t capacity = journal->capacity ? journal->capacity * 2u : 64u;
        if (capacity > UINT32_MAX / sizeof(*journal->slots))
            return false;
        XiTypeRewriteSlot *slots = (XiTypeRewriteSlot *)
            xr_realloc(journal->slots, capacity * sizeof(*slots));
        if (!slots)
            return false;
        journal->slots = slots;
        journal->capacity = capacity;
    }
    journal->slots[journal->count++] = (XiTypeRewriteSlot) {address, source, owned};
    return true;
}

static bool type_rewrite_literals(XiTypeRewriteJournal *journal, XiConstLiteral *items,
                                  uint16_t count) {
    if (!items)
        return count == 0;
    for (uint16_t i = 0; i < count; i++)
        if (!type_rewrite_stage(journal, &items[i].type))
            return false;
    return true;
}

static bool type_rewrite_enum(XiTypeRewriteJournal *journal, XiEnumData *data) {
    if (!data)
        return true;
    if (snapshot_map_get(&journal->snapshot.enum_datas, data))
        return true;
    if (!snapshot_map_put(&journal->snapshot.enum_datas, data, data) ||
        (data->member_count && !data->members))
        return false;
    for (uint32_t i = 0; i < data->member_count; i++) {
        XiEnumMemberData *member = &data->members[i];
        if (member->payload_count < 0 || (member->payload_count && !member->payload_types))
            return false;
        for (int p = 0; p < member->payload_count; p++)
            if (!type_rewrite_stage(journal, &member->payload_types[p]))
                return false;
    }
    return true;
}

static bool type_rewrite_class(XiTypeRewriteJournal *journal, XiClassData *data) {
    if (!data)
        return true;
    if (snapshot_map_get(&journal->snapshot.class_datas, data))
        return true;
    if (!snapshot_map_put(&journal->snapshot.class_datas, data, data) ||
        (data->instance_field_count && !data->instance_field_types))
        return false;
    for (uint16_t i = 0; i < data->instance_field_count; i++)
        if (!type_rewrite_stage(journal, &data->instance_field_types[i]))
            return false;
    return true;
}

static bool type_rewrite_value(XiTypeRewriteJournal *journal, XiValue *value) {
    if (!value)
        return true;
    if (snapshot_map_get(&journal->snapshot.values, value))
        return true;
    if (!snapshot_map_put(&journal->snapshot.values, value, value) ||
        !type_rewrite_stage(journal, &value->type) ||
        !type_rewrite_stage(journal, &value->enum_metadata_owner))
        return false;
    if (value->op == XI_IS && value->aux) {
        if (!type_rewrite_stage(journal, &value->aux))
            return false;
    } else if (value->op == XI_CLASS_CREATE) {
        if (!type_rewrite_class(journal, (XiClassData *) value->aux))
            return false;
    } else if (value->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE) {
        if (!type_rewrite_enum(journal, (XiEnumData *) value->aux))
            return false;
    } else if (value->aux_kind == XI_AUX_KIND_PAR_FOR && value->aux) {
        XiParallelForData *data = (XiParallelForData *) value->aux;
        if (!type_rewrite_stage(journal, &data->state_type))
            return false;
    } else if (value->aux_kind == XI_AUX_KIND_PAR_MAP && value->aux) {
        XiParallelMapData *data = (XiParallelMapData *) value->aux;
        if (!type_rewrite_stage(journal, &data->element_type) ||
            !type_rewrite_stage(journal, &data->state_type))
            return false;
    } else if (value->aux_kind == XI_AUX_KIND_PAR_REDUCE && value->aux) {
        XiParallelReduceData *data = (XiParallelReduceData *) value->aux;
        if (!type_rewrite_stage(journal, &data->accumulator_type) ||
            !type_rewrite_stage(journal, &data->state_type))
            return false;
    }
    if (value->nargs && !value->args)
        return false;
    for (uint16_t i = 0; i < value->nargs; i++)
        if (!type_rewrite_value(journal, value->args[i]))
            return false;
    return true;
}

static bool type_rewrite_module(XiTypeRewriteJournal *journal, XiModule *module) {
    if (!module)
        return true;
    if ((module->nexports && !module->exports) || (module->nclasses && !module->classes))
        return false;
    for (uint16_t i = 0; i < module->nexports; i++)
        if (!type_rewrite_stage(journal, &module->exports[i].value_type))
            return false;
    for (uint16_t i = 0; i < module->nclasses; i++)
        if (!type_rewrite_class(journal, module->classes[i]))
            return false;
    for (uint16_t i = 0; i < module->nslots; i++) {
        if (module->slot_classes && !type_rewrite_class(journal, module->slot_classes[i]))
            return false;
        if (module->slot_enums && !type_rewrite_enum(journal, module->slot_enums[i]))
            return false;
    }
    return (!module->slot_const_literals ||
            type_rewrite_literals(journal, module->slot_const_literals, module->nslots)) &&
        (!module->slot_shared_initializers ||
         type_rewrite_literals(journal, module->slot_shared_initializers, module->nslots));
}

static bool type_rewrite_function(XiTypeRewriteJournal *journal, XiFunc *function) {
    if (!function || function->semantic_snapshot_detached || function->semantic_plan ||
        function->coro_plan || !type_rewrite_stage(journal, &function->return_type) ||
        !type_rewrite_stage(journal, &function->source_callable_type))
        return false;
    for (uint32_t i = 0; function->source_var_types && i < function->source_var_count; i++)
        if (!type_rewrite_stage(journal, &function->source_var_types[i]))
            return false;
    for (uint16_t i = 0; function->module_slots && i < function->nshared; i++)
        if (!type_rewrite_stage(journal, &function->module_slots[i].type))
            return false;
    for (uint16_t i = 0; i < function->ncaptures; i++)
        if (!type_rewrite_stage(journal, &function->captures[i].type))
            return false;
    if (!type_rewrite_literals(journal, function->shared_const_literals,
                                function->shared_const_literal_count) ||
        !type_rewrite_literals(journal, function->shared_init_literals,
                                function->shared_init_literal_count))
        return false;
    for (uint16_t i = 0; i < function->nparams; i++)
        if (!function->params || !type_rewrite_value(journal, function->params[i]))
            return false;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        XiBlock *block = function->blocks ? function->blocks[b] : NULL;
        if (!block)
            return false;
        for (XiPhi *phi = block->phis; phi; phi = phi->next)
            if (!type_rewrite_value(journal, &phi->value))
                return false;
        if (!type_rewrite_value(journal, block->control))
            return false;
        for (uint32_t v = 0; v < block->nvalues; v++)
            if (!block->values || !type_rewrite_value(journal, block->values[v]))
                return false;
    }
    if (function->module && function->module->init == function &&
        !type_rewrite_module(journal, function->module))
        return false;
    for (uint16_t i = 0; i < function->nchildren; i++)
        if (!function->children || !type_rewrite_function(journal, function->children[i]))
            return false;
    return true;
}

XR_FUNC bool xi_semantic_type_rewrite_atomic(XiFunc *root,
                                            const XiSemanticTypeReplacement *replacements,
                                            uint32_t replacement_count) {
    if (!root || !replacements || replacement_count == 0)
        return false;
    XiTypeRewriteJournal journal = {0};
    journal.snapshot.root = root;
    bool ok = true;
    for (uint32_t i = 0; ok && i < replacement_count; i++) {
        const XrType *source = replacements[i].source;
        const XrType *canonical = replacements[i].canonical;
        if (!source || source->frozen || !canonical || canonical == source) {
            ok = false;
            break;
        }
        XrType *owned = snapshot_type(&journal.snapshot, canonical);
        XrType *prior = (XrType *) snapshot_map_get(&journal.snapshot.types, source);
        ok = owned && (!prior || prior == owned) &&
             snapshot_map_put(&journal.snapshot.types, source, owned);
    }
    ok = ok && type_rewrite_function(&journal, root) && !journal.snapshot.failed;
    for (size_t i = 0; ok && i < journal.count; i++) {
        void *current = NULL;
        memcpy(&current, journal.slots[i].address, sizeof(current));
        ok = current == journal.slots[i].before;
    }
    if (ok)
        for (size_t i = 0; i < journal.count; i++)
            memcpy(journal.slots[i].address, &journal.slots[i].after, sizeof(void *));
    xr_free(journal.slots);
    snapshot_map_dispose(&journal.snapshot.types);
    snapshot_map_dispose(&journal.snapshot.values);
    snapshot_map_dispose(&journal.snapshot.enum_layouts);
    snapshot_map_dispose(&journal.snapshot.aggregate_layouts);
    snapshot_map_dispose(&journal.snapshot.class_infos);
    snapshot_map_dispose(&journal.snapshot.enum_datas);
    snapshot_map_dispose(&journal.snapshot.class_datas);
    return ok;
}
