/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_errors.inc.c - Bounded escaping-error flow on verified CFGs
 *
 * KEY CONCEPT:
 *   Known enum variants and unknown errors are independent facts. Only a
 *   reachable error edge can contribute to an escaping summary. PHIs observe
 *   simultaneous predecessor values; copied immutable errors share filters.
 */
static bool error_bit(const uint64_t *set, uint32_t bit) {
    return (set[bit / 64] & ((uint64_t)1 << (bit % 64))) != 0;
}
static void error_add(uint64_t *set, uint32_t bit) { set[bit / 64] |= (uint64_t)1 << (bit % 64); }
bool xr_xir_effects_error(const XrXirEffects *effects, uint32_t f, XrXirType type, uint32_t variant) {
    if (!effects || f >= effects->count) return false;
    if ((uint32_t)type >= XR_XIR_CONSTRUCTED_TYPE_BASE && (uint32_t)type < XR_XIR_CONSTRUCTED_TYPE_LIMIT &&
        (uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE >= effects->source_type_count) return false;
    for (uint32_t a = 0; a < effects->atom_count; ++a)
        if (effects->atoms[a].type == type && effects->atoms[a].variant == variant)
            return error_bit(effects->errors + (size_t)f * effects->words, a + 2);
    return false;
}
bool xr_xir_effects_error_unknown(const XrXirEffects *effects, uint32_t f) {
    return effects && f < effects->count && error_bit(effects->errors + (size_t)f * effects->words, 0);
}
bool xr_xir_effects_error_unidentified(const XrXirEffects *effects, uint32_t f) {
    return effects && f < effects->count && error_bit(effects->errors + (size_t)f * effects->words, 1);
}
typedef struct ErrorInvocationContexts ErrorInvocationContexts;
static uint32_t error_invocation_summary(const ErrorInvocationContexts *contexts,uint32_t n);
enum { ERROR_BLOCK_REACHABLE = 1, ERROR_BLOCK_PENDING = 2 };
typedef struct ErrorFlow {
    const XrXirModule *module;
    const XrXirFunction *function;
    XrXirEffects *effects;
    XrXirCompileContext *remaining;
    EffectTerms *terms;
    uint64_t *states, *work, *edge, *snapshot, *escaping;
    uint8_t *reachable;
    uint32_t *roots, *cells, *slots, *active;
    const uint64_t *zero;
    uint8_t *storage;
    size_t storage_capacity;
    size_t stride;
    uint32_t values, cell_count, active_count;
    ErrorInvocationContexts *invocations;
    uint32_t invocation,summary_count;
    bool changed, summary_changed, restart;
} ErrorFlow;
static XrXirStatus error_invocation_contexts_new(ErrorFlow *flow,ErrorInvocationContexts **output);
static void error_invocation_contexts_free(ErrorInvocationContexts *contexts);
static XrXirStatus error_invocation_functions(ErrorFlow *flow);
static XrXirStatus error_invocation_summaries(ErrorFlow *flow);
static XrXirStatus error_invocation_call(ErrorFlow *flow,const XrXirInstruction *call,
    uint64_t *set,bool *handled);
/* Missing scalar rows are read-only zero facts, never writable aliases. */
static uint64_t *error_value(ErrorFlow *flow, uint64_t *row, uint32_t value) {
    if (value >= flow->values || flow->slots[value] == UINT32_MAX) return NULL;
    return row + (size_t)flow->slots[value] * flow->effects->words;
}
static const uint64_t *error_read(const ErrorFlow *flow, const uint64_t *row, uint32_t value) {
    if (value >= flow->values) return NULL;
    return flow->slots[value] == UINT32_MAX ? flow->zero :
        row + (size_t)flow->slots[value] * flow->effects->words;
}
/* Complete type verification precedes inference. Closed outer containers are
 * not Error atoms, even when an extracted payload has its own error facts. */
static bool error_closed_node_shape(const XrXirTypeNode *node) {
    if (node->kind != XR_XIR_TYPE_NOMINAL &&
        (node->nominal.declaration || node->nominal.arguments || node->nominal.argument_count ||
         node->nominal.fields || node->nominal.field_count)) return false;
    switch (node->kind) {
    case XR_XIR_TYPE_CALLABLE:
        return node->element == XR_XIR_UNIT && xr_xir_callable_flags_valid(node->flags) &&
            ((node->parameter_count != 0) == (node->parameters != NULL));
    case XR_XIR_TYPE_TUPLE:
        return node->element == XR_XIR_UNIT && node->result == XR_XIR_UNIT && !node->flags &&
            node->parameter_count && node->parameters;
    case XR_XIR_TYPE_ARRAY: case XR_XIR_TYPE_NULLABLE:
        return !node->parameters && !node->parameter_count && node->result == XR_XIR_UNIT && !node->flags;
    case XR_XIR_TYPE_ATOMIC:
        return !node->parameters && !node->parameter_count && node->result == XR_XIR_UNIT && !node->flags &&
            (node->element == XR_XIR_I64 || node->element == XR_XIR_F64 || node->element == XR_XIR_BOOL);
    case XR_XIR_TYPE_NOMINAL:
        return node->element == XR_XIR_UNIT && !node->parameters && !node->parameter_count &&
            node->result == XR_XIR_UNIT && !node->flags &&
            ((node->nominal.argument_count != 0) == (node->nominal.arguments != NULL)) &&
            ((node->nominal.field_count != 0) == (node->nominal.fields != NULL));
    default: return false;
    }
}
static XrXirStatus error_row_active(const ErrorFlow *flow, uint32_t value, bool *active) {
    if (!flow || !flow->function || !active ||
        (uint64_t)value >= (uint64_t)flow->function->parameter_count + flow->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    if (value >= flow->function->parameter_count) {
        if (!flow->function->instructions) return XR_XIR_BAD_STRUCTURE;
        XrXirOp op = flow->function->instructions[value - flow->function->parameter_count].op;
        if (op == XR_XIR_GO || op == XR_XIR_INVOKE_ERROR) { *active = true; return XR_XIR_OK; }
    } else if (!flow->function->parameters) return XR_XIR_BAD_STRUCTURE;
    XrXirType type = xr_xir_operand_type(flow->function, value);
    switch (type) {
    case XR_XIR_UNIT: case XR_XIR_BOOL: case XR_XIR_I64: case XR_XIR_STRING:
    case XR_XIR_I8: case XR_XIR_I16: case XR_XIR_I32: case XR_XIR_U8:
    case XR_XIR_U16: case XR_XIR_U32: case XR_XIR_U64: case XR_XIR_F32:
    case XR_XIR_F64: case XR_XIR_PANIC_INFO: case XR_XIR_RUNE:
        *active = false; return XR_XIR_OK;
    default: break;
    }
    const XrXirTypes *types = flow->module ? flow->module->types : NULL;
    uint32_t id = (uint32_t)type;
    if (!types || !types->nodes || id < XR_XIR_CONSTRUCTED_TYPE_BASE ||
        id >= XR_XIR_CONSTRUCTED_TYPE_LIMIT) { *active = true; return XR_XIR_OK; }
    if (!xir_compile_work(flow->remaining, 1)) return XR_XIR_BUDGET;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    if (!node || node->parameter_span || !error_closed_node_shape(node)) {
        *active = true; return XR_XIR_OK;
    }
    if (node->kind != XR_XIR_TYPE_NOMINAL) { *active = false; return XR_XIR_OK; }
    const XrXirNominalTable *table = types->nominals;
    if (!table || node->nominal.declaration >= table->count ||
        (table->declarations != NULL) == (table->identities != NULL)) {
        *active = true; return XR_XIR_OK;
    }
    if (!xir_compile_work(flow->remaining, 1)) return XR_XIR_BUDGET;
    uint32_t d = node->nominal.declaration;
    uint32_t kind = table->declarations ? table->declarations[d].kind : table->identities[d].kind;
    *active = kind != XR_XIR_NOMINAL_STRUCT && kind != XR_XIR_NOMINAL_CLASS;
    return XR_XIR_OK;
}
static bool error_any(const uint64_t *set, uint32_t words) {
    for (uint32_t w = 0; w < words; ++w) if (set[w]) return true;
    return false;
}
static bool error_join(uint64_t *to, const uint64_t *from, size_t words) {
    bool changed = false;
    for (size_t w = 0; w < words; ++w) {
        uint64_t joined = to[w] | from[w]; changed |= joined != to[w]; to[w] = joined;
    }
    return changed;
}
static void error_type(ErrorFlow *flow, XrXirType type, uint64_t *set) {
    if (xr_xir_type_is_cell(flow->module->types, type)) type = xr_xir_cell_element(flow->module->types, type);
    const XrXirTypeNode *node = xr_xir_type_node(flow->module->types, type);
    if (type == XR_XIR_ERROR || (node && node->kind == XR_XIR_TYPE_TASK)) error_add(set, 1);
    else for (uint32_t a = 0; a < flow->effects->atom_count; ++a)
        if (flow->effects->atoms[a].type == type) error_add(set, a + 2);
}
static const XrXirInstruction *error_definition(ErrorFlow *flow, uint32_t value) {
    return value >= flow->function->parameter_count && value < flow->values ?
        &flow->function->instructions[value - flow->function->parameter_count] : NULL;
}
static bool error_copy_op(XrXirOp op) {
    return op == XR_XIR_COPY || op == XR_XIR_SCALAR_COPY || op == XR_XIR_OWNED_RETAIN ||
        op == XR_XIR_ERROR_ERASE || op == XR_XIR_ERROR_NARROW;
}
static XrXirStatus error_roots(ErrorFlow *flow) {
    for (uint32_t v = 0; v < flow->values; ++v) {
        uint32_t root = v;
        for (;;) {
            if (!xir_compile_work(flow->remaining, 1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op = error_definition(flow, root);
            if (!op || !error_copy_op(op->op)) break;
            root = op->args[0];
        }
        flow->roots[v] = root;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_atom(ErrorFlow *flow, EffectErrorAtom atom, uint64_t *set) {
    XrXirStatus allocation_status = XR_XIR_OK;
    XrXirEffects *effects = flow->effects;
    for (uint32_t a = 0; a < effects->atom_count; ++a) {
        if (!xir_compile_work(flow->remaining, 1)) return XR_XIR_BUDGET;
        if (effects->atoms[a].type == atom.type && effects->atoms[a].variant == atom.variant) {
            error_add(set, a + 2); return XR_XIR_OK;
        }
    }
    if (effects->atom_count == UINT32_MAX - 2u) return XR_XIR_BUDGET;
    if (effects->atom_count == effects->atom_capacity) {
        uint64_t capacity = effects->atom_capacity ? (uint64_t)effects->atom_capacity * 2 : 8;
        if (capacity > UINT32_MAX - 2u) capacity = UINT32_MAX - 2u;
        uint64_t bytes = capacity * sizeof(EffectErrorAtom);
        if (bytes > SIZE_MAX ||
            !xir_compile_work(flow->remaining, capacity + effects->atom_count)) return XR_XIR_BUDGET;
        EffectErrorAtom *atoms = xir_compile_calloc(flow->remaining, (size_t)capacity, sizeof(*atoms), &allocation_status);
        if (!atoms) return allocation_status;

        if (effects->atom_count) { if (!xir_compile_work(flow->remaining, (size_t)effects->atom_count * sizeof(*atoms))) { xr_compile_resources_free(atoms); return XR_XIR_BUDGET; } memcpy(atoms, effects->atoms, (size_t)effects->atom_count * sizeof(*atoms)); }
        xr_compile_resources_free(effects->atoms); effects->atoms = atoms; effects->atom_capacity = (uint32_t)capacity;
    }
    effects->atoms[effects->atom_count++] = atom;
    flow->restart = true;
    return XR_XIR_OK;
}
static XrXirStatus error_call_summary(ErrorFlow *flow, const XrXirInstruction *call,
    const uint64_t *from,uint64_t *set) {
    uint32_t caller = (uint32_t)(flow->function - flow->module->functions);
    if (error_bit(from, 0)) error_add(set, 0);
    if (error_bit(from, 1)) error_add(set, 1);
    for (uint32_t a = 0; a < flow->effects->atom_count; ++a) if (error_bit(from, a + 2)) {
        EffectErrorAtom atom = flow->effects->atoms[a];
        if (!xir_compile_work(flow->remaining, (uint64_t)flow->effects->atom_count + 1)) return XR_XIR_BUDGET;
        if (atom.variant == XR_XIR_ERROR_SYMBOLIC_VARIANT) {
            uint32_t p = (uint32_t)atom.type - XR_XIR_TYPE_PARAMETER_BASE;
            if (p >= call->type_arguments[1]) return XR_XIR_BAD_TYPE;
            XrXirType type = flow->module->generics[caller].arguments[call->type_arguments[0] + p];
            error_type(flow, type, set); continue;
        }
        if (call->type_arguments[1]) {
            XrXirGeneric arguments = {0}; arguments.argument_count = call->type_arguments[1];
            arguments.arguments = flow->module->generics[caller].arguments + call->type_arguments[0];
            XrXirStatus status = effect_terms_substitute(flow->terms, atom.type, &arguments, &atom.type);
            if (status != XR_XIR_OK) return status;
        }
        XrXirStatus status = error_atom(flow, atom, set);
        if (status != XR_XIR_OK || flow->restart) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus error_call(ErrorFlow *flow, const XrXirInstruction *call, uint64_t *set) {
    bool handled=false;
    XrXirStatus precise=error_invocation_call(flow,call,set,&handled);
    if (precise!=XR_XIR_OK || handled) return precise;
    if (call->op == XR_XIR_CALL_INDIRECT || call->op == XR_XIR_INVOKE_INDIRECT || call->op == XR_XIR_CALL_REQUIREMENT) {
        error_add(set, 0); return XR_XIR_OK;
    }
    XrXirInstruction resolved;
    if(call->op==XR_XIR_CALL_DEFAULT || call->op==XR_XIR_INVOKE_DEFAULT) {
        const XrXirDefaultBinding *binding=NULL;
        const uint32_t *identity=xr_xir_default_identity(call);
        XrXirStatus status=xr_xir_compile_default_lookup(flow->remaining, flow->module, identity[0], identity[1], &binding);
        if(status!=XR_XIR_OK) return status;
        if(!binding) return XR_XIR_BAD_STRUCTURE;
        resolved=*call;resolved.immediate=binding->function;call=&resolved;
    }
    const uint64_t *from = flow->effects->errors + (size_t)call->immediate * flow->effects->words;
    return error_call_summary(flow,call,from,set);
}

/* Calls, scheduler/host boundaries and writes through other references can
 * change any aliased cell.
 * Immutable values already read from a cell keep their independent facts. */
static XrXirStatus error_cells(ErrorFlow *flow, uint32_t target, const uint64_t *value) {
    for (uint32_t c = 0; c < flow->cell_count; ++c) {
        if (!xir_compile_work(flow->remaining, flow->effects->words + (uint64_t)flow->effects->atom_count + 1))
            return XR_XIR_BUDGET;
        uint32_t v = flow->cells[c];
        if (v >= flow->values) return XR_XIR_BAD_STRUCTURE;
        XrXirType type = xr_xir_operand_type(flow->function,v);
        if (!xr_xir_type_is_cell(flow->module->types,type)) return XR_XIR_BAD_STRUCTURE;
        uint64_t *set = error_value(flow,flow->work,v);
        if (!set) return XR_XIR_BAD_STRUCTURE;
        { if (!xir_compile_work(flow->remaining, (size_t)flow->effects->words*sizeof(uint64_t))) { return XR_XIR_BUDGET; } memset(set,0,(size_t)flow->effects->words*sizeof(uint64_t)); }
        if (target != UINT32_MAX && flow->roots[v] == flow->roots[target])
            { if (!xir_compile_work(flow->remaining, (size_t)flow->effects->words*sizeof(uint64_t))) { return XR_XIR_BUDGET; } memcpy(set,value,(size_t)flow->effects->words*sizeof(uint64_t)); }
        else error_type(flow,type,set);
    }
    return XR_XIR_OK;
}
static XrXirStatus error_instruction(ErrorFlow *flow, uint32_t i) {
    const XrXirInstruction *op = &flow->function->instructions[i];
    uint32_t words = flow->effects->words;
    if (!xir_compile_work(flow->remaining, (uint64_t)words + flow->effects->atom_count + 1)) return XR_XIR_BUDGET;
    uint64_t *out = error_value(flow, flow->work, flow->function->parameter_count + i);
    if (op->op == XR_XIR_PHI) return XR_XIR_OK;
    if (out) { if (!xir_compile_work(flow->remaining, (size_t)words * sizeof(*out))) { return XR_XIR_BUDGET; } memset(out, 0, (size_t)words * sizeof(*out)); }
    if (op->op == XR_XIR_CELL_WRITE) {
        XrXirType cell=xr_xir_operand_type(flow->function,op->args[0]);
        const uint64_t *source=xr_xir_cell_payload_operands(flow->module->types,cell,op->op)==1 ?
            flow->zero : error_read(flow,flow->work,op->args[1]);
        { if (!xir_compile_work(flow->remaining, (size_t)words*sizeof(uint64_t))) { return XR_XIR_BUDGET; } memcpy(flow->snapshot,source,(size_t)words*sizeof(uint64_t)); }
        XrXirStatus status = error_cells(flow,op->args[0],flow->snapshot);
        if (status != XR_XIR_OK) return status;
    }
    if (error_copy_op(op->op) || op->op == XR_XIR_CELL_NEW ||
        (op->op == XR_XIR_CELL_READ && !xr_xir_type_is_cell(flow->module->types,op->type)) || op->op == XR_XIR_LOCAL_NEW || op->op == XR_XIR_OWNED_LOCAL_NEW ||
        op->op == XR_XIR_SCALAR_LOCAL_NEW || op->op == XR_XIR_LOCAL_READ ||
        op->op == XR_XIR_OWNED_LOCAL_READ || op->op == XR_XIR_SCALAR_LOCAL_READ)
        { if (out) {
            const uint64_t *source=op->op==XR_XIR_CELL_NEW &&
                !xr_xir_cell_payload_operands(flow->module->types,op->type,op->op) ?
                flow->zero : error_read(flow,flow->work,op->args[0]);
            if (!xir_compile_work(flow->remaining, (size_t)words * sizeof(*out))) return XR_XIR_BUDGET;
            memcpy(out,source,(size_t)words*sizeof(*out));
        } }
    else if (op->op == XR_XIR_LOCAL_WRITE || op->op == XR_XIR_OWNED_LOCAL_WRITE || op->op == XR_XIR_SCALAR_LOCAL_WRITE)
        {
            uint64_t *target = error_value(flow, flow->work, op->args[0]);
            const uint64_t *source = error_read(flow, flow->work, op->args[1]);
            if (target && target != source) {
                if (!xir_compile_work(flow->remaining, (size_t)words * sizeof(*target))) return XR_XIR_BUDGET;
                memcpy(target, source, (size_t)words * sizeof(*target));
            }
        }
    else if (op->op == XR_XIR_CELL_PROJECT) {
        /* The descriptor has no leaf SSA payload or independent value.
         * Existing writes invalidate all other COPY identities. */
        if (!out) return XR_XIR_BAD_STRUCTURE;
        error_type(flow,op->type,out);
    }
    else if (op->op == XR_XIR_ENUM_NEW) {
        if (!out) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a = 0; a < flow->effects->atom_count; ++a)
            if (flow->effects->atoms[a].type == op->type && flow->effects->atoms[a].variant == (uint32_t)op->immediate)
                error_add(out, a + 2);
    } else if (op->op == XR_XIR_GO) {
        if (!out) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status = error_call(flow, op, out);
        if (status != XR_XIR_OK || flow->restart) return status;
    } else if (op->op == XR_XIR_INVOKE_ERROR) {
        if (!out) return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *call = &flow->function->instructions[op->immediate];
        if (call->op == XR_XIR_TASK_AWAIT) {
            if (!xir_compile_work(flow->remaining, (uint64_t)words * sizeof(*out))) return XR_XIR_BUDGET;
            memcpy(out, error_read(flow, flow->work, call->args[0]), (size_t)words * sizeof(*out));
            return XR_XIR_OK;
        }
        XrXirStatus status=error_call(flow, call, out);
        if (status!=XR_XIR_OK || flow->restart) return status;
    }
    else if (out) error_type(flow, op->type, out);
    if (op->op == XR_XIR_THROW)
        flow->summary_changed |= error_join(flow->escaping, error_read(flow, flow->work, op->args[0]), words);
    else if (op->op == XR_XIR_CALL_DEFAULT || op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_CALL_REQUIREMENT) {
        uint64_t *temporary = flow->snapshot;
        { if (!xir_compile_work(flow->remaining, (size_t)words * sizeof(*temporary))) { return XR_XIR_BUDGET; } memset(temporary, 0, (size_t)words * sizeof(*temporary)); }
        XrXirStatus status=error_call(flow, op, temporary); if (status!=XR_XIR_OK || flow->restart) return status;
        flow->summary_changed |= error_join(flow->escaping, temporary, words);
    }
    if (op->op == XR_XIR_CALL_DEFAULT || op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_CALL_REQUIREMENT ||
        op->op == XR_XIR_INVOKE_DEFAULT || op->op == XR_XIR_INVOKE || op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_SUSPEND || op->op == XR_XIR_TIMER_AFTER_MS ||
        op->op == XR_XIR_OUTPUT || op->op == XR_XIR_WRITE_STREAM || op->op == XR_XIR_PRINT ||
        op->op == XR_XIR_GO || op->op == XR_XIR_TASK_AWAIT ||
        op->op == XR_XIR_CLEANUP_LEAVE || op->op == XR_XIR_CLEANUP_ERROR || op->op == XR_XIR_CELL_LOCAL_WRITE)
        return error_cells(flow,UINT32_MAX,NULL);
    return XR_XIR_OK;
}
/* Return false only when a recognized filter excludes the complete value set. */
static bool error_filter(ErrorFlow *flow, const XrXirInstruction *branch, bool yes) {
    const XrXirInstruction *test = error_definition(flow, branch->args[0]);
    if (!test) return true;
    uint32_t value;
    XrXirType type = XR_XIR_UNIT;
    int64_t variant = -1;
    if (test->op == XR_XIR_ERROR_IS) { value = test->args[0]; type = (XrXirType)test->immediate; }
    else if (test->op == XR_XIR_EQ_INT || test->op == XR_XIR_NE_INT) {
        const XrXirInstruction *tag = error_definition(flow, test->args[0]);
        const XrXirInstruction *constant = error_definition(flow, test->args[1]);
        if (!tag || !constant) return true;
        if (tag->op == XR_XIR_CONST_INT) { const XrXirInstruction *swap = tag; tag = constant; constant = swap; }
        if (tag->op != XR_XIR_ENUM_TAG || constant->op != XR_XIR_CONST_INT) return true;
        value = tag->args[0]; variant = constant->immediate;
        if (variant < 0) return test->op == XR_XIR_EQ_INT ? !yes : yes;
        type = xr_xir_operand_type(flow->function, value);
        if (test->op == XR_XIR_NE_INT) yes = !yes;
    } else return true;
    uint32_t root = flow->roots[value];
    for (uint32_t slot = 0; slot < flow->active_count; ++slot) {
        uint32_t v = flow->active[slot];
        if (flow->roots[v] != root) continue;
        uint64_t *set = error_value(flow, flow->edge, v);
        for (uint32_t a = 0; a < flow->effects->atom_count; ++a) {
            if (flow->effects->atoms[a].variant==XR_XIR_ERROR_SYMBOLIC_VARIANT) continue;
            bool match = flow->effects->atoms[a].type == type &&
                (variant < 0 || flow->effects->atoms[a].variant == (uint64_t)variant);
            if (match != yes) set[(a + 2) / 64] &= ~((uint64_t)1 << ((a + 2) % 64));
        }
    }
    return error_any(error_read(flow, flow->edge, value), flow->effects->words);
}
static XrXirStatus error_edge(ErrorFlow *flow, uint32_t from, uint32_t to,
    const XrXirInstruction *branch, bool yes) {
    if (!xir_compile_work(flow->remaining, (uint64_t)flow->stride * 3) ||
        !xir_compile_work(flow->remaining, (uint64_t)flow->active_count * ((uint64_t)flow->effects->atom_count + 1)))
        return XR_XIR_BUDGET;
    /* Both original charges precede metadata access, including extreme probes. */
    const XrXirBlock *block = &flow->function->blocks[to];
    bool has_phi = block->count && flow->function->instructions[block->first].op == XR_XIR_PHI;
    bool panic_cells = flow->function->blocks[from].panic == to &&
        flow->function->blocks[from].frontier != block->frontier;
    uint64_t *incoming = flow->work;
    if (branch || has_phi || panic_cells) {
        { if (!xir_compile_work(flow->remaining, flow->stride * sizeof(uint64_t))) { return XR_XIR_BUDGET; } memcpy(flow->edge, flow->work, flow->stride * sizeof(uint64_t)); }
        if (flow->function->blocks[from].panic == to &&
            flow->function->blocks[from].frontier != flow->function->blocks[to].frontier) {
            uint64_t *saved = flow->work;
            flow->work = flow->edge;
            XrXirStatus status = error_cells(flow, UINT32_MAX, NULL);
            flow->work = saved;
            if (status != XR_XIR_OK) return status;
        }
        if (branch && !error_filter(flow, branch, yes)) return XR_XIR_OK;
        if (block->count && flow->function->instructions[block->first].op == XR_XIR_PHI) {
            if (!xir_compile_work(flow->remaining, flow->stride * sizeof(uint64_t))) return XR_XIR_BUDGET;
            memcpy(flow->snapshot, flow->edge, flow->stride * sizeof(uint64_t));
        }
        for (uint32_t i = block->first; i < block->first + block->count; ++i) {
            const XrXirInstruction *phi = &flow->function->instructions[i];
            if (phi->op != XR_XIR_PHI) break;
            for (uint32_t p = 0; p < phi->args[1]; p += 2) {
                if (!xir_compile_work(flow->remaining, 1)) return XR_XIR_BUDGET;
                if (flow->function->operands[phi->args[0] + p] != from) continue;
                uint32_t value = flow->function->operands[phi->args[0] + p + 1];
                uint64_t *out = error_value(flow, flow->edge, flow->function->parameter_count + i);
                if (out) {
                    if (!xir_compile_work(flow->remaining, (size_t)flow->effects->words * sizeof(*out))) return XR_XIR_BUDGET;
                    memcpy(out, error_read(flow, flow->snapshot, value), (size_t)flow->effects->words * sizeof(*out));
                }
            }
        }
        incoming = flow->edge;
    }
    bool changed = error_join(flow->states + (size_t)to * flow->stride, incoming, flow->stride);
    if (!(flow->reachable[to] & ERROR_BLOCK_REACHABLE)) {
        flow->reachable[to] |= ERROR_BLOCK_REACHABLE;
        changed = true;
    }
    if (changed) {
        flow->reachable[to] |= ERROR_BLOCK_PENDING;
        flow->changed = true;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_block(ErrorFlow *flow, uint32_t b) {
    const XrXirBlock *block = &flow->function->blocks[b];
    if (!xir_compile_work(flow->remaining, flow->stride)) return XR_XIR_BUDGET;
    { if (!xir_compile_work(flow->remaining, flow->stride * sizeof(uint64_t))) { return XR_XIR_BUDGET; } memcpy(flow->work, flow->states + (size_t)b * flow->stride, flow->stride * sizeof(uint64_t)); }
    for (uint32_t i = block->first; i < block->first + block->count; ++i) {
        XrXirStatus status = block->panic ? error_edge(flow, b, block->panic, NULL, true) : XR_XIR_OK;
        if (status == XR_XIR_OK) status = error_instruction(flow, i);
        if (status != XR_XIR_OK || flow->restart) return status;
    }
    const XrXirInstruction *end = &flow->function->instructions[block->first + block->count - 1];
    if (end->op == XR_XIR_JUMP || end->op == XR_XIR_CLEANUP_REGISTER ||
        end->op == XR_XIR_CLEANUP_LEAVE || end->op == XR_XIR_CLEANUP_ERROR) return error_edge(flow, b, end->targets[0], NULL, true);
    if (end->op == XR_XIR_BRANCH) {
        XrXirStatus status = error_edge(flow, b, end->targets[0], end, true);
        return status == XR_XIR_OK ? error_edge(flow, b, end->targets[1], end, false) : status;
    }
    if (end->op == XR_XIR_INVOKE_DEFAULT || end->op == XR_XIR_INVOKE || end->op == XR_XIR_INVOKE_INDIRECT ||
        end->op == XR_XIR_TASK_AWAIT) {
        XrXirStatus status = error_edge(flow, b, end->targets[0], NULL, true);
        if (status != XR_XIR_OK) return status;
        { if (!xir_compile_work(flow->remaining, (size_t)flow->effects->words * sizeof(uint64_t))) { return XR_XIR_BUDGET; } memset(flow->snapshot, 0, (size_t)flow->effects->words * sizeof(uint64_t)); }
        if (end->op == XR_XIR_TASK_AWAIT) {
            if (!xir_compile_work(flow->remaining, (uint64_t)flow->effects->words * sizeof(uint64_t))) return XR_XIR_BUDGET;
            memcpy(flow->snapshot, error_read(flow, flow->work, end->args[0]),
                (size_t)flow->effects->words * sizeof(uint64_t));
        } else status=error_call(flow, end, flow->snapshot);
        if (status!=XR_XIR_OK || flow->restart) return status;
        if (error_any(flow->snapshot, flow->effects->words)) return error_edge(flow, b, end->targets[1], NULL, true);
    }
    return XR_XIR_OK;
}
/* Reusable owned bytes belong to one analysis, never to a cached proof. */
static void error_storage_free(ErrorFlow *flow) {
    xr_compile_resources_free(flow->storage);
    flow->storage = NULL; flow->storage_capacity = 0;
    flow->states = flow->work = flow->edge = flow->snapshot = NULL;
    flow->roots = flow->cells = flow->slots = flow->active = NULL;
    flow->zero = NULL; flow->cell_count = flow->active_count = 0;
    flow->reachable = NULL; flow->escaping = NULL;
}
static XrXirStatus error_storage_prepare(ErrorFlow *flow, size_t bytes) {
    if (!bytes) return XR_XIR_BAD_STRUCTURE;
    if (bytes > flow->storage_capacity) {
        XrXirStatus status = XR_XIR_OK;
        uint8_t *storage = xir_compile_calloc(flow->remaining, bytes, 1, &status);
        if (!storage) return status;
        xr_compile_resources_free(flow->storage);
        flow->storage = storage; flow->storage_capacity = bytes;
    } else {
        if (!xir_compile_work(flow->remaining, bytes)) return XR_XIR_BUDGET;
        memset(flow->storage, 0, bytes);
    }
    return XR_XIR_OK;
}
static XrXirStatus error_function(ErrorFlow *flow, uint32_t f) {
    flow->function = &flow->module->functions[f];
    flow->cells = NULL; flow->cell_count = flow->active_count = 0;
    if (flow->function->parameter_count > UINT32_MAX - flow->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    flow->values = flow->function->parameter_count + flow->function->instruction_count;
    uint64_t words = flow->effects->words, rows = (uint64_t)flow->function->block_count + 3;
    uint64_t minimum_extra = (uint64_t)flow->values * 2 * sizeof(uint32_t) + flow->function->block_count;
    /* At least one writable words row and a separate zero vector are required
     * even when no SSA value can carry error facts. Reject resource-impossible
     * metadata before reading it, without charging a fictitious allocation. */
    if (!words || minimum_extra > SIZE_MAX ||
        words > (SIZE_MAX - minimum_extra) / sizeof(uint64_t) / (rows + 1)) return XR_XIR_BUDGET;
    uint64_t minimum = words * (rows + 1) * sizeof(uint64_t) + minimum_extra;
    if (minimum > flow->storage_capacity) {
        if (!xir_compile_context_valid(flow->remaining)) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus admitted = xir_compile_resource_status(xr_compile_resources_admit(
            flow->remaining->resources, (size_t)minimum, minimum));
        if (admitted != XR_XIR_OK) return admitted;
    }
    /* Every invocation recounts immutable type membership; no summary or
     * permission proof is cached across functions or atom-width restarts. */
    for (uint32_t v = 0; v < flow->values; ++v) {
        if (!xir_compile_work(flow->remaining, 1)) return XR_XIR_BUDGET;
        bool active = true;
        XrXirStatus classified = error_row_active(flow, v, &active);
        if (classified != XR_XIR_OK) return classified;
        if (active) ++flow->active_count;
        if (xr_xir_type_is_cell(flow->module->types, xr_xir_operand_type(flow->function,v)))
            ++flow->cell_count;
    }
    uint64_t stride = (uint64_t)(flow->active_count ? flow->active_count : 1) * words;
    uint64_t extra = ((uint64_t)flow->values * 2 + flow->active_count + flow->cell_count) *
        sizeof(uint32_t) + flow->function->block_count;
    uint64_t zero_bytes = words * sizeof(uint64_t);
    if (extra > SIZE_MAX - zero_bytes ||
        stride > (SIZE_MAX - zero_bytes - extra) / sizeof(uint64_t) / rows) return XR_XIR_BUDGET;
    uint64_t bytes = stride * rows * sizeof(uint64_t) + zero_bytes + extra;
    if (!xir_compile_work(flow->remaining, stride * rows + words + flow->values + flow->function->block_count))
        return XR_XIR_BUDGET;
    flow->stride = (size_t)stride;
    XrXirStatus status = error_storage_prepare(flow, (size_t)bytes);
    if (status == XR_XIR_OK) {
        size_t state_bytes = (size_t)(stride * rows) * sizeof(uint64_t);
        flow->states = (uint64_t *)flow->storage;
        flow->zero = (const uint64_t *)(flow->storage + state_bytes);
        flow->roots = (uint32_t *)(flow->storage + state_bytes + (size_t)zero_bytes);
        flow->slots = flow->roots + flow->values;
        flow->active = flow->slots + flow->values;
        flow->cells = flow->active + flow->active_count;
        flow->reachable = (uint8_t *)(flow->cells + flow->cell_count);
        /* Charge the membership scan and all map, inverse and Cell writes. */
        if (!xir_compile_work(flow->remaining, (uint64_t)flow->values * 2 + flow->active_count + flow->cell_count))
            return XR_XIR_BUDGET;
        uint32_t cell = 0, active = 0;
        for (uint32_t v = 0; v < flow->values; ++v) {
            bool member = true;
            XrXirStatus classified = error_row_active(flow, v, &member);
            if (classified != XR_XIR_OK) return classified;
            if (member) {
                if (active >= flow->active_count) return XR_XIR_BAD_STRUCTURE;
                flow->slots[v] = active; flow->active[active++] = v;
            } else flow->slots[v] = UINT32_MAX;
            if (xr_xir_type_is_cell(flow->module->types, xr_xir_operand_type(flow->function,v))) {
                if (cell >= flow->cell_count) return XR_XIR_BAD_STRUCTURE;
                flow->cells[cell++] = v;
            }
        }
        if (cell != flow->cell_count || active != flow->active_count) return XR_XIR_BAD_STRUCTURE;
        flow->work = flow->states + (size_t)flow->function->block_count * flow->stride;
        flow->edge = flow->work + flow->stride; flow->snapshot = flow->edge + flow->stride;
        uint32_t summary=flow->invocations?error_invocation_summary(flow->invocations,flow->invocation):f;
        if (summary>=(flow->invocations?flow->summary_count:flow->effects->count)) return XR_XIR_BAD_STRUCTURE;
        flow->escaping = flow->effects->errors + (size_t)summary * flow->effects->words;
        status = error_roots(flow); flow->reachable[0] = ERROR_BLOCK_REACHABLE | ERROR_BLOCK_PENDING;
        for (uint32_t p = 0; p < flow->function->parameter_count && status == XR_XIR_OK; ++p) {
            if (!xir_compile_work(flow->remaining, flow->effects->atom_count + 1)) status = XR_XIR_BUDGET;
            else {
                uint64_t *set = error_value(flow, flow->states, p);
                if (set) error_type(flow, flow->function->parameters[p], set);
            }
        }
        do {
            flow->changed = false;
            for (uint32_t b = 0; b < flow->function->block_count && status == XR_XIR_OK && !flow->restart; ++b) {
                if (!xir_compile_work(flow->remaining, 1)) status = XR_XIR_BUDGET;
                else if (flow->reachable[b] & ERROR_BLOCK_PENDING) {
                    /* Clear before transfer so self/back edges can schedule it again. */
                    flow->reachable[b] &= (uint8_t)~ERROR_BLOCK_PENDING;
                    status = error_block(flow, b);
                }
            }
        } while (status == XR_XIR_OK && flow->changed && !flow->restart);
    }
    return status;
}
static uint32_t error_variant_count(const XrXirTypes *types, uint32_t t) {
    uint32_t d = types->nodes[t].nominal.declaration;
    return types->nominals->declarations ? types->nominals->declarations[d].variant_count :
        types->nominals->identities[d].variant_count;
}
static XrXirStatus effect_errors_seed(const XrXirModule *module, XrXirEffects *effects, XrXirCompileContext *remaining) {
    XrXirStatus allocation_status = XR_XIR_OK;
    uint32_t parameters=0;
    for (uint32_t f=0;module->generics && f<module->function_count;++f) {
        if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
        if (module->generics[f].parameter_count>parameters) parameters=module->generics[f].parameter_count;
    }
    uint64_t atoms = parameters;
    for (uint32_t t = 0; module->types && t < module->types->count; ++t) {
        if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
        if (xr_xir_type_is_enum(module->types, (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + t)))
            atoms += error_variant_count(module->types, t);
    }
    if (atoms > UINT32_MAX-2u) return XR_XIR_BUDGET;
    effects->source_type_count = module->types ? module->types->count : 0;
    effects->atom_count = (uint32_t)atoms; effects->atom_capacity = (uint32_t)atoms; effects->words = (uint32_t)((atoms + 65) / 64);
    uint64_t bytes = atoms * sizeof(EffectErrorAtom) + (uint64_t)effects->count * effects->words * sizeof(uint64_t);
    if ((bytes > SIZE_MAX) || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    if (atoms) effects->atoms = xir_compile_calloc(remaining, (size_t)atoms, sizeof(EffectErrorAtom), &allocation_status);
    effects->errors = xir_compile_calloc(remaining, (size_t)effects->count * effects->words, sizeof(uint64_t), &allocation_status);
    if ((atoms && !effects->atoms) || !effects->errors) return allocation_status;

    uint32_t at = 0;
    for (uint32_t t = 0; module->types && t < module->types->count; ++t) {
        XrXirType type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + t);
        if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
        if (!xr_xir_type_is_enum(module->types, type)) continue;
        uint32_t count = error_variant_count(module->types, t);
        if (!xir_compile_work(remaining, count)) return XR_XIR_BUDGET;
        for (uint32_t v = 0; v < count; ++v) effects->atoms[at++] = (EffectErrorAtom){type, v};
    }
    if (!xir_compile_work(remaining,parameters)) return XR_XIR_BUDGET;
    for (uint32_t p=0;p<parameters;++p)
        effects->atoms[at++]=(EffectErrorAtom){(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p),XR_XIR_ERROR_SYMBOLIC_VARIANT};
    return XR_XIR_OK;
}
static XrXirStatus error_summary_resize(ErrorFlow *flow) {
    XrXirEffects *effects=flow->effects;XrXirCompileContext *remaining=flow->remaining;
    XrXirStatus allocation_status = XR_XIR_OK;
    uint32_t words = (uint32_t)(((uint64_t)effects->atom_count + 65) / 64);
    if (words == effects->words) return XR_XIR_OK;
    uint32_t summaries=flow->invocations?flow->summary_count:effects->count;
    uint64_t count = (uint64_t)summaries * words;
    if (count > SIZE_MAX / sizeof(uint64_t) ||(count * sizeof(uint64_t) > SIZE_MAX) ||
        !xir_compile_work(remaining, count)) return XR_XIR_BUDGET;
    uint64_t *errors = xir_compile_calloc(remaining, (size_t)count, sizeof(*errors), &allocation_status);
    if (!errors) return allocation_status;

    for (uint32_t f = 0; f < summaries; ++f)
        { if (!xir_compile_work(remaining, (size_t)effects->words * sizeof(*errors))) { xr_compile_resources_free(errors); return XR_XIR_BUDGET; } memcpy(errors + (size_t)f * words, effects->errors + (size_t)f * effects->words,
            (size_t)effects->words * sizeof(*errors)); }
    xr_compile_resources_free(effects->errors); effects->errors = errors; effects->words = words;
    return XR_XIR_OK;
}
/* A child task's errors affect GO values and await paths, but cannot grant
 * parent execution authority. Keep these dependencies outside authority heads. */
static XrXirStatus error_graph_prepare(const XrXirModule *module, EffectGraph *graph,
    XrXirCompileContext *remaining) {
    uint32_t count = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (!xir_compile_work(remaining, (uint64_t)function->instruction_count + 1)) return XR_XIR_BUDGET;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            if (function->instructions[i].op != XR_XIR_GO) continue;
            if (count == UINT32_MAX) return XR_XIR_BUDGET;
            ++count;
        }
    }
    if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
    graph->error_edge_count = count;
    if (!count) return XR_XIR_OK;
    uint64_t bytes = (uint64_t)module->function_count * sizeof(*graph->error_heads) +
        (uint64_t)count * sizeof(*graph->error_edges);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    graph->error_heads = xir_compile_calloc(remaining, module->function_count, sizeof(*graph->error_heads), &status);
    graph->error_edges = xir_compile_calloc(remaining, count, sizeof(*graph->error_edges), &status);
    if (!graph->error_heads || !graph->error_edges) return status;
    if (!xir_compile_work(remaining, module->function_count)) return XR_XIR_BUDGET;
    for (uint32_t f = 0; f < module->function_count; ++f) graph->error_heads[f] = UINT32_MAX;
    uint32_t at = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (!xir_compile_work(remaining, (uint64_t)function->instruction_count + 1)) return XR_XIR_BUDGET;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_GO) continue;
            uint32_t callee = (uint32_t)op->immediate;
            if (callee >= module->function_count || at >= count) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(remaining, 2)) return XR_XIR_BUDGET;
            graph->error_edges[at] = (EffectEdge){f, graph->error_heads[callee], i, false};
            graph->error_heads[callee] = at++;
        }
    }
    return at == count ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
/* Empty escaping is an exact equation only for these known op semantics.
 * Every visit rebuilds this proof; new operations conservatively interpret CFG. */
static XrXirStatus error_empty_escaping(const XrXirFunction *function,
    const XrXirCompileContext *remaining, bool *empty) {
    *empty = false;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
        XrXirOp op = function->instructions[i].op;
        if (op <= XR_XIR_INVALID || op >= XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        switch (op) {
        case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_CONST_STRING: case XR_XIR_SLOT_LOAD:
        case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_STORE: case XR_XIR_ATOMIC_NEW: case XR_XIR_ATOMIC_LOAD:
        case XR_XIR_ATOMIC_STORE: case XR_XIR_ATOMIC_ADD: case XR_XIR_ATOMIC_SUB: case XR_XIR_ATOMIC_FETCH_ADD:
        case XR_XIR_ATOMIC_FETCH_SUB: case XR_XIR_ATOMIC_SWAP: case XR_XIR_ATOMIC_COMPARE_EXCHANGE: case XR_XIR_ATOMIC_TOGGLE:
        case XR_XIR_ATOMIC_TO_STRING: case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
        case XR_XIR_CONCAT_STRING: case XR_XIR_OUTPUT: case XR_XIR_WRITE_STREAM: case XR_XIR_PRINT:
        case XR_XIR_ADD_INT: case XR_XIR_EQ_INT: case XR_XIR_LT_INT: case XR_XIR_SUSPEND:
        case XR_XIR_JUMP: case XR_XIR_BRANCH: case XR_XIR_RETURN: case XR_XIR_LOCAL_NEW:
        case XR_XIR_LOCAL_READ: case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_READ:
        case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ: case XR_XIR_OWNED_LOCAL_WRITE:
        case XR_XIR_SUB_INT: case XR_XIR_MUL_INT: case XR_XIR_DIV_INT: case XR_XIR_REM_INT:
        case XR_XIR_NE_INT: case XR_XIR_LE_INT: case XR_XIR_GT_INT: case XR_XIR_GE_INT:
        case XR_XIR_AND_INT: case XR_XIR_OR_INT: case XR_XIR_XOR_INT: case XR_XIR_SHL_INT:
        case XR_XIR_SHR_INT: case XR_XIR_PHI: case XR_XIR_FUNCTION_REF: case XR_XIR_CELL_NEW:
        case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE: case XR_XIR_CELL_PROJECT:
        case XR_XIR_CONVERT_NUMBER: case XR_XIR_CONST_FLOAT:
        case XR_XIR_NEG_FLOAT: case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT: case XR_XIR_LT_FLOAT:
        case XR_XIR_LE_FLOAT: case XR_XIR_GT_FLOAT: case XR_XIR_GE_FLOAT: case XR_XIR_CELL_PLACE:
        case XR_XIR_SLOT_PLACE: case XR_XIR_ARRAY_NEW: case XR_XIR_ARRAY_GET: case XR_XIR_ARRAY_SET:
        case XR_XIR_ARRAY_PUSH: case XR_XIR_ARRAY_LEN: case XR_XIR_ADD_FLOAT: case XR_XIR_SUB_FLOAT:
        case XR_XIR_MUL_FLOAT: case XR_XIR_DIV_FLOAT: case XR_XIR_STRUCT_NEW: case XR_XIR_STRUCT_GET:
        case XR_XIR_STRUCT_SET: case XR_XIR_LOCAL_UNINIT: case XR_XIR_STRING_LEN: case XR_XIR_EQ_STRING:
        case XR_XIR_NE_STRING: case XR_XIR_STRING_CONTAINS: case XR_XIR_STRING_STARTS_WITH: case XR_XIR_STRING_ENDS_WITH:
        case XR_XIR_STRING_INDEX_OF: case XR_XIR_STRING_LAST_INDEX_OF: case XR_XIR_ENUM_NEW: case XR_XIR_ENUM_TAG:
        case XR_XIR_ENUM_GET: case XR_XIR_MATCH_FAIL: case XR_XIR_ERROR_ERASE: case XR_XIR_INVOKE_RESULT:
        case XR_XIR_ERROR_IS: case XR_XIR_ERROR_NARROW: case XR_XIR_PANIC_CATCH: case XR_XIR_PANIC_CODE:
        case XR_XIR_PANIC_MESSAGE: case XR_XIR_CLEANUP_REGISTER: case XR_XIR_CLEANUP_LEAVE: case XR_XIR_CLEANUP_ERROR:
        case XR_XIR_CELL_LOCAL_WRITE: case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE: case XR_XIR_PLACE_READ:
        case XR_XIR_PLACE_WRITE: case XR_XIR_FUNCTION_WEAKEN: case XR_XIR_CLASS_NEW: case XR_XIR_CLASS_GET:
        case XR_XIR_CLASS_SET: case XR_XIR_ASSERT_CONDITION: case XR_XIR_INVOKE_DISCARD: case XR_XIR_EQUAL:
        case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME: case XR_XIR_CLOCK_NANOS: case XR_XIR_UTC_OFFSET_AT:
        case XR_XIR_TIMER_AFTER_MS: case XR_XIR_TO_STRING: case XR_XIR_OBJECT_PLACE: case XR_XIR_ARRAY_REPEAT:
        case XR_XIR_NULLABLE_IS_SOME: case XR_XIR_NULLABLE_UNWRAP: case XR_XIR_CONST_RUNE: case XR_XIR_RUNE_TO_INTEGER:
        case XR_XIR_INTEGER_TO_RUNE: case XR_XIR_LT_STRING: case XR_XIR_LE_STRING: case XR_XIR_GT_STRING:
        case XR_XIR_GE_STRING: case XR_XIR_TUPLE_NEW: case XR_XIR_TUPLE_FIELD: case XR_XIR_SLOT_GROUP_INIT:
        case XR_XIR_RANGE_CHECK: case XR_XIR_ARRAY_CAPACITY: case XR_XIR_ARRAY_WITH_CAPACITY: case XR_XIR_ARRAY_RESERVE:
            break;
        default: return XR_XIR_OK;
        }
    }
    *empty = true;
    return XR_XIR_OK;
}
/* Callees with no outstanding dependencies run first. Cycles retain original
 * function order and the existing fair FIFO reaches their least fixed point.
 * Ordering is rebuilt on every atom-width restart, never a cached summary. */
static XrXirStatus error_queue_prepare(EffectGraph *graph, ErrorFlow *flow) {
    if (!graph || !flow || !flow->module || !flow->effects ||
        !xir_compile_context_valid(flow->remaining)) return XR_XIR_BAD_STRUCTURE;
    uint32_t count = flow->effects->count;
    if (!count || count != flow->module->function_count || !graph->heads ||
        !graph->queue || !graph->queued || (graph->edge_count && !graph->edges) ||
        (graph->error_edge_count && (!graph->error_heads || !graph->error_edges)))
        return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes = (uint64_t)count * sizeof(uint32_t);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    uint32_t *degree = NULL;
    bool temporary = !flow->storage || flow->storage_capacity < (size_t)bytes;
    if (temporary) {
        degree = xir_compile_calloc(flow->remaining, count, sizeof(*degree), &status);
        if (!degree) return status;
    } else {
        /* All CFG views are dead here. The next interpretation fully rebinds
         * them after preparing storage; escaping belongs to separate facts. */
        if (!xir_compile_work(flow->remaining, 10)) return XR_XIR_BUDGET;
        flow->states = flow->work = flow->edge = flow->snapshot = NULL;
        flow->reachable = NULL;
        flow->roots = flow->cells = flow->slots = flow->active = NULL;
        flow->zero = NULL;
        if (!xir_compile_work(flow->remaining, bytes)) return XR_XIR_BUDGET;
        degree = (uint32_t *)flow->storage;
        memset(degree, 0, (size_t)bytes);
    }
    const uint32_t *heads[2] = {graph->heads, graph->error_heads};
    const EffectEdge *edges[2] = {graph->edges, graph->error_edges};
    uint32_t counts[2] = {graph->edge_count, graph->error_edge_count};
    if (!xir_compile_work(flow->remaining, count)) { status = XR_XIR_BUDGET; goto done; }
    for (uint32_t f = 0; f < count; ++f) graph->queued[f] = 0;
    for (uint32_t list = 0; list < 2; ++list) {
        if (!heads[list]) continue;
        uint32_t seen = 0;
        for (uint32_t f = 0; f < count; ++f) {
            if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
            uint32_t e = heads[list][f];
            while (e != UINT32_MAX) {
                if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
                if (e >= counts[list] || seen >= counts[list]) { status = XR_XIR_BAD_STRUCTURE; goto done; }
                EffectEdge edge = edges[list][e];
                if (edge.caller >= count || (edge.next != UINT32_MAX && edge.next >= e)) {
                    status = XR_XIR_BAD_STRUCTURE; goto done;
                }
                if (degree[edge.caller] == UINT32_MAX) { status = XR_XIR_BUDGET; goto done; }
                if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
                ++degree[edge.caller]; ++seen; e = edge.next;
            }
        }
        if (seen != counts[list]) { status = XR_XIR_BAD_STRUCTURE; goto done; }
    }
    uint32_t front = 0, back = 0;
    for (uint32_t f = 0; f < count; ++f) {
        if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
        if (degree[f]) continue;
        if (!xir_compile_work(flow->remaining, 2)) { status = XR_XIR_BUDGET; goto done; }
        graph->queue[back++] = f; graph->queued[f] = 1;
    }
    while (front < back) {
        if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
        uint32_t f = graph->queue[front++];
        for (uint32_t list = 0; list < 2; ++list) {
            if (!heads[list]) continue;
            if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
            uint32_t e = heads[list][f];
            while (e != UINT32_MAX) {
                if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
                if (e >= counts[list]) { status = XR_XIR_BAD_STRUCTURE; goto done; }
                EffectEdge edge = edges[list][e];
                if (edge.caller >= count || !degree[edge.caller] ||
                    (edge.next != UINT32_MAX && edge.next >= e)) { status = XR_XIR_BAD_STRUCTURE; goto done; }
                if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
                --degree[edge.caller];
                if (!degree[edge.caller]) {
                    if (graph->queued[edge.caller] || back >= count) { status = XR_XIR_BAD_STRUCTURE; goto done; }
                    if (!xir_compile_work(flow->remaining, 2)) { status = XR_XIR_BUDGET; goto done; }
                    graph->queue[back++] = edge.caller; graph->queued[edge.caller] = 1;
                }
                e = edge.next;
            }
        }
    }
    for (uint32_t f = 0; f < count; ++f) {
        if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
        if (graph->queued[f]) continue;
        if (back >= count) { status = XR_XIR_BAD_STRUCTURE; goto done; }
        if (!xir_compile_work(flow->remaining, 2)) { status = XR_XIR_BUDGET; goto done; }
        graph->queue[back++] = f; graph->queued[f] = 1;
    }
    if (back != count) status = XR_XIR_BAD_STRUCTURE;
done:
    if (temporary) xr_compile_resources_free(degree);
    return status;
}
/* Summaries grow monotonically. Only reverse dependants need fresh local CFG
 * interpretation; new atom identities still restart every function. */
static XrXirStatus error_functions(const XrXirModule *module, XrXirEffects *effects,
    EffectGraph *graph, ErrorFlow *flow) {
    XrXirStatus status = XR_XIR_OK;
    uint32_t front = 0, back = 0, pending = 0;
    do {
        status = error_queue_prepare(graph, flow);
        if (status != XR_XIR_OK) return status;
        front = back = 0; pending = effects->count; flow->restart = false;
        while (pending && status == XR_XIR_OK && !flow->restart) {
            if (!xir_compile_work(flow->remaining, 2)) return XR_XIR_BUDGET;
            uint32_t f = graph->queue[front];
            front = front + 1 == effects->count ? 0 : front + 1;
            --pending; graph->queued[f] = 0; flow->summary_changed = false;
            bool empty = false;
            status = error_empty_escaping(&module->functions[f], flow->remaining, &empty);
            if (status == XR_XIR_OK && empty) {
                /* Preserve an existing nonempty summary rather than washing it. */
                if (!xir_compile_work(flow->remaining, effects->words)) status = XR_XIR_BUDGET;
                else if (error_any(effects->errors + (size_t)f * effects->words, effects->words)) empty = false;
            }
            if (status == XR_XIR_OK && !empty) status = error_function(flow, f);
            if (status != XR_XIR_OK || flow->restart || !flow->summary_changed) continue;
            for (uint32_t list = 0; list < 2 && status == XR_XIR_OK; ++list) {
                const uint32_t *heads = list ? graph->error_heads : graph->heads;
                const EffectEdge *edges = list ? graph->error_edges : graph->edges;
                if (!heads) continue;
                for (uint32_t e = heads[f]; e != UINT32_MAX; e = edges[e].next) {
                    if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; break; }
                    uint32_t caller = edges[e].caller;
                    if (caller >= module->function_count) { status = XR_XIR_BAD_STRUCTURE; break; }
                    if (graph->queued[caller]) continue;
                    if (!xir_compile_work(flow->remaining, 2)) { status = XR_XIR_BUDGET; break; }
                    graph->queue[back] = caller;
                    back = back + 1 == effects->count ? 0 : back + 1;
                    graph->queued[caller] = 1; ++pending;
                }
            }
        }
        if (status == XR_XIR_OK && flow->restart) status = error_summary_resize(flow);
    } while (status == XR_XIR_OK && flow->restart);
    return status;
}
static XrXirStatus effect_errors_analyze(const XrXirModule *module, XrXirEffects *effects,
    EffectGraph *graph, XrXirCompileContext *remaining) {
    EffectTerms terms = {0}; terms.remaining = remaining;
    if (module->types) terms.types = *module->types;
    ErrorFlow flow = {0}; flow.module = module; flow.effects = effects; flow.remaining = remaining; flow.terms = &terms;
    flow.summary_count=effects->count;flow.invocation=UINT32_MAX;
    XrXirStatus status=error_invocation_contexts_new(&flow,&flow.invocations);
    if (status==XR_XIR_OK) status=effect_errors_seed(module,effects,remaining);
    if (status==XR_XIR_OK && flow.invocations) status=error_invocation_summaries(&flow);
    if (status==XR_XIR_OK && flow.invocations) status=error_invocation_functions(&flow);
    else if (status==XR_XIR_OK) {
        status = error_graph_prepare(module, graph, remaining);
        if (status == XR_XIR_OK) status = error_functions(module, effects, graph, &flow);
    }
    /* Temporary substituted identities die with terms. Prove their complete
     * outcomes while the sole error summary and authentic binders are alive. */
    XrXirModule permission = *module; permission.types = &terms.types;
    for (uint32_t f = 0; status == XR_XIR_OK && f < effects->count; ++f) {
        const uint64_t *set = effects->errors + (size_t)f * effects->words;
        XrXirStatus qualified = error_bit(set, 0) || error_bit(set, 1) ? XR_XIR_BAD_TYPE : XR_XIR_OK;
        XrXirProofContext proof = {&permission, {XR_XIR_CONTEXT_FUNCTION, f, 0}};
        for (uint32_t a = 0; qualified == XR_XIR_OK && a < effects->atom_count; ++a) {
            if (!xir_compile_work(remaining, 1)) { status = XR_XIR_BUDGET; break; }
            if (!error_bit(set, a + 2)) continue;
            qualified = xr_xir_compile_type_markers_prove(remaining, &proof, effects->atoms[a].type,
                XR_XIR_CONSTRAINT_ERROR | XR_XIR_CONSTRAINT_SENDABLE);
            if (qualified == XR_XIR_BUDGET || qualified == XR_XIR_OUT_OF_MEMORY) { status = qualified; break; }
        }
        effects->task_errors[f] = qualified;
    }
    error_storage_free(&flow);
    error_invocation_contexts_free(flow.invocations);
    effect_terms_free(&terms);
    if (status != XR_XIR_OK) return status;
    for (uint32_t f = 0; f < effects->count; ++f) {
        const uint64_t *set = effects->errors + (size_t)f * effects->words;
        XrXirEffect fact = error_bit(set, 0) ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_NONE;
        if (error_bit(set,1)) fact=XR_XIR_EFFECT_MAY;
        if (!xir_compile_work(remaining, effects->atom_count + 1)) return XR_XIR_BUDGET;
        for (uint32_t a = 0; a < effects->atom_count; ++a) if (error_bit(set, a + 2)) fact = XR_XIR_EFFECT_MAY;
        effects->functions[f].throws = fact;
    }
    return XR_XIR_OK;
}
