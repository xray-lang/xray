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

/* A captured binding is read in the callback, after all intervening stores. */
static const XrXirInstruction *lower_directory_cleanup_call(const Lower *c, uint32_t function) {
    const XrXirFunctionIdentity *ids = c->source->declarations->functions;
    if (function >= c->source->function_count || !ids[function].cleanup_owner) return NULL;
    const XrXirFunction *body = &c->source->functions[function];
    if (body->block_count != 1 || body->blocks[0].frontier || body->blocks[0].panic ||
        body->parameter_count != 1 || body->result != XR_XIR_UNIT || !lower_empty_error(body->error_type))
        return NULL;
    bool read = lower_cleanup_cell(c, body->parameters[0]);
    if ((!read && body->parameters[0] != XR_XIR_I64) || body->instruction_count != 2u + read ||
        body->operand_count != 1 || body->operands[0] != (uint32_t)read)
        return NULL;
    const XrXirInstruction *op = &body->instructions[read];
    if ((read && (body->instructions[0].op != XR_XIR_CELL_READ || body->instructions[0].args[0] ||
                    body->instructions[0].type != XR_XIR_I64)) ||
        op[1].op != XR_XIR_RETURN || op->op != XR_XIR_CALL || op->type != XR_XIR_BOOL ||
        op->args[0] || op->args[1] != 1 || op->immediate < 0 ||
        (uint64_t)op->immediate >= c->source->function_count)
        return NULL;
    uint32_t callee = (uint32_t)op->immediate;
    uint32_t fields[2];
    return ids[callee].native_callable == XR_NATIVE_CALLABLE_IO_DIR_CLOSE &&
        lower_host_abi(c, callee, fields) == XR_NATIVE_HOST_DIRECTORY ? op : NULL;
}

static bool lower_cleanup_literal(Lower *c, const XrXirFunction *f,
    const XrXirInstruction *literal, uint8_t **uses) {
    if (!literal || literal->op != XR_XIR_CONST_STRING) return false;
    if (!*uses) *uses = lower_literal_uses(c, f);
    uint32_t index = (uint32_t)(literal - f->instructions);
    return *uses && (*uses)[index] == 1 && lower_literal_valid(c, f, index);
}

/* This subset has no guest panic port. The two admitted output forms share
 * the host failure policy and direct literal output creates no String owner. */
static bool lower_cleanup_safe(Lower *c, const XrXirFunction *f, const XrXirInstruction *op,
    uint8_t **literal_uses) {
    uint32_t function = (uint32_t)(f - c->source->functions);
    switch (op->op) {
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT: case XR_XIR_PHI:
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
        return lower_cleanup_scalar(op->type);
    case XR_XIR_CONST_STRING:
        return lower_cleanup_literal(c, f, op, literal_uses);
    case XR_XIR_SCALAR_COPY: case XR_XIR_COPY:
        return lower_cleanup_scalar(op->type) || lower_cleanup_cell(c, op->type);
    case XR_XIR_ARRAY_NEW:
        return op->args[1] == 0;
    case XR_XIR_ARRAY_LEN:
        return true;
    case XR_XIR_VIEW_CLEANUP:
        /* Retiring a temporary view loan releases only the recorded loan. */
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
    case XR_XIR_PRINT: {
        if (op->args[1] != 1 || op->args[0] >= f->operand_count) return false;
        uint32_t value = f->operands[op->args[0]];
        return xr_xir_operand_type(f, value) == XR_XIR_I64 ||
            lower_cleanup_literal(c, f, lower_place_definition(f, value), literal_uses);
    }
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
    uint8_t *literal_uses = NULL;
    bool safe = true;
    for (uint32_t b = 0; b < f->block_count && safe; ++b) {
        const XrXirBlock *block = &f->blocks[b];
        safe = !block->panic;
        for (uint32_t i = block->first; i < block->first + block->count && safe; ++i) {
            const XrXirInstruction *op = &f->instructions[i];
            safe = lower_cleanup_safe(c, f, op, &literal_uses) &&
                !(op->op == XR_XIR_CLEANUP_REGISTER &&
                  lower_directory_cleanup_call(c, (uint32_t)op->immediate));
        }
    }
    xr_free(literal_uses);
    *proof = safe ? SAFE : UNSAFE;
    return safe;
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

static bool lower_cleanup_call(LowerFunction *f, XrLirBlock block, uint32_t frontier) {
    Lower *c = f->owner;
    const XrXirInstruction *registration = &f->source->instructions[frontier - 1];
    XrLirValue *arguments = NULL;
    if (!lower_cleanup_arguments(f, registration, &arguments)) return false;
    uint32_t at = c->instruction;
    c->instruction = frontier - 1;
    XrLirInstruction invoke = {.op = XR_LIR_CALL, .type = XR_LIR_UNIT,
        .callee = lower_cleanup_target(f, registration), .operands = arguments,
        .operand_count = registration->args[1], .source = lower_source(c)};
    bool good = lower_build(c, xr_lir_emit_effect(c->module, block, &invoke));
    c->instruction = at;
    xr_free(arguments);
    return good;
}

static bool lower_cleanup_unwind(LowerFunction *f, XrLirBlock block, uint32_t frontier, uint32_t target,
    bool *directory_closed) {
    bool ordinary_cleanup = false;
    while (frontier != target) {
        uint32_t parent, handler;
        if (xr_xir_cleanup_frontier(f->source, frontier, &parent, &handler) != XR_XIR_OK || handler)
            return lower_unsupported(f->owner, "invalid synchronous cleanup frontier");
        const XrXirInstruction *registration = &f->source->instructions[frontier - 1];
        if (lower_directory_cleanup_call(f->owner, (uint32_t)registration->immediate)) {
            if (!directory_closed || ordinary_cleanup)
                return lower_unsupported(f->owner, "directory cleanup requires a complete normal exit");
            *directory_closed = true;
        } else {
            if (directory_closed && *directory_closed)
                return lower_unsupported(f->owner, "mixed directory cleanup frontier");
            ordinary_cleanup = true;
        }
        if (!lower_cleanup_call(f, block, frontier)) return false;
        frontier = parent;
    }
    return true;
}

static bool lower_cleanup_frame_type(const Lower *c, XrXirType type) {
    return type == XR_XIR_UNIT || lower_cleanup_scalar(type) || lower_cleanup_cell(c, type);
}

/* Physical failure may leave this activation after its defer callbacks. Keep
 * frames with owners, loans or callable scopes on the existing refusal path
 * until they can retire those obligations on the new successor. */
static bool lower_cleanup_scalar_frame(const Lower *c, const XrXirFunction *f) {
    if (!lower_empty_error(f->error_type) || !lower_cleanup_frame_type(c, f->result)) return false;
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (!lower_cleanup_scalar(f->parameters[p])) return false;
    for (uint32_t i = 0; i < f->instruction_count; ++i)
        if (!lower_cleanup_frame_type(c, f->instructions[i].type)) return false;
    return true;
}

static bool lower_cleanup_exit(LowerFunction *f, uint32_t block, const XrXirInstruction *term,
    bool *directory_closed) {
    uint32_t frontier = f->source->blocks[block].frontier;
    if (!frontier) return true;
    uint32_t target = 0;
    if (term->op == XR_XIR_CLEANUP_LEAVE || term->op == XR_XIR_CLEANUP_ERROR)
        target = (uint32_t)term->immediate;
    else if (term->op != XR_XIR_RETURN && term->op != XR_XIR_THROW && term->op != XR_XIR_PANIC_ERROR) return true;
    bool normal = (term->op == XR_XIR_RETURN || term->op == XR_XIR_CLEANUP_LEAVE) && !target;
    if (!lower_cleanup_unwind(f, f->blocks[block], frontier, target, normal ? directory_closed : NULL))
        return false;
    if (!*directory_closed || f->directory_scalar_frame) return true;
    f->directory_scalar_frame = lower_cleanup_scalar_frame(f->owner, f->source);
    return f->directory_scalar_frame || lower_unsupported(f->owner, "directory cleanup requires a scalar frame");
}

static bool lower_cleanup_arithmetic(const XrXirInstruction *op) {
    return (op->op == XR_XIR_DIV_INT || op->op == XR_XIR_REM_INT) && op->type == XR_XIR_I64;
}

static bool lower_cleanup_arithmetic_edge(LowerFunction *f, uint32_t b, const XrXirInstruction *op) {
    Lower *c = f->owner;
    const uint32_t frontier = f->source->blocks[b].frontier;
    const uint32_t role = lower_cleanup_role(f, frontier);
    XrLirSource source = lower_source(c);
    XrLirBlock check = f->blocks[b], nonzero = {0}, zero = {0};
    XrLirValue arguments[2] = {{0}}, condition = {0};
    if (!f->exits) {
        f->exits = lower_alloc(c, f->source->block_count, sizeof(*f->exits));
        if (!f->exits) return false;
    }
    XrLirInstruction constant = {.op = XR_LIR_CONST, .type = XR_LIR_I64, .source = source};
    XrLirInstruction compare = {.op = XR_LIR_ICMP, .type = XR_LIR_I1, .predicate = XR_LIR_NE,
        .operands = arguments, .operand_count = 2, .source = source};
    if (!lower_value(f, op->args[1], &arguments[0]) ||
        !lower_build(c, xr_lir_emit_value(c->module, check, &constant, &arguments[1])) ||
        !lower_build(c, xr_lir_emit_value(c->module, check, &compare, &condition)) ||
        !lower_build(c, xr_lir_add_block(c->module, f->function, NULL, 0, &nonzero)) ||
        !lower_build(c, xr_lir_add_block(c->module, f->function, NULL, 0, &zero)) ||
        !lower_build(c, xr_lir_internal_cleanup_block(c->module, check, role, 0, NULL, 0)) ||
        !lower_build(c, xr_lir_internal_cleanup_block(c->module, zero, role, 0, NULL, 0))) return false;
    XrLirInstruction branch = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT, .operands = &condition,
        .operand_count = 1, .edge_count = 2, .edges = {{.target = nonzero}, {.target = zero}}, .source = source};
    XrLirInstruction panic = {.op = XR_LIR_ARITHMETIC_PANIC, .type = XR_LIR_UNIT,
        .bits = op->op == XR_XIR_DIV_INT ? 420 : 421, .source = source};
    if (!lower_build(c, xr_lir_set_terminator(c->module, check, &branch)) ||
        !lower_cleanup_unwind(f, zero, frontier, 0, NULL) ||
        !lower_build(c, xr_lir_set_terminator(c->module, zero, &panic))) return false;
    f->blocks[b] = nonzero;
    return true;
}
