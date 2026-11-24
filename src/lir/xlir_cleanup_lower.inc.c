/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_cleanup_lower.inc.c - Lower checked synchronous cleanup frontiers
 */

static bool lower_cleanup_scalar(XrXirType type) {
    return type == XR_XIR_I64 || type == XR_XIR_BOOL;
}

static bool lower_cleanup_cell(const Lower *c, XrXirType type) {
    return xr_xir_cell_element(c->source->types, type) == XR_XIR_I64;
}

/* This subset has no guest panic port. In particular, a Never error bound
 * proves nothing about panic. Output here uses the scalar host sink. */
static bool lower_cleanup_safe(const Lower *c, const XrXirFunction *f, const XrXirInstruction *op) {
    uint32_t function = (uint32_t)(f - c->source->functions);
    switch (op->op) {
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_PHI:
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
        return lower_cleanup_scalar(op->type);
    case XR_XIR_SCALAR_COPY: case XR_XIR_COPY:
        return lower_cleanup_scalar(op->type) || lower_cleanup_cell(c, op->type);
    case XR_XIR_ARRAY_NEW:
        return op->args[1] == 0;
    case XR_XIR_ARRAY_LEN:
        return true;
    case XR_XIR_CELL_PLACE: {
        const XrXirInstruction *cell = lower_place_definition(f, op->args[0]);
        return cell && cell->op == XR_XIR_CELL_NEW && lower_type(c, cell->type) == XR_LIR_ARRAY_CELL;
    }
    case XR_XIR_CELL_NEW:
        return lower_cleanup_cell(c, op->type) || lower_type(c, op->type) == XR_LIR_ARRAY_CELL;
    case XR_XIR_CELL_READ: {
        XrXirType type = xr_xir_operand_type(f, op->args[0]);
        return lower_cleanup_cell(c, type) || lower_type(c, type) == XR_LIR_ARRAY_CELL;
    }
    case XR_XIR_CELL_WRITE:
        return lower_cleanup_cell(c, xr_xir_operand_type(f, op->args[0]));
    case XR_XIR_EQUAL: case XR_XIR_EQ_INT: case XR_XIR_NE_INT:
    case XR_XIR_LT_INT: case XR_XIR_LE_INT: case XR_XIR_GT_INT: case XR_XIR_GE_INT:
        return lower_cleanup_scalar(xr_xir_operand_type(f, op->args[0]));
    case XR_XIR_ENUM_NEW:
        return op->type == lower_error_bound(c, function) && lower_is_plain_error_enum(c, op->type);
    case XR_XIR_PRINT:
        return op->args[1] == 1 && xr_xir_operand_type(f, f->operands[op->args[0]]) == XR_XIR_I64;
    case XR_XIR_RETURN:
        return f->result == XR_XIR_UNIT || lower_cleanup_scalar(f->result);
    case XR_XIR_THROW:
        return lower_function_error(c, function) &&
            xr_xir_operand_type(f, op->args[0]) == lower_error_bound(c, function);
    case XR_XIR_JUMP: case XR_XIR_BRANCH:
    case XR_XIR_CLEANUP_REGISTER: case XR_XIR_CLEANUP_LEAVE:
        return true;
    default:
        return false;
    }
}

/* A direct leaf must also be safe before its own first registration. Each
 * registered helper is checked over its entire body by cleanup admission when
 * the owning registration puts it on the lowering queue. */
static bool lower_cleanup_direct_leaf(Lower *c, uint32_t function) {
    enum { UNKNOWN, SAFE, UNSAFE };
    if (!c->cleanup_leaves) {
        c->cleanup_leaves = lower_alloc(c, c->source->function_count, sizeof(*c->cleanup_leaves));
        if (!c->cleanup_leaves) return false;
    }
    uint8_t *proof = &c->cleanup_leaves[function];
    if (*proof != UNKNOWN) return *proof == SAFE;
    const XrXirFunction *f = &c->source->functions[function];
    *proof = UNSAFE;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        if (block->panic) return false;
        for (uint32_t i = block->first; i < block->first + block->count; ++i)
            if (!lower_cleanup_safe(c, f, &f->instructions[i])) return false;
    }
    *proof = SAFE;
    return true;
}

static bool lower_cleanup_admit(Lower *c, const XrXirFunction *f, const XrXirInstruction *op) {
    if (op->immediate < 0 || (uint64_t)op->immediate >= c->source->function_count ||
        op->args[0] > f->operand_count || op->args[1] > f->operand_count - op->args[0])
        return lower_unsupported(c, "invalid cleanup registration");
    uint32_t index = (uint32_t)op->immediate;
    const XrXirFunctionIdentity *ids = c->source->declarations->functions;
    const XrXirFunction *target = &c->source->functions[index];
    if (ids[index].cleanup_owner != c->function + 1 || ids[index].module != ids[c->function].module ||
        target->result != XR_XIR_UNIT || !lower_empty_error(target->error_type) ||
        target->parameter_count != op->args[1] || !target->block_count)
        return lower_unsupported(c, "invalid cleanup body role");
    for (uint32_t p = 0; p < target->parameter_count; ++p) {
        const XrXirType type = target->parameters[p];
        bool binding = lower_cleanup_cell(c, type);
        if (!binding && !lower_cleanup_scalar(type))
            return lower_unsupported(c, "cleanup capture is not a scalar or live scalar binding");
        c->scalar_bindings |= binding;
    }
    return lower_mark(c, index);
}

static bool lower_cleanup_arguments(LowerFunction *f, const XrXirInstruction *registration,
    XrLirValue **output) {
    uint32_t count = registration->args[1];
    if (!count) return true;
    XrLirValue *arguments = lower_alloc(f->owner, count, sizeof(*arguments));
    if (!arguments) return false;
    for (uint32_t a = 0; a < count; ++a) {
        uint32_t value = f->source->operands[registration->args[0] + a];
        if (!lower_value(f, value, &arguments[a])) { xr_free(arguments); return false; }
    }
    *output = arguments;
    return true;
}

static XrLirFunction lower_cleanup_target(const LowerFunction *f, const XrXirInstruction *registration) {
    return f->owner->functions[registration->immediate];
}

static uint32_t lower_cleanup_role(const LowerFunction *f, uint32_t frontier) {
    return frontier ? lower_cleanup_target(f, &f->source->instructions[frontier - 1]).id : 0;
}

static bool lower_cleanup_block(LowerFunction *f, uint32_t block, const XrXirInstruction *term) {
    Lower *c = f->owner;
    const XrXirFunction *source = f->source;
    uint32_t frontier = source->blocks[block].frontier;
    uint32_t role = lower_cleanup_role(f, frontier);
    if (term->op != XR_XIR_CLEANUP_REGISTER)
        return !frontier || lower_build(c,
            xr_lir_internal_cleanup_block(c->module, f->blocks[block], role, 0, NULL, 0));
    XrLirValue *arguments = NULL;
    if (!lower_cleanup_arguments(f, term, &arguments)) return false;
    XrLirFunction target = lower_cleanup_target(f, term);
    bool good = lower_build(c, xr_lir_internal_cleanup_block(c->module, f->blocks[block],
        role, target.id, arguments, term->args[1]));
    xr_free(arguments);
    return good;
}

static bool lower_cleanup_call(LowerFunction *f, uint32_t block, uint32_t frontier) {
    Lower *c = f->owner;
    const XrXirInstruction *registration = &f->source->instructions[frontier - 1];
    XrLirValue *arguments = NULL;
    if (!lower_cleanup_arguments(f, registration, &arguments)) return false;
    uint32_t at = c->instruction;
    c->instruction = frontier - 1;
    XrLirInstruction invoke = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT,
        .callee = lower_cleanup_target(f, registration), .operands = arguments,
        .operand_count = registration->args[1], .source = lower_source(c)};
    bool good = lower_build(c, xr_lir_emit_effect(c->module, f->blocks[block], &invoke));
    c->instruction = at;
    xr_free(arguments);
    return good;
}

static bool lower_cleanup_exit(LowerFunction *f, uint32_t block, const XrXirInstruction *term) {
    uint32_t frontier = f->source->blocks[block].frontier;
    if (!frontier) return true;
    uint32_t target = 0;
    if (term->op == XR_XIR_CLEANUP_LEAVE || term->op == XR_XIR_CLEANUP_ERROR)
        target = (uint32_t)term->immediate;
    else if (term->op != XR_XIR_RETURN && term->op != XR_XIR_THROW && term->op != XR_XIR_PANIC_ERROR) return true;
    while (frontier != target) {
        uint32_t parent, handler;
        if (xr_xir_cleanup_frontier(f->source, frontier, &parent, &handler) != XR_XIR_OK || handler)
            return lower_unsupported(f->owner, "invalid synchronous cleanup frontier");
        if (!lower_cleanup_call(f, block, frontier)) return false;
        frontier = parent;
    }
    return true;
}
