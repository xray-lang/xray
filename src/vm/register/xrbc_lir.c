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
#include "xrbc_internal.h"
#include "lir/xlir_verify.h"
#include "base/xmalloc.h"
#include "base/xchecks.h"
#include <stdio.h>

struct XrRbcProgram {
    XrRbcModule module;
    XrtLocationOwner *locations;
    XrRbcNativeKind natives[XR_RBC_NATIVE_COUNT];
    XrLirType parameters[XR_RBC_REGISTER_COUNT_MAX];
    uint32_t parameter_count;
};

#define RBC_LIR_PRINT_STRING (UINT32_MAX - 1)
#define RBC_LIR_NATIVE_BASE (UINT32_MAX - 8)

static bool rbc_lir_native(uint32_t mapping) {
    return mapping >= RBC_LIR_NATIVE_BASE && mapping < RBC_LIR_NATIVE_BASE + XR_RBC_NATIVE_COUNT;
}

typedef struct {
    uint32_t *literals;
    bool *lengths;
} RbcLirOutputValues;

typedef struct {
    XrRbcProgram *program;
    uint32_t *mapping;
    size_t capacity;
} RbcLirLiteralPool;

typedef struct { size_t pc; uint32_t block; } RbcLirPatch;
typedef struct {
    const XrLirModule *input;
    XrLirFunction function;
    XrLirFunctionView view;
    const uint32_t *functions;
    RbcLirOutputValues output;
    RbcLirLiteralPool *literals;
    uint32_t *slots, *spill_home;
    uint32_t spill_count, reference_spill_count, temporary_strings, temporary_bits;
    bool typed_spills;
    uint32_t slot_count, scratch_count, register_count, source_id, current_source_id, cell_count;
    uint32_t *code, *source_ids;
    uint64_t *constants;
    XrRbcRegisterKind *register_kinds, *cell_kinds;
    size_t code_count, code_capacity, source_id_capacity, constant_count, constant_capacity;
    size_t *blocks;
    RbcLirPatch *patches;
    size_t patch_count, patch_capacity;
    XrRbcCompileDiagnostic *diagnostic;
    XrRbcCompileStatus status;
    bool string_scratch, control_scratch;
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
    if (op == XR_RBC_MOV && a == b) return true;
    void *storage = c->code;
    if (c->code_count == SIZE_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "instruction count exceeds address space");
    if (!rbc_lir_grow(c, &storage, &c->code_capacity,
            c->code_count + 1, sizeof(*c->code))) return false;
    c->code = storage;
    storage = c->source_ids;
    if (!rbc_lir_grow(c, &storage, &c->source_id_capacity,
            c->code_count + 1, sizeof(*c->source_ids))) return false;
    c->source_ids = storage;
    c->code[c->code_count] = word;
    c->source_ids[c->code_count++] = c->current_source_id;
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

static bool rbc_lir_string_call(const XrLirModule *input, const XrLirInstructionView *i) {
    if (i->op != XR_LIR_CALL) return false;
    XrLirFunctionView callee;
    return xr_lir_function_view(input, (XrLirFunction) {input, i->callee}, &callee) == XR_LIR_OK &&
        callee.kind == XR_LIR_FUNCTION_EXTERNAL && callee.extern_kind == XR_LIR_EXTERN_PRINT_STRING;
}

static void rbc_lir_output_free(RbcLirOutputValues *output) {
    xr_free(output->literals); xr_free(output->lengths);
    *output=(RbcLirOutputValues){0};
}
static bool rbc_lir_output_values(const XrLirModule *input, XrLirFunction f,
    const XrLirFunctionView *view, RbcLirOutputValues *output, XrRbcCompileDiagnostic *d) {
    *output = (RbcLirOutputValues) {0};
    size_t count=(size_t)view->value_count+1;
    if (!count || count>SIZE_MAX/sizeof(*output->literals)) {
        rbc_lir_error(d,XR_RBC_COMPILE_LIMIT,"SSA bookkeeping exceeds address space",(XrLirSource){0});
        return false;
    }
    output->literals=xr_calloc(count,sizeof(*output->literals));
    output->lengths=xr_calloc(count,sizeof(*output->lengths));
    if (!output->literals || !output->lengths) {
        rbc_lir_output_free(output);
        rbc_lir_error(d,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot allocate SSA output bookkeeping",(XrLirSource){0});
        return false;
    }
    for (uint32_t b = 0; b < view->block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView bv;
        xr_lir_block_at(input, f, b, &block);
        xr_lir_block_view(input, block, &bv);
        for (uint32_t n = 0; n < bv.instruction_count; ++n) {
            XrLirInstructionView i;
            xr_lir_instruction_view(input, block, n, &i);
            if (i.op == XR_LIR_LITERAL_ADDR) output->literals[i.result] = (uint32_t)i.bits;
            // The LIR verifier owns the pair's provenance, exact length and
            // unique direct address use. These IDs only select no-code values.
            if (rbc_lir_string_call(input, &i)) output->lengths[i.operands[1]] = true;
        }
    }
    return true;
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
    const XrLirFunctionView *v, XrRbcCompileDiagnostic *d, RbcLirOutputValues *output) {
    d->function = f.id;
    d->block = d->instruction = 0;
    if(v->value_count==UINT32_MAX) {
        rbc_lir_error(d,XR_RBC_COMPILE_LIMIT,"SSA index space is exhausted",(XrLirSource){0});
        return false;
    }
    if (v->kind != XR_LIR_FUNCTION_DEFINITION ||
        (v->result != XR_LIR_UNIT && v->result != XR_LIR_STRING && !rbc_lir_scalar(v->result))) {
        rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
            "execution requires a supported typed function definition",
            rbc_lir_value_source(input, f, v, 0, d));
        return false;
    }
    if (!rbc_lir_output_values(input, f, v, output,d)) return false;
    for (uint32_t value = 1; value <= v->value_count; ++value) {
        XrLirType type;
        if (xr_lir_value_type(input, (XrLirValue) {input, f.id, value}, &type) != XR_LIR_OK ||
            (!rbc_lir_scalar(type) && type != XR_LIR_I64_CELL && type != XR_LIR_STRING && type != XR_LIR_STRING_CELL &&
             !(type == XR_LIR_PTR && output->literals[value]) &&
             !(type == XR_LIR_U64 && output->lengths[value]))) {
            rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
                "value type is not an admitted scalar",
                rbc_lir_value_source(input, f, v, value, d));
            return false;
        }
    }
    return true;
}

static bool rbc_lir_operation(const XrLirModule *input, XrLirFunction function,
    const XrLirInstructionView *i, const RbcLirOutputValues *output) {
    if (rbc_lir_string_call(input, i)) return true;
    // A length can also be an operand of otherwise valid unsigned LIR. Its
    // output use must not silently enable signed arithmetic or comparisons.
    for (uint32_t a = 0; a < i->operand_count; ++a)
        if (output->literals[i->operands[a]] || output->lengths[i->operands[a]]) return false;
    for (uint32_t e = 0; e < i->edge_count; ++e)
        for (uint32_t a = 0; a < i->edges[e].argument_count; ++a)
            if (output->literals[i->edges[e].arguments[a]] ||
                output->lengths[i->edges[e].arguments[a]]) return false;
    switch (i->op) {
    case XR_LIR_LITERAL_ADDR:
    case XR_LIR_CONST: case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL:
    case XR_LIR_NEG: case XR_LIR_DIV: case XR_LIR_REM: case XR_LIR_ICMP:
    case XR_LIR_CALL: case XR_LIR_BR: case XR_LIR_CONDBR: case XR_LIR_RET:
    case XR_LIR_CELL_NEW: case XR_LIR_CELL_READ: case XR_LIR_CELL_WRITE:
    case XR_LIR_STRING_LITERAL: case XR_LIR_STRING_I64: case XR_LIR_STRING_CONCAT:
    case XR_LIR_STRING_RETAIN: case XR_LIR_STRING_DROP: case XR_LIR_STRING_CELL_DROP:
    case XR_LIR_STRING_PRINT: case XR_LIR_STRING_LEN:
        return true;
    case XR_LIR_INTCAST: {
        XrLirType source = XR_LIR_TYPE_INVALID;
        return i->type == XR_LIR_I64 && i->operand_count == 1 &&
            xr_lir_value_type(input, (XrLirValue){input, function.id, i->operands[0]},
                &source) == XR_LIR_OK && source == XR_LIR_I64;
    }
    case XR_LIR_FNEG: {
        XrLirType source = XR_LIR_TYPE_INVALID;
        return i->type == XR_LIR_F64 && i->operand_count == 1 &&
            xr_lir_value_type(input, (XrLirValue){input, function.id, i->operands[0]},
                &source) == XR_LIR_OK && source == XR_LIR_F64;
    }
    case XR_LIR_FADD: case XR_LIR_FSUB: case XR_LIR_FMUL: case XR_LIR_FDIV: case XR_LIR_FCMP: {
        XrLirType left=XR_LIR_TYPE_INVALID,right=XR_LIR_TYPE_INVALID;
        return i->operand_count==2 && i->type==(i->op==XR_LIR_FCMP?XR_LIR_I1:XR_LIR_F64) &&
            xr_lir_value_type(input,(XrLirValue){input,function.id,i->operands[0]},&left)==XR_LIR_OK &&
            xr_lir_value_type(input,(XrLirValue){input,function.id,i->operands[1]},&right)==XR_LIR_OK &&
            left==XR_LIR_F64 && right==XR_LIR_F64;
    }
    default: return false;
    }
}

static bool rbc_lir_closure(const XrLirModule *input, uint32_t *ids, uint32_t *mapping,
    uint32_t *count, XrRbcCompileDiagnostic *d) {
    for (uint32_t index = 0; index < *count; ++index) {
        XrLirFunction f = {input, ids[index]};
        XrLirFunctionView v;
        RbcLirOutputValues output={0};
        if (xr_lir_function_view(input, f, &v) != XR_LIR_OK ||
            !rbc_lir_admit(input, f, &v, d, &output)) goto rejected;
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
                if (!rbc_lir_operation(input, f, &i, &output)) {
                    rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
                        "operation has no admitted bytecode implementation", i.source);
                    goto rejected;
                }
                if (i.op == XR_LIR_CALL && !mapping[i.callee - 1]) {
                    XrLirFunctionView callee;
                    xr_lir_function_view(input, (XrLirFunction) {input, i.callee}, &callee);
                    if (callee.kind != XR_LIR_FUNCTION_DEFINITION) {
                        if (callee.kind != XR_LIR_FUNCTION_EXTERNAL ||
                            (callee.extern_kind < XR_LIR_EXTERN_PRINT_I64 ||
                             callee.extern_kind > XR_LIR_EXTERN_UTC_OFFSET_AT)) {
                            rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
                                "native call has no registered runtime leaf", i.source);
                            goto rejected;
                        }
                        mapping[i.callee - 1] = callee.extern_kind == XR_LIR_EXTERN_PRINT_STRING ?
                            RBC_LIR_PRINT_STRING : RBC_LIR_NATIVE_BASE +
                            (callee.extern_kind == XR_LIR_EXTERN_PRINT_I64 ? XR_RBC_NATIVE_PRINT_I64 :
                             XR_RBC_NATIVE_CLOCK_REALTIME + callee.extern_kind - XR_LIR_EXTERN_CLOCK_REALTIME);
                        continue;
                    }
                    if (*count == 65536) {
                        rbc_lir_error(d, XR_RBC_COMPILE_LIMIT,
                            "call closure exceeds function encoding", i.source);
                        goto rejected;
                    }
                    mapping[i.callee - 1] = *count + 1;
                    ids[(*count)++] = i.callee;
                }
            }
        }
        rbc_lir_output_free(&output);
        continue;
rejected:
        rbc_lir_output_free(&output);
        return false;
    }
    return true;
}

/* Parallel copies run with no staged instruction operands. Reuse the first
 * temporary of the matching bank, never a slot from the other typed pool. */
static uint32_t rbc_lir_copy_temporary(RbcLirCompiler *c, bool string) {
    uint32_t offset = string ? 1u : 0u;
    if (c->typed_spills) {
        if ((string && !c->temporary_strings) ||
            (!string && !c->temporary_bits && !c->control_scratch)) {
            rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "parallel copy has no matching typed scratch");
            return UINT32_MAX;
        }
        offset = string ? 0u : c->temporary_strings;
        if (offset >= c->scratch_count) {
            rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "parallel copy exceeds typed scratch budget");
            return UINT32_MAX;
        }
    }
    return c->slot_count + offset;
}

#include "xrbc_lir_registers.inc.c"

static bool rbc_lir_layout(RbcLirCompiler *c) {
    uint32_t window = 0;
    for(uint32_t b=0;b<c->view.block_count;++b) {
        XrLirBlock block;XrLirBlockView view;
        xr_lir_block_at(c->input,c->function,b,&block);xr_lir_block_view(c->input,block,&view);
        for(uint32_t n=0;n<view.instruction_count;++n) {
            XrLirInstructionView i;xr_lir_instruction_view(c->input,block,n,&i);
            uint64_t strings=0,bits=0;
            for(uint32_t a=0;a<i.operand_count;++a) {
                uint32_t v=i.operands[a];XrLirType type=rbc_lir_value_type(c,v);
                if(c->output.literals[v] || c->output.lengths[v] ||
                    type==XR_LIR_STRING_CELL || type==XR_LIR_I64_CELL)continue;
                if(type==XR_LIR_STRING)++strings;else ++bits;
            }
            if(i.result && !c->output.literals[i.result] && !c->output.lengths[i.result]) {
                if(i.type==XR_LIR_STRING)++strings;
                else if(i.type!=XR_LIR_STRING_CELL && i.type!=XR_LIR_I64_CELL)++bits;
            }
            if(strings>UINT32_MAX || bits>UINT32_MAX)
                return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"instruction temporary count exceeds index space");
            if(c->temporary_strings<strings)c->temporary_strings=(uint32_t)strings;
            if(c->temporary_bits<bits)c->temporary_bits=(uint32_t)bits;
            if(i.op==XR_LIR_CONDBR)c->control_scratch=true;
            if(i.op==XR_LIR_CALL && c->functions[i.callee-1]!=RBC_LIR_PRINT_STRING) {
                uint32_t mapped=c->functions[i.callee-1];
                uint32_t physical=rbc_lir_native(mapped)?
                    (mapped==RBC_LIR_NATIVE_BASE+XR_RBC_NATIVE_PRINT_I64 ||
                     mapped==RBC_LIR_NATIVE_BASE+XR_RBC_NATIVE_UTC_OFFSET_AT?2u:1u):i.operand_count;
                if(physical==UINT32_MAX)
                    return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"call window exceeds index space");
                if(window<physical+1)window=physical+1;
            }
        }
    }
    if(c->view.value_count>XR_RBC_REGISTER_COUNT_MAX)return rbc_lir_spill_slots(c,window);
    if (!rbc_lir_assign_slots(c)) return false;
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView bv;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &bv);
        for (uint32_t n = 0; n < bv.instruction_count; ++n) {
            XrLirInstructionView i;
            xr_lir_instruction_view(c->input, block, n, &i);
            if (i.op == XR_LIR_CALL) {
                if (c->functions[i.callee - 1] == RBC_LIR_PRINT_STRING) continue;
                uint32_t mapped = c->functions[i.callee - 1];
                uint32_t physical = rbc_lir_native(mapped) ?
                    (mapped == RBC_LIR_NATIVE_BASE + XR_RBC_NATIVE_PRINT_I64 ||
                     mapped == RBC_LIR_NATIVE_BASE + XR_RBC_NATIVE_UTC_OFFSET_AT ? 2u : 1u) : i.operand_count;
                if (window < physical + 1) window = physical + 1;
            }
            if (i.op == XR_LIR_CONDBR && !c->scratch_count) c->scratch_count = 1;
            for (uint32_t e = 0; e < i.edge_count; ++e) {
                bool scratch = false;
                if (!rbc_lir_copies(c, i.edges[e], false, &scratch)) return false;
                if (scratch) c->scratch_count = c->string_scratch ? 2u : 1u;
            }
        }
    }
    uint64_t registers = (uint64_t) c->slot_count + c->scratch_count + window;
    if (registers > XR_RBC_REGISTER_COUNT_MAX)
        return rbc_lir_spill_slots(c,window);
    c->register_count = registers ? (uint32_t) registers : 1;
    return true;
}

static bool rbc_lir_kinds(RbcLirCompiler *c) {
    c->register_kinds = xr_calloc(c->register_count, sizeof(*c->register_kinds));
    c->cell_kinds = xr_calloc(c->view.value_count ? c->view.value_count : 1, sizeof(*c->cell_kinds));
    if (!c->register_kinds || !c->cell_kinds)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate typed register declarations");
    bool assigned[XR_RBC_REGISTER_COUNT_MAX] = {0};
    for (uint32_t v = 1; v <= c->view.value_count; ++v) {
        XrLirType type = XR_LIR_TYPE_INVALID;
        xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, v}, &type);
        if (c->output.literals[v] || c->output.lengths[v]) continue;
        XrRbcRegisterKind kind = type == XR_LIR_STRING ? XR_RBC_STRING :
            type == XR_LIR_STRING_CELL ? XR_RBC_STRING_CELL : XR_RBC_BITS;
        uint32_t slot = c->slots[v];
        if(c->typed_spills && c->spill_home[v]!=UINT32_MAX)continue;
        if (assigned[slot] && c->register_kinds[slot] != kind)
            return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "edge merges distinct register kinds");
        assigned[slot] = true;
        c->register_kinds[slot] = kind;
    }
    if(c->typed_spills)
        for(uint32_t r=0;r<c->temporary_strings;++r)c->register_kinds[c->slot_count+r]=XR_RBC_STRING;
    else if (c->string_scratch) c->register_kinds[c->slot_count + 1] = XR_RBC_STRING;
    for (uint32_t r = c->slot_count + c->scratch_count; r < c->register_count; ++r)
        c->register_kinds[r] = XR_RBC_CALL_WINDOW;
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
    bool scratch = false;
    return rbc_lir_copies(c, edge, true, &scratch) && rbc_lir_jump(c, edge.target);
}

static bool rbc_lir_conditional(RbcLirCompiler *c, const XrLirInstructionView *i) {
    uint32_t scratch = c->slot_count + (c->typed_spills ? c->temporary_strings+c->temporary_bits : 0);
    if (!rbc_lir_emit(c, XR_RBC_LOADI, scratch, 0, 0, 0) ||
        !rbc_lir_emit(c, XR_RBC_EQ, c->slots[i->operands[0]], scratch, 0, 0))
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

static bool rbc_lir_literal(RbcLirCompiler *c, uint32_t origin, uint32_t *ordinal) {
    RbcLirLiteralPool *pool = c->literals;
    if (!origin || origin > xr_lir_literal_count(c->input) || !pool->mapping)
        return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "literal has no verified output identity");
    if (pool->mapping[origin - 1]) {
        *ordinal = pool->mapping[origin - 1] - 1;
        return true;
    }
    XrLirLiteral literal = {0};
    const uint8_t *bytes = NULL;
    size_t length = 0;
    if (xr_lir_literal_at(c->input, origin - 1, &literal) != XR_LIR_OK ||
        xr_lir_literal_view(c->input, literal, &bytes, &length) != XR_LIR_OK)
        return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "cannot read verified literal bytes");
    XrRbcModule *module = &pool->program->module;
    if (module->literal_count > UINT16_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "literal table exceeds bytecode encoding");
    void *storage = (void *)module->literals;
    if (!rbc_lir_grow(c, &storage, &pool->capacity,
            module->literal_count + 1, sizeof(*module->literals))) return false;
    module->literals = storage;
    char *copy = length ? xr_malloc(length) : NULL;
    if (length && !copy)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate immutable literal bytes");
    if (length) memcpy(copy, bytes, length);
    *ordinal = (uint32_t)module->literal_count;
    ((XrRbcLiteral *)module->literals)[module->literal_count++] = (XrRbcLiteral) {copy, length};
    pool->mapping[origin - 1] = *ordinal + 1;
    return true;
}

static bool rbc_lir_instruction(RbcLirCompiler *c, const XrLirInstructionView *i) {
    uint32_t result = i->result ? c->slots[i->result] : 0;
    switch (i->op) {
    case XR_LIR_CONST:
        return c->output.lengths[i->result] || rbc_lir_constant(c, result, i->bits);
    case XR_LIR_LITERAL_ADDR:
        return true;
    case XR_LIR_INTCAST:
        return rbc_lir_emit(c, XR_RBC_MOV, result, c->slots[i->operands[0]], 0, 0);
    case XR_LIR_STRING_LITERAL: {
        uint32_t ordinal;
        return rbc_lir_literal(c, (uint32_t)i->bits, &ordinal) &&
            rbc_lir_emit(c, XR_RBC_STRING_LITERAL, result, 0, 0, ordinal);
    }
    case XR_LIR_STRING_I64: case XR_LIR_STRING_RETAIN: case XR_LIR_STRING_CONCAT:
        return rbc_lir_emit(c, i->op == XR_LIR_STRING_I64 ? XR_RBC_STRING_I64 :
            i->op == XR_LIR_STRING_RETAIN ? XR_RBC_STRING_RETAIN : XR_RBC_STRING_CONCAT,
            result, c->slots[i->operands[0]], i->operand_count == 2 ? c->slots[i->operands[1]] : 0, 0);
    case XR_LIR_STRING_LEN:
        return rbc_lir_emit(c, XR_RBC_STRING_LEN, result, c->slots[i->operands[0]], 0, 0);
    case XR_LIR_STRING_DROP: case XR_LIR_STRING_CELL_DROP: case XR_LIR_STRING_PRINT:
        return rbc_lir_emit(c, i->op == XR_LIR_STRING_DROP ? XR_RBC_STRING_DROP :
            i->op == XR_LIR_STRING_CELL_DROP ? XR_RBC_STRING_CELL_DROP : XR_RBC_STRING_PRINT,
            c->slots[i->operands[0]], 0, 0, 0);
    case XR_LIR_CELL_NEW:
        if (i->type == XR_LIR_STRING_CELL) {
            c->cell_kinds[c->cell_count] = XR_RBC_STRING;
            return rbc_lir_emit(c, XR_RBC_STRING_CELL_NEW, result, c->slots[i->operands[0]], c->cell_count++, 0);
        }
        c->cell_kinds[c->cell_count] = XR_RBC_BITS;
        return rbc_lir_emit(c, XR_RBC_MOV, result, c->slots[i->operands[0]], 0, 0) &&
            rbc_lir_emit(c, XR_RBC_CELL_NEW, result, 0, 0, c->cell_count++);
    case XR_LIR_CELL_READ:
        return rbc_lir_emit(c, XR_RBC_CELL_READ, result, c->slots[i->operands[0]], 0, 0);
    case XR_LIR_CELL_WRITE:
        return rbc_lir_emit(c, XR_RBC_CELL_WRITE, c->slots[i->operands[0]],
            c->slots[i->operands[1]], 0, 0);
    case XR_LIR_FCMP: {
        XrRbcOp op;
        switch (i->predicate) {
            case XR_LIR_EQ: op=XR_RBC_FEQ;break;
            case XR_LIR_NE: op=XR_RBC_FNE;break;
            case XR_LIR_LT: op=XR_RBC_FLT;break;
            case XR_LIR_LE: op=XR_RBC_FLE;break;
            case XR_LIR_GT: op=XR_RBC_FGT;break;
            case XR_LIR_GE: op=XR_RBC_FGE;break;
            default:return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"invalid floating predicate");
        }
        return rbc_lir_emit(c,op,result,c->slots[i->operands[0]],c->slots[i->operands[1]],0);
    }
    case XR_LIR_ICMP: return rbc_lir_comparison(c, i);
    case XR_LIR_BR: return rbc_lir_edge(c, i->edges[0]);
    case XR_LIR_CONDBR: return rbc_lir_conditional(c, i);
    case XR_LIR_RET:
        return rbc_lir_emit(c, i->operand_count ? XR_RBC_RET : XR_RBC_RET0,
            i->operand_count ? c->slots[i->operands[0]] : 0, 0, 0, 0);
    case XR_LIR_CALL: {
        if (c->functions[i->callee - 1] == RBC_LIR_PRINT_STRING) {
            uint32_t ordinal;
            return rbc_lir_literal(c, c->output.literals[i->operands[0]], &ordinal) &&
                rbc_lir_emit(c, XR_RBC_PRINT_LITERAL, 0, 0, 0, ordinal);
        }
        if (rbc_lir_native(c->functions[i->callee - 1])) {
            uint32_t kind = c->functions[i->callee - 1] - RBC_LIR_NATIVE_BASE;
            bool argument = kind == XR_RBC_NATIVE_PRINT_I64 || kind == XR_RBC_NATIVE_UTC_OFFSET_AT;
            uint32_t destination = c->register_count - (argument ? 3u : 2u);
            if (argument && !rbc_lir_emit(c, XR_RBC_MOV, destination + 1,
                    c->slots[i->operands[0]], 0, 0)) return false;
            if (!rbc_lir_constant(c, destination + (argument ? 2u : 1u),
                    i->source.path_length ? c->source_id : 0) ||
                !rbc_lir_emit(c, XR_RBC_CALLN, destination, 0, 0, kind)) return false;
            return !i->result || rbc_lir_emit(c, XR_RBC_MOV, result, destination, 0, 0);
        }
        uint32_t destination = c->register_count - i->operand_count - 1;
        for (uint32_t n = 0; n < i->operand_count; ++n)
            if (!rbc_lir_emit(c, XR_RBC_MOV, destination + n + 1,
                    c->slots[i->operands[n]], 0, 0)) return false;
        if (!rbc_lir_emit(c, XR_RBC_CALL, destination, 0, 0,
                c->functions[i->callee - 1] - 1)) return false;
        return !i->result || rbc_lir_emit(c,
            i->type == XR_LIR_STRING ? XR_RBC_STRING_TAKE : XR_RBC_MOV, result, destination, 0, 0);
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
        case XR_LIR_FNEG: op = XR_RBC_FNEG; break;
        case XR_LIR_FADD: op = XR_RBC_FADD; break;
        case XR_LIR_FSUB: op = XR_RBC_FSUB; break;
        case XR_LIR_FMUL: op = XR_RBC_FMUL; break;
        case XR_LIR_FDIV: op = XR_RBC_FDIV; break;
        default: return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "unsupported operation");
        }
        return rbc_lir_emit(c, op, result, c->slots[i->operands[0]],
            i->operand_count == 2 ? c->slots[i->operands[1]] : 0, 0);
    }
    }
}

static bool rbc_lir_spilled_instruction(RbcLirCompiler *c,const XrLirInstructionView *i) {
    if(!c->spill_count)return rbc_lir_instruction(c,i);
    uint32_t staged[XR_RBC_REGISTER_COUNT_MAX],count=0,strings=0,bits=0;
    for(uint32_t a=0;a<i->operand_count;++a) {
        uint32_t value=i->operands[a];
        if(c->spill_home[value]==UINT32_MAX || c->slots[value]!=UINT32_MAX)continue;
        bool string=rbc_lir_value_type(c,value)==XR_LIR_STRING;
        uint32_t slot=c->slot_count+(string?strings++:c->temporary_strings+bits++);
        if(strings>c->temporary_strings || bits>c->temporary_bits || count==XR_RBC_REGISTER_COUNT_MAX)
            return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"spill operands exceed typed temporary registers");
        if(!rbc_lir_emit(c,XR_RBC_RELOAD,slot,0,0,c->spill_home[value]))return false;
        c->slots[value]=slot;staged[count++]=value;
    }
    bool result=i->result && c->spill_home[i->result]!=UINT32_MAX;
    if(result) {
        bool string=i->type==XR_LIR_STRING;
        c->slots[i->result]=c->slot_count+(string?strings++:c->temporary_strings+bits++);
        if(strings>c->temporary_strings || bits>c->temporary_bits)
            return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"spill result exceeds typed temporary registers");
    }
    bool valid=rbc_lir_instruction(c,i);
    if(valid && result)
        valid=rbc_lir_emit(c,XR_RBC_SPILL,c->slots[i->result],0,0,c->spill_home[i->result]);
    for(uint32_t n=0;n<count;++n) {
        uint32_t value=staged[n];
        if(valid && i->op!=XR_LIR_RET && i->op!=XR_LIR_STRING_DROP)
            valid=rbc_lir_emit(c,XR_RBC_SPILL,c->slots[value],0,0,c->spill_home[value]);
        c->slots[value]=UINT32_MAX;
    }
    if(result)c->slots[i->result]=UINT32_MAX;
    return valid;
}
static bool rbc_lir_function(RbcLirCompiler *c, XrRbcFunction *out) {
    if(!rbc_lir_output_values(c->input,c->function,&c->view,&c->output,c->diagnostic)) {
        c->status=c->diagnostic->status;return false;
    }
    size_t values=(size_t)c->view.value_count+1;
    if(!values || values>SIZE_MAX/sizeof(*c->slots))
        return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"SSA home table exceeds address space");
    c->slots=xr_malloc(values*sizeof(*c->slots));
    c->spill_home=xr_malloc(values*sizeof(*c->spill_home));
    if(!c->slots || !c->spill_home)
        return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot allocate SSA home tables");
    for(uint32_t v=0;v<=c->view.value_count;++v)c->spill_home[v]=UINT32_MAX;
    if (!rbc_lir_layout(c) || !rbc_lir_kinds(c)) return false;
    size_t count = (size_t) c->view.block_count + 1;
    if (count > SIZE_MAX / sizeof(*c->blocks))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "block table exceeds address space");
    c->blocks = xr_malloc(count * sizeof(*c->blocks));
    if (!c->blocks)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate block offsets");
    if ((!c->typed_spills || c->view.block_count > 1) && !rbc_lir_jump(c, c->view.entry)) return false;
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView bv;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &bv);
        c->blocks[block.id] = c->code_count;
        for (uint32_t n = 0; n < bv.instruction_count; ++n) {
            XrLirInstructionView i;
            xr_lir_instruction_view(c->input, block, n, &i);
            c->current_source_id = i.source.path_length ? ++c->source_id : 0;
            c->diagnostic->block = block.id;
            c->diagnostic->instruction = n + 1;
            rbc_lir_error(c->diagnostic, XR_RBC_COMPILE_OK, "", i.source);
            if (!rbc_lir_spilled_instruction(c, &i)) return false;
        }
    }
    for (size_t i = 0; i < c->patch_count; ++i) {
        RbcLirPatch patch = c->patches[i];
        if (!xr_rbc_jump(&c->code[patch.pc], patch.pc, c->blocks[patch.block], c->code_count))
            return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "branch exceeds jump encoding");
    }
    *out = (XrRbcFunction) {.code=c->code, .code_count=c->code_count,
        .constants=c->constants, .constant_count=c->constant_count,
        .register_count=c->register_count, .parameter_count=c->view.parameter_count,
        .source_ids=c->source_ids, .source_id_count=c->code_count, .cell_count=c->cell_count,
        .register_kinds=c->register_kinds, .register_kind_count=c->register_count,
        .cell_kinds=c->cell_kinds, .cell_kind_count=c->cell_count,
        .result_kind=c->view.result == XR_LIR_STRING ? XR_RBC_STRING : XR_RBC_BITS,
        .spill_count=c->spill_count,.reference_spill_count=c->reference_spill_count};
    return true;
}

XR_FUNC void xr_rbc_program_destroy(XrRbcProgram *program) {
    if (!program) return;
    for (size_t f = 0; f < program->module.function_count; ++f) {
        xr_free((void *) program->module.functions[f].code);
        xr_free((void *) program->module.functions[f].constants);
        xr_free((void *) program->module.functions[f].source_ids);
        xr_free((void *) program->module.functions[f].register_kinds);
        xr_free((void *) program->module.functions[f].cell_kinds);
    }
    xr_free((void *) program->module.functions);
    for (size_t i = 0; i < program->module.literal_count; ++i)
        xr_free((void *)program->module.literals[i].bytes);
    xr_free((void *)program->module.literals);
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
    size_t literals = xr_lir_literal_count(input);
    RbcLirLiteralPool pool = {.program = p};
    if (literals > SIZE_MAX / sizeof(*pool.mapping))
        return rbc_lir_error(d, XR_RBC_COMPILE_LIMIT, "literal mapping exceeds address space",
            (XrLirSource) {0});
    if (literals) {
        pool.mapping = xr_calloc(literals, sizeof(*pool.mapping));
        if (!pool.mapping)
            return rbc_lir_error(d, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate literal mapping",
                (XrLirSource) {0});
    }
    for (size_t f = 0; f < p->module.function_count; ++f) {
        RbcLirCompiler c = {.input = input, .function = {input, ids[f]},
            .functions = mapping, .diagnostic = d, .source_id = source_id, .literals = &pool};
        xr_lir_function_view(input, c.function, &c.view);
        d->function = ids[f];
        bool success = rbc_lir_function(&c, &((XrRbcFunction *) p->module.functions)[f]);
        source_id = c.source_id;
        xr_free(c.blocks);
        xr_free(c.patches);
        rbc_lir_output_free(&c.output);
        xr_free(c.slots);xr_free(c.spill_home);
        if (!success) {
            xr_free(c.code);
            xr_free(c.constants);
            xr_free(c.source_ids);
            xr_free(c.register_kinds);
            xr_free(c.cell_kinds);
            xr_free(pool.mapping);
            return c.status;
        }
        if (!f) {
            p->parameter_count = c.view.parameter_count;
            if (p->parameter_count)
                memcpy(p->parameters, c.view.parameter_types,
                    p->parameter_count * sizeof(*p->parameters));
        }
    }
    xr_free(pool.mapping);
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
    for (uint32_t i = 0; i < root.parameter_count; ++i)
        if (root.parameter_types[i] == XR_LIR_I64_CELL || root.parameter_types[i] == XR_LIR_STRING_CELL ||
            root.parameter_types[i] == XR_LIR_STRING)
            return rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
                "execution entry cannot borrow a cell argument or String value", (XrLirSource) {0});
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
        if (!rbc_lir_native(mapping[i])) continue;
        for (uint32_t kind=0; kind<XR_RBC_NATIVE_COUNT; ++kind)
            p->natives[kind] = (XrRbcNativeKind)kind;
        p->module.natives = p->natives;
        p->module.native_count = XR_RBC_NATIVE_COUNT;
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
        XrRbcStatus admitted = xr_rbc_module_admit(&p->module, p->locations);
        if (admitted != XR_RBC_OK) {
            status = admitted == XR_RBC_OUT_OF_MEMORY ? XR_RBC_COMPILE_OUT_OF_MEMORY :
                XR_RBC_COMPILE_UNSUPPORTED;
            rbc_lir_error(d, status, "register program does not satisfy runtime admission",
                (XrLirSource) {0});
            goto done;
        }
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
