/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_cleanup_verify.inc.c - Authenticate static cleanup edges and captures
 */

static const XrLirCleanupBlock *lir_cleanup_registration(const XrLirModule *m,
    uint32_t owner, uint32_t target) {
    if (!owner || owner > m->function_count || !target || target > m->function_count)
        return NULL;
    const XrLirFunctionData *body = &m->functions[owner - 1];
    const XrLirFunctionData *cleanup = &m->functions[target - 1];
    uint32_t block = cleanup->cleanup_registration;
    if (!body->has_cleanups || cleanup->cleanup_owner != owner ||
        !body->blocks || !block || block > body->block_count) return NULL;
    const XrLirCleanupBlock *registration = body->blocks[block - 1].cleanup;
    return registration && registration->registration == target ? registration : NULL;
}

static bool lir_cleanup_retirement(XrLirOp op) {
    switch (op) {
    case XR_LIR_STRING_DROP: case XR_LIR_STRING_CELL_DROP:
    case XR_LIR_ARRAY_DROP: case XR_LIR_ARRAY_CELL_DROP:
    case XR_LIR_MAP_DROP: case XR_LIR_MAP_CELL_DROP:
    case XR_LIR_SET_DROP: case XR_LIR_SET_CELL_DROP:
    case XR_LIR_VALUE_DROP: case XR_LIR_FUNCTION_SCOPE_LEAVE:
        return true;
    default:
        return false;
    }
}

static bool lir_cleanup_captures(const XrLirFunctionData *f, const LirGraph *g,
    uint32_t block, const XrLirCleanupBlock *registration, const XrLirFunctionData *target) {
    if (registration->capture_count != target->parameter_count ||
        (registration->capture_count && (!registration->captures || !target->parameter_types))) return false;
    uint32_t transfer = f->blocks[block].instruction_count - 1;
    for (uint32_t a = 0; a < registration->capture_count; ++a) {
        uint32_t value = registration->captures[a];
        if (!value || value > f->value_count || !lir_dominates_use(f, g, value, block, transfer))
            return false;
        XrLirType actual = f->values[value - 1].type, expected = target->parameter_types[a];
        if (actual != expected && !(expected == XR_LIR_I64_REF && actual == XR_LIR_I64_CELL))
            return false;
    }
    return true;
}

static bool lir_cleanup_calls(const XrLirModule *m, uint32_t owner,
    const XrLirBlockData *block, uint32_t *frontier) {
    bool clearing = false;
    for (uint32_t i = 0; i + 1 < block->instruction_count; ++i) {
        const XrLirInstructionView *call = &block->instructions[i].view;
        if (call->op != XR_LIR_CALL || !m->functions[call->callee - 1].cleanup_owner) {
            if (clearing && !lir_cleanup_retirement(call->op)) return false;
            continue;
        }
        if (call->callee != *frontier) return false;
        const XrLirCleanupBlock *registration = lir_cleanup_registration(m, owner, *frontier);
        if (!registration || call->operand_count != registration->capture_count) return false;
        for (uint32_t a = 0; a < call->operand_count; ++a)
            if (call->operands[a] != registration->captures[a]) return false;
        *frontier = registration->frontier;
        clearing = true;
    }
    return true;
}

static uint32_t lir_cleanup_frontier(const XrLirFunctionData *f, uint32_t target) {
    const XrLirCleanupBlock *block = f->blocks[target - 1].cleanup;
    return block ? block->frontier : 0;
}

static bool lir_cleanup_paths(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t owner, const LirGraph *g) {
    if (!f->has_cleanups) return true;
    if (lir_cleanup_frontier(f, f->entry)) return false;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        const XrLirCleanupBlock *meta = block->cleanup;
        const XrLirInstructionView *term = lir_terminator(f, b);
        uint32_t from = meta ? meta->frontier : 0, remaining = from;
        if (from && !lir_cleanup_registration(m, owner, from)) return false;
        if (!lir_cleanup_calls(m, owner, block, &remaining)) return false;
        if (meta && meta->registration) {
            uint32_t target = meta->registration;
            if (lir_cleanup_registration(m, owner, target) != meta || target == from ||
                remaining != from || term->op != XR_LIR_BR ||
                !lir_cleanup_captures(f, g, b, meta, &m->functions[target - 1])) return false;
            remaining = target;
        }
        if (from && term->op != XR_LIR_BR && term->op != XR_LIR_CONDBR && !xr_lir_function_exit(term->op))
            return false;
        if (term->op == XR_LIR_CONDBR && remaining != from) return false;
        if (!term->edge_count && remaining) return false;
        for (uint32_t e = 0; e < term->edge_count; ++e)
            if (lir_cleanup_frontier(f, term->edges[e].target) != remaining) return false;
    }
    return true;
}
