/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir.c - Scalar SSA to register bytecode with parallel edge copies.
 *
 * KEY CONCEPT:
 *   Permanent SSA slots stay below edge scratch and the top call window.
 *   Callee growth therefore cannot overwrite a caller's live SSA values.
 */
#include "xrbc_lir.h"
#include "lir/xlir_verify.h"
#include "base/xmalloc.h"
#include "base/xchecks.h"
#include <stdio.h>

struct XrRbcProgram {
    XrRbcModule module;
    XrtLocationOwner *locations;
    XrRbcNativeI64 print;
    XrLirType parameters[XR_RBC_REGISTER_COUNT_MAX];
    uint32_t parameter_count;
};

typedef struct { size_t pc; uint32_t block; } RbcLirPatch;
typedef struct {
    const XrLirModule *input;
    XrLirFunction function;
    XrLirFunctionView view;
    const uint32_t *functions;
    uint32_t slots[XR_RBC_REGISTER_COUNT_MAX + 1];
    uint32_t slot_count, scratch_count, register_count, source_id;
    uint32_t *code;
    uint64_t *constants;
    size_t code_count, code_capacity, constant_count, constant_capacity;
    size_t *blocks;
    RbcLirPatch *patches;
    size_t patch_count, patch_capacity;
    XrRbcCompileDiagnostic *diagnostic;
    XrRbcCompileStatus status;
} RbcLirCompiler;

static XrRbcCompileStatus rbc_lir_error(XrRbcCompileDiagnostic *d,
    XrRbcCompileStatus status, const char *message, XrLirSource source) {
    d->status = status;
    d->line = source.line;
    d->column = source.column;
    size_t length = source.path_length;
    if (length >= sizeof(d->path)) length = sizeof(d->path) - 1;
    if (length && source.path) memcpy(d->path, source.path, length);
    d->path[length] = 0;
    snprintf(d->message, sizeof(d->message), "%s", message);
    return status;
}

static bool rbc_lir_fail(RbcLirCompiler *c, XrRbcCompileStatus status, const char *message) {
    if (c->status == XR_RBC_COMPILE_OK) {
        c->status = status;
        c->diagnostic->status = status;
        snprintf(c->diagnostic->message, sizeof(c->diagnostic->message), "%s", message);
    }
    return false;
}

static bool rbc_lir_grow(RbcLirCompiler *c, void **data, size_t *capacity,
    size_t count, size_t size) {
    if (count <= *capacity) return true;
    if (count > SIZE_MAX / size)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "bytecode storage exceeds address space");
    size_t next = *capacity ? *capacity : 16;
    while (next < count && next <= SIZE_MAX / 2) next *= 2;
    if (next < count || next > SIZE_MAX / size) next = count;
    void *grown = xr_realloc(*data, next * size);
    if (!grown)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate bytecode storage");
    *data = grown;
    *capacity = next;
    return true;
}

static bool rbc_lir_emit(RbcLirCompiler *c, XrRbcOp op, uint32_t a,
    uint32_t b, uint32_t third, int64_t immediate) {
    uint32_t word;
    if (!xr_rbc_encode(&word, (XrRbcOperands) {op, a, b, third, immediate}))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "operand exceeds bytecode encoding");
    void *storage = c->code;
    if (c->code_count == SIZE_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "instruction count exceeds address space");
    if (!rbc_lir_grow(c, &storage, &c->code_capacity,
            c->code_count + 1, sizeof(*c->code))) return false;
    c->code = storage;
    c->code[c->code_count++] = word;
    return true;
}

static bool rbc_lir_jump(RbcLirCompiler *c, uint32_t target) {
    void *storage = c->patches;
    if (c->patch_count == SIZE_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "branch count exceeds address space");
    if (!rbc_lir_grow(c, &storage, &c->patch_capacity,
            c->patch_count + 1, sizeof(*c->patches))) return false;
    c->patches = storage;
    c->patches[c->patch_count++] = (RbcLirPatch) {c->code_count, target};
    return rbc_lir_emit(c, XR_RBC_JMP, 0, 0, 0, 0);
}

static bool rbc_lir_scalar(XrLirType type) {
    // Slot transport is bit-preserving; operation admission remains separate.
    return type == XR_LIR_I64 || type == XR_LIR_I1 || type == XR_LIR_F64;
}

static XrLirSource rbc_lir_value_source(const XrLirModule *input, XrLirFunction f,
    const XrLirFunctionView *view, uint32_t value, XrRbcCompileDiagnostic *d) {
    XrLirSource fallback = {0};
    uint32_t fallback_block = 0, fallback_instruction = 0;
    for (uint32_t b = 0; b < view->block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView bv;
        xr_lir_block_at(input, f, b, &block);
        xr_lir_block_view(input, block, &bv);
        for (uint32_t n = 0; n < bv.instruction_count; ++n) {
            XrLirInstructionView i;
            xr_lir_instruction_view(input, block, n, &i);
            if (!i.source.path_length || !i.source.line || !i.source.column) continue;
            if (!fallback.path_length) {
                fallback = i.source;
                fallback_block = block.id;
                fallback_instruction = n + 1;
            }
            bool selected = !value || i.result == value;
            for (uint32_t a = 0; a < i.operand_count && !selected; ++a)
                selected = i.operands[a] == value;
            if (selected) {
                d->block = block.id;
                d->instruction = n + 1;
                return i.source;
            }
        }
    }
    d->block = fallback_block;
    d->instruction = fallback_instruction;
    return fallback;
}

static bool rbc_lir_admit(const XrLirModule *input, XrLirFunction f,
    const XrLirFunctionView *v, XrRbcCompileDiagnostic *d) {
    d->function = f.id;
    d->block = d->instruction = 0;
    if (v->kind != XR_LIR_FUNCTION_DEFINITION ||
        (v->result != XR_LIR_UNIT && !rbc_lir_scalar(v->result))) {
        rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
            "execution requires a scalar function definition",
            rbc_lir_value_source(input, f, v, 0, d));
        return false;
    }
    if (v->value_count > XR_RBC_REGISTER_COUNT_MAX) {
        rbc_lir_error(d, XR_RBC_COMPILE_LIMIT, "SSA values exceed register capacity",
            rbc_lir_value_source(input, f, v, 0, d));
        return false;
    }
    for (uint32_t value = 1; value <= v->value_count; ++value) {
        XrLirType type;
        if (xr_lir_value_type(input, (XrLirValue) {input, f.id, value}, &type) != XR_LIR_OK ||
            !rbc_lir_scalar(type)) {
            rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
                "value type is not an admitted scalar",
                rbc_lir_value_source(input, f, v, value, d));
            return false;
        }
    }
    return true;
}

static bool rbc_lir_operation(const XrLirModule *input, XrLirFunction function,
    const XrLirInstructionView *i) {
    switch (i->op) {
    case XR_LIR_CONST: case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL:
    case XR_LIR_NEG: case XR_LIR_DIV: case XR_LIR_REM: case XR_LIR_ICMP:
    case XR_LIR_CALL: case XR_LIR_BR: case XR_LIR_CONDBR: case XR_LIR_RET:
        return true;
    case XR_LIR_INTCAST: {
        XrLirType source = XR_LIR_TYPE_INVALID;
        return i->type == XR_LIR_I64 && i->operand_count == 1 &&
            xr_lir_value_type(input, (XrLirValue){input, function.id, i->operands[0]},
                &source) == XR_LIR_OK && source == XR_LIR_I64;
    }
    default: return false;
    }
}

static bool rbc_lir_closure(const XrLirModule *input, uint32_t *ids, uint32_t *mapping,
    uint32_t *count, XrRbcCompileDiagnostic *d) {
    for (uint32_t index = 0; index < *count; ++index) {
        XrLirFunction f = {input, ids[index]};
        XrLirFunctionView v;
        if (xr_lir_function_view(input, f, &v) != XR_LIR_OK ||
            !rbc_lir_admit(input, f, &v, d)) return false;
        for (uint32_t b = 0; b < v.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView bv;
            xr_lir_block_at(input, f, b, &block);
            xr_lir_block_view(input, block, &bv);
            for (uint32_t n = 0; n < bv.instruction_count; ++n) {
                XrLirInstructionView i;
                xr_lir_instruction_view(input, block, n, &i);
                d->block = block.id;
                d->instruction = n + 1;
                if (!rbc_lir_operation(input, f, &i)) {
                    rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
                        "operation has no admitted bytecode implementation", i.source);
                    return false;
                }
                if (i.op == XR_LIR_CALL && !mapping[i.callee - 1]) {
                    XrLirFunctionView callee;
                    xr_lir_function_view(input, (XrLirFunction) {input, i.callee}, &callee);
                    if (callee.kind != XR_LIR_FUNCTION_DEFINITION) {
                        if (callee.kind != XR_LIR_FUNCTION_EXTERNAL ||
                            callee.extern_kind != XR_LIR_EXTERN_PRINT_I64) {
                            rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
                                "native call has no registered runtime leaf", i.source);
                            return false;
                        }
                        mapping[i.callee - 1] = UINT32_MAX;
                        continue;
                    }
                    if (*count == 65536) {
                        rbc_lir_error(d, XR_RBC_COMPILE_LIMIT,
                            "call closure exceeds function encoding", i.source);
                        return false;
                    }
                    mapping[i.callee - 1] = *count + 1;
                    ids[(*count)++] = i.callee;
                }
            }
        }
    }
    return true;
}

static bool rbc_lir_layout(RbcLirCompiler *c) {
    for (size_t i = 0; i < sizeof(c->slots) / sizeof(c->slots[0]); ++i)
        c->slots[i] = UINT32_MAX;
    for (uint32_t i = 0; i < c->view.parameter_count; ++i)
        c->slots[c->view.parameters[i]] = c->slot_count++;
    uint32_t window = 0;
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView bv;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &bv);
        for (uint32_t n = 0; n < bv.parameter_count; ++n)
            c->slots[bv.parameters[n]] = c->slot_count++;
        for (uint32_t n = 0; n < bv.instruction_count; ++n) {
            XrLirInstructionView i;
            xr_lir_instruction_view(c->input, block, n, &i);
            if (i.result) c->slots[i.result] = c->slot_count++;
            if (i.op == XR_LIR_CALL) {
                uint32_t physical = c->functions[i.callee - 1] == UINT32_MAX ? 2 : i.operand_count;
                if (window < physical + 1) window = physical + 1;
            }
            if (i.op == XR_LIR_CONDBR && !c->scratch_count) c->scratch_count = 1;
            for (uint32_t e = 0; e < i.edge_count; ++e)
                if (c->scratch_count < i.edges[e].argument_count)
                    c->scratch_count = i.edges[e].argument_count;
        }
    }
    uint64_t registers = (uint64_t) c->slot_count + c->scratch_count + window;
    if (registers > XR_RBC_REGISTER_COUNT_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT,
            "SSA values, edge scratch and call window exceed register capacity");
    c->register_count = registers ? (uint32_t) registers : 1;
    return true;
}

static bool rbc_lir_constant(RbcLirCompiler *c, uint32_t slot, uint64_t bits) {
    // Small signed constants use the immediate form without a pool entry.
    if (bits <= INT16_MAX || bits >= UINT64_MAX - 32767)
        return rbc_lir_emit(c, XR_RBC_LOADI, slot, 0, 0,
            bits <= INT16_MAX ? (int64_t) bits : -1 - (int64_t) ~bits);
    if (c->constant_count > UINT16_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "constant pool exceeds encoding");
    void *storage = c->constants;
    if (!rbc_lir_grow(c, &storage, &c->constant_capacity,
            c->constant_count + 1, sizeof(*c->constants))) return false;
    c->constants = storage;
    uint32_t index = (uint32_t) c->constant_count;
    c->constants[c->constant_count++] = bits;
    return rbc_lir_emit(c, XR_RBC_LOADK, slot, 0, 0, index);
}

static bool rbc_lir_comparison(RbcLirCompiler *c, const XrLirInstructionView *i) {
    uint32_t a = c->slots[i->operands[0]], b = c->slots[i->operands[1]];
    bool inverse = i->predicate == XR_LIR_NE;
    XrRbcOp op = XR_RBC_EQ;
    if (i->predicate == XR_LIR_GT || i->predicate == XR_LIR_GE) {
        uint32_t swap = a; a = b; b = swap;
    }
    if (i->predicate == XR_LIR_LT || i->predicate == XR_LIR_GT) op = XR_RBC_LT;
    if (i->predicate == XR_LIR_LE || i->predicate == XR_LIR_GE) op = XR_RBC_LE;
    uint32_t result = c->slots[i->result];
    return rbc_lir_emit(c, XR_RBC_LOADI, result, 0, 0, inverse ? 1 : 0) &&
        rbc_lir_emit(c, op, a, b, 0, 0) &&
        rbc_lir_emit(c, XR_RBC_JMP, 0, 0, 0, 1) &&
        rbc_lir_emit(c, XR_RBC_JMP, 0, 0, 0, 1) &&
        rbc_lir_emit(c, XR_RBC_LOADI, result, 0, 0, inverse ? 0 : 1);
}

static bool rbc_lir_edge(RbcLirCompiler *c, XrLirEdgeView edge) {
    XrLirBlockView target;
    xr_lir_block_view(c->input,
        (XrLirBlock) {c->input, c->function.id, edge.target}, &target);
    // Snapshot every source before writing any destination: loops can swap
    // block arguments or form longer cycles, including repeated sources.
    for (uint32_t i = 0; i < edge.argument_count; ++i)
        if (!rbc_lir_emit(c, XR_RBC_MOV, c->slot_count + i,
                c->slots[edge.arguments[i]], 0, 0)) return false;
    for (uint32_t i = 0; i < edge.argument_count; ++i)
        if (!rbc_lir_emit(c, XR_RBC_MOV, c->slots[target.parameters[i]],
                c->slot_count + i, 0, 0)) return false;
    return rbc_lir_jump(c, edge.target);
}

static bool rbc_lir_conditional(RbcLirCompiler *c, const XrLirInstructionView *i) {
    if (!rbc_lir_emit(c, XR_RBC_LOADI, c->slot_count, 0, 0, 0) ||
        !rbc_lir_emit(c, XR_RBC_EQ, c->slots[i->operands[0]], c->slot_count, 0, 0))
        return false;
    size_t failure = c->code_count;
    if (!rbc_lir_emit(c, XR_RBC_JMP, 0, 0, 0, 0) ||
        !rbc_lir_edge(c, i->edges[0])) return false;
    size_t false_start = c->code_count;
    if (!rbc_lir_edge(c, i->edges[1])) return false;
    if (!xr_rbc_jump(&c->code[failure], failure, false_start, c->code_count))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "conditional edge exceeds jump encoding");
    return true;
}

static bool rbc_lir_instruction(RbcLirCompiler *c, const XrLirInstructionView *i) {
    uint32_t result = i->result ? c->slots[i->result] : 0;
    switch (i->op) {
    case XR_LIR_CONST: return rbc_lir_constant(c, result, i->bits);
    case XR_LIR_INTCAST:
        return rbc_lir_emit(c, XR_RBC_MOV, result, c->slots[i->operands[0]], 0, 0);
    case XR_LIR_ICMP: return rbc_lir_comparison(c, i);
    case XR_LIR_BR: return rbc_lir_edge(c, i->edges[0]);
    case XR_LIR_CONDBR: return rbc_lir_conditional(c, i);
    case XR_LIR_RET:
        return rbc_lir_emit(c, i->operand_count ? XR_RBC_RET : XR_RBC_RET0,
            i->operand_count ? c->slots[i->operands[0]] : 0, 0, 0, 0);
    case XR_LIR_CALL: {
        if (c->functions[i->callee - 1] == UINT32_MAX) {
            uint32_t destination = c->register_count - 3;
            return rbc_lir_emit(c, XR_RBC_MOV, destination + 1,
                    c->slots[i->operands[0]], 0, 0) &&
                rbc_lir_constant(c, destination + 2, i->source.path_length ? c->source_id : 0) &&
                rbc_lir_emit(c, XR_RBC_CALLN, destination, 0, 0, 0);
        }
        uint32_t destination = c->register_count - i->operand_count - 1;
        for (uint32_t n = 0; n < i->operand_count; ++n)
            if (!rbc_lir_emit(c, XR_RBC_MOV, destination + n + 1,
                    c->slots[i->operands[n]], 0, 0)) return false;
        if (!rbc_lir_emit(c, XR_RBC_CALL, destination, 0, 0,
                c->functions[i->callee - 1] - 1)) return false;
        return !i->result || rbc_lir_emit(c, XR_RBC_MOV, result, destination, 0, 0);
    }
    default: {
        XrRbcOp op;
        switch (i->op) {
        case XR_LIR_ADD: op = XR_RBC_ADD; break;
        case XR_LIR_SUB: op = XR_RBC_SUB; break;
        case XR_LIR_MUL: op = XR_RBC_MUL; break;
        case XR_LIR_DIV: op = XR_RBC_DIV; break;
        case XR_LIR_REM: op = XR_RBC_REM; break;
        case XR_LIR_NEG: op = XR_RBC_NEG; break;
        default: return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "unsupported operation");
        }
        return rbc_lir_emit(c, op, result, c->slots[i->operands[0]],
            i->operand_count == 2 ? c->slots[i->operands[1]] : 0, 0);
    }
    }
}

static bool rbc_lir_function(RbcLirCompiler *c, XrRbcFunction *out) {
    if (!rbc_lir_layout(c)) return false;
    size_t count = (size_t) c->view.block_count + 1;
    if (count > SIZE_MAX / sizeof(*c->blocks))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "block table exceeds address space");
    c->blocks = xr_malloc(count * sizeof(*c->blocks));
    if (!c->blocks)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate block offsets");
    if (!rbc_lir_jump(c, c->view.entry)) return false;
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView bv;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &bv);
        c->blocks[block.id] = c->code_count;
        for (uint32_t n = 0; n < bv.instruction_count; ++n) {
            XrLirInstructionView i;
            xr_lir_instruction_view(c->input, block, n, &i);
            if (i.source.path_length) ++c->source_id;
            c->diagnostic->block = block.id;
            c->diagnostic->instruction = n + 1;
            rbc_lir_error(c->diagnostic, XR_RBC_COMPILE_OK, "", i.source);
            if (!rbc_lir_instruction(c, &i)) return false;
        }
    }
    for (size_t i = 0; i < c->patch_count; ++i) {
        RbcLirPatch patch = c->patches[i];
        if (!xr_rbc_jump(&c->code[patch.pc], patch.pc, c->blocks[patch.block], c->code_count))
            return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "branch exceeds jump encoding");
    }
    *out = (XrRbcFunction) {c->code, c->code_count, c->constants, c->constant_count,
        c->register_count, c->view.parameter_count};
    return true;
}

XR_FUNC void xr_rbc_program_destroy(XrRbcProgram *program) {
    if (!program) return;
    for (size_t f = 0; f < program->module.function_count; ++f) {
        xr_free((void *) program->module.functions[f].code);
        xr_free((void *) program->module.functions[f].constants);
    }
    xr_free((void *) program->module.functions);
    xrt_locations_release(program->locations);
    xr_free(program);
}

// Use the same retained function/block/instruction order as code emission.
// Unknown source does not occupy an ID; equal locations remain distinct sites.
static XrRbcCompileStatus rbc_lir_locations(const XrLirModule *input, const uint32_t *ids,
    uint32_t count, XrRbcProgram *p, XrRbcCompileDiagnostic *d) {
    XrtSourceLocation *locations = NULL;
    size_t capacity = 0, used = 0;
    XrRbcCompileStatus status = XR_RBC_COMPILE_OK;
    for (uint32_t f = 0; f < count; ++f) {
        XrLirFunction function = {input, ids[f]};
        XrLirFunctionView view;
        xr_lir_function_view(input, function, &view);
        d->function = ids[f];
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView bv;
            xr_lir_block_at(input, function, b, &block);
            xr_lir_block_view(input, block, &bv);
            for (uint32_t n = 0; n < bv.instruction_count; ++n) {
                XrLirInstructionView instruction;
                xr_lir_instruction_view(input, block, n, &instruction);
                XrLirSource source = instruction.source;
                if (!source.path_length) continue;
                d->block = block.id;
                d->instruction = n + 1;
                if (used == UINT32_MAX || used >= SIZE_MAX / sizeof(*locations)) {
                    status = rbc_lir_error(d, XR_RBC_COMPILE_LIMIT,
                        "source location table exceeds address space", source);
                    goto done;
                }
                if (used == capacity) {
                    size_t maximum = SIZE_MAX / sizeof(*locations);
                    size_t next = capacity ? (capacity > maximum / 2 ? maximum : capacity * 2) : 8;
                    if (next > maximum) next = maximum;
                    XrtSourceLocation *grown = xr_realloc(locations, next * sizeof(*locations));
                    if (!grown) {
                        status = rbc_lir_error(d, XR_RBC_COMPILE_OUT_OF_MEMORY,
                            "cannot collect source locations", source);
                        goto done;
                    }
                    locations = grown;
                    capacity = next;
                }
                locations[used++] = (XrtSourceLocation) {source.path, source.path_length,
                    source.line, source.column, source.end_line, source.end_column};
            }
        }
    }
    XrtLocationStatus created = xrt_locations_create(locations, used, &p->locations);
    if (created != XRT_LOCATION_OK) {
        status = created == XRT_LOCATION_OUT_OF_MEMORY ? XR_RBC_COMPILE_OUT_OF_MEMORY :
            created == XRT_LOCATION_LIMIT ? XR_RBC_COMPILE_LIMIT : XR_RBC_COMPILE_INVALID;
        rbc_lir_error(d, status, "cannot own source locations", (XrLirSource) {0});
    }
done:
    xr_free(locations);
    return status;
}

static XrRbcCompileStatus rbc_lir_build(const XrLirModule *input, const uint32_t *ids,
    const uint32_t *mapping, XrRbcProgram *p, XrRbcCompileDiagnostic *d) {
    uint32_t source_id = 0;
    for (size_t f = 0; f < p->module.function_count; ++f) {
        RbcLirCompiler c = {.input = input, .function = {input, ids[f]},
            .functions = mapping, .diagnostic = d, .source_id = source_id};
        xr_lir_function_view(input, c.function, &c.view);
        d->function = ids[f];
        bool success = rbc_lir_function(&c, &((XrRbcFunction *) p->module.functions)[f]);
        source_id = c.source_id;
        xr_free(c.blocks);
        xr_free(c.patches);
        if (!success) {
            xr_free(c.code);
            xr_free(c.constants);
            return c.status;
        }
        if (!f) {
            p->parameter_count = c.view.parameter_count;
            if (p->parameter_count)
                memcpy(p->parameters, c.view.parameter_types,
                    p->parameter_count * sizeof(*p->parameters));
        }
    }
    return XR_RBC_COMPILE_OK;
}

XR_FUNC XrRbcCompileStatus xr_rbc_compile(const XrLirModule *input, XrLirFunction entry,
    XrRbcProgram **output, XrRbcCompileDiagnostic *diagnostic) {
    XrRbcCompileDiagnostic local = {0};
    XrRbcCompileDiagnostic *d = diagnostic ? diagnostic : &local;
    *d = (XrRbcCompileDiagnostic) {0};
    XrLirFunctionView root;
    if (!output || *output || entry.module != input ||
        xr_lir_function_view(input, entry, &root) != XR_LIR_OK)
        return rbc_lir_error(d, XR_RBC_COMPILE_INVALID, "invalid module, root or output",
            (XrLirSource) {0});
    XrLirDiagnostic invalid;
    XrLirStatus verified = xr_lir_verify(input, &invalid);
    if (verified != XR_LIR_OK) {
        d->function = invalid.function;
        d->block = invalid.block;
        d->instruction = invalid.instruction;
        return rbc_lir_error(d, verified == XR_LIR_OUT_OF_MEMORY ?
            XR_RBC_COMPILE_OUT_OF_MEMORY : XR_RBC_COMPILE_INVALID, invalid.message, invalid.source);
    }
    size_t all = xr_lir_function_count(input);
    if (all > SIZE_MAX / sizeof(uint32_t))
        return rbc_lir_error(d, XR_RBC_COMPILE_LIMIT, "function table exceeds address space",
            (XrLirSource) {0});
    uint32_t *ids = xr_malloc(all * sizeof(*ids));
    uint32_t *mapping = xr_calloc(all, sizeof(*mapping));
    XrRbcProgram *p = xr_calloc(1, sizeof(*p));
    XrRbcCompileStatus status = XR_RBC_COMPILE_OUT_OF_MEMORY;
    if (!ids || !mapping || !p) {
        rbc_lir_error(d, status, "cannot allocate compilation state", (XrLirSource) {0});
        goto done;
    }
    uint32_t count = 1;
    ids[0] = entry.id;
    mapping[entry.id - 1] = 1;
    if (!rbc_lir_closure(input, ids, mapping, &count, d)) {
        status = d->status;
        goto done;
    }
    status = rbc_lir_locations(input, ids, count, p, d);
    if (status != XR_RBC_COMPILE_OK) goto done;
    for (size_t i = 0; i < all; ++i) {
        if (mapping[i] != UINT32_MAX) continue;
        p->print = xrt_print_i64;
        p->module.natives = &p->print;
        p->module.native_count = 1;
        break;
    }
    status = XR_RBC_COMPILE_OUT_OF_MEMORY;
    XrRbcFunction *functions = xr_calloc(count, sizeof(*functions));
    if (!functions) {
        rbc_lir_error(d, status, "cannot allocate executable functions", (XrLirSource) {0});
        goto done;
    }
    p->module.functions = functions;
    p->module.function_count = count;
    status = rbc_lir_build(input, ids, mapping, p, d);
    if (status == XR_RBC_COMPILE_OK) {
        *output = p;
        p = NULL;
        *d = (XrRbcCompileDiagnostic) {0};
    }
done:
    xr_rbc_program_destroy(p);
    xr_free(mapping);
    xr_free(ids);
    return status;
}

XR_FUNC XrRbcStatus xr_rbc_program_prepare(const XrRbcProgram *p, const uint64_t *arguments,
    size_t count, XrRbcExecution **output) {
    if (!output || *output || !p || count != p->parameter_count || (count && !arguments))
        return XR_RBC_INVALID;
    for (size_t i = 0; i < count; ++i)
        if (p->parameters[i] == XR_LIR_I1 && arguments[i] > 1) return XR_RBC_INVALID;
    return xr_rbc_module_prepare(&p->module, 0, arguments, count, p->locations, output);
}

XR_FUNC const XrRbcModule *xr_rbc_program_view(const XrRbcProgram *program) {
    return program ? &program->module : NULL;
}

XR_FUNC XrtLocationOwner *xr_rbc_program_locations(const XrRbcProgram *program) {
    return program ? program->locations : NULL;
}
