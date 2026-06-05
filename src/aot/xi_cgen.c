/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen.c - Xi IR to C code generation
 *
 * Walks XiFunc -> XiBlock -> XiValue and emits equivalent C code.
 *
 * Strategy:
 *   - All locals declared at function top (C89 style) to avoid
 *     scope issues across labels.
 *   - Each basic block emits a label (L0:, L1:, ...).
 *   - PHI nodes are eliminated by inserting assignments before
 *     jumps in predecessor blocks.
 *   - Value representation (I64/F64/TAGGED) read from v->rep,
 *     populated by xi_opt_select_rep in the pipeline.
 */

#include "xi_cgen.h"
#include "../ir/xi_analysis.h"
#include "../ir/xi_backend_lower.h"
#include "../ir/xi_op_name.h"
#include "../ir/xi_opt.h"
#include "../ir/xi_own.h"
#include "../base/xdefs.h"
#include "../runtime/value/xstruct_layout.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "xrt_method_symbols.h"
#include "../frontend/parser/xast_nodes.h"
#include <string.h>
#include <inttypes.h>

/* Iterator/hasNext/next symbols defined in xrt_method_symbols.h
 * with values matching SYMBOL_ITERATOR(54)/HASNEXT(56)/NEXT(57). */

/* ========== Representation Helpers ========== */

/* Read the stored representation set by select_rep.
 * select_rep always runs in the AOT pipeline before code generation. */
static inline XrRep cg_rep(const XiValue *v) {
    return v ? (XrRep) v->rep : XR_REP_TAGGED;
}

static const char *ctype_str(XrRep rep) {
    switch (rep) {
        case XR_REP_I64:
            return "int64_t";
        case XR_REP_F64:
            return "double";
        default:
            return "XrValue";
    }
}

/* Check whether an op is void-like (produces no named result).
 * At STAGE_BACKEND, XI_PRINT etc. are XI_CALL_BUILTIN with aux name. */
static bool cg_is_void_like(const XiValue *v) {
    switch (v->op) {
        case XI_SET_SHARED:
        case XI_STORE_UPVAL:
        case XI_STORE_FIELD:
        case XI_STRUCT_SET:
        case XI_INDEX_SET:
        case XI_THROW:
        case XI_RETAIN:
        case XI_RELEASE:
            return true;
        case XI_CORO_OP:
            return v->aux_int == XI_CORO_SUB_SET_PRIORITY ||
                   v->aux_int == XI_CORO_SUB_LOCK_THREAD ||
                   v->aux_int == XI_CORO_SUB_UNLOCK_THREAD || v->aux_int == XI_CORO_SUB_SET_LOCAL;
        case XI_CALL_BUILTIN:
            if (v->aux) {
                const char *n = (const char *) v->aux;
                if (strcmp(n, "print") == 0 || strcmp(n, "json_init_f") == 0 ||
                    strcmp(n, "json_set_f") == 0)
                    return true;
            }
            return false;
        default:
            return false;
    }
}

static bool cg_is_unsupported_coroutine_op(uint16_t op) {
    switch (op) {
        case XI_GO:
        case XI_AWAIT:
        case XI_CHAN_SEND:
        case XI_CHAN_RECV:
        case XI_CHAN_TRY_SEND:
        case XI_CHAN_TRY_RECV:
        case XI_CHAN_IS_CLOSED:
        case XI_TIME_AFTER:
        case XI_SELECT_BLOCK:
        case XI_YIELD:
        case XI_CHAN_NEW:
        case XI_SCOPE_ENTER:
        case XI_SCOPE_EXIT:
        case XI_CORO_OP:
            return true;
        default:
            return false;
    }
}

static bool cg_is_aot_suspend_op(uint16_t op) {
    switch (op) {
        case XI_GO:
        case XI_AWAIT:
        case XI_YIELD:
            return true;
        case XI_CHAN_SEND:
        case XI_CHAN_RECV:
        case XI_CHAN_TRY_SEND:
        case XI_CHAN_TRY_RECV:
        case XI_CHAN_IS_CLOSED:
        case XI_TIME_AFTER:
        case XI_SELECT_BLOCK:
        case XI_CHAN_NEW:
        case XI_SCOPE_ENTER:
        case XI_SCOPE_EXIT:
        case XI_CORO_OP:
            return true;
        default:
            return false;
    }
}

static bool cg_type_is_channel(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_CHANNEL)
        return true;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (cg_type_is_channel(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

static bool cg_value_type_is_channel(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY) && v->nargs >= 1)
        v = v->args[0];
    return v && cg_type_is_channel(v->type);
}

static bool cg_type_is_task(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_INSTANCE)
        return type->instance.class_name && strcmp(type->instance.class_name, "Task") == 0;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (cg_type_is_task(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

static bool cg_value_type_is_task(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY) && v->nargs >= 1)
        v = v->args[0];
    return v && cg_type_is_task(v->type);
}

static const char *cg_task_field_helper(const char *field) {
    if (!field)
        return NULL;
    if (strcmp(field, "done") == 0)
        return "xr_aot_task_done";
    if (strcmp(field, "cancelled") == 0)
        return "xr_aot_task_cancelled";
    if (strcmp(field, "result") == 0)
        return "xr_aot_task_result";
    if (strcmp(field, "error") == 0)
        return "xr_aot_task_error";
    return NULL;
}

static bool cg_task_field_needs_xrt_bridge(const char *field) {
    return field && (strcmp(field, "result") == 0 || strcmp(field, "error") == 0);
}

static bool cg_channel_method_may_suspend(const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !cg_value_type_is_channel(v->args[0]))
        return false;
    const char *method = (const char *) v->aux;
    return method && (strcmp(method, "send") == 0 || strcmp(method, "recv") == 0 ||
                      strcmp(method, "sendTimeout") == 0 || strcmp(method, "recvTimeout") == 0);
}

static bool cg_import_ref_is_module(const XiValue *v, const char *module_name) {
    if (!v || v->op != XI_IMPORT_REF || !v->aux || !module_name)
        return false;
    const XiImportRef *ref = (const XiImportRef *) v->aux;
    return ref->module_path && strcmp(ref->module_path, module_name) == 0 && !ref->member_name;
}

static bool cg_shared_slot_is_module_import(const XiFunc *f, int slot, const char *module_name) {
    if (!f || slot < 0 || !module_name)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            if (cg_import_ref_is_module(v->args[0], module_name))
                return true;
        }
    }
    return false;
}

static bool cg_value_is_module_import(const XiFunc *f, const XiValue *v, const char *module_name) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY) && v->nargs >= 1)
        v = v->args[0];
    if (cg_import_ref_is_module(v, module_name))
        return true;
    if (v && v->op == XI_GET_SHARED)
        return cg_shared_slot_is_module_import(f, (int) v->aux_int, module_name);
    return false;
}

static bool cg_is_time_sleep_call(const XiFunc *f, const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs != 2)
        return false;
    const char *method = (const char *) v->aux;
    if (!method || strcmp(method, "sleep") != 0)
        return false;
    return cg_value_is_module_import(f, v->args[0], "time");
}

static bool cg_value_needs_aot_coro(const XiFunc *f, const XiValue *v) {
    if (!v)
        return false;
    if (cg_is_aot_suspend_op(v->op))
        return true;
    return cg_channel_method_may_suspend(v) || cg_is_time_sleep_call(f, v);
}

static bool cg_func_needs_aot_coro(const XiFunc *f) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_value_needs_aot_coro(f, v))
                return true;
        }
    }
    return false;
}

/* ========== Codegen Context ========== */

#define CG_MAX_SHARED 512
#define CG_MAX_METHODS 256
#define CG_MAX_IMPORTS 256

typedef struct {
    const char *class_name; /* owning class (e.g. "Rect") */
    const char *name;       /* method name (e.g. "area") */
    const XiFunc *func;
    const char *module_prefix; /* C function name prefix (NULL = current module) */
} CgMethodEntry;

typedef struct {
    const char *module_path;         /* import source (e.g. "./math_lib") */
    const char *member_name;         /* exported name (e.g. "square") */
    const char *target_mod_name;     /* C identifier prefix (e.g. "math_lib") */
    int shared_slot;                 /* slot in target's xrt_shared_<mod>[] */
    const XiFunc *target_func;       /* XiFunc* if this export is a function (for direct calls) */
    const XiClassData *target_class; /* XiClassData* if this export is a class */
    const XiFunc *exporter_func;     /* exporter module XiFunc (for class child resolution) */
} CgImportEntry;

/* All mutable codegen state for one C-generation session.
 * Heap-allocated via xi_cgen_ctx_new; no file-scope globals. */
struct XiCgenCtx {
    int fname_counter;
    const XiFunc *shared_funcs[CG_MAX_SHARED];
    const XiClassData *shared_class[CG_MAX_SHARED];
    int nshared;
    CgMethodEntry methods[CG_MAX_METHODS];
    int nmethod;
    XiModule *module; /* current module being emitted */
    bool pre_decl_all;
    const char *shared_name;
    CgImportEntry imports[CG_MAX_IMPORTS];
    int nimports;
    XiModule **all_modules; /* full modules array for resolved-index lookups */
    int all_nmodules;
    bool error; /* set on fatal codegen errors (unknown builtin, etc.) */
    XiCgenCoroFrameStats coro_frame_stats;
};

XR_FUNC XiCgenCtx *xi_cgen_ctx_new(void) {
    XiCgenCtx *ctx = (XiCgenCtx *) xr_calloc(1, sizeof(XiCgenCtx));
    if (!ctx)
        return NULL;
    ctx->shared_name = "xrt_shared";
    return ctx;
}

XR_FUNC void xi_cgen_ctx_free(XiCgenCtx *ctx) {
    xr_free(ctx);
}

XR_FUNC bool xi_cgen_has_error(const XiCgenCtx *ctx) {
    return ctx && ctx->error;
}

XR_FUNC XiCgenCoroFrameStats xi_cgen_coro_frame_stats(const XiCgenCtx *ctx) {
    XiCgenCoroFrameStats stats = {0};
    if (ctx)
        stats = ctx->coro_frame_stats;
    return stats;
}

/* Find the constructor child XiFunc from a XiClassData descriptor.
 * Uses arena-safe XiClassMethod array (no AST dependency). */
static const XiFunc *cg_find_constructor(const XiFunc *parent, const XiClassData *cd) {
    if (!cd || !cd->methods || !parent)
        return NULL;
    for (uint16_t ci = 0; ci < cd->nmethod; ci++) {
        if (cd->methods[ci].is_static_constructor)
            continue;
        if (cd->methods[ci].is_constructor) {
            if (cd->child_idx && ci < cd->ninst + cd->nstat) {
                uint16_t idx = cd->child_idx[ci];
                if (idx < parent->nchildren)
                    return parent->children[idx];
            }
        }
    }
    return NULL;
}

/* Register all instance methods from a class descriptor into ctx->methods.
 * Constructors are excluded — they are resolved via the XI_CALL class path.
 * Uses arena-safe XiClassMethod array (no AST dependency). */
static void cg_register_class_methods(XiCgenCtx *ctx, const XiFunc *parent, const XiClassData *cd) {
    if (!cd || !cd->methods || !parent)
        return;
    for (uint16_t ci = 0; ci < cd->nmethod; ci++) {
        const XiClassMethod *m = &cd->methods[ci];
        if (m->is_static_constructor)
            continue;
        if (!m->is_constructor && !m->is_static && m->name) {
            if (cd->child_idx && ci < cd->ninst + cd->nstat) {
                uint16_t idx = cd->child_idx[ci];
                if (idx < parent->nchildren && ctx->nmethod < CG_MAX_METHODS) {
                    ctx->methods[ctx->nmethod].class_name = cd->class_name;
                    ctx->methods[ctx->nmethod].name = m->name;
                    ctx->methods[ctx->nmethod].func = parent->children[idx];
                    ctx->nmethod++;
                }
            }
        }
    }
}

/* Find the shared slot index that holds a class by name.
 * Returns -1 if not found. The slot holds the type_id (as xr_int).
 * For `is` checks against a skeleton name (e.g. "Box"), prefer the
 * skeleton class itself; xrt_instanceof walks generic_origin on each
 * mono instance to match. */
static int cg_find_class_slot(const XiCgenCtx *ctx, const char *class_name) {
    if (!class_name)
        return -1;
    int display_match = -1;
    for (int s = 0; s < ctx->nshared && s < CG_MAX_SHARED; s++) {
        const XiClassData *cd = ctx->shared_class[s];
        if (!cd || !cd->class_name)
            continue;
        /* Exact internal name match (skeleton or mono) */
        if (strcmp(cd->class_name, class_name) == 0)
            return s;
        /* display_name match: remember first, but keep scanning for exact */
        if (display_match < 0 && cd->display_name && strcmp(cd->display_name, class_name) == 0)
            display_match = s;
    }
    return display_match;
}

/* Lookup constructor XiFunc for a class by name.
 * Scans module slot_classes instead of raw IR blocks. */
static const XiFunc *cg_lookup_class_ctor(XiCgenCtx *ctx, const char *class_name) {
    if (!class_name || !ctx->module)
        return NULL;
    XiModule *mod = ctx->module;
    for (uint16_t s = 0; s < mod->nslots; s++) {
        const XiClassData *cd = mod->slot_classes ? mod->slot_classes[s] : NULL;
        if (!cd || !cd->class_name)
            continue;
        if (strcmp(cd->class_name, class_name) == 0)
            return cg_find_constructor(mod->init, cd);
    }
    return NULL;
}

/* Lookup a class instance method by name, optionally filtered by class.
 * If class_name is non-NULL, only match methods of that class.
 * If class_name is NULL, return the last match (most-derived class
 * is registered last due to topo-order scan).
 * If out_prefix is non-NULL, stores the method's module prefix (for
 * cross-module class methods; NULL means current module). */
static const XiFunc *cg_lookup_method(XiCgenCtx *ctx, const char *name, const char *class_name,
                                      const char **out_prefix) {
    if (!name)
        return NULL;
    const XiFunc *last_match = NULL;
    const char *last_prefix = NULL;
    for (int i = 0; i < ctx->nmethod; i++) {
        if (!ctx->methods[i].name || strcmp(ctx->methods[i].name, name) != 0)
            continue;
        if (class_name && ctx->methods[i].class_name &&
            strcmp(ctx->methods[i].class_name, class_name) == 0) {
            if (out_prefix)
                *out_prefix = ctx->methods[i].module_prefix;
            return ctx->methods[i].func;
        }
        last_match = ctx->methods[i].func;
        last_prefix = ctx->methods[i].module_prefix;
    }
    if (out_prefix)
        *out_prefix = class_name ? NULL : last_prefix;
    return class_name ? NULL : last_match;
}

static const XiFunc *cg_lookup_method_by_index(XiCgenCtx *ctx, const char *class_name,
                                               int method_idx, const char **out_prefix) {
    if (!class_name || method_idx < 0)
        return NULL;
    for (uint16_t s = 0; ctx->module && s < ctx->module->nslots; s++) {
        const XiClassData *cd = ctx->module->slot_classes ? ctx->module->slot_classes[s] : NULL;
        if (!cd || !cd->class_name || strcmp(cd->class_name, class_name) != 0)
            continue;
        if ((uint16_t) method_idx >= cd->ninst || !cd->child_idx)
            return NULL;
        uint16_t child_idx = cd->child_idx[method_idx];
        if (child_idx >= ctx->module->init->nchildren)
            return NULL;
        if (out_prefix)
            *out_prefix = NULL;
        return ctx->module->init->children[child_idx];
    }
    return NULL;
}

/* Initialize ctx from XiModule metadata.  Reads slot_funcs/slot_classes
 * directly from the module struct — no IR block scanning required. */
static void cg_init_from_module(XiCgenCtx *ctx, XiModule *mod) {
    XR_DCHECK(ctx != NULL, "cg_init_from_module: NULL ctx");
    XR_DCHECK(mod != NULL, "cg_init_from_module: NULL module");
    XR_DCHECK(mod->init != NULL, "cg_init_from_module: NULL init func");

    memset(ctx->shared_funcs, 0, sizeof(ctx->shared_funcs));
    memset(ctx->shared_class, 0, sizeof(ctx->shared_class));
    ctx->nshared = mod->init->nshared;
    ctx->nmethod = 0;
    ctx->module = mod;

    /* Copy slot mappings from module metadata */
    uint16_t nslots = mod->nslots < CG_MAX_SHARED ? mod->nslots : CG_MAX_SHARED;
    if (mod->slot_funcs) {
        for (uint16_t s = 0; s < nslots; s++)
            ctx->shared_funcs[s] = mod->slot_funcs[s];
    }
    if (mod->slot_classes) {
        for (uint16_t s = 0; s < nslots; s++) {
            ctx->shared_class[s] = mod->slot_classes[s];
            /* For class slots, also map to their constructor */
            if (mod->slot_classes[s] && !ctx->shared_funcs[s]) {
                const XiFunc *ctor = cg_find_constructor(mod->init, mod->slot_classes[s]);
                if (ctor)
                    ctx->shared_funcs[s] = ctor;
            }
        }
    }

    /* Register class methods from all module classes */
    for (uint16_t ci = 0; ci < mod->nclasses; ci++) {
        if (mod->classes[ci])
            cg_register_class_methods(ctx, mod->init, mod->classes[ci]);
    }
}

/* Register imported class data and methods from the cross-module import
 * table.  Called after cg_init_from_module so that class imports from other
 * modules are available for constructor-call and method resolution. */
static void cg_register_imported_classes(XiCgenCtx *ctx) {
    for (int i = 0; i < ctx->nimports; i++) {
        const CgImportEntry *imp = &ctx->imports[i];
        if (!imp->target_class || !imp->exporter_func)
            continue;
        /* Register the exporter's class methods into ctx->methods so that
         * XI_CALL_METHOD on imported class instances can resolve them.
         * Record the exporter's module prefix for correct C name emission. */
        int base = ctx->nmethod;
        cg_register_class_methods(ctx, imp->exporter_func, imp->target_class);
        for (int m = base; m < ctx->nmethod; m++)
            ctx->methods[m].module_prefix = imp->target_mod_name;
    }
}

static void sanitize_c_ident_part(char *buf, size_t cap, const char *raw) {
    XR_DCHECK(buf != NULL, "sanitize_c_ident_part: NULL buffer");
    XR_DCHECK(cap > 0, "sanitize_c_ident_part: zero capacity");

    size_t j = 0;
    if (!raw || !raw[0]) {
        buf[j++] = '_';
        buf[j] = '\0';
        return;
    }

    char first = raw[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_'))
        buf[j++] = '_';

    for (const char *p = raw; *p && j < cap - 1; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            buf[j++] = c;
        else
            buf[j++] = '_';
    }
    buf[j] = '\0';
}

/* Write the C name for a function (prefix_funcname_id).
 * Each XiFunc gets a unique numeric suffix to prevent name collisions
 * (e.g. multiple anonymous closures or same-named constructors).
 * The suffix is stored in cgen_id the first time and reused thereafter. */
static void emit_fname(XiCgenCtx *ctx, FILE *out, const char *prefix, const XiFunc *f) {
    XR_DCHECK(f != NULL, "emit_fname: NULL func");
    const char *raw = f->name ? f->name : "anon";

    char buf[128];
    sanitize_c_ident_part(buf, sizeof(buf), raw);

    /* Assign a stable unique ID on first use (cgen_id == 0 means unassigned) */
    XiFunc *mf = (XiFunc *) (uintptr_t) f; /* cast away const for cgen_id write */
    if (mf->cgen_id == 0)
        mf->cgen_id = ++ctx->fname_counter;

    if (prefix && prefix[0]) {
        char prefix_buf[128];
        sanitize_c_ident_part(prefix_buf, sizeof(prefix_buf), prefix);
        fprintf(out, "%s_%s_%d", prefix_buf, buf, f->cgen_id);
    } else {
        fprintf(out, "fn_%s_%d", buf, f->cgen_id);
    }
}

static void emit_fname_suffix(XiCgenCtx *ctx, FILE *out, const char *prefix, const XiFunc *f,
                              const char *suffix) {
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "%s", suffix ? suffix : "");
}

/* Write a value reference: v<id> or phi<id> for phi nodes */
static void emit_vref(FILE *out, const XiValue *v) {
    if (v->op == XI_PHI)
        fprintf(out, "phi%u", v->id);
    else
        fprintf(out, "v%u", v->id);
}

/* Write a phi variable reference: phi<id> */
static void emit_phi_ref(FILE *out, const XiPhi *phi) {
    fprintf(out, "phi%u", phi->value.id);
}

/* ========== Method Symbol Resolution ========== */

/* Map a method name string (from XI_CALL_METHOD/XI_LOAD_FIELD aux) to
 * the corresponding XRT_SYM_* integer. Returns -1 if not a known builtin. */
static int cg_method_sym(const char *name) {
    if (!name)
        return -1;
    /* Auto-generated from xi_method_sym.def */
    static const struct {
        const char *name;
        int sym;
    } map[] = {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) {display_name, XRT_SYM_##aot_name},
#include "../ir/xi_method_sym.def"
#undef XI_METHOD_SYM
        /* Aliases not in the .def */
        {"size", XRT_SYM_SIZE}, /* alias for length */
        {"add", XRT_SYM_SET},   /* set.add() maps to SET symbol */
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].name) == 0)
            return map[i].sym;
    }
    return -1;
}

/* ========== Struct Inline Helpers ========== */

/* Check if all uses of `target` are safe for struct inlining.
 * Safe uses: STRUCT_GET(args[0]), STRUCT_SET(args[0]), COPY(args[0]).
 * COPY is transparent — its result is recursively checked.
 * Depth-limited to prevent pathological COPY chains. */
static bool cg_struct_uses_safe(const XiFunc *f, const XiValue *target, int depth) {
    if (depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++)
                if (phi->value.args[k] == target)
                    return false;
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_STRUCT_GET || v->op == XI_STRUCT_SET) && a == 0)
                    continue;
                /* COPY is transparent: check the copy's own uses */
                if (v->op == XI_COPY && a == 0) {
                    if (!cg_struct_uses_safe(f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

/* Check if XI_STRUCT_NEW value can be inlined as a local C struct.
 * True iff all transitive uses (through COPY chains) are
 * STRUCT_GET(args[0]) or STRUCT_SET(args[0]). */
static bool cg_struct_can_inline(const XiFunc *f, const XiValue *target) {
    XR_DCHECK(f != NULL && target != NULL, "cg_struct_can_inline: NULL");
    return cg_struct_uses_safe(f, target, 0);
}

/* Trace COPY chain to find the defining XI_STRUCT_NEW. */
static const XiValue *cg_trace_struct_new(const XiValue *v) {
    while (v && v->op == XI_COPY && v->nargs >= 1)
        v = v->args[0];
    return (v && v->op == XI_STRUCT_NEW) ? v : NULL;
}

/* ========== Value Emission ========== */

static void emit_binop(FILE *out, const XiValue *v, const char *op) {
    emit_vref(out, v->args[0]);
    fprintf(out, " %s ", op);
    emit_vref(out, v->args[1]);
}

static void emit_condition_expr(FILE *out, const XiValue *v) {
    if (cg_rep(v) == XR_REP_TAGGED) {
        fprintf(out, "xr_truthy(");
        emit_vref(out, v);
        fprintf(out, ")");
    } else {
        fprintf(out, "(");
        emit_vref(out, v);
        fprintf(out, " != 0)");
    }
}

static void emit_codegen_abort_expr(FILE *out) {
    fprintf(out, "(abort(), XR_NULL_VAL)");
}

static void emit_codegen_abort_aot_result(FILE *out) {
    fprintf(out, "    return (abort(), xr_aot_error(XR_NULL_VAL, false));\n");
}

/* Emit the RHS expression for a single value. */
static void emit_value_rhs(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    XR_DCHECK(ctx != NULL, "emit_value_rhs: NULL ctx");
    XR_DCHECK(v != NULL, "emit_value_rhs: NULL value");

    if (cg_is_unsupported_coroutine_op(v->op)) {
        const char *op_name = xi_op_name(v->op);
        fprintf(stderr, "[xi_cgen] ERROR: unsupported coroutine Xi op %s\n", op_name);
        emit_codegen_abort_expr(out);
        ctx->error = true;
        return;
    }

    switch (v->op) {
        case XI_CONST:
            if (v->type->kind == XR_KIND_INT)
                fprintf(out, "INT64_C(%" PRId64 ")", v->aux_int);
            else if (v->type->kind == XR_KIND_FLOAT) {
                double d;
                memcpy(&d, &v->aux_int, sizeof(double));
                fprintf(out, "%a", d);
            } else if (v->type->kind == XR_KIND_BOOL)
                fprintf(out, "%" PRId64, v->aux_int);
            else if (v->type->kind == XR_KIND_NULL)
                fprintf(out, "XR_NULL_VAL");
            else if (v->type->kind == XR_KIND_STRING) {
                /* Emit escaped string literal wrapped in xrt_box_str */
                const char *s = (const char *) v->aux;
                fprintf(out, "xr_box_str(\"");
                if (s) {
                    for (const char *p = s; *p; p++) {
                        if (*p == '"')
                            fprintf(out, "\\\"");
                        else if (*p == '\\')
                            fprintf(out, "\\\\");
                        else if (*p == '\n')
                            fprintf(out, "\\n");
                        else if (*p == '\t')
                            fprintf(out, "\\t");
                        else
                            fputc(*p, out);
                    }
                }
                fprintf(out, "\")");
            } else {
                fprintf(out, "XR_NULL_VAL /* unknown const kind */");
            }
            break;

        case XI_PARAM:
            fprintf(out, "p%u", (unsigned) v->aux_int);
            if (cg_rep(v) == XR_REP_I64)
                fprintf(out, ".i");
            else if (cg_rep(v) == XR_REP_F64)
                fprintf(out, ".f");
            break;

        case XI_COPY:
            emit_vref(out, v->args[0]);
            break;

        /* Arithmetic: use C operators when both operands are scalar.
         * When any operand is tagged (XrValue), must use runtime functions.
         * When result rep is scalar but operands are mixed, box each tagged
         * operand before the C operation, or fall back to runtime dispatch. */
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD: {
            XrRep result_rep = cg_rep(v);
            XrRep a_rep = cg_rep(v->args[0]);
            XrRep b_rep = cg_rep(v->args[1]);
            bool any_tagged = (a_rep == XR_REP_TAGGED || b_rep == XR_REP_TAGGED);
            if (result_rep == XR_REP_TAGGED || any_tagged) {
                /* Use runtime dispatch — handles int/float/mixed correctly */
                const char *fn = NULL;
                switch (v->op) {
                    case XI_ADD:
                        fn = "xrt_add";
                        break;
                    case XI_SUB:
                        fn = "xrt_sub";
                        break;
                    case XI_MUL:
                        fn = "xrt_mul";
                        break;
                    case XI_DIV:
                        fn = "xrt_div";
                        break;
                    case XI_MOD:
                        fn = "xrt_mod";
                        break;
                    default:
                        break;
                }
                /* xrt_* returns XrValue.  If result_rep is scalar,
                 * extract with .f or .i after the runtime call. */
                if (result_rep == XR_REP_F64) {
                    fprintf(out, "%s(", fn);
                    if (a_rep != XR_REP_TAGGED) {
                        fprintf(out, "XR_FROM_FLOAT(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, ")");
                    } else {
                        emit_vref(out, v->args[0]);
                    }
                    fprintf(out, ", ");
                    if (b_rep != XR_REP_TAGGED) {
                        fprintf(out, "XR_FROM_FLOAT(");
                        emit_vref(out, v->args[1]);
                        fprintf(out, ")");
                    } else {
                        emit_vref(out, v->args[1]);
                    }
                    fprintf(out, ").f");
                } else if (result_rep == XR_REP_I64) {
                    fprintf(out, "%s(", fn);
                    if (a_rep != XR_REP_TAGGED) {
                        fprintf(out, "XR_FROM_INT(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, ")");
                    } else {
                        emit_vref(out, v->args[0]);
                    }
                    fprintf(out, ", ");
                    if (b_rep != XR_REP_TAGGED) {
                        fprintf(out, "XR_FROM_INT(");
                        emit_vref(out, v->args[1]);
                        fprintf(out, ")");
                    } else {
                        emit_vref(out, v->args[1]);
                    }
                    fprintf(out, ").i");
                } else {
                    fprintf(out, "%s(", fn);
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", ");
                    emit_vref(out, v->args[1]);
                    fprintf(out, ")");
                }
            } else {
                /* Typed scalar path. For DIV/MOD route through xrt_int_div /
                 * xrt_int_mod so zero-check + INT64_MIN/-1 wrap match VM/JIT
                 * even when both operands are XR_REP_I64 native int64. */
                if ((v->op == XI_DIV || v->op == XI_MOD) && result_rep == XR_REP_I64) {
                    const char *fn = (v->op == XI_DIV) ? "xrt_int_div" : "xrt_int_mod";
                    fprintf(out, "%s(", fn);
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", ");
                    emit_vref(out, v->args[1]);
                    fprintf(out, ")");
                } else if ((v->op == XI_DIV || v->op == XI_MOD) && result_rep == XR_REP_F64) {
                    /* Float div: emit checked division to throw on /0
                     * with parity to the tagged xrt_div / xrt_mod path. */
                    if (v->op == XI_DIV) {
                        fprintf(out, "(xrt_div(XR_FROM_FLOAT(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, "), XR_FROM_FLOAT(");
                        emit_vref(out, v->args[1]);
                        fprintf(out, ")).f)");
                    } else {
                        fprintf(out, "(xrt_mod(XR_FROM_FLOAT(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, "), XR_FROM_FLOAT(");
                        emit_vref(out, v->args[1]);
                        fprintf(out, ")).f)");
                    }
                } else {
                    const char *op = "+";
                    switch (v->op) {
                        case XI_SUB:
                            op = "-";
                            break;
                        case XI_MUL:
                            op = "*";
                            break;
                        default:
                            break;
                    }
                    emit_binop(out, v, op);
                }
            }
            break;
        }
        case XI_NEG: {
            XrRep result_rep = cg_rep(v);
            XrRep a_rep = cg_rep(v->args[0]);
            if (result_rep == XR_REP_TAGGED || a_rep == XR_REP_TAGGED) {
                fprintf(out, "xrt_neg(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else {
                fprintf(out, "-");
                emit_vref(out, v->args[0]);
            }
            break;
        }

        /* Bitwise */
        case XI_BAND:
            emit_binop(out, v, "&");
            break;
        case XI_BOR:
            emit_binop(out, v, "|");
            break;
        case XI_BXOR:
            emit_binop(out, v, "^");
            break;
        case XI_BNOT:
            fprintf(out, "~");
            emit_vref(out, v->args[0]);
            break;
        case XI_SHL:
            emit_binop(out, v, "<<");
            break;
        case XI_SHR:
            emit_binop(out, v, ">>");
            break;

        /* Comparison: scalar comparison uses C operators; tagged uses xrt_eq/lt/le.
         * Result is always I64 (boolean 0/1). */
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE: {
            XrRep a0_rep = cg_rep(v->args[0]);
            XrRep a1_rep = cg_rep(v->args[1]);
            XrRep arg_rep =
                (a0_rep == XR_REP_TAGGED || a1_rep == XR_REP_TAGGED) ? XR_REP_TAGGED : a0_rep;
            if (arg_rep == XR_REP_TAGGED) {
                switch (v->op) {
                    case XI_EQ:
                        fprintf(out, "xrt_eq(");
                        break;
                    case XI_NE:
                        fprintf(out, "!xrt_eq(");
                        break;
                    case XI_LT:
                        fprintf(out, "xrt_lt(");
                        break;
                    case XI_LE:
                        fprintf(out, "xrt_le(");
                        break;
                    case XI_GT:
                        fprintf(out, "xrt_lt(");
                        break;
                    case XI_GE:
                        fprintf(out, "xrt_le(");
                        break;
                    default:
                        break;
                }
                /* GT/GE: swap operands → xrt_lt(b,a) / xrt_le(b,a) */
                if (v->op == XI_GT || v->op == XI_GE) {
                    emit_vref(out, v->args[1]);
                    fprintf(out, ", ");
                    emit_vref(out, v->args[0]);
                } else {
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", ");
                    emit_vref(out, v->args[1]);
                }
                fprintf(out, ")");
            } else {
                const char *op = "==";
                switch (v->op) {
                    case XI_NE:
                        op = "!=";
                        break;
                    case XI_LT:
                        op = "<";
                        break;
                    case XI_LE:
                        op = "<=";
                        break;
                    case XI_GT:
                        op = ">";
                        break;
                    case XI_GE:
                        op = ">=";
                        break;
                    default:
                        break;
                }
                emit_binop(out, v, op);
            }
            break;
        }

        /* Strict (identity) comparison: raw bit equality on tagged values */
        case XI_EQ_STRICT:
        case XI_NE_STRICT: {
            const char *eq_op = (v->op == XI_EQ_STRICT) ? "==" : "!=";
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ".i %s ", eq_op);
            emit_vref(out, v->args[1]);
            fprintf(out, ".i)");
            break;
        }

        /* Logical */
        case XI_NOT:
            fprintf(out, "!(");
            emit_condition_expr(out, v->args[0]);
            fprintf(out, ")");
            break;

        case XI_SELECT:
            XR_DCHECK(v->nargs == 3, "XI_SELECT: need cond, true, false");
            fprintf(out, "(");
            emit_condition_expr(out, v->args[0]);
            fprintf(out, " ? ");
            emit_vref(out, v->args[1]);
            fprintf(out, " : ");
            emit_vref(out, v->args[2]);
            fprintf(out, ")");
            break;

        case XI_ISNULL:
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ".tag == XR_TAG_NULL)");
            break;

        /* Box / Unbox */
        case XI_BOX: {
            struct XrType *sty = v->args[0]->type;
            if (sty && sty->kind == XR_KIND_NULL) {
                /* Null is already tagged; no actual boxing needed */
                emit_vref(out, v->args[0]);
            } else if (sty && sty->kind == XR_KIND_FLOAT) {
                fprintf(out, "XR_FROM_FLOAT(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else if (sty && sty->kind == XR_KIND_BOOL) {
                fprintf(out, "XR_FROM_BOOL(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else if (sty && sty->kind == XR_KIND_STRING) {
                /* String is already tagged */
                emit_vref(out, v->args[0]);
            } else {
                fprintf(out, "XR_FROM_INT(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            }
            break;
        }

        case XI_UNBOX: {
            /* Determine unbox accessor based on result representation:
             * I64 → .i, F64 → .f, TAGGED → pass through as XrValue. */
            XrRep ur = cg_rep(v);
            emit_vref(out, v->args[0]);
            if (ur == XR_REP_F64)
                fprintf(out, ".f");
            else if (ur == XR_REP_I64)
                fprintf(out, ".i");
            /* else: TAGGED — no accessor, keep as XrValue */
            break;
        }

        /* Narrow: truncate to sub-width, sign/zero-extend back to i64/f64.
         * AOT emits native C casts — the C compiler produces optimal code. */
        case XI_NARROW_I8:
        case XI_WIDEN_I8:
            fprintf(out, "(int64_t)(int8_t)");
            emit_vref(out, v->args[0]);
            break;
        case XI_NARROW_U8:
        case XI_WIDEN_U8:
            fprintf(out, "(int64_t)(uint8_t)");
            emit_vref(out, v->args[0]);
            break;
        case XI_NARROW_I16:
        case XI_WIDEN_I16:
            fprintf(out, "(int64_t)(int16_t)");
            emit_vref(out, v->args[0]);
            break;
        case XI_NARROW_U16:
        case XI_WIDEN_U16:
            fprintf(out, "(int64_t)(uint16_t)");
            emit_vref(out, v->args[0]);
            break;
        case XI_NARROW_I32:
        case XI_WIDEN_I32:
            fprintf(out, "(int64_t)(int32_t)");
            emit_vref(out, v->args[0]);
            break;
        case XI_NARROW_U32:
        case XI_WIDEN_U32:
            fprintf(out, "(int64_t)(uint32_t)");
            emit_vref(out, v->args[0]);
            break;
        case XI_NARROW_F32:
        case XI_WIDEN_F32:
            fprintf(out, "(double)(float)");
            emit_vref(out, v->args[0]);
            break;

        /* Convert */
        case XI_CONVERT:
            if (v->type->kind == XR_KIND_FLOAT) {
                fprintf(out, "(double)");
                emit_vref(out, v->args[0]);
            } else if (v->type->kind == XR_KIND_INT) {
                fprintf(out, "(int64_t)");
                emit_vref(out, v->args[0]);
            } else {
                emit_vref(out, v->args[0]);
            }
            break;

        /* Function call: args[0]=callee, args[1..n]=params */
        case XI_CALL: {
            XiValue *callee = v->args[0];
            const XiFunc *target = NULL;

            /* Resolve callee to a known XiFunc:
             *   1. XI_CLOSURE_NEW with aux → direct child reference
             *   2. XI_BOX of XI_CLOSURE_NEW → boxed child reference
             *   3. XI_CONST null inside a named child → self-recursive call
             *   4. XI_GET_SHARED → module-level function via prescan table
             *   5. XI_UNBOX / XI_BOX wrappers around the above */
            if (callee->op == XI_CLOSURE_NEW && callee->aux) {
                target = (const XiFunc *) callee->aux;
            } else if (callee->op == XI_BOX && callee->args[0]->op == XI_CLOSURE_NEW &&
                       callee->args[0]->aux) {
                target = (const XiFunc *) callee->args[0]->aux;
            } else if (callee->op == XI_CONST && callee->type &&
                       callee->type->kind == XR_KIND_NULL && f->name) {
                /* Self-recursive call: lowerer stores null as self-ref */
                target = f;
            } else if (callee->op == XI_BOX && callee->args[0]->op == XI_CONST &&
                       callee->args[0]->type && callee->args[0]->type->kind == XR_KIND_NULL &&
                       f->name) {
                /* Self-recursive call wrapped by select_rep BOX */
                target = f;
            }
            /* GET_SHARED(slot) → lookup in prescan table */
            if (!target && callee->op == XI_GET_SHARED) {
                int slot = (int) callee->aux_int;
                if (slot >= 0 && slot < ctx->nshared)
                    target = ctx->shared_funcs[slot];
            }
            /* BOX(GET_SHARED(slot)) or UNBOX(GET_SHARED(slot)) */
            if (!target && (callee->op == XI_BOX || callee->op == XI_UNBOX) && callee->nargs >= 1 &&
                callee->args[0]->op == XI_GET_SHARED) {
                int slot = (int) callee->args[0]->aux_int;
                if (slot >= 0 && slot < ctx->nshared)
                    target = ctx->shared_funcs[slot];
            }

            /* XI_IMPORT_REF callee → cross-module imported function or class.
             * Try resolved-index fast path first, then fall back to string scan. */
            const char *import_prefix = NULL;
            bool import_is_class = false;
            if (!target && callee->op == XI_IMPORT_REF && callee->aux) {
                const XiImportRef *ref = (const XiImportRef *) callee->aux;
                /* Fast path: use resolved fields to find the matching import entry */
                if (ref->resolved_mod_index >= 0 && ref->resolved_mod_index < ctx->all_nmodules) {
                    const char *tmod_name = ctx->all_modules[ref->resolved_mod_index]
                                                ? ctx->all_modules[ref->resolved_mod_index]->name
                                                : NULL;
                    for (int ii = 0; ii < ctx->nimports; ii++) {
                        if (ctx->imports[ii].target_mod_name && tmod_name &&
                            strcmp(ctx->imports[ii].target_mod_name, tmod_name) == 0 &&
                            ctx->imports[ii].member_name && ref->member_name &&
                            strcmp(ctx->imports[ii].member_name, ref->member_name) == 0) {
                            if (ctx->imports[ii].target_func) {
                                target = ctx->imports[ii].target_func;
                                import_prefix = ctx->imports[ii].target_mod_name;
                            }
                            if (ctx->imports[ii].target_class)
                                import_is_class = true;
                            break;
                        }
                    }
                }
                /* Fallback: string-scan by module_path */
                if (!target && !import_is_class) {
                    for (int ii = 0; ii < ctx->nimports; ii++) {
                        if (ctx->imports[ii].module_path && ref->module_path &&
                            strcmp(ctx->imports[ii].module_path, ref->module_path) == 0 &&
                            ctx->imports[ii].member_name && ref->member_name &&
                            strcmp(ctx->imports[ii].member_name, ref->member_name) == 0) {
                            if (ctx->imports[ii].target_func) {
                                target = ctx->imports[ii].target_func;
                                import_prefix = ctx->imports[ii].target_mod_name;
                            }
                            if (ctx->imports[ii].target_class)
                                import_is_class = true;
                            break;
                        }
                    }
                }
            }

            /* Detect class constructor call.
             * Patterns:
             *   a) CALL(GET_SHARED(slot))       — slot has class data
             *   b) CALL(BOX/UNBOX(GET_SHARED))  — wrapped variant
             *   c) CALL(CLASS_CREATE(data))      — direct (same scope)
             *   d) CALL(BOX(CLASS_CREATE(data))) — boxed direct
             * For (c)/(d), also resolve the constructor target. */
            bool is_class_call = false;
            if (callee->op == XI_GET_SHARED) {
                int s = (int) callee->aux_int;
                if (s >= 0 && s < CG_MAX_SHARED && ctx->shared_class[s])
                    is_class_call = true;
            }
            if (!is_class_call && (callee->op == XI_BOX || callee->op == XI_UNBOX) &&
                callee->nargs >= 1 && callee->args[0]->op == XI_GET_SHARED) {
                int s = (int) callee->args[0]->aux_int;
                if (s >= 0 && s < CG_MAX_SHARED && ctx->shared_class[s])
                    is_class_call = true;
            }
            /* Direct CLASS_CREATE callee (not via shared slot) */
            if (!is_class_call && callee->op == XI_CLASS_CREATE && callee->aux) {
                const XiClassData *cd = (const XiClassData *) callee->aux;
                const XiFunc *ctor = cg_find_constructor(f, cd);
                if (ctor) {
                    target = ctor;
                    is_class_call = true;
                }
            }
            if (!is_class_call && callee->op == XI_BOX && callee->nargs >= 1 &&
                callee->args[0]->op == XI_CLASS_CREATE && callee->args[0]->aux) {
                const XiClassData *cd = (const XiClassData *) callee->args[0]->aux;
                const XiFunc *ctor = cg_find_constructor(f, cd);
                if (ctor) {
                    target = ctor;
                    is_class_call = true;
                }
            }
            /* Cross-module class import via XI_IMPORT_REF */
            if (!is_class_call && import_is_class && target)
                is_class_call = true;

            if (target && cg_func_needs_aot_coro(target)) {
                ctx->error = true;
                fprintf(stderr,
                        "[xi_cgen] ERROR: unsupported AOT sync call to suspendable function '%s'\n",
                        target->name ? target->name : "?");
                emit_codegen_abort_expr(out);
                break;
            }

            if (target && is_class_call) {
                /* Class constructor call: alloc map instance + call ctor.
                 * xrt_map_new returns a tagged XrValue directly. */
                fprintf(out, "({ XrValue _inst = xrt_map_new(4); ");
                emit_fname(ctx, out, import_prefix ? import_prefix : prefix, target);
                fprintf(out, "(NULL, _inst");
                for (uint16_t a = 1; a < v->nargs; a++) {
                    fprintf(out, ", ");
                    emit_vref(out, v->args[a]);
                }
                fprintf(out, "); _inst; })");
            } else if (target) {
                /* Use the exporter's module prefix for cross-module calls */
                emit_fname(ctx, out, import_prefix ? import_prefix : prefix, target);
                fprintf(out, "(NULL");
                for (uint16_t a = 1; a < v->nargs; a++) {
                    fprintf(out, ", ");
                    emit_vref(out, v->args[a]);
                }
                fprintf(out, ")");
            } else {
                /* Indirect call (fully dynamic, not yet supported) */
                ctx->error = true;
                fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT indirect call\n");
                emit_codegen_abort_expr(out);
            }
            break;
        }

        /* Shared variables (module-level) */
        case XI_GET_SHARED:
            fprintf(out, "%s[%d]", ctx->shared_name, (int) v->aux_int);
            break;

        case XI_SET_SHARED:
            fprintf(out, "(%s[%d] = ", ctx->shared_name, (int) v->aux_int);
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            break;

        /* Closure creation — wrap C function pointer in AOT closure value.
         * Allocate xrt_closure_t with upvals[], initialize captured values
         * from the XI_CLOSURE_NEW args (populated by xi_lower from XiCapture). */
        case XI_CLOSURE_NEW:
            if (v->aux) {
                XiFunc *child = (XiFunc *) v->aux;
                uint16_t ncap = child->ncaptures;
                fprintf(out, "({ xrt_closure_t *_c = (xrt_closure_t*)xrt_closure_new((void*)");
                emit_fname(ctx, out, prefix, child);
                fprintf(out, ", %u).ptr; ", ncap);
                for (uint16_t ci = 0; ci < ncap && ci < v->nargs; ci++) {
                    if (v->args[ci]) {
                        fprintf(out, "_c->upvals[%u] = ", ci);
                        emit_vref(out, v->args[ci]);
                        fprintf(out, "; ");
                    }
                }
                fprintf(out, "xr_mkptr(_c, XR_TAG_CLOSURE); })");
            } else {
                fprintf(out, "XR_NULL_VAL /* closure: unknown */");
            }
            break;

        /* Upvalue access: reads/writes from the hidden _cl parameter.
         * Closure children with captures receive xrt_closure_t *_cl as
         * their first C parameter; upvals are stored in _cl->upvals[]. */
        case XI_LOAD_UPVAL:
            fprintf(out, "_cl->upvals[%d]", (int) v->aux_int);
            break;

        case XI_STORE_UPVAL:
            fprintf(out, "(_cl->upvals[%d] = ", (int) v->aux_int);
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            break;

        /* Runtime type check: args[0]=value, aux=target XrType*.
         * AOT tag namespace extends VM tags: string uses XR_TAG_STR(14)
         * and XR_TAG_STR_ARC(19), not XR_TAG_PTR(5). */
        case XI_IS: {
            XR_DCHECK(v->nargs >= 1, "XI_IS: missing arg");
            struct XrType *target = (struct XrType *) v->aux;
            if (!target) {
                fprintf(out, "0 /* XI_IS: NULL target type */");
                break;
            }
            switch (target->kind) {
                case XR_KIND_INT:
                    fprintf(out, "(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ".tag == %u)", XR_TAG_I64);
                    break;
                case XR_KIND_FLOAT:
                    fprintf(out, "(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ".tag == %u)", XR_TAG_F64);
                    break;
                case XR_KIND_BOOL:
                    fprintf(out, "(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ".tag == %u)", XR_TAG_BOOL);
                    break;
                case XR_KIND_NULL:
                    fprintf(out, "(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ".tag == %u)", XR_TAG_NULL);
                    break;
                case XR_KIND_STRING:
                    /* XR_IS_STR checks both XR_TAG_STR and XR_TAG_STR_ARC */
                    fprintf(out, "XR_IS_STR(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ")");
                    break;
                case XR_KIND_INSTANCE:
                case XR_KIND_CLASS: {
                    /* Class instanceof: resolve class name to shared slot holding
                     * the type_id, then emit xrt_instanceof(val, tid). */
                    const char *cname = target->instance.class_name;
                    int slot = cg_find_class_slot(ctx, cname);
                    if (slot >= 0) {
                        fprintf(out, "xrt_instanceof(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, ", (uint16_t)%s[%d].i)", ctx->shared_name, slot);
                    } else {
                        /* Class not found in this module — fall back to tag check */
                        fprintf(out, "(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, ".tag == %u) /* is %s: class not resolved */",
                                (unsigned) XR_TAG_PTR, cname ? cname : "?");
                    }
                    break;
                }
                default: {
                    uint8_t tag = xr_type_to_xr_tag(target);
                    if (tag != 0xFF) {
                        fprintf(out, "(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, ".tag == %u)", (unsigned) tag);
                    } else {
                        fprintf(out, "0 /* unsupported is-check */");
                    }
                    break;
                }
            }
            break;
        }

        /* ============ Containers ============ */

        /* Indexed read: args[0]=collection, args[1]=key */
        case XI_INDEX_GET:
            XR_DCHECK(v->nargs >= 2, "XI_INDEX_GET: need obj+key");
            fprintf(out, "xrt_index_get(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ")");
            break;

        /* Indexed write: args[0]=collection, args[1]=key, args[2]=value */
        case XI_INDEX_SET:
            XR_DCHECK(v->nargs >= 3, "XI_INDEX_SET: need obj+key+val");
            fprintf(out, "xrt_index_set(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ", ");
            emit_vref(out, v->args[2]);
            fprintf(out, ")");
            break;

        /* ============ Field Access ============ */

        /* Property read: args[0]=object, aux=field name string */
        case XI_LOAD_FIELD: {
            XR_DCHECK(v->nargs >= 1, "XI_LOAD_FIELD: need object");
            const char *field = (const char *) v->aux;
            const char *task_helper =
                cg_value_type_is_task(v->args[0]) ? cg_task_field_helper(field) : NULL;
            if (task_helper) {
                if (cg_rep(v) == XR_REP_I64)
                    fprintf(out, "XR_TO_INT(");
                else if (cg_rep(v) == XR_REP_F64)
                    fprintf(out, "XR_TO_FLOAT(");
                if (cg_task_field_needs_xrt_bridge(field))
                    fprintf(out, "xr_aot_bridge_value_to_xrt(");
                fprintf(out, "%s(NULL, ", task_helper);
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
                if (cg_task_field_needs_xrt_bridge(field))
                    fprintf(out, ")");
                if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
                    fprintf(out, ")");
                break;
            }
            /* Use xrt_getprop with symbol lookup for builtin properties,
             * or xrt_map_get for map-like objects */
            int sym = cg_method_sym(field);
            if (sym >= 0) {
                fprintf(out, "xrt_getprop(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", %d)", sym);
            } else {
                /* Generic field: use map get with string key */
                fprintf(out, "xrt_map_get((xrt_map_t*)");
                emit_vref(out, v->args[0]);
                fprintf(out, ".ptr, xr_box_str(\"%s\"))", field ? field : "?");
            }
            break;
        }

        /* Property write: args[0]=object, args[1]=value, aux=field name string */
        case XI_STORE_FIELD: {
            XR_DCHECK(v->nargs >= 2, "XI_STORE_FIELD: need obj+val");
            const char *field = (const char *) v->aux;
            fprintf(out, "(xrt_map_set((xrt_map_t*)");
            emit_vref(out, v->args[0]);
            fprintf(out, ".ptr, xr_box_str(\"%s\"), ", field ? field : "?");
            emit_vref(out, v->args[1]);
            fprintf(out, "), ");
            emit_vref(out, v->args[1]);
            fprintf(out, ")");
            break;
        }

        /* Struct native ops — inlined C struct for non-escaping instances,
         * runtime fallback for escaping ones. */
        case XI_STRUCT_NEW: {
            XR_DCHECK(v->nargs >= 1, "XI_STRUCT_NEW: need class arg");
            if (cg_struct_can_inline(f, v)) {
                /* Handled in emit_value_stmt — this RHS is never reached */
                fprintf(out, "XR_NULL_VAL");
            } else {
                fprintf(out, "xrt_call_method(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", %d, 0)", cg_method_sym("constructor"));
            }
            break;
        }
        case XI_STRUCT_GET: {
            XR_DCHECK(v->nargs >= 1, "XI_STRUCT_GET: need struct arg");
            const XiValue *origin = cg_trace_struct_new(v->args[0]);
            if (origin && cg_struct_can_inline(f, origin)) {
                fprintf(out, "_st%u.f%d", origin->id, (int) v->aux_int);
            } else {
                XrStructLayout *sl = (XrStructLayout *) v->aux;
                const char *fname = (sl && sl->field_names && v->aux_int < sl->field_count)
                                        ? sl->field_names[v->aux_int]
                                        : NULL;
                if (fname) {
                    int sym = cg_method_sym(fname);
                    if (sym >= 0) {
                        fprintf(out, "xrt_getprop(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, ", %d)", sym);
                    } else {
                        fprintf(out, "xrt_map_get((xrt_map_t*)");
                        emit_vref(out, v->args[0]);
                        fprintf(out, ".ptr, xr_box_str(\"%s\"))", fname);
                    }
                } else {
                    fprintf(out, "XR_NULL");
                }
            }
            break;
        }
        case XI_STRUCT_SET: {
            XR_DCHECK(v->nargs >= 2, "XI_STRUCT_SET: need struct + val");
            const XiValue *origin = cg_trace_struct_new(v->args[0]);
            if (origin && cg_struct_can_inline(f, origin)) {
                fprintf(out, "(_st%u.f%d = ", origin->id, (int) v->aux_int);
                emit_vref(out, v->args[1]);
                fprintf(out, ")");
            } else {
                XrStructLayout *sl = (XrStructLayout *) v->aux;
                const char *fname = (sl && sl->field_names && v->aux_int < sl->field_count)
                                        ? sl->field_names[v->aux_int]
                                        : NULL;
                if (fname) {
                    fprintf(out, "(xrt_map_set((xrt_map_t*)");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ".ptr, xr_box_str(\"%s\"), ", fname);
                    emit_vref(out, v->args[1]);
                    fprintf(out, "), ");
                    emit_vref(out, v->args[1]);
                    fprintf(out, ")");
                }
            }
            break;
        }

        /* Json object: flat field array with O(1) indexed access */
        case XI_JSON_NEW: {
            int64_t fc = v->aux_int > 0 ? v->aux_int : 0;
            fprintf(out, "xrt_json_new(%" PRId64 ")", fc);
            break;
        }
        /* ============ Method Call ============ */

        /* Method dispatch: args[0]=recv, args[1..n]=params, aux=name string.
         * Resolution order:
         *   1. Super call (aux_int bit 0) → find parent class constructor
         *   2. Class instance method → direct C call
         *   3. Builtin method → xrt_method_N runtime dispatch */
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT: {
            XR_DCHECK(v->nargs >= 1, "XI_CALL_METHOD: need receiver");
            const char *method = (const char *) v->aux;
            bool is_super = v->op == XI_CALL_METHOD && (v->aux_int & 1) != 0;
            const XiFunc *mfunc = NULL;
            const char *method_prefix = NULL;

            if (is_super && ctx->module) {
                /* super call: find which class owns the current method,
                 * look up its parent class name from module slot_classes. */
                const char *parent_class = NULL;
                XiModule *mod = ctx->module;
                for (uint16_t s = 0; s < mod->nslots && !parent_class; s++) {
                    const XiClassData *cd = mod->slot_classes ? mod->slot_classes[s] : NULL;
                    if (!cd || !cd->super_name)
                        continue;
                    for (uint16_t ci = 0; ci < cd->ninst + cd->nstat; ci++) {
                        if (cd->child_idx && cd->child_idx[ci] < mod->init->nchildren &&
                            mod->init->children[cd->child_idx[ci]] == f) {
                            parent_class = cd->super_name;
                            break;
                        }
                    }
                }
                if (parent_class) {
                    bool is_ctor_call = (method && strcmp(method, "constructor") == 0);
                    if (is_ctor_call)
                        mfunc = cg_lookup_class_ctor(ctx, parent_class);
                    else
                        mfunc = cg_lookup_method(ctx, method, parent_class, &method_prefix);
                }
            }
            if (!mfunc && !is_super) {
                /* Try receiver-type-specific lookup first.
                 * XI_CALL_METHOD args[0] = receiver, whose type may carry
                 * the class name (XR_KIND_INSTANCE → instance.class_name). */
                const char *recv_class = NULL;
                if (v->args[0] && v->args[0]->type && v->args[0]->type->kind == XR_KIND_INSTANCE)
                    recv_class = v->args[0]->type->instance.class_name;
                if (v->op == XI_CALL_METHOD_DIRECT)
                    mfunc = cg_lookup_method_by_index(ctx, recv_class, (int) v->aux_int,
                                                      &method_prefix);
                else
                    mfunc = cg_lookup_method(ctx, method, recv_class, &method_prefix);
            }
            uint16_t nargs = (uint16_t) (v->nargs - 1);

            if (v->op == XI_CALL_METHOD && cg_value_type_is_task(v->args[0])) {
                if (nargs == 0 && method && strcmp(method, "cancel") == 0) {
                    if (cg_rep(v) == XR_REP_I64)
                        fprintf(out, "XR_TO_INT(");
                    else if (cg_rep(v) == XR_REP_F64)
                        fprintf(out, "XR_TO_FLOAT(");
                    fprintf(out, "xr_aot_task_cancel(NULL, ");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ")");
                    if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
                        fprintf(out, ")");
                } else {
                    ctx->error = true;
                    fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Task method '%s'\n",
                            method ? method : "?");
                    emit_codegen_abort_expr(out);
                }
                break;
            }

            if (mfunc) {
                if (cg_func_needs_aot_coro(mfunc)) {
                    ctx->error = true;
                    fprintf(stderr,
                            "[xi_cgen] ERROR: unsupported AOT sync method call to suspendable "
                            "function '%s'\n",
                            mfunc->name ? mfunc->name : "?");
                    emit_codegen_abort_expr(out);
                    break;
                }
                /* Direct class method call: NULL _cl, receiver is first visible param */
                emit_fname(ctx, out, method_prefix ? method_prefix : prefix, mfunc);
                fprintf(out, "(NULL");
                for (uint16_t a = 0; a < v->nargs; a++) {
                    fprintf(out, ", ");
                    emit_vref(out, v->args[a]);
                }
                fprintf(out, ")");
            } else {
                int sym = cg_method_sym(method);
                if (nargs == 0) {
                    fprintf(out, "xrt_method_0(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", %d)", sym >= 0 ? sym : 0);
                } else if (nargs == 1) {
                    fprintf(out, "xrt_method_1(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", %d, ", sym >= 0 ? sym : 0);
                    emit_vref(out, v->args[1]);
                    fprintf(out, ")");
                } else if (nargs == 2) {
                    fprintf(out, "xrt_method_2(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", %d, ", sym >= 0 ? sym : 0);
                    emit_vref(out, v->args[1]);
                    fprintf(out, ", ");
                    emit_vref(out, v->args[2]);
                    fprintf(out, ")");
                } else {
                    ctx->error = true;
                    fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT method call with %u args\n",
                            (unsigned) nargs);
                    emit_codegen_abort_expr(out);
                }
            }
            break;
        }

        /* throw(value): abort with exception */
        case XI_THROW:
            XR_DCHECK(v->nargs >= 1, "XI_THROW: need arg");
            fprintf(out, "xrt_throw_exc(");
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            break;

            /* ============ ARC / Ownership ============ */

        case XI_RETAIN:
            XR_DCHECK(v->nargs >= 1, "XI_RETAIN: need arg");
            fprintf(out, "xrt_retain(");
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            break;

        case XI_RELEASE:
            XR_DCHECK(v->nargs >= 1, "XI_RELEASE: need arg");
            fprintf(out, "xrt_release(");
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            break;

        case XI_MOVE:
            XR_DCHECK(v->nargs >= 1, "XI_MOVE: need arg");
            emit_vref(out, v->args[0]);
            break;

        case XI_DROP_REUSE:
            XR_DCHECK(v->nargs >= 1, "XI_DROP_REUSE: need arg");
            fprintf(out, "xrt_drop_reuse(");
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            break;

        case XI_ALLOC_AT: {
            XR_DCHECK(v->nargs >= 1, "XI_ALLOC_AT: need token arg");
            uint8_t gc_type = (uint8_t) ((v->aux_int >> 16) & 0xFF);
            uint32_t alloc_sz = (uint32_t) (v->aux_int & 0xFFFF);
            fprintf(out, "xrt_alloc_at(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", %u, %u)", (unsigned) gc_type, (unsigned) alloc_sz);
            break;
        }

            /* ============ Stack Allocation ============ */

        case XI_STACK_ALLOC: {
            int32_t orig_op = v->aux_int;
            if (orig_op == XI_ARRAY_NEW) {
                int64_t cap = (v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST)
                                  ? v->args[0]->aux_int
                                  : 4;
                fprintf(out, "xrt_array_stack_new(%" PRId64 ")", cap);
            } else if (orig_op == XI_MAP_NEW) {
                /* map: fallback to heap (stack map not yet implemented) */
                int64_t cap = (v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST)
                                  ? v->args[0]->aux_int
                                  : 8;
                fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
            } else if (orig_op == XI_SET_NEW) {
                int64_t cap = (v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST)
                                  ? v->args[0]->aux_int
                                  : 8;
                fprintf(out, "xrt_set_new(%" PRId64 ")", cap);
            } else {
                /* Unknown original op: fallback to XR_NULL_VAL */
                fprintf(out, "XR_NULL_VAL /* STACK_ALLOC: unknown orig_op %d */", orig_op);
            }
            break;
        }

        /* ============ Assertions ============ */

        /* assert(cond): aux=location string, aux_int: 0=truthy, 1=falsy */
        case XI_ASSERT: {
            XR_DCHECK(v->nargs >= 1, "XI_ASSERT: need cond");
            const char *loc = v->aux ? (const char *) v->aux : "<unknown>";
            bool invert = (v->aux_int == 1);
            if (invert) {
                fprintf(out, "(xr_truthy(");
                emit_vref(out, v->args[0]);
                fprintf(out,
                        ") ? (fprintf(stderr, \"Assertion failed (expected false): %s\\n\"), "
                        "abort(), XR_NULL_VAL) : XR_NULL_VAL)",
                        loc);
            } else {
                fprintf(out, "(!xr_truthy(");
                emit_vref(out, v->args[0]);
                fprintf(out,
                        ") ? (fprintf(stderr, \"Assertion failed: %s\\n\"), abort(), XR_NULL_VAL) "
                        ": XR_NULL_VAL)",
                        loc);
            }
            break;
        }

        /* assert_eq(actual, expected): aux=location string */
        case XI_ASSERT_EQ: {
            XR_DCHECK(v->nargs >= 2, "XI_ASSERT_EQ: need 2 args");
            const char *loc = v->aux ? (const char *) v->aux : "<unknown>";
            fprintf(out, "(xrt_eq(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out,
                    ") ? XR_NULL_VAL : (fprintf(stderr, \"assert_eq failed: %s\\n\"), abort(), "
                    "XR_NULL_VAL))",
                    loc);
            break;
        }

        /* assert_ne(actual, unexpected): aux=location string */
        case XI_ASSERT_NE: {
            XR_DCHECK(v->nargs >= 2, "XI_ASSERT_NE: need 2 args");
            const char *loc = v->aux ? (const char *) v->aux : "<unknown>";
            fprintf(out, "(!xrt_eq(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out,
                    ") ? XR_NULL_VAL : (fprintf(stderr, \"assert_ne failed: %s\\n\"), abort(), "
                    "XR_NULL_VAL))",
                    loc);
            break;
        }

        /* ============ Exception Handling ============ */

        /* TRY/END_TRY: handled structurally in emit_value_stmt */
        case XI_TRY:
        case XI_END_TRY:
            fprintf(out, "XR_NULL_VAL");
            break;

        /* CATCH: destination receives caught exception from the frame.
         * Find the matching XI_TRY to use the correct _efN. */
        case XI_CATCH: {
            uint32_t try_id = 0;
            for (uint32_t bi = 0; bi < f->nblocks; bi++) {
                const XiBlock *blk2 = f->blocks[bi];
                if (!blk2)
                    continue;
                for (uint32_t vi2 = 0; vi2 < blk2->nvalues; vi2++) {
                    const XiValue *tv = blk2->values[vi2];
                    if (tv && tv->op == XI_TRY)
                        try_id = tv->id;
                }
            }
            fprintf(out, "_ef%u.exception", try_id);
            break;
        }

        /* Defer: no-op in emit_value_rhs — actual calls are emitted
         * before RETURN terminators in emit_block(). */
        case XI_DEFER:
            fprintf(out, "XR_NULL_VAL");
            break;

        case XI_ERR_CHECK:
            if (cg_rep(v) == XR_REP_I64)
                fprintf(out, "0");
            else
                fprintf(out, "XR_NULL_VAL");
            break;

        /* Builtin calls: dispatches both AST-lowered builtins (dump, copy,
         * chr, etc.) and backend-lowered builtins (print, iter_*, etc.). */
        case XI_CALL_BUILTIN: {
            const char *bn = v->aux ? (const char *) v->aux : "";

            if (strcmp(bn, "print") == 0) {
                int flags = (int) v->aux_int;
                bool add_space = (flags & 1) != 0;
                bool newline = (flags & 2) != 0;
                if (add_space)
                    fprintf(out, "(putchar(' '), ");
                fprintf(out, "%s(", newline ? "xrt_println" : "xrt_print");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
                if (add_space)
                    fprintf(out, ")");
            } else if (strcmp(bn, "str_concat") == 0) {
                if (v->nargs == 2) {
                    fprintf(out, "xrt_add(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", ");
                    emit_vref(out, v->args[1]);
                    fprintf(out, ")");
                } else {
                    fprintf(out, "xrt_str_concat(%u", v->nargs);
                    for (uint16_t i = 0; i < v->nargs; i++) {
                        fprintf(out, ", ");
                        emit_vref(out, v->args[i]);
                    }
                    fprintf(out, ")");
                }
            } else if (strcmp(bn, "array_new") == 0) {
                int64_t cap =
                    (v->nargs >= 1 && v->args[0]->op == XI_CONST) ? v->args[0]->aux_int : 4;
                fprintf(out, "xrt_array_new(%" PRId64 ")", cap);
            } else if (strcmp(bn, "map_new") == 0) {
                int64_t cap =
                    (v->nargs >= 1 && v->args[0]->op == XI_CONST) ? v->args[0]->aux_int : 8;
                fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
            } else if (strcmp(bn, "set_new") == 0) {
                fprintf(out, "xrt_map_new(8)");
            } else if (strcmp(bn, "json_new") == 0) {
                int64_t fc = v->aux_int > 0 ? v->aux_int : 0;
                fprintf(out, "xrt_json_new(%" PRId64 ")", fc);
            } else if (strcmp(bn, "json_init_f") == 0 || strcmp(bn, "json_set_f") == 0) {
                fprintf(out, "xrt_json_set_field(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", %d, ", (int) v->aux_int);
                emit_vref(out, v->args[1]);
                fprintf(out, ")");
            } else if (strcmp(bn, "json_get_f") == 0) {
                fprintf(out, "xrt_json_get_field(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", %d)", (int) v->aux_int);
            } else if (strcmp(bn, "iter_new") == 0) {
                XR_DCHECK(v->nargs >= 1, "builtin iter_new: need arg");
                fprintf(out, "xrt_method_0(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", %d)", XRT_SYM_ITERATOR);
            } else if (strcmp(bn, "iter_valid") == 0) {
                XR_DCHECK(v->nargs >= 1, "builtin iter_valid: need arg");
                fprintf(out, "xr_truthy(xrt_method_0(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", %d))", XRT_SYM_HAS_NEXT);
            } else if (strcmp(bn, "iter_next") == 0) {
                XR_DCHECK(v->nargs >= 1, "builtin iter_next: need arg");
                fprintf(out, "xrt_method_0(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", %d)", XRT_SYM_NEXT);
            } else if (strcmp(bn, "slice") == 0) {
                XR_DCHECK(v->nargs >= 3, "builtin slice: need 3 args");
                fprintf(out, "xrt_slice(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", ");
                emit_vref(out, v->args[1]);
                fprintf(out, ", ");
                emit_vref(out, v->args[2]);
                fprintf(out, ")");
            } else if (strcmp(bn, "range") == 0) {
                XR_DCHECK(v->nargs >= 2, "builtin range: need 2 args");
                fprintf(out, "xrt_range(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", ");
                emit_vref(out, v->args[1]);
                fprintf(out, ")");
            } else if (strcmp(bn, "typeof") == 0) {
                XR_DCHECK(v->nargs >= 1, "builtin typeof: need arg");
                if (v->aux_int == 1) {
                    fprintf(out, "xr_typename(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ")");
                } else {
                    fprintf(out, "XR_FROM_INT(xr_typeof_id(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, "))");
                }
            } else if (strcmp(bn, "regex_compile") == 0) {
                XR_DCHECK(v->nargs >= 2, "builtin regex_compile: need 2 args");
                fprintf(out, "xr_regex_compile_literal(iso, ");
                emit_vref(out, v->args[0]);
                fprintf(out, ", ");
                emit_vref(out, v->args[1]);
                fprintf(out, ")");
            } else {
                /* Hard fail: unrecognized builtin in AOT codegen. */
                fprintf(stderr, "[xi_cgen] ERROR: unknown builtin '%s'\n", bn);
                emit_codegen_abort_expr(out);
                ctx->error = true;
            }
            break;
        }

        /* Cross-module import reference: use resolved indices when available,
         * otherwise fall back to string-scanning the import table. */
        case XI_IMPORT_REF: {
            const XiImportRef *ref = (const XiImportRef *) v->aux;
            bool found = false;
            if (ref && ref->resolved_mod_index >= 0 && ref->resolved_shared_slot >= 0 &&
                ref->resolved_mod_index < ctx->all_nmodules &&
                ctx->all_modules[ref->resolved_mod_index]) {
                /* Fast path: resolved at compile time via module graph */
                const char *tname = ctx->all_modules[ref->resolved_mod_index]->name;
                fprintf(out, "xrt_shared_%s[%d]", tname ? tname : "mod", ref->resolved_shared_slot);
                found = true;
            }
            if (!found && ref) {
                for (int ii = 0; ii < ctx->nimports; ii++) {
                    if (ctx->imports[ii].module_path && ref->module_path &&
                        strcmp(ctx->imports[ii].module_path, ref->module_path) == 0 &&
                        ctx->imports[ii].member_name && ref->member_name &&
                        strcmp(ctx->imports[ii].member_name, ref->member_name) == 0) {
                        fprintf(out, "xrt_shared_%s[%d]", ctx->imports[ii].target_mod_name,
                                ctx->imports[ii].shared_slot);
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                fprintf(out, "XR_NULL_VAL /* unresolved import: %s.%s */",
                        ref && ref->module_path ? ref->module_path : "?",
                        ref && ref->member_name ? ref->member_name : "?");
            }
            break;
        }

        /* Class creation: register the type in xrt_type_table.
         * For monomorphized classes, also link to skeleton via generic_origin
         * and set concrete type arg display names for Reflect.typeOf. */
        case XI_CLASS_CREATE: {
            const XiClassData *cd = (const XiClassData *) v->aux;
            if (!cd) {
                fprintf(out, "XR_NULL_VAL /* class descriptor: no data */");
                break;
            }
            const char *name = cd->class_name ? cd->class_name : "?";
            if (cd->is_monomorphized && cd->display_name) {
                /* Emit static type arg names array + register + set_generic.
                 * The skeleton's type_id is resolved by scanning xrt_type_table
                 * at runtime (skeleton is always registered first). */
                fprintf(out, "({ ");
                /* Emit static const char* array for type arg names */
                if (cd->mono_type_arg_count > 0 && cd->mono_type_arg_names) {
                    fprintf(out, "static const char *_ta_%s[] = {", name);
                    for (int ti = 0; ti < cd->mono_type_arg_count; ti++) {
                        fprintf(out, "%s\"%s\"", ti > 0 ? ", " : "",
                                cd->mono_type_arg_names[ti] ? cd->mono_type_arg_names[ti]
                                                            : "unknown");
                    }
                    fprintf(out, "}; ");
                }
                fprintf(out, "uint16_t _tid = xrt_type_register(\"%s\", 0, NULL, 0, NULL, 0); ",
                        name);
                /* Find skeleton type_id by scanning the table for display_name match */
                fprintf(out,
                        "uint16_t _orig = 0; "
                        "for (uint16_t _i = 1; _i < xrt_type_count; _i++) "
                        "{ if (xrt_type_table[_i].name && strcmp(xrt_type_table[_i].name, \"%s\") "
                        "== 0) "
                        "{ _orig = _i; break; } } ",
                        cd->display_name);
                fprintf(out, "xrt_type_set_generic(_tid, _orig, \"%s\", ", cd->display_name);
                if (cd->mono_type_arg_count > 0 && cd->mono_type_arg_names) {
                    fprintf(out, "_ta_%s, %d", name, cd->mono_type_arg_count);
                } else {
                    fprintf(out, "NULL, 0");
                }
                fprintf(out, "); XR_FROM_INT(_tid); })");
            } else {
                fprintf(out, "XR_FROM_INT(xrt_type_register(\"%s\", 0, NULL, 0, NULL, 0))", name);
            }
            break;
        }

        default:
            fprintf(stderr, "[xi_cgen] ERROR: unsupported Xi op %s (%d)\n", xi_op_name(v->op),
                    v->op);
            emit_codegen_abort_expr(out);
            ctx->error = true;
            break;
    }
}

/* Emit a complete value statement: type vN = <rhs>; */
static void emit_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    XR_DCHECK(v != NULL, "emit_value_stmt: NULL value");

    /* Inlined struct: emit local anonymous C struct (no XrValue variable).
     * Fields are stored as XrValue for type compatibility with GET/SET. */
    if (v->op == XI_STRUCT_NEW && cg_struct_can_inline(f, v)) {
        XrStructLayout *sl = (XrStructLayout *) v->aux;
        XR_DCHECK(sl != NULL, "inlined XI_STRUCT_NEW: missing layout");
        fprintf(out, "    struct { ");
        for (uint16_t i = 0; i < sl->field_count; i++)
            fprintf(out, "XrValue f%u; ", i);
        fprintf(out, "} _st%u = {{0}};\n", v->id);
        return;
    }

    /* COPY of an inlined struct is a no-op — GET/SET trace through
     * the COPY chain to the origin _st variable. */
    if (v->op == XI_COPY && v->nargs >= 1) {
        const XiValue *origin = cg_trace_struct_new(v);
        if (origin && cg_struct_can_inline(f, origin))
            return;
    }

    /* Side-effect-only values that don't produce a named result. */
    bool void_like = cg_is_void_like(v);

    if (void_like) {
        fprintf(out, "    ");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
        return;
    }

    /* XI_TRY: emit setjmp/longjmp exception frame setup.
     * The exception frame must be declared at function scope so it
     * survives across goto labels.  Use the value ID for uniqueness. */
    if (v->op == XI_TRY) {
        const XiBlock *catch_blk = (const XiBlock *) v->aux;
        fprintf(out, "    XrtExcFrame _ef%u;\n", v->id);
        fprintf(out, "    _ef%u.prev = xrt_exc_top;\n", v->id);
        fprintf(out, "    xrt_exc_top = &_ef%u;\n", v->id);
        if (catch_blk) {
            fprintf(out,
                    "    if (setjmp(_ef%u.buf) != 0) {"
                    " xrt_exc_top = _ef%u.prev; goto L%u; }\n",
                    v->id, v->id, catch_blk->id);
        } else {
            fprintf(out,
                    "    if (setjmp(_ef%u.buf) != 0) {"
                    " xrt_exc_top = _ef%u.prev; }\n",
                    v->id, v->id);
        }
        return;
    }

    /* XI_END_TRY: pop the exception frame.
     * Finds the matching XI_TRY by scanning earlier blocks. */
    if (v->op == XI_END_TRY) {
        /* Find the TRY value ID for the matching _efN variable */
        uint32_t try_id = 0;
        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            const XiBlock *blk = f->blocks[bi];
            if (!blk)
                continue;
            for (uint32_t vi2 = 0; vi2 < blk->nvalues; vi2++) {
                const XiValue *tv = blk->values[vi2];
                if (tv && tv->op == XI_TRY)
                    try_id = tv->id;
            }
        }
        fprintf(out, "    xrt_exc_top = _ef%u.prev;\n", try_id);
        return;
    }

    if (ctx->pre_decl_all) {
        /* Variable already declared at function top — emit assignment */
        fprintf(out, "    ");
        emit_vref(out, v);
        fprintf(out, " = ");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
    } else {
        XrRep rep = cg_rep(v);
        fprintf(out, "    %s ", ctype_str(rep));
        emit_vref(out, v);
        fprintf(out, " = ");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
    }
}

/* ========== PHI Elimination ========== */

/* Emit phi assignments for all phis in `target` whose predecessor
 * at index `pred_idx` is `pred_blk`. Called before the jump/branch. */
static void emit_phi_copies(FILE *out, const XiBlock *target, uint16_t pred_idx) {
    for (const XiPhi *phi = target->phis; phi; phi = phi->next) {
        if (pred_idx < phi->value.nargs && phi->value.args[pred_idx]) {
            fprintf(out, "    ");
            emit_phi_ref(out, phi);
            fprintf(out, " = ");
            emit_vref(out, phi->value.args[pred_idx]);
            fprintf(out, ";\n");
        }
    }
}

/* Find predecessor index of `pred` in `blk`. */
static uint16_t find_pred_idx(const XiBlock *blk, const XiBlock *pred) {
    for (uint16_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i] == pred)
            return i;
    }
    return 0; /* fallback; should not happen in valid IR */
}

/* ========== Block Emission ========== */

static void emit_block(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiBlock *blk,
                       const char *prefix) {
    XR_DCHECK(blk != NULL, "emit_block: NULL block");

    /* Label (skip for entry block b0 to reduce clutter) */
    if (blk->id != 0)
        fprintf(out, "L%u:;\n", blk->id);

    /* Instructions */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (!v)
            continue;
        emit_value_stmt(ctx, out, f, v, prefix);
    }

    /* Terminator */
    switch (blk->kind) {
        case XI_BLOCK_RETURN: {
            /* Call deferred closures in LIFO order before returning.
             * Scan all blocks for XI_DEFER whose args[0] is CLOSURE_NEW.
             * Pass the closure's xrt_closure_t* as hidden first arg so
             * the deferred function can access captured upvalues. */
            const XiValue *deferred_vals[32];
            int ndeferred = 0;
            for (uint32_t dbi = 0; dbi < f->nblocks && ndeferred < 32; dbi++) {
                const XiBlock *db = f->blocks[dbi];
                if (!db)
                    continue;
                for (uint32_t dvi = 0; dvi < db->nvalues; dvi++) {
                    const XiValue *dv = db->values[dvi];
                    if (!dv || dv->op != XI_DEFER || dv->nargs < 1)
                        continue;
                    const XiValue *callee = dv->args[0];
                    if (callee && callee->op == XI_CLOSURE_NEW && callee->aux)
                        deferred_vals[ndeferred++] = callee;
                }
            }
            for (int di = ndeferred - 1; di >= 0; di--) {
                const XiValue *cv = deferred_vals[di];
                const XiFunc *cf = (const XiFunc *) cv->aux;
                fprintf(out, "    ");
                emit_fname(ctx, out, prefix, cf);
                if (cf->ncaptures > 0) {
                    fprintf(out, "((xrt_closure_t*)");
                    emit_vref(out, cv);
                    fprintf(out, ".ptr);\n");
                } else {
                    fprintf(out, "(NULL);\n");
                }
            }
            if (blk->control) {
                fprintf(out, "    return ");
                emit_vref(out, blk->control);
                fprintf(out, ";\n");
            } else {
                fprintf(out, "    return XR_NULL_VAL;\n");
            }
            break;
        }

        case XI_BLOCK_PLAIN:
            if (blk->succs[0]) {
                emit_phi_copies(out, blk->succs[0], find_pred_idx(blk->succs[0], blk));
                fprintf(out, "    goto L%u;\n", blk->succs[0]->id);
            }
            break;

        case XI_BLOCK_IF:
            XR_DCHECK(blk->control != NULL, "IF block missing control");
            XR_DCHECK(blk->succs[0] != NULL, "IF block missing then");
            XR_DCHECK(blk->succs[1] != NULL, "IF block missing else");
            /* Emit phi copies for both branches */
            fprintf(out, "    if (");
            emit_condition_expr(out, blk->control);
            fprintf(out, ") {\n");
            emit_phi_copies(out, blk->succs[0], find_pred_idx(blk->succs[0], blk));
            fprintf(out, "        goto L%u;\n", blk->succs[0]->id);
            fprintf(out, "    } else {\n");
            emit_phi_copies(out, blk->succs[1], find_pred_idx(blk->succs[1], blk));
            fprintf(out, "        goto L%u;\n", blk->succs[1]->id);
            fprintf(out, "    }\n");
            break;

        case XI_BLOCK_UNREACHABLE:
            fprintf(out, "    __builtin_unreachable();\n");
            break;

        default:
            fprintf(out, "    /* unknown block kind %d */\n", blk->kind);
            break;
    }
}

#include "xi_cgen_coro.inc"

/* ========== Function Emission ========== */

/* Check whether a function contains any exception handling ops.
 * If so, all variables must be pre-declared at function scope to
 * avoid jumping over declarations with initializers via goto. */
static bool cg_has_exception_handling(const XiFunc *f) {
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            if (blk->values[vi] && blk->values[vi]->op == XI_TRY)
                return true;
        }
    }
    return false;
}

/* Collect all values and phis to declare at function top.
 * When the function contains exception handling (setjmp/goto),
 * ALL SSA values are pre-declared to avoid jumping over decls. */
static void emit_declarations(FILE *out, const XiFunc *f) {
    bool pre_decl_all = cg_has_exception_handling(f);

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        /* Phi variables (always pre-declared) */
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            XrRep rep = cg_rep(&phi->value);
            fprintf(out, "    %s ", ctype_str(rep));
            emit_phi_ref(out, phi);
            if (rep == XR_REP_TAGGED)
                fprintf(out, " = XR_NULL_VAL;\n");
            else
                fprintf(out, " = 0;\n");
        }

        /* SSA values (only pre-declared when exception handling present) */
        if (pre_decl_all) {
            for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
                const XiValue *v = blk->values[vi];
                if (!v)
                    continue;
                /* Skip void-like ops, exception ops, and inlined structs */
                if (cg_is_void_like(v) || v->op == XI_TRY || v->op == XI_END_TRY)
                    continue;
                if (v->op == XI_STRUCT_NEW && cg_struct_can_inline(f, v))
                    continue;
                if (v->op == XI_COPY && v->nargs >= 1) {
                    const XiValue *origin = cg_trace_struct_new(v);
                    if (origin && cg_struct_can_inline(f, origin))
                        continue;
                }
                XrRep rep = cg_rep(v);
                fprintf(out, "    %s ", ctype_str(rep));
                emit_vref(out, v);
                if (rep == XR_REP_TAGGED)
                    fprintf(out, " = XR_NULL_VAL;\n");
                else
                    fprintf(out, " = 0;\n");
            }
        }
    }
}

static void xi_cgen_func(XiCgenCtx *ctx, FILE *out, XiFunc *f, const char *prefix) {
    XR_DCHECK(out != NULL, "xi_cgen_func: NULL output");
    XR_DCHECK(f != NULL, "xi_cgen_func: NULL func");
    /* Auto-lower to STAGE_BACKEND if caller bypasses the pipeline
     * (e.g. unit tests that construct IR manually). */
    if (f->stage < XI_STAGE_REPPED) {
        xi_opt_select_rep(f);
        xi_opt_box_elim(f);
    }
    if (f->stage < XI_STAGE_BACKEND)
        xi_backend_lower(f);

    /* Emit nested children first (forward declarations already emitted) */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_cgen_func(ctx, out, f->children[i], prefix);
    }

    if (cg_func_needs_aot_coro(f)) {
        xi_cgen_coro_func(ctx, out, f, prefix);
        return;
    }

    /* Function signature.  Closure children with captures receive a hidden
     * first parameter xrt_closure_t *_cl for per-closure upvalue access. */
    bool has_cl = (f->ncaptures > 0);
    fprintf(out, "static XrValue ");
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "(");
    if (has_cl) {
        fprintf(out, "xrt_closure_t *_cl");
        for (uint16_t i = 0; i < f->nparams; i++)
            fprintf(out, ", XrValue p%u", i);
    } else if (f->nparams == 0) {
        fprintf(out, "xrt_closure_t *_cl");
    } else {
        fprintf(out, "xrt_closure_t *_cl");
        for (uint16_t i = 0; i < f->nparams; i++)
            fprintf(out, ", XrValue p%u", i);
    }
    fprintf(out, ") {\n");
    if (!has_cl)
        fprintf(out, "    (void)_cl;\n");

    /* Shared array declared at file scope by xi_cgen_program(),
     * so child functions can reference it too. */

    /* Pre-declare variables (phis always; all SSA values when
     * exception handling is present to avoid goto-over-decl UB). */
    ctx->pre_decl_all = cg_has_exception_handling(f);
    emit_declarations(out, f);

    /* Blocks in order */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (f->blocks[bi])
            emit_block(ctx, out, f, f->blocks[bi], prefix);
    }

    fprintf(out, "}\n\n");

    if (cg_func_can_emit_sync_go_wrapper(f))
        emit_sync_go_wrapper(ctx, out, f, prefix);
}

/* ========== Forward Declarations ========== */

static void emit_forward_decls(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    /* Recurse children first */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            emit_forward_decls(ctx, out, f->children[i], prefix);
    }

    fprintf(out, "static XrValue ");
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "(");
    fprintf(out, "xrt_closure_t *_cl");
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, ", XrValue p%u", i);
    fprintf(out, ");\n");

    if (cg_func_needs_aot_coro(f) || cg_func_can_emit_sync_go_wrapper(f)) {
        fprintf(out, "static void *");
        emit_fname_suffix(ctx, out, prefix, f, "_aot_frame_new");
        fprintf(out, "(");
        emit_aot_frame_new_params(out, f);
        fprintf(out, ");\n");
        fprintf(out, "static XrAotResult ");
        emit_fname_suffix(ctx, out, prefix, f, "_aot_resume");
        fprintf(out, "(void *raw_frame, const XrAotContext *ctx);\n");
        fprintf(out, "static const XrAotCoroDesc ");
        emit_fname_suffix(ctx, out, prefix, f, "_aot_desc");
        fprintf(out, ";\n");
    }
}

/* ========== Cross-module Import Resolution ========== */

/* Derive the relative import path from exporter to importer directory.
 * E.g. "/a/b/math.xr" from dir "/a/b" → "./math".  Caller must free. */
static char *cg_derive_import_string(const char *target_path, const char *importer_dir) {
    size_t dir_len = strlen(importer_dir);
    if (strncmp(target_path, importer_dir, dir_len) == 0 && target_path[dir_len] == '/') {
        const char *filename = target_path + dir_len + 1;
        size_t flen = strlen(filename);
        if (flen > 3 && strcmp(filename + flen - 3, ".xr") == 0)
            flen -= 3;
        char *result = (char *) xr_malloc(2 + flen + 1);
        if (!result)
            return NULL;
        result[0] = '.';
        result[1] = '/';
        memcpy(result + 2, filename, flen);
        result[2 + flen] = '\0';
        return result;
    }
    const char *base = strrchr(target_path, '/');
    base = base ? base + 1 : target_path;
    size_t blen = strlen(base);
    if (blen > 3 && strcmp(base + blen - 3, ".xr") == 0)
        blen -= 3;
    char *result = (char *) xr_malloc(2 + blen + 1);
    if (!result)
        return NULL;
    result[0] = '.';
    result[1] = '/';
    memcpy(result + 2, base, blen);
    result[2 + blen] = '\0';
    return result;
}

/* Add one entry to the internal import table. */
static void cg_add_import(XiCgenCtx *ctx, const char *module_path, const char *member_name,
                          const char *target_mod_name, int shared_slot, const XiFunc *target_func,
                          const XiClassData *target_class, const XiFunc *exporter_func) {
    if (ctx->nimports >= CG_MAX_IMPORTS)
        return;
    CgImportEntry *e = &ctx->imports[ctx->nimports++];
    e->module_path = module_path;
    e->member_name = member_name;
    e->target_mod_name = target_mod_name;
    e->shared_slot = shared_slot;
    e->target_func = target_func;
    e->target_class = target_class;
    e->exporter_func = exporter_func;
}

XR_FUNC void xi_cgen_resolve_module_imports(XiCgenCtx *ctx, XiModule **modules, int nmodules) {
    XR_DCHECK(ctx != NULL, "xi_cgen_resolve_module_imports: NULL ctx");
    if (!modules || nmodules <= 1)
        return;

    ctx->nimports = 0;
    memset(ctx->imports, 0, sizeof(ctx->imports));
    ctx->all_modules = modules;
    ctx->all_nmodules = nmodules;

    for (int exporter = 0; exporter < nmodules; exporter++) {
        XiModule *emod = modules[exporter];
        if (!emod || emod->nexports == 0)
            continue;

        for (int importer = 0; importer < nmodules; importer++) {
            if (importer == exporter)
                continue;
            XR_DCHECK(modules[importer] != NULL,
                      "xi_cgen_resolve_module_imports: NULL importer module");

            /* Derive importer directory from its path */
            char importer_dir[1024];
            const char *imp_path = modules[importer]->path;
            if (!imp_path)
                continue;
            strncpy(importer_dir, imp_path, sizeof(importer_dir) - 1);
            importer_dir[sizeof(importer_dir) - 1] = '\0';
            char *slash = strrchr(importer_dir, '/');
            if (slash)
                *slash = '\0';

            char *import_str = cg_derive_import_string(emod->path, importer_dir);
            if (!import_str)
                continue;

            for (uint16_t ei = 0; ei < emod->nexports; ei++) {
                const XiModuleExport *exp = &emod->exports[ei];
                const XiFunc *target_fn = exp->function;
                const XiClassData *target_cd = exp->class_data;

                /* For class exports, resolve constructor if not already set */
                if (target_cd && !target_fn && target_cd->methods) {
                    for (uint16_t mi = 0; mi < target_cd->nmethod; mi++) {
                        if (target_cd->methods[mi].is_constructor && target_cd->child_idx &&
                            mi < target_cd->ninst + target_cd->nstat) {
                            uint16_t idx = target_cd->child_idx[mi];
                            if (idx < emod->init->nchildren) {
                                target_fn = emod->init->children[idx];
                                break;
                            }
                        }
                    }
                }

                cg_add_import(ctx, import_str, exp->name, emod->name, (int) exp->shared_slot,
                              target_fn, target_cd, emod->init);
            }
            /* import_str leaked intentionally — pointers stored in table,
             * freed implicitly at process exit (short-lived AOT tool). */
        }
    }
}

/* ========== Multi-module API ========== */

XR_FUNC void xi_cgen_header(FILE *out) {
    XR_DCHECK(out != NULL, "xi_cgen_header: NULL output");
    fprintf(out, "/* Generated by xi_cgen \xe2\x80\x94 do not edit */\n");
    fprintf(out, "#define XRT_IMPL\n");
    fprintf(out, "#include \"xrt.h\"\n\n");
    fprintf(out, "#include \"xray.h\"\n");
    fprintf(out, "#include \"xray_isolate.h\"\n");
    fprintf(out, "#include \"xaot_coro.h\"\n\n");
}

XR_FUNC void xi_cgen_module(XiCgenCtx *ctx, FILE *out, XiModule *module) {
    XR_DCHECK(ctx != NULL, "xi_cgen_module: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_module: NULL output");
    XR_DCHECK(module != NULL, "xi_cgen_module: NULL module");
    XR_DCHECK(module->init != NULL, "xi_cgen_module: NULL init func");

    const char *prefix = module->name ? module->name : "mod";
    XiFunc *module_func = module->init;

    /* Initialize ctx from module metadata (no IR scanning) */
    cg_init_from_module(ctx, module);
    cg_register_imported_classes(ctx);

    /* Module-scoped shared variable array.
     * Multi-module builds use a prefixed name to avoid collisions. */
    char shared_buf[256];
    snprintf(shared_buf, sizeof(shared_buf), "xrt_shared_%s", prefix);
    ctx->shared_name = shared_buf;

    if (module_func->nshared > 0)
        fprintf(out, "static XrValue %s[%u];\n", ctx->shared_name, module_func->nshared);

    fprintf(out, "\n");

    /* Forward declarations */
    emit_forward_decls(ctx, out, module_func, prefix);
    fprintf(out, "\n");

    /* Function bodies */
    xi_cgen_func(ctx, out, module_func, prefix);

    /* Reset shared name to default */
    ctx->shared_name = "xrt_shared";
}

XR_FUNC void xi_cgen_main(XiCgenCtx *ctx, FILE *out, XiModule **modules, int n, int entry_index) {
    XR_DCHECK(ctx != NULL, "xi_cgen_main: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_main: NULL output");
    XR_DCHECK(n > 0, "xi_cgen_main: no modules");
    XR_DCHECK(entry_index >= 0 && entry_index < n, "xi_cgen_main: bad entry_index");

    bool entry_is_coro = modules[entry_index] && modules[entry_index]->init &&
                         cg_func_needs_aot_coro(modules[entry_index]->init);

    fprintf(out, "int main(void) {\n");
    fprintf(out, "    xrt_bump_enabled = 1;\n");
    fprintf(out, "    xrt_arc_init();\n");
    if (entry_is_coro) {
        fprintf(out, "    XrayIsolateParams params;\n");
        fprintf(out, "    xray_isolate_params_init(&params);\n");
        fprintf(out, "    xray_isolate_setup_full(&params);\n");
        fprintf(out, "    params.init_flags = XR_INIT_RUNTIME;\n");
        fprintf(out, "    XrayIsolate *X = xray_isolate_new(&params);\n");
        fprintf(out, "    if (!X) return 1;\n");
        fprintf(out, "    xr_multicore_init(X, 0);\n");
    }
    for (int m = 0; m < n; m++) {
        if (!modules[m] || !modules[m]->init)
            continue;
        if (m == entry_index && entry_is_coro) {
            fprintf(out, "    void *_entry_frame = ");
            emit_fname_suffix(ctx, out, modules[m]->name ? modules[m]->name : "mod",
                              modules[m]->init, "_aot_frame_new");
            fprintf(out, "();\n");
            fprintf(out, "    xr_aot_run_main(X, &");
            emit_fname_suffix(ctx, out, modules[m]->name ? modules[m]->name : "mod",
                              modules[m]->init, "_aot_desc");
            fprintf(out, ", _entry_frame);\n");
        } else {
            if (cg_func_needs_aot_coro(modules[m]->init)) {
                fprintf(stderr,
                        "[xi_cgen] ERROR: suspendable AOT dependency module init '%s' must "
                        "be the entry module\n",
                        modules[m]->name ? modules[m]->name : "mod");
                ctx->error = true;
                fprintf(out, "    return 1;\n");
                continue;
            }
            fprintf(out, "    ");
            emit_fname(ctx, out, modules[m]->name ? modules[m]->name : "mod", modules[m]->init);
            fprintf(out, "(NULL);\n");
        }
    }
    if (entry_is_coro) {
        fprintf(out, "    xr_multicore_destroy(X);\n");
        fprintf(out, "    xray_isolate_delete(X);\n");
    }
    fprintf(out, "    xrt_bump_destroy();\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
}

XR_FUNC void xi_cgen_program(XiCgenCtx *ctx, FILE *out, XiModule *module) {
    XR_DCHECK(ctx != NULL, "xi_cgen_program: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_program: NULL output");
    XR_DCHECK(module != NULL, "xi_cgen_program: NULL module");
    XR_DCHECK(module->init != NULL, "xi_cgen_program: NULL init func");

    XiFunc *main_func = module->init;
    const char *prefix = module->name ? module->name : "mod";

    /* Reset function name counter for each compilation unit */
    ctx->fname_counter = 0;

    /* Initialize from module metadata (no IR scanning) */
    cg_init_from_module(ctx, module);
    ctx->shared_name = "xrt_shared";

    /* Header */
    xi_cgen_header(out);

    /* File-scope shared variable array */
    if (main_func->nshared > 0)
        fprintf(out, "static XrValue xrt_shared[%u];\n", main_func->nshared);

    fprintf(out, "\n");

    /* Forward declarations */
    emit_forward_decls(ctx, out, main_func, prefix);
    fprintf(out, "\n");

    /* Function bodies */
    xi_cgen_func(ctx, out, main_func, prefix);

    /* Entry point */
    fprintf(out, "int main(void) {\n");
    fprintf(out, "    xrt_bump_enabled = 1;\n");
    fprintf(out, "    xrt_arc_init();\n");
    if (cg_func_needs_aot_coro(main_func)) {
        fprintf(out, "    XrayIsolateParams params;\n");
        fprintf(out, "    xray_isolate_params_init(&params);\n");
        fprintf(out, "    xray_isolate_setup_full(&params);\n");
        fprintf(out, "    params.init_flags = XR_INIT_RUNTIME;\n");
        fprintf(out, "    XrayIsolate *X = xray_isolate_new(&params);\n");
        fprintf(out, "    if (!X) return 1;\n");
        fprintf(out, "    xr_multicore_init(X, 0);\n");
        fprintf(out, "    void *_entry_frame = ");
        emit_fname_suffix(ctx, out, prefix, main_func, "_aot_frame_new");
        fprintf(out, "();\n");
        fprintf(out, "    xr_aot_run_main(X, &");
        emit_fname_suffix(ctx, out, prefix, main_func, "_aot_desc");
        fprintf(out, ", _entry_frame);\n");
        fprintf(out, "    xr_multicore_destroy(X);\n");
        fprintf(out, "    xray_isolate_delete(X);\n");
    } else {
        fprintf(out, "    ");
        emit_fname(ctx, out, prefix, main_func);
        fprintf(out, "(NULL);\n");
    }
    fprintf(out, "    xrt_bump_destroy();\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
}
