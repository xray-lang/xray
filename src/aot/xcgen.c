/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcgen.c - AOT C code generator main logic
 *
 * KEY CONCEPT:
 *   Module-level orchestration: creates sections, compiles each XIR function,
 *   generates forward declarations, and assembles the final C source file.
 *
 * RELATED MODULES:
 *   - xcgen_expr.c: expression/instruction translation
 *   - xcgen_stmt.c: control flow (terminators, phi lowering)
 */

#include "xcgen.h"
#include "../runtime/value/xchunk.h"
#include "../base/xchecks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../base/xmalloc.h"
#include "../jit/xir_intrinsic.h"  // xir_resolve_intrinsics, XR_INTRIN_*
#include "xrt_method_symbols.h"    // XRT_SYM_* constants only (avoids xrt_arc.h)

/* ========== Dynamic String Buffer ========== */

void xcgen_buf_init(XcgenBuf *b) {
    XR_DCHECK(b != NULL, "xcgen_buf_init: b is NULL");
    b->cap = 4096;
    b->data = (char *) xr_malloc(b->cap);
    if (!b->data) {
        b->cap = 0;
        b->len = 0;
        return;
    }
    b->data[0] = '\0';
    b->len = 0;
}

void xcgen_buf_free(XcgenBuf *b) {
    xr_free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void xcgen_buf_ensure(XcgenBuf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t new_cap = b->cap;
        while (b->len + extra + 1 > new_cap)
            new_cap *= 2;
        char *new_data = (char *) xr_realloc(b->data, new_cap);
        if (!new_data)
            return;
        b->data = new_data;
        b->cap = new_cap;
    }
}

void xcgen_buf_puts(XcgenBuf *b, const char *s) {
    XR_DCHECK(b != NULL, "xcgen_buf_puts: b is NULL");
    XR_DCHECK(s != NULL, "xcgen_buf_puts: s is NULL");
    size_t slen = strlen(s);
    xcgen_buf_ensure(b, slen);
    memcpy(b->data + b->len, s, slen + 1);
    b->len += slen;
}

void xcgen_buf_append(XcgenBuf *dst, const XcgenBuf *src) {
    if (!src || src->len == 0)
        return;
    xcgen_buf_ensure(dst, src->len);
    memcpy(dst->data + dst->len, src->data, src->len);
    dst->len += src->len;
    dst->data[dst->len] = '\0';
}

XR_PRINTF_FMT(2, 3) void xcgen_buf_printf(XcgenBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0)
        return;
    xcgen_buf_ensure(b, (size_t) needed);
    va_start(ap, fmt);
    b->len += vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
}

/* ========== Type Helpers ========== */

const char *xcg_c_type(uint8_t xir_type) {
    switch (xir_type) {
        case XR_REP_F64:
            return "double";
        case XR_REP_STR:
        case XR_REP_PTR:
        case XR_REP_TAGGED:
            return "XrValue";
        case XR_REP_I64:
        default:
            return "int64_t";
    }
}

bool xcg_is_float_type(uint8_t xir_type) {
    return xir_type == XR_REP_F64;
}

static bool xcg_is_tagged_type(uint8_t xir_type) {
    return xir_type == XR_REP_STR || xir_type == XR_REP_TAGGED || xir_type == XR_REP_PTR;
}


// Derive C type string directly from XrType* (no lossy downgrade)
static const char *xcg_c_type_for_xrtype(struct XrType *t) {
    if (!t)
        return "XrValue";
    XrRep rep = xr_type_rep(t);
    switch (rep) {
        case XR_REP_F64:
            return "double";
        case XR_REP_I64:
            return "int64_t";
        case XR_REP_PTR:
        case XR_REP_STR:
        case XR_REP_TAGGED:
            return "XrValue";
        default:
            return "XrValue";
    }
}

/* ========== Compilation Lifecycle ========== */

XcgenCompilation *xcgen_compilation_new(void) {
    XcgenCompilation *comp = (XcgenCompilation *) xr_calloc(1, sizeof(XcgenCompilation));
    if (!comp)
        return NULL;

    comp->modules_cap = 4;
    comp->modules = (XcgenModule **) xr_calloc(comp->modules_cap, sizeof(XcgenModule *));
    if (!comp->modules) {
        xr_free(comp);
        return NULL;
    }

    comp->proto_map_cap = 64;
    comp->proto_map = (XcgenProtoEntry *) xr_malloc(comp->proto_map_cap * sizeof(XcgenProtoEntry));
    if (!comp->proto_map) {
        xr_free(comp->modules);
        xr_free(comp);
        return NULL;
    }
    comp->max_shared_index = -1;
    return comp;
}

XcgenModule *xcgen_compilation_add_module(XcgenCompilation *comp, const char *name,
                                          const char *path) {
    XR_DCHECK(comp != NULL, "xcgen_compilation_add_module: NULL comp");

    // Grow modules array if needed
    if (comp->nmodules >= comp->modules_cap) {
        int new_cap = comp->modules_cap * 2;
        XcgenModule **tmp =
            (XcgenModule **) xr_realloc(comp->modules, new_cap * sizeof(XcgenModule *));
        if (!tmp)
            return NULL;
        comp->modules = tmp;
        comp->modules_cap = new_cap;
    }

    XcgenModule *mod = (XcgenModule *) xr_calloc(1, sizeof(XcgenModule));
    if (!mod)
        return NULL;

    mod->module_name = name;
    mod->module_path = path;
    mod->module_id = (int16_t) comp->nmodules;
    mod->comp = comp;
    mod->struct_reg = comp->struct_reg;
    for (int i = 0; i < XCGEN_SEC_COUNT; i++)
        xcgen_buf_init(&mod->sections[i]);

    mod->funcs_cap = 16;
    mod->funcs = (XcgenFunc *) xr_calloc(mod->funcs_cap, sizeof(XcgenFunc));
    if (!mod->funcs) {
        xr_free(mod);
        return NULL;
    }

    comp->modules[comp->nmodules++] = mod;
    return mod;
}

void xcgen_module_add_export(XcgenModule *mod, const char *name, int shared_index, bool is_const) {
    XR_DCHECK(mod != NULL, "xcgen_module_add_export: NULL mod");
    if (!name)
        return;

    // Grow exports array if needed
    if (mod->nexports >= mod->exports_cap) {
        int new_cap = mod->exports_cap > 0 ? mod->exports_cap * 2 : 8;
        XcgenExport *tmp =
            (XcgenExport *) xr_realloc(mod->exports, (size_t) new_cap * sizeof(XcgenExport));
        if (!tmp)
            return;
        mod->exports = tmp;
        mod->exports_cap = new_cap;
    }
    XcgenExport *e = &mod->exports[mod->nexports++];
    e->name = xr_strdup(name);  // copy: source may be on GC heap
    e->shared_index = shared_index;
    e->is_const = is_const;
}

void xcgen_register_proto(XcgenCompilation *comp, void *proto_ptr, const char *c_name) {
    XR_DCHECK(comp != NULL, "xcgen_register_proto: NULL comp");
    if (!proto_ptr || !c_name)
        return;

    if (comp->proto_map_count >= comp->proto_map_cap) {
        int new_cap = comp->proto_map_cap * 2;
        XcgenProtoEntry *tmp =
            (XcgenProtoEntry *) xr_realloc(comp->proto_map, new_cap * sizeof(XcgenProtoEntry));
        if (!tmp)
            return;
        comp->proto_map = tmp;
        comp->proto_map_cap = new_cap;
    }
    XcgenProtoEntry *e = &comp->proto_map[comp->proto_map_count++];
    e->proto_ptr = proto_ptr;
    e->c_name = c_name;
    e->func_idx = -1;
    e->non_escaping = false;
    e->num_upvals = 0;
}

void xcgen_register_class(XcgenCompilation *comp, void *ctor_proto, const char *class_name,
                          const char *parent_name, int nfields) {
    XR_DCHECK(comp != NULL, "xcgen_register_class: NULL comp");
    if (!ctor_proto || !class_name)
        return;

    if (comp->nclass_infos >= comp->class_infos_cap) {
        int new_cap = comp->class_infos_cap > 0 ? comp->class_infos_cap * 2 : 8;
        XcgenClassInfo *tmp = (XcgenClassInfo *) xr_realloc(
            comp->class_infos, (size_t) new_cap * sizeof(XcgenClassInfo));
        if (!tmp)
            return;
        comp->class_infos = tmp;
        comp->class_infos_cap = new_cap;
    }
    XcgenClassInfo *ci = &comp->class_infos[comp->nclass_infos++];
    ci->ctor_proto = ctor_proto;
    ci->class_name = class_name;
    ci->parent_name = parent_name;
    ci->nfields = nfields;
}

const XcgenClassInfo *xcgen_lookup_class(XcgenCompilation *comp, void *ctor_proto) {
    if (!comp || !ctor_proto)
        return NULL;
    for (int i = 0; i < comp->nclass_infos; i++) {
        if (comp->class_infos[i].ctor_proto == ctor_proto)
            return &comp->class_infos[i];
    }
    return NULL;
}

/* ========== Proto Lookup (via mod->comp global registry) ========== */

const char *xcg_lookup_proto_name(XcgenModule *mod, void *proto_ptr) {
    XR_DCHECK(mod != NULL && mod->comp != NULL, "xcg_lookup_proto_name: NULL mod/comp");
    XcgenCompilation *comp = mod->comp;
    for (int i = 0; i < comp->proto_map_count; i++) {
        if (comp->proto_map[i].proto_ptr == proto_ptr)
            return comp->proto_map[i].c_name;
    }
    return NULL;
}

// Returns the index of the compiled function in mod->funcs, or -1 if not found.
// Callers must use mod->funcs[idx] to get the actual XcgenFunc — never cache
// the pointer across calls that may trigger mod->funcs realloc.
int xcg_lookup_proto_func_idx(XcgenModule *mod, void *proto_ptr) {
    XR_DCHECK(mod != NULL && mod->comp != NULL, "xcg_lookup_proto_func_idx: NULL");
    XcgenCompilation *comp = mod->comp;
    for (int i = 0; i < comp->proto_map_count; i++) {
        if (comp->proto_map[i].proto_ptr == proto_ptr)
            return comp->proto_map[i].func_idx;
    }
    return -1;
}

/* ========== Module Lifecycle ========== */

void xcgen_module_free(XcgenModule *mod) {
    if (!mod)
        return;
    for (int i = 0; i < XCGEN_SEC_COUNT; i++)
        xcgen_buf_free(&mod->sections[i]);
    for (int i = 0; i < mod->nfuncs; i++) {
        xcgen_buf_free(&mod->funcs[i].body);
        xr_free(mod->funcs[i].vreg_struct_id);
        xr_free(mod->funcs[i].call_args);
    }
    xr_free(mod->funcs);
    for (int i = 0; i < mod->nexports; i++)
        xr_free((void *) mod->exports[i].name);
    xr_free(mod->exports);
    xr_free(mod);
}

void xcgen_compilation_free(XcgenCompilation *comp) {
    if (!comp)
        return;
    for (int i = 0; i < comp->nmodules; i++)
        xcgen_module_free(comp->modules[i]);
    xr_free(comp->modules);
    xr_free(comp->proto_map);
    xr_free(comp->class_infos);
    xr_free(comp);
}

/* ========== Forward Declaration Generation ========== */

// Get the C type string for a parameter vreg, considering struct promotion.
// Returns "xrs_N*" when the param has been identified as a promoted struct ptr.
static const char *xcg_param_c_type(XcgenModule *mod, XcgenFunc *cf, int param_idx) {
    if (cf->vreg_struct_id && param_idx < (int) cf->xfunc->nvreg) {
        int si = cf->vreg_struct_id[param_idx];
        if (si >= 0 && mod->struct_reg && si < mod->struct_reg->nstructs) {
            return mod->struct_reg->structs[si].c_name;  // e.g. "xrs_7"
            // Caller appends "*"
        }
    }
    // Read type directly from proto (no lossy downgrade through XrSlotType)
    XrProto *proto = cf->xfunc->proto;
    if (proto && proto->param_types && param_idx < proto->param_types_count)
        return xcg_c_type_for_xrtype(proto->param_types[param_idx]);
    return "XrValue";
}

// Returns true if param i is a promoted struct (should be xrs_N*, not XrtValue)
static bool xcg_param_is_struct(XcgenModule *mod, XcgenFunc *cf, int param_idx) {
    if (!cf->vreg_struct_id || param_idx >= cf->num_params)
        return false;
    int si = cf->vreg_struct_id[param_idx];
    return si >= 0 && mod->struct_reg && si < mod->struct_reg->nstructs;
}

static void xcgen_emit_forward_decl(XcgenModule *mod, XcgenFunc *cf) {
    XcgenBuf *fwd = &mod->sections[XCGEN_SEC_FORWARD];
    XirFunc *func = cf->xfunc;

    const char *ret_type =
        cf->void_return ? "void"
                        : xcg_c_type_for_xrtype(func->proto ? func->proto->return_type_info : NULL);
    xcgen_buf_printf(fwd, "static %s %s(", ret_type, cf->c_name);
    bool has_params = false;
    xcgen_buf_puts(fwd, "XrtContext");
    has_params = true;
    if (cf->needs_closure_param) {
        xcgen_buf_puts(fwd, ", xrt_closure_t*");
    } else if (cf->non_escaping && cf->num_upvals > 0) {
        // Non-escaping closure: upvalues passed as direct XrtValue params
        for (int u = 0; u < cf->num_upvals; u++)
            xcgen_buf_puts(fwd, ", XrtValue");
    }
    for (int i = 0; i < func->num_params; i++) {
        if (has_params || i > 0)
            xcgen_buf_puts(fwd, ", ");
        if (xcg_param_is_struct(mod, cf, i)) {
            xcgen_buf_printf(fwd, "%s*", xcg_param_c_type(mod, cf, i));
        } else {
            xcgen_buf_printf(fwd, "%s", xcg_param_c_type(mod, cf, i));
        }
        has_params = true;
    }
    if (!has_params)
        xcgen_buf_puts(fwd, "void");
    xcgen_buf_puts(fwd, ");\n");
}

/* ========== Single Function Compilation ========== */

static void xcgen_compile_function_body(XcgenModule *mod, XcgenFunc *cf) {
    XR_DCHECK(mod != NULL, "xcgen_compile_function_body: NULL mod");
    XR_DCHECK(cf != NULL, "xcgen_compile_function_body: NULL cf");
    XR_DCHECK(cf->xfunc != NULL, "xcgen_compile_function_body: NULL xfunc");
    XirFunc *func = cf->xfunc;
    XcgenBuf *b = &cf->body;

    const char *tagged_type = "XrValue";
    const char *ret_type =
        xcg_c_type_for_xrtype(func->proto ? func->proto->return_type_info : NULL);

    // Block reachability analysis.
    // Use a stack buffer for small functions, heap for larger ones. The
    // explicit is_heap flags are required so that the subsequent xr_free()
    // calls can never touch the stack buffer (defensive against refactors
    // and required to pass clang-analyzer-unix.Malloc).
    bool reachable_buf[256];
    bool reachable_is_heap = (func->nblk > 256);
    bool *reachable = reachable_is_heap ? xr_calloc(func->nblk, 1) : reachable_buf;
    if (!reachable)
        return;
    xcg_compute_reachable(func, reachable);

    // Vreg usage analysis (same stack/heap split pattern).
    bool used_buf[512];
    bool used_is_heap = (func->nvreg > 512);
    bool *used = used_is_heap ? xr_calloc(func->nvreg, 1) : used_buf;
    if (!used) {
        if (reachable_is_heap)
            xr_free(reachable);
        return;
    }
    xcg_compute_used_vregs(func, reachable, used);
    cf->used_vregs = used;

    // --- Phase 1: locals buffer ---
    // Collect all local variable declarations into a separate buffer.
    // This allows stmts generation to append new locals at any point
    // without interleaving them with executable code.
    XcgenBuf locals;
    xcgen_buf_init(&locals);

    // Classify vreg types for grouped declarations
    bool has_int_locals = false, has_float_locals = false, has_tagged_locals = false;
    for (uint32_t i = func->num_params; i < func->nvreg; i++) {
        if (cf->vreg_struct_id && cf->vreg_struct_id[i] >= 0) {
            has_tagged_locals = true;  // promoted struct → XrtValue
            continue;
        }
        uint8_t vt = func->vregs[i].rep;
        if (xcg_is_float_type(vt))
            has_float_locals = true;
        else if (xcg_is_tagged_type(vt))
            has_tagged_locals = true;
        else
            has_int_locals = true;
    }
    // Phi auto-boxing may create implicit runtime dependencies
    if (has_tagged_locals)
        cf->needs_runtime = true;

    // Declare ALL local vregs grouped by type.
    // Declaring all (not just 'used') avoids use-before-def errors when
    // DCE/const_prop eliminate def instructions but xcgen still emits the vreg.
    if (has_int_locals) {
        xcgen_buf_puts(&locals, "    int64_t");
        bool first = true;
        for (uint32_t i = func->num_params; i < func->nvreg; i++) {
            if (cf->vreg_struct_id && cf->vreg_struct_id[i] >= 0)
                continue;
            uint8_t vt = func->vregs[i].rep;
            if (!xcg_is_float_type(vt) && !xcg_is_tagged_type(vt)) {
                xcgen_buf_printf(&locals, "%s v%u = 0", first ? " " : ", ", i);
                first = false;
            }
        }
        xcgen_buf_puts(&locals, ";\n");
    }
    if (has_float_locals) {
        xcgen_buf_puts(&locals, "    double");
        bool first = true;
        for (uint32_t i = func->num_params; i < func->nvreg; i++) {
            if (cf->vreg_struct_id && cf->vreg_struct_id[i] >= 0)
                continue;
            if (xcg_is_float_type(func->vregs[i].rep)) {
                xcgen_buf_printf(&locals, "%s v%u = 0.0", first ? " " : ", ", i);
                first = false;
            }
        }
        xcgen_buf_puts(&locals, ";\n");
    }
    if (has_tagged_locals) {
        xcgen_buf_printf(&locals, "    %s", tagged_type);
        bool first = true;
        for (uint32_t i = func->num_params; i < func->nvreg; i++) {
            bool is_promoted = (cf->vreg_struct_id && cf->vreg_struct_id[i] >= 0);
            if (is_promoted || xcg_is_tagged_type(func->vregs[i].rep)) {
                xcgen_buf_printf(&locals, "%s v%u = {0}", first ? " " : ", ", i);
                first = false;
            }
        }
        xcgen_buf_puts(&locals, ";\n");
    }

    // Phi temp variables (only for reachable blocks)
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        if (!reachable[bi])
            continue;
        XirBlock *blk = func->blocks[bi];
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            uint32_t vi = XIR_REF_INDEX(phi->dst);
            xcgen_buf_printf(&locals, "    %s phi_v%u;\n", xcg_c_type(phi->rep), vi);
        }
    }

    // Exception locals are emitted after Phase 2 (needs_exception may be set there)

    // --- Phase 2: stmts buffer ---
    // Emit basic blocks (skip unreachable) into a separate stmts buffer.
    XcgenBuf stmts;
    xcgen_buf_init(&stmts);

    // Exception frame tracking with finally/re-throw support.
    // Push setjmp frames when entering try regions, pop before the
    // terminator when the block exits the try region, and track state
    // for re-throw after finally via _ef{n}_state variables.
    XirBlock *cur_exc_handler = NULL;
    int exc_frame_idx = 0;  // counter for unique frame variable names
    memset(cf->exc_catch_frame, -1, sizeof(cf->exc_catch_frame));
    cf->exc_pending_depth = 0;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        if (!reachable[bi])
            continue;
        XirBlock *blk = func->blocks[bi];
        XirBlock *blk_eh = blk->exception_handler;

        // Label (emitted FIRST so gotos land before frame management)
        xcgen_buf_printf(&stmts, "L%u:", blk->id);
        if (blk->label)
            xcgen_buf_printf(&stmts, " /* %s */", blk->label);
        xcgen_buf_puts(&stmts, "\n");

        // Copy phi temps to actual variables
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            uint32_t vi = XIR_REF_INDEX(phi->dst);
            xcgen_buf_printf(&stmts, "    v%u = phi_v%u;\n", vi, vi);
        }

        // Entering new try region: push setjmp frame (after label + phis)
        if (blk_eh != NULL && blk_eh != cur_exc_handler) {
            int fidx = exc_frame_idx;
            xcgen_buf_printf(&stmts, "    _ef%d_state = 0;\n", fidx);
            xcgen_buf_printf(&stmts, "    _ef%d.prev = xrt_exc_top;\n", fidx);
            xcgen_buf_printf(&stmts, "    xrt_exc_top = &_ef%d;\n", fidx);
            xcgen_buf_printf(&stmts, "    if (setjmp(_ef%d.buf) != 0) {\n", fidx);
            xcgen_buf_printf(&stmts, "        xrt_exception = _ef%d.exception;\n", fidx);
            xcgen_buf_printf(&stmts, "        xrt_exc_top = _ef%d.prev;\n", fidx);
            xcgen_buf_printf(&stmts, "        _ef%d_state = 1;\n", fidx);
            xcgen_buf_printf(&stmts, "        goto L%u;\n", blk_eh->id);
            xcgen_buf_puts(&stmts, "    }\n");
            // Record handler block → frame index mapping
            XR_DCHECK(blk_eh->id < 256, "exc_catch_frame: block id overflow");
            cf->exc_catch_frame[blk_eh->id] = fidx;
            // Push onto pending stack for TRY_END re-throw
            XR_DCHECK(cf->exc_pending_depth < 8, "exc_pending_stack overflow");
            cf->exc_pending_stack[cf->exc_pending_depth++] = fidx;
            exc_frame_idx++;
            cf->needs_exception = true;
            cur_exc_handler = blk_eh;
        } else if (blk_eh == NULL && cur_exc_handler != NULL) {
            cur_exc_handler = NULL;
        }

        // Instructions (skip dead: unused dst + no side effects)
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (!(ins->flags & XIR_FLAG_SIDE_EFFECT) && !xir_ref_is_none(ins->dst) &&
                xir_ref_is_vreg(ins->dst)) {
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi < func->nvreg && !used[dvi])
                    continue;
            }

            // XIR_CATCH: reset exception state (exception successfully caught)
            if (ins->op == XIR_CATCH && blk->id < 256) {
                int fidx = cf->exc_catch_frame[blk->id];
                if (fidx >= 0) {
                    xcgen_buf_printf(&stmts, "    _ef%d_state = 0; /* caught */\n", fidx);
                }
            }

            xcg_emit_instruction(&stmts, func, ins, cf->c_name, mod, cf);

            // XIR_TRY_END: re-throw if exception still pending after finally
            if (ins->op == XIR_TRY_END && cf->exc_pending_depth > 0) {
                int fidx = cf->exc_pending_stack[--cf->exc_pending_depth];
                xcgen_buf_printf(&stmts, "    if (_ef%d_state) xrt_throw_exc(xrt_exception);\n",
                                 fidx);
            }
        }

        // Pop exception frame BEFORE terminator if leaving try region.
        // Check whether any successor block has a different exception_handler.
        if (blk_eh != NULL && exc_frame_idx > 0) {
            bool leaving = false;
            if (blk->jmp.type == XIR_JMP_RET) {
                leaving = true;
            } else if (blk->jmp.type == XIR_JMP_JMP && blk->s1) {
                if (blk->s1->exception_handler != blk_eh)
                    leaving = true;
            } else if (blk->jmp.type == XIR_JMP_BR) {
                if ((blk->s1 && blk->s1->exception_handler != blk_eh) ||
                    (blk->s2 && blk->s2->exception_handler != blk_eh))
                    leaving = true;
            }
            if (leaving) {
                xcgen_buf_printf(&stmts, "    xrt_exc_top = _ef%d.prev; /* exit try */\n",
                                 exc_frame_idx - 1);
            }
        }

        // Terminator
        xcg_emit_terminator(&stmts, func, blk, cf->c_name, cf);
    }

    // Emit exception locals now that needs_exception and exc_frame_idx are known
    if (cf->needs_exception) {
        xcgen_buf_printf(&locals, "    %s xrt_exception = {0};\n", tagged_type);
        for (int ei = 0; ei < exc_frame_idx; ei++) {
            xcgen_buf_printf(&locals, "    XrtExcFrame _ef%d;\n", ei);
            xcgen_buf_printf(&locals, "    int _ef%d_state = 0;\n", ei);
        }
    }
    // Emit defer locals
    if (cf->defer_count > 0) {
        for (int di = 0; di < cf->defer_count; di++) {
            xcgen_buf_printf(&locals, "    XrValue _defer_%d = {0};\n", di);
            xcgen_buf_printf(&locals, "    int _defer_%d_set = 0;\n", di);
            // Emit arg save slots for deferred calls with arguments
            int nargs = func->defer_entries[di].arg_count;
            for (int ai = 0; ai < nargs; ai++) {
                xcgen_buf_printf(&locals, "    XrValue _defer_%d_arg%d = {0};\n", di, ai);
            }
        }
    }
    if (locals.len > 0)
        xcgen_buf_puts(&locals, "\n");

    // Suppress unused label warnings
    xcgen_buf_puts(&stmts, "    (void)0;\n");

    // --- Assemble: signature + locals + stmts → body ---
    // Override ret_type if function always returns null
    if (cf->void_return)
        ret_type = "void";
    xcgen_buf_printf(b, "static %s %s(", ret_type, cf->c_name);
    bool has_sig_params = false;
    xcgen_buf_puts(b, "XrtContext xrt_ctx");
    has_sig_params = true;
    if (cf->needs_closure_param) {
        xcgen_buf_puts(b, ", xrt_closure_t *xrt_cl");
    } else if (cf->non_escaping && cf->num_upvals > 0) {
        // Non-escaping closure: upvalues as direct XrtValue params (xrt_upv0, xrt_upv1, ...)
        for (int u = 0; u < cf->num_upvals; u++)
            xcgen_buf_printf(b, ", XrtValue xrt_upv%d", u);
    }
    for (int i = 0; i < func->num_params; i++) {
        if (has_sig_params || i > 0)
            xcgen_buf_puts(b, ", ");
        if (xcg_param_is_struct(mod, cf, i)) {
            xcgen_buf_printf(b, "%s* v%d", xcg_param_c_type(mod, cf, i), i);
        } else {
            xcgen_buf_printf(b, "%s v%d", xcg_param_c_type(mod, cf, i), i);
        }
        has_sig_params = true;
    }
    if (!has_sig_params)
        xcgen_buf_puts(b, "void");
    xcgen_buf_puts(b, ") {\n");
    xcgen_buf_puts(b, "    (void)xrt_ctx;\n");
    if (cf->needs_closure_param) {
        xcgen_buf_puts(b, "    (void)xrt_cl;\n");
    } else if (cf->non_escaping && cf->num_upvals > 0) {
        for (int u = 0; u < cf->num_upvals; u++)
            xcgen_buf_printf(b, "    (void)xrt_upv%d;\n", u);
    }
    xcgen_buf_append(b, &locals);
    xcgen_buf_append(b, &stmts);
    xcgen_buf_puts(b, "}\n");

    xcgen_buf_free(&locals);
    xcgen_buf_free(&stmts);

    // Free heap-allocated analysis buffers if used. The explicit flags
    // mirror the allocation site above and prevent freeing the stack buffer.
    if (reachable_is_heap)
        xr_free(reachable);
    if (used_is_heap)
        xr_free(used);
}

XcgenFunc *xcgen_compile_func(XcgenModule *mod, XirFunc *xfunc, const char *c_name) {
    if (!mod || !xfunc || !c_name)
        return NULL;

    /* Convert CALL_C(fn_ptr) → CALL_INTRINSIC(id) before codegen.
     * This decouples AOT C emission from JIT runtime symbol addresses. */
    xir_resolve_intrinsics(xfunc);

    // Grow func array if needed
    if (mod->nfuncs >= mod->funcs_cap) {
        uint32_t new_cap = mod->funcs_cap * 2;
        XcgenFunc *new_funcs = (XcgenFunc *) xr_realloc(mod->funcs, new_cap * sizeof(XcgenFunc));
        if (!new_funcs)
            return NULL;
        mod->funcs = new_funcs;
        mod->funcs_cap = new_cap;
    }

    XcgenFunc *cf = &mod->funcs[mod->nfuncs];
    memset(cf, 0, sizeof(*cf));
    cf->xfunc = xfunc;
    cf->c_name = c_name;
    cf->num_params = xfunc->num_params;
    xcgen_buf_init(&cf->body);

    // Pre-scan: detect closure param and exception handling needs
    for (uint32_t bi = 0; bi < xfunc->nblk; bi++) {
        XirBlock *blk = xfunc->blocks[bi];
        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            uint8_t op = blk->ins[ii].op;
            if (op == XIR_LOAD_UPVAL) {
                cf->needs_closure_param = true;
            }
            if (op == XIR_STORE_UPVAL && !xir_ref_is_vreg(blk->ins[ii].args[0])) {
                cf->needs_closure_param = true;
            }
            if (op == XIR_CATCH) {
                cf->needs_exception = true;
            }
        }
    }

    // Non-escaping closure: check if this function was marked by its parent's
    // escape analysis. If so, replace xrt_closure_t* param with direct upval params.
    if (cf->needs_closure_param) {
        XcgenProtoEntry *self_entry = xcg_lookup_proto_entry(mod, xfunc->proto);
        if (self_entry && self_entry->non_escaping && self_entry->num_upvals > 0) {
            cf->non_escaping = true;
            cf->num_upvals = self_entry->num_upvals;
            cf->needs_closure_param = false;  // no longer needs xrt_closure_t* param
        }
    }

    // Transfer defer count from XIR function to codegen state
    cf->defer_count = xfunc->defer_count;

    // Run escape analysis on this function's child closures.
    // Must happen before child functions are compiled so they see the non_escaping flag.
    xcg_prescan_closure_escape(mod, xfunc);

    // Detect void-return functions (always return null) before forward decl
    cf->void_return = xcg_detect_void_return(xfunc);

    // Initialize struct promotion tracking (before forward decl so param types are known)
    if (xfunc->nvreg > 0 && mod->struct_reg && mod->struct_reg->nstructs > 0) {
        cf->vreg_struct_id = (int16_t *) xr_malloc(xfunc->nvreg * sizeof(int16_t));
        if (!cf->vreg_struct_id)
            return NULL;
        for (uint32_t vi = 0; vi < xfunc->nvreg; vi++)
            cf->vreg_struct_id[vi] = -1;
        xcg_prescan_struct_vregs(mod, cf);
    } else {
        cf->vreg_struct_id = NULL;
    }

    // Retype non-promoted LOAD_FIELD results to TAGGED (16-byte XrtValue slots).
    // Runs unconditionally — class instances skip struct registration.
    xcg_retype_field_loads(cf);

    // Retype boolean method results to TAGGED so print preserves true/false.
    xcg_retype_bool_method_results(cf);

    // Generate forward declaration (after struct prescan so param struct types are known)
    xcgen_emit_forward_decl(mod, cf);

    // Initialize call args buffer (heap-allocated, grows on demand)
    cf->call_args_cap = XCGEN_MAX_CALL_ARGS;
    cf->call_args = (XirRef *) xr_malloc(cf->call_args_cap * sizeof(XirRef));
    cf->call_args_count = 0;
    for (int i = 0; i < cf->call_args_cap; i++)
        cf->call_args[i] = XIR_NONE;

    // Compile function body
    xcgen_compile_function_body(mod, cf);

    // Backfill func_idx into global proto_map
    XcgenCompilation *comp = mod->comp;
    XR_DCHECK(comp != NULL, "xcgen_compile_func: NULL comp backpointer");
    for (int i = 0; i < comp->proto_map_count; i++) {
        if (comp->proto_map[i].c_name == c_name) {
            comp->proto_map[i].func_idx = mod->nfuncs;
            break;
        }
    }

    mod->nfuncs++;
    return cf;
}

/* ========== Final Source Assembly ========== */

// Emit source for a single module into buf (internal helper)
static void xcgen_emit_module_source(XcgenBuf *out, XcgenModule *mod) {
    XR_DCHECK(out != NULL && mod != NULL, "xcgen_emit_module_source: NULL arg");

    // Per-module sections: types, forward decls, data, function bodies
    if (mod->sections[XCGEN_SEC_TYPES].len > 0) {
        xcgen_buf_puts(out, mod->sections[XCGEN_SEC_TYPES].data);
        xcgen_buf_puts(out, "\n");
    }
    if (mod->sections[XCGEN_SEC_FORWARD].len > 0) {
        xcgen_buf_printf(out, "/* Forward declarations [%s] */\n",
                         mod->module_name ? mod->module_name : "main");
        xcgen_buf_puts(out, mod->sections[XCGEN_SEC_FORWARD].data);
        xcgen_buf_puts(out, "\n");
    }
    if (mod->sections[XCGEN_SEC_DATA].len > 0) {
        xcgen_buf_puts(out, mod->sections[XCGEN_SEC_DATA].data);
        xcgen_buf_puts(out, "\n");
    }
    for (int i = 0; i < mod->nfuncs; i++) {
        xcgen_buf_puts(out, mod->funcs[i].body.data);
        xcgen_buf_puts(out, "\n");
    }
    if (mod->sections[XCGEN_SEC_MAIN].len > 0) {
        xcgen_buf_puts(out, mod->sections[XCGEN_SEC_MAIN].data);
    }
}

char *xcgen_emit_source(XcgenCompilation *comp) {
    if (!comp)
        return NULL;

    XcgenBuf out;
    xcgen_buf_init(&out);

    xcgen_buf_puts(&out, "/* Auto-generated by xray build --native (transpile to C) */\n\n");

    // Check if any function needs runtime
    bool any_needs_runtime = false;
    for (int m = 0; m < comp->nmodules; m++) {
        XcgenModule *mod = comp->modules[m];
        if (mod->sections[XCGEN_SEC_MAIN].len > 0) {
            any_needs_runtime = true;
            break;
        }
        for (int i = 0; i < mod->nfuncs; i++) {
            if (mod->funcs[i].needs_runtime) {
                any_needs_runtime = true;
                break;
            }
        }
        if (any_needs_runtime)
            break;
    }

// Helper: sanitize module name for C identifier (replace non-alnum with '_')
#define SANITIZE_IDENT(dst, src, cap)                                                              \
    do {                                                                                           \
        const char *_s = (src);                                                                    \
        int _k;                                                                                    \
        for (_k = 0; _s[_k] && _k < (cap) - 1; _k++)                                               \
            (dst)[_k] = ((_s[_k] >= 'a' && _s[_k] <= 'z') || (_s[_k] >= 'A' && _s[_k] <= 'Z') ||   \
                         (_s[_k] >= '0' && _s[_k] <= '9') || _s[_k] == '_')                        \
                            ? _s[_k]                                                               \
                            : '_';                                                                 \
        (dst)[_k] = '\0';                                                                          \
    } while (0)

    // Headers
    xcgen_buf_puts(&out, "#include <stdio.h>\n"
                         "#include <stdlib.h>\n"
                         "#include <string.h>\n"
                         "#include <stdint.h>\n"
                         "#include <inttypes.h>\n");
    if (any_needs_runtime)
        xcgen_buf_puts(&out, "#define XRT_IMPL  /* define globals once in this TU */\n"
                             "#include \"xrt.h\"\n");
    xcgen_buf_puts(&out, "\n");

    // Per-module custom headers
    for (int m = 0; m < comp->nmodules; m++) {
        XcgenModule *mod = comp->modules[m];
        if (mod->sections[XCGEN_SEC_HEADERS].len > 0) {
            xcgen_buf_puts(&out, mod->sections[XCGEN_SEC_HEADERS].data);
            xcgen_buf_puts(&out, "\n");
        }
    }

    // Shared variable array (GETSHARED/SETSHARED → C global)
    if (comp->max_shared_index >= 0) {
        xcgen_buf_printf(&out,
                         "/* Module-level shared variables */\n"
                         "static XrValue xrt_shared[%d];\n\n",
                         comp->max_shared_index + 1);
    }

    // Class type_id variables (lazy-initialized via xrt_type_register)
    if (comp->nclass_infos > 0) {
        xcgen_buf_puts(&out, "/* Class type IDs (populated at first constructor call) */\n");
        for (int i = 0; i < comp->nclass_infos; i++) {
            xcgen_buf_printf(&out, "static uint16_t _tid_%s = 0;\n",
                             comp->class_infos[i].class_name);
        }
        xcgen_buf_puts(&out, "\n");
    }

    // Struct typedefs (global, from Json promotion)
    if (comp->struct_reg && comp->struct_reg->nstructs > 0)
        xcgen_emit_all_typedefs(&out, comp->struct_reg);

    // Emit each module's code
    for (int m = 0; m < comp->nmodules; m++)
        xcgen_emit_module_source(&out, comp->modules[m]);

    // Ownership transfers to caller
    return out.data;
}
