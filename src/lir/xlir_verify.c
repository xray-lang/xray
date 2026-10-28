/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_verify.c - Scalar signatures, graph structure and SSA dominance
 *
 * KEY CONCEPT:
 *   Validation derives dominance from the actual edges and checks every
 *   definition, including block parameters and values passed along backedges.
 */
#include "xlir_internal.h"
#include "xlir_verify.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <stdio.h>
#include <string.h>

static const char *const lir_type_names[] = {"invalid", "unit", "i1",  "i8",  "i16", "i32", "i64",
                                             "u8",      "u16",  "u32", "u64", "f32", "f64", "ptr", "i64_cell"};
static const char *const lir_op_names[] = {
    "invalid",
#define XR_LIR_OP(name, text, arity) text,
#include "xlir_ops.def"
#undef XR_LIR_OP
};
static const int lir_arities[] = {
    -1,
#define XR_LIR_OP(name, text, arity) arity,
#include "xlir_ops.def"
#undef XR_LIR_OP
};

XR_FUNC const char *xr_lir_type_name(XrLirType type) {
    return type > XR_LIR_TYPE_INVALID && type < XR_LIR_TYPE_COUNT ? lir_type_names[type]
                                                                  : "invalid";
}
XR_FUNC const char *xr_lir_op_name(XrLirOp op) {
    return op > XR_LIR_OP_INVALID && op < XR_LIR_OP_COUNT ? lir_op_names[op] : "invalid";
}
XR_FUNC bool xr_lir_internal_type_valid(XrLirType type, bool unit) {
    return type > XR_LIR_TYPE_INVALID && type < XR_LIR_TYPE_COUNT && (unit || type != XR_LIR_UNIT);
}
XR_FUNC bool xr_lir_internal_terminal(XrLirOp op) {
    return op == XR_LIR_BR || op == XR_LIR_CONDBR || op == XR_LIR_RET;
}
XR_FUNC XrLirStatus xr_lir_internal_error(XrLirDiagnostic *d, const char *message,
                                          uint32_t function, uint32_t block, uint32_t instruction,
                                          const XrLirSource *source) {
    if (d) {
        *d = (XrLirDiagnostic) {.status = XR_LIR_VERIFY_ERROR,
                                .function = function,
                                .block = block,
                                .instruction = instruction};
        if (source)
            d->source = *source;
        snprintf(d->message, sizeof(d->message), "%s", message);
    }
    return XR_LIR_VERIFY_ERROR;
}

static XrLirStatus lir_status_error(XrLirDiagnostic *d, XrLirStatus status, const char *message) {
    xr_lir_internal_error(d, message, 0, 0, 0, NULL);
    if (d)
        d->status = status;
    return status;
}

static bool lir_integer(XrLirType type) {
    return type >= XR_LIR_I8 && type <= XR_LIR_U64;
}
static bool lir_float(XrLirType type) {
    return type == XR_LIR_F32 || type == XR_LIR_F64;
}
static bool lir_constant_valid(XrLirType type, uint64_t bits) {
    if (type == XR_LIR_I1)
        return bits <= 1;
    if (type == XR_LIR_PTR)
        return bits == 0;
    if (lir_integer(type)) {
        unsigned ordinal =
            type <= XR_LIR_I64 ? (unsigned) (type - XR_LIR_I8) : (unsigned) (type - XR_LIR_U8);
        unsigned width = 8u << ordinal;
        return width == 64 || (bits >> width) == 0;
    }
    if (type == XR_LIR_F32) {
        if (bits >> 32)
            return false;
        bool nan =
            (bits & UINT64_C(0x7f800000)) == UINT64_C(0x7f800000) && (bits & UINT64_C(0x007fffff));
        return !nan || bits == UINT64_C(0x7fc00000);
    }
    if (type == XR_LIR_F64) {
        bool nan = (bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000) &&
                   (bits & UINT64_C(0x000fffffffffffff));
        return !nan || bits == UINT64_C(0x7ff8000000000000);
    }
    return false;
}

static bool lir_signature_matches(const XrLirFunctionData *f, const XrLirInstructionView *v,
                                  const XrLirType *parameters, uint32_t count, XrLirType result) {
    if (v->operand_count != count || v->type != result)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (f->values[v->operands[i] - 1].type != parameters[i])
            return false;
    return true;
}

static bool lir_scalar_instruction_types(const XrLirFunctionData *f,
                                         const XrLirInstructionView *v) {
    XrLirType a = v->operand_count ? f->values[v->operands[0] - 1].type : XR_LIR_TYPE_INVALID;
    XrLirType b = v->operand_count > 1 ? f->values[v->operands[1] - 1].type : XR_LIR_TYPE_INVALID;
    switch (v->op) {
        case XR_LIR_CONST:
            return lir_constant_valid(v->type, v->bits);
        case XR_LIR_CELL_NEW:
            return v->type == XR_LIR_I64_CELL && a == XR_LIR_I64;
        case XR_LIR_CELL_READ:
            return v->type == XR_LIR_I64 && a == XR_LIR_I64_CELL;
        case XR_LIR_CELL_WRITE:
            return v->type == XR_LIR_UNIT && a == XR_LIR_I64_CELL && b == XR_LIR_I64;
        case XR_LIR_ADD:
        case XR_LIR_SUB:
        case XR_LIR_MUL:
        case XR_LIR_DIV:
        case XR_LIR_REM:
            return lir_integer(v->type) && a == v->type && b == v->type;
        case XR_LIR_NEG:
            return lir_integer(v->type) && a == v->type;
        case XR_LIR_FADD:
        case XR_LIR_FSUB:
        case XR_LIR_FMUL:
        case XR_LIR_FDIV:
            return lir_float(v->type) && a == v->type && b == v->type;
        case XR_LIR_FNEG:
            return lir_float(v->type) && a == v->type;
        case XR_LIR_ICMP:
            return v->type == XR_LIR_I1 && a == b &&
                   (lir_integer(a) || ((a == XR_LIR_I1 || a == XR_LIR_PTR) &&
                                       (v->predicate == XR_LIR_EQ || v->predicate == XR_LIR_NE)));
        case XR_LIR_FCMP:
            return v->type == XR_LIR_I1 && lir_float(a) && a == b;
        case XR_LIR_INTCAST:
            return lir_integer(a) && lir_integer(v->type);
        case XR_LIR_ITOF:
            return lir_integer(a) && lir_float(v->type);
        case XR_LIR_FCAST:
            return lir_float(a) && lir_float(v->type);
        case XR_LIR_BR:
            return v->type == XR_LIR_UNIT;
        case XR_LIR_CONDBR:
            return v->type == XR_LIR_UNIT && a == XR_LIR_I1;
        case XR_LIR_RET:
            return v->type == XR_LIR_UNIT &&
                   v->operand_count == (f->result == XR_LIR_UNIT ? 0u : 1u) &&
                   (f->result == XR_LIR_UNIT || a == f->result);
        default:
            return false;
    }
}

static bool lir_edges_valid(const XrLirFunctionData *f, const XrLirInstructionView *v) {
    for (uint32_t e = 0; e < v->edge_count; ++e) {
        const XrLirEdgeView *edge = &v->edges[e];
        if (!edge->target || edge->target > f->block_count ||
            (edge->argument_count && !edge->arguments))
            return false;
        const XrLirBlockData *target = &f->blocks[edge->target - 1];
        if (edge->argument_count != target->parameter_count ||
            (target->parameter_count && !target->parameters))
            return false;
        for (uint32_t i = 0; i < edge->argument_count; ++i) {
            uint32_t id = edge->arguments[i], parameter = target->parameters[i];
            if (!id || id > f->value_count || !parameter || parameter > f->value_count ||
                f->values[id - 1].type != f->values[parameter - 1].type)
                return false;
        }
    }
    return true;
}

XR_FUNC XrLirStatus xr_lir_internal_instruction_check(const XrLirModule *m, uint32_t function,
                                                      const XrLirInstructionView *v,
                                                      XrLirDiagnostic *d) {
    if (d)
        memset(d, 0, sizeof(*d));
    if (!m || !function || function > m->function_count || !v)
        return lir_status_error(d, XR_LIR_INVALID_ARGUMENT,
                                "invalid LIR instruction check argument");
    const XrLirFunctionData *f = &m->functions[function - 1];
    const char *message = "instruction type or signature mismatch";
    if (v->op <= XR_LIR_OP_INVALID || v->op >= XR_LIR_OP_COUNT ||
        !xr_lir_internal_type_valid(v->type, true))
        goto invalid;
    if ((v->operand_count && !v->operands) ||
        (lir_arities[v->op] >= 0 && v->operand_count != (uint32_t) lir_arities[v->op]))
        goto invalid;
    for (uint32_t i = 0; i < v->operand_count; ++i)
        if (!v->operands[i] || v->operands[i] > f->value_count) {
            message = "instruction uses an undefined value";
            goto invalid;
        }
    bool comparison = v->op == XR_LIR_ICMP || v->op == XR_LIR_FCMP;
    if (comparison ? v->predicate < XR_LIR_EQ || v->predicate > XR_LIR_GE
                   : v->predicate != XR_LIR_PREDICATE_NONE)
        goto invalid;
    if (v->op != XR_LIR_CONST && v->bits)
        goto invalid;
    if ((v->op != XR_LIR_CALL && v->callee) || (v->op != XR_LIR_CALL_INTRINSIC && v->intrinsic))
        goto invalid;
    uint32_t edges = v->op == XR_LIR_BR ? 1 : (v->op == XR_LIR_CONDBR ? 2 : 0);
    if (v->edge_count != edges || !lir_edges_valid(f, v)) {
        message = "control flow edge target or parameter signature mismatch";
        goto invalid;
    }
    if (v->op == XR_LIR_CALL) {
        if (!v->callee || v->callee > m->function_count)
            goto invalid;
        const XrLirFunctionData *callee = &m->functions[v->callee - 1];
        if (!lir_signature_matches(f, v, callee->parameter_types, callee->parameter_count,
                                   callee->result))
            goto invalid;
        return XR_LIR_OK;
    }
    if (v->op == XR_LIR_CALL_INTRINSIC) {
        for (uint32_t i = 0; i < m->intrinsic_count; ++i) {
            const XrLirIntrinsic *entry = &m->intrinsics[i];
            if (entry->id != v->intrinsic)
                continue;
            bool cell = entry->result == XR_LIR_I64_CELL;
            for (uint32_t p = 0; p < entry->parameter_count; ++p)
                cell |= entry->parameters[p] == XR_LIR_I64_CELL;
            if (cell || !entry->scalar_sync ||
                !lir_signature_matches(f, v, entry->parameters, entry->parameter_count,
                                       entry->result))
                goto invalid;
            return XR_LIR_OK;
        }
        xr_lir_internal_error(d, "intrinsic is not registered", function, 0, 0, &v->source);
        if (d)
            d->status = XR_LIR_UNSUPPORTED;
        return XR_LIR_UNSUPPORTED;
    }
    if (!lir_scalar_instruction_types(f, v))
        goto invalid;
    return XR_LIR_OK;
invalid:
    return xr_lir_internal_error(d, message, function, 0, 0, &v->source);
}

static bool lir_definition(const XrLirFunctionData *f, uint8_t *seen, uint32_t value,
                           XrLirType type, uint32_t block, uint32_t instruction, bool parameter) {
    if (!value || value > f->value_count || seen[value - 1])
        return false;
    const XrLirValueData *v = &f->values[value - 1];
    if (v->type != type || v->block != block || v->instruction != instruction ||
        v->parameter != parameter)
        return false;
    seen[value - 1] = 1;
    return true;
}

static XrLirStatus lir_definitions(const XrLirModule *m, uint32_t index, uint8_t *seen,
                                   XrLirDiagnostic *d) {
    const XrLirFunctionData *f = &m->functions[index];
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (!lir_definition(f, seen, f->parameters[p], f->parameter_types[p], 0, p, true))
            return xr_lir_internal_error(d, "function parameter definition is inconsistent",
                                         index + 1, 0, 0, NULL);
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        if ((block->parameter_count && !block->parameters) || !block->instructions ||
            !block->instruction_count || block->instruction_count > block->instruction_capacity ||
            !block->terminated)
            return xr_lir_internal_error(d, "block has no complete terminator", index + 1, b + 1, 0,
                                         NULL);
        for (uint32_t p = 0; p < block->parameter_count; ++p) {
            uint32_t id = block->parameters[p];
            if (!id || id > f->value_count ||
                !xr_lir_internal_type_valid(f->values[id - 1].type, false) ||
                !lir_definition(f, seen, id, f->values[id - 1].type, b + 1, p, true))
                return xr_lir_internal_error(d, "block parameter definition is inconsistent",
                                             index + 1, b + 1, 0, NULL);
        }
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *v = &block->instructions[i].view;
            if (v->op == XR_LIR_CELL_NEW && b + 1 != f->entry)
                return xr_lir_internal_error(d, "Cell storage requires the activation entry block",
                                             index + 1, b + 1, i + 1, &v->source);
            bool terminal = xr_lir_internal_terminal(v->op);
            if (terminal != (i == block->instruction_count - 1))
                return xr_lir_internal_error(d, "terminator must be the final instruction",
                                             index + 1, b + 1, i + 1, &v->source);
            XrLirStatus status = xr_lir_internal_instruction_check(m, index + 1, v, d);
            if (status != XR_LIR_OK) {
                if (d) {
                    d->status = status;
                    d->function = index + 1;
                    d->block = b + 1;
                    d->instruction = i + 1;
                }
                return status;
            }
            if (terminal || v->type == XR_LIR_UNIT) {
                if (!v->result)
                    continue;
            } else if (lir_definition(f, seen, v->result, v->type, b + 1, i, false))
                continue;
            return xr_lir_internal_error(d, "instruction result definition is inconsistent",
                                         index + 1, b + 1, i + 1, &v->source);
        }
    }
    for (uint32_t v = 0; v < f->value_count; ++v)
        if (!seen[v])
            return xr_lir_internal_error(d, "value has no definition", index + 1, 0, 0, NULL);
    return XR_LIR_OK;
}

typedef struct LirGraph {
    uint32_t count, entry;
    uint32_t *memory, *offsets, *predecessors, *order, *rank, *idom, *stack, *next, *cursor;
} LirGraph;

static const XrLirInstructionView *lir_terminator(const XrLirFunctionData *f, uint32_t block) {
    const XrLirBlockData *b = &f->blocks[block];
    XR_DCHECK(b->instruction_count > 0, "validated block has a terminator");
    return &b->instructions[b->instruction_count - 1].view;
}

static XrLirStatus lir_graph_create(const XrLirFunctionData *f, LirGraph *g) {
    uint64_t edge_count = 0;
    for (uint32_t b = 0; b < f->block_count; ++b)
        edge_count += lir_terminator(f, b)->edge_count;
    uint64_t words = (uint64_t) f->block_count * 7 + edge_count + 1;
    if (edge_count > UINT32_MAX || words > SIZE_MAX / sizeof(uint32_t))
        return XR_LIR_OUT_OF_MEMORY;
    g->memory = xr_malloc((size_t) words * sizeof(*g->memory));
    if (!g->memory)
        return XR_LIR_OUT_OF_MEMORY;
    memset(g->memory, 0, (size_t) words * sizeof(*g->memory));
    g->count = f->block_count;
    g->entry = f->entry - 1;
    g->offsets = g->memory;
    g->predecessors = g->offsets + g->count + 1;
    g->order = g->predecessors + (size_t) edge_count;
    g->rank = g->order + g->count;
    g->idom = g->rank + g->count;
    g->stack = g->idom + g->count;
    g->next = g->stack + g->count;
    g->cursor = g->next + g->count;
    for (uint32_t b = 0; b < g->count; ++b) {
        g->rank[b] = UINT32_MAX;
        g->idom[b] = UINT32_MAX;
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t e = 0; e < term->edge_count; ++e) {
            uint32_t target = term->edges[e].target - 1;
            if (target == g->entry)
                return XR_LIR_VERIFY_ERROR;
            ++g->offsets[target + 1];
        }
    }
    for (uint32_t b = 0; b < g->count; ++b) {
        g->offsets[b + 1] += g->offsets[b];
        g->cursor[b] = g->offsets[b];
    }
    for (uint32_t b = 0; b < g->count; ++b) {
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t e = 0; e < term->edge_count; ++e) {
            uint32_t target = term->edges[e].target - 1;
            g->predecessors[g->cursor[target]++] = b;
        }
    }
    return XR_LIR_OK;
}

static bool lir_graph_order(const XrLirFunctionData *f, LirGraph *g) {
    uint32_t depth = 1, count = 0;
    g->stack[0] = g->entry;
    g->rank[g->entry] = 0;
    while (depth) {
        uint32_t b = g->stack[depth - 1];
        const XrLirInstructionView *term = lir_terminator(f, b);
        if (g->next[b] < term->edge_count) {
            uint32_t target = term->edges[g->next[b]++].target - 1;
            if (g->rank[target] == UINT32_MAX) {
                g->rank[target] = 0;
                g->stack[depth++] = target;
            }
        } else {
            g->order[count++] = b;
            --depth;
        }
    }
    if (count != g->count)
        return false;
    for (uint32_t i = 0; i < count / 2; ++i) {
        uint32_t saved = g->order[i];
        g->order[i] = g->order[count - i - 1];
        g->order[count - i - 1] = saved;
    }
    for (uint32_t i = 0; i < count; ++i)
        g->rank[g->order[i]] = i;
    return true;
}

static uint32_t lir_intersect(const LirGraph *g, uint32_t a, uint32_t b) {
    while (a != b) {
        while (g->rank[a] > g->rank[b])
            a = g->idom[a];
        while (g->rank[b] > g->rank[a])
            b = g->idom[b];
    }
    return a;
}

static void lir_graph_dominators(LirGraph *g) {
    g->idom[g->entry] = g->entry;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 1; i < g->count; ++i) {
            uint32_t b = g->order[i], parent = UINT32_MAX;
            for (uint32_t p = g->offsets[b]; p < g->offsets[b + 1]; ++p) {
                uint32_t predecessor = g->predecessors[p];
                if (g->idom[predecessor] == UINT32_MAX)
                    continue;
                parent = parent == UINT32_MAX ? predecessor : lir_intersect(g, parent, predecessor);
            }
            if (parent != g->idom[b]) {
                g->idom[b] = parent;
                changed = true;
            }
        }
    }
}

static bool lir_dominates_use(const XrLirFunctionData *f, const LirGraph *g, uint32_t id,
                              uint32_t block, uint32_t instruction) {
    const XrLirValueData *value = &f->values[id - 1];
    if (!value->block)
        return value->parameter;
    uint32_t definition = value->block - 1;
    if (definition == block)
        return value->parameter || value->instruction < instruction;
    while (g->rank[block] > g->rank[definition])
        block = g->idom[block];
    return block == definition;
}

static XrLirStatus lir_uses(const XrLirFunctionData *f, uint32_t function, const LirGraph *g,
                            XrLirDiagnostic *d) {
    for (uint32_t b = 0; b < f->block_count; ++b)
        for (uint32_t i = 0; i < f->blocks[b].instruction_count; ++i) {
            const XrLirInstructionView *v = &f->blocks[b].instructions[i].view;
            for (uint32_t a = 0; a < v->operand_count; ++a)
                if (!lir_dominates_use(f, g, v->operands[a], b, i))
                    return xr_lir_internal_error(d, "value definition does not dominate its use",
                                                 function, b + 1, i + 1, &v->source);
            for (uint32_t e = 0; e < v->edge_count; ++e)
                for (uint32_t a = 0; a < v->edges[e].argument_count; ++a)
                    if (!lir_dominates_use(f, g, v->edges[e].arguments[a], b, i)) {
                        XrLirStatus status = xr_lir_internal_error(
                            d, "edge argument definition does not dominate its use", function,
                            b + 1, i + 1, &v->source);
                        if (d)
                            d->edge = e + 1;
                        return status;
                    }
        }
    return XR_LIR_OK;
}

static bool lir_function_shape(const XrLirFunctionData *f) {
    if (!f->name || !f->name_length || !xr_lir_internal_type_valid(f->result, true) ||
        f->result == XR_LIR_I64_CELL ||
        f->value_count > f->value_capacity || (f->value_count && !f->values) ||
        (f->parameter_count && (!f->parameters || !f->parameter_types)))
        return false;
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (!xr_lir_internal_type_valid(f->parameter_types[p], false))
            return false;
    if (f->kind == XR_LIR_FUNCTION_EXTERNAL)
        return f->extern_kind == XR_LIR_EXTERN_PRINT_I64 && f->parameter_count == 1 &&
               f->parameter_types[0] == XR_LIR_I64 && f->result == XR_LIR_UNIT &&
               f->name_length == sizeof("xrt_print_i64") - 1 &&
               !memcmp(f->name, "xrt_print_i64", sizeof("xrt_print_i64") - 1) && !f->defined &&
               !f->block_count && !f->entry;
    return f->kind == XR_LIR_FUNCTION_DEFINITION && f->extern_kind == XR_LIR_EXTERN_NONE &&
           f->defined && f->entry && f->entry <= f->block_count && f->blocks &&
           f->block_count <= f->block_capacity && !f->blocks[f->entry - 1].parameter_count;
}

XR_FUNC XrLirStatus xr_lir_verify(const XrLirModule *m, XrLirDiagnostic *d) {
    if (d)
        memset(d, 0, sizeof(*d));
    if (!m || m->failure != XR_LIR_OK || m->function_count > m->function_capacity ||
        (m->function_count && !m->functions))
        return lir_status_error(d, XR_LIR_INVALID_ARGUMENT, "invalid LIR module");
    for (uint32_t index = 0; index < m->function_count; ++index) {
        const XrLirFunctionData *f = &m->functions[index];
        if (!lir_function_shape(f))
            return xr_lir_internal_error(d, "function signature or entry is invalid", index + 1, 0,
                                         0, NULL);
        if (f->kind == XR_LIR_FUNCTION_EXTERNAL)
            continue;
        uint8_t *seen = f->value_count ? xr_malloc(f->value_count) : NULL;
        if (f->value_count && !seen)
            return lir_status_error(d, XR_LIR_OUT_OF_MEMORY, "LIR verifier allocation failed");
        if (seen)
            memset(seen, 0, f->value_count);
        XrLirStatus status = lir_definitions(m, index, seen, d);
        xr_free(seen);
        if (status != XR_LIR_OK)
            return status;
        LirGraph graph = {0};
        status = lir_graph_create(f, &graph);
        if (status == XR_LIR_OK && !lir_graph_order(f, &graph))
            status = XR_LIR_VERIFY_ERROR;
        if (status == XR_LIR_VERIFY_ERROR)
            xr_lir_internal_error(d, "entry has an incoming edge or a block is unreachable",
                                  index + 1, 0, 0, NULL);
        if (status == XR_LIR_OK) {
            lir_graph_dominators(&graph);
            status = lir_uses(f, index + 1, &graph, d);
        }
        xr_free(graph.memory);
        if (status != XR_LIR_OK) {
            if (d && d->status == XR_LIR_OK) {
                lir_status_error(d, status, "LIR graph allocation failed");
                d->function = index + 1;
            }
            return status;
        }
    }
    return XR_LIR_OK;
}
