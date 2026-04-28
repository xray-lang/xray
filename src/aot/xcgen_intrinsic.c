/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen_intrinsic.c - AOT C code generator: intrinsic call lowering
 *
 * KEY CONCEPT:
 *   Translates XIR_CALL_INTRINSIC instructions into inline C code.
 *   Each intrinsic (XirIntrinsicId from xir_intrinsic.h) has a dedicated
 *   lowering case that emits optimized C — type-specialized where possible,
 *   with fallback to xrt_method_N runtime dispatch.
 *
 *   Intrinsic IDs are resolved from const refs after xir_resolve_intrinsics()
 *   has converted JIT fn_ptr CALL_C into CALL_INTRINSIC(id).
 *
 * RELATED MODULES:
 *   - xcgen_call.c: dispatches CALL_INTRINSIC here via xcg_emit_call_intrinsic
 *   - xcgen.h: shared types, xcg_emit_ref / xcg_emit_ref_as_tagged
 *   - xir_intrinsic.h: XirIntrinsicId enum definitions
 */

#include "xcgen.h"
#include "../base/xchecks.h"
#include "../jit/xir_intrinsic.h"
#include "xrt_method_symbols.h"
#include <stdio.h>
#include <string.h>

static const char *const tagged_type = "XrValue";

/* ========== Shared Emit Helper ========== */

// Auto-box int64_t/double refs to XrtValue for runtime calls.
// Defined here as the canonical implementation; declared in xcgen.h.
XR_FUNC void xcg_emit_ref_as_tagged(XcgenBuf *b, XirFunc *func, XirRef ref) {
    XR_DCHECK(b != NULL, "xcg_emit_ref_as_tagged: NULL buf");
    XR_DCHECK(func != NULL, "xcg_emit_ref_as_tagged: NULL func");
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

/* ========== Intrinsic ID Resolution ========== */

// After xir_resolve_intrinsics(), args[0] is a const i64 holding the ID.
static int resolve_intrinsic_id(XirFunc *func, XirIns *ins) {
    int64_t id = XR_INTRIN_NONE;
    if (xir_ref_is_const(ins->args[0])) {
        uint32_t ci = XIR_REF_INDEX(ins->args[0]);
        if (ci < func->nconst)
            id = func->consts[ci].val.i64;
    }
    return (int) id;
}

/* ========== Method Invoke Lowering ========== */

// Emit inlined C for INVOKE_METHOD intrinsic — type-specialized fast paths
// for math/string/array/map, with fallback to xrt_method_N runtime dispatch.
static void emit_invoke_method(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf,
                               XcgenModule *mod);

// Inline string 0-arg methods (length/isEmpty). Returns true if handled.
static bool emit_str_method_0(XcgenBuf *b, XirFunc *func, XcgenFunc *cf, XirRef recv_ref,
                              uint32_t dst_idx, uint8_t dst_vtype, int method_symbol) {
    bool dst_is_int = (dst_vtype == XR_REP_I64);
    if (dst_is_int && (method_symbol == XRT_SYM_LENGTH || method_symbol == XRT_SYM_SIZE)) {
        xcgen_buf_printf(b, "    v%u = (int64_t)strlen((const char *)", dst_idx);
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ".ptr);\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return true;
    }
    if ((dst_is_int || dst_vtype == XR_REP_TAGGED) && method_symbol == XRT_SYM_IS_EMPTY) {
        if (dst_vtype == XR_REP_TAGGED)
            xcgen_buf_printf(b, "    v%u = xrt_box_bool(", dst_idx);
        else
            xcgen_buf_printf(b, "    v%u = ", dst_idx);
        xcgen_buf_puts(b, "(((const char *)");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ".ptr)[0] == '\\0') ? 1 : 0");
        if (dst_vtype == XR_REP_TAGGED)
            xcgen_buf_puts(b, ");");
        else
            xcgen_buf_puts(b, ";");
        xcgen_buf_puts(b, "\n");
        cf->call_args_count = 0;
        return true;
    }
    return false;
}

// Inline string 1-arg methods (contains/indexOf/startsWith/endsWith). Returns true if handled.
static bool emit_str_method_1(XcgenBuf *b, XirFunc *func, XcgenFunc *cf, XirRef recv_ref,
                              uint32_t dst_idx, uint8_t dst_vtype, int method_symbol) {
    bool dst_is_int = (dst_vtype == XR_REP_I64);
    XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
    uint8_t a1t = xir_ref_is_none(arg1) ? XR_REP_TAGGED : xcg_ref_type(func, arg1);
    bool a1_str = (a1t == XR_REP_STR);
    if ((dst_is_int || dst_vtype == XR_REP_TAGGED) && a1_str && method_symbol == XRT_SYM_CONTAINS) {
        if (dst_vtype == XR_REP_TAGGED)
            xcgen_buf_printf(b, "    v%u = xrt_box_bool(", dst_idx);
        else
            xcgen_buf_printf(b, "    v%u = ", dst_idx);
        xcgen_buf_puts(b, "strstr((const char *)");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ".ptr, (const char *)");
        xcg_emit_ref(b, func, arg1);
        xcgen_buf_puts(b, ".ptr) ? 1 : 0");
        if (dst_vtype == XR_REP_TAGGED)
            xcgen_buf_puts(b, ");");
        else
            xcgen_buf_puts(b, ";");
        xcgen_buf_puts(b, "\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return true;
    }
    if (dst_is_int && a1_str && method_symbol == XRT_SYM_INDEXOF) {
        xcgen_buf_puts(b, "    { const char *_s = (const char *)");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ".ptr; const char *_p = strstr(_s, (const char *)");
        xcg_emit_ref(b, func, arg1);
        xcgen_buf_printf(b, ".ptr); v%u = _p ? (int64_t)(_p - _s) : -1; }\n", dst_idx);
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return true;
    }
    if ((dst_is_int || dst_vtype == XR_REP_TAGGED) && a1_str &&
        method_symbol == XRT_SYM_STARTSWITH) {
        xcgen_buf_printf(b, "    { const char *_s = (const char *)");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ".ptr; const char *_p = (const char *)");
        xcg_emit_ref(b, func, arg1);
        xcgen_buf_puts(b, ".ptr; size_t _pl = strlen(_p); ");
        if (dst_vtype == XR_REP_TAGGED)
            xcgen_buf_printf(b,
                             "v%u = xrt_box_bool((strlen(_s) >= _pl && "
                             "memcmp(_s, _p, _pl) == 0) ? 1 : 0); }\n",
                             dst_idx);
        else
            xcgen_buf_printf(b,
                             "v%u = (strlen(_s) >= _pl && memcmp(_s, _p, _pl) "
                             "== 0) ? 1 : 0; }\n",
                             dst_idx);
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return true;
    }
    if ((dst_is_int || dst_vtype == XR_REP_TAGGED) && a1_str && method_symbol == XRT_SYM_ENDSWITH) {
        xcgen_buf_printf(b, "    { const char *_s = (const char *)");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ".ptr; size_t _sl = strlen(_s); const char *_p = (const char *)");
        xcg_emit_ref(b, func, arg1);
        xcgen_buf_puts(b, ".ptr; size_t _pl = strlen(_p); ");
        if (dst_vtype == XR_REP_TAGGED)
            xcgen_buf_printf(b,
                             "v%u = xrt_box_bool((_sl >= _pl && memcmp(_s + "
                             "_sl - _pl, _p, _pl) == 0) ? 1 : 0); }\n",
                             dst_idx);
        else
            xcgen_buf_printf(b,
                             "v%u = (_sl >= _pl && memcmp(_s + _sl - _pl, _p, "
                             "_pl) == 0) ? 1 : 0; }\n",
                             dst_idx);
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return true;
    }
    return false;
}

// Inline array/map method fast paths. Returns true if handled.
static bool emit_coll_method(XcgenBuf *b, XirFunc *func, XcgenFunc *cf, XirRef recv_ref,
                             uint32_t dst_idx, uint8_t dst_vtype, int method_symbol, int nargs) {
    if (nargs == 1 && method_symbol == XRT_SYM_PUSH) {
        XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
        xcgen_buf_puts(b, "    xrt_array_push(");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ", ");
        xcg_emit_ref_as_tagged(b, func, arg1);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return true;
    }
    if (nargs == 0 && method_symbol == XRT_SYM_POP) {
        xcgen_buf_printf(b, "    { xrt_array_t *_a = (xrt_array_t *)");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_printf(b,
                         ".ptr; v%u = (_a->len > 0) ? _a->data[--_a->len] "
                         ": (XrtValue){0}; }\n",
                         dst_idx);
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return true;
    }
    if (nargs == 1 && method_symbol == XRT_SYM_GET) {
        XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
        xcgen_buf_printf(b, "    v%u = xrt_map_get((xrt_map_t *)", dst_idx);
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ".ptr, ");
        xcg_emit_ref_as_tagged(b, func, arg1);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return true;
    }
    if (nargs == 2 && method_symbol == XRT_SYM_SET) {
        XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
        XirRef arg2 = (cf->call_args_count > 2) ? cf->call_args[2] : XIR_NONE;
        xcgen_buf_puts(b, "    xrt_map_set((xrt_map_t *)");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ".ptr, ");
        xcg_emit_ref_as_tagged(b, func, arg1);
        xcgen_buf_puts(b, ", ");
        xcg_emit_ref_as_tagged(b, func, arg2);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return true;
    }
    if (nargs == 1 && method_symbol == XRT_SYM_HAS) {
        XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
        xcgen_buf_printf(b, "    { xrt_map_t *_m = (xrt_map_t *)");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ".ptr; int64_t _found = 0; ");
        xcgen_buf_puts(b, "for (int64_t _i = 0; _i < _m->len; _i++) ");
        xcgen_buf_puts(b, "if (xrt_key_eq(_m->entries[_i].key, ");
        xcg_emit_ref_as_tagged(b, func, arg1);
        if (dst_vtype == XR_REP_TAGGED)
            xcgen_buf_printf(b, ")) { _found = 1; break; } v%u = xrt_box_bool(_found); }\n",
                             dst_idx);
        else
            xcgen_buf_printf(b, ")) { _found = 1; break; } v%u = _found; }\n", dst_idx);
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return true;
    }
    return false;
}

// Fallback: emit xrt_method_0/1/2 runtime dispatch call
static void emit_method_fallback(XcgenBuf *b, XirFunc *func, XcgenFunc *cf, uint32_t dst_idx,
                                 bool dst_is_float, bool dst_is_int, int method_symbol, int nargs) {
    if (nargs == 0) {
        if (dst_is_float)
            xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_method_0(", dst_idx);
        else if (dst_is_int)
            xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_method_0(", dst_idx);
        else
            xcgen_buf_printf(b, "    v%u = xrt_method_0(", dst_idx);
        if (cf->call_args_count > 0)
            xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_printf(b, ", %d", method_symbol);
    } else if (nargs == 1) {
        if (dst_is_float)
            xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_method_1(", dst_idx);
        else if (dst_is_int)
            xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_method_1(", dst_idx);
        else
            xcgen_buf_printf(b, "    v%u = xrt_method_1(", dst_idx);
        if (cf->call_args_count > 0)
            xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_printf(b, ", %d", method_symbol);
        if (cf->call_args_count > 1) {
            xcgen_buf_puts(b, ", ");
            xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
        } else {
            xcgen_buf_printf(b, ", (%s){0}", tagged_type);
        }
    } else {
        if (dst_is_float)
            xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_method_2(", dst_idx);
        else if (dst_is_int)
            xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_method_2(", dst_idx);
        else
            xcgen_buf_printf(b, "    v%u = xrt_method_2(", dst_idx);
        if (cf->call_args_count > 0)
            xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_printf(b, ", %d", method_symbol);
        for (int ai = 0; ai < 2; ai++) {
            xcgen_buf_puts(b, ", ");
            if (ai + 1 < cf->call_args_count)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[ai + 1]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
        }
    }
    if (dst_is_float || dst_is_int)
        xcgen_buf_puts(b, "));\n");
    else
        xcgen_buf_puts(b, ");\n");
    cf->needs_runtime = true;
    cf->call_args_count = 0;
}

/* ========== Intrinsic Lowering ========== */

XR_FUNC void xcg_emit_call_intrinsic(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf,
                                     XcgenModule *mod) {
    XR_DCHECK(b != NULL, "xcg_emit_call_intrinsic: NULL buf");
    XR_DCHECK(func != NULL, "xcg_emit_call_intrinsic: NULL func");
    XR_DCHECK(ins != NULL, "xcg_emit_call_intrinsic: NULL ins");
    int intrin = resolve_intrinsic_id(func, ins);

    switch (intrin) {
        case XR_INTRIN_JSON_NEW_SHAPE: {
            if (!mod->struct_reg) {
                cf->call_args_count = 0;
                return;
            }
            void *shape_ptr = NULL;
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t si = XIR_REF_INDEX(ins->args[1]);
                if (si < func->nconst)
                    shape_ptr = func->consts[si].val.ptr;
            }
            int sidx = shape_ptr ? xcgen_find_struct(mod->struct_reg, shape_ptr) : -1;
            if (sidx >= 0) {
                XcgenStruct *st = &mod->struct_reg->structs[sidx];
                uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
                xcgen_buf_printf(
                    b, "    v%u = (%s){.ptr = xrt_arc_alloc(sizeof(%s)), .tag = XRT_TAG_PTR};\n",
                    dst_idx, tagged_type, st->c_name);
                if (!st->all_native) {
                    xcgen_buf_printf(b,
                                     "    { XrtArcHdr *_h = XRT_ARC_HDR(v%u.ptr);"
                                     " _h->type = %d; _h->flags |= XRT_ARC_HAS_DEINIT; }\n",
                                     dst_idx, sidx);
                }
                if (cf->vreg_struct_id && dst_idx < func->nvreg)
                    cf->vreg_struct_id[dst_idx] = (int16_t) sidx;
            }
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_GETPROP: {
            uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
            int64_t symbol_id = -1;
            xcg_resolve_const_i64(func, ins->args[1], &symbol_id);
            if (mod->struct_reg) {
                const XcgenFieldEntry *fe =
                    xcgen_find_field_by_symbol(mod->struct_reg, (uint32_t) symbol_id);
                if (fe) {
                    XcgenStruct *st = &mod->struct_reg->structs[fe->struct_idx];
                    xcgen_buf_printf(b, "    v%u = ((%s*)v%u.ptr)->%s;\n", dst_idx, st->c_name,
                                     XIR_REF_INDEX(cf->call_args[0]),
                                     st->fields[fe->field_idx].name);
                    cf->call_args_count = 0;
                    return;
                }
            }
            uint8_t dst_type = xcg_ref_type(func, ins->dst);
            if ((symbol_id == 1 || symbol_id == 2 || symbol_id == 3) && cf->call_args_count >= 1) {
                if (dst_type == XR_REP_I64) {
                    xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_printf(b, ", %lldLL).i;\n", (long long) symbol_id);
                } else {
                    xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_printf(b, ", %lldLL);\n", (long long) symbol_id);
                }
                cf->needs_runtime = true;
                cf->call_args_count = 0;
                return;
            }
            if (cf->call_args_count >= 1) {
                if (dst_type == XR_REP_I64) {
                    xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_printf(b, ", %lldLL).i;\n", (long long) symbol_id);
                } else if (dst_type == XR_REP_F64) {
                    xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_printf(b, ", %lldLL).f;\n", (long long) symbol_id);
                } else {
                    xcgen_buf_printf(b, "    v%u = xrt_getprop(", dst_idx);
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_printf(b, ", %lldLL);\n", (long long) symbol_id);
                }
                cf->needs_runtime = true;
            } else {
                if (dst_type == XR_REP_I64)
                    xcgen_buf_printf(b, "    v%u = 0;\n", dst_idx);
                else if (dst_type == XR_REP_F64)
                    xcgen_buf_printf(b, "    v%u = 0.0;\n", dst_idx);
                else {
                    xcgen_buf_printf(b, "    v%u = (%s){.i = 0, .tag = XRT_TAG_NULL};\n", dst_idx,
                                     tagged_type);
                    cf->needs_runtime = true;
                }
            }
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_INDEX_GET: {
            uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
            if (cf->call_args_count >= 2) {
                uint8_t dst_type = xcg_ref_type(func, ins->dst);
                uint8_t key_type = xcg_ref_type(func, cf->call_args[1]);
                bool key_is_i64 = (key_type == XR_REP_I64);
                if (dst_type == XR_REP_I64 && key_is_i64) {
                    xcgen_buf_printf(b, "    v%u = xrt_array_get_i(", dst_idx);
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ");\n");
                } else if (dst_type == XR_REP_F64 && key_is_i64) {
                    xcgen_buf_printf(b, "    v%u = xrt_array_get_f(", dst_idx);
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ");\n");
                } else if (dst_type == XR_REP_I64) {
                    xcgen_buf_printf(b, "    v%u = xrt_unbox_int(xrt_index_get(", dst_idx);
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, "));\n");
                } else if (dst_type == XR_REP_F64) {
                    xcgen_buf_printf(b, "    v%u = xrt_unbox_float(xrt_index_get(", dst_idx);
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, "));\n");
                } else {
                    xcgen_buf_printf(b, "    v%u = xrt_index_get(", dst_idx);
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ");\n");
                }
            } else {
                xcgen_buf_printf(b, "    v%u = xrt_index_get(", dst_idx);
                if (cf->call_args_count > 0)
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                else
                    xcgen_buf_printf(b, "(%s){0}", tagged_type);
                xcgen_buf_puts(b, ", ");
                if (cf->call_args_count > 1)
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
                else
                    xcgen_buf_printf(b, "(%s){0}", tagged_type);
                xcgen_buf_puts(b, ");\n");
            }
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_INDEX_SET: {
            if (cf->call_args_count >= 3) {
                uint8_t key_type = xcg_ref_type(func, cf->call_args[1]);
                uint8_t val_type = xcg_ref_type(func, cf->call_args[2]);
                bool key_is_i64 = (key_type == XR_REP_I64);
                if (key_is_i64 && val_type == XR_REP_I64) {
                    xcgen_buf_puts(b, "    xrt_array_set_i(");
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[2]);
                    xcgen_buf_puts(b, ");\n");
                } else if (key_is_i64 && val_type == XR_REP_F64) {
                    xcgen_buf_puts(b, "    xrt_array_set_f(");
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref(b, func, cf->call_args[2]);
                    xcgen_buf_puts(b, ");\n");
                } else {
                    xcgen_buf_puts(b, "    xrt_index_set(");
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
                    xcgen_buf_puts(b, ", ");
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[2]);
                    xcgen_buf_puts(b, ");\n");
                }
            } else {
                xcgen_buf_puts(b, "    xrt_index_set(");
                xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                xcgen_buf_puts(b, ", ");
                xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
                xcgen_buf_puts(b, ", ");
                if (cf->call_args_count > 2)
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[2]);
                else
                    xcgen_buf_printf(b, "(%s){0}", tagged_type);
                xcgen_buf_puts(b, ");\n");
            }
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_THROW: {
            XirIns *throw_ins = ins;
            bool has_local_catch = false;
            for (uint32_t tbi = 0; tbi < func->nblk; tbi++) {
                XirBlock *tblk = func->blocks[tbi];
                for (uint32_t ti = 0; ti < tblk->nins; ti++) {
                    if (&tblk->ins[ti] == throw_ins) {
                        has_local_catch = (tblk->exception_handler != NULL);
                        goto throw_done;
                    }
                }
            }
        throw_done:
            if (has_local_catch) {
                xcgen_buf_puts(b, "    xrt_exception = ");
                xcg_emit_ref_as_tagged(b, func, ins->args[1]);
                xcgen_buf_puts(b, ";\n");
                cf->needs_exception = true;
            } else {
                xcgen_buf_puts(b, "    xrt_throw_exc(");
                xcg_emit_ref_as_tagged(b, func, ins->args[1]);
                xcgen_buf_puts(b, ");\n");
            }
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_TARRAY_GET: {
            uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
            xcgen_buf_printf(b, "    v%u = xrt_tarray_get(", dst_idx);
            if (cf->call_args_count > 0)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ", ");
            if (cf->call_args_count > 1)
                xcg_emit_ref(b, func, cf->call_args[1]);
            else
                xcgen_buf_puts(b, "0");
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_TARRAY_SET: {
            xcgen_buf_puts(b, "    xrt_tarray_set(");
            if (cf->call_args_count > 0)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ", ");
            if (cf->call_args_count > 1)
                xcg_emit_ref(b, func, cf->call_args[1]);
            else
                xcgen_buf_puts(b, "0");
            xcgen_buf_puts(b, ", ");
            if (cf->call_args_count > 2)
                xcg_emit_ref(b, func, cf->call_args[2]);
            else
                xcgen_buf_puts(b, "0");
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_MAP_GET: {
            uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
            xcgen_buf_printf(b, "    v%u = xrt_map_get((xrt_map_t *)", dst_idx);
            if (cf->call_args_count > 0)
                xcg_emit_ref(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ".ptr, ");
            if (cf->call_args_count > 1)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_MAP_SET: {
            xcgen_buf_puts(b, "    xrt_map_set((xrt_map_t *)");
            if (cf->call_args_count > 0)
                xcg_emit_ref(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ".ptr, ");
            if (cf->call_args_count > 1)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ", ");
            if (cf->call_args_count > 2)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[2]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_MAP_INCREMENT: {
            xcgen_buf_puts(b, "    { xrt_map_t *_m = (xrt_map_t *)");
            if (cf->call_args_count > 0)
                xcg_emit_ref(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ".ptr; ");
            xcgen_buf_puts(b, "XrtValue _k = ");
            if (cf->call_args_count > 1)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, "; XrtValue _cv = xrt_map_get(_m, _k); ");
            xcgen_buf_puts(b, "xrt_map_set(_m, _k, xrt_box_int((_cv.tag == XRT_TAG_I64 ? _cv.i "
                              ": 0) + 1)); }\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_STRBUF_NEW: {
            uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
            xcgen_buf_printf(b, "    v%u = xrt_strbuf_new();\n", dst_idx);
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_STRBUF_APPEND: {
            xcgen_buf_puts(b, "    xrt_strbuf_append(");
            if (cf->call_args_count > 0)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ", ");
            if (cf->call_args_count > 1)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_STRBUF_FINISH: {
            uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
            xcgen_buf_printf(b, "    v%u = xrt_strbuf_finish(", dst_idx);
            if (cf->call_args_count > 0)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_INVOKE_METHOD:
            emit_invoke_method(b, func, ins, cf, mod);
            return;

        case XR_INTRIN_GET_SHARED: {
            int64_t shared_idx = -1;
            if (xcg_resolve_const_i64(func, ins->args[1], &shared_idx) && shared_idx >= 0) {
                uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
                uint8_t dst_type =
                    (dst_idx < func->nvreg) ? func->vregs[dst_idx].rep : XR_REP_TAGGED;
                bool dst_tagged =
                    (dst_type == XR_REP_STR || dst_type == XR_REP_PTR || dst_type == XR_REP_TAGGED);
                if (dst_tagged)
                    xcgen_buf_printf(b, "    v%u = xrt_shared[%d];\n", dst_idx, (int) shared_idx);
                else
                    xcgen_buf_printf(b, "    v%u = xrt_shared[%d].i;\n", dst_idx, (int) shared_idx);
                XcgenCompilation *comp = mod->comp;
                XR_DCHECK(comp != NULL, "xcg_emit_call_intrinsic GETSHARED: NULL comp");
                if ((int) shared_idx > comp->max_shared_index)
                    comp->max_shared_index = (int) shared_idx;
                cf->needs_runtime = true;
            } else {
                xcgen_buf_puts(b, "    /* GETSHARED: unresolved index */\n");
            }
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_SET_SHARED: {
            int64_t encoded = -1;
            if (xcg_resolve_const_i64(func, ins->args[1], &encoded) && encoded >= 0) {
                int shared_idx = (int) (encoded & 0xFFFF);
                xcgen_buf_printf(b, "    xrt_shared[%d] = ", shared_idx);
                if (cf->call_args_count > 0)
                    xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
                else
                    xcgen_buf_printf(b, "(%s){0}", tagged_type);
                xcgen_buf_puts(b, ";\n");
                XcgenCompilation *comp = mod->comp;
                XR_DCHECK(comp != NULL, "xcg_emit_call_intrinsic SETSHARED: NULL comp");
                if (shared_idx > comp->max_shared_index)
                    comp->max_shared_index = shared_idx;
                cf->needs_runtime = true;
            } else {
                xcgen_buf_puts(b, "    /* SETSHARED: unresolved encoded */\n");
            }
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_PRINT: {
            int64_t flags = 0;
            xcg_resolve_const_i64(func, ins->args[1], &flags);
            int newline = (int) (flags & 1);
            int add_space = (int) ((flags >> 1) & 1);
            if (add_space)
                xcgen_buf_puts(b, "    printf(\" \");\n");
            xcgen_buf_printf(b, "    %s(", newline ? "xrt_println" : "xrt_print");
            if (cf->call_args_count > 0)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_RT_ADD:
        case XR_INTRIN_RT_SUB:
        case XR_INTRIN_RT_MUL:
        case XR_INTRIN_RT_DIV:
        case XR_INTRIN_RT_MOD: {
            const char *arith_name = NULL;
            switch (intrin) {
                case XR_INTRIN_RT_ADD:
                    arith_name = "xrt_add";
                    break;
                case XR_INTRIN_RT_SUB:
                    arith_name = "xrt_sub";
                    break;
                case XR_INTRIN_RT_MUL:
                    arith_name = "xrt_mul";
                    break;
                case XR_INTRIN_RT_DIV:
                    arith_name = "xrt_div";
                    break;
                case XR_INTRIN_RT_MOD:
                    arith_name = "xrt_mod";
                    break;
                default:
                    arith_name = "xrt_add";
                    break;
            }
            uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
            uint8_t dst_type = (dst_idx < func->nvreg) ? func->vregs[dst_idx].rep : XR_REP_TAGGED;
            bool di = (dst_type == XR_REP_I64);
            bool df = (dst_type == XR_REP_F64);
            if (di)
                xcgen_buf_printf(b, "    v%u = xrt_unbox_int(%s(", dst_idx, arith_name);
            else if (df)
                xcgen_buf_printf(b, "    v%u = xrt_unbox_float(%s(", dst_idx, arith_name);
            else
                xcgen_buf_printf(b, "    v%u = %s(", dst_idx, arith_name);
            if (cf->call_args_count > 0)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ", ");
            if (cf->call_args_count > 1)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            if (di || df)
                xcgen_buf_puts(b, "));\n");
            else
                xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_SUBSTRING: {
            /* call_args: [0]=str, [1]=start, [2]=end */
            uint32_t di = XIR_REF_INDEX(ins->dst);
            xcgen_buf_printf(b, "    v%u = xrt_method_2(", di);
            if (cf->call_args_count > 0)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_printf(b, ", %d, ", XRT_SYM_SUBSTRING);
            if (cf->call_args_count > 1)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ", ");
            if (cf->call_args_count > 2)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[2]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_STR_REPEAT: {
            /* call_args: [0]=str, [1]=count */
            uint32_t di = XIR_REF_INDEX(ins->dst);
            xcgen_buf_printf(b, "    v%u = xrt_method_1(", di);
            if (cf->call_args_count > 0)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_printf(b, ", %d, ", XRT_SYM_REPEAT);
            if (cf->call_args_count > 1)
                xcg_emit_ref_as_tagged(b, func, cf->call_args[1]);
            else
                xcgen_buf_printf(b, "(%s){0}", tagged_type);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_CHR: {
            /* call_args: [0]=code_point (i64) */
            uint32_t di = XIR_REF_INDEX(ins->dst);
            xcgen_buf_printf(b, "    { char _cb[5] = {0}; int64_t _cp = ");
            if (cf->call_args_count > 0)
                xcg_emit_ref(b, func, cf->call_args[0]);
            else
                xcgen_buf_puts(b, "0");
            xcgen_buf_printf(b, "; if (_cp >= 0 && _cp < 128) { _cb[0] = (char)_cp; } ");
            xcgen_buf_printf(b, "v%u = xrt_str_concat(_cb, \"\"); }\n", di);
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }

        case XR_INTRIN_TYPEOF:
            /* No specialized AOT lowering yet */
            cf->call_args_count = 0;
            return;

        default:
            fprintf(stderr, "AOT: unhandled intrinsic %d\n", intrin);
            cf->call_args_count = 0;
            return;
    }
}

/* ========== INVOKE_METHOD Implementation ========== */

static void emit_invoke_method(XcgenBuf *b, XirFunc *func, XirIns *ins, XcgenFunc *cf,
                               XcgenModule *mod) {
    (void) mod;
    int64_t encoded = 0;
    xcg_resolve_const_i64(func, ins->args[1], &encoded);
    uint32_t dst_idx = XIR_REF_INDEX(ins->dst);
    uint8_t dst_vtype = (dst_idx < func->nvreg) ? func->vregs[dst_idx].rep : XR_REP_TAGGED;
    bool dst_is_float = xcg_is_float_type(dst_vtype);
    bool dst_is_int = (dst_vtype == XR_REP_I64);

    if (encoded < 0) {
        int slot_hint = (int) (-(encoded + 1));
        xcgen_buf_printf(b, "    v%u = xrt_tostring(", dst_idx);
        if (cf->call_args_count > 0)
            xcg_emit_ref_as_tagged(b, func, cf->call_args[0]);
        else
            xcgen_buf_printf(b, "(%s){0}", tagged_type);
        xcgen_buf_printf(b, ", %d);\n", slot_hint);
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    int method_symbol = (int) (encoded >> 32);
    int nargs = (int) (encoded & 0xFF);
    XirRef recv_ref = (cf->call_args_count > 0) ? cf->call_args[0] : XIR_NONE;
    uint8_t recv_type = xir_ref_is_none(recv_ref) ? XR_REP_TAGGED : xcg_ref_type(func, recv_ref);
    bool recv_is_float = xcg_is_float_type(recv_type);
    bool recv_is_int = (recv_type == XR_REP_I64);
    bool recv_is_str = (recv_type == XR_REP_STR);
    bool recv_is_tagged = (recv_type == XR_REP_PTR || recv_type == XR_REP_TAGGED);

    /* Inline float math: floor/ceil/round/abs/sqrt/pow */
    if (recv_is_float && dst_is_float && nargs == 1 && method_symbol == XRT_SYM_POW) {
        XirRef arg1 = (cf->call_args_count > 1) ? cf->call_args[1] : XIR_NONE;
        uint8_t a1t = xir_ref_is_none(arg1) ? XR_REP_TAGGED : xcg_ref_type(func, arg1);
        if (xcg_is_float_type(a1t) || a1t == XR_REP_I64) {
            xcgen_buf_printf(b, "    v%u = pow(", dst_idx);
            xcg_emit_ref(b, func, recv_ref);
            xcgen_buf_puts(b, ", ");
            xcg_emit_ref(b, func, arg1);
            xcgen_buf_puts(b, ");\n");
            cf->needs_runtime = true;
            cf->call_args_count = 0;
            return;
        }
    }
    const char *math_fn = NULL;
    if (recv_is_float && dst_is_float && nargs == 0) {
        switch (method_symbol) {
            case XRT_SYM_FLOOR:
                math_fn = "floor";
                break;
            case XRT_SYM_CEIL:
                math_fn = "ceil";
                break;
            case XRT_SYM_ROUND:
                math_fn = "round";
                break;
            case XRT_SYM_ABS:
                math_fn = "fabs";
                break;
            case XRT_SYM_SQRT:
                math_fn = "sqrt";
                break;
            default:
                break;
        }
    }
    if (math_fn) {
        xcgen_buf_printf(b, "    v%u = %s(", dst_idx, math_fn);
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ");\n");
        cf->needs_runtime = true;
        cf->call_args_count = 0;
        return;
    }

    /* Inline I64 abs */
    if (recv_is_int && dst_is_int && nargs == 0 && method_symbol == XRT_SYM_ABS) {
        xcgen_buf_printf(b, "    v%u = (", dst_idx);
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, " < 0) ? -(");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ") : ");
        xcg_emit_ref(b, func, recv_ref);
        xcgen_buf_puts(b, ";\n");
        cf->call_args_count = 0;
        return;
    }

    /* Inline string methods with known types */
    if (recv_is_str && nargs == 0) {
        if (emit_str_method_0(b, func, cf, recv_ref, dst_idx, dst_vtype, method_symbol))
            return;
    }
    if (recv_is_str && nargs == 1) {
        if (emit_str_method_1(b, func, cf, recv_ref, dst_idx, dst_vtype, method_symbol))
            return;
    }

    /* Inline array push/pop, map get/set/has */
    if (recv_is_tagged &&
        emit_coll_method(b, func, cf, recv_ref, dst_idx, dst_vtype, method_symbol, nargs))
        return;

    /* Fixed-arity fallback: xrt_method_N */
    emit_method_fallback(b, func, cf, dst_idx, dst_is_float, dst_is_int, method_symbol, nargs);
}
