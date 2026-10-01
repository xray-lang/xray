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
typedef struct ErrorFlow {
    const XrXirModule *module;
    const XrXirFunction *function;
    XrXirEffects *effects;
    XrXirBudget *remaining;
    EffectTerms *terms;
    uint64_t *states, *work, *edge, *snapshot, *escaping;
    uint8_t *reachable;
    uint32_t *roots;
    size_t stride;
    uint32_t values;
    bool changed, summary_changed, restart;
} ErrorFlow;
static uint64_t *error_value(ErrorFlow *flow, uint64_t *row, uint32_t value) {
    return row + (size_t)value * flow->effects->words;
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
    if (type == XR_XIR_ERROR) error_add(set, 1);
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
            if (!effect_spend(&flow->remaining->work, 1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op = error_definition(flow, root);
            if (!op || !error_copy_op(op->op)) break;
            root = op->args[0];
        }
        flow->roots[v] = root;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_atom(ErrorFlow *flow, EffectErrorAtom atom, uint64_t *set) {
    XrXirEffects *effects = flow->effects;
    for (uint32_t a = 0; a < effects->atom_count; ++a) {
        if (!effect_spend(&flow->remaining->work, 1)) return XR_XIR_BUDGET;
        if (effects->atoms[a].type == atom.type && effects->atoms[a].variant == atom.variant) {
            error_add(set, a + 2); return XR_XIR_OK;
        }
    }
    if (effects->atom_count == UINT32_MAX - 2u) return XR_XIR_BUDGET;
    if (effects->atom_count == effects->atom_capacity) {
        uint64_t capacity = effects->atom_capacity ? (uint64_t)effects->atom_capacity * 2 : 8;
        if (capacity > UINT32_MAX - 2u) capacity = UINT32_MAX - 2u;
        uint64_t bytes = capacity * sizeof(EffectErrorAtom);
        if (bytes > SIZE_MAX || bytes > flow->remaining->metadata_bytes ||
            !effect_spend(&flow->remaining->work, capacity + effects->atom_count)) return XR_XIR_BUDGET;
        EffectErrorAtom *atoms = xr_calloc((size_t)capacity, sizeof(*atoms));
        if (!atoms) return XR_XIR_OUT_OF_MEMORY;
        flow->remaining->metadata_bytes -= bytes;
        if (effects->atom_count) memcpy(atoms, effects->atoms, (size_t)effects->atom_count * sizeof(*atoms));
        xr_free(effects->atoms); effects->atoms = atoms; effects->atom_capacity = (uint32_t)capacity;
    }
    effects->atoms[effects->atom_count++] = atom;
    flow->restart = true;
    return XR_XIR_OK;
}
static XrXirStatus error_call(ErrorFlow *flow, const XrXirInstruction *call, uint64_t *set) {
    if (call->op == XR_XIR_CALL_INDIRECT || call->op == XR_XIR_INVOKE_INDIRECT || call->op == XR_XIR_CALL_REQUIREMENT) {
        error_add(set, 0); return XR_XIR_OK;
    }
    XrXirInstruction resolved;
    if(call->op==XR_XIR_CALL_DEFAULT || call->op==XR_XIR_INVOKE_DEFAULT) {
        const XrXirDefaultBinding *binding=NULL;
        const uint32_t *identity=xr_xir_default_identity(call);
        XrXirStatus status=xr_xir_default_lookup(flow->module,identity[0],identity[1],flow->remaining,&binding);
        if(status!=XR_XIR_OK) return status;
        if(!binding) return XR_XIR_BAD_STRUCTURE;
        resolved=*call;resolved.immediate=binding->function;call=&resolved;
    }
    uint32_t caller = (uint32_t)(flow->function - flow->module->functions);
    const uint64_t *from = flow->effects->errors + (size_t)call->immediate * flow->effects->words;
    if (error_bit(from, 0)) error_add(set, 0);
    if (error_bit(from, 1)) error_add(set, 1);
    for (uint32_t a = 0; a < flow->effects->atom_count; ++a) if (error_bit(from, a + 2)) {
        EffectErrorAtom atom = flow->effects->atoms[a];
        if (!effect_spend(&flow->remaining->work, (uint64_t)flow->effects->atom_count + 1)) return XR_XIR_BUDGET;
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

/* Calls, scheduler/host boundaries and writes through other references can
 * change any aliased cell.
 * Immutable values already read from a cell keep their independent facts. */
static XrXirStatus error_cells(ErrorFlow *flow, uint32_t target, const uint64_t *value) {
    for (uint32_t v = 0; v < flow->values; ++v) {
        if (!effect_spend(&flow->remaining->work, flow->effects->words + (uint64_t)flow->effects->atom_count + 1))
            return XR_XIR_BUDGET;
        XrXirType type = xr_xir_operand_type(flow->function,v);
        if (!xr_xir_type_is_cell(flow->module->types,type)) continue;
        uint64_t *set = error_value(flow,flow->work,v);
        memset(set,0,(size_t)flow->effects->words*sizeof(uint64_t));
        if (target != UINT32_MAX && flow->roots[v] == flow->roots[target])
            memcpy(set,value,(size_t)flow->effects->words*sizeof(uint64_t));
        else error_type(flow,type,set);
    }
    return XR_XIR_OK;
}
static XrXirStatus error_instruction(ErrorFlow *flow, uint32_t i) {
    const XrXirInstruction *op = &flow->function->instructions[i];
    uint32_t words = flow->effects->words;
    if (!effect_spend(&flow->remaining->work, (uint64_t)words + flow->effects->atom_count + 1)) return XR_XIR_BUDGET;
    uint64_t *out = error_value(flow, flow->work, flow->function->parameter_count + i);
    if (op->op == XR_XIR_PHI) return XR_XIR_OK;
    memset(out, 0, (size_t)words * sizeof(*out));
    if (op->op == XR_XIR_CELL_WRITE) {
        memcpy(flow->snapshot,error_value(flow,flow->work,op->args[1]),(size_t)words*sizeof(uint64_t));
        XrXirStatus status = error_cells(flow,op->args[0],flow->snapshot);
        if (status != XR_XIR_OK) return status;
    }
    if (error_copy_op(op->op) || op->op == XR_XIR_CELL_NEW ||
        (op->op == XR_XIR_CELL_READ && !xr_xir_type_is_cell(flow->module->types,op->type)) || op->op == XR_XIR_LOCAL_NEW || op->op == XR_XIR_OWNED_LOCAL_NEW ||
        op->op == XR_XIR_SCALAR_LOCAL_NEW || op->op == XR_XIR_LOCAL_READ ||
        op->op == XR_XIR_OWNED_LOCAL_READ || op->op == XR_XIR_SCALAR_LOCAL_READ)
        memcpy(out, error_value(flow, flow->work, op->args[0]), (size_t)words * sizeof(*out));
    else if (op->op == XR_XIR_LOCAL_WRITE || op->op == XR_XIR_OWNED_LOCAL_WRITE || op->op == XR_XIR_SCALAR_LOCAL_WRITE)
        memcpy(error_value(flow, flow->work, op->args[0]), error_value(flow, flow->work, op->args[1]),
            (size_t)words * sizeof(*out));
    else if (op->op == XR_XIR_ENUM_NEW) {
        for (uint32_t a = 0; a < flow->effects->atom_count; ++a)
            if (flow->effects->atoms[a].type == op->type && flow->effects->atoms[a].variant == (uint32_t)op->immediate)
                error_add(out, a + 2);
    } else if (op->op == XR_XIR_INVOKE_ERROR) {
        XrXirStatus status=error_call(flow, &flow->function->instructions[op->immediate], out);
        if (status!=XR_XIR_OK || flow->restart) return status;
    }
    else error_type(flow, op->type, out);
    if (op->op == XR_XIR_THROW)
        flow->summary_changed |= error_join(flow->escaping, error_value(flow, flow->work, op->args[0]), words);
    else if (op->op == XR_XIR_CALL_DEFAULT || op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_CALL_REQUIREMENT) {
        uint64_t *temporary = flow->snapshot;
        memset(temporary, 0, (size_t)words * sizeof(*temporary));
        XrXirStatus status=error_call(flow, op, temporary); if (status!=XR_XIR_OK || flow->restart) return status;
        flow->summary_changed |= error_join(flow->escaping, temporary, words);
    }
    if (op->op == XR_XIR_CALL_DEFAULT || op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_CALL_REQUIREMENT ||
        op->op == XR_XIR_INVOKE_DEFAULT || op->op == XR_XIR_INVOKE || op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_SUSPEND ||
        op->op == XR_XIR_OUTPUT || op->op == XR_XIR_WRITE_STREAM || op->op == XR_XIR_PRINT ||
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
    for (uint32_t v = 0; v < flow->values; ++v) if (flow->roots[v] == root) {
        uint64_t *set = error_value(flow, flow->edge, v);
        for (uint32_t a = 0; a < flow->effects->atom_count; ++a) {
            if (flow->effects->atoms[a].variant==XR_XIR_ERROR_SYMBOLIC_VARIANT) continue;
            bool match = flow->effects->atoms[a].type == type &&
                (variant < 0 || flow->effects->atoms[a].variant == (uint64_t)variant);
            if (match != yes) set[(a + 2) / 64] &= ~((uint64_t)1 << ((a + 2) % 64));
        }
    }
    return error_any(error_value(flow, flow->edge, value), flow->effects->words);
}
static XrXirStatus error_edge(ErrorFlow *flow, uint32_t from, uint32_t to,
    const XrXirInstruction *branch, bool yes) {
    if (!effect_spend(&flow->remaining->work, (uint64_t)flow->stride * 3) ||
        !effect_spend(&flow->remaining->work, (uint64_t)flow->values * (flow->effects->atom_count + 1)))
        return XR_XIR_BUDGET;
    memcpy(flow->edge, flow->work, flow->stride * sizeof(uint64_t));
    if (flow->function->blocks[from].panic == to &&
        flow->function->blocks[from].frontier != flow->function->blocks[to].frontier) {
        uint64_t *saved = flow->work;
        flow->work = flow->edge;
        XrXirStatus status = error_cells(flow, UINT32_MAX, NULL);
        flow->work = saved;
        if (status != XR_XIR_OK) return status;
    }
    if (branch && !error_filter(flow, branch, yes)) return XR_XIR_OK;
    memcpy(flow->snapshot, flow->edge, flow->stride * sizeof(uint64_t));
    const XrXirBlock *block = &flow->function->blocks[to];
    for (uint32_t i = block->first; i < block->first + block->count; ++i) {
        const XrXirInstruction *phi = &flow->function->instructions[i];
        if (phi->op != XR_XIR_PHI) break;
        for (uint32_t p = 0; p < phi->args[1]; p += 2) {
            if (!effect_spend(&flow->remaining->work, 1)) return XR_XIR_BUDGET;
            if (flow->function->operands[phi->args[0] + p] != from) continue;
            uint32_t value = flow->function->operands[phi->args[0] + p + 1];
            memcpy(error_value(flow, flow->edge, flow->function->parameter_count + i),
                error_value(flow, flow->snapshot, value), (size_t)flow->effects->words * sizeof(uint64_t));
        }
    }
    if (!flow->reachable[to]) { flow->reachable[to] = 1; flow->changed = true; }
    flow->changed |= error_join(flow->states + (size_t)to * flow->stride, flow->edge, flow->stride);
    return XR_XIR_OK;
}
static XrXirStatus error_block(ErrorFlow *flow, uint32_t b) {
    const XrXirBlock *block = &flow->function->blocks[b];
    if (!effect_spend(&flow->remaining->work, flow->stride)) return XR_XIR_BUDGET;
    memcpy(flow->work, flow->states + (size_t)b * flow->stride, flow->stride * sizeof(uint64_t));
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
    if (end->op == XR_XIR_INVOKE_DEFAULT || end->op == XR_XIR_INVOKE || end->op == XR_XIR_INVOKE_INDIRECT) {
        XrXirStatus status = error_edge(flow, b, end->targets[0], NULL, true);
        if (status != XR_XIR_OK) return status;
        memset(flow->snapshot, 0, (size_t)flow->effects->words * sizeof(uint64_t));
        status=error_call(flow, end, flow->snapshot);
        if (status!=XR_XIR_OK || flow->restart) return status;
        if (error_any(flow->snapshot, flow->effects->words)) return error_edge(flow, b, end->targets[1], NULL, true);
    }
    return XR_XIR_OK;
}
static XrXirStatus error_function(ErrorFlow *flow, uint32_t f) {
    flow->function = &flow->module->functions[f];
    flow->values = flow->function->parameter_count + flow->function->instruction_count;
    uint64_t stride = (uint64_t)flow->values * flow->effects->words;
    uint64_t rows = (uint64_t)flow->function->block_count + 3;
    uint64_t extra = (uint64_t)flow->values * sizeof(uint32_t) + flow->function->block_count;
    if (extra > SIZE_MAX || stride > (SIZE_MAX - extra) / sizeof(uint64_t) / rows) return XR_XIR_BUDGET;
    uint64_t bytes = stride * rows * sizeof(uint64_t) + extra;
    if (bytes > flow->remaining->scratch_bytes || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    if (!effect_spend(&flow->remaining->work, stride * rows + flow->values + flow->function->block_count))
        return XR_XIR_BUDGET;
    flow->remaining->scratch_bytes -= bytes;
    flow->stride = (size_t)stride;
    flow->states = xr_calloc((size_t)(stride * rows), sizeof(uint64_t));
    flow->roots = xr_calloc(flow->values, sizeof(uint32_t));
    flow->reachable = xr_calloc(flow->function->block_count, 1);
    XrXirStatus status = XR_XIR_OUT_OF_MEMORY;
    if (flow->states && flow->roots && flow->reachable) {
        flow->work = flow->states + (size_t)flow->function->block_count * flow->stride;
        flow->edge = flow->work + flow->stride; flow->snapshot = flow->edge + flow->stride;
        flow->escaping = flow->effects->errors + (size_t)f * flow->effects->words;
        status = error_roots(flow); flow->reachable[0] = 1;
        for (uint32_t p = 0; p < flow->function->parameter_count && status == XR_XIR_OK; ++p) {
            if (!effect_spend(&flow->remaining->work, flow->effects->atom_count + 1)) status = XR_XIR_BUDGET;
            else error_type(flow, flow->function->parameters[p], error_value(flow, flow->states, p));
        }
        do {
            flow->changed = false;
            for (uint32_t b = 0; b < flow->function->block_count && status == XR_XIR_OK && !flow->restart; ++b) {
                if (!effect_spend(&flow->remaining->work, 1)) status = XR_XIR_BUDGET;
                else if (flow->reachable[b]) status = error_block(flow, b);
            }
        } while (status == XR_XIR_OK && flow->changed && !flow->restart);
    }
    xr_free(flow->states); xr_free(flow->roots); xr_free(flow->reachable);
    flow->remaining->scratch_bytes += bytes;
    return status;
}
static uint32_t error_variant_count(const XrXirTypes *types, uint32_t t) {
    uint32_t d = types->nodes[t].nominal.declaration;
    return types->nominals->declarations ? types->nominals->declarations[d].variant_count :
        types->nominals->identities[d].variant_count;
}
static XrXirStatus effect_errors_seed(const XrXirModule *module, XrXirEffects *effects, XrXirBudget *remaining) {
    uint32_t parameters=0;
    for (uint32_t f=0;module->generics && f<module->function_count;++f) {
        if (!effect_spend(&remaining->work,1)) return XR_XIR_BUDGET;
        if (module->generics[f].parameter_count>parameters) parameters=module->generics[f].parameter_count;
    }
    uint64_t atoms = parameters;
    for (uint32_t t = 0; module->types && t < module->types->count; ++t) {
        if (!effect_spend(&remaining->work, 1)) return XR_XIR_BUDGET;
        if (xr_xir_type_is_enum(module->types, (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + t)))
            atoms += error_variant_count(module->types, t);
    }
    if (atoms > UINT32_MAX-2u) return XR_XIR_BUDGET;
    effects->source_type_count = module->types ? module->types->count : 0;
    effects->atom_count = (uint32_t)atoms; effects->atom_capacity = (uint32_t)atoms; effects->words = (uint32_t)((atoms + 65) / 64);
    uint64_t bytes = atoms * sizeof(EffectErrorAtom) + (uint64_t)effects->count * effects->words * sizeof(uint64_t);
    if (bytes > remaining->metadata_bytes || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    if (atoms) effects->atoms = xr_calloc((size_t)atoms, sizeof(EffectErrorAtom));
    effects->errors = xr_calloc((size_t)effects->count * effects->words, sizeof(uint64_t));
    if ((atoms && !effects->atoms) || !effects->errors) return XR_XIR_OUT_OF_MEMORY;
    remaining->metadata_bytes-=bytes;
    uint32_t at = 0;
    for (uint32_t t = 0; module->types && t < module->types->count; ++t) {
        XrXirType type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + t);
        if (!effect_spend(&remaining->work, 1)) return XR_XIR_BUDGET;
        if (!xr_xir_type_is_enum(module->types, type)) continue;
        uint32_t count = error_variant_count(module->types, t);
        if (!effect_spend(&remaining->work, count)) return XR_XIR_BUDGET;
        for (uint32_t v = 0; v < count; ++v) effects->atoms[at++] = (EffectErrorAtom){type, v};
    }
    if (!effect_spend(&remaining->work,parameters)) return XR_XIR_BUDGET;
    for (uint32_t p=0;p<parameters;++p)
        effects->atoms[at++]=(EffectErrorAtom){(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p),XR_XIR_ERROR_SYMBOLIC_VARIANT};
    return XR_XIR_OK;
}
static XrXirStatus error_summary_resize(XrXirEffects *effects, XrXirBudget *remaining) {
    uint32_t words = (uint32_t)(((uint64_t)effects->atom_count + 65) / 64);
    if (words == effects->words) return XR_XIR_OK;
    uint64_t count = (uint64_t)effects->count * words;
    if (count > SIZE_MAX / sizeof(uint64_t) || count * sizeof(uint64_t) > remaining->metadata_bytes ||
        !effect_spend(&remaining->work, count)) return XR_XIR_BUDGET;
    uint64_t *errors = xr_calloc((size_t)count, sizeof(*errors));
    if (!errors) return XR_XIR_OUT_OF_MEMORY;
    remaining->metadata_bytes -= count * sizeof(*errors);
    for (uint32_t f = 0; f < effects->count; ++f)
        memcpy(errors + (size_t)f * words, effects->errors + (size_t)f * effects->words,
            (size_t)effects->words * sizeof(*errors));
    xr_free(effects->errors); effects->errors = errors; effects->words = words;
    return XR_XIR_OK;
}
static XrXirStatus effect_errors_analyze(const XrXirModule *module, XrXirEffects *effects, XrXirBudget *remaining) {
    XrXirStatus status = effect_errors_seed(module, effects, remaining);
    if (status != XR_XIR_OK) return status;
    EffectTerms terms = {0}; terms.remaining = remaining;
    if (module->types) terms.types = *module->types;
    ErrorFlow flow = {0}; flow.module = module; flow.effects = effects; flow.remaining = remaining; flow.terms = &terms;
    do {
        flow.summary_changed = false; flow.restart = false;
        for (uint32_t f = 0; f < module->function_count && status == XR_XIR_OK && !flow.restart; ++f)
            status = error_function(&flow, f);
        if (status == XR_XIR_OK && flow.restart) status = error_summary_resize(effects, remaining);
    } while (status == XR_XIR_OK && (flow.summary_changed || flow.restart));
    effect_terms_free(&terms);
    if (status != XR_XIR_OK) return status;
    for (uint32_t f = 0; f < effects->count; ++f) {
        const uint64_t *set = effects->errors + (size_t)f * effects->words;
        XrXirEffect fact = error_bit(set, 0) ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_NONE;
        if (error_bit(set,1)) fact=XR_XIR_EFFECT_MAY;
        if (!effect_spend(&remaining->work, effects->atom_count + 1)) return XR_XIR_BUDGET;
        for (uint32_t a = 0; a < effects->atom_count; ++a) if (error_bit(set, a + 2)) fact = XR_XIR_EFFECT_MAY;
        effects->functions[f].throws = fact;
    }
    return XR_XIR_OK;
}
