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

static bool lir_cleanup_scalar(XrLirType type) {
    return type == XR_LIR_I64 || type == XR_LIR_I1;
}

static const XrLirInstructionView *lir_cleanup_directory_close(const XrLirFunctionData *f) {
    if (!f->cleanup_owner || f->has_cleanups || f->block_count != 1 || !f->blocks ||
        f->parameter_count != 1 || !f->parameter_types || !f->parameters ||
        f->result != XR_LIR_UNIT || f->error)
        return NULL;
    const XrLirBlockData *block = &f->blocks[0];
    bool read = f->parameter_types[0] == XR_LIR_I64_REF;
    if ((!read && f->parameter_types[0] != XR_LIR_I64) ||
        block->instruction_count != 2u + read || !block->instructions)
        return NULL;
    const XrLirInstructionView *load = &block->instructions[0].view;
    const XrLirInstructionView *close = &block->instructions[read].view;
    if ((read && (load->op != XR_LIR_CELL_READ || load->operand_count != 1 ||
                    !load->operands || load->operands[0] != f->parameters[0])) ||
        close->op != XR_LIR_HOST_DIRECTORY_CLEANUP_CLOSE ||
        close->operand_count != 1 || !close->operands ||
        close->operands[0] != (read ? load->result : f->parameters[0]) ||
        block->instructions[read + 1].view.op != XR_LIR_RET)
        return NULL;
    return close;
}

static bool lir_cleanup_nopanic(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *op) {
    switch (op->op) {
    case XR_LIR_CONST:
        return lir_cleanup_scalar(op->type) || op->type == XR_LIR_U64;
    case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL:
        return lir_cleanup_scalar(op->type);
    case XR_LIR_LITERAL_ADDR:
        /* The ordinary verifier restricts this address to one direct print. */
        return true;
    case XR_LIR_ICMP:
        return lir_cleanup_scalar(f->values[op->operands[0] - 1].type);
    case XR_LIR_CALL_INTRINSIC: {
        const XrLirArrayIntrinsic *shape = xr_lir_internal_array_intrinsic(m, op->intrinsic);
        return shape && ((shape->operation == XR_LIR_ARRAY_OP_NEW && op->operand_count == 0) ||
            (shape->operation == XR_LIR_ARRAY_OP_LEN && op->operand_count == 1));
    }
    case XR_LIR_ARRAY_RETAIN: case XR_LIR_ARRAY_DROP: case XR_LIR_ARRAY_CELL_DROP:
        /* Snapshots and local bindings retain and retire within the active scope. */
        return true;
    case XR_LIR_SLICE_CLEANUP: case XR_LIR_SLICE_FORMAL_CLEANUP:
        /* Retiring a temporary view loan only releases the recorded loan. */
        return true;
    case XR_LIR_STRING_RETAIN: case XR_LIR_STRING_DROP: case XR_LIR_STRING_CELL_DROP:
        /* Retaining or releasing a temporary string runs no guest code. */
        return true;
    case XR_LIR_CELL_NEW:
        return op->type == XR_LIR_I64_CELL || op->type == XR_LIR_ARRAY_CELL;
    case XR_LIR_CELL_READ: case XR_LIR_CELL_WRITE: {
        XrLirType type = f->values[op->operands[0] - 1].type;
        return type == XR_LIR_I64_CELL || type == XR_LIR_I64_REF ||
            (op->op == XR_LIR_CELL_READ && type == XR_LIR_ARRAY_CELL);
    }
    case XR_LIR_SUM_NEW: {
        const XrLirRecordField *fields;
        const XrLirSumVariant *variants;
        uint32_t field_count, variant_count;
        if (xr_lir_boxed_type(m, op->type) ||
            !xr_lir_sum_variants(m, op->type, &fields, &field_count, &variants, &variant_count)) return false;
        for (uint32_t i = 0; i < field_count; ++i)
            if (lir_pod_scalar(fields[i].type) >= XR_POD_SCALAR_COUNT) return false;
        return true;
    }
    case XR_LIR_CALL: {
        const XrLirFunctionData *callee = &m->functions[op->callee - 1];
        return callee->kind == XR_LIR_FUNCTION_EXTERNAL &&
            (callee->extern_kind == XR_LIR_EXTERN_PRINT_I64 ||
             (callee->extern_kind == XR_LIR_EXTERN_PRINT_STRING && lir_literal_call(m, f, op)));
    }
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

static bool lir_cleanup_scalar_frame(const XrLirFunctionData *f);

static bool lir_cleanup_calls(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t owner, const XrLirBlockData *block, uint32_t *frontier, bool guarded_arithmetic,
    bool *scalar_frame) {
    enum { BODY, CLEANUPS, RETIREMENT } phase = BODY;
    bool guarded = *frontier || f->cleanup_owner;
    bool directory_closed = false;
    bool ordinary_cleanup = false;
    for (uint32_t i = 0; i + 1 < block->instruction_count; ++i) {
        const XrLirInstructionView *call = &block->instructions[i].view;
        if (!i && guarded_arithmetic) continue;
        if (call->op == XR_LIR_HOST_DIRECTORY_CLEANUP_CLOSE) {
            if (call != lir_cleanup_directory_close(f)) return false;
            continue;
        }
        if (call->op == XR_LIR_HOST_DIRECTORY_CLEANUP_POLL) {
            XrLirOp exit = block->instructions[block->instruction_count - 1].view.op;
            if (i + 2 != block->instruction_count || !directory_closed || ordinary_cleanup ||
                *frontier || f->cleanup_owner || (exit != XR_LIR_RET && exit != XR_LIR_BR)) return false;
            if (!*scalar_frame) *scalar_frame = lir_cleanup_scalar_frame(f);
            return *scalar_frame;
        }
        if (call->op != XR_LIR_CALL || !m->functions[call->callee - 1].cleanup_owner) {
            if (phase != BODY) {
                if (!lir_cleanup_retirement(call->op)) return false;
                phase = RETIREMENT;
            } else if (guarded && !lir_cleanup_nopanic(m, f, call)) return false;
            continue;
        }
        if (phase == RETIREMENT || call->callee != *frontier) return false;
        const XrLirCleanupBlock *registration = lir_cleanup_registration(m, owner, *frontier);
        if (!registration || call->operand_count != registration->capture_count) return false;
        for (uint32_t a = 0; a < call->operand_count; ++a)
            if (call->operands[a] != registration->captures[a]) return false;
        if (lir_cleanup_directory_close(&m->functions[call->callee - 1])) directory_closed = true;
        else ordinary_cleanup = true;
        *frontier = registration->frontier;
        phase = CLEANUPS;
    }
    return !directory_closed;
}

static uint32_t lir_cleanup_frontier(const XrLirFunctionData *f, uint32_t target) {
    const XrLirCleanupBlock *block = f->blocks[target - 1].cleanup;
    return block ? block->frontier : 0;
}

static bool lir_cleanup_catch_shape(const XrLirFunctionData *f, const XrLirBlockData *failure,
    const XrLirInstructionView *exit) {
    if (!lir_catch_receives_error(failure, exit)) return false;
    const XrLirEdgeView *edge = &exit->edges[0];
    if (lir_cleanup_frontier(f, edge->target)) return false;
    for (uint32_t a = 1; a < edge->argument_count; ++a) {
        uint32_t value = edge->arguments[a];
        if (value == failure->parameters[0] || lir_pod_scalar(f->values[value - 1].type) >= XR_POD_SCALAR_COUNT)
            return false;
    }
    return true;
}

static bool lir_cleanup_invoke_shape(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *call, uint32_t frontier) {
    uint32_t failed = call->edges[1].target - 1;
    const XrLirBlockData *failure = &f->blocks[failed];
    const XrLirInstructionView *first = &failure->instructions[0].view;
    const XrLirInstructionView *exit = lir_terminator(f, failed);
    bool typed_exit = exit->op == XR_LIR_PANIC_ERROR ||
        (exit->op == XR_LIR_RETURN_ERROR && f->error && f->error == m->functions[call->callee - 1].error) ||
        lir_cleanup_catch_shape(f, failure, exit);
    return typed_exit && (call->type == XR_LIR_UNIT || lir_cleanup_scalar(call->type)) &&
        first->op == XR_LIR_CALL && first->callee == frontier;
}

static bool lir_cleanup_same_source(const XrLirSource *a, const XrLirSource *b) {
    return a->path && b->path && a->path_length == b->path_length &&
        a->line == b->line && a->column == b->column &&
        a->end_line == b->end_line && a->end_column == b->end_column &&
        !memcmp(a->path, b->path, a->path_length);
}

static uint32_t lir_cleanup_arithmetic_code(XrLirOp op) {
    return op == XR_LIR_DIV ? 420u : op == XR_LIR_REM ? 421u : 0;
}

static bool lir_cleanup_one_predecessor(const LirGraph *g, uint32_t block) {
    return g->offsets[block + 1] - g->offsets[block] == 1;
}

static bool lir_cleanup_zero_test(const XrLirFunctionData *f, uint32_t check,
    const XrLirInstructionView *branch, const XrLirInstructionView *arithmetic) {
    const XrLirInstructionView *compare = xr_lir_internal_value_definition(f, branch->operands[0]);
    if (!compare || compare->op != XR_LIR_ICMP || compare->predicate != XR_LIR_NE ||
        f->values[compare->result - 1].block != check + 1 ||
        compare->operands[0] != arithmetic->operands[1]) return false;
    const XrLirInstructionView *zero = xr_lir_internal_value_definition(f, compare->operands[1]);
    return zero && zero->op == XR_LIR_CONST && zero->type == XR_LIR_I64 && !zero->bits;
}

/* Authenticate both edges before allowing the first instruction of the
 * nonzero successor to use normal arithmetic under an active frontier. */
static const XrLirInstructionView *lir_cleanup_arithmetic_pair(const XrLirFunctionData *f,
    const LirGraph *g, uint32_t nonzero, uint32_t frontier) {
    if (!frontier || !lir_cleanup_one_predecessor(g, nonzero)) return NULL;
    uint32_t check = g->predecessors[g->offsets[nonzero]];
    const XrLirInstructionView *branch = lir_terminator(f, check);
    if (branch->op != XR_LIR_CONDBR || branch->edges[0].target != nonzero + 1 ||
        branch->edges[0].argument_count || branch->edges[1].argument_count ||
        lir_cleanup_frontier(f, check + 1) != frontier) return NULL;
    uint32_t zero = branch->edges[1].target - 1;
    if (!lir_cleanup_one_predecessor(g, zero) || lir_cleanup_frontier(f, zero + 1) != frontier)
        return NULL;
    const XrLirInstructionView *arithmetic = &f->blocks[nonzero].instructions[0].view;
    const XrLirInstructionView *panic = lir_terminator(f, zero);
    uint32_t code = lir_cleanup_arithmetic_code(arithmetic->op);
    if (!code || arithmetic->type != XR_LIR_I64 || panic->op != XR_LIR_ARITHMETIC_PANIC ||
        panic->bits != code || !lir_cleanup_same_source(&arithmetic->source, &panic->source) ||
        !lir_cleanup_same_source(&branch->source, &panic->source) ||
        !lir_cleanup_zero_test(f, check, branch, arithmetic)) return NULL;
    return panic;
}

static bool lir_cleanup_arithmetic_landing(const XrLirModule *m, const XrLirFunctionData *f,
    const LirGraph *g, uint32_t block, uint32_t frontier) {
    if (!lir_cleanup_one_predecessor(g, block)) return false;
    const XrLirInstructionView *branch = lir_terminator(f, g->predecessors[g->offsets[block]]);
    if (branch->op != XR_LIR_CONDBR || branch->edges[1].target != block + 1 ||
        lir_cleanup_arithmetic_pair(f, g, branch->edges[0].target - 1, frontier) != lir_terminator(f, block))
        return false;
    // Cleanup calls and the terminal are the only effects on the zero edge.
    const XrLirBlockData *landing = &f->blocks[block];
    for (uint32_t i = 0; i + 1 < landing->instruction_count; ++i) {
        const XrLirInstructionView *call = &landing->instructions[i].view;
        if (call->op != XR_LIR_CALL || !m->functions[call->callee - 1].cleanup_owner) return false;
    }
    return true;
}

static bool lir_cleanup_scalar_frame(const XrLirFunctionData *f) {
    if (f->error || f->closure.callable || (f->result != XR_LIR_UNIT && !lir_cleanup_scalar(f->result))) return false;
    for (uint32_t v = 0; v < f->value_count; ++v) {
        XrLirType type = f->values[v].type;
        if (lir_cleanup_scalar(type)) continue;
        if (type != XR_LIR_I64_CELL) return false;
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, v + 1);
        if (!definition || definition->op != XR_LIR_CELL_NEW) return false;
    }
    return true;
}

static bool lir_cleanup_paths(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t owner, const LirGraph *g) {
    if (!f->has_cleanups && !f->cleanup_owner) return true;
    if (lir_cleanup_frontier(f, f->entry)) return false;
    bool scalar_frame = false;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        const XrLirCleanupBlock *meta = block->cleanup;
        const XrLirInstructionView *term = lir_terminator(f, b);
        uint32_t from = meta ? meta->frontier : 0, remaining = from;
        if (from && !lir_cleanup_registration(m, owner, from)) return false;
        /* A callback is already unwinding its owner's frontier and must
         * complete normally even when it registers its own cleanup. */
        if (f->cleanup_owner && (term->op == XR_LIR_INVOKE || term->op == XR_LIR_PANIC_ERROR))
            return false;
        bool guarded_arithmetic = from && lir_cleanup_arithmetic_code(block->instructions[0].view.op);
        if (guarded_arithmetic && !lir_cleanup_arithmetic_pair(f, g, b, from)) return false;
        if (term->op == XR_LIR_ARITHMETIC_PANIC) {
            if (!scalar_frame) scalar_frame = lir_cleanup_scalar_frame(f);
            if (!scalar_frame || !lir_cleanup_arithmetic_landing(m, f, g, b, from)) return false;
        }
        if (!lir_cleanup_calls(m, f, owner, block, &remaining, guarded_arithmetic, &scalar_frame)) return false;
        if (meta && meta->registration) {
            uint32_t target = meta->registration;
            if (lir_cleanup_registration(m, owner, target) != meta || target == from ||
                remaining != from || term->op != XR_LIR_BR ||
                !lir_cleanup_captures(f, g, b, meta, &m->functions[target - 1])) return false;
            remaining = target;
        }
        if (from || f->cleanup_owner) {
            if (term->op == XR_LIR_INVOKE) {
                if (!from || !lir_cleanup_invoke_shape(m, f, term, from)) return false;
            } else if (term->op != XR_LIR_BR && term->op != XR_LIR_CONDBR &&
                !xr_lir_function_exit(term->op) && term->op != XR_LIR_PANIC_ERROR &&
                term->op != XR_LIR_ARITHMETIC_PANIC) {
                return false;
            }
        }
        if ((term->op == XR_LIR_CONDBR || term->op == XR_LIR_INVOKE) && remaining != from) return false;
        /* The invoke verifier already ties PANIC_ERROR to the original
         * failure value. Every terminal must empty the active frontier. */
        if (!term->edge_count && remaining) return false;
        for (uint32_t e = 0; e < term->edge_count; ++e)
            if (lir_cleanup_frontier(f, term->edges[e].target) != remaining) return false;
    }
    return true;
}

/* Called after every function's instructions, registrations and whole CFG
 * have been verified. A direct leaf can call its own proved cleanups and the
 * scalar and direct literal output sinks. The entire body must exclude other
 * operations that can panic, including before its own registrations. */
static bool lir_cleanup_direct_leaf(const XrLirModule *m, uint32_t function) {
    const XrLirFunctionData *f = &m->functions[function - 1];
    if (f->kind != XR_LIR_FUNCTION_DEFINITION || !f->error || f->closure.callable) return false;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *body = &f->blocks[b];
        const XrLirInstructionView *term = lir_terminator(f, b);
        if (term->op != XR_LIR_BR && term->op != XR_LIR_CONDBR && !xr_lir_function_exit(term->op))
            return false;
        for (uint32_t n = 0; n + 1 < body->instruction_count; ++n) {
            const XrLirInstructionView *op = &body->instructions[n].view;
            if (op->op == XR_LIR_CALL && m->functions[op->callee - 1].cleanup_owner == function)
                continue;
            if (!lir_cleanup_nopanic(m, f, op)) return false;
        }
    }
    return true;
}

static XrLirStatus lir_cleanup_call_targets(const XrLirModule *m, XrLirDiagnostic *d) {
    enum { UNKNOWN, SAFE_LEAF, NOT_SAFE };
    uint8_t *proofs = NULL;
    XrLirStatus status = XR_LIR_OK;
    for (uint32_t f = 0; f < m->function_count && status == XR_LIR_OK; ++f) {
        const XrLirFunctionData *body = &m->functions[f];
        if (!body->has_cleanups) continue;
        for (uint32_t b = 0; b < body->block_count; ++b) {
            const XrLirInstructionView *term = lir_terminator(body, b);
            if (term->op != XR_LIR_INVOKE || !lir_cleanup_frontier(body, b + 1)) continue;
            if (!proofs) proofs = xr_calloc(m->function_count, sizeof(*proofs));
            if (!proofs) return lir_status_error(d, XR_LIR_OUT_OF_MEMORY, "cleanup call proof allocation failed");
            uint8_t *proof = &proofs[term->callee - 1];
            if (*proof == UNKNOWN) *proof = lir_cleanup_direct_leaf(m, term->callee) ? SAFE_LEAF : NOT_SAFE;
            if (*proof != SAFE_LEAF) {
                status = xr_lir_internal_error(d, "call with active cleanups requires a panic-free leaf", f + 1,
                    b + 1, body->blocks[b].instruction_count, &term->source);
                break;
            }
        }
    }
    xr_free(proofs);
    return status;
}
