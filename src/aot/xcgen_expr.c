/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen_expr.c - AOT C code generator: expression/instruction translation
 *
 * KEY CONCEPT:
 *   Translates individual XIR instructions into C statements.
 *   Each XIR vreg maps to a C local variable (int64_t / double).
 *   XIR constants are emitted as C literals.
 *
 * RELATED MODULES:
 *   - xcgen.c: module-level orchestration
 *   - xcgen_stmt.c: control flow (terminators, phi lowering)
 */

#include "xcgen.h"
#include "../base/xchecks.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/object/xstring.h"
#include "../runtime/gc/xgc_header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* ========== Type Query ========== */

// Get XIR type of a ref (vreg type or const type)
uint8_t xcg_ref_type(XirFunc *func, XirRef ref) {
    XR_DCHECK(func != NULL, "xcg_ref_type: func is NULL");
    if (xir_ref_is_vreg(ref)) {
        uint32_t idx = XIR_REF_INDEX(ref);
        if (idx < func->nvreg)
            return func->vregs[idx].rep;
    } else if (xir_ref_is_const(ref)) {
        uint32_t idx = XIR_REF_INDEX(ref);
        if (idx < func->nconst)
            return func->consts[idx].rep;
    }
    return XR_REP_TAGGED;
}

/* ========== Ref → C Expression ========== */

void xcg_emit_ref(XcgenBuf *b, XirFunc *func, XirRef ref) {
    XR_DCHECK(b != NULL, "xcg_emit_ref: b is NULL");
    XR_DCHECK(func != NULL, "xcg_emit_ref: func is NULL");
    switch (XIR_REF_KIND(ref)) {
        case XIR_REF_VREG: {
            uint32_t idx = XIR_REF_INDEX(ref);
            // Inline constants: if vreg is defined by CONST_I64/CONST_F64
            // AND vreg type matches (not tagged), emit literal directly
            // to avoid cross-block use-before-def issues
            if (idx < func->nvreg && func->vregs[idx].def) {
                XirIns *def = func->vregs[idx].def;
                uint8_t vtype = func->vregs[idx].rep;
                if (def->op == XIR_CONST_I64 && vtype == XR_REP_I64 &&
                    xir_ref_is_const(def->args[0])) {
                    uint32_t ci = XIR_REF_INDEX(def->args[0]);
                    if (ci < func->nconst) {
                        xcgen_buf_printf(b, "INT64_C(%" PRId64 ")", func->consts[ci].val.i64);
                        break;
                    }
                }
                if (def->op == XIR_CONST_F64 && vtype == XR_REP_F64 &&
                    xir_ref_is_const(def->args[0])) {
                    uint32_t ci = XIR_REF_INDEX(def->args[0]);
                    if (ci < func->nconst) {
                        xcgen_buf_printf(b, "%.17g", func->consts[ci].val.f64);
                        break;
                    }
                }
            }
            xcgen_buf_printf(b, "v%u", idx);
            break;
        }
        case XIR_REF_CONST: {
            uint32_t idx = XIR_REF_INDEX(ref);
            if (idx < func->nconst) {
                XirConst *c = &func->consts[idx];
                if (c->rep == XR_REP_I64) {
                    xcgen_buf_printf(b, "INT64_C(%" PRId64 ")", c->val.i64);
                } else if (c->rep == XR_REP_F64) {
                    xcgen_buf_printf(b, "%.17g", c->val.f64);
                } else if (c->rep == XR_REP_STR) {
                    // Emit escaped C string literal
                    xcgen_buf_puts(b, "\"");
                    const char *s = c->val.str.chars;
                    for (uint32_t si = 0; si < c->val.str.len; si++) {
                        char ch = s[si];
                        switch (ch) {
                            case '\\':
                                xcgen_buf_puts(b, "\\\\");
                                break;
                            case '"':
                                xcgen_buf_puts(b, "\\\"");
                                break;
                            case '\n':
                                xcgen_buf_puts(b, "\\n");
                                break;
                            case '\r':
                                xcgen_buf_puts(b, "\\r");
                                break;
                            case '\t':
                                xcgen_buf_puts(b, "\\t");
                                break;
                            case '\0':
                                xcgen_buf_puts(b, "\\0");
                                break;
                            default:
                                xcgen_buf_printf(b, "%c", ch);
                                break;
                        }
                    }
                    xcgen_buf_puts(b, "\"");
                } else if (c->rep == XR_REP_PTR && c->val.raw != 0 &&
                           XR_GC_GET_TYPE((XrGCHeader *) (uintptr_t) c->val.raw) == XR_TSTRING) {
                    // PTR const pointing to XrString*: emit the string data
                    XrString *xrs = (XrString *) (uintptr_t) c->val.raw;
                    xcgen_buf_puts(b, "\"");
                    for (uint32_t si = 0; si < xrs->length; si++) {
                        char ch = xrs->data[si];
                        switch (ch) {
                            case '\\':
                                xcgen_buf_puts(b, "\\\\");
                                break;
                            case '"':
                                xcgen_buf_puts(b, "\\\"");
                                break;
                            case '\n':
                                xcgen_buf_puts(b, "\\n");
                                break;
                            case '\r':
                                xcgen_buf_puts(b, "\\r");
                                break;
                            case '\t':
                                xcgen_buf_puts(b, "\\t");
                                break;
                            case '\0':
                                xcgen_buf_puts(b, "\\0");
                                break;
                            default:
                                xcgen_buf_printf(b, "%c", ch);
                                break;
                        }
                    }
                    xcgen_buf_puts(b, "\"");
                } else {
                    xcgen_buf_printf(b, "(int64_t)0x%" PRIx64, (uint64_t) c->val.raw);
                }
            } else {
                xcgen_buf_printf(b, "/* bad const %u */ 0", idx);
            }
            break;
        }
        default:
            xcgen_buf_puts(b, "/* unknown ref */ 0");
            break;
    }
}

// Emit a ref as XrtValue, auto-boxing int64_t/double if needed
static void xcg_emit_ref_as_tagged(XcgenBuf *b, XirFunc *func, XirRef ref) {
    uint8_t t = xcg_ref_type(func, ref);
    if (t == XR_REP_I64) {
        xcgen_buf_puts(b, "xrt_box_int(");
        xcg_emit_ref(b, func, ref);
        xcgen_buf_puts(b, ")");
    } else if (t == XR_REP_F64) {
        xcgen_buf_puts(b, "xrt_box_float(");
        xcg_emit_ref(b, func, ref);
        xcgen_buf_puts(b, ")");
    } else {
        xcg_emit_ref(b, func, ref);
    }
}

/* ========== Binary/Compare/Unary Operations ========== */

// Emit a ref as a native numeric type, auto-unboxing XrtValue if needed
static void xcg_emit_ref_as_native(XcgenBuf *b, XirFunc *func, XirRef ref, uint8_t target_type) {
    uint8_t t = xcg_ref_type(func, ref);
    bool is_tagged = (t == XR_REP_STR || t == XR_REP_PTR || t == XR_REP_TAGGED);
    if (is_tagged) {
        if (target_type == XR_REP_F64) {
            xcgen_buf_puts(b, "xrt_unbox_float(");
        } else {
            xcgen_buf_puts(b, "xrt_unbox_int(");
        }
        xcg_emit_ref(b, func, ref);
        xcgen_buf_puts(b, ")");
    } else {
        xcg_emit_ref(b, func, ref);
    }
}

void xcg_emit_binary_op(XcgenBuf *b, XirFunc *func, XirIns *ins, const char *op) {
    XR_DCHECK(b != NULL, "xcg_emit_binary_op: NULL buf");
    XR_DCHECK(func != NULL, "xcg_emit_binary_op: NULL func");
    uint8_t dst_type = xcg_ref_type(func, ins->dst);
    xcgen_buf_printf(b, "    v%u = ", XIR_REF_INDEX(ins->dst));
    xcg_emit_ref_as_native(b, func, ins->args[0], dst_type);
    xcgen_buf_printf(b, " %s ", op);
    xcg_emit_ref_as_native(b, func, ins->args[1], dst_type);
    xcgen_buf_puts(b, ";\n");
}

void xcg_emit_compare_op(XcgenBuf *b, XirFunc *func, XirIns *ins, const char *op) {
    XR_DCHECK(b != NULL, "xcg_emit_compare_op: NULL buf");
    XR_DCHECK(func != NULL, "xcg_emit_compare_op: NULL func");
    uint8_t ta = xcg_ref_type(func, ins->args[0]);
    uint8_t tb = xcg_ref_type(func, ins->args[1]);
    bool a_tagged = (ta == XR_REP_STR || ta == XR_REP_PTR || ta == XR_REP_TAGGED);

    // ISNULL pattern: EQ/NE between TAGGED and constant 0 → tag check
    if (a_tagged && (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)) {
        bool b_is_zero = false;
        if (xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            if (ci < func->nconst && func->consts[ci].val.i64 == 0)
                b_is_zero = true;
        } else if (tb == XR_REP_I64 && xir_ref_is_vreg(ins->args[1])) {
            // Only treat I64 vreg as zero if its definition is CONST_I64(0).
            // Use SSA def pointer for O(1) lookup instead of full scan.
            uint32_t vi = XIR_REF_INDEX(ins->args[1]);
            if (vi < func->nvreg && func->vregs[vi].def) {
                XirIns *def = func->vregs[vi].def;
                if (def->op == XIR_CONST_I64 && xir_ref_is_const(def->args[0])) {
                    uint32_t ci2 = XIR_REF_INDEX(def->args[0]);
                    if (ci2 < func->nconst && func->consts[ci2].val.i64 == 0)
                        b_is_zero = true;
                }
            }
        }
        if (b_is_zero) {
            xcgen_buf_printf(b, "    v%u = (int64_t)(", XIR_REF_INDEX(ins->dst));
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_printf(b, ".tag %s XRT_TAG_NULL);\n", op);
            return;
        }
    }

    xcgen_buf_printf(b, "    v%u = (int64_t)(", XIR_REF_INDEX(ins->dst));
    xcg_emit_ref_as_native(b, func, ins->args[0], XR_REP_I64);
    xcgen_buf_printf(b, " %s ", op);
    xcg_emit_ref_as_native(b, func, ins->args[1], XR_REP_I64);
    xcgen_buf_puts(b, ");\n");
}

/* ========== Runtime Operation Helpers ========== */

// Emit runtime mixed-type arithmetic/comparison ops (RT_ADD..RT_EQ, RT_UNM)
static void xcg_emit_rt_arith(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf) {
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    switch (ins->op) {
        case XIR_RT_ADD:
        case XIR_RT_SUB:
        case XIR_RT_MUL:
        case XIR_RT_DIV:
        case XIR_RT_MOD: {
            uint8_t ta = xcg_ref_type(func, ins->args[0]);
            uint8_t tb = xcg_ref_type(func, ins->args[1]);
            if ((ta == XR_REP_I64 || ta == XR_REP_F64) && (tb == XR_REP_I64 || tb == XR_REP_F64)) {
                const char *op = ins->op == XIR_RT_ADD   ? "+"
                                 : ins->op == XIR_RT_SUB ? "-"
                                 : ins->op == XIR_RT_MUL ? "*"
                                 : ins->op == XIR_RT_DIV ? "/"
                                                         : "%";
                xcg_emit_binary_op(b, func, ins, op);
            } else {
                const char *fn = ins->op == XIR_RT_ADD   ? "xrt_add"
                                 : ins->op == XIR_RT_SUB ? "xrt_sub"
                                 : ins->op == XIR_RT_MUL ? "xrt_mul"
                                 : ins->op == XIR_RT_DIV ? "xrt_div"
                                                         : "xrt_mod";
                xcgen_buf_printf(b, "    v%u = %s(", dst_idx, fn);
                xcg_emit_ref_as_tagged(b, func, ins->args[0]);
                xcgen_buf_puts(b, ", ");
                xcg_emit_ref_as_tagged(b, func, ins->args[1]);
                xcgen_buf_puts(b, ");\n");
                cf->needs_runtime = true;
            }
            return;
        }
        case XIR_RT_UNM: {
            uint8_t ta = xcg_ref_type(func, ins->args[0]);
            if (ta == XR_REP_I64 || ta == XR_REP_F64) {
                xcgen_buf_printf(b, "    v%u = -", dst_idx);
                xcg_emit_ref(b, func, ins->args[0]);
                xcgen_buf_puts(b, ";\n");
            } else {
                xcgen_buf_printf(b, "    v%u = xrt_neg(", dst_idx);
                xcg_emit_ref(b, func, ins->args[0]);
                xcgen_buf_puts(b, ");\n");
                cf->needs_runtime = true;
            }
            return;
        }
        case XIR_RT_LT:
        case XIR_RT_LE:
        case XIR_RT_EQ: {
            uint8_t ta = xcg_ref_type(func, ins->args[0]);
            uint8_t tb = xcg_ref_type(func, ins->args[1]);
            if ((ta == XR_REP_I64 || ta == XR_REP_F64) && (tb == XR_REP_I64 || tb == XR_REP_F64)) {
                const char *op = ins->op == XIR_RT_LT ? "<" : ins->op == XIR_RT_LE ? "<=" : "==";
                xcg_emit_compare_op(b, func, ins, op);
            } else {
                const char *fn = ins->op == XIR_RT_LT   ? "xrt_lt"
                                 : ins->op == XIR_RT_LE ? "xrt_le"
                                                        : "xrt_eq";
                xcgen_buf_printf(b, "    v%u = %s(", dst_idx, fn);
                xcg_emit_ref_as_tagged(b, func, ins->args[0]);
                xcgen_buf_puts(b, ", ");
                xcg_emit_ref_as_tagged(b, func, ins->args[1]);
                xcgen_buf_puts(b, ");\n");
                cf->needs_runtime = true;
            }
            return;
        }
        default:
            return;
    }
}

// Emit print instruction with type specialization
static void xcg_emit_print_op(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf) {
    // args[0]=value, args[1]=const(flags)
    // flags: bit0=newline, bit1-2=slot_hint(0=ANY,1=I64,2=F64), bit3=add_space
    int64_t flags = 0;
    if (xir_ref_is_const(ins->args[1])) {
        uint32_t ci = XIR_REF_INDEX(ins->args[1]);
        if (ci < func->nconst)
            flags = func->consts[ci].val.i64;
    }
    int newline = (int) (flags & 1);
    int slot_hint = (int) ((flags >> 1) & 3);
    int add_space = (int) ((flags >> 3) & 1);

    if (add_space)
        xcgen_buf_puts(b, "    printf(\" \");\n");

    uint8_t val_type = xcg_ref_type(func, ins->args[0]);
    if (slot_hint == 1 || val_type == XR_REP_I64) {
        xcgen_buf_printf(b, "    printf(\"%%lld%s\", (long long)", newline ? "\\n" : "");
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ");\n");
    } else if (slot_hint == 2 || val_type == XR_REP_F64) {
        xcgen_buf_printf(b, "    printf(\"%%g%s\", (double)", newline ? "\\n" : "");
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ");\n");
    } else {
        if (newline) {
            xcgen_buf_puts(b, "    xrt_println(");
        } else {
            xcgen_buf_puts(b, "    xrt_print(");
        }
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
    }
}

// Emit array/index/map/isnull runtime operations
static void xcg_emit_collection_op(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf) {
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    switch (ins->op) {
        case XIR_RT_ARRAY_NEW:
            xcgen_buf_printf(b, "    v%u = xrt_array_new(", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            return;
        case XIR_RT_ARRAY_PUSH: {
            uint8_t val_type = xcg_ref_type(func, ins->args[1]);
            if (val_type == XR_REP_I64) {
                xcgen_buf_puts(b, "    xrt_array_push_i(");
                xcg_emit_ref(b, func, ins->args[0]);
                xcgen_buf_puts(b, ", ");
                xcg_emit_ref(b, func, ins->args[1]);
                xcgen_buf_puts(b, ");\n");
            } else if (val_type == XR_REP_F64) {
                xcgen_buf_puts(b, "    xrt_array_push_f(");
                xcg_emit_ref(b, func, ins->args[0]);
                xcgen_buf_puts(b, ", ");
                xcg_emit_ref(b, func, ins->args[1]);
                xcgen_buf_puts(b, ");\n");
            } else {
                xcgen_buf_puts(b, "    xrt_array_push(");
                xcg_emit_ref(b, func, ins->args[0]);
                xcgen_buf_puts(b, ", ");
                xcg_emit_ref_as_tagged(b, func, ins->args[1]);
                xcgen_buf_puts(b, ");\n");
            }
            cf->needs_runtime = true;
            return;
        }
        case XIR_RT_ARRAY_LEN:
            xcgen_buf_printf(b, "    v%u = xrt_array_len(", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            return;
        case XIR_RT_INDEX_GET:
            xcgen_buf_printf(b, "    v%u = xrt_index_get(", dst_idx);
            xcg_emit_ref_as_tagged(b, func, ins->args[0]);
            xcgen_buf_puts(b, ", ");
            xcg_emit_ref_as_tagged(b, func, ins->args[1]);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            return;
        case XIR_RT_INDEX_SET:
            xcgen_buf_puts(b, "    xrt_index_set(");
            xcg_emit_ref_as_tagged(b, func, ins->args[0]);
            xcgen_buf_puts(b, ", ");
            xcg_emit_ref_as_tagged(b, func, ins->args[1]);
            xcgen_buf_puts(b, ", ");
            xcg_emit_ref_as_tagged(b, func, ins->dst);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            return;
        case XIR_RT_MAP_NEW:
            xcgen_buf_printf(b, "    v%u = xrt_map_new(", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            return;
        case XIR_RT_ISNULL:
            xcgen_buf_printf(b, "    v%u = (", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ".tag == XRT_TAG_NULL) ? 1 : 0;\n");
            return;
        default:
            return;
    }
}

/* ========== Field Access Helpers ========== */

// Emit the prefix for struct field access
#define EMIT_STRUCT_BASE(b_, st_, base_vi_, func_)                                                 \
    do {                                                                                           \
        bool _is_ptr_param = ((base_vi_) < (func_)->num_params);                                   \
        if (_is_ptr_param) {                                                                       \
            xcgen_buf_printf((b_), "(v%u)->", (base_vi_));                                         \
        } else {                                                                                   \
            xcgen_buf_printf((b_), "((%s*)v%u.ptr)->", (st_)->c_name, (base_vi_));                 \
        }                                                                                          \
    } while (0)

// Emit LOAD_FIELD instruction
static void xcg_emit_field_load(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenModule *mod,
                                XcgenFunc *cf) {
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    const char *tagged_type = "XrValue";
    (void) tagged_type;
    // args[0] = base ptr, args[1] = const(byte_offset)
    int64_t offset = 0;
    if (xir_ref_is_const(ins->args[1])) {
        uint32_t ci = XIR_REF_INDEX(ins->args[1]);
        if (ci < func->nconst)
            offset = func->consts[ci].val.i64;
    }
    // Check if base is a promoted struct
    if (xir_ref_is_vreg(ins->args[0]) && cf->vreg_struct_id && mod->struct_reg) {
        uint32_t base_vi = XIR_REF_INDEX(ins->args[0]);
        int si = (base_vi < func->nvreg) ? cf->vreg_struct_id[base_vi] : -1;
        if (si >= 0) {
            XcgenStruct *st = &mod->struct_reg->structs[si];
            int fi = xcgen_field_by_offset(st, offset);
            if (fi >= 0) {
                uint8_t dst_t = xcg_ref_type(func, ins->dst);
                uint8_t fc = st->fields[fi].c_type;
                uint8_t fhint = st->fields[fi].val_hint_type;
                if (dst_t == XR_REP_F64 && fc == 2) {
                    if (fhint == XR_REP_F64) {
                        xcgen_buf_printf(b, "    v%u = ", dst_idx);
                        EMIT_STRUCT_BASE(b, st, base_vi, func);
                        xcgen_buf_printf(b, "%s.f;\n", st->fields[fi].name);
                    } else {
                        xcgen_buf_printf(b, "    v%u = xrt_unbox_float(", dst_idx);
                        EMIT_STRUCT_BASE(b, st, base_vi, func);
                        xcgen_buf_printf(b, "%s);\n", st->fields[fi].name);
                    }
                } else if (dst_t == XR_REP_I64 && fc == 2 && fhint == XR_REP_I64) {
                    xcgen_buf_printf(b, "    v%u = ", dst_idx);
                    EMIT_STRUCT_BASE(b, st, base_vi, func);
                    xcgen_buf_printf(b, "%s.i;\n", st->fields[fi].name);
                } else if (dst_t == XR_REP_I64 && fc == 1) {
                    xcgen_buf_printf(b, "    { double _tmp = ");
                    EMIT_STRUCT_BASE(b, st, base_vi, func);
                    xcgen_buf_printf(b, "%s; memcpy(&v%u, &_tmp, 8); }\n", st->fields[fi].name,
                                     dst_idx);
                } else if (dst_t == XR_REP_I64 && fc == 2) {
                    xcgen_buf_printf(b, "    v%u = xrt_unbox_int(", dst_idx);
                    EMIT_STRUCT_BASE(b, st, base_vi, func);
                    xcgen_buf_printf(b, "%s);\n", st->fields[fi].name);
                } else if ((dst_t == XR_REP_PTR || dst_t == XR_REP_TAGGED) && fc == 0) {
                    xcgen_buf_printf(b, "    v%u = xrt_box_int(", dst_idx);
                    EMIT_STRUCT_BASE(b, st, base_vi, func);
                    xcgen_buf_printf(b, "%s);\n", st->fields[fi].name);
                } else if ((dst_t == XR_REP_PTR || dst_t == XR_REP_TAGGED) && fc == 1) {
                    xcgen_buf_printf(b, "    v%u = xrt_box_float(", dst_idx);
                    EMIT_STRUCT_BASE(b, st, base_vi, func);
                    xcgen_buf_printf(b, "%s);\n", st->fields[fi].name);
                } else {
                    xcgen_buf_printf(b, "    v%u = ", dst_idx);
                    EMIT_STRUCT_BASE(b, st, base_vi, func);
                    xcgen_buf_printf(b, "%s;\n", st->fields[fi].name);
                }
                return;
            }
        }
    }
    // Fallback: raw byte offset access into 16-byte XrtValue field slots
    {
        uint8_t dst_t = xcg_ref_type(func, ins->dst);
        uint8_t base_t = xcg_ref_type(func, ins->args[0]);
        bool base_tagged =
            (base_t == XR_REP_PTR || base_t == XR_REP_TAGGED || base_t == XR_REP_STR);
        bool dst_is_tagged = (dst_t == XR_REP_PTR || dst_t == XR_REP_TAGGED || dst_t == XR_REP_STR);
        const char *cast = dst_is_tagged ? "XrValue" : (dst_t == XR_REP_F64) ? "double" : "int64_t";
        int64_t adj_offset = offset - 24;
        if (adj_offset < 0)
            adj_offset = 0;
        xcgen_buf_printf(b, "    v%u = *(%s*)((char*)", dst_idx, cast);
        if (base_tagged) {
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ".ptr");
        } else {
            xcg_emit_ref(b, func, ins->args[0]);
        }
        xcgen_buf_printf(b, " + %" PRId64 ");\n", adj_offset);
    }
}

// Emit STORE_FIELD instruction
static void xcg_emit_field_store(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenModule *mod,
                                 XcgenFunc *cf) {
    const char *tagged_type = "XrValue";
    // dst = const(byte_offset), args[0] = base ptr, args[1] = value
    int64_t offset = 0;
    if (xir_ref_is_const(ins->dst)) {
        uint32_t ci = XIR_REF_INDEX(ins->dst);
        if (ci < func->nconst)
            offset = func->consts[ci].val.i64;
    }
    // Check if base is a promoted struct
    if (xir_ref_is_vreg(ins->args[0]) && cf->vreg_struct_id && mod->struct_reg) {
        uint32_t base_vi = XIR_REF_INDEX(ins->args[0]);
        int si = (base_vi < func->nvreg) ? cf->vreg_struct_id[base_vi] : -1;
        if (si >= 0) {
            XcgenStruct *st = &mod->struct_reg->structs[si];
            int fi = xcgen_field_by_offset(st, offset);
            if (fi >= 0) {
                // tag=0 means null store — calloc already zeroed, skip
                if (ins->rep == 0)
                    return;
                uint8_t val_type = xcg_ref_type(func, ins->args[1]);
                uint8_t hint = st->fields[fi].val_hint_type;
                bool val_tagged =
                    (val_type == XR_REP_PTR || val_type == XR_REP_TAGGED || val_type == XR_REP_STR);
                // ARC: retain new val, release old field for tagged fields
                if (st->fields[fi].c_type == 2 && val_tagged) {
                    xcgen_buf_puts(b, "    xrt_arc_retain_val(");
                    xcg_emit_ref(b, func, ins->args[1]);
                    xcgen_buf_puts(b, "); xrt_arc_release_val(");
                    EMIT_STRUCT_BASE(b, st, base_vi, func);
                    xcgen_buf_printf(b, "%s);\n", st->fields[fi].name);
                }
                xcgen_buf_puts(b, "    ");
                EMIT_STRUCT_BASE(b, st, base_vi, func);
                xcgen_buf_printf(b, "%s = ", st->fields[fi].name);
                if (st->fields[fi].c_type == 1 && val_tagged) {
                    xcgen_buf_puts(b, "xrt_unbox_float(");
                    xcg_emit_ref(b, func, ins->args[1]);
                    xcgen_buf_puts(b, ")");
                } else if (st->fields[fi].c_type == 0 && val_tagged) {
                    xcgen_buf_puts(b, "xrt_unbox_int(");
                    xcg_emit_ref(b, func, ins->args[1]);
                    xcgen_buf_puts(b, ")");
                } else if (st->fields[fi].c_type == 2 && val_type == XR_REP_F64) {
                    xcgen_buf_printf(b, "(%s){.f = ", tagged_type);
                    xcg_emit_ref(b, func, ins->args[1]);
                    xcgen_buf_printf(b, ", .tag = %d}", XR_TAG_F64);
                } else if (st->fields[fi].c_type == 2 && val_type == XR_REP_I64) {
                    xcgen_buf_printf(b, "(%s){.i = ", tagged_type);
                    xcg_emit_ref(b, func, ins->args[1]);
                    xcgen_buf_printf(b, ", .tag = %d}", XR_TAG_I64);
                } else if (st->fields[fi].c_type == 2 && val_tagged && hint == XR_REP_F64) {
                    xcgen_buf_printf(b, "(%s){.f = ", tagged_type);
                    xcg_emit_ref(b, func, ins->args[1]);
                    xcgen_buf_printf(b, ".f, .tag = %d}", XR_TAG_F64);
                } else if (st->fields[fi].c_type == 2 && val_tagged && hint == XR_REP_I64) {
                    xcgen_buf_printf(b, "(%s){.i = ", tagged_type);
                    xcg_emit_ref(b, func, ins->args[1]);
                    xcgen_buf_printf(b, ".i, .tag = %d}", XR_TAG_I64);
                } else {
                    xcg_emit_ref(b, func, ins->args[1]);
                }
                xcgen_buf_puts(b, ";\n");
                return;
            }
        }
    }
    // Fallback: raw byte offset store into 16-byte XrtValue field slots.
    // Always write a full 16-byte XrValue (with proper tag) so that the
    // LOAD_FIELD fallback (which reads *(XrValue*)) gets a complete value.
    {
        int64_t adj_offset = offset - 24;
        if (adj_offset < 0)
            adj_offset = 0;
        uint8_t base_t = xcg_ref_type(func, ins->args[0]);
        bool base_tagged =
            (base_t == XR_REP_PTR || base_t == XR_REP_TAGGED || base_t == XR_REP_STR);
        uint8_t val_type = xcg_ref_type(func, ins->args[1]);
        bool val_tagged =
            (val_type == XR_REP_PTR || val_type == XR_REP_TAGGED || val_type == XR_REP_STR);

        // Emit: { XrValue _sv = <boxed_value>; memcpy(base + off, &_sv, 16); }
        if (val_tagged) {
            // Already a full XrValue — store directly
            xcgen_buf_puts(b, "    { XrValue _sv = ");
            xcg_emit_ref(b, func, ins->args[1]);
        } else if (val_type == XR_REP_F64) {
            // Raw double — box with float tag
            xcgen_buf_puts(b, "    { XrValue _sv = xrt_box_float(");
            xcg_emit_ref(b, func, ins->args[1]);
            xcgen_buf_puts(b, ")");
        } else {
            // Raw int64_t — box with int tag
            xcgen_buf_puts(b, "    { XrValue _sv = xrt_box_int(");
            xcg_emit_ref(b, func, ins->args[1]);
            xcgen_buf_puts(b, ")");
        }
        xcgen_buf_puts(b, "; memcpy((char*)");
        if (base_tagged) {
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ".ptr");
        } else {
            xcg_emit_ref(b, func, ins->args[0]);
        }
        xcgen_buf_printf(b, " + %" PRId64 ", &_sv, 16); }\n", adj_offset);
    }
}

/* ========== Upvalue Access Helpers ========== */

// Emit LOAD_UPVAL instruction
static void xcg_emit_upval_load(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf) {
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    int64_t uv_idx = 0;
    if (xir_ref_is_const(ins->args[0])) {
        uint32_t ci = XIR_REF_INDEX(ins->args[0]);
        if (ci < func->nconst)
            uv_idx = func->consts[ci].val.i64;
    }
    if (cf->non_escaping && uv_idx < cf->num_upvals) {
        xcgen_buf_printf(b, "    v%u = xrt_upv%d;\n", dst_idx, (int) uv_idx);
    } else {
        xcgen_buf_printf(b, "    v%u = xrt_cl->upvals[%d];\n", dst_idx, (int) uv_idx);
        cf->needs_closure_param = true;
    }
    cf->needs_runtime = true;
}

// Emit STORE_UPVAL instruction
static void xcg_emit_upval_store(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenModule *mod,
                                 XcgenFunc *cf) {
    // New convention: dst=idx(const), args[0]=closure(vreg), args[1]=value
    int64_t uv_idx = 0;
    if (xir_ref_is_const(ins->dst)) {
        uint32_t ci = XIR_REF_INDEX(ins->dst);
        if (ci < func->nconst)
            uv_idx = func->consts[ci].val.i64;
    }
    if (xir_ref_is_vreg(ins->args[0])) {
        // Child closure upvalue initialization
        uint32_t cl_vreg = XIR_REF_INDEX(ins->args[0]);
        XcgenProtoEntry *child_entry = NULL;
        if (cl_vreg < func->nvreg && func->vregs[cl_vreg].def) {
            void *child_proto = NULL;
            if (func->call_arg_pool && func->vregs[cl_vreg].call_nargs > 0) {
                XirRef pr = func->call_arg_pool[func->vregs[cl_vreg].call_arg_start];
                if (xir_ref_is_const(pr)) {
                    uint32_t pci = XIR_REF_INDEX(pr);
                    if (pci < func->nconst)
                        child_proto = (void *) (uintptr_t) func->consts[pci].val.raw;
                }
            }
            if (!child_proto) {
                XirIns *cl_def = func->vregs[cl_vreg].def;
                if (xir_ref_is_const(cl_def->args[0])) {
                    uint32_t pci = XIR_REF_INDEX(cl_def->args[0]);
                    if (pci < func->nconst)
                        child_proto = (void *) (uintptr_t) func->consts[pci].val.raw;
                }
            }
            if (child_proto) {
                XcgenCompilation *comp = mod->comp;
                for (int pi = 0; pi < comp->proto_map_count; pi++) {
                    if (comp->proto_map[pi].proto_ptr == child_proto) {
                        child_entry = &comp->proto_map[pi];
                        break;
                    }
                }
            }
        }
        if (child_entry && child_entry->non_escaping) {
            xcgen_buf_printf(b, "    /* upval[%d] for non-escaping %s: passed at call site */\n",
                             (int) uv_idx, child_entry->c_name);
        } else {
            xcgen_buf_printf(b, "    ((xrt_closure_t*)v%u.ptr)->upvals[%d] = ", cl_vreg,
                             (int) uv_idx);
            xcg_emit_ref_as_tagged(b, func, ins->args[1]);
            xcgen_buf_puts(b, ";\n");
        }
    } else if (!xir_ref_is_vreg(ins->args[0])) {
        if (cf->non_escaping && uv_idx < cf->num_upvals) {
            xcgen_buf_printf(b, "    /* WARN: store to non-escaping upval[%d] */\n", (int) uv_idx);
        } else {
            xcgen_buf_printf(b, "    xrt_cl->upvals[%d] = ", (int) uv_idx);
            xcg_emit_ref_as_tagged(b, func, ins->args[1]);
            xcgen_buf_puts(b, ";\n");
            cf->needs_closure_param = true;
        }
    }
    cf->needs_runtime = true;
}

/* ========== Raw Memory Access Helper ========== */

// Emit raw memory load/store instructions (XIR_LOAD..XIR_STORE_F32, XIR_ALLOC)
static void xcg_emit_raw_mem_op(XcgenBuf *b, XirFunc *func, XirIns *ins) {
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    switch (ins->op) {
        case XIR_LOAD: {
            uint8_t dst_t = xcg_ref_type(func, ins->dst);
            const char *cast = (dst_t == XR_REP_F64) ? "double" : "int64_t";
            xcgen_buf_printf(b, "    v%u = *(%s*)(void*)(uintptr_t)", dst_idx, cast);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;
        }
        case XIR_STORE: {
            uint8_t val_t = xcg_ref_type(func, ins->args[1]);
            const char *cast = (val_t == XR_REP_F64) ? "double" : "int64_t";
            xcgen_buf_printf(b, "    *(%s*)(void*)(uintptr_t)", cast);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, " = ");
            xcg_emit_ref(b, func, ins->args[1]);
            xcgen_buf_puts(b, ";\n");
            return;
        }
        case XIR_LOAD8Z:
            xcgen_buf_printf(b, "    v%u = (int64_t)*(uint8_t*)(void*)(uintptr_t)", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_LOAD8S:
            xcgen_buf_printf(b, "    v%u = (int64_t)(int8_t)*(uint8_t*)(void*)(uintptr_t)",
                             dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_STORE8:
            xcgen_buf_puts(b, "    *(uint8_t*)(void*)(uintptr_t)");
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, " = (uint8_t)");
            xcg_emit_ref(b, func, ins->args[1]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_LOAD16Z:
            xcgen_buf_printf(b, "    v%u = (int64_t)*(uint16_t*)(void*)(uintptr_t)", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_LOAD16S:
            xcgen_buf_printf(b, "    v%u = (int64_t)(int16_t)*(uint16_t*)(void*)(uintptr_t)",
                             dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_STORE16:
            xcgen_buf_puts(b, "    *(uint16_t*)(void*)(uintptr_t)");
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, " = (uint16_t)");
            xcg_emit_ref(b, func, ins->args[1]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_LOAD32Z:
            xcgen_buf_printf(b, "    v%u = (int64_t)*(uint32_t*)(void*)(uintptr_t)", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_STORE32:
            xcgen_buf_puts(b, "    *(uint32_t*)(void*)(uintptr_t)");
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, " = (uint32_t)");
            xcg_emit_ref(b, func, ins->args[1]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_LOAD_F32:
            xcgen_buf_printf(b, "    v%u = (double)*(float*)(void*)(uintptr_t)", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_STORE_F32:
            xcgen_buf_puts(b, "    *(float*)(void*)(uintptr_t)");
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, " = (float)");
            xcg_emit_ref(b, func, ins->args[1]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_ALLOC:
            if (!xir_ref_is_none(ins->dst)) {
                // args[0] = type tag (unused in AOT), args[1] = byte size
                xcgen_buf_printf(b, "    v%u = xrt_mkptr(xrt_arc_alloc((size_t)(", dst_idx);
                xcg_emit_ref(b, func, ins->args[1]);
                xcgen_buf_puts(b, ")), XRT_TAG_PTR);\n");
            }
            return;
        default:
            return;
    }
}

/* ========== Box/Unbox/Tag Helper ========== */

// Emit tagged value operations (BOX_I64, BOX_F64, UNBOX_I64, UNBOX_F64, TAG_LOAD, TAG_CHECK)
static void xcg_emit_box_op(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf) {
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    const char *tagged_type = "XrValue";
    switch (ins->op) {
        case XIR_BOX_I64:
            xcgen_buf_printf(b, "    v%u = xrt_box_int(", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ");\n");
            break;
        case XIR_BOX_F64:
            xcgen_buf_printf(b, "    v%u = xrt_box_float(", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ");\n");
            break;
        case XIR_UNBOX_I64:
            xcgen_buf_printf(b, "    v%u = xrt_unbox_int(", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ");\n");
            break;
        case XIR_UNBOX_F64:
            xcgen_buf_printf(b, "    v%u = xrt_unbox_float(", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ");\n");
            break;
        case XIR_TAG_LOAD:
            xcgen_buf_printf(b, "    v%u = ((%s*)&v%u)->tag;\n", dst_idx, tagged_type,
                             XIR_REF_INDEX(ins->args[0]));
            break;
        case XIR_TAG_CHECK:
            xcgen_buf_printf(b, "    if (((%s*)&v%u)->tag != ", tagged_type,
                             XIR_REF_INDEX(ins->args[0]));
            xcg_emit_ref(b, func, ins->args[1]);
            xcgen_buf_puts(b, ") { __builtin_trap(); }\n");
            break;
        default:
            return;
    }
    cf->needs_runtime = true;
}

/* ========== Constant/Move Helpers ========== */

// Emit CONST_PTR instruction (string literal, null, or raw pointer)
static void xcg_emit_const_ptr(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf) {
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    const char *tagged_type = "XrValue";
    uint8_t vtype = XR_REP_PTR;
    if (xir_ref_is_vreg(ins->dst)) {
        uint32_t vi = XIR_REF_INDEX(ins->dst);
        if (vi < func->nvreg)
            vtype = func->vregs[vi].rep;
    }
    // Check if underlying const is a string: either XR_REP_STR const, or a
    // raw PTR const that points to a live XrString* (from bytecode const pool).
    bool const_is_str = false;
    if (xir_ref_is_const(ins->args[0])) {
        uint32_t ci = XIR_REF_INDEX(ins->args[0]);
        if (ci < func->nconst) {
            if (func->consts[ci].rep == XR_REP_STR) {
                const_is_str = true;
            } else if (func->consts[ci].rep == XR_REP_PTR) {
                void *p = (void *) (uintptr_t) func->consts[ci].val.raw;
                if (p && XR_GC_GET_TYPE((XrGCHeader *) p) == XR_TSTRING)
                    const_is_str = true;
            }
        }
    }
    if (vtype == XR_REP_STR || const_is_str) {
        xcgen_buf_printf(b, "    v%u = xrt_box_str(", dst_idx);
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
    } else if (vtype == XR_REP_PTR || vtype == XR_REP_TAGGED) {
        int64_t raw = 0;
        if (xir_ref_is_const(ins->args[0])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            if (ci < func->nconst)
                raw = func->consts[ci].val.i64;
        }
        if (raw == 0) {
            xcgen_buf_printf(b, "    v%u = xrt_mkptr(NULL, XRT_TAG_NULL);\n", dst_idx);
        } else {
            xcgen_buf_printf(b, "    v%u = xrt_mkptr((void*)0x%" PRIx64 ", XRT_TAG_PTR);\n",
                             dst_idx, (uint64_t) raw);
        }
        cf->needs_runtime = true;
    } else {
        xcgen_buf_printf(b, "    v%u = ", dst_idx);
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ";\n");
    }
}

// Emit MOV instruction with auto-boxing/unboxing between typed and tagged reps
static void xcg_emit_mov(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf) {
    if (xir_ref_is_none(ins->dst) || xir_ref_is_none(ins->args[0]))
        return;
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    uint8_t dst_t = xcg_ref_type(func, ins->dst);
    uint8_t src_t = xcg_ref_type(func, ins->args[0]);
    bool src_tagged = (src_t == XR_REP_STR || src_t == XR_REP_PTR || src_t == XR_REP_TAGGED);
    bool dst_tagged = (dst_t == XR_REP_STR || dst_t == XR_REP_PTR || dst_t == XR_REP_TAGGED);
    if (src_tagged && dst_t == XR_REP_I64) {
        xcgen_buf_printf(b, "    v%u = xrt_unbox_int(", dst_idx);
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
    } else if (src_tagged && dst_t == XR_REP_F64) {
        xcgen_buf_printf(b, "    v%u = xrt_unbox_float(", dst_idx);
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
    } else if (!src_tagged && dst_tagged && src_t == XR_REP_I64) {
        xcgen_buf_printf(b, "    v%u = xrt_box_int(", dst_idx);
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
    } else if (!src_tagged && dst_tagged && src_t == XR_REP_F64) {
        xcgen_buf_printf(b, "    v%u = xrt_box_float(", dst_idx);
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
    } else if (src_tagged && dst_tagged) {
        // ARC: retain new value, release old, then assign
        xcgen_buf_puts(b, "    xrt_arc_retain_val(");
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_printf(b, "); xrt_arc_release_val(v%u);\n", dst_idx);
        xcgen_buf_printf(b, "    v%u = ", dst_idx);
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ";\n");
        cf->needs_runtime = true;
    } else {
        xcgen_buf_printf(b, "    v%u = ", dst_idx);
        xcg_emit_ref(b, func, ins->args[0]);
        xcgen_buf_puts(b, ";\n");
    }
}

/* ========== Instruction Translation ========== */

void xcg_emit_instruction(XcgenBuf *b, XirFunc *func, XirIns *ins, const char *self_name,
                          XcgenModule *mod, XcgenFunc *cf) {
    XR_DCHECK(b != NULL, "xcg_emit_instruction: b is NULL");
    XR_DCHECK(func != NULL, "xcg_emit_instruction: func is NULL");
    XR_DCHECK(ins != NULL, "xcg_emit_instruction: ins is NULL");
    // Delegate call-related instructions to xcgen_call.c
    if (xcg_emit_call_instruction(b, func, ins, self_name, mod, cf))
        return;

    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    switch (ins->op) {
        // --- Arithmetic ---
        case XIR_ADD:
            xcg_emit_binary_op(b, func, ins, "+");
            return;
        case XIR_SUB:
            xcg_emit_binary_op(b, func, ins, "-");
            return;
        case XIR_MUL:
            xcg_emit_binary_op(b, func, ins, "*");
            return;
        case XIR_DIV:
            xcg_emit_binary_op(b, func, ins, "/");
            return;
        case XIR_MOD:
            xcg_emit_binary_op(b, func, ins, "%");
            return;
        case XIR_AND:
            xcg_emit_binary_op(b, func, ins, "&");
            return;
        case XIR_OR:
            xcg_emit_binary_op(b, func, ins, "|");
            return;
        case XIR_XOR:
            xcg_emit_binary_op(b, func, ins, "^");
            return;
        case XIR_SHL:
            xcg_emit_binary_op(b, func, ins, "<<");
            return;
        case XIR_SHR:
            xcg_emit_binary_op(b, func, ins, ">>");
            return;

        case XIR_FADD:
            xcg_emit_binary_op(b, func, ins, "+");
            return;
        case XIR_FSUB:
            xcg_emit_binary_op(b, func, ins, "-");
            return;
        case XIR_FMUL:
            xcg_emit_binary_op(b, func, ins, "*");
            return;
        case XIR_FDIV:
            xcg_emit_binary_op(b, func, ins, "/");
            return;

        // --- Unary ---
        case XIR_NEG:
        case XIR_FNEG:
            xcgen_buf_printf(b, "    v%u = -", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_NOT:
            xcgen_buf_printf(b, "    v%u = ~", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;

        // --- Type conversion ---
        case XIR_I2F:
            xcgen_buf_printf(b, "    v%u = (double)", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_F2I:
            xcgen_buf_printf(b, "    v%u = (int64_t)", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;

        // --- Integer comparison ---
        case XIR_EQ:
            xcg_emit_compare_op(b, func, ins, "==");
            return;
        case XIR_NE:
            xcg_emit_compare_op(b, func, ins, "!=");
            return;
        case XIR_LT:
            xcg_emit_compare_op(b, func, ins, "<");
            return;
        case XIR_LE:
            xcg_emit_compare_op(b, func, ins, "<=");
            return;
        case XIR_GT:
            xcg_emit_compare_op(b, func, ins, ">");
            return;
        case XIR_GE:
            xcg_emit_compare_op(b, func, ins, ">=");
            return;

        // --- Float comparison (result is int64_t 0/1) ---
        case XIR_FEQ:
            xcg_emit_compare_op(b, func, ins, "==");
            return;
        case XIR_FNE:
            xcg_emit_compare_op(b, func, ins, "!=");
            return;
        case XIR_FLT:
            xcg_emit_compare_op(b, func, ins, "<");
            return;
        case XIR_FLE:
            xcg_emit_compare_op(b, func, ins, "<=");
            return;

        // --- Constants ---
        case XIR_CONST_I64:
        case XIR_CONST_F64:
            xcgen_buf_printf(b, "    v%u = ", dst_idx);
            xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, ";\n");
            return;
        case XIR_CONST_PTR:
            xcg_emit_const_ptr(b, func, ins, cf);
            return;

        // --- Move → delegate to helper ---
        case XIR_MOV:
            xcg_emit_mov(b, func, ins, cf);
            return;

        // --- Tagged value operations (BOX/UNBOX/TAG) → delegate to helper ---
        case XIR_BOX_I64:
        case XIR_BOX_F64:
        case XIR_UNBOX_I64:
        case XIR_UNBOX_F64:
        case XIR_TAG_LOAD:
        case XIR_TAG_CHECK:
            xcg_emit_box_op(b, func, ins, cf);
            return;

        // --- Runtime mixed-type operations → delegate to helper ---
        case XIR_RT_ADD:
        case XIR_RT_SUB:
        case XIR_RT_MUL:
        case XIR_RT_DIV:
        case XIR_RT_MOD:
        case XIR_RT_UNM:
        case XIR_RT_LT:
        case XIR_RT_LE:
        case XIR_RT_EQ:
            xcg_emit_rt_arith(b, func, ins, cf);
            return;

        // --- Print → delegate to helper ---
        case XIR_RT_PRINT:
            xcg_emit_print_op(b, func, ins, cf);
            return;

        // --- Array / Index / Map / Null → delegate to helper ---
        case XIR_RT_ARRAY_NEW:
        case XIR_RT_ARRAY_PUSH:
        case XIR_RT_ARRAY_LEN:
        case XIR_RT_INDEX_GET:
        case XIR_RT_INDEX_SET:
        case XIR_RT_MAP_NEW:
        case XIR_RT_ISNULL:
            xcg_emit_collection_op(b, func, ins, cf);
            return;

        // --- Memory / Field access → delegate to helpers ---
        case XIR_LOAD_FIELD:
            xcg_emit_field_load(b, func, ins, mod, cf);
            return;
        case XIR_STORE_FIELD:
            xcg_emit_field_store(b, func, ins, mod, cf);
            return;

        // --- Exception handling ---
        case XIR_TRY_BEGIN:
        case XIR_TRY_END:
            return;
        case XIR_CATCH:
            xcgen_buf_printf(b, "    v%u = xrt_exception;\n", dst_idx);
            cf->needs_exception = true;
            return;
        case XIR_THROW:
            xcgen_buf_puts(b, "    abort(); /* unexpected throw */\n");
            return;

        // --- Closure upvalue access → delegate to helpers ---
        case XIR_LOAD_UPVAL:
            xcg_emit_upval_load(b, func, ins, cf);
            return;
        case XIR_STORE_UPVAL:
            xcg_emit_upval_store(b, func, ins, mod, cf);
            return;

        // --- Defer ---
        case XIR_DEFER_PUSH: {
            // Save closure and args to defer locals, set active flag.
            // defer_idx is a static counter per function (cf->defer_count tracks total).
            // Each XIR_DEFER_PUSH corresponds to one defer entry in order.
            int di = -1;
            XirRef cl_ref = ins->args[0];
            // Find matching defer entry by closure ref
            for (int k = 0; k < func->defer_count; k++) {
                if (func->defer_entries[k].closure == cl_ref) {
                    di = k;
                    break;
                }
            }
            if (di < 0)
                return;  // safety: no match
            xcgen_buf_printf(b, "    _defer_%d = ", di);
            xcg_emit_ref_as_tagged(b, func, cl_ref);
            xcgen_buf_puts(b, ";\n");
            int nargs = func->defer_entries[di].arg_count;
            for (int ai = 0; ai < nargs; ai++) {
                xcgen_buf_printf(b, "    _defer_%d_arg%d = ", di, ai);
                xcg_emit_ref_as_tagged(b, func, func->defer_entries[di].args[ai]);
                xcgen_buf_puts(b, ";\n");
            }
            xcgen_buf_printf(b, "    _defer_%d_set = 1;\n", di);
            return;
        }

        // --- ARC retain/release ---
        case XIR_RETAIN:
        case XIR_RELEASE: {
            uint8_t t = xcg_ref_type(func, ins->args[0]);
            bool tagged = (t == XR_REP_PTR || t == XR_REP_TAGGED || t == XR_REP_STR);
            if (tagged) {
                xcgen_buf_printf(b, "    %s(",
                                 ins->op == XIR_RETAIN ? "xrt_arc_retain_val"
                                                       : "xrt_arc_release_val");
                xcg_emit_ref(b, func, ins->args[0]);
                xcgen_buf_puts(b, ");\n");
            }
            return;
        }

        // --- No-op categories: guards, GC barriers ---
        case XIR_GUARD_TAG:
        case XIR_GUARD_CLASS:
        case XIR_GUARD_NONNULL:
        case XIR_DEOPT:
        case XIR_BARRIER_FWD:
        case XIR_BARRIER_BACK:
            return;

        // --- Raw memory ops + allocation → delegate to helper ---
        case XIR_LOAD:
        case XIR_STORE:
        case XIR_LOAD8Z:
        case XIR_LOAD8S:
        case XIR_STORE8:
        case XIR_LOAD16Z:
        case XIR_LOAD16S:
        case XIR_STORE16:
        case XIR_LOAD32Z:
        case XIR_STORE32:
        case XIR_LOAD_F32:
        case XIR_STORE_F32:
        case XIR_ALLOC:
            xcg_emit_raw_mem_op(b, func, ins);
            return;

        // --- Conditional select (from if-conversion) ---
        case XIR_SELECT_COND:
            // Store condition ref for the upcoming XIR_SELECT.
            cf->last_select_cond = ins->args[0];
            return;
        case XIR_SELECT: {
            // dst = cond ? args[0] : args[1]   (NE semantics: non-zero picks args[0])
            uint8_t dst_t = (dst_idx < func->nvreg) ? func->vregs[dst_idx].rep : XR_REP_I64;
            bool tagged = (dst_t == XR_REP_TAGGED || dst_t == XR_REP_PTR || dst_t == XR_REP_STR);
            xcgen_buf_printf(b, "    v%u = ", dst_idx);
            xcg_emit_ref(b, func, cf->last_select_cond);
            xcgen_buf_puts(b, " ? ");
            if (tagged)
                xcg_emit_ref_as_tagged(b, func, ins->args[0]);
            else
                xcg_emit_ref(b, func, ins->args[0]);
            xcgen_buf_puts(b, " : ");
            if (tagged)
                xcg_emit_ref_as_tagged(b, func, ins->args[1]);
            else
                xcg_emit_ref(b, func, ins->args[1]);
            xcgen_buf_puts(b, ";\n");
            return;
        }

        // --- No-ops in C ---
        case XIR_SAFEPOINT:
        case XIR_NOP:
        case XIR_PHI:
            return;

        default:
            xcgen_buf_printf(b, "    /* TODO: op %u (%s) */\n", ins->op, xir_op_name(ins->op));
            return;
    }
}
