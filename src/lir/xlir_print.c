/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_print.c - Stable scalar SSA text without host formatting state
 *
 * KEY CONCEPT:
 *   Display IDs follow definition order rather than allocation order. The
 *   sink receives LF bytes directly, and floating constants retain bit forms.
 */
#include "xlir_internal.h"
#include "xlir_print.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct LirPrinter {
    XrLirWrite write;
    void *context;
    bool good;
    uint32_t *ids;
} LirPrinter;

static void lir_print_bytes(LirPrinter *p, const char *bytes, size_t length) {
    if (p->good && !p->write(p->context, bytes, length))
        p->good = false;
}
static void lir_print_text(LirPrinter *p, const char *text) {
    lir_print_bytes(p, text, strlen(text));
}
static void lir_print_format(LirPrinter *p, const char *format, ...) XR_PRINTF_FMT(2, 3);
static void lir_print_format(LirPrinter *p, const char *format, ...) {
    if (!p->good)
        return;
    char buffer[192];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length < 0 || (size_t) length >= sizeof(buffer)) {
        p->good = false;
        return;
    }
    lir_print_bytes(p, buffer, (size_t) length);
}

static bool lir_name_initial(unsigned char byte) {
    return byte == '_' || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}
static bool lir_name_simple(const char *name, size_t length) {
    if (!length || !lir_name_initial((unsigned char) name[0]))
        return false;
    for (size_t i = 1; i < length; ++i)
        if (!lir_name_initial((unsigned char) name[i]) && (name[i] < '0' || name[i] > '9'))
            return false;
    return true;
}
static void lir_print_name(LirPrinter *p, const XrLirFunctionData *f) {
    lir_print_text(p, "@");
    if (lir_name_simple(f->name, f->name_length)) {
        lir_print_bytes(p, f->name, f->name_length);
        return;
    }
    lir_print_text(p, "\"");
    for (size_t i = 0; i < f->name_length && p->good; ++i) {
        unsigned char byte = (unsigned char) f->name[i];
        if (byte == '"' || byte == '\\')
            lir_print_format(p, "\\%c", byte);
        else if (byte < 32 || byte >= 127)
            lir_print_format(p, "\\x%02x", (unsigned) byte);
        else
            lir_print_bytes(p, &f->name[i], 1);
    }
    lir_print_text(p, "\"");
}

static void lir_print_value(LirPrinter *p, uint32_t id) {
    XR_DCHECK(id != 0, "printed operand has a value");
    lir_print_format(p, "%%%" PRIu32, p->ids[id - 1]);
}
static void lir_print_values(LirPrinter *p, const uint32_t *values, uint32_t count) {
    for (uint32_t i = 0; i < count && p->good; ++i) {
        if (i)
            lir_print_text(p, ", ");
        lir_print_value(p, values[i]);
    }
}
static void lir_print_edge(LirPrinter *p, const XrLirEdgeView *edge) {
    lir_print_format(p, "^b%" PRIu32 "(", edge->target - 1);
    lir_print_values(p, edge->arguments, edge->argument_count);
    lir_print_text(p, ")");
}

static void lir_print_constant(LirPrinter *p, XrLirType type, uint64_t bits) {
    if (type == XR_LIR_PTR) {
        lir_print_text(p, "null");
        return;
    }
    if (type == XR_LIR_F32) {
        lir_print_format(p, "0x%08" PRIx64, bits);
        return;
    }
    if (type == XR_LIR_F64) {
        lir_print_format(p, "0x%016" PRIx64, bits);
        return;
    }
    if (type >= XR_LIR_I8 && type <= XR_LIR_I64) {
        unsigned width = 8u << (unsigned) (type - XR_LIR_I8);
        uint64_t sign = UINT64_C(1) << (width - 1);
        if (bits & sign) {
            uint64_t mask = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
            lir_print_format(p, "-%" PRIu64, ((~bits) & mask) + 1);
            return;
        }
    }
    lir_print_format(p, "%" PRIu64, bits);
}

static void lir_print_instruction(LirPrinter *p, const XrLirModule *m, const XrLirFunctionData *f,
                                  const XrLirInstructionView *v) {
    static const char *const predicates[] = {"invalid", "eq", "ne", "lt", "le", "gt", "ge"};
    lir_print_text(p, "    ");
    if (v->result) {
        lir_print_value(p, v->result);
        lir_print_text(p, " = ");
    }
    lir_print_text(p, xr_lir_op_name(v->op));
    if (v->op == XR_LIR_CALL) {
        lir_print_text(p, " ");
        lir_print_name(p, &m->functions[v->callee - 1]);
        lir_print_text(p, "(");
        lir_print_values(p, v->operands, v->operand_count);
        lir_print_text(p, ")");
    } else if (v->op == XR_LIR_CALL_INTRINSIC) {
        lir_print_format(p, " #%" PRIu32 "(", v->intrinsic);
        lir_print_values(p, v->operands, v->operand_count);
        lir_print_text(p, ")");
    } else if (v->op == XR_LIR_BR || v->op == XR_LIR_CONDBR) {
        lir_print_text(p, " ");
        if (v->op == XR_LIR_CONDBR) {
            lir_print_value(p, v->operands[0]);
            lir_print_text(p, ", ");
        }
        for (uint32_t e = 0; e < v->edge_count; ++e) {
            if (e)
                lir_print_text(p, ", ");
            lir_print_edge(p, &v->edges[e]);
        }
    } else if (v->op == XR_LIR_RET) {
        if (v->operand_count) {
            lir_print_text(p, " ");
            lir_print_value(p, v->operands[0]);
        }
    } else {
        XrLirType displayed = v->type;
        if (v->op == XR_LIR_ICMP || v->op == XR_LIR_FCMP) {
            lir_print_format(p, " %s", predicates[v->predicate]);
            displayed = f->values[v->operands[0] - 1].type;
        }
        lir_print_format(p, " %s ", xr_lir_type_name(displayed));
        if (v->op == XR_LIR_CONST)
            lir_print_constant(p, v->type, v->bits);
        else
            lir_print_values(p, v->operands, v->operand_count);
    }
    lir_print_text(p, "\n");
}

static void lir_print_parameters(LirPrinter *p, const XrLirFunctionData *f, const uint32_t *ids,
                                 uint32_t count, bool external) {
    for (uint32_t i = 0; i < count; ++i) {
        if (i)
            lir_print_text(p, ", ");
        if (!external) {
            lir_print_value(p, ids[i]);
            lir_print_text(p, ": ");
        }
        XrLirType type = external ? f->parameter_types[i] : f->values[ids[i] - 1].type;
        lir_print_text(p, xr_lir_type_name(type));
    }
}

static XrLirStatus lir_print_function(LirPrinter *p, const XrLirModule *m,
                                      const XrLirFunctionData *f) {
    bool external = f->kind == XR_LIR_FUNCTION_EXTERNAL;
    if (f->value_count) {
        if ((uint64_t) f->value_count * sizeof(*p->ids) > SIZE_MAX)
            return XR_LIR_OUT_OF_MEMORY;
        p->ids = xr_malloc((size_t) f->value_count * sizeof(*p->ids));
        if (!p->ids)
            return XR_LIR_OUT_OF_MEMORY;
        uint32_t next = 0;
        for (uint32_t i = 0; i < f->parameter_count; ++i)
            p->ids[f->parameters[i] - 1] = next++;
        for (uint32_t b = 0; b < f->block_count; ++b) {
            const XrLirBlockData *block = &f->blocks[b];
            for (uint32_t i = 0; i < block->parameter_count; ++i)
                p->ids[block->parameters[i] - 1] = next++;
            for (uint32_t i = 0; i < block->instruction_count; ++i) {
                uint32_t result = block->instructions[i].view.result;
                if (result)
                    p->ids[result - 1] = next++;
            }
        }
    }
    lir_print_text(p, external ? "  extern " : "  func ");
    lir_print_name(p, f);
    lir_print_text(p, "(");
    lir_print_parameters(p, f, f->parameters, f->parameter_count, external);
    lir_print_format(p, ") -> %s", xr_lir_type_name(f->result));
    if (external)
        lir_print_text(p, "\n");
    else {
        lir_print_format(p, " {\n    entry ^b%" PRIu32 "\n", f->entry - 1);
        for (uint32_t b = 0; b < f->block_count && p->good; ++b) {
            const XrLirBlockData *block = &f->blocks[b];
            lir_print_format(p, "  ^b%" PRIu32, b);
            if (block->parameter_count) {
                lir_print_text(p, "(");
                lir_print_parameters(p, f, block->parameters, block->parameter_count, false);
                lir_print_text(p, ")");
            }
            lir_print_text(p, ":\n");
            for (uint32_t i = 0; i < block->instruction_count && p->good; ++i)
                lir_print_instruction(p, m, f, &block->instructions[i].view);
        }
        lir_print_text(p, "  }\n");
    }
    xr_free(p->ids);
    p->ids = NULL;
    return p->good ? XR_LIR_OK : XR_LIR_OUTPUT_ERROR;
}

XR_FUNC XrLirStatus xr_lir_print(const XrLirModule *m, XrLirWrite write, void *context) {
    if (!m || !m->finished || m->failure != XR_LIR_OK || !write)
        return XR_LIR_INVALID_ARGUMENT;
    LirPrinter printer = {write, context, true, NULL};
    lir_print_text(&printer, "module {\n");
    for (uint32_t i = 0; i < m->function_count && printer.good; ++i) {
        XrLirStatus status = lir_print_function(&printer, m, &m->functions[i]);
        if (status != XR_LIR_OK)
            return status;
    }
    lir_print_text(&printer, "}\n");
    return printer.good ? XR_LIR_OK : XR_LIR_OUTPUT_ERROR;
}
