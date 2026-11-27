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
static bool lir_directory_nopanic_name(const XrLirModule *m, const XrLirFunctionData *f,
    const uint8_t *names, uint32_t b, const XrLirInstructionView *op);

static bool lir_cleanup_calls(const XrLirModule *m, const XrLirFunctionData *f,
    const uint8_t *names, uint32_t owner, const XrLirBlockData *block, uint32_t count, uint32_t *frontier,
    bool guarded_arithmetic, bool *scalar_frame) {
    enum { BODY, CLEANUPS, RETIREMENT } phase = BODY;
    bool guarded = *frontier || f->cleanup_owner;
    bool directory_closed = false;
    bool ordinary_cleanup = false;
    for (uint32_t i = 0; i < count; ++i) {
        const XrLirInstructionView *call = &block->instructions[i].view;
        if (!i && guarded_arithmetic) continue;
        if (call->op == XR_LIR_HOST_DIRECTORY_FRAME) {
            if (i || guarded || block != &f->blocks[f->entry - 1]) return false;
            continue;
        }
        if (call->op == XR_LIR_HOST_DIRECTORY_CLOSE_CHECKED || call->op == XR_LIR_HOST_DIRECTORY_RETURN_GUARD)
            return false;
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
            } else if (guarded && !lir_cleanup_nopanic(m, f, call) &&
                !lir_directory_nopanic_name(m, f, names, (uint32_t)(block - f->blocks), call)) return false;
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

static bool lir_checked_predecessor(const XrLirFunctionData *f, const LirGraph *g, uint32_t b,
    uint32_t edge, XrLirOp op) {
    if (!lir_cleanup_one_predecessor(g, b)) return false;
    const XrLirInstructionView *next = lir_terminator(f, g->predecessors[g->offsets[b]]);
    return next->op == op && next->edges[edge].target == b + 1;
}

static bool lir_directory_predecessor(const XrLirFunctionData *f, const LirGraph *g, uint32_t b,
    uint32_t edge) {
    return lir_checked_predecessor(f, g, b, edge, XR_LIR_HOST_DIRECTORY_NEXT_CHECKED);
}

enum { LIR_DIRECTORY_PRESENT = 1, LIR_DIRECTORY_ABSENT };

/* A sole predecessor dominates its receiving block and has already been
 * visited in reverse postorder. Only the exact owner can carry a tag fact. */
static void lir_directory_name_facts(const XrLirFunctionData *f, const LirGraph *g, uint8_t *names) {
    for (uint32_t index = 0; index < g->count; ++index) {
        uint32_t b = g->order[index];
        if (!lir_cleanup_one_predecessor(g, b)) continue;
        const XrLirBlockData *block = &f->blocks[b];
        if (!block->parameter_count) continue;
        uint32_t before = g->predecessors[g->offsets[b]];
        const XrLirInstructionView *term = lir_terminator(f, before);
        if (term->op != XR_LIR_CONDBR && term->op != XR_LIR_BR) continue;
        uint32_t edge = term->edges[0].target == b + 1 ? 0 : 1;
        if (edge >= term->edge_count || term->edges[edge].target != b + 1) continue;
        const XrLirEdgeView *transfer = &term->edges[edge];
        if (transfer->argument_count != block->parameter_count) continue;
        if (term->op == XR_LIR_CONDBR && lir_directory_predecessor(f, g, before, 0)) {
            const XrLirBlockData *receiver = &f->blocks[before];
            const XrLirInstructionView *tag = &receiver->instructions[0].view;
            if (receiver->instruction_count == 2 && receiver->parameter_count == 1 &&
                transfer->argument_count == 1 && transfer->arguments[0] == receiver->parameters[0] &&
                tag->op == XR_LIR_NULLABLE_IS_SOME && tag->operands[0] == receiver->parameters[0] &&
                term->operands[0] == tag->result)
                names[block->parameters[0] - 1] = edge == 0 ? LIR_DIRECTORY_PRESENT : LIR_DIRECTORY_ABSENT;
        } else if (term->op == XR_LIR_BR) {
            for (uint32_t p = 0; p < transfer->argument_count; ++p) {
                uint32_t from = transfer->arguments[p];
                if (f->values[from - 1].block == before + 1)
                    names[block->parameters[p] - 1] = names[from - 1];
            }
        }
    }
}

static bool lir_directory_nopanic_name(const XrLirModule *m, const XrLirFunctionData *f,
    const uint8_t *names, uint32_t b, const XrLirInstructionView *op) {
    if (!names || (op->op != XR_LIR_VALUE_DROP && op->op != XR_LIR_NULLABLE_UNWRAP && op->op != XR_LIR_STRING_LEN))
        return false;
    if (op->op == XR_LIR_STRING_LEN) {
        uint32_t name = op->operands[0];
        if (f->values[name - 1].block != b + 1) return false;
        op = xr_lir_internal_value_definition(f, name);
        if (!op || op->op != XR_LIR_NULLABLE_UNWRAP) return false;
    }
    uint32_t input = op->operands[0];
    const XrLirValueData *value = &f->values[input - 1];
    if (value->block != b + 1 || !lir_directory_string_result(m, value->type)) return false;
    return op->op == XR_LIR_VALUE_DROP ? names[input - 1] != 0 : names[input - 1] == LIR_DIRECTORY_PRESENT;
}

/* Class parameters and retained copies used only before registration are
 * checked by the same value flow as the scalar and Array directory frame. */
static bool lir_directory_prefix_class(const XrLirFunctionData *f, uint32_t id) {
    const XrLirValueData *value = &f->values[id - 1];
    if (value->parameter) {
        if (value->block) return false;
    } else {
        if (lir_cleanup_frontier(f, value->block)) return false;
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, id);
        if (!definition || definition->op != XR_LIR_VALUE_RETAIN) return false;
        const XrLirValueData *formal = &f->values[definition->operands[0] - 1];
        if (!formal->parameter || formal->block) return false;
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        if (!lir_cleanup_frontier(f, b + 1)) continue;
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *op = &block->instructions[i].view;
            for (uint32_t a = 0; a < op->operand_count; ++a)
                if (op->operands[a] == id) return false;
            for (uint32_t e = 0; e < op->edge_count; ++e)
                for (uint32_t a = 0; a < op->edges[e].argument_count; ++a)
                    if (op->edges[e].arguments[a] == id) return false;
        }
    }
    return true;
}

static bool lir_directory_absent_result(const XrLirFunctionData *f, const XrLirValueData *value,
    const XrLirInstructionView *definition) {
    if (!definition || value->type != f->result || lir_cleanup_frontier(f, value->block)) return false;
    if (definition->op == XR_LIR_VALUE_RETAIN)
        definition = xr_lir_internal_value_definition(f, definition->operands[0]);
    return definition && definition->op == XR_LIR_NULLABLE_NONE;
}

static bool lir_directory_frame(const XrLirModule *m, const XrLirFunctionData *f, const LirGraph *g,
    const uint8_t *names) {
    if (f->cleanup_owner || f->error || f->closure.callable) return false;
    for (uint32_t v = 0; v < f->value_count; ++v) {
        const XrLirValueData *value = &f->values[v];
        if (lir_cleanup_scalar(value->type)) continue;
        if (lir_directory_loop_result(m, f->result) && xr_lir_class_payload(m, value->type, NULL) &&
            lir_directory_prefix_class(f, v + 1)) continue;
        if (value->parameter) {
            if (!value->block) return false;
            if ((lir_directory_string_result(m, value->type) &&
                 (lir_directory_predecessor(f, g, value->block - 1, 0) ||
                  (names && names[v]))) ||
                (value->type == XR_LIR_ARRAY && value->array_element == XR_LIR_STRING &&
                 lir_checked_predecessor(f, g, value->block - 1, 0, XR_LIR_CHECKED_EMPTY_ARRAY)) ||
                (value->type == XR_LIR_U64 && (lir_directory_predecessor(f, g, value->block - 1, 1) ||
                 lir_checked_predecessor(f, g, value->block - 1, 1, XR_LIR_CHECKED_EMPTY_ARRAY)))) continue;
            return false;
        }
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, v + 1);
        if (definition && lir_directory_loop_result(m, f->result) && !lir_cleanup_frontier(f, value->block) &&
            (value->type == XR_LIR_STRING || value->type == XR_LIR_ARRAY ||
             (value->type == XR_LIR_ARRAY_CELL && definition->op == XR_LIR_CELL_NEW))) continue;
        if (definition && lir_directory_loop_result(m, f->result) && value->type == XR_LIR_ARRAY_CELL &&
            definition->op == XR_LIR_CELL_NEW) {
            const XrLirValueData *array = &f->values[definition->operands[0] - 1];
            if (array->parameter && array->block && array->array_element == XR_LIR_STRING &&
                lir_checked_predecessor(f, g, array->block - 1, 0, XR_LIR_CHECKED_EMPTY_ARRAY)) continue;
        }
        if (definition && lir_directory_loop_result(m, f->result) && value->type == XR_LIR_ARRAY &&
            value->array_element == XR_LIR_STRING && definition->op == XR_LIR_CELL_READ &&
            f->values[definition->operands[0] - 1].type == XR_LIR_ARRAY_CELL) continue;
        if (lir_directory_array_result(m, f->result) && value->type == f->result && definition &&
            (lir_directory_absent_result(f, value, definition) ||
             (lir_cleanup_frontier(f, value->block) &&
              (definition->op == XR_LIR_NULLABLE_NONE || definition->op == XR_LIR_NULLABLE_SOME)))) continue;
        if (definition && value->type == XR_LIR_STRING && definition->op == XR_LIR_NULLABLE_UNWRAP &&
            lir_directory_nopanic_name(m, f, names, value->block - 1, definition)) continue;
        if (!definition || !((value->type == XR_LIR_I64_CELL && definition->op == XR_LIR_CELL_NEW) ||
            (value->type == XR_LIR_U64 && (definition->op == XR_LIR_HOST_DIRECTORY_CLOSE_CHECKED ||
             definition->op == XR_LIR_CHECKED_ARRAY_PUSH || (definition->op == XR_LIR_CONST && !definition->bits)))))
            return false;
    }
    return true;
}

static bool lir_directory_steps(const XrLirModule *m, uint32_t owner, uint32_t frontier,
    const XrLirBlockData *block, uint32_t *at, uint32_t *status) {
    uint32_t position = *at, previous = 0;
    while (frontier) {
        const XrLirCleanupBlock *role = lir_cleanup_registration(m, owner, frontier);
        if (!role || role->capture_count != 1 || !role->captures) return false;
        const XrLirFunctionData *callback = &m->functions[frontier - 1];
        const XrLirInstructionView *close = lir_cleanup_directory_close(callback);
        if (!close) return false;
        uint32_t token = role->captures[0];
        if (callback->parameter_types[0] == XR_LIR_I64_REF) {
            if (position >= block->instruction_count) return false;
            const XrLirInstructionView *load = &block->instructions[position++].view;
            if (load->op != XR_LIR_CELL_READ || load->operands[0] != token ||
                !lir_cleanup_same_source(&load->source, &close->source)) return false;
            token = load->result;
        }
        if (position >= block->instruction_count) return false;
        const XrLirInstructionView *step = &block->instructions[position++].view;
        if (step->op != XR_LIR_HOST_DIRECTORY_CLOSE_CHECKED || step->bits != frontier || step->operands[0] != token ||
            step->operand_count != (previous ? 2u : 1u) || (previous && step->operands[1] != previous) ||
            !lir_cleanup_same_source(&step->source, &close->source)) return false;
        previous = step->result;
        frontier = role->frontier;
    }
    *at = position;
    *status = previous;
    return true;
}

static bool lir_directory_chain(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t owner, uint32_t frontier, const XrLirInstructionView *next, uint32_t edge) {
    const XrLirBlockData *block = &f->blocks[next->edges[edge].target - 1];
    uint32_t position = 0, previous = 0;
    if (block->cleanup || block->parameter_count != 1 ||
        !lir_directory_steps(m, owner, frontier, block, &position, &previous)) return false;
    if (edge && lir_directory_loop_result(m, f->result))
        while (position < block->instruction_count &&
               block->instructions[position].view.op == XR_LIR_ARRAY_CELL_DROP) ++position;
    if (position >= block->instruction_count) return false;
    const XrLirInstructionView *exit = &block->instructions[position++].view;
    if (exit->op != (edge ? XR_LIR_HOST_DIRECTORY_FAIL : XR_LIR_HOST_DIRECTORY_RETURN_GUARD) ||
        exit->operand_count != 2 || exit->operands[0] != block->parameters[0] ||
        exit->operands[1] != previous) return false;
    if (edge) return position == block->instruction_count && lir_cleanup_same_source(&exit->source, &next->source);
    if (position + 1 != block->instruction_count) return false;
    const XrLirInstructionView *ret = &block->instructions[position].view;
    return ret->op == XR_LIR_RET && ret->operands[0] == block->parameters[0] &&
        lir_cleanup_same_source(&ret->source, &exit->source);
}


/* Both successors share the same tag. Either the receiver retires the name
 * immediately, or the selected edge takes exactly that owner. */
static bool lir_directory_loop_receiver(const XrLirFunctionData *f, uint32_t frontier,
    const XrLirInstructionView *next) {
    uint32_t target = next->edges[0].target;
    const XrLirBlockData *block = &f->blocks[target - 1];
    bool forwards = block->instruction_count == 2;
    if (lir_cleanup_frontier(f, target) != frontier || block->cleanup->registration ||
        block->parameter_count != 1 || (!forwards && block->instruction_count != 3)) return false;
    const XrLirInstructionView *test = &block->instructions[0].view;
    const XrLirInstructionView *drop = &block->instructions[1].view;
    const XrLirInstructionView *branch = lir_terminator(f, target - 1);
    if (test->op != XR_LIR_NULLABLE_IS_SOME || test->operands[0] != block->parameters[0] ||
        (!forwards && (drop->op != XR_LIR_VALUE_DROP || drop->operands[0] != block->parameters[0])) ||
        branch->op != XR_LIR_CONDBR || branch->operands[0] != test->result) return false;
    for (uint32_t e = 0; e < branch->edge_count; ++e) {
        const XrLirEdgeView *edge = &branch->edges[e];
        if (lir_cleanup_frontier(f, edge->target) != frontier) return false;
        if (forwards) {
            if (edge->argument_count != 1 || edge->arguments[0] != block->parameters[0]) return false;
            continue;
        }
        for (uint32_t a = 0; a < edge->argument_count; ++a)
            if (!lir_cleanup_scalar(f->values[edge->arguments[a] - 1].type)) return false;
    }
    return true;
}

static bool lir_directory_array_return_tail(const XrLirFunctionData *f, const XrLirBlockData *block,
    uint32_t at, uint32_t cell) {
    uint32_t end = block->instruction_count - 1;
    const XrLirInstructionView *ret = &block->instructions[end].view;
    if (at >= end) return false;
    const XrLirInstructionView *read = NULL;
    const XrLirInstructionView *op = &block->instructions[at].view;
    if (op->op == XR_LIR_CELL_READ && op->type == XR_LIR_ARRAY &&
        f->values[op->operands[0] - 1].type == XR_LIR_ARRAY_CELL) {
        if (++at >= end) return false;
        read = op;
        op = &block->instructions[at].view;
    }
    if (ret->operands[0] != op->result ||
        (read ? read->operands[0] != cell || op->op != XR_LIR_NULLABLE_SOME || op->operands[0] != read->result :
            cell || op->op != XR_LIR_NULLABLE_NONE)) return false;
    for (++at; at < end; ++at)
        if (!lir_cleanup_retirement(block->instructions[at].view.op)) return false;
    return true;
}

static bool lir_directory_loop_exit(const XrLirModule *m, const XrLirFunctionData *f,
    const uint8_t *names, uint32_t owner, uint32_t frontier, const XrLirBlockData *block, bool *scalar_frame) {
    if (block->cleanup->registration || block->instruction_count < 3) return false;
    uint32_t guard_index = block->instruction_count - 2;
    if (lir_directory_array_result(m, f->result)) {
        while (guard_index && block->instructions[guard_index].view.op != XR_LIR_HOST_DIRECTORY_RETURN_GUARD)
            --guard_index;
    }
    uint32_t start = guard_index;
    const XrLirInstructionView *guard = &block->instructions[guard_index].view;
    const XrLirInstructionView *ret = &block->instructions[block->instruction_count - 1].view;
    if (guard->op != XR_LIR_HOST_DIRECTORY_RETURN_GUARD ||
        !lir_cleanup_same_source(&guard->source, &ret->source)) return false;
    uint32_t cell = guard->operand_count == 2 ? guard->operands[0] : 0;
    if (lir_directory_array_result(m, f->result)) {
        if (!lir_directory_array_return_tail(f, block, guard_index + 1, cell)) return false;
    } else if (cell) return false;
    while (start && lir_cleanup_retirement(block->instructions[start - 1].view.op)) --start;
    uint32_t retired = start;
    for (uint32_t role = frontier; role;) {
        const XrLirCleanupBlock *registration = lir_cleanup_registration(m, owner, role);
        if (!registration || !lir_cleanup_directory_close(&m->functions[role - 1])) return false;
        uint32_t steps = m->functions[role - 1].parameter_types[0] == XR_LIR_I64_REF ? 2u : 1u;
        if (start < steps) return false;
        start -= steps;
        role = registration->frontier;
    }
    uint32_t at = start, status = 0, remaining = frontier;
    return lir_directory_steps(m, owner, frontier, block, &at, &status) &&
        at == retired && status == guard->operands[guard->operand_count - 1] &&
        lir_cleanup_calls(m, f, names, owner, block, start, &remaining, false, scalar_frame) &&
        remaining == frontier;
}

static const XrLirInstructionView *lir_allocation_cleanup_print(const XrLirModule *m,
    const XrLirFunctionData *role) {
    if (!role->cleanup_owner || role->has_cleanups || role->error || role->result != XR_LIR_UNIT ||
        role->block_count != 1 || !role->blocks || role->parameter_count > 1 ||
        (role->parameter_count && (!role->parameters || !role->parameter_types))) return NULL;
    const XrLirBlockData *body = &role->blocks[0];
    if (!body->instructions || body->instruction_count < 2 || body->instruction_count > 3) return NULL;
    const XrLirInstructionView *print = &body->instructions[body->instruction_count - 2].view;
    const XrLirInstructionView *ret = &body->instructions[body->instruction_count - 1].view;
    if (ret->op != XR_LIR_RET || ret->operand_count || print->op != XR_LIR_CALL || !print->callee ||
        print->callee > m->function_count || print->operand_count != 1 || !print->operands) return NULL;
    const XrLirFunctionData *output = &m->functions[print->callee - 1];
    if (output->kind != XR_LIR_FUNCTION_EXTERNAL || output->extern_kind != XR_LIR_EXTERN_PRINT_I64)
        return NULL;
    return print;
}

static bool lir_allocation_cleanup_chain(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t owner, uint32_t frontier, const XrLirInstructionView *allocation, bool directory) {
    const XrLirBlockData *block = &f->blocks[allocation->edges[1].target - 1];
    if (block->cleanup || block->parameter_count != 1) return false;
    uint32_t position = 0, close = 0;
    if (directory) {
        if (!lir_directory_steps(m, owner, frontier, block, &position, &close)) return false;
        frontier = 0;
    }
    while (frontier) {
        const XrLirCleanupBlock *registration = lir_cleanup_registration(m, owner, frontier);
        if (!registration || (registration->capture_count && !registration->captures)) return false;
        const XrLirFunctionData *role = &m->functions[frontier - 1];
        if (registration->capture_count != role->parameter_count) return false;
        const XrLirInstructionView *print = lir_allocation_cleanup_print(m, role);
        if (!print) return false;
        uint32_t input = registration->capture_count ? registration->captures[0] : 0;
        const XrLirBlockData *body = &role->blocks[0];
        if (body->instruction_count == 3) {
            const XrLirInstructionView *original = &body->instructions[0].view;
            if (position >= block->instruction_count || original->result != print->operands[0] ||
                original->type != XR_LIR_I64) return false;
            const XrLirInstructionView *copy = &block->instructions[position++].view;
            if (copy->op != original->op || copy->type != XR_LIR_I64 ||
                !lir_cleanup_same_source(&copy->source, &original->source)) return false;
            if (original->op == XR_LIR_CONST) {
                if (input || copy->bits != original->bits) return false;
            } else if (original->op == XR_LIR_CELL_READ) {
                if (!input || role->parameter_types[0] != XR_LIR_I64_REF || original->operand_count != 1 ||
                    !original->operands || original->operands[0] != role->parameters[0] ||
                    !copy->operands || copy->operands[0] != input) return false;
            } else return false;
            input = copy->result;
        } else if (!input || role->parameter_types[0] != XR_LIR_I64 ||
            print->operands[0] != role->parameters[0]) return false;
        if (!input || position >= block->instruction_count) return false;
        const XrLirInstructionView *copy = &block->instructions[position++].view;
        if (copy->op != XR_LIR_CALL || copy->callee != print->callee ||
            copy->operand_count != 1 || copy->operands[0] != input ||
            !lir_cleanup_same_source(&copy->source, &print->source)) return false;
        frontier = registration->frontier;
    }
    if (position + 1 != block->instruction_count) return false;
    const XrLirInstructionView *exit = &block->instructions[position].view;
    return exit->op == XR_LIR_MANAGED_FAILURE && exit->operands[0] == block->parameters[0] &&
        exit->operand_count == (directory ? 2u : 1u) && (!directory || exit->operands[1] == close) &&
        lir_cleanup_same_source(&exit->source, &allocation->source);
}

/* The cleanup suffix owns no slot before allocation. Non-scalar values
 * must be supplied on the normal edge or be defined after its receiver. */
static bool lir_allocation_cleanup_frame(const XrLirModule *m, const XrLirFunctionData *f, const LirGraph *g,
    const XrLirInstructionView *allocation, bool directory) {
    uint32_t normal = f->blocks[allocation->edges[0].target - 1].parameters[0];
    uint32_t status = f->blocks[allocation->edges[1].target - 1].parameters[0];
    if (f->error || f->cleanup_owner || f->closure.callable) return false;
    for (uint32_t i = 0; i < f->value_count; ++i) {
        const XrLirValueData *value = &f->values[i];
        if (lir_cleanup_scalar(value->type) || i + 1 == normal || i + 1 == status) continue;
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, i + 1);
        if (value->type == XR_LIR_I64_CELL && definition && definition->op == XR_LIR_CELL_NEW) continue;
        if ((directory || value->type == XR_LIR_ARRAY || value->type == XR_LIR_ARRAY_CELL) &&
            value->block && lir_dominates_use(f, g, normal, value->block - 1, value->instruction)) continue;
        if (directory && (xr_lir_class_payload(m, value->type, NULL) ||
            (lir_directory_array_result(m, f->result) && lir_directory_absent_result(f, value, definition)) ||
            (value->type == XR_LIR_U64 && value->block == allocation->edges[1].target &&
             definition && definition->op == XR_LIR_HOST_DIRECTORY_CLOSE_CHECKED))) continue;
        return false;
    }
    return true;
}

static const XrLirInstructionView *lir_directory_push_status(const XrLirFunctionData *f,
    const XrLirInstructionView *branch) {
    if (branch->op != XR_LIR_CONDBR) return NULL;
    const XrLirInstructionView *compare = xr_lir_internal_value_definition(f,branch->operands[0]);
    if (!compare || compare->op != XR_LIR_ICMP || compare->predicate != XR_LIR_EQ) return NULL;
    const XrLirInstructionView *push = xr_lir_internal_value_definition(f,compare->operands[0]);
    return push && push->op == XR_LIR_CHECKED_ARRAY_PUSH ? push : NULL;
}

static bool lir_directory_push_exit(const XrLirModule *m, const XrLirFunctionData *f,
    const LirGraph *g, const uint8_t *names, uint32_t owner, uint32_t from, uint32_t b,
    const XrLirInstructionView *push, bool *scalar_frame) {
    const XrLirBlockData *block = &f->blocks[b];
    const XrLirInstructionView *branch = lir_terminator(f,b);
    uint32_t instruction = f->values[push->result - 1].instruction;
    if (!from || block->cleanup->registration || f->values[push->result - 1].block != b + 1 ||
        !instruction || instruction + 3 >= block->instruction_count) return false;
    const XrLirInstructionView *unwrap = xr_lir_internal_value_definition(f,push->operands[1]);
    if (!unwrap || unwrap->op != XR_LIR_NULLABLE_UNWRAP ||
        !lir_directory_nopanic_name(m,f,names,b,unwrap)) return false;
    const XrLirInstructionView *zero = &block->instructions[block->instruction_count - 3].view;
    const XrLirInstructionView *compare = &block->instructions[block->instruction_count - 2].view;
    if (zero->op != XR_LIR_CONST || zero->type != XR_LIR_U64 || zero->bits ||
        compare->op != XR_LIR_ICMP || compare->result != branch->operands[0] ||
        compare->operands[1] != zero->result ||
        !lir_cleanup_same_source(&push->source,&branch->source)) return false;
    for (uint32_t i = instruction + 1; i + 3 < block->instruction_count; ++i)
        if (!lir_cleanup_retirement(block->instructions[i].view.op)) return false;
    uint32_t remaining = from;
    if (!lir_cleanup_calls(m,f,names,owner,block,instruction,&remaining,false,scalar_frame) ||
        remaining != from || lir_cleanup_frontier(f,branch->edges[0].target) != from) return false;
    uint32_t target = branch->edges[1].target - 1;
    const XrLirBlockData *failure = &f->blocks[target];
    uint32_t at = 0, close = 0;
    if (failure->cleanup || failure->parameter_count || !lir_cleanup_one_predecessor(g,target) ||
        !lir_directory_steps(m,owner,from,failure,&at,&close) || at + 2 != failure->instruction_count)
        return false;
    const XrLirInstructionView *drop = &failure->instructions[at].view;
    const XrLirInstructionView *report = &failure->instructions[at + 1].view;
    return drop->op == XR_LIR_ARRAY_CELL_DROP && drop->operands[0] == push->operands[0] &&
        report->op == XR_LIR_MANAGED_FAILURE && report->operand_count == 2 &&
        report->operands[0] == push->result && report->operands[1] == close &&
        lir_cleanup_same_source(&push->source,&report->source);
}

static bool lir_cleanup_verified_paths(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t owner, const LirGraph *g, const uint8_t *names) {
    if (!f->has_cleanups && !f->cleanup_owner) return true;
    if (lir_cleanup_frontier(f, f->entry)) return false;
    const XrLirInstructionView *entry = &f->blocks[f->entry - 1].instructions[0].view;
    bool directory = entry->op == XR_LIR_HOST_DIRECTORY_FRAME, frame_source = false;
    bool directory_loop = directory && lir_directory_loop_result(m, f->result);
    if (directory && !lir_directory_frame(m, f, g, names)) return false;
    bool scalar_frame = false;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        const XrLirCleanupBlock *meta = block->cleanup;
        const XrLirInstructionView *term = lir_terminator(f, b);
        if ((directory && (lir_directory_predecessor(f, g, b, 0) || lir_directory_predecessor(f, g, b, 1))) ||
            (directory_loop && lir_checked_predecessor(f, g, b, 1, XR_LIR_CHECKED_EMPTY_ARRAY)))
            continue;
        if (directory_loop && term->op == XR_LIR_MANAGED_FAILURE && lir_cleanup_one_predecessor(g,b)) {
            const XrLirInstructionView *origin = lir_terminator(f,g->predecessors[g->offsets[b]]);
            if (lir_directory_push_status(f,origin) && origin->edges[1].target == b + 1) continue;
        }
        uint32_t from = meta ? meta->frontier : 0, remaining = from;
        if (from && !lir_cleanup_registration(m, owner, from)) return false;
        const XrLirInstructionView *push = lir_directory_push_status(f,term);
        if (push) {
            if (!directory_loop || !lir_directory_push_exit(m,f,g,names,owner,from,b,push,&scalar_frame))
                return false;
            continue;
        }
        if (directory_loop && from && term->op == XR_LIR_RET) {
            if (!lir_directory_loop_exit(m, f, names, owner, from, block, &scalar_frame)) return false;
            continue;
        }
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
        if (!lir_cleanup_calls(m, f, names, owner, block, block->instruction_count - 1,
                &remaining, guarded_arithmetic, &scalar_frame)) return false;
        if (meta && meta->registration) {
            uint32_t target = meta->registration;
            if (lir_cleanup_registration(m, owner, target) != meta || target == from ||
                remaining != from || term->op != XR_LIR_BR ||
                !lir_cleanup_captures(f, g, b, meta, &m->functions[target - 1])) return false;
            remaining = target;
        }
        if (term->op == XR_LIR_HOST_DIRECTORY_NEXT_CHECKED) {
            bool matching_source = lir_cleanup_same_source(&entry->source, &term->source);
            if (!directory || !from || remaining != from ||
                (directory_loop && (frame_source || !matching_source)) ||
                !lir_cleanup_one_predecessor(g, term->edges[0].target - 1) ||
                !lir_cleanup_one_predecessor(g, term->edges[1].target - 1) ||
                !(directory_loop ? lir_directory_loop_receiver(f, from, term) :
                    lir_directory_chain(m, f, owner, from, term, 0)) ||
                !lir_directory_chain(m, f, owner, from, term, 1))
                return false;
            frame_source |= matching_source;
            continue;
        }
        if (term->op == XR_LIR_CHECKED_EMPTY_ARRAY) {
            if (!from || remaining != from ||
                !lir_cleanup_one_predecessor(g, term->edges[0].target - 1) ||
                !lir_cleanup_one_predecessor(g, term->edges[1].target - 1) ||
                lir_cleanup_frontier(f, term->edges[0].target) != from ||
                !lir_allocation_cleanup_frame(m, f, g, term, directory_loop) ||
                !lir_allocation_cleanup_chain(m, f, owner, from, term, directory_loop)) return false;
            continue;
        }
        if (term->op == XR_LIR_MANAGED_FAILURE) {
            if (!lir_cleanup_one_predecessor(g, b)) return false;
            const XrLirInstructionView *origin = lir_terminator(f, g->predecessors[g->offsets[b]]);
            if (origin->op != XR_LIR_CHECKED_EMPTY_ARRAY || origin->edges[1].target != b + 1) return false;
            continue;
        }
        if (term->op == XR_LIR_HOST_DIRECTORY_FAIL) return false;
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
    return !directory || frame_source;
}

static bool lir_cleanup_paths(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t owner, const LirGraph *g) {
    bool directory_loop = f->has_cleanups && lir_directory_loop_result(m, f->result) &&
        f->blocks[f->entry - 1].instructions[0].view.op == XR_LIR_HOST_DIRECTORY_FRAME;
    if (!directory_loop) return lir_cleanup_verified_paths(m, f, owner, g, NULL);
    uint8_t *names = xr_calloc(f->value_count, sizeof(*names));
    if (!names) return false;
    lir_directory_name_facts(f, g, names);
    bool valid = lir_cleanup_verified_paths(m, f, owner, g, names);
    xr_free(names);
    return valid;
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
