#include "lir/xlir_state_projection.h"
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
#include "lir/xlir_string.h"
#include "xray_native_string.h"
#include "lir/xlir_verify.h"
#include "lir/xlir_state.h"
#include "lir/xlir_array.h"
#include "lir/xlir_view.h"
#include "lir/xlir_display.h"
#include "lir/xlir_builtin_admission.h"
#include "base/xmalloc.h"
#include "base/xchecks.h"
#include <stdio.h>
#include <string.h>

struct XrRbcProgram {
    XrRbcModule module;
    XrtLocationOwner *locations;
    XrRbcNativeKind natives[XR_RBC_NATIVE_COUNT];
    XrLirType parameters[XR_RBC_REGISTER_COUNT_MAX];
    uint32_t parameter_count;
    uint32_t *physical_aggregates;
    uint32_t *physical_callables;
};

#define RBC_LIR_PRINT_STRING (UINT32_MAX - 1)
#define RBC_LIR_NATIVE_BASE (UINT32_MAX - 8)

static bool rbc_lir_native(uint32_t mapping) {
    return mapping >= RBC_LIR_NATIVE_BASE && mapping < RBC_LIR_NATIVE_BASE + XR_RBC_NATIVE_COUNT;
}

typedef struct {
    uint32_t *literals;
    bool *lengths;
    uint8_t *uses;
} RbcLirOutputValues;

typedef struct {
    XrRbcProgram *program;
    uint32_t *mapping;
    size_t capacity;
} RbcLirLiteralPool;

typedef struct { size_t pc; uint32_t block; } RbcLirPatch;
typedef struct {
    int16_t value;
    uint16_t constant;
    bool known, pooled, materialize;
} RbcLirImmediate;
typedef struct {
    const XrLirModule *input;
    XrLirFunction function;
    XrLirFunctionView view;
    const uint32_t *functions;
    RbcLirOutputValues output;
    RbcLirLiteralPool *literals;
    uint32_t *slots, *spill_home;
    uint32_t *aggregate_home, *aggregate_scratch;
    uint32_t *aggregate_parameters, *aggregate_arguments;
    uint32_t scalar_parameter_count, aggregate_parameter_count;
    size_t aggregate_argument_count, aggregate_argument_capacity;
    XrRbcAggregateValue *aggregate_values;
    uint32_t aggregate_value_count;
    size_t aggregate_size;
    XrRbcAggregateAccess *aggregate_accesses;
    XrRbcArrayLocation *aggregate_operands;
    size_t aggregate_access_count, aggregate_access_capacity, aggregate_operand_count, aggregate_operand_capacity;
    RbcLirImmediate *immediates;
    uint32_t *view_constants;
    uint32_t spill_count, reference_spill_count, temporary_strings, temporary_bits, temporary_refs;
    uint32_t temporary_arrays[XR_RBC_ARRAY_FAMILY_COUNT];
    bool typed_spills;
    uint32_t slot_count, scratch_count, register_count, source_id, current_source_id, cell_count;
    uint32_t *code, *source_ids;
    uint64_t *constants;
    XrRbcRegisterKind *register_kinds, *cell_kinds;
    uint32_t *register_callables;
    XrRbcStateAccess *state_accesses;
    size_t state_access_count, state_access_capacity;
    XrRbcViewAccess *view_accesses;
    size_t view_access_count, view_access_capacity;
    XrRbcArrayAccess *array_accesses;
    XrRbcArrayLocation *array_operands;
    XrRbcRegisterKind *reference_spill_kinds;
    size_t array_access_count, array_access_capacity, array_operand_count, array_operand_capacity;
    size_t code_count, code_capacity, source_id_capacity, constant_count, constant_capacity;
    size_t *blocks;
    RbcLirPatch *patches;
    size_t patch_count, patch_capacity;
    XrRbcCompileDiagnostic *diagnostic;
    XrRbcCompileStatus status;
    bool string_scratch, control_scratch, state_scratch, state_ref_scratch;
    bool array_scratch[XR_RBC_ARRAY_FAMILY_COUNT];
    bool view_scratch[3], has_views;
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
    return type == XR_LIR_I64 || type == XR_LIR_U8 || type == XR_LIR_U64 || type == XR_LIR_I1 || type == XR_LIR_F64;
}

static bool rbc_lir_callable_type(const XrLirModule *module, XrLirType type) {
    XrLirCallableType signature;
    return type >= XR_LIR_TYPE_COUNT && xr_lir_callable_type_view(module, type, &signature) == XR_LIR_OK;
}

static uint32_t rbc_lir_callable_ordinal(const RbcLirCompiler *c, XrLirType type) {
    return c->literals->program->physical_callables[type - XR_LIR_TYPE_COUNT];
}

static bool rbc_lir_unsigned_operation(const XrLirModule *input, XrLirFunction function,
    const XrLirInstructionView *i) {
    XrLirType source = XR_LIR_TYPE_INVALID;
    bool unsigned_value = i->type == XR_LIR_U8 || i->type == XR_LIR_U64;
    for (uint32_t p = 0; p < i->operand_count; ++p) {
        XrLirType type = XR_LIR_TYPE_INVALID;
        if (xr_lir_value_type(input, (XrLirValue){input, function.id, i->operands[p]}, &type) != XR_LIR_OK) return false;
        if (!p) source = type;
        unsigned_value |= type == XR_LIR_U8 || type == XR_LIR_U64;
    }
    if (!unsigned_value) return true;
    switch (i->op) {
    case XR_LIR_NULLABLE_NONE: case XR_LIR_NULLABLE_SOME:
    case XR_LIR_NULLABLE_IS_SOME: case XR_LIR_NULLABLE_UNWRAP:
    case XR_LIR_FIXED_ARRAY_NEW: case XR_LIR_FIXED_ARRAY_GET:
    case XR_LIR_STATE_READ: case XR_LIR_STATE_WRITE:
    case XR_LIR_STATE_CONSTRUCT: case XR_LIR_STATE_INITIALIZE:
    case XR_LIR_CONST: case XR_LIR_CALL: case XR_LIR_CALL_INDIRECT: case XR_LIR_CALL_INTRINSIC:
    case XR_LIR_BR: case XR_LIR_RET: return true;
    case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL: case XR_LIR_NEG: return i->type == XR_LIR_U64;
    case XR_LIR_ICMP: return source == XR_LIR_U8 || i->predicate == XR_LIR_EQ || i->predicate == XR_LIR_NE;
    case XR_LIR_INTCAST: return (i->type == XR_LIR_I64 || i->type == XR_LIR_U8 || i->type == XR_LIR_U64) &&
        (source == XR_LIR_I64 || source == XR_LIR_U8 || source == XR_LIR_U64);
    default: return false;
    }
}

#include "xrbc_lir_array.inc.c"
#include "xrbc_lir_view.inc.c"
#include "xrbc_lir_aggregate.inc.c"

static bool rbc_lir_string_call(const XrLirModule *input, const XrLirInstructionView *i) {
    if (i->op != XR_LIR_CALL) return false;
    XrLirFunctionView callee;
    return xr_lir_function_view(input, (XrLirFunction) {input, i->callee}, &callee) == XR_LIR_OK &&
        callee.kind == XR_LIR_FUNCTION_EXTERNAL && callee.extern_kind == XR_LIR_EXTERN_PRINT_STRING;
}

static void rbc_lir_output_free(RbcLirOutputValues *output) {
    xr_free(output->literals); xr_free(output->lengths);
    xr_free(output->uses);
    *output=(RbcLirOutputValues){0};
}
static void rbc_lir_count_use(RbcLirOutputValues *output, uint32_t value) {
    // Only uniqueness matters, so counts saturate at two.
    if (output->uses[value] < 2)
        ++output->uses[value];
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
    output->uses=xr_calloc(count,sizeof(*output->uses));
    if (!output->literals || !output->lengths || !output->uses) {
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
            for (uint32_t a = 0; a < i.operand_count; ++a)
                rbc_lir_count_use(output, i.operands[a]);
            for (uint32_t e = 0; e < i.edge_count; ++e)
                for (uint32_t a = 0; a < i.edges[e].argument_count; ++a)
                    rbc_lir_count_use(output, i.edges[e].arguments[a]);
            if (i.op == XR_LIR_LITERAL_ADDR) output->literals[i.result] = (uint32_t)i.bits;
            // The LIR verifier owns the pair's provenance, exact length and
            // unique direct address use. These IDs only select no-code values.
            if (rbc_lir_string_call(input, &i)) output->lengths[i.operands[1]] = true;
        }
    }
    return true;
}

static bool rbc_lir_pool(RbcLirCompiler *c, uint64_t bits, uint32_t *index) {
    if (c->constant_count > UINT16_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "constant pool exceeds encoding");
    void *storage = c->constants;
    if (!rbc_lir_grow(c, &storage, &c->constant_capacity,
            c->constant_count + 1, sizeof(*c->constants))) return false;
    c->constants = storage;
    *index = (uint32_t)c->constant_count;
    c->constants[c->constant_count++] = bits;
    return true;
}

// Return the operand encoded in the instruction, or operand_count if both
// operands still require registers. Selection depends only on immutable SSA.
static uint32_t rbc_lir_immediate_operand(const RbcLirCompiler *c,
    const XrLirInstructionView *i) {
    if (i->operand_count != 2) return i->operand_count;
    if (i->op == XR_LIR_FCMP && i->predicate == XR_LIR_LE &&
        c->immediates[i->operands[1]].pooled) return 1;
    bool comparison = i->op == XR_LIR_ICMP || i->op == XR_LIR_CONDBR;
    if (!comparison && (i->type != XR_LIR_I64 ||
        (i->op != XR_LIR_ADD && i->op != XR_LIR_SUB &&
         i->op != XR_LIR_MUL && i->op != XR_LIR_REM))) return i->operand_count;
    for (uint32_t n = 2; n; --n) {
        uint32_t a = n - 1;
        const RbcLirImmediate *value = &c->immediates[i->operands[a]];
        if (value->known && (comparison ||
            ((a == 1 || i->op == XR_LIR_ADD || i->op == XR_LIR_MUL) &&
             value->value >= INT8_MIN && value->value <= INT8_MAX))) return a;
    }
    return i->operand_count;
}

static bool rbc_lir_immediates(RbcLirCompiler *c) {
    size_t count = (size_t)c->view.value_count + 1;
    if (count > SIZE_MAX / sizeof(*c->immediates))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "immediate table exceeds address space");
    c->immediates = xr_calloc(count, sizeof(*c->immediates));
    if (!c->immediates)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate immediate table");
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &view);
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView i;
            xr_lir_instruction_view(c->input, block, n, &i);
            // Reuse entries at every other use of the same SSA constant. Only
            // reserve values that already need LOADK, preserving pool limits.
            c->has_views |= rbc_lir_view_role(c->input, &i) != 0;
            if (i.op == XR_LIR_CONST && i.type == XR_LIR_F64 &&
                i.bits > INT16_MAX && i.bits < UINT64_MAX - 32767 &&
                c->constant_count <= UINT8_MAX) {
                uint32_t index;
                if (!rbc_lir_pool(c, i.bits, &index)) return false;
                c->immediates[i.result].pooled = true;
                c->immediates[i.result].constant = (uint16_t)index;
            }
            if (i.op != XR_LIR_CONST || i.type != XR_LIR_I64 ||
                (i.bits > INT16_MAX && i.bits < UINT64_MAX - 32767)) continue;
            c->immediates[i.result].known = true;
            c->immediates[i.result].value = (int16_t)(i.bits <= INT16_MAX ?
                (int64_t)i.bits : -1 - (int64_t)~i.bits);
        }
    }
    if (c->has_views) {
        c->view_constants = xr_malloc(count * sizeof(*c->view_constants));
        if (!c->view_constants)
            return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot receive view slot constants");
        for (size_t v = 0; v < count; ++v) c->view_constants[v] = UINT32_MAX;
    }
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &view);
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView i;
            xr_lir_instruction_view(c->input, block, n, &i);
            if (c->view_constants && i.op == XR_LIR_CONST && i.type == XR_LIR_U32 && i.bits < UINT32_MAX)
                c->view_constants[i.result] = (uint32_t)i.bits;
            uint32_t selected = rbc_lir_immediate_operand(c, &i);
            for (uint32_t a = 0; a < i.operand_count; ++a)
                if (a != selected) c->immediates[i.operands[a]].materialize = true;
            for (uint32_t e = 0; e < i.edge_count; ++e)
                for (uint32_t a = 0; a < i.edges[e].argument_count; ++a)
                    c->immediates[i.edges[e].arguments[a]].materialize = true;
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
        (v->result != XR_LIR_UNIT && v->result != XR_LIR_STRING && v->result != XR_LIR_ARRAY &&
         v->result != XR_LIR_SLICE_I64 &&
         !rbc_lir_scalar(v->result) && !rbc_lir_aggregate_type(input, v->result))) {
        rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
            "execution requires a supported typed function definition",
            rbc_lir_value_source(input, f, v, 0, d));
        return false;
    }
    for (uint32_t p = 0; p < v->parameter_count; ++p)
        if (v->parameter_types[p] == XR_LIR_U32) {
            rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
                "U32 is admitted only for closed view slot constants", (XrLirSource){0});
            return false;
        }
    if (!rbc_lir_output_values(input, f, v, output,d)) return false;
    if (v->result == XR_LIR_ARRAY) {
        XrLirArrayShape shape = {0};
        if (xr_lir_array_result_view(input, f, &shape) != XR_LIR_OK ||
            rbc_lir_array_family(shape.element) == XR_RBC_ARRAY_FAMILY_COUNT) {
            rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED, "Array result has no bytecode element implementation", (XrLirSource){0});
            return false;
        }
    }
    for (uint32_t value = 1; value <= v->value_count; ++value) {
        XrLirType type;
        if (xr_lir_value_type(input, (XrLirValue) {input, f.id, value}, &type) != XR_LIR_OK ||
            (!rbc_lir_scalar(type) && type != XR_LIR_I64_CELL && type != XR_LIR_STRING && type != XR_LIR_STRING_CELL &&
             type != XR_LIR_STATE && type != XR_LIR_STATE_REF && type != XR_LIR_U32 &&
             type != XR_LIR_VIEW_FRAME && type != XR_LIR_VIEW_CELL && type != XR_LIR_SLICE_I64 &&
             type != XR_LIR_ARRAY && type != XR_LIR_ARRAY_CELL && type != XR_LIR_ARRAY_REF && type != XR_LIR_STRING_REF &&
             !rbc_lir_aggregate_type(input, type) && !rbc_lir_callable_type(input, type) &&
             !(type == XR_LIR_PTR && output->literals[value]) &&
             !(type == XR_LIR_U64 && output->lengths[value]))) {
            rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
                "value type is not an admitted scalar",
                rbc_lir_value_source(input, f, v, value, d));
            return false;
        }
        if (type == XR_LIR_ARRAY || type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF) {
            XrLirArrayShape shape = {0};
            if (xr_lir_array_shape_view(input, (XrLirValue){input, f.id, value}, &shape) != XR_LIR_OK ||
                rbc_lir_array_family(shape.element) == XR_RBC_ARRAY_FAMILY_COUNT) {
                rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED, "Array value has no bytecode element implementation", (XrLirSource){0});
                return false;
            }
        } else if (type == XR_LIR_STATE_REF) {
            XrLirStateReference shape = {0};
            if (xr_lir_state_reference_view(input, (XrLirValue){input, f.id, value}, &shape) != XR_LIR_OK ||
                (shape.payload == XR_LIR_ARRAY && rbc_lir_array_family(shape.array_element) == XR_RBC_ARRAY_FAMILY_COUNT)) {
                rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED, "module reference has no bytecode element implementation", (XrLirSource){0});
                return false;
            }
        }
    }
    return true;
}

static bool rbc_lir_operation(const XrLirModule *input, XrLirFunction function,
    const XrLirInstructionView *i, const RbcLirOutputValues *output) {
    if (i->type == XR_LIR_U32 && i->op != XR_LIR_CONST) return false;
    if (rbc_lir_string_call(input, i)) return true;
    if (!rbc_lir_unsigned_operation(input, function, i)) return false;
    if (xr_lir_constructed_type_count(input) && !rbc_lir_aggregate_point(i) && !xr_lir_display_point(input, i) &&
        !rbc_lir_aggregate_state(input, function, i) && i->op != XR_LIR_CALL && i->op != XR_LIR_RET &&
        i->op != XR_LIR_FUNCTION_REF && i->op != XR_LIR_CALL_INDIRECT &&
        i->op != XR_LIR_BR && i->op != XR_LIR_CONDBR) {
        for (uint32_t p = 0; p < i->operand_count; ++p) {
            XrLirType type = XR_LIR_TYPE_INVALID;
            xr_lir_value_type(input, (XrLirValue){input, function.id, i->operands[p]}, &type);
            if (type >= XR_LIR_TYPE_COUNT) return false;
        }
    }
    // A length can also be an operand of otherwise valid unsigned LIR. Its
    // output use must not silently enable signed arithmetic or comparisons.
    for (uint32_t a = 0; a < i->operand_count; ++a)
        if (output->literals[i->operands[a]] || output->lengths[i->operands[a]]) return false;
    for (uint32_t e = 0; e < i->edge_count; ++e)
        for (uint32_t a = 0; a < i->edges[e].argument_count; ++a)
            if (output->literals[i->edges[e].arguments[a]] ||
                output->lengths[i->edges[e].arguments[a]]) return false;
    switch (i->op) {
    case XR_LIR_FUNCTION_REF: {
        XrLirClosureTarget target;
        return !i->operand_count && xr_lir_closure_target_view(input,
            (XrLirFunction){input, i->callee}, &target) == XR_LIR_OK &&
            !target.capture_count && !target.state_capture && target.callable == i->type;
    }
    case XR_LIR_CALL_INDIRECT: return true;
    case XR_LIR_CALL_INTRINSIC: {
        XrLirArrayIntrinsic shape = {0};
        return xr_lir_string_role(input, i) || xr_lir_display_point(input, i) || rbc_lir_view_role(input, i) ||
            (xr_lir_array_intrinsic_view(input, i->intrinsic, &shape) == XR_LIR_OK && shape.id);
    }
    case XR_LIR_ARRAY_RETAIN: case XR_LIR_ARRAY_DROP: case XR_LIR_ARRAY_CELL_DROP:
    case XR_LIR_ARRAY_BORROW: case XR_LIR_ARRAY_REF_READ: case XR_LIR_ARRAY_REF_WRITE: return true;
    case XR_LIR_VIEW_FRAME_ENTER: case XR_LIR_VIEW_FRAME_LEAVE: return true;
    case XR_LIR_MODULE_BEGIN: case XR_LIR_MODULE_READY:
    case XR_LIR_STATE_CONSTRUCT: case XR_LIR_STATE_PUBLISH:
    case XR_LIR_STATE_INITIALIZE: case XR_LIR_STATE_REFERENCE:
    case XR_LIR_STATE_READ: case XR_LIR_STATE_WRITE:
        return true;
    case XR_LIR_NULLABLE_NONE: case XR_LIR_NULLABLE_SOME:
    case XR_LIR_NULLABLE_IS_SOME: case XR_LIR_NULLABLE_UNWRAP:
    case XR_LIR_FIXED_ARRAY_NEW: case XR_LIR_FIXED_ARRAY_GET:
    case XR_LIR_LITERAL_ADDR:
    case XR_LIR_CONST: case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL:
    case XR_LIR_NEG: case XR_LIR_DIV: case XR_LIR_REM: case XR_LIR_ICMP:
    case XR_LIR_CALL: case XR_LIR_BR: case XR_LIR_CONDBR: case XR_LIR_RET:
    case XR_LIR_CELL_NEW: case XR_LIR_CELL_READ: case XR_LIR_CELL_WRITE:
    case XR_LIR_STRING_LITERAL: case XR_LIR_STRING_I64: case XR_LIR_STRING_F64: case XR_LIR_STRING_CONCAT:
    case XR_LIR_STRING_RETAIN: case XR_LIR_STRING_DROP: case XR_LIR_STRING_CELL_DROP:
    case XR_LIR_STRING_PRINT: case XR_LIR_STRING_LEN:
        return true;
    case XR_LIR_INTCAST: {
        XrLirType source = XR_LIR_TYPE_INVALID;
        return (i->type == XR_LIR_I64 || i->type == XR_LIR_U8 || i->type == XR_LIR_U64) && i->operand_count == 1 &&
            xr_lir_value_type(input, (XrLirValue){input, function.id, i->operands[0]},
                &source) == XR_LIR_OK && (source == XR_LIR_I64 || source == XR_LIR_U8 || source == XR_LIR_U64);
    }
    case XR_LIR_FNEG: {
        XrLirType source = XR_LIR_TYPE_INVALID;
        return i->type == XR_LIR_F64 && i->operand_count == 1 &&
            xr_lir_value_type(input, (XrLirValue){input, function.id, i->operands[0]},
                &source) == XR_LIR_OK && source == XR_LIR_F64;
    }
    case XR_LIR_ITOF: case XR_LIR_FTOI: {
        XrLirType source = XR_LIR_TYPE_INVALID;
        bool to_float = i->op == XR_LIR_ITOF;
        XrLirType result_type = to_float ? XR_LIR_F64 : XR_LIR_I64;
        XrLirType operand_type = to_float ? XR_LIR_I64 : XR_LIR_F64;
        return i->type == result_type && i->operand_count == 1 &&
            xr_lir_value_type(input, (XrLirValue){input, function.id, i->operands[0]},
                &source) == XR_LIR_OK && source == operand_type;
    }
    case XR_LIR_FADD: case XR_LIR_FSUB: case XR_LIR_FMUL: case XR_LIR_FDIV: case XR_LIR_FCMP: {
        XrLirType left=XR_LIR_TYPE_INVALID,right=XR_LIR_TYPE_INVALID;
        XrLirType result_type = i->op == XR_LIR_FCMP ? XR_LIR_I1 : XR_LIR_F64;
        return i->operand_count==2 && i->type==result_type &&
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
                if ((i.op == XR_LIR_CALL || i.op == XR_LIR_FUNCTION_REF) && !mapping[i.callee - 1]) {
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
static XrRbcRegisterKind rbc_lir_register_kind(RbcLirCompiler *c, uint32_t value, XrLirType type) {
    if (rbc_lir_callable_type(c->input, type)) return XR_RBC_FUNCTION;
    if (type == XR_LIR_ARRAY || type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF) {
        XrLirArrayShape shape = {0};
        if (xr_lir_array_shape_view(c->input, (XrLirValue){c->input, c->function.id, value}, &shape) != XR_LIR_OK)
            return XR_RBC_BITS;
        uint32_t family = rbc_lir_array_family(shape.element);
        return type == XR_LIR_ARRAY_CELL ? xr_rbc_array_cell_at(family) :
            type == XR_LIR_ARRAY_REF ? xr_rbc_array_ref_at(family) : xr_rbc_array_kind_at(family);
    }
    if (type == XR_LIR_VIEW_FRAME) return XR_RBC_VIEW_FRAME;
    if (type == XR_LIR_VIEW_CELL) return XR_RBC_VIEW_CELL;
    if (type == XR_LIR_SLICE_I64) return XR_RBC_SLICE_I64;
    if (type == XR_LIR_STRING_REF) return XR_RBC_STRING_REF;
    return type == XR_LIR_STRING ? XR_RBC_STRING : type == XR_LIR_STRING_CELL ? XR_RBC_STRING_CELL :
        type == XR_LIR_STATE ? XR_RBC_STATE : type == XR_LIR_STATE_REF ? XR_RBC_STATE_REF : XR_RBC_BITS;
}

static uint32_t rbc_lir_copy_temporary(RbcLirCompiler *c, XrRbcRegisterKind kind) {
    uint32_t family = xr_rbc_array_family(kind);
    uint32_t offset = kind == XR_RBC_STRING ? 1u : kind == XR_RBC_STATE ? 2u :
        kind == XR_RBC_STATE_REF ? 3u : family < XR_RBC_ARRAY_FAMILY_COUNT ? 4u + family : 0u;
    if (xr_rbc_view_kind(kind)) {
        uint32_t base = c->typed_spills ? c->scratch_count - 3u : 8u;
        return c->slot_count + base + (uint32_t)(kind - XR_RBC_VIEW_FRAME);
    }
    if (c->typed_spills) {
        if ((kind == XR_RBC_STRING && !c->temporary_strings) ||
            (kind == XR_RBC_STATE_REF && !c->temporary_refs) ||
            (family < XR_RBC_ARRAY_FAMILY_COUNT && !c->temporary_arrays[family]) ||
            (kind == XR_RBC_BITS && !c->temporary_bits && !c->control_scratch)) {
            rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "parallel copy has no matching typed scratch");
            return UINT32_MAX;
        }
        uint32_t arrays = (uint32_t)rbc_lir_array_temporaries(c, XR_RBC_ARRAY_FAMILY_COUNT);
        offset = kind == XR_RBC_STRING ? 0u : family < XR_RBC_ARRAY_FAMILY_COUNT ?
            c->temporary_strings + (uint32_t)rbc_lir_array_temporaries(c, family) :
            kind == XR_RBC_STATE_REF ? c->temporary_strings + arrays : c->temporary_strings + arrays + c->temporary_refs;
        if (kind == XR_RBC_STATE) offset += c->temporary_bits + (c->control_scratch ? 1u : 0u);
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
            c->has_views |= rbc_lir_view_role(c->input, &i) != 0;
            for(uint32_t e=0;e<i.edge_count;++e)
                for(uint32_t a=0;a<i.edges[e].argument_count;++a) {
                    uint32_t value=i.edges[e].arguments[a];
                    if(rbc_lir_value_type(c,value)==XR_LIR_ARRAY) {
                        uint32_t family=xr_rbc_array_family(rbc_lir_register_kind(c,value,XR_LIR_ARRAY));
                        if(!c->temporary_arrays[family])c->temporary_arrays[family]=1;
                    }
                }
            uint64_t strings=0,bits=0,refs=0,arrays[XR_RBC_ARRAY_FAMILY_COUNT]={0};
            if (xr_lir_string_role(c->input, &i) || xr_lir_display_point(c->input, &i) || rbc_lir_view_role(c->input, &i) || rbc_lir_array_point(c, &i) || rbc_lir_aggregate_point(&i) ||
                rbc_lir_aggregate_state(c->input, c->function, &i)) continue;
            for(uint32_t a=0;a<i.operand_count;++a) {
                uint32_t v=i.operands[a];XrLirType type=rbc_lir_value_type(c,v);
                if(c->output.literals[v] || c->output.lengths[v] ||
                    type==XR_LIR_STRING_CELL || type==XR_LIR_ARRAY_CELL || type==XR_LIR_ARRAY_REF || type==XR_LIR_STRING_REF ||
                    type==XR_LIR_I64_CELL || type==XR_LIR_STATE ||
                    type==XR_LIR_VIEW_FRAME || type==XR_LIR_VIEW_CELL || type==XR_LIR_SLICE_I64 || type>=XR_LIR_TYPE_COUNT)continue;
                if(type==XR_LIR_STRING)++strings;else if(type==XR_LIR_STATE_REF)++refs;
                else if(type==XR_LIR_ARRAY)++arrays[xr_rbc_array_family(rbc_lir_register_kind(c,v,type))];else ++bits;
            }
            if(i.result && !c->output.literals[i.result] && !c->output.lengths[i.result]) {
                if(i.type==XR_LIR_STRING)++strings;
                else if(i.type==XR_LIR_ARRAY)++arrays[xr_rbc_array_family(rbc_lir_register_kind(c,i.result,i.type))];
                else if(i.type==XR_LIR_STATE_REF)++refs;
                else if(i.type!=XR_LIR_STRING_CELL && i.type!=XR_LIR_ARRAY_CELL && i.type!=XR_LIR_ARRAY_REF && i.type!=XR_LIR_STRING_REF &&
                    i.type!=XR_LIR_I64_CELL && i.type!=XR_LIR_STATE && i.type!=XR_LIR_VIEW_FRAME &&
                    i.type!=XR_LIR_VIEW_CELL && i.type!=XR_LIR_SLICE_I64 && i.type<XR_LIR_TYPE_COUNT)++bits;
            }
            if (i.op == XR_LIR_INTCAST && i.type == XR_LIR_U8) ++bits;
            if(strings>UINT32_MAX || bits>UINT32_MAX)
                return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"instruction temporary count exceeds index space");
            if(c->temporary_strings<strings)c->temporary_strings=(uint32_t)strings;
            if(c->temporary_bits<bits)c->temporary_bits=(uint32_t)bits;
            if(c->temporary_refs<refs)c->temporary_refs=(uint32_t)refs;
            for (uint32_t a=0;a<XR_RBC_ARRAY_FAMILY_COUNT;++a) {
                if(arrays[a]>UINT32_MAX)return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"Array temporaries exceed index space");
                if(c->temporary_arrays[a]<arrays[a])c->temporary_arrays[a]=(uint32_t)arrays[a];
            }
            if(i.op==XR_LIR_CONDBR)c->control_scratch=true;
            if (i.op == XR_LIR_CALL_INDIRECT && window < i.operand_count) window = i.operand_count;
            if(i.op==XR_LIR_CALL && c->functions[i.callee-1]!=RBC_LIR_PRINT_STRING) {
                uint32_t mapped=c->functions[i.callee-1];
                uint32_t physical=rbc_lir_native(mapped)?
                    (mapped==RBC_LIR_NATIVE_BASE+XR_RBC_NATIVE_PRINT_I64 ||
                     mapped==RBC_LIR_NATIVE_BASE+XR_RBC_NATIVE_UTC_OFFSET_AT?2u:1u):rbc_lir_scalar_arguments(c->input, &i);
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
            if (i.op == XR_LIR_CALL_INDIRECT && window < i.operand_count) window = i.operand_count;
            if (i.op == XR_LIR_CALL) {
                if (c->functions[i.callee - 1] == RBC_LIR_PRINT_STRING) continue;
                uint32_t mapped = c->functions[i.callee - 1];
                uint32_t physical = rbc_lir_native(mapped) ?
                    (mapped == RBC_LIR_NATIVE_BASE + XR_RBC_NATIVE_PRINT_I64 ||
                     mapped == RBC_LIR_NATIVE_BASE + XR_RBC_NATIVE_UTC_OFFSET_AT ? 2u : 1u) : rbc_lir_scalar_arguments(c->input, &i);
                if (window < physical + 1) window = physical + 1;
            }
            if (i.op == XR_LIR_CONDBR && !c->scratch_count) c->scratch_count = 1;
            if (i.op == XR_LIR_INTCAST && i.type == XR_LIR_U8 && !c->scratch_count) c->scratch_count = 1;
            for (uint32_t e = 0; e < i.edge_count; ++e) {
                bool scratch = false;
                if (!rbc_lir_copies(c, i.edges[e], false, &scratch)) return false;
                if (scratch) {
                    uint32_t count = c->state_ref_scratch ? 4u : c->state_scratch ? 3u : c->string_scratch ? 2u : 1u;
                    for (uint32_t a = 0; a < XR_RBC_ARRAY_FAMILY_COUNT; ++a)
                        if (c->array_scratch[a]) count = 5u + a;
                    for (uint32_t v = 0; v < 3; ++v)
                        if (c->view_scratch[v]) count = 9u + v;
                    if (c->scratch_count < count) c->scratch_count = count;
                }
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
        if (c->output.literals[v] || c->output.lengths[v] || rbc_lir_aggregate_type(c->input, type)) continue;
        XrRbcRegisterKind kind = rbc_lir_register_kind(c, v, type);
        uint32_t slot = c->slots[v];
        if(c->typed_spills && c->spill_home[v]!=UINT32_MAX)continue;
        if (kind == XR_RBC_FUNCTION) {
            if (!c->register_callables) {
                c->register_callables = xr_calloc(c->register_count, sizeof(*c->register_callables));
                if (!c->register_callables)
                    return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot own register invocation signatures");
            }
            uint32_t signature = rbc_lir_callable_ordinal(c, type) + 1;
            if (c->register_callables[slot] && c->register_callables[slot] != signature)
                return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "register coalescing crosses invocation signatures");
            c->register_callables[slot] = signature;
        }
        if (assigned[slot] && c->register_kinds[slot] != kind)
            return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "edge merges distinct register kinds");
        assigned[slot] = true;
        c->register_kinds[slot] = kind;
    }
    if(c->typed_spills) {
        uint32_t arrays=(uint32_t)rbc_lir_array_temporaries(c,XR_RBC_ARRAY_FAMILY_COUNT);
        for(uint32_t r=0;r<c->temporary_strings;++r)c->register_kinds[c->slot_count+r]=XR_RBC_STRING;
        for(uint32_t a=0;a<XR_RBC_ARRAY_FAMILY_COUNT;++a)
            for(uint32_t r=0;r<c->temporary_arrays[a];++r)
                c->register_kinds[c->slot_count+c->temporary_strings+(uint32_t)rbc_lir_array_temporaries(c,a)+r]=xr_rbc_array_kind_at(a);
        for(uint32_t r=0;r<c->temporary_refs;++r)
            c->register_kinds[c->slot_count+c->temporary_strings+arrays+r]=XR_RBC_STATE_REF;
        if(c->state_scratch)c->register_kinds[c->slot_count+c->scratch_count-1-(c->has_views?3u:0u)]=XR_RBC_STATE;
    } else {
        if(c->string_scratch)c->register_kinds[c->slot_count+1]=XR_RBC_STRING;
        if(c->state_scratch)c->register_kinds[c->slot_count+2]=XR_RBC_STATE;
        if(c->state_ref_scratch)c->register_kinds[c->slot_count+3]=XR_RBC_STATE_REF;
        for(uint32_t a=0;a<XR_RBC_ARRAY_FAMILY_COUNT;++a)
            if(c->array_scratch[a])c->register_kinds[c->slot_count+4+a]=xr_rbc_array_kind_at(a);
    }
    for (uint32_t v = 0; v < 3; ++v) {
        uint32_t offset = c->typed_spills ? c->scratch_count - 3u : 8u;
        if (c->view_scratch[v]) c->register_kinds[c->slot_count + offset + v] =
            (XrRbcRegisterKind)(XR_RBC_VIEW_FRAME + v);
    }
    if(c->reference_spill_count) {
        c->reference_spill_kinds=xr_malloc((size_t)c->reference_spill_count*sizeof(*c->reference_spill_kinds));
        if(!c->reference_spill_kinds)return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot allocate managed spill shapes");
        for(uint32_t v=1;v<=c->view.value_count;++v)
            if(c->spill_home[v]<c->reference_spill_count)
                c->reference_spill_kinds[c->spill_home[v]]=rbc_lir_register_kind(c,v,rbc_lir_value_type(c,v));
    }
    for (uint32_t r = c->slot_count + c->scratch_count; r < c->register_count; ++r)
        c->register_kinds[r] = XR_RBC_CALL_WINDOW;
    return true;
}

static bool rbc_lir_constant(RbcLirCompiler *c, uint32_t slot, uint64_t bits) {
    // Small signed constants use the immediate form without a pool entry.
    if (bits <= INT16_MAX || bits >= UINT64_MAX - 32767)
        return rbc_lir_emit(c, XR_RBC_LOADI, slot, 0, 0,
            bits <= INT16_MAX ? (int64_t) bits : -1 - (int64_t) ~bits);
    uint32_t index;
    return rbc_lir_pool(c, bits, &index) &&
        rbc_lir_emit(c, XR_RBC_LOADK, slot, 0, 0, index);
}

static bool rbc_lir_predicate(RbcLirCompiler *c, const XrLirInstructionView *i) {
    XR_DCHECK(i->operand_count == 2, "integer predicates have two verified operands");
    uint32_t selected = rbc_lir_immediate_operand(c, i);
    if (selected < 2) {
        XrRbcOp op = XR_RBC_EQI;
        bool reverse = selected == 0;
        if (i->predicate == XR_LIR_LT) op = reverse ? XR_RBC_GTI : XR_RBC_LTI;
        if (i->predicate == XR_LIR_LE) op = reverse ? XR_RBC_GEI : XR_RBC_LEI;
        if (i->predicate == XR_LIR_GT) op = reverse ? XR_RBC_LTI : XR_RBC_GTI;
        if (i->predicate == XR_LIR_GE) op = reverse ? XR_RBC_LEI : XR_RBC_GEI;
        return rbc_lir_emit(c, op, c->slots[i->operands[1u - selected]], 0, 0,
            c->immediates[i->operands[selected]].value);
    }
    uint32_t a = c->slots[i->operands[0]], b = c->slots[i->operands[1]];
    XrRbcOp op = XR_RBC_EQ;
    if (i->predicate == XR_LIR_GT || i->predicate == XR_LIR_GE) {
        uint32_t swap = a; a = b; b = swap;
    }
    if (i->predicate == XR_LIR_LT || i->predicate == XR_LIR_GT) op = XR_RBC_LT;
    if (i->predicate == XR_LIR_LE || i->predicate == XR_LIR_GE) op = XR_RBC_LE;
    return rbc_lir_emit(c, op, a, b, 0, 0);
}

static bool rbc_lir_comparison(RbcLirCompiler *c, const XrLirInstructionView *i) {
    bool inverse = i->predicate == XR_LIR_NE;
    uint32_t result = c->slots[i->result];
    return rbc_lir_emit(c, XR_RBC_LOADI, result, 0, 0, inverse ? 1 : 0) &&
        rbc_lir_predicate(c, i) &&
        rbc_lir_emit(c, XR_RBC_JMP, 0, 0, 0, 1) &&
        rbc_lir_emit(c, XR_RBC_JMP, 0, 0, 0, 1) &&
        rbc_lir_emit(c, XR_RBC_LOADI, result, 0, 0, inverse ? 0 : 1);
}

static bool rbc_lir_edge(RbcLirCompiler *c, XrLirEdgeView edge) {
    bool scratch = false;
    return rbc_lir_copies(c, edge, true, &scratch) && rbc_lir_jump(c, edge.target);
}

static bool rbc_lir_conditional(RbcLirCompiler *c, const XrLirInstructionView *i) {
    // A two-operand internal conditional carries its sole-use integer predicate.
    bool comparison = i->operand_count == 2;
    if (comparison) {
        if (!rbc_lir_predicate(c, i)) return false;
    } else {
        if (!rbc_lir_emit(c, XR_RBC_EQI, c->slots[i->operands[0]], 0, 0, 0))
            return false;
    }
    uint32_t taken = comparison && i->predicate != XR_LIR_NE ? 0u : 1u;
    size_t taken_jump = c->code_count;
    if (!rbc_lir_emit(c, XR_RBC_JMP, 0, 0, 0, 0) ||
        !rbc_lir_edge(c, i->edges[1u - taken])) return false;
    size_t taken_start = c->code_count;
    if (!rbc_lir_edge(c, i->edges[taken])) return false;
    if (!xr_rbc_jump(&c->code[taken_jump], taken_jump, taken_start, c->code_count))
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

#include "xrbc_lir_state.inc.c"
#include "xrbc_lir_aggregate_call.inc.c"
#include "xrbc_lir_callable_emit.inc.c"

static bool rbc_lir_instruction(RbcLirCompiler *c, const XrLirInstructionView *i) {
    uint32_t result = i->result ? c->slots[i->result] : 0;
    switch (i->op) {
    case XR_LIR_MODULE_BEGIN: case XR_LIR_MODULE_READY:
    case XR_LIR_STATE_CONSTRUCT: case XR_LIR_STATE_PUBLISH:
    case XR_LIR_STATE_INITIALIZE: case XR_LIR_STATE_REFERENCE:
    case XR_LIR_STATE_READ: case XR_LIR_STATE_WRITE:
        return rbc_lir_state_instruction(c, i);
    case XR_LIR_CONST:
        if (c->immediates[i->result].pooled)
            return rbc_lir_emit(c, XR_RBC_LOADK, result, 0, 0,
                c->immediates[i->result].constant);
        return c->output.lengths[i->result] || rbc_lir_constant(c, result, i->bits);
    case XR_LIR_LITERAL_ADDR:
        return true;
    case XR_LIR_INTCAST:
        if (i->type == XR_LIR_U8) {
            uint32_t temporary = c->slot_count;
            if (c->typed_spills) temporary += c->temporary_strings +
                (uint32_t)rbc_lir_array_temporaries(c, XR_RBC_ARRAY_FAMILY_COUNT) + c->temporary_refs + c->temporary_bits - 1;
            return rbc_lir_emit(c, XR_RBC_LOADI, temporary, 0, 0, UINT8_MAX) &&
                rbc_lir_emit(c, XR_RBC_AND, result, c->slots[i->operands[0]], temporary, 0);
        }
        return rbc_lir_emit(c, XR_RBC_MOV, result, c->slots[i->operands[0]], 0, 0);
    case XR_LIR_ITOF: case XR_LIR_FTOI:
        return rbc_lir_emit(c, i->op == XR_LIR_ITOF ? XR_RBC_ITOF : XR_RBC_FTOI,
            result, c->slots[i->operands[0]], 0, 0);
    case XR_LIR_STRING_LITERAL: {
        uint32_t ordinal;
        return rbc_lir_literal(c, (uint32_t)i->bits, &ordinal) &&
            rbc_lir_emit(c, XR_RBC_STRING_LITERAL, result, 0, 0, ordinal);
    }
    case XR_LIR_STRING_I64: case XR_LIR_STRING_F64: case XR_LIR_STRING_RETAIN: case XR_LIR_STRING_CONCAT:
        return rbc_lir_emit(c, i->op == XR_LIR_STRING_I64 ? XR_RBC_STRING_I64 :
            i->op == XR_LIR_STRING_F64 ? XR_RBC_STRING_F64 :
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
        if (rbc_lir_immediate_operand(c, i) == 1)
            return rbc_lir_emit(c, XR_RBC_FLEK, result, c->slots[i->operands[0]],
                c->immediates[i->operands[1]].constant, 0);
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
    case XR_LIR_FUNCTION_REF: case XR_LIR_CALL_INDIRECT: return rbc_lir_callable_emit(c, i);
    case XR_LIR_ICMP: return rbc_lir_comparison(c, i);
    case XR_LIR_BR: return rbc_lir_edge(c, i->edges[0]);
    case XR_LIR_CONDBR: return rbc_lir_conditional(c, i);
    case XR_LIR_RET:
        {
            XrLirClosureTarget target = {0};
            if (xr_lir_closure_target_view(c->input, c->function, &target) == XR_LIR_OK && target.callable)
                return rbc_lir_aggregate_append(c, (XrRbcAggregateAccess){.operation=XR_RBC_FUNCTION_RETURN,
                    .source=i->operand_count ? c->slots[i->operands[0]] : 0, .count=i->operand_count,
                    .slot=rbc_lir_callable_ordinal(c, target.callable) + 1});
        }
        if (rbc_lir_aggregate_type(c->input, c->view.result))
            return rbc_lir_aggregate_append(c, (XrRbcAggregateAccess){.operation=XR_RBC_AGGREGATE_RETURN,
                .source=c->aggregate_home[i->operands[0]]});
        return rbc_lir_emit(c, i->operand_count ? XR_RBC_RET : XR_RBC_RET0,
            i->operand_count ? c->slots[i->operands[0]] : 0, 0, 0, 0);
    case XR_LIR_CALL: {
        if (rbc_lir_aggregate_call(c->input, i)) return rbc_lir_aggregate_call_emit(c, i);
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
        return !i->result || rbc_lir_array_move(c,
            rbc_lir_register_kind(c,i->result,i->type), result, destination);
    }
    default: {
        uint32_t selected = rbc_lir_immediate_operand(c, i);
        if (selected < i->operand_count) {
            XrRbcOp immediate;
            switch (i->op) {
            case XR_LIR_ADD: immediate = XR_RBC_ADDI; break;
            case XR_LIR_SUB: immediate = XR_RBC_SUBI; break;
            case XR_LIR_MUL: immediate = XR_RBC_MULI; break;
            case XR_LIR_REM: immediate = XR_RBC_REMI; break;
            default: return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "invalid immediate arithmetic");
            }
            return rbc_lir_emit(c, immediate,
                result, c->slots[i->operands[1u - selected]], 0,
                c->immediates[i->operands[selected]].value);
        }
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

typedef struct RbcLirTemporaryUse { uint32_t strings, arrays[XR_RBC_ARRAY_FAMILY_COUNT], references, bits; } RbcLirTemporaryUse;

static uint32_t rbc_lir_stage_slot(RbcLirCompiler *c, uint32_t value, RbcLirTemporaryUse *use) {
    XrLirType type=rbc_lir_value_type(c,value);
    uint32_t arrays=(uint32_t)rbc_lir_array_temporaries(c,XR_RBC_ARRAY_FAMILY_COUNT);
    uint32_t *used = &use->bits, offset = c->temporary_strings + arrays + c->temporary_refs;
    uint32_t capacity = c->temporary_bits;
    if (type == XR_LIR_STRING) {
        used = &use->strings; offset = 0; capacity = c->temporary_strings;
    } else if (type == XR_LIR_ARRAY) {
        uint32_t family=xr_rbc_array_family(rbc_lir_register_kind(c,value,type));
        used=&use->arrays[family];offset=c->temporary_strings+(uint32_t)rbc_lir_array_temporaries(c,family);
        capacity=c->temporary_arrays[family];
    } else if (type == XR_LIR_STATE_REF) {
        used = &use->references; offset = c->temporary_strings+arrays; capacity = c->temporary_refs;
    }
    if (type == XR_LIR_STATE || *used >= capacity) {
        rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "spill exceeds its typed temporary registers");
        return UINT32_MAX;
    }
    return c->slot_count + offset + (*used)++;
}

static bool rbc_lir_spilled_instruction(RbcLirCompiler *c,const XrLirInstructionView *i) {
    if (xr_lir_string_role(c->input, i)) return rbc_lir_string_library(c, i);
    if (xr_lir_display_point(c->input, i)) return rbc_lir_display_emit(c, i);
    if (rbc_lir_aggregate_point(i)) return rbc_lir_aggregate_emit(c, i);
    if (rbc_lir_aggregate_state(c->input, c->function, i)) return rbc_lir_aggregate_state_emit(c, i);
    if (rbc_lir_view_role(c->input, i)) return rbc_lir_view_emit(c, i);
    if(rbc_lir_array_point(c,i))return rbc_lir_array_emit(c,i);
    if (i->op == XR_LIR_CONST &&
        (c->immediates[i->result].known || c->immediates[i->result].pooled) &&
        !c->immediates[i->result].materialize) return true;
    if(!c->spill_count)return rbc_lir_instruction(c,i);
    uint32_t staged[XR_RBC_REGISTER_COUNT_MAX],count=0;
    RbcLirTemporaryUse use = {0};
    uint32_t immediate = rbc_lir_immediate_operand(c, i);
    for(uint32_t a=0;a<i->operand_count;++a) {
        if (a == immediate) continue;
        uint32_t value=i->operands[a];
        if(c->spill_home[value]==UINT32_MAX || c->slots[value]!=UINT32_MAX)continue;
        uint32_t slot=rbc_lir_stage_slot(c,value,&use);
        if(slot==UINT32_MAX)return false;
        if(count==XR_RBC_REGISTER_COUNT_MAX)
            return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"spill operands exceed typed temporary registers");
        if(!rbc_lir_emit(c,XR_RBC_RELOAD,slot,0,0,c->spill_home[value]))return false;
        c->slots[value]=slot;staged[count++]=value;
    }
    bool result=i->result && c->spill_home[i->result]!=UINT32_MAX;
    if(result) {
        c->slots[i->result]=rbc_lir_stage_slot(c,i->result,&use);
        if(c->slots[i->result]==UINT32_MAX)return false;
    }
    bool valid=rbc_lir_instruction(c,i);
    if(valid && result)
        valid=rbc_lir_emit(c,XR_RBC_SPILL,c->slots[i->result],0,0,c->spill_home[i->result]);
    for(uint32_t n=0;n<count;++n) {
        uint32_t value=staged[n];
        if(valid && i->op!=XR_LIR_RET && i->op!=XR_LIR_STRING_DROP && i->op!=XR_LIR_CONDBR)
            valid=rbc_lir_emit(c,XR_RBC_SPILL,c->slots[value],0,0,c->spill_home[value]);
        c->slots[value]=UINT32_MAX;
    }
    if(result)c->slots[i->result]=UINT32_MAX;
    return valid;
}
static size_t rbc_lir_jump_target(const RbcLirCompiler *c, size_t pc) {
    uint32_t bits = c->code[pc] >> 8;
    int32_t offset = bits <= XR_RBC_JUMP_MAX ? (int32_t) bits : (int32_t) bits - 16777216;
    size_t target = pc + 1 + (size_t) offset;
    XR_DCHECK(target < c->code_count, "emitted jumps stay inside their function");
    return target;
}

static bool rbc_lir_thread_jumps(RbcLirCompiler *c) {
    size_t first = 0;
    while (first < c->code_count) {
        if ((c->code[first] & 255) == XR_RBC_JMP &&
            (c->code[rbc_lir_jump_target(c, first)] & 255) == XR_RBC_JMP)
            break;
        ++first;
    }
    if (first == c->code_count)
        return true;
    if (c->code_count > SIZE_MAX / (sizeof(size_t) + sizeof(uint8_t)))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "jump workspace exceeds address space");
    size_t *targets = xr_malloc(c->code_count * (sizeof(*targets) + sizeof(uint8_t)));
    if (!targets)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate jump workspace");
    uint8_t *states = (uint8_t *) (targets + c->code_count);
    memset(states, 0, c->code_count);
    // Resolve each functional-graph path once. A pure jump cycle keeps a
    // representative self-loop, and instruction/source positions stay fixed.
    for (size_t pc = 0; pc < c->code_count; ++pc) {
        if ((c->code[pc] & 255) != XR_RBC_JMP || states[pc])
            continue;
        size_t cursor = pc;
        while ((c->code[cursor] & 255) == XR_RBC_JMP && !states[cursor]) {
            states[cursor] = 1;
            cursor = rbc_lir_jump_target(c, cursor);
        }
        size_t target = states[cursor] == 2 ? targets[cursor] : cursor;
        cursor = pc;
        while ((c->code[cursor] & 255) == XR_RBC_JMP && states[cursor] == 1) {
            size_t next = rbc_lir_jump_target(c, cursor);
            targets[cursor] = target;
            states[cursor] = 2;
            cursor = next;
        }
    }
    for (size_t pc = 0; pc < c->code_count; ++pc) {
        if ((c->code[pc] & 255) != XR_RBC_JMP)
            continue;
        uint32_t word;
        // A chain may span more than one encodable displacement. Keep the
        // original hop whenever the direct edge cannot fit its signed field.
        if (xr_rbc_jump(&word, pc, targets[pc], c->code_count))
            c->code[pc] = word;
    }
    xr_free(targets);
    return true;
}

static bool rbc_lir_paired_jump(uint32_t op) {
    return op == XR_RBC_LT || op == XR_RBC_LE || op == XR_RBC_EQ ||
        (op >= XR_RBC_LTI && op <= XR_RBC_EQI);
}

static bool rbc_lir_compact_jumps(RbcLirCompiler *c) {
    enum { REACHABLE = 1, PAIRED = 2, RETAIN = 4 };
    size_t count = c->code_count;
    if (!count)
        return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "function has no emitted instructions");
    size_t jump = 0;
    while (jump < count && (c->code[jump] & 255) != XR_RBC_JMP) ++jump;
    if (jump == count) return true;
    if (count > SIZE_MAX / (sizeof(size_t) + sizeof(uint8_t)))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "control-flow workspace exceeds address space");
    size_t *positions = xr_malloc(count * (sizeof(*positions) + sizeof(uint8_t)));
    if (!positions)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate control-flow workspace");
    uint8_t *flags = (uint8_t *)(positions + count);
    memset(flags, 0, count);
    // Use the relocation array as a work queue until reachability is complete.
    size_t first = 0, pending = 1;
    positions[0] = 0;
    flags[0] = REACHABLE;
    while (first < pending) {
        size_t pc = positions[first++], successors[2], successors_count = 0;
        uint32_t op = c->code[pc] & 255;
        if (op == XR_RBC_JMP) {
            successors[successors_count++] = rbc_lir_jump_target(c, pc);
        } else if (op != XR_RBC_RET && op != XR_RBC_RET0 &&
            !(op == XR_RBC_AGGREGATE_ACCESS &&
              (c->aggregate_accesses[c->code[pc] >> 16].operation == XR_RBC_AGGREGATE_RETURN ||
               c->aggregate_accesses[c->code[pc] >> 16].operation == XR_RBC_FUNCTION_RETURN))) {
            XR_DCHECK(pc + 1 < count, "emitted fallthrough stays inside its function");
            successors[successors_count++] = pc + 1;
            if (rbc_lir_paired_jump(op)) {
                XR_DCHECK(pc + 2 < count && (c->code[pc + 1] & 255) == XR_RBC_JMP,
                    "comparisons retain their paired jump and skip successor");
                flags[pc + 1] |= PAIRED;
                successors[successors_count++] = pc + 2;
            }
        }
        for (size_t n = 0; n < successors_count; ++n) {
            size_t target = successors[n];
            if (!(flags[target] & REACHABLE)) {
                flags[target] |= REACHABLE;
                positions[pending++] = target;
            }
        }
    }
    size_t next = count, retained = 0;
    for (size_t pc = count; pc-- > 0;) {
        if (!(flags[pc] & REACHABLE)) continue;
        // A paired jump is part of the preceding comparison's encoding even
        // when its taken target coincides with the skip successor.
        if (!(flags[pc] & PAIRED) && (c->code[pc] & 255) == XR_RBC_JMP &&
            rbc_lir_jump_target(c, pc) == next) continue;
        flags[pc] |= RETAIN;
        next = pc;
        ++retained;
    }
    XR_DCHECK(retained, "reachable return or jump cycle survives compaction");
    size_t output = retained;
    for (size_t pc = count; pc-- > 0;) {
        if (flags[pc] & RETAIN) --output;
        positions[pc] = output;
    }
    for (size_t pc = 0; pc < count; ++pc) {
        if (!(flags[pc] & RETAIN)) continue;
        uint32_t word = c->code[pc];
        if ((word & 255) == XR_RBC_JMP) {
            size_t target = rbc_lir_jump_target(c, pc);
            bool fits = xr_rbc_jump(&word, positions[pc], positions[target], retained);
            XR_DCHECK(fits, "compaction cannot increase a jump displacement");
            if (!fits) {
                xr_free(positions);
                return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "compacted branch exceeds jump encoding");
            }
        }
        c->code[positions[pc]] = word;
        c->source_ids[positions[pc]] = c->source_ids[pc];
    }
    c->code_count = retained;
    xr_free(positions);
    return true;
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
    c->scalar_parameter_count = c->view.parameter_count;
    if (!rbc_lir_aggregate_homes(c) || !rbc_lir_immediates(c) || !rbc_lir_layout(c) || !rbc_lir_kinds(c)) return false;
    size_t count = (size_t) c->view.block_count + 1;
    if (count > SIZE_MAX / sizeof(*c->blocks))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "block table exceeds address space");
    c->blocks = xr_malloc(count * sizeof(*c->blocks));
    if (!c->blocks)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate block offsets");
    XrLirBlock first;
    xr_lir_block_at(c->input, c->function, 0, &first);
    if (first.id != c->view.entry && !rbc_lir_jump(c, c->view.entry)) return false;
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
            if (i.op == XR_LIR_ICMP && c->output.uses[i.result] == 1 &&
                n + 1 < bv.instruction_count) {
                XrLirInstructionView branch;
                xr_lir_instruction_view(c->input, block, n + 1, &branch);
                if (branch.op == XR_LIR_CONDBR && branch.operands[0] == i.result) {
                    c->current_source_id = branch.source.path_length ? ++c->source_id : 0;
                    branch.operands = i.operands;
                    branch.operand_count = i.operand_count;
                    branch.predicate = i.predicate;
                    i = branch;
                    ++n;
                }
            }
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
    if (!rbc_lir_thread_jumps(c) || !rbc_lir_compact_jumps(c)) return false;
    XrLirArrayShape array_result={0};
    if(c->view.result==XR_LIR_ARRAY && xr_lir_array_result_view(c->input,c->function,&array_result)!=XR_LIR_OK)
        return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"cannot read owned Array result shape");
    *out = (XrRbcFunction) {.code=c->code, .code_count=c->code_count,
        .constants=c->constants, .constant_count=c->constant_count,
        .register_count=c->register_count, .parameter_count=c->scalar_parameter_count,
        .source_ids=c->source_ids, .source_id_count=c->code_count, .cell_count=c->cell_count,
        .register_kinds=c->register_kinds, .register_kind_count=c->register_count,
        .cell_kinds=c->cell_kinds, .cell_kind_count=c->cell_count,
        .state_accesses=c->state_accesses, .state_access_count=c->state_access_count,
        .array_accesses=c->array_accesses,.array_access_count=c->array_access_count,
        .array_operands=c->array_operands,.array_operand_count=c->array_operand_count,
        .reference_spill_kinds=c->reference_spill_kinds,.reference_spill_kind_count=c->reference_spill_count,
        .result_kind=c->view.result == XR_LIR_STRING ? XR_RBC_STRING : c->view.result==XR_LIR_ARRAY ?
            xr_rbc_array_kind_at(rbc_lir_array_family(array_result.element)) :
            c->view.result == XR_LIR_SLICE_I64 ? XR_RBC_SLICE_I64 : XR_RBC_BITS,
        .view_accesses=c->view_accesses, .view_access_count=c->view_access_count,
        .spill_count=c->spill_count,.reference_spill_count=c->reference_spill_count,
        .aggregate_values=c->aggregate_values, .aggregate_value_count=c->aggregate_value_count,
        .aggregate_size=c->aggregate_size, .aggregate_accesses=c->aggregate_accesses,
        .aggregate_access_count=c->aggregate_access_count, .aggregate_operands=c->aggregate_operands,
        .aggregate_operand_count=c->aggregate_operand_count,
        .aggregate_parameters=c->aggregate_parameters, .aggregate_parameter_count=c->aggregate_parameter_count,
        .aggregate_arguments=c->aggregate_arguments, .aggregate_argument_count=c->aggregate_argument_count,
        .register_callables=c->register_callables,
        .register_callable_count=c->register_callables ? c->register_count : 0,
        .aggregate_result_type=rbc_lir_aggregate_type(c->input, c->view.result) ?
            rbc_lir_aggregate_ordinal(c, c->view.result) + 1 : 0};
    XrLirClosureTarget target = {0};
    if (xr_lir_closure_target_view(c->input, c->function, &target) == XR_LIR_OK && target.callable)
        out->callable = rbc_lir_callable_ordinal(c, target.callable) + 1;
    return true;
}

XR_FUNC void xr_rbc_program_destroy(XrRbcProgram *program) {
    if (!program) return;
    for (size_t f = 0; f < program->module.function_count; ++f) {
        xr_free((void *) program->module.functions[f].code);
        xr_free((void *) program->module.functions[f].constants);
        xr_free((void *) program->module.functions[f].source_ids);
        xr_free((void *) program->module.functions[f].register_kinds);
        xr_free((void *) program->module.functions[f].register_callables);
        xr_free((void *) program->module.functions[f].cell_kinds);
        xr_free((void *) program->module.functions[f].state_accesses);
        xr_free((void *) program->module.functions[f].view_accesses);
        xr_free((void *) program->module.functions[f].array_accesses);
        xr_free((void *) program->module.functions[f].array_operands);
        xr_free((void *) program->module.functions[f].reference_spill_kinds);
        xr_free((void *) program->module.functions[f].aggregate_values);
        xr_free((void *) program->module.functions[f].aggregate_accesses);
        xr_free((void *) program->module.functions[f].aggregate_operands);
        xr_free((void *) program->module.functions[f].aggregate_parameters);
        xr_free((void *) program->module.functions[f].aggregate_arguments);
    }
    xr_free((void *) program->module.functions);
    for (size_t i = 0; i < program->module.literal_count; ++i)
        xr_free((void *)program->module.literals[i].bytes);
    xr_free((void *)program->module.literals);
    xr_free((void *)program->module.aggregate_types);
    xr_free(program->physical_aggregates);
    xr_free(program->physical_callables);
    for (size_t n = 0; n < program->module.callable_type_count; ++n)
        xr_free((void *)program->module.callable_types[n].parameters);
    xr_free((void *)program->module.callable_types);
    xrt_locations_release(program->locations);
    xrt_module_layout_drop(&program->module.state_layout);
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
        xr_free(c.aggregate_home);
        xr_free(c.aggregate_scratch);
        xr_free(c.immediates);
        xr_free(c.view_constants);
        if (!success) {
            xr_free(c.code);
            xr_free(c.constants);
            xr_free(c.source_ids);
            xr_free(c.register_kinds);
            xr_free(c.register_callables);
            xr_free(c.cell_kinds);
            xr_free(c.state_accesses);
            xr_free(c.view_accesses);
            xr_free(c.array_accesses);xr_free(c.array_operands);xr_free(c.reference_spill_kinds);
            xr_free(c.aggregate_values);xr_free(c.aggregate_accesses);xr_free(c.aggregate_operands);
            xr_free(c.aggregate_parameters);xr_free(c.aggregate_arguments);
            xr_free(pool.mapping);
            return c.status;
        }
        if (!f) {
            p->parameter_count = c.view.parameter_count - (p->module.state_layout ? 1u : 0u);
            if (p->parameter_count)
                memcpy(p->parameters, c.view.parameter_types,
                    p->parameter_count * sizeof(*p->parameters));
        }
    }
    xr_free(pool.mapping);
    return XR_RBC_COMPILE_OK;
}

static XrRbcCompileStatus rbc_lir_aggregate_types(const XrLirModule *input,
    XrRbcProgram *program, XrRbcCompileDiagnostic *diagnostic) {
    uint32_t count = xr_lir_constructed_type_count(input);
    if (!count) return XR_RBC_COMPILE_OK;
    uint32_t aggregates = 0;
    for (uint32_t n = 0; n < count; ++n)
        aggregates += rbc_lir_aggregate_type(input, XR_LIR_TYPE_COUNT + n);
    if (aggregates != count) {
        if (sizeof(*program->physical_aggregates) > SIZE_MAX / count)
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_LIMIT,
                "physical type mapping exceeds address space", (XrLirSource){0});
        program->physical_aggregates = xr_malloc((size_t)count * sizeof(*program->physical_aggregates));
        if (!program->physical_aggregates)
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_OUT_OF_MEMORY,
                "cannot receive physical type mapping", (XrLirSource){0});
        uint32_t index = 0;
        for (uint32_t n = 0; n < count; ++n)
            program->physical_aggregates[n] = rbc_lir_aggregate_type(input, XR_LIR_TYPE_COUNT + n) ? index++ : UINT32_MAX;
    }
    if (!aggregates) return XR_RBC_COMPILE_OK;
    if (sizeof(XrPodType) > SIZE_MAX / aggregates)
        return rbc_lir_error(diagnostic, XR_RBC_COMPILE_LIMIT,
            "physical type projection exceeds address space", (XrLirSource){0});
    XrPodType *types = xr_malloc((size_t)aggregates * sizeof(*types));
    if (!types)
        return rbc_lir_error(diagnostic, XR_RBC_COMPILE_OUT_OF_MEMORY,
            "cannot receive physical aggregate types", (XrLirSource){0});
    program->module.aggregate_types = types;
    program->module.aggregate_type_count = aggregates;
    for (uint32_t n = 0; n < count; ++n) {
        uint32_t ordinal = program->physical_aggregates ? program->physical_aggregates[n] : n;
        if (ordinal == UINT32_MAX) continue;
        if (xr_lir_pod_type(input, XR_LIR_TYPE_COUNT + n, &types[ordinal]) != XR_LIR_OK)
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_UNSUPPORTED,
                "physical aggregate type has no bytecode projection", (XrLirSource){0});
        if (program->physical_aggregates && types[ordinal].kind == XR_POD_NULLABLE &&
            types[ordinal].element >= XR_POD_SCALAR_COUNT) {
            uint32_t child = types[ordinal].element - XR_POD_SCALAR_COUNT;
            if (child >= count || program->physical_aggregates[child] == UINT32_MAX)
                return rbc_lir_error(diagnostic, XR_RBC_COMPILE_INVALID,
                    "aggregate dependency is not an inline value", (XrLirSource){0});
            types[ordinal].element = XR_POD_SCALAR_COUNT + program->physical_aggregates[child];
        }
        if (!xr_rbc_aggregate_type_valid(&program->module, ordinal))
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_INVALID,
                "physical aggregate descriptor differs from its owned layout", (XrLirSource){0});
        XrRbcAggregateLayout layout;
        if (!xr_rbc_aggregate_layout(types[ordinal], &layout))
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_UNSUPPORTED,
                "physical aggregate layout has no bytecode projection", (XrLirSource){0});
    }
    return XR_RBC_COMPILE_OK;
}

#include "xrbc_lir_callable_types.inc.c"

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
#if defined(XR_XIR_PARANOID) && XR_XIR_PARANOID
    XrLirDiagnostic invalid;
    XrLirStatus verified = xr_lir_verify(input, &invalid);
    if (verified != XR_LIR_OK) {
        d->function = invalid.function;
        d->block = invalid.block;
        d->instruction = invalid.instruction;
        return rbc_lir_error(d, verified == XR_LIR_OUT_OF_MEMORY ?
            XR_RBC_COMPILE_OUT_OF_MEMORY : XR_RBC_COMPILE_INVALID, invalid.message, invalid.source);
    }
#endif
    if (!lir_module_has_builtin_abi(input))
        return rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
            "constructed physical types have no bytecode implementation", (XrLirSource) {0});
    for (uint32_t i = 0; i < root.parameter_count; ++i)
        if (root.parameter_types[i] >= XR_LIR_TYPE_COUNT ||
            root.parameter_types[i] == XR_LIR_I64_CELL || root.parameter_types[i] == XR_LIR_STRING_CELL ||
            root.parameter_types[i] == XR_LIR_STRING || root.parameter_types[i]==XR_LIR_ARRAY ||
            root.parameter_types[i]==XR_LIR_ARRAY_CELL || root.parameter_types[i]==XR_LIR_ARRAY_REF ||
            root.parameter_types[i]==XR_LIR_STRING_REF)
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
    status = rbc_lir_aggregate_types(input, p, d);
    if (status != XR_RBC_COMPILE_OK) goto done;
    status = rbc_lir_callable_types(input, p, d);
    if (status != XR_RBC_COMPILE_OK) goto done;
    status = rbc_lir_state_layout(input, entry, p, d);
    if (status != XR_RBC_COMPILE_OK) goto done;
    if (root.parameter_count && root.parameter_types[root.parameter_count - 1] == XR_LIR_STATE &&
        !p->module.state_layout) {
        status = rbc_lir_error(d, XR_RBC_COMPILE_UNSUPPORTED,
            "only the designated entry receives module state", (XrLirSource){0});
        goto done;
    }
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
        else if (p->parameters[i] == XR_LIR_U8 && arguments[i] > UINT8_MAX) return XR_RBC_INVALID;
    return xr_rbc_module_prepare(&p->module, 0, arguments, count, p->locations, output);
}

XR_FUNC const XrRbcModule *xr_rbc_program_view(const XrRbcProgram *program) {
    return program ? &program->module : NULL;
}

XR_FUNC XrtLocationOwner *xr_rbc_program_locations(const XrRbcProgram *program) {
    return program ? program->locations : NULL;
}
