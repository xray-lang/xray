/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c.c - Direct scalar C calls and parallel SSA edge copies
 *
 * KEY CONCEPT:
 *   Wrapping arithmetic widens to unsigned before operations. Signed
 *   reconstruction uses only representable conversions. I64 division guards
 *   possible zero through the located runtime panic boundary, then selects
 *   the wrapping MIN/-1 result before evaluating signed C operators.
 *   No interpreter frame is emitted.
 */
#include "xlir_c.h"
#include "xlir_verify.h"
#include "xlir_internal.h"
#include "../aot/xi_cgen_verify_output.h"
#include "../base/xchecks.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct LirC {
    const XrLirCRequest *request;
    XrLirCDiagnostic *diagnostic;
    XrLirCStatus status;
    char *text;
    size_t length, capacity;
    uint32_t *closure;
    uint32_t closure_count;
    uint8_t *seen, *fixed_divisors, *used_literals;
    XrLirFunction function;
    XrLirFunctionView view;
    uint32_t block, instruction, scratch;
    XrLirSource source;
    uint32_t location_count, location_cursor;
    bool runtime, f64, fenv;
} LirC;

static void c_error(LirC *c, XrLirCStatus status, const char *message) {
    if (c->status != XR_LIR_C_OK)
        return;
    c->status = status;
    if (c->diagnostic) {
        *c->diagnostic = (XrLirCDiagnostic) {.status = status,
            .function = c->function.id, .block = c->block, .instruction = c->instruction,
            .source = c->source};
        snprintf(c->diagnostic->message, sizeof(c->diagnostic->message), "%s", message);
    }
}
static bool c_resource(LirC *c, XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK)
        return c->status == XR_LIR_C_OK;
    XrLirCStatus mapped;
    switch (status) {
        case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: mapped = XR_LIR_C_OUT_OF_MEMORY; break;
        case XR_COMPILE_RESOURCE_MEMORY_LIMIT: mapped = XR_LIR_C_MEMORY_LIMIT; break;
        case XR_COMPILE_RESOURCE_OVERFLOW: mapped = XR_LIR_C_OVERFLOW; break;
        case XR_COMPILE_RESOURCE_CANCELLED: mapped = XR_LIR_C_CANCELLED; break;
        case XR_COMPILE_RESOURCE_DEADLINE: mapped = XR_LIR_C_DEADLINE; break;
        case XR_COMPILE_RESOURCE_CLOCK_FAILURE: mapped = XR_LIR_C_CLOCK_FAILURE; break;
        default: mapped = XR_LIR_C_BAD_ARGUMENT; break;
    }
    c_error(c, mapped, "C emission resource operation failed");
    return false;
}
static bool c_request_poll(LirC *c) {
    if (c->status != XR_LIR_C_OK)
        return false;
    return !c->request->control ||
        c_resource(c, xr_compile_request_control_poll(c->request->control));
}
static void c_bytes(LirC *c, const char *bytes, size_t length) {
    if (c->status != XR_LIR_C_OK)
        return;
    if (length > SIZE_MAX - c->length - 1) {
        c_error(c, XR_LIR_C_OVERFLOW, "C text exceeds machine capacity");
        return;
    }
    size_t required = c->length + length + 1;
    if (required > c->capacity) {
        size_t grown = c->capacity ? c->capacity : 1024;
        while (grown < required && grown <= SIZE_MAX / 2)
            grown *= 2;
        if (grown < required)
            grown = required;
        void *memory = c->text;
        if (!c_resource(c, xr_compile_resources_resize(c->request->resources, &memory, grown)))
            return;
        c->text = memory;
        c->capacity = grown;
    }
    XR_DCHECK(c->capacity >= required, "owned text contains the appended bytes and terminator");
    memcpy(c->text + c->length, bytes, length);
    c->length += length;
    c->text[c->length] = '\0';
}
static void c_text(LirC *c, const char *text) {
    c_bytes(c, text, strlen(text));
}
static void c_format(LirC *c, const char *format, ...) XR_PRINTF_FMT(2, 3);
static void c_format(LirC *c, const char *format, ...) {
    if (c->status != XR_LIR_C_OK)
        return;
    char buffer[256];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t) length >= sizeof(buffer)) {
        c_error(c, XR_LIR_C_BUDGET, "C formatting capacity exceeded");
        return;
    }
    c_bytes(c, buffer, (size_t) length);
}
static const char *c_type(XrLirType type) {
    static const char *const types[] = {NULL, "void", "_Bool", "int8_t", "int16_t",
        "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        NULL, "double", "void *", "int64_t *", "XrtString *", "XrtString **"};
    return (unsigned) type < sizeof(types) / sizeof(types[0]) ? types[type] : NULL;
}
static unsigned c_width(XrLirType type) {
    XR_DCHECK(type >= XR_LIR_I8 && type <= XR_LIR_U64, "integer has a fixed width");
    unsigned ordinal = type <= XR_LIR_I64 ? (unsigned) (type - XR_LIR_I8)
        : (unsigned) (type - XR_LIR_U8);
    return 8u << ordinal;
}
static uint64_t c_mask(XrLirType type) {
    unsigned width = c_width(type);
    return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}
static void c_function_name(LirC *c, uint32_t id) {
    if (id == c->request->entry.id && !c->runtime)
        c_text(c, "xray_lir_entry");
    else
        c_format(c, "xrl_f%" PRIu32, id);
}
static void c_value(LirC *c, uint32_t id) {
    XR_DCHECK(id && id <= c->view.value_count, "operand belongs to this function");
    for (uint32_t p = 0; p < c->view.parameter_count; ++p) {
        if (c->view.parameters[p] == id) {
            c_format(c, "p%" PRIu32, p);
            return;
        }
    }
    c_format(c, "v%" PRIu32, id);
}
static XrLirType c_value_type(LirC *c, uint32_t id) {
    XrLirType type = XR_LIR_TYPE_INVALID;
    XrLirValue value = {c->request->module, c->function.id, id};
    XrLirStatus status = xr_lir_value_type(c->request->module, value, &type);
    XR_CHECK(status == XR_LIR_OK, "validated C operand has a type");
    return type;
}
static bool c_scalar(LirC *c, XrLirType type) {
    if (!c_type(type))
        c_error(c, XR_LIR_C_UNSUPPORTED, "C backend requires integer, bool, unit, f64 transport or data pointer types");
    c->f64 |= type == XR_LIR_F64;
    c->runtime |= type == XR_LIR_STRING || type == XR_LIR_STRING_CELL;
    return c->status == XR_LIR_C_OK;
}
static bool c_runtime_call(LirC *c, const XrLirInstructionView *v) {
    if (v->op != XR_LIR_CALL)
        return false;
    XrLirFunction callee = {c->request->module, v->callee};
    XrLirFunctionView view;
    xr_lir_function_view(c->request->module, callee, &view);
    return view.kind == XR_LIR_FUNCTION_EXTERNAL;
}
static bool c_string_point(LirC *c, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_STRING_LITERAL || v->op == XR_LIR_STRING_I64 || v->op == XR_LIR_STRING_F64 ||
        v->op == XR_LIR_STRING_CONCAT || v->op == XR_LIR_STRING_RETAIN ||
        v->op == XR_LIR_STRING_PRINT || v->op == XR_LIR_STRING_LEN) return true;
    return (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_STRING_CELL) ||
        (v->op == XR_LIR_CELL_READ && v->type == XR_LIR_STRING) ||
        (v->op == XR_LIR_CELL_WRITE && c_value_type(c, v->operands[0]) == XR_LIR_STRING_CELL);
}
static bool c_located(const XrLirSource *source) {
    return source->path && source->path_length && source->line && source->column;
}
static void c_visit(LirC *c, uint32_t id) {
    XR_DCHECK(id && id <= xr_lir_function_count(c->request->module), "valid callee");
    if (!c->seen[id - 1]) {
        XR_DCHECK(c->closure_count < xr_lir_function_count(c->request->module),
                  "each closure function enters the queue once");
        c->seen[id - 1] = 1;
        c->closure[c->closure_count++] = id;
    }
}
static bool c_has_division(LirC *c) {
    for (uint32_t b = 0; b < c->view.block_count && c->status == XR_LIR_C_OK; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->request->module, c->function, b, &block);
        xr_lir_block_view(c->request->module, block, &view);
        c->block = block.id;
        if (!c_request_poll(c))
            return false;
        for (uint32_t i = 0; i < view.instruction_count && c->status == XR_LIR_C_OK; ++i) {
            XrLirInstructionView v;
            xr_lir_instruction_view(c->request->module, block, i, &v);
            if (v.op == XR_LIR_DIV || v.op == XR_LIR_REM)
                return true;
        }
    }
    return false;
}
/* SSA verification already supplies unique definitions and dominance. Facts
 * are local to this function and never inferred through aliases or edges. */
static void c_prepare_divisors(LirC *c) {
    c->block = c->instruction = 0;
    c->source = (XrLirSource) {0};
    xr_compile_resources_free(c->fixed_divisors);
    c->fixed_divisors = NULL;
    if (!c_has_division(c) || c->status != XR_LIR_C_OK)
        return;
    void *facts = NULL;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources,
                                                  (size_t)c->view.value_count + 1, 1, &facts)))
        return;
    c->fixed_divisors = facts;
    for (uint32_t b = 0; b < c->view.block_count && c->status == XR_LIR_C_OK; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->request->module, c->function, b, &block);
        xr_lir_block_view(c->request->module, block, &view);
        c->block = block.id;
        if (!c_request_poll(c))
            return;
        for (uint32_t i = 0; i < view.instruction_count && c->status == XR_LIR_C_OK; ++i) {
            XrLirInstructionView v;
            xr_lir_instruction_view(c->request->module, block, i, &v);
            if (v.op == XR_LIR_CONST && v.type == XR_LIR_I64 && v.bits)
                c->fixed_divisors[v.result] = 1;
        }
    }
}
static bool c_arithmetic_point(const LirC *c, const XrLirInstructionView *v) {
    return (v->op == XR_LIR_DIV || v->op == XR_LIR_REM) &&
        c->fixed_divisors && !c->fixed_divisors[v->operands[1]];
}
static void c_admit_instruction(LirC *c, const XrLirInstructionView *v) {
    if (!c_request_poll(c))
        return;
    c_scalar(c, v->type);
    if (c_string_point(c, v)) {
        c->runtime = true;
        if (c_located(&v->source)) {
            if (c->location_count == UINT32_MAX)
                c_error(c, XR_LIR_C_UNSUPPORTED, "source map cannot represent another nonzero ID");
            else ++c->location_count;
        }
    }
    switch (v->op) {
        case XR_LIR_CELL_NEW: case XR_LIR_CELL_READ: case XR_LIR_CELL_WRITE:
        case XR_LIR_STRING_I64: case XR_LIR_STRING_F64: case XR_LIR_STRING_CONCAT: case XR_LIR_STRING_RETAIN:
        case XR_LIR_STRING_DROP: case XR_LIR_STRING_CELL_DROP: case XR_LIR_STRING_PRINT:
        case XR_LIR_STRING_LEN:
            break;
        case XR_LIR_LITERAL_ADDR: case XR_LIR_STRING_LITERAL:
            c->used_literals[v->bits - 1] = 1;
            break;
        case XR_LIR_CONST: case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL:
        case XR_LIR_NEG: case XR_LIR_ICMP: case XR_LIR_INTCAST:
        case XR_LIR_BR: case XR_LIR_CONDBR: case XR_LIR_RET:
            break;
        case XR_LIR_DIV: case XR_LIR_REM:
            if (v->type != XR_LIR_I64)
                c_error(c, XR_LIR_C_UNSUPPORTED, "division requires i64 operands");
            else if (c_arithmetic_point(c, v)) {
                c->runtime = true;
                if (c_located(&v->source)) {
                    if (c->location_count == UINT32_MAX)
                        c_error(c, XR_LIR_C_OVERFLOW, "source location count exceeds 32 bits");
                    else
                        ++c->location_count;
                }
            }
            break;
        case XR_LIR_FNEG:
            if (v->type == XR_LIR_F64)
                c->fenv = true;
            else
                c_error(c, XR_LIR_C_UNSUPPORTED, "floating negation requires f64");
            break;
        case XR_LIR_FADD: case XR_LIR_FSUB: case XR_LIR_FMUL: case XR_LIR_FDIV:
        case XR_LIR_FCMP:
            if (c_value_type(c,v->operands[0]) != XR_LIR_F64 ||
                c_value_type(c,v->operands[1]) != XR_LIR_F64)
                c_error(c,XR_LIR_C_UNSUPPORTED,"floating operation requires f64 operands");
            else {
                c->runtime=true;
                c->fenv=true;
            }
            break;
        case XR_LIR_CALL:
            c_visit(c, v->callee);
            if (c_runtime_call(c, v) && c_located(&v->source)) {
                if (c->location_count == UINT32_MAX)
                    c_error(c, XR_LIR_C_UNSUPPORTED, "source map cannot represent another nonzero ID");
                else
                    ++c->location_count;
            }
            break;
        default:
            c_error(c, XR_LIR_C_UNSUPPORTED, "operation has no admitted portable C implementation");
            break;
    }
}
static void c_admit(LirC *c) {
    uint32_t count = xr_lir_function_count(c->request->module);
    if ((uint64_t) count * sizeof(uint32_t) > SIZE_MAX) {
        c_error(c, XR_LIR_C_OVERFLOW, "C function closure exceeds machine capacity");
        return;
    }
    void *queue = NULL, *seen = NULL;
    if (!c_resource(c, xr_compile_resources_alloc(c->request->resources,
                                                  (size_t) count * sizeof(uint32_t), &queue)))
        return;
    c->closure = queue;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources, count, 1, &seen)))
        return;
    c->seen = seen;
    c_visit(c, c->request->entry.id);
    for (uint32_t cursor = 0; cursor < c->closure_count && c->status == XR_LIR_C_OK; ++cursor) {
        c->block = c->instruction = 0;
        c->source = (XrLirSource) {0};
        c->function = (XrLirFunction) {c->request->module, c->closure[cursor]};
        xr_lir_function_view(c->request->module, c->function, &c->view);
        if (!c_request_poll(c))
            return;
        if (c->view.kind == XR_LIR_FUNCTION_EXTERNAL) {
            c->runtime = true;
            continue;
        }
        c_scalar(c, c->view.result);
        for (uint32_t p = 0; p < c->view.parameter_count && c->status == XR_LIR_C_OK; ++p)
            c_scalar(c, c->view.parameter_types[p]);
        if (c->view.value_count >= UINT32_C(8388607))
            c_error(c, XR_LIR_C_UNSUPPORTED, "C temporary index exceeds the W4 limit");
        if (c->status == XR_LIR_C_OK)
            c_prepare_divisors(c);
        for (uint32_t b = 0; b < c->view.block_count && c->status == XR_LIR_C_OK; ++b) {
            XrLirBlock block;
            XrLirBlockView view;
            xr_lir_block_at(c->request->module, c->function, b, &block);
            xr_lir_block_view(c->request->module, block, &view);
            c->block = block.id;
            if (!c_request_poll(c))
                return;
            for (uint32_t p = 0; p < view.parameter_count && c->status == XR_LIR_C_OK; ++p)
                c_scalar(c, c_value_type(c, view.parameters[p]));
            for (uint32_t i = 0; i < view.instruction_count && c->status == XR_LIR_C_OK; ++i) {
                XrLirInstructionView v;
                xr_lir_instruction_view(c->request->module, block, i, &v);
                c->instruction = i + 1;
                c->source = v.source;
                c_admit_instruction(c, &v);
            }
        }
    }
}
static void c_path(LirC *c, const XrLirSource *source) {
    c_text(c, "\"");
    for (size_t n = 0; n < source->path_length && c->status == XR_LIR_C_OK; ++n) {
        unsigned char byte = (unsigned char) source->path[n];
        if (byte == '"' || byte == '\\')
            c_format(c, "\\%c", byte);
        else if (byte < 32 || byte >= 127)
            c_format(c, "\\%03o", (unsigned) byte);
        else
            c_bytes(c, source->path + n, 1);
    }
    c_text(c, "\"");
}
static void c_locations(LirC *c) {
    if (!c->runtime)
        return;
    uint32_t id = 0;
    if (c->location_count)
        c_text(c, "static const XrtSourceLocation xrl_locations[] = {\n");
    for (uint32_t n = 0; n < c->closure_count && c->status == XR_LIR_C_OK; ++n) {
        c->function = (XrLirFunction) {c->request->module, c->closure[n]};
        xr_lir_function_view(c->request->module, c->function, &c->view);
        if (!c_request_poll(c))
            return;
        c_prepare_divisors(c);
        for (uint32_t b = 0; b < c->view.block_count && c->status == XR_LIR_C_OK; ++b) {
            XrLirBlock block;
            XrLirBlockView view;
            xr_lir_block_at(c->request->module, c->function, b, &block);
            xr_lir_block_view(c->request->module, block, &view);
            c->block = block.id;
            if (!c_request_poll(c))
                return;
            for (uint32_t i = 0; i < view.instruction_count && c->status == XR_LIR_C_OK; ++i) {
                XrLirInstructionView v;
                xr_lir_instruction_view(c->request->module, block, i, &v);
                if ((!c_runtime_call(c, &v) && !c_string_point(c, &v) &&
                     !c_arithmetic_point(c, &v)) || !c_located(&v.source))
                    continue;
                ++id;
                c_text(c, "    { ");
                c_path(c, &v.source);
                c_format(c, ", %zu, UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32
                    "), UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 ") },\n",
                    v.source.path_length, v.source.line, v.source.column,
                    v.source.end_line, v.source.end_column);
            }
        }
    }
    if (c->location_count)
        c_text(c, "};\n");
    c_text(c, "const XrtSourceMap xrt_standalone_source_map = { ");
    c_format(c, "%s, UINT32_C(%" PRIu32 ") };\n\n",
        c->location_count ? "xrl_locations" : "NULL", c->location_count);
    (void) id;
    XR_DCHECK(c->status != XR_LIR_C_OK || id == c->location_count,
              "static source locations match admitted calls");
}
static void c_signature(LirC *c, bool definition) {
    XR_DCHECK(c_type(c->view.result) != NULL, "function result passed scalar admission");
    if (c->runtime || c->function.id != c->request->entry.id)
        c_text(c, "static ");
    c_format(c, "%s ", c_type(c->view.result));
    c_function_name(c, c->function.id);
    c_text(c, "(");
    if (!c->view.parameter_count)
        c_text(c, "void");
    for (uint32_t p = 0; p < c->view.parameter_count && c->status == XR_LIR_C_OK; ++p) {
        if (p)
            c_text(c, ", ");
        c_format(c, "%s p%" PRIu32, c_type(c->view.parameter_types[p]), p);
    }
    c_text(c, definition ? ") {\n" : ");\n");
}
static bool c_literal_value(LirC *c, uint32_t id) {
    const XrLirFunctionData *function = &c->request->module->functions[c->function.id - 1];
    const XrLirInstructionView *definition = xr_lir_internal_value_definition(function, id);
    return definition && definition->op == XR_LIR_LITERAL_ADDR;
}
static void c_declarations(LirC *c) {
    for (uint32_t id = 1; id <= c->view.value_count && c->status == XR_LIR_C_OK; ++id) {
        if (!c_request_poll(c))
            return;
        bool parameter = false;
        for (uint32_t p = 0; p < c->view.parameter_count && c->status == XR_LIR_C_OK; ++p)
            parameter |= c->view.parameters[p] == id;
        if (!parameter) {
            c_format(c, "    %s v%" PRIu32 ";\n", c_literal_value(c, id) ? "const void *" : c_type(c_value_type(c, id)), id);
            c_format(c, "    (void)&v%" PRIu32 ";\n", id);
        }
    }
    /* Unsigned expression storage is shared only across sequential instructions. */
    c->scratch = c->view.value_count + 1;
    XR_DCHECK(c->scratch < UINT32_C(8388608), "expression storage remains visible to W4");
    c_format(c, "    uint64_t v%" PRIu32 ";\n", c->scratch);
    c_format(c, "    (void)&v%" PRIu32 ";\n", c->scratch);
    for (uint32_t p = 0; p < c->view.parameter_count && c->status == XR_LIR_C_OK; ++p)
        c_format(c, "    (void)p%" PRIu32 ";\n", p);
}
static void c_reconstruct(LirC *c, XrLirType type, uint32_t result) {
    c_format(c, "    v%" PRIu32 " = ", result);
    if (type >= XR_LIR_I8 && type <= XR_LIR_I64) {
        uint64_t maximum = c_mask(type) >> 1;
        c_format(c, "v%" PRIu32 " <= UINT64_C(%" PRIu64 ") ? (%s)v%" PRIu32
            " : (%s)(-INT64_C(1) - (int64_t)((~v%" PRIu32 ") & UINT64_C(%" PRIu64 ")));\n",
            c->scratch, maximum, c_type(type), c->scratch, c_type(type), c->scratch, c_mask(type));
    } else {
        c_format(c, "(%s)v%" PRIu32 ";\n", c_type(type), c->scratch);
    }
}
static void c_integer(LirC *c, const XrLirInstructionView *v) {
    if (v->type == XR_LIR_I1) {
        c_format(c, "    v%" PRIu32 " = %u;\n", v->result, (unsigned) v->bits);
        return;
    }
    c_format(c, "    v%" PRIu32 " = (", c->scratch);
    if (v->op == XR_LIR_CONST) {
        c_format(c, "UINT64_C(%" PRIu64 ")", v->bits);
    } else {
        if (v->op == XR_LIR_NEG)
            c_text(c, "UINT64_C(0) - ");
        c_text(c, "(uint64_t)");
        c_value(c, v->operands[0]);
        if (v->operand_count == 2) {
            c_text(c, v->op == XR_LIR_ADD ? " + " : v->op == XR_LIR_SUB ? " - " : " * ");
            c_text(c, "(uint64_t)");
            c_value(c, v->operands[1]);
        }
    }
    c_format(c, ") & UINT64_C(%" PRIu64 ");\n", c_mask(v->type));
    c_reconstruct(c, v->type, v->result);
}
/* Declared environment access prevents an integer NaN test from becoming a
 * floating comparison that raises invalid for a signaling payload. */
static void c_fneg(LirC *c, const XrLirInstructionView *v) {
    c_format(c, "    memcpy(&v%" PRIu32 ", &", c->scratch);
    c_value(c, v->operands[0]);
    c_format(c, ", sizeof(v%" PRIu32 "));\n", c->scratch);
    c_format(c, "    v%" PRIu32 " = ((v%" PRIu32 " & UINT64_C(0x7ff0000000000000)) == "
        "UINT64_C(0x7ff0000000000000) && (v%" PRIu32 " & UINT64_C(0x000fffffffffffff)) != 0)\n",
        c->scratch, c->scratch, c->scratch);
    c_format(c, "        ? UINT64_C(0x7ff8000000000000) : v%" PRIu32 " ^ UINT64_C(0x8000000000000000);\n",
        c->scratch);
    c_format(c, "    memcpy(&v%" PRIu32 ", &v%" PRIu32 ", sizeof(v%" PRIu32 "));\n",
        v->result, c->scratch, v->result);
}
static void c_float_operation(LirC *c, const XrLirInstructionView *v) {
    c_text(c,"    { uint64_t xrl_left, xrl_right;\n        memcpy(&xrl_left, &");
    c_value(c,v->operands[0]);c_text(c,", sizeof(xrl_left));\n        memcpy(&xrl_right, &");
    c_value(c,v->operands[1]);c_text(c,", sizeof(xrl_right));\n");
    if (v->op==XR_LIR_FCMP) {
        c_text(c,"        int32_t xrl_order = xrt_f64_compare(xrl_left, xrl_right);\n");
        c_format(c,"        v%" PRIu32 " = ",v->result);
        switch (v->predicate) {
            case XR_LIR_EQ:c_text(c,"xrl_order == INT32_C(0)");break;
            case XR_LIR_NE:c_text(c,"xrl_order != INT32_C(0)");break;
            case XR_LIR_LT:c_text(c,"xrl_order == -INT32_C(1)");break;
            case XR_LIR_LE:c_text(c,"xrl_order == -INT32_C(1) || xrl_order == INT32_C(0)");break;
            case XR_LIR_GT:c_text(c,"xrl_order == INT32_C(1)");break;
            case XR_LIR_GE:c_text(c,"xrl_order == INT32_C(1) || xrl_order == INT32_C(0)");break;
            default:c_error(c,XR_LIR_C_INVALID_LIR,"invalid floating predicate");return;
        }
        c_text(c,";\n");
    } else {
        const char *name=v->op==XR_LIR_FADD?"xrt_f64_add":v->op==XR_LIR_FSUB?"xrt_f64_subtract":
            v->op==XR_LIR_FMUL?"xrt_f64_multiply":"xrt_f64_divide";
        c_format(c,"        uint64_t xrl_bits = %s(xrl_left, xrl_right);\n        memcpy(&",name);
        c_value(c,v->result);c_text(c,", &xrl_bits, sizeof(xrl_bits));\n");
    }
    c_text(c,"    }\n");
}
static void c_edge(LirC *c, const XrLirEdgeView *edge) {
    XrLirBlock target = {c->request->module, c->function.id, edge->target};
    XrLirBlockView block;
    xr_lir_block_view(c->request->module, target, &block);
    XR_DCHECK(block.parameter_count == edge->argument_count, "validated edge matches its target");
    c_text(c, "    {\n");
    for (uint32_t a = 0; a < edge->argument_count && c->status == XR_LIR_C_OK; ++a) {
        uint64_t temp = (uint64_t) c->scratch + 1 + a;
        if (temp >= UINT32_C(8388608)) {
            c_error(c, XR_LIR_C_UNSUPPORTED, "C edge temporary index exceeds the W4 limit");
            return;
        }
        c_format(c, "        %s v%" PRIu64 " = ", c_type(c_value_type(c, edge->arguments[a])), temp);
        c_value(c, edge->arguments[a]);
        c_text(c, ";\n");
    }
    for (uint32_t a = 0; a < edge->argument_count && c->status == XR_LIR_C_OK; ++a)
        if (c_value_type(c, edge->arguments[a]) == XR_LIR_STRING) {
            c_text(c, "        "); c_value(c, edge->arguments[a]); c_text(c, " = NULL;\n");
        }
    for (uint32_t a = 0; a < edge->argument_count && c->status == XR_LIR_C_OK; ++a)
        c_format(c, "        v%" PRIu32 " = v%" PRIu32 ";\n", block.parameters[a],
                 c->scratch + 1 + a);
    c_format(c, "        goto bb%" PRIu32 ";\n    }\n", edge->target);
}
static void c_division(LirC *c, const XrLirInstructionView *v) {
    bool guarded = c_arithmetic_point(c, v);
    if (guarded) {
        uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
        /* One volatile read keeps host constant propagation from diagnosing
         * guest code after a known panic as unreachable. The check and the
         * operation share the same captured value. */
        c_text(c, "    { volatile int64_t xrl_guard = ");
        c_value(c, v->operands[1]);
        c_text(c, ";\n    int64_t xrl_divisor = xrl_guard;\n");
        c_format(c, "    if (xrl_divisor == INT64_C(0)) "
            "xrt_arithmetic_panic(UINT32_C(%u), UINT32_C(%" PRIu32 "));\n",
            v->op == XR_LIR_DIV ? 420u : 421u, source);
    }
    /* Select the wrapping pair before C can evaluate signed overflow. */
    c_format(c, "    v%" PRIu32 " = ", v->result);
    c_value(c, v->operands[0]);
    c_text(c, " == INT64_MIN && ");
    if (guarded)
        c_text(c, "xrl_divisor");
    else
        c_value(c, v->operands[1]);
    c_text(c, " == -INT64_C(1) ? ");
    c_text(c, v->op == XR_LIR_DIV ? "INT64_MIN" : "INT64_C(0)");
    c_text(c, " : ");
    c_value(c, v->operands[0]);
    c_text(c, v->op == XR_LIR_DIV ? " / " : " % ");
    if (guarded)
        c_text(c, "xrl_divisor");
    else
        c_value(c, v->operands[1]);
    c_text(c, guarded ? "; }\n" : ";\n");
}
static void c_instruction(LirC *c, const XrLirInstructionView *v) {
    if (!c_request_poll(c))
        return;
    static const char *const predicates[] = {NULL, "==", "!=", "<", "<=", ">", ">="};
    uint32_t string_source = c_string_point(c, v) && c_located(&v->source) ? ++c->location_cursor : 0;
    switch (v->op) {
        case XR_LIR_STRING_LITERAL: {
            XrLirLiteral literal = {c->request->module, (uint32_t)v->bits};
            const uint8_t *bytes = NULL;
            size_t length = 0;
            xr_lir_literal_view(c->request->module, literal, &bytes, &length);
            c_format(c, "    v%" PRIu32 " = xrt_string_new((const char *)xrl_literal%" PRIu64 ", %zu, UINT32_C(%" PRIu32 "));\n",
                v->result, v->bits, length, string_source);
            break;
        }
        case XR_LIR_STRING_F64:
            c_text(c, "    { uint64_t xrl_bits; memcpy(&xrl_bits, &");
            c_value(c, v->operands[0]);
            c_format(c, ", sizeof(xrl_bits));\n        v%" PRIu32
                " = xrt_string_f64(xrl_bits, UINT32_C(%" PRIu32 "));\n    }\n", v->result, string_source);
            break;
        case XR_LIR_STRING_I64: case XR_LIR_STRING_RETAIN: case XR_LIR_STRING_CONCAT:
            c_format(c, "    v%" PRIu32 " = %s(", v->result,
                v->op == XR_LIR_STRING_I64 ? "xrt_string_i64" :
                v->op == XR_LIR_STRING_RETAIN ? "xrt_string_retain" : "xrt_string_concat");
            c_value(c, v->operands[0]);
            if (v->operand_count == 2) { c_text(c, ", "); c_value(c, v->operands[1]); }
            c_format(c, ", UINT32_C(%" PRIu32 "));\n", string_source);
            break;
        case XR_LIR_STRING_LEN:
            c_format(c, "    v%" PRIu32 " = xrt_string_length(", v->result);
            c_value(c, v->operands[0]);
            c_format(c, ", UINT32_C(%" PRIu32 "));\n", string_source);
            break;
        case XR_LIR_STRING_DROP:
            c_text(c, "    xrt_string_drop(&"); c_value(c, v->operands[0]); c_text(c, ");\n");
            break;
        case XR_LIR_STRING_CELL_DROP:
            c_text(c, "    xrt_string_drop("); c_value(c, v->operands[0]); c_text(c, ");\n");
            break;
        case XR_LIR_STRING_PRINT:
            c_text(c, "    { size_t length; const char *bytes = xrt_string_bytes(");
            c_value(c, v->operands[0]);
            c_format(c, ", &length); xrt_print_string(bytes, length, UINT32_C(%" PRIu32 ")); }\n", string_source);
            break;
        case XR_LIR_CELL_NEW:
            if (v->type == XR_LIR_STRING_CELL) {
                c_format(c, "    XrtString *xrl_cell%" PRIu32 " = xrt_string_retain(", v->result);
                c_value(c, v->operands[0]);
                c_format(c, ", UINT32_C(%" PRIu32 "));\n    v%" PRIu32 " = &xrl_cell%" PRIu32 ";\n",
                    string_source, v->result, v->result);
                break;
            }
            c_format(c, "    int64_t xrl_cell%" PRIu32 " = ", v->result);
            c_value(c, v->operands[0]);
            c_format(c, ";\n    v%" PRIu32 " = &xrl_cell%" PRIu32 ";\n", v->result, v->result);
            break;
        case XR_LIR_CELL_READ:
            if (v->type == XR_LIR_STRING) {
                c_format(c, "    v%" PRIu32 " = xrt_string_retain(*", v->result);
                c_value(c, v->operands[0]);
                c_format(c, ", UINT32_C(%" PRIu32 "));\n", string_source);
                break;
            }
            c_format(c, "    v%" PRIu32 " = *", v->result);
            c_value(c, v->operands[0]);
            c_text(c, ";\n");
            break;
        case XR_LIR_CELL_WRITE:
            if (c_value_type(c, v->operands[0]) == XR_LIR_STRING_CELL) {
                c_text(c, "    { XrtString *owned = xrt_string_retain("); c_value(c, v->operands[1]);
                c_format(c, ", UINT32_C(%" PRIu32 ")); xrt_string_drop(", string_source);
                c_value(c, v->operands[0]); c_text(c, "); *"); c_value(c, v->operands[0]);
                c_text(c, " = owned; }\n");
                break;
            }
            c_text(c, "    *"); c_value(c, v->operands[0]);
            c_text(c, " = "); c_value(c, v->operands[1]); c_text(c, ";\n");
            break;
        case XR_LIR_LITERAL_ADDR:
            c_format(c, "    v%" PRIu32 " = xrl_literal%" PRIu64 ";\n", v->result, v->bits);
            break;
        case XR_LIR_CONST:
            if (v->type == XR_LIR_PTR)
                c_format(c, "    v%" PRIu32 " = 0;\n", v->result);
            else if (v->type == XR_LIR_F64) {
                c_format(c, "    v%" PRIu32 " = UINT64_C(%" PRIu64 ");\n", c->scratch, v->bits);
                c_format(c, "    memcpy(&v%" PRIu32 ", &v%" PRIu32 ", sizeof(v%" PRIu32 "));\n",
                         v->result, c->scratch, v->result);
            } else
                c_integer(c, v);
            break;
        case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL:
        case XR_LIR_NEG: case XR_LIR_INTCAST:
            c_integer(c, v);
            break;
        case XR_LIR_DIV: case XR_LIR_REM:
            c_division(c, v);
            break;
        case XR_LIR_FNEG:
            c_fneg(c, v);
            break;
        case XR_LIR_FADD: case XR_LIR_FSUB: case XR_LIR_FMUL: case XR_LIR_FDIV: case XR_LIR_FCMP:
            c_float_operation(c,v);
            break;
        case XR_LIR_ICMP:
            c_format(c, "    v%" PRIu32 " = ", v->result);
            c_value(c, v->operands[0]);
            c_format(c, " %s ", predicates[v->predicate]);
            c_value(c, v->operands[1]);
            c_text(c, ";\n");
            break;
        case XR_LIR_CALL: {
            c_text(c, "    ");
            if (v->result)
                c_format(c, "v%" PRIu32 " = ", v->result);
            bool string_output = false;
            if (c_runtime_call(c, v)) {
                XrLirFunctionView external;
                xr_lir_function_view(c->request->module,
                    (XrLirFunction){c->request->module, v->callee}, &external);
                string_output = external.extern_kind == XR_LIR_EXTERN_PRINT_STRING;
                const char *name = string_output ? "xrt_print_string" :
                    external.extern_kind == XR_LIR_EXTERN_PRINT_I64 ? "xrt_print_i64" :
                    external.extern_kind == XR_LIR_EXTERN_CLOCK_REALTIME ? "xrt_clock_realtime_nanos" :
                    external.extern_kind == XR_LIR_EXTERN_CLOCK_CPU ? "xrt_clock_process_cpu_nanos" :
                    external.extern_kind == XR_LIR_EXTERN_UTC_OFFSET_AT ? "xrt_clock_utc_offset_at" :
                    "xrt_clock_monotonic_nanos";
                c_text(c, name);
            } else c_function_name(c, v->callee);
            c_text(c, "(");
            for (uint32_t a = 0; a < v->operand_count && c->status == XR_LIR_C_OK; ++a) {
                if (a)
                    c_text(c, ", ");
                if (string_output && a == 1) c_text(c, "(size_t)");
                c_value(c, v->operands[a]);
            }
            if (c_runtime_call(c, v)) {
                uint32_t id = c_located(&v->source) ? ++c->location_cursor : 0;
                c_format(c, "%sUINT32_C(%" PRIu32 ")", v->operand_count ? ", " : "", id);
            }
            c_text(c, ");\n");
            break;
        }
        case XR_LIR_BR:
            c_edge(c, &v->edges[0]);
            break;
        case XR_LIR_CONDBR:
            c_text(c, "    if (");
            c_value(c, v->operands[0]);
            c_text(c, ") {\n");
            c_edge(c, &v->edges[0]);
            c_text(c, "    } else {\n");
            c_edge(c, &v->edges[1]);
            c_text(c, "    }\n");
            break;
        case XR_LIR_RET:
            c_text(c, "    return");
            if (v->operand_count) {
                c_text(c, " ");
                c_value(c, v->operands[0]);
            }
            c_text(c, ";\n");
            break;
        default:
            XR_CHECK(false, "C instruction passed admission");
    }
}
static void c_standalone_root(LirC *c, XrLirType result) {
    const char *type = c_type(result);
    XR_DCHECK(type != NULL && c->runtime, "a bound root keeps its admitted result type");
    c->function = c->request->entry;
    c->block = c->instruction = 0;
    c->source = (XrLirSource) {0};
    c_text(c, "static void xrl_invoke(void *context) {\n");
    if (result == XR_LIR_STRING) {
        c_text(c, "    XrtString *owned = ");
        c_function_name(c, c->request->entry.id);
        c_text(c, "();\n    *(XrtString **)context = xrt_string_root_take(owned);\n}\n\n");
    } else {
    if (result == XR_LIR_UNIT)
        c_text(c, "    (void)context;\n    ");
    else
        c_format(c, "    *(%s *)context = ", type);
    c_function_name(c, c->request->entry.id);
    c_text(c, "();\n}\n\n");
    }
    c_format(c, "%s xray_lir_entry(void) {\n", type);
    if (result == XR_LIR_UNIT) {
        c_text(c, "    xrt_run_standalone(&xrt_standalone_source_map, xrl_invoke, NULL);\n");
    } else {
        c_format(c, "    %s result;\n", type);
        c_text(c, "    xrt_run_standalone(&xrt_standalone_source_map, xrl_invoke, &result);\n");
        c_text(c, "    return result;\n");
    }
    c_text(c, "}\n\n");
}
/* Compiler target macros belong to the emitted target, not this host.
 * The selected binary64 closure must fail before unsupported C can execute. */
static void c_preamble(LirC *c) {
    if (c->f64) {
        c_text(c, "#if !defined(_MSC_VER) || !defined(_M_X64) || defined(_M_ARM64EC)\n"
            "#error F64_TRANSPORT_REQUIRES_VALIDATED_MICROSOFT_X64_ABI\n#endif\n"
            "#if defined(_M_FP_FAST) || defined(__FAST_MATH__) || "
            "(defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)\n"
            "#error F64_TRANSPORT_REQUIRES_NONFAST_FLOATING_POINT_MODE\n#endif\n"
            "#include <stdint.h>\n#include <limits.h>\n#include <float.h>\n#include <string.h>\n"
            "_Static_assert(CHAR_BIT == 8 && sizeof(double) == 8 && sizeof(uint64_t) == 8, "
            "\"binary64 object size\");\n"
            "_Static_assert(FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MIN_EXP == -1021 "
            "&& DBL_MAX_EXP == 1024, \"binary64 precision\");\n\n");
    } else {
        c_text(c, "#include <stdint.h>\n\n");
    }
}
static void c_literals(LirC *c) {
    for (uint32_t i = 0; i < xr_lir_literal_count(c->request->module) && c_request_poll(c); ++i) {
        if (!c->used_literals[i]) continue;
        XrLirLiteral literal = {0};const uint8_t *bytes = NULL;size_t length = 0;
        xr_lir_literal_at(c->request->module, i, &literal);
        xr_lir_literal_view(c->request->module, literal, &bytes, &length);
        c_format(c, "_Static_assert(SIZE_MAX >= UINT64_C(%" PRIu64 "), \"literal length fits size_t\");\n",
            (uint64_t)length);
        c_format(c, "static const unsigned char xrl_literal%" PRIu32 "[] = {", i + 1);
        for (size_t b = 0; b < length && c_request_poll(c); ++b) c_format(c, "0x%02x,", (unsigned)bytes[b]);
        c_text(c, "0};\n");
    }
}
static void c_emit(LirC *c, XrLirType root_result) {
    XR_DCHECK(c->closure_count && c->closure[0] == c->request->entry.id,
              "emission starts from the explicit execution root");
    c_preamble(c);
    if (c->runtime)
        c_text(c, "#include \"xray_native_runtime.h\"\n\n");
    if (root_result == XR_LIR_STRING)
        c_text(c, "extern XrtString *xrt_string_root_take(XrtString *string);\n\n");
    if (c->fenv)
        c_text(c, "#if defined(_MSC_VER) && defined(_M_X64) && !defined(_M_ARM64EC) && "
            "!defined(_M_FP_FAST) && !defined(__FAST_MATH__) && "
            "(!defined(__FINITE_MATH_ONLY__) || __FINITE_MATH_ONLY__ <= 0)\n"
            "#if defined(__clang__)\n#pragma STDC FENV_ACCESS ON\n"
            "#elif defined(_MSC_VER)\n#pragma fenv_access(on)\n#endif\n#endif\n\n");
    c_locations(c);
    c_literals(c);
    for (uint32_t n = 0; n < c->closure_count && c->status == XR_LIR_C_OK; ++n) {
        c->function = (XrLirFunction) {c->request->module, c->closure[n]};
        xr_lir_function_view(c->request->module, c->function, &c->view);
        if (!c_request_poll(c))
            return;
        if (c->view.kind == XR_LIR_FUNCTION_EXTERNAL)
            continue;
        c_signature(c, false);
    }
    for (uint32_t n = 0; n < c->closure_count && c->status == XR_LIR_C_OK; ++n) {
        c->function = (XrLirFunction) {c->request->module, c->closure[n]};
        xr_lir_function_view(c->request->module, c->function, &c->view);
        if (!c_request_poll(c))
            return;
        if (c->view.kind == XR_LIR_FUNCTION_EXTERNAL)
            continue;
        c_prepare_divisors(c);
        c_signature(c, true);
        c_declarations(c);
        c_format(c, "    goto bb%" PRIu32 ";\n", c->view.entry);
        for (uint32_t b = 0; b < c->view.block_count && c->status == XR_LIR_C_OK; ++b) {
            XrLirBlock block;
            XrLirBlockView view;
            xr_lir_block_at(c->request->module, c->function, b, &block);
            xr_lir_block_view(c->request->module, block, &view);
            c->block = block.id;
            if (!c_request_poll(c))
                return;
            c_format(c, "bb%" PRIu32 ": ;\n", block.id);
            for (uint32_t i = 0; i < view.instruction_count && c->status == XR_LIR_C_OK; ++i) {
                XrLirInstructionView v;
                xr_lir_instruction_view(c->request->module, block, i, &v);
                c->instruction = i + 1;
                c->source = v.source;
                c_instruction(c, &v);
            }
        }
        c_text(c, "}\n\n");
    }
    if (c->runtime && c->status == XR_LIR_C_OK)
        c_standalone_root(c, root_result);
    XR_DCHECK(c->status != XR_LIR_C_OK || c->location_cursor == c->location_count,
              "ordinary output calls consume their own static location IDs");
}
XR_FUNC void xr_lir_c_output_free(XrLirCOutput *output) {
    if (!output)
        return;
    xr_compile_resources_free(output->bytes);
    *output = (XrLirCOutput) {0};
}
XR_FUNC XrLirCStatus xr_lir_emit_c(const XrLirCRequest *request, XrLirCOutput *output,
                                   XrLirCDiagnostic *diagnostic) {
    if (diagnostic)
        *diagnostic = (XrLirCDiagnostic) {0};
    LirC c = {.request = request, .diagnostic = diagnostic};
    XrLirFunctionView entry;
    if (!request || !output || output->bytes || output->length || output->entry_result ||
        !request->resources || !request->translation_unit || !request->module ||
        request->entry.module != request->module ||
        xr_lir_function_view(request->module, request->entry, &entry) != XR_LIR_OK ||
        entry.kind != XR_LIR_FUNCTION_DEFINITION || entry.parameter_count) {
        c_error(&c, XR_LIR_C_BAD_ARGUMENT, "C emission requires empty output and a no-argument root");
        return c.status;
    }
    if (!c_request_poll(&c))
        return c.status;
    XrLirDiagnostic invalid;
    XrLirStatus status = xr_lir_verify(request->module, &invalid);
    if (status != XR_LIR_OK) {
        c.function.id = invalid.function;
        c.block = invalid.block;
        c.instruction = invalid.instruction;
        c.source = invalid.source;
        c_error(&c, status == XR_LIR_OUT_OF_MEMORY ? XR_LIR_C_OUT_OF_MEMORY : XR_LIR_C_INVALID_LIR,
                invalid.message);
        return c.status;
    }
    if (!c_request_poll(&c))
        return c.status;
    uint32_t literal_count = xr_lir_literal_count(request->module);
    if (literal_count) {
        void *used = NULL;
        if (c_resource(&c, xr_compile_resources_calloc(request->resources, literal_count, 1, &used)))
            c.used_literals = used;
    }
    if (c.status == XR_LIR_C_OK)
        c_admit(&c);
    if (c.status == XR_LIR_C_OK)
        c_emit(&c, entry.result);
    if (c_request_poll(&c)) {
        XiCgenVerifyStatus verified = xr_compile_cgen_verify_output_or_ice(request->resources,
            c.text, c.length, request->translation_unit);
        if (verified != XI_CGEN_VERIFY_PASSED) {
            XrLirCStatus mapped;
            switch (verified) {
                case XI_CGEN_VERIFY_OUT_OF_MEMORY: mapped = XR_LIR_C_OUT_OF_MEMORY; break;
                case XI_CGEN_VERIFY_MEMORY_LIMIT: mapped = XR_LIR_C_MEMORY_LIMIT; break;
                case XI_CGEN_VERIFY_OVERFLOW: mapped = XR_LIR_C_OVERFLOW; break;
                case XI_CGEN_VERIFY_BUDGET: mapped = XR_LIR_C_BUDGET; break;
                case XI_CGEN_VERIFY_CANCELLED: mapped = XR_LIR_C_CANCELLED; break;
                case XI_CGEN_VERIFY_DEADLINE: mapped = XR_LIR_C_DEADLINE; break;
                case XI_CGEN_VERIFY_CLOCK_FAILURE: mapped = XR_LIR_C_CLOCK_FAILURE; break;
                default: mapped = XR_LIR_C_BAD_ARGUMENT; break;
            }
            c_error(&c, mapped, "C output verifier could not complete");
        }
    }

    if (c_request_poll(&c)) {
        *output = (XrLirCOutput) {c.text, c.length, entry.result};
        c.text = NULL;
    }
    xr_compile_resources_free(c.text);
    xr_compile_resources_free(c.closure);
    xr_compile_resources_free(c.seen);
    xr_compile_resources_free(c.fixed_divisors);
    xr_compile_resources_free(c.used_literals);
    return c.status;
}
