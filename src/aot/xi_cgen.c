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
 *   - PHI locals are declared at function top to avoid scope issues across labels.
 *   - Each basic block emits a label (L0:, L1:, ...).
 *   - PHI nodes are eliminated by inserting assignments before predecessor jumps.
 *   - Value representation (I64/F64/TAGGED) read from v->rep,
 *     populated by xi_opt_select_rep in the pipeline.
 */
#include "xi_cgen.h"
#include "xaot_bundle.h"
#include "xaot_class_native.h"
#include "xaot_rep_gen.h"
#include "xaot_abi_gen.h"
#include "xaot_layout_gen.h"
#include "xi_to_c_dispatch_gen.h"
#include "xi_to_c_stmt_dispatch_gen.h"
#include "../ir/xi_analysis.h"
#include "../ir/xi_backend_lower.h"
#include "../ir/xi_op_name.h"
#include "../ir/xi_ops_gen.h"
#include "../ir/xi_opt.h"
#include "../ir/xi_own.h"
#include "../ir/xi_range.h"
#include "../base/xdefs.h"
#include "../runtime/class/xenum.h"
#include "../runtime/value/xstruct_layout.h"
#include "../base/xglobal_indices.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "xrt_method_symbols.h"
#include "xrt_hash.h"
#include "../base/xmemstream.h"
#include "../frontend/parser/xast_nodes.h"
#include <string.h>
#include <inttypes.h>
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
        case XR_REP_PTR:
            return "void *";
        default:
            return "XrValue";
    }
}

static const char *cg_native_int_ctype(uint8_t native_width) {
    return xaot_c_type_for_native_int_type(native_width);
}

static uint8_t cg_narrow_int_native_width(const XiValue *v) {
    if (!v || !xi_generated_op_result_native_type(v->op) || !v->type ||
        v->type->kind != XR_KIND_INT || v->type->native_width == 0)
        return 0;
    return v->type->native_width;
}

static bool cg_const_int_fits_native_width(int64_t value, uint8_t native_width) {
    return xaot_native_int_const_fits(native_width, value);
}

static bool cg_value_narrow_local_native_width(const XiValue *v, uint8_t depth,
                                               uint8_t *out_native_width) {
    if (!v || cg_rep(v) != XR_REP_I64 || depth > 8)
        return false;

    uint8_t op_width = cg_narrow_int_native_width(v);
    if (op_width != 0) {
        if (out_native_width)
            *out_native_width = op_width;
        return true;
    }

    if (v->op != XI_PHI)
        return false;

    uint8_t phi_width = 0;
    if (v->type && v->type->kind == XR_KIND_INT && v->type->native_width != 0 &&
        cg_native_int_ctype(v->type->native_width))
        phi_width = v->type->native_width;

    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg)
            return false;
        if (arg->op == XI_CONST)
            continue;
        uint8_t arg_width = 0;
        if (!cg_value_narrow_local_native_width(arg, (uint8_t) (depth + 1), &arg_width) ||
            !cg_native_int_ctype(arg_width))
            return false;
        if (phi_width == 0)
            phi_width = arg_width;
        else if (arg_width != phi_width)
            return false;
    }

    if (phi_width == 0)
        return false;

    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (arg && arg->op == XI_CONST && !cg_const_int_fits_native_width(arg->aux_int, phi_width))
            return false;
    }

    if (out_native_width)
        *out_native_width = phi_width;
    return true;
}

static const char *local_ctype_str(const XiValue *v) {
    uint8_t native_width = 0;
    if (cg_value_narrow_local_native_width(v, 0, &native_width)) {
        const char *ctype = cg_native_int_ctype(native_width);
        if (ctype)
            return ctype;
    }
    return ctype_str(cg_rep(v));
}

static const XiValue *cg_unwrap_identity_value(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY || v->op == XI_MOVE) &&
           v->nargs >= 1) {
        v = v->args[0];
    }
    return v;
}

static bool cg_type_has_no_aot_arc_header(const XrType *type) {
    return type && (type->kind == XR_KIND_ARRAY || type->kind == XR_KIND_MAP ||
                    type->kind == XR_KIND_SET || type->kind == XR_KIND_FIXED_ARRAY);
}

static bool cg_ownership_op_is_noop(const XiValue *v) {
    if (!v || (v->op != XI_RETAIN && v->op != XI_RELEASE) || v->nargs < 1)
        return false;
    const XiValue *arg = cg_unwrap_identity_value(v->args[0]);
    return arg && cg_type_has_no_aot_arc_header(arg->type);
}

/* Check whether an op is void-like (produces no named result). */
static bool cg_is_void_like(const XiValue *v) {
    if (!v)
        return false;
    if (v->type && XR_TYPE_IS_UNIT(v->type))
        return true;

    uint8_t result_kind = xi_generated_op_result_kind(v->op);
    if (result_kind == XI_GEN_RESULT_VOID)
        return true;
    return false;
}

static bool cg_is_unsupported_coroutine_op(uint16_t op) {
    return xi_generated_op_class(op) == XI_GEN_CLASS_COROUTINE;
}

static bool cg_is_aot_suspend_op(uint16_t op) {
    return xi_generated_op_class(op) == XI_GEN_CLASS_COROUTINE;
}

static const XaotBundle *cg_ctx_aot_bundle(const XiCgenCtx *ctx);

#include "xi_cgen_type_helpers.inc.c"

static const XiImportRef *cg_value_import_ref(const XiValue *v) {
    v = cg_unwrap_identity_value(v);
    if (!v || v->op != XI_IMPORT_REF || !v->aux)
        return NULL;
    return (const XiImportRef *) v->aux;
}

static bool cg_import_ref_is_module(const XiValue *v, const char *module_name) {
    const XiImportRef *ref = cg_value_import_ref(v);
    return ref && module_name && ref->module_path && strcmp(ref->module_path, module_name) == 0 &&
           !ref->member_name;
}

static const XiImportRef *cg_shared_slot_import_ref(const XiFunc *f, int slot) {
    if (!f || slot < 0)
        return NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            const XiImportRef *ref = cg_value_import_ref(v->args[0]);
            if (ref)
                return ref;
        }
    }
    return NULL;
}

static bool cg_shared_slot_is_module_import(const XiFunc *f, int slot, const char *module_name) {
    const XiImportRef *ref = cg_shared_slot_import_ref(f, slot);
    return ref && module_name && ref->module_path && strcmp(ref->module_path, module_name) == 0 &&
           !ref->member_name;
}

static bool cg_value_is_module_import(const XiFunc *f, const XiValue *v, const char *module_name) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY) && v->nargs >= 1)
        v = v->args[0];
    if (cg_import_ref_is_module(v, module_name))
        return true;
    if (v && v->op == XI_GET_SHARED) {
        if (cg_shared_slot_is_module_import(f, (int) v->aux_int, module_name))
            return true;
        if (f && f->module && f->module->init != f)
            return cg_shared_slot_is_module_import(f->module->init, (int) v->aux_int, module_name);
    }
    return false;
}

#include "xi_cgen_time_helpers.inc.c"
static bool cg_value_needs_aot_coro(const XiFunc *f, const XiValue *v) {
    if (!v)
        return false;
    if (cg_is_aot_suspend_op(v->op))
        return true;
    return cg_channel_method_may_suspend(v) || cg_work_queue_method_needs_aot_coro(v) ||
           cg_work_queue_constructor_needs_aot_coro(v) || cg_is_time_sleep_call(f, v);
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
/* Initial capacities for the per-module shared-slot / method / import tables.
 * These grow on demand (cg_reserve_shared / _methods / _imports), so AOT can
 * compile modules with more than the starting counts.  The Xi->C path is not
 * constrained by the VM bytecode register operand space. */
#define CG_INIT_SHARED 512
#define CG_INIT_METHODS 256
#define CG_INIT_IMPORTS 256
#define CG_MAX_SYNC_GO_TARGETS 512
#define CG_MAX_CLASS_FIELD_CACHE 16
#define CG_MAX_CLASS_FIELD_CACHE_ALIASES 32
typedef struct {
    const char *class_name; /* owning class (e.g. "Rect") */
    const char *name;       /* method name (e.g. "area") */
    const XiFunc *func;
    const char *module_prefix; /* C function name prefix (NULL = current module) */
    const XiClassData *class_data;
    const XrStructLayout *instance_layout;
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

typedef struct {
    const char *name;
    const XrType *type;
    XrRep rep;
    bool dirty;
    int16_t layout_index;
} CgClassFieldCacheEntry;

typedef struct {
    bool active;
    bool native_receiver;
    const XiValue *receiver;
    const XrStructLayout *layout;
    const XiClassData *class_data;
    uint16_t nreceiver_aliases;
    const XiValue *receiver_aliases[CG_MAX_CLASS_FIELD_CACHE_ALIASES];
    uint16_t nfields;
    CgClassFieldCacheEntry fields[CG_MAX_CLASS_FIELD_CACHE];
} CgClassFieldCache;

typedef struct {
    bool active;
    const XiClassData *class_data;
    const XiFunc *ctor;
    const char *ctor_prefix;
    const XiValue *ctor_call;
} CgSharedNativeInstance;

typedef struct {
    bool active;
    const XiModule *module;
    const char *module_name;
    int module_index;
    int slot;
    const XiClassData *class_data;
} CgSharedNativeExport;

/* Interned string literal: emitted once as a static const xrt_str_t with
 * its content hash precomputed, so literal use sites are zero-cost loads
 * instead of per-evaluation xr_box_str allocations. */
typedef struct CgStrLit {
    char *str; /* owned copy (sources may be transient buffers) */
    size_t len;
    int id;
    struct CgStrLit *next; /* hash bucket chain */
} CgStrLit;

#define CG_STRLIT_BUCKETS 1024

/* All mutable codegen state for one C-generation session.
 * Heap-allocated via xi_cgen_ctx_new; no file-scope globals. */
struct XiCgenCtx {
    int fname_counter;
    /* Parallel shared-slot tables (length shared_cap), grown together. */
    const XiFunc **shared_funcs;
    const XiClassData **shared_class;
    const XiEnumData **shared_enum;
    CgSharedNativeInstance *shared_native_instances;
    int shared_cap;
    CgSharedNativeExport *shared_native_exports;
    int nshared_native_exports;
    int shared_native_exports_cap;
    int nshared;
    CgMethodEntry *methods;
    int methods_cap;
    int nmethod;
    XiModule *module; /* current module being emitted */
    bool pre_decl_all;
    bool *cell_vars;
    const XiValue **cell_origins;
    uint32_t cell_var_count;
    const char *shared_name;
    CgImportEntry *imports;
    int imports_cap;
    int nimports;
    XiModule **all_modules; /* full modules array for resolved-index lookups */
    int all_nmodules;
    bool error; /* set on fatal codegen errors (unknown builtin, etc.) */
    XiCgenStats stats;
    XiCgenCoroFrameStats coro_frame_stats;
    const XaotBundle *aot_bundle;
    const XiFunc *sync_go_targets[CG_MAX_SYNC_GO_TARGETS];
    int nsync_go_targets;
    CgClassFieldCache class_field_cache;
    const XiValue **array_data_cache_decls;
    int narray_data_cache_decls;
    int array_data_cache_decl_cap;
    CgStrLit *strlit_buckets[CG_STRLIT_BUCKETS];
    CgStrLit **strlit_list; /* ordered by id for definition emission */
    int nstrlit;
    int strlit_cap;
};

/* Grow the parallel shared-slot tables to at least `need` entries.  All four
 * arrays share one capacity (a slot holds a func / class / enum / native
 * instance at the same index).  New entries are zeroed.  Returns false (and
 * sets ctx->error) on allocation failure. */
static bool cg_reserve_shared(XiCgenCtx *ctx, int need) {
    if (need <= ctx->shared_cap)
        return true;
    int nc = ctx->shared_cap > 0 ? ctx->shared_cap : CG_INIT_SHARED;
    while (nc < need)
        nc *= 2;
    const XiFunc **nf = (const XiFunc **) xr_realloc(ctx->shared_funcs, (size_t) nc * sizeof(*nf));
    const XiClassData **ncl =
        (const XiClassData **) xr_realloc(ctx->shared_class, (size_t) nc * sizeof(*ncl));
    const XiEnumData **ne =
        (const XiEnumData **) xr_realloc(ctx->shared_enum, (size_t) nc * sizeof(*ne));
    CgSharedNativeInstance *ni = (CgSharedNativeInstance *) xr_realloc(ctx->shared_native_instances,
                                                                       (size_t) nc * sizeof(*ni));
    if (!nf || !ncl || !ne || !ni) {
        ctx->shared_funcs = nf ? nf : ctx->shared_funcs;
        ctx->shared_class = ncl ? ncl : ctx->shared_class;
        ctx->shared_enum = ne ? ne : ctx->shared_enum;
        ctx->shared_native_instances = ni ? ni : ctx->shared_native_instances;
        ctx->error = true;
        return false;
    }
    int added = nc - ctx->shared_cap;
    memset(&nf[ctx->shared_cap], 0, (size_t) added * sizeof(*nf));
    memset(&ncl[ctx->shared_cap], 0, (size_t) added * sizeof(*ncl));
    memset(&ne[ctx->shared_cap], 0, (size_t) added * sizeof(*ne));
    memset(&ni[ctx->shared_cap], 0, (size_t) added * sizeof(*ni));
    ctx->shared_funcs = nf;
    ctx->shared_class = ncl;
    ctx->shared_enum = ne;
    ctx->shared_native_instances = ni;
    ctx->shared_cap = nc;
    return true;
}

/* Grow the shared-native-export table to at least `need` entries (zeroed).
 * Separate from cg_reserve_shared: exports are keyed by (module, slot) and grow
 * independently of the func/class/enum slot tables. Returns false (and sets
 * ctx->error) on allocation failure. */
static bool cg_reserve_shared_native_exports(XiCgenCtx *ctx, int need) {
    if (need <= ctx->shared_native_exports_cap)
        return true;
    int nc = ctx->shared_native_exports_cap > 0 ? ctx->shared_native_exports_cap : CG_INIT_SHARED;
    while (nc < need)
        nc *= 2;
    CgSharedNativeExport *ne =
        (CgSharedNativeExport *) xr_realloc(ctx->shared_native_exports, (size_t) nc * sizeof(*ne));
    if (!ne) {
        ctx->error = true;
        return false;
    }
    memset(&ne[ctx->shared_native_exports_cap], 0,
           (size_t) (nc - ctx->shared_native_exports_cap) * sizeof(*ne));
    ctx->shared_native_exports = ne;
    ctx->shared_native_exports_cap = nc;
    return true;
}

/* Grow the method table to at least `need` entries (zeroed). */
static bool cg_reserve_methods(XiCgenCtx *ctx, int need) {
    if (need <= ctx->methods_cap)
        return true;
    int nc = ctx->methods_cap > 0 ? ctx->methods_cap : CG_INIT_METHODS;
    while (nc < need)
        nc *= 2;
    CgMethodEntry *nm = (CgMethodEntry *) xr_realloc(ctx->methods, (size_t) nc * sizeof(*nm));
    if (!nm) {
        ctx->error = true;
        return false;
    }
    memset(&nm[ctx->methods_cap], 0, (size_t) (nc - ctx->methods_cap) * sizeof(*nm));
    ctx->methods = nm;
    ctx->methods_cap = nc;
    return true;
}

/* Grow the import table to at least `need` entries (zeroed). */
static bool cg_reserve_imports(XiCgenCtx *ctx, int need) {
    if (need <= ctx->imports_cap)
        return true;
    int nc = ctx->imports_cap > 0 ? ctx->imports_cap : CG_INIT_IMPORTS;
    while (nc < need)
        nc *= 2;
    CgImportEntry *ni = (CgImportEntry *) xr_realloc(ctx->imports, (size_t) nc * sizeof(*ni));
    if (!ni) {
        ctx->error = true;
        return false;
    }
    memset(&ni[ctx->imports_cap], 0, (size_t) (nc - ctx->imports_cap) * sizeof(*ni));
    ctx->imports = ni;
    ctx->imports_cap = nc;
    return true;
}

/* Intern a literal; returns its stable id. */
static int cg_intern_str_lit(XiCgenCtx *ctx, const char *s) {
    if (!s)
        s = "";
    size_t len = strlen(s);
    uint32_t bucket = (uint32_t) xrt_hash_bytes(s, len) & (CG_STRLIT_BUCKETS - 1);
    for (CgStrLit *e = ctx->strlit_buckets[bucket]; e; e = e->next) {
        if (e->len == len && memcmp(e->str, s, len) == 0)
            return e->id;
    }
    CgStrLit *e = (CgStrLit *) xr_malloc(sizeof(CgStrLit));
    e->str = xr_strdup(s);
    e->len = len;
    e->id = ctx->nstrlit;
    e->next = ctx->strlit_buckets[bucket];
    ctx->strlit_buckets[bucket] = e;
    if (ctx->nstrlit >= ctx->strlit_cap) {
        int ncap = ctx->strlit_cap ? ctx->strlit_cap * 2 : 64;
        ctx->strlit_list = (CgStrLit **) xr_realloc(ctx->strlit_list, ncap * sizeof(CgStrLit *));
        ctx->strlit_cap = ncap;
    }
    ctx->strlit_list[ctx->nstrlit++] = e;
    return e->id;
}

/* Emit a string literal value expression: a pointer to the module-level
 * static xrt_str_t emitted by xi_cgen_emit_str_literal_defs. */
static void cg_emit_str_value(XiCgenCtx *ctx, FILE *out, const char *s) {
    fprintf(out, "xr_str_lit(&_xstr_%d)", cg_intern_str_lit(ctx, s));
}

static const XaotBundle *cg_ctx_aot_bundle(const XiCgenCtx *ctx) {
    return ctx ? ctx->aot_bundle : NULL;
}

static void cg_array_data_cache_decls_reset(XiCgenCtx *ctx) {
    if (ctx)
        ctx->narray_data_cache_decls = 0;
}

static bool cg_array_data_cache_decl_mark(XiCgenCtx *ctx, const XiValue *origin) {
    if (!ctx || !origin)
        return false;
    for (int i = 0; i < ctx->narray_data_cache_decls; i++) {
        if (ctx->array_data_cache_decls[i] == origin)
            return false;
    }
    if (ctx->narray_data_cache_decls >= ctx->array_data_cache_decl_cap) {
        int new_cap = ctx->array_data_cache_decl_cap ? ctx->array_data_cache_decl_cap * 2 : 16;
        const XiValue **new_decls = (const XiValue **) xr_realloc(
            ctx->array_data_cache_decls, sizeof(const XiValue *) * (size_t) new_cap);
        if (!new_decls) {
            ctx->error = true;
            return false;
        }
        ctx->array_data_cache_decls = new_decls;
        ctx->array_data_cache_decl_cap = new_cap;
    }
    ctx->array_data_cache_decls[ctx->narray_data_cache_decls++] = origin;
    return true;
}

#include "xi_cgen_ctx_impl.inc.c"
#include "xi_cgen_time_ctx_helpers.inc.c"

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
                if (idx < parent->nchildren) {
                    if (!cg_reserve_methods(ctx, ctx->nmethod + 1))
                        return;
                    ctx->methods[ctx->nmethod].class_name = cd->class_name;
                    ctx->methods[ctx->nmethod].name = m->name;
                    ctx->methods[ctx->nmethod].func = parent->children[idx];
                    ctx->methods[ctx->nmethod].class_data = cd;
                    ctx->methods[ctx->nmethod].instance_layout = cd->instance_layout;
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
    for (int s = 0; s < ctx->nshared && s < ctx->shared_cap; s++) {
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

/* Lookup a class instance method by name and receiver class.
 * Builtin receivers must never fall through to a class method with the
 * same source-level name.
 * If out_prefix is non-NULL, stores the method's module prefix (for
 * cross-module class methods; NULL means current module). */
static const XiFunc *cg_lookup_method(XiCgenCtx *ctx, const char *name, const char *class_name,
                                      const char **out_prefix) {
    if (!name || !class_name)
        return NULL;
    for (int i = 0; i < ctx->nmethod; i++) {
        if (!ctx->methods[i].name || strcmp(ctx->methods[i].name, name) != 0)
            continue;
        if (ctx->methods[i].class_name && strcmp(ctx->methods[i].class_name, class_name) == 0) {
            if (out_prefix)
                *out_prefix = ctx->methods[i].module_prefix;
            return ctx->methods[i].func;
        }
    }
    if (out_prefix)
        *out_prefix = NULL;
    return NULL;
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

/* Initialize ctx from XiModule metadata.  Reads shared-slot metadata
 * directly from the module struct — no IR block scanning required. */
static void cg_init_from_module(XiCgenCtx *ctx, XiModule *mod) {
    XR_DCHECK(ctx != NULL, "cg_init_from_module: NULL ctx");
    XR_DCHECK(mod != NULL, "cg_init_from_module: NULL module");
    XR_DCHECK(mod->init != NULL, "cg_init_from_module: NULL init func");

    /* Grow the shared-slot tables to fit this module (they grow on demand and
     * are reused across modules, keeping the largest allocation). */
    uint16_t need_slots = mod->init->nshared > mod->nslots ? mod->init->nshared : mod->nslots;
    if (!cg_reserve_shared(ctx, (int) need_slots))
        return;

    memset(ctx->shared_funcs, 0, (size_t) ctx->shared_cap * sizeof(*ctx->shared_funcs));
    memset(ctx->shared_class, 0, (size_t) ctx->shared_cap * sizeof(*ctx->shared_class));
    memset(ctx->shared_enum, 0, (size_t) ctx->shared_cap * sizeof(*ctx->shared_enum));
    memset(ctx->shared_native_instances, 0,
           (size_t) ctx->shared_cap * sizeof(*ctx->shared_native_instances));
    ctx->nshared = mod->init->nshared;
    ctx->nmethod = 0;
    ctx->module = mod;

    /* Copy slot mappings from module metadata (bounds enforced above). */
    uint16_t nslots = mod->nslots;
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
    if (mod->slot_enums) {
        for (uint16_t s = 0; s < nslots; s++)
            ctx->shared_enum[s] = mod->slot_enums[s];
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

typedef struct {
    const XiFunc *func;
    const char *prefix;
    bool is_class_constructor;
} CgStaticFunctionCall;

static bool cg_func_needs_aot_coro_ctx(XiCgenCtx *ctx, const XiFunc *f);

static CgStaticFunctionCall cg_no_static_function_call(void) {
    CgStaticFunctionCall call;
    call.func = NULL;
    call.prefix = NULL;
    call.is_class_constructor = false;
    return call;
}

static CgStaticFunctionCall cg_static_function_call(const XiFunc *func, const char *prefix) {
    CgStaticFunctionCall call;
    call.func = func;
    call.prefix = prefix;
    call.is_class_constructor = false;
    return call;
}

static CgStaticFunctionCall cg_static_class_constructor_call(const XiFunc *func,
                                                             const char *prefix) {
    CgStaticFunctionCall call;
    call.func = func;
    call.prefix = prefix;
    call.is_class_constructor = true;
    return call;
}

static bool cg_func_tree_contains(const XiFunc *root, const XiFunc *target) {
    if (!root || !target)
        return false;
    if (root == target)
        return true;
    for (uint16_t i = 0; i < root->nchildren; i++) {
        if (cg_func_tree_contains(root->children[i], target))
            return true;
    }
    return false;
}

static const char *cg_module_prefix_for_func(const XiCgenCtx *ctx, const XiFunc *target) {
    if (!ctx || !target || !ctx->all_modules || ctx->all_nmodules <= 0)
        return NULL;
    for (int i = 0; i < ctx->all_nmodules; i++) {
        const XiModule *mod = ctx->all_modules[i];
        if (!mod || !mod->init)
            continue;
        if (cg_func_tree_contains(mod->init, target))
            return mod->name;
    }
    return NULL;
}

#include "xi_cgen_call_resolve.inc.c"

static bool cg_shared_static_function_ownership_is_noop(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *v) {
    if (!ctx || !v || (v->op != XI_RETAIN && v->op != XI_RELEASE) || v->nargs < 1)
        return false;
    const XiValue *arg = cg_unwrap_identity_value(v->args[0]);
    if (!arg || arg->op != XI_GET_SHARED)
        return false;
    if (arg->type && XR_TYPE_IS_FUNCTION(arg->type))
        return true;
    CgStaticFunctionCall call = cg_resolve_static_function_call(ctx, current, arg);
    return call.func && !call.is_class_constructor && call.func->ncaptures == 0;
}

static const XiFunc *cg_shared_function_slot_target(XiCgenCtx *ctx, const XiFunc *current,
                                                    int slot) {
    if (slot < 0)
        return NULL;
    if (current && current->shared_slot_funcs && slot < (int) current->shared_slot_func_count)
        return current->shared_slot_funcs[slot];
    if (ctx && ctx->module && ctx->module->slot_funcs && slot < (int) ctx->module->nslots)
        return ctx->module->slot_funcs[slot];
    return NULL;
}

static bool cg_shared_function_target_can_elide(XiCgenCtx *ctx, const XiFunc *target) {
    return target && target->ncaptures == 0 && !cg_func_needs_aot_coro_ctx(ctx, target);
}

static bool cg_shared_static_function_value_uses_are_direct(XiCgenCtx *ctx, const XiFunc *owner,
                                                            const XiValue *value,
                                                            const XiFunc *target, int depth) {
    if (!ctx || !owner || !value || !target || depth > 8)
        return false;

    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != value)
                    continue;
                switch ((XiOp) user->op) {
                    case XI_CALL: {
                        if (ai != 0)
                            return false;
                        CgStaticFunctionCall call =
                            cg_resolve_static_function_call(ctx, owner, user->args[0]);
                        if (call.func != target || call.is_class_constructor)
                            return false;
                        break;
                    }
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_COPY:
                    case XI_MOVE:
                        if (ai != 0 || !cg_shared_static_function_value_uses_are_direct(
                                           ctx, owner, user, target, depth + 1))
                            return false;
                        break;
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (ai != 0)
                            return false;
                        break;
                    default:
                        return false;
                }
            }
        }
    }
    return true;
}

static bool cg_shared_static_function_slot_uses_are_direct(XiCgenCtx *ctx, const XiFunc *owner,
                                                           int slot, const XiFunc *target) {
    if (!ctx || !owner || slot < 0 || !target)
        return false;
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_GET_SHARED || (int) v->aux_int != slot)
                continue;
            if (!cg_shared_static_function_value_uses_are_direct(ctx, owner, v, target, 0))
                return false;
        }
    }
    for (uint16_t ci = 0; ci < owner->nchildren; ci++) {
        if (!cg_shared_static_function_slot_uses_are_direct(ctx, owner->children[ci], slot, target))
            return false;
    }
    return true;
}

static bool cg_shared_static_function_slot_can_elide(XiCgenCtx *ctx, const XiFunc *current,
                                                     int slot, const XiFunc *target) {
    if (!ctx || !ctx->module || !ctx->module->init || slot < 0 || !target)
        return false;
    if (ctx->all_nmodules > 1)
        return false;
    if (cg_shared_function_slot_target(ctx, current, slot) != target)
        return false;
    if (!cg_shared_function_target_can_elide(ctx, target))
        return false;
    return cg_shared_static_function_slot_uses_are_direct(ctx, ctx->module->init, slot, target);
}

static bool cg_shared_static_function_set_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                                    const XiValue *v) {
    if (!ctx || !v || v->op != XI_SET_SHARED || v->nargs < 1)
        return false;
    const XiValue *arg = cg_unwrap_identity_value(v->args[0]);
    if (!arg ||
        (arg->op != XI_CLOSURE_NEW &&
         !(arg->op == XI_STACK_ALLOC && arg->aux_int == XI_CLOSURE_NEW)) ||
        !arg->aux)
        return false;
    return cg_shared_static_function_slot_can_elide(ctx, current, (int) v->aux_int,
                                                    (const XiFunc *) arg->aux);
}

static bool cg_shared_static_function_closure_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *v) {
    if (!ctx || !current || !v ||
        (v->op != XI_CLOSURE_NEW && !(v->op == XI_STACK_ALLOC && v->aux_int == XI_CLOSURE_NEW)) ||
        !v->aux)
        return false;
    const XiFunc *target = (const XiFunc *) v->aux;
    bool saw_store = false;
    for (uint32_t bi = 0; bi < current->nblocks; bi++) {
        const XiBlock *blk = current->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != v)
                    continue;
                if (user->op == XI_SET_SHARED && ai == 0 &&
                    cg_shared_static_function_slot_can_elide(ctx, current, (int) user->aux_int,
                                                             target)) {
                    saw_store = true;
                    continue;
                }
                if ((user->op == XI_RETAIN || user->op == XI_RELEASE) && ai == 0)
                    continue;
                return false;
            }
        }
    }
    return saw_store;
}

static bool cg_shared_static_function_get_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                                    const XiValue *v) {
    if (!ctx || !v || v->op != XI_GET_SHARED)
        return false;
    int slot = (int) v->aux_int;
    const XiFunc *target = cg_shared_function_slot_target(ctx, current, slot);
    return cg_shared_static_function_slot_can_elide(ctx, current, slot, target);
}

static bool cg_shared_static_function_value_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                                      const XiValue *v) {
    return cg_shared_static_function_get_is_elided(ctx, current, v) ||
           cg_shared_static_function_set_is_elided(ctx, current, v) ||
           cg_shared_static_function_closure_is_elided(ctx, current, v);
}

/* Write a value reference: v<id> or phi<id> for phi nodes */
static void emit_vref(FILE *out, const XiValue *v) {
    if (v->op == XI_PHI)
        fprintf(out, "phi%u", v->id);
    else
        fprintf(out, "v%u", v->id);
}

#include "xi_cgen_class_native_meta.inc.c"
static void emit_codegen_abort_expr(FILE *out);
#include "xi_cgen_abi_helpers.inc.c"
#include "xi_cgen_value_helpers.inc.c"
#include "xi_cgen_method_symbols.inc.c"
static bool cg_array_same_value(const XiValue *a, const XiValue *b);
static bool cg_array_value_known_nonnegative(const XiValue *v, const XiValue *root, uint8_t depth);
static bool cg_array_block_has_no_side_effect_after(const XiBlock *blk, const XiValue *start);
static bool cg_array_block_has_no_side_effect_before(const XiBlock *blk, const XiValue *target);
static bool cg_array_index_access_bounds_proven(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);
static void emit_condition_expr(FILE *out, const XiValue *v);
static void emit_condition_expr_ctx(XiCgenCtx *ctx, FILE *out, const XiValue *v);

static const char *cg_current_source_path(const XiCgenCtx *ctx) {
    if (ctx && ctx->module && ctx->module->path && ctx->module->path[0])
        return ctx->module->path;
    if (ctx && ctx->module && ctx->module->name && ctx->module->name[0])
        return ctx->module->name;
    return "<xray>";
}

static void emit_source_line_directive(XiCgenCtx *ctx, FILE *out, uint32_t line) {
    if (!out || line == 0)
        return;
    fprintf(out, "#line %u ", line);
    emit_c_string_literal(out, cg_current_source_path(ctx));
    fprintf(out, "\n");
}

static void emit_value_source_line(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (v)
        emit_source_line_directive(ctx, out, v->line);
}

#include "xi_cgen_struct_helpers.inc.c"
#include "xi_cgen_class_helpers.inc.c"
static bool cg_has_exception_handling(const XiFunc *f);
#include "xi_cgen_class_native_helpers.inc.c"
#include "xi_cgen_array_helpers.inc.c"

static bool cg_class_native_ref_stack_return_consumes_ctor(XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *ctor_call);

static bool cg_class_descriptor_slot_can_elide_depth(XiCgenCtx *ctx, const XiFunc *current,
                                                     int slot, const XiClassData *cd, int depth);
static bool cg_class_descriptor_create_is_elided_depth(XiCgenCtx *ctx, const XiFunc *current,
                                                       const XiValue *v, int depth);

static const XiClassData *cg_class_descriptor_slot_data(XiCgenCtx *ctx, int slot) {
    if (!ctx || slot < 0)
        return NULL;
    if (ctx->module && ctx->module->slot_classes && slot < (int) ctx->module->nslots)
        return ctx->module->slot_classes[slot];
    if (slot < ctx->shared_cap)
        return ctx->shared_class[slot];
    return NULL;
}

static int cg_class_descriptor_slot_for_data(XiCgenCtx *ctx, const XiClassData *cd) {
    if (!ctx || !ctx->module || !cd)
        return -1;
    int slot = cg_class_native_slot_in_module(ctx->module, cd);
    if (slot < 0)
        return -1;
    const XiClassData *slot_cd = cg_class_descriptor_slot_data(ctx, slot);
    return cg_class_native_data_matches(slot_cd, cd) ? slot : -1;
}

static bool cg_class_descriptor_native_stack_only_data(const XiClassData *cd) {
    return cd && cd->instance_layout && !cd->is_monomorphized &&
           !cg_class_native_layout_has_ref_fields(cd->instance_layout);
}

static bool cg_class_descriptor_elidable_native_data(const XiClassData *cd) {
    return cd && cd->instance_layout && !cd->is_monomorphized;
}

static bool cg_class_descriptor_ctor_call_is_elidable(XiCgenCtx *ctx, const XiFunc *owner,
                                                      const XiValue *call, const XiClassData *cd) {
    if (!ctx || !owner || !call || !cd || call->op != XI_CALL || call->nargs < 1)
        return false;
    const XiFunc *ctor = NULL;
    const XiClassData *call_cd = cg_class_native_ctor_call_data(ctx, owner, call, &ctor, NULL);
    if (!ctor || !cg_class_native_data_matches(call_cd, cd) ||
        !cg_class_descriptor_elidable_native_data(call_cd))
        return false;
    if (cg_class_descriptor_native_stack_only_data(call_cd))
        return cg_class_native_ctor_can_inline(ctx, owner, call);
    return cg_class_native_layout_has_ref_fields(call_cd->instance_layout) &&
           cg_class_native_ref_stack_return_consumes_ctor(ctx, owner, call);
}

static bool cg_class_descriptor_value_uses_are_elidable(XiCgenCtx *ctx, const XiFunc *owner,
                                                        const XiValue *value, const XiClassData *cd,
                                                        int depth, bool *saw_elidable_use) {
    if (!ctx || !owner || !value || !cd || depth > 8)
        return false;

    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == value)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] == value)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == value)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != value)
                    continue;
                switch ((XiOp) user->op) {
                    case XI_CALL:
                        if (ai != 0 ||
                            !cg_class_descriptor_ctor_call_is_elidable(ctx, owner, user, cd))
                            return false;
                        if (saw_elidable_use)
                            *saw_elidable_use = true;
                        break;
                    case XI_CLASS_CREATE:
                        if (ai != 0 || !cg_class_descriptor_create_is_elided_depth(ctx, owner, user,
                                                                                   depth + 1))
                            return false;
                        if (saw_elidable_use)
                            *saw_elidable_use = true;
                        break;
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_COPY:
                    case XI_MOVE:
                        if (ai != 0 || !cg_class_descriptor_value_uses_are_elidable(
                                           ctx, owner, user, cd, depth + 1, saw_elidable_use))
                            return false;
                        break;
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (ai != 0)
                            return false;
                        break;
                    default:
                        return false;
                }
            }
        }
    }
    return true;
}

static bool cg_class_descriptor_slot_uses_are_elidable(XiCgenCtx *ctx, const XiFunc *owner,
                                                       int slot, const XiClassData *cd, int depth,
                                                       bool *saw_elidable_use) {
    if (!ctx || !owner || slot < 0 || !cd || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_GET_SHARED || (int) v->aux_int != slot)
                continue;
            if (!cg_class_descriptor_value_uses_are_elidable(ctx, owner, v, cd, depth,
                                                             saw_elidable_use))
                return false;
        }
    }
    for (uint16_t ci = 0; ci < owner->nchildren; ci++) {
        if (!cg_class_descriptor_slot_uses_are_elidable(ctx, owner->children[ci], slot, cd,
                                                        depth + 1, saw_elidable_use))
            return false;
    }
    return true;
}

static bool cg_class_descriptor_slot_can_elide_depth(XiCgenCtx *ctx, const XiFunc *current,
                                                     int slot, const XiClassData *cd, int depth) {
    (void) current;
    if (!ctx || !ctx->module || !ctx->module->init || ctx->all_nmodules > 1 || slot < 0 ||
        depth > 8)
        return false;
    const XiClassData *slot_cd = cg_class_descriptor_slot_data(ctx, slot);
    if (!cg_class_native_data_matches(slot_cd, cd) ||
        !cg_class_descriptor_elidable_native_data(slot_cd))
        return false;
    bool saw_elidable_use = false;
    return cg_class_descriptor_slot_uses_are_elidable(ctx, ctx->module->init, slot, slot_cd,
                                                      depth + 1, &saw_elidable_use) &&
           saw_elidable_use;
}

static bool cg_class_descriptor_slot_can_elide(XiCgenCtx *ctx, const XiFunc *current, int slot,
                                               const XiClassData *cd) {
    return cg_class_descriptor_slot_can_elide_depth(ctx, current, slot, cd, 0);
}

static bool cg_class_descriptor_set_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                              const XiValue *v) {
    if (!ctx || !v || v->op != XI_SET_SHARED || v->nargs < 1)
        return false;
    const XiValue *arg = cg_unwrap_identity_value(v->args[0]);
    if (!arg || arg->op != XI_CLASS_CREATE || !arg->aux)
        return false;
    return cg_class_descriptor_slot_can_elide(ctx, current, (int) v->aux_int,
                                              (const XiClassData *) arg->aux);
}

static bool cg_class_descriptor_create_is_elided_depth(XiCgenCtx *ctx, const XiFunc *current,
                                                       const XiValue *v, int depth) {
    if (!ctx || !current || !v || v->op != XI_CLASS_CREATE || !v->aux || depth > 8)
        return false;
    const XiClassData *cd = (const XiClassData *) v->aux;
    int slot = cg_class_descriptor_slot_for_data(ctx, cd);
    if (!cg_class_descriptor_slot_can_elide_depth(ctx, current, slot, cd, depth + 1))
        return false;

    bool saw_store = false;
    for (uint32_t bi = 0; bi < current->nblocks; bi++) {
        const XiBlock *blk = current->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == v)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != v)
                    continue;
                if (user->op == XI_SET_SHARED && ai == 0 && (int) user->aux_int == slot) {
                    saw_store = true;
                    continue;
                }
                if ((user->op == XI_RETAIN || user->op == XI_RELEASE) && ai == 0)
                    continue;
                return false;
            }
        }
    }
    return saw_store;
}

static bool cg_class_descriptor_create_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                                 const XiValue *v) {
    return cg_class_descriptor_create_is_elided_depth(ctx, current, v, 0);
}

static bool cg_class_descriptor_get_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                              const XiValue *v) {
    if (!ctx || !v || v->op != XI_GET_SHARED)
        return false;
    int slot = (int) v->aux_int;
    const XiClassData *cd = cg_class_descriptor_slot_data(ctx, slot);
    return cg_class_descriptor_slot_can_elide(ctx, current, slot, cd);
}

static bool cg_class_descriptor_ownership_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                                    const XiValue *v) {
    if (!ctx || !v || (v->op != XI_RETAIN && v->op != XI_RELEASE) || v->nargs < 1)
        return false;
    const XiValue *arg = cg_unwrap_identity_value(v->args[0]);
    if (!arg)
        return false;
    if (arg->op == XI_GET_SHARED)
        return cg_class_descriptor_get_is_elided(ctx, current, arg);
    if (arg->op == XI_CLASS_CREATE)
        return cg_class_descriptor_create_is_elided(ctx, current, arg);
    return false;
}

static bool cg_class_descriptor_value_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                                const XiValue *v) {
    return cg_class_descriptor_get_is_elided(ctx, current, v) ||
           cg_class_descriptor_set_is_elided(ctx, current, v) ||
           cg_class_descriptor_create_is_elided(ctx, current, v) ||
           cg_class_descriptor_ownership_is_elided(ctx, current, v);
}

static const char *local_ctype_str_ctx(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (cg_array_value_uses_native_local(ctx, f, v))
        return "xrt_array_t *";
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    if (plan && plan->rep.c_type)
        return plan->rep.c_type;
    return local_ctype_str(v);
}

/* Storage representation of v's declared C local. Must stay in sync with
 * local_ctype_str_ctx: a native-local array is emitted as xrt_array_t* (PTR),
 * everything else uses its planned storage rep. Identity ops (XI_COPY/XI_MOVE)
 * use this to bridge a source whose declared rep differs from the result's,
 * e.g. a native-local PTR array moved into a TAGGED-declared local. */
static XrRep cg_value_decl_storage_rep(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (cg_array_value_uses_native_local(ctx, f, v))
        return XR_REP_PTR;
    return cg_value_plan_storage_rep(ctx, v);
}

/* Write a phi variable reference: phi<id> */
static void emit_phi_ref(FILE *out, const XiPhi *phi) {
    fprintf(out, "phi%u", phi->value.id);
}

/* ========== Value Emission ========== */

static void emit_binop(FILE *out, const XiValue *v, const char *op) {
    emit_vref(out, v->args[0]);
    fprintf(out, " %s ", op);
    emit_vref(out, v->args[1]);
}

static void emit_bitwise_binop_ctx(XiCgenCtx *ctx, FILE *out, const XiValue *v, const char *op) {
    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "XR_FROM_INT(");
    fprintf(out, "(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ") %s (", op);
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, ")");
    if (boxed)
        fprintf(out, ")");
}

static void emit_bitwise_unop_ctx(XiCgenCtx *ctx, FILE *out, const XiValue *v, const char *op) {
    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "XR_FROM_INT(");
    fprintf(out, "%s(", op);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ")");
    if (boxed)
        fprintf(out, ")");
}

/* Shifts cannot use raw C << / >>: out-of-range counts and shifting into the
 * sign bit are UB. Route through xrt_i64_shl / xrt_i64_shr, which implement
 * the language's count-mod-64 semantics (see xrt_arith.h). */
static void emit_shift_binop_ctx(XiCgenCtx *ctx, FILE *out, const XiValue *v, const char *fn) {
    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "XR_FROM_INT(");
    fprintf(out, "%s(", fn);
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ", ");
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, ")");
    if (boxed)
        fprintf(out, ")");
}

#include "xi_cgen_arith_helpers.inc.c"

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

static void emit_condition_expr_ctx(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED) {
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

#include "xi_cgen_array_builtin_helpers.inc.c"

#include "xi_cgen_dispatch_helpers.inc.c"

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

    if (xi_to_c_emit_generated(ctx, out, f, v, prefix))
        return;

    fprintf(stderr, "[xi_cgen] ERROR: unsupported Xi op %s (%d)\n", xi_op_name(v->op), v->op);
    emit_codegen_abort_expr(out);
    ctx->error = true;
}

typedef struct CgClassNativeRefStackReturn {
    const XiValue *ctor_call;
    const XiValue *return_call;
    const XiClassData *class_data;
    const XiFunc *ctor;
    const char *ctor_prefix;
} CgClassNativeRefStackReturn;

static bool cg_func_has_defer_stmt(const XiFunc *f) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && v->op == XI_DEFER)
                return true;
        }
    }
    return false;
}

static bool cg_class_native_ref_stack_ctor_uses_only_return_call(XiCgenCtx *ctx, const XiFunc *f,
                                                                 const XiValue *target,
                                                                 const XiValue *return_call,
                                                                 uint8_t depth,
                                                                 bool *saw_return_call) {
    (void) ctx;
    if (!f || !target || !return_call || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == target)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != target)
                    continue;
                if (user == return_call && ai == 0) {
                    if (saw_return_call)
                        *saw_return_call = true;
                    continue;
                }
                if ((user->op == XI_COPY || user->op == XI_MOVE) && ai == 0) {
                    if (!cg_class_native_ref_stack_ctor_uses_only_return_call(
                            ctx, f, user, return_call, (uint8_t) (depth + 1), saw_return_call))
                        return false;
                    continue;
                }
                if ((user->op == XI_RETAIN || user->op == XI_RELEASE) && ai == 0)
                    continue;
                return false;
            }
        }
    }
    return true;
}

static bool cg_class_native_ref_stack_return_info(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *return_call,
                                                  CgClassNativeRefStackReturn *out_info) {
    if (out_info)
        memset(out_info, 0, sizeof(*out_info));
    if (!ctx || !f || !return_call ||
        (return_call->op != XI_CALL_METHOD && return_call->op != XI_CALL_METHOD_DIRECT) ||
        return_call->nargs < 1 || !return_call->block ||
        return_call->block->kind != XI_BLOCK_RETURN || return_call->block->control != return_call ||
        cg_has_exception_handling(f) || cg_func_has_defer_stmt(f))
        return false;

    const XiValue *ctor_call = cg_class_native_trace_ctor_origin(ctx, f, return_call->args[0], 0);
    if (!ctor_call || ctor_call->block != return_call->block)
        return false;
    const XiFunc *ctor = NULL;
    const char *ctor_prefix = NULL;
    const XiClassData *cd = cg_class_native_ctor_call_data(ctx, f, ctor_call, &ctor, &ctor_prefix);
    if (!cd || !ctor || !cd->instance_layout ||
        !cg_class_native_layout_has_ref_fields(cd->instance_layout) ||
        !cg_class_native_ctor_can_inline(ctx, f, ctor_call))
        return false;

    const char *method_prefix = NULL;
    const XiFunc *method = cg_class_native_resolve_method_call(ctx, f, return_call, &method_prefix);
    (void) method_prefix;
    if (!method || !cg_class_func_uses_native_receiver(ctx, method) ||
        !cg_class_native_call_is_nothrow_direct(ctx, f, return_call))
        return false;
    CgClassNativeFunc method_info = cg_class_native_func(ctx, method);
    if (!cg_class_native_can_pass_instance_as(ctx, cd, method_info.class_data))
        return false;

    bool saw_return_call = false;
    if (!cg_class_native_ref_stack_ctor_uses_only_return_call(ctx, f, ctor_call, return_call, 0,
                                                              &saw_return_call) ||
        !saw_return_call)
        return false;
    if (out_info) {
        out_info->ctor_call = ctor_call;
        out_info->return_call = return_call;
        out_info->class_data = cd;
        out_info->ctor = ctor;
        out_info->ctor_prefix = ctor_prefix;
    }
    return true;
}

static bool cg_class_native_ref_stack_return_consumes_ctor(XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *ctor_call) {
    if (!ctx || !f || !ctor_call || ctor_call->op != XI_CALL)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk || blk->kind != XI_BLOCK_RETURN || !blk->control)
            continue;
        CgClassNativeRefStackReturn info;
        if (cg_class_native_ref_stack_return_info(ctx, f, blk->control, &info) &&
            info.ctor_call == ctor_call)
            return true;
    }
    return false;
}

static bool cg_class_native_ref_stack_return_takes_value(XiCgenCtx *ctx, const XiFunc *f,
                                                         const XiValue *v) {
    if (!ctx || !f || !v)
        return false;
    if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) &&
        cg_class_native_ref_stack_return_info(ctx, f, v, NULL))
        return true;
    return v->op == XI_CALL && cg_class_native_ref_stack_return_consumes_ctor(ctx, f, v);
}

static bool emit_class_native_ref_stack_return_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                    const XiBlock *blk, const char *prefix) {
    CgClassNativeRefStackReturn info;
    if (!blk || !cg_class_native_ref_stack_return_info(ctx, f, blk->control, &info))
        return false;
    const char *class_prefix = info.ctor_prefix ? info.ctor_prefix : prefix;
    fprintf(out, "    ");
    emit_class_native_type_name(out, class_prefix, info.class_data->class_name);
    fprintf(out, " _ci%u;\n", info.ctor_call->id);
    fprintf(out, "    memset(&_ci%u, 0, sizeof(_ci%u));\n", info.ctor_call->id, info.ctor_call->id);
    fprintf(out, "    ");
    emit_class_native_type_name(out, class_prefix, info.class_data->class_name);
    fprintf(out, " *");
    emit_vref(out, info.ctor_call);
    fprintf(out, " = &_ci%u;\n", info.ctor_call->id);
    fprintf(out, "    (void)");
    emit_fname(ctx, out, class_prefix, info.ctor);
    fprintf(out, "(NULL, ");
    emit_vref(out, info.ctor_call);
    for (uint16_t i = 1; i < info.ctor_call->nargs; i++) {
        fprintf(out, ", ");
        emit_value_as_rep(out, info.ctor_call->args[i], cg_func_param_abi_rep(ctx, info.ctor, i));
    }
    fprintf(out, ");\n");

    XrRep ret_rep = cg_func_return_abi_rep(ctx, f);
    fprintf(out, "    %s _ret%u = ", ctype_str(ret_rep), info.return_call->id);
    const char *conv_suffix = emit_conversion_prefix(
        out, info.return_call->type, cg_value_plan_storage_rep(ctx, info.return_call), ret_rep);
    emit_value_rhs(ctx, out, f, info.return_call, prefix);
    emit_conversion_suffix(out, conv_suffix);
    fprintf(out, ";\n");
    if (cg_class_native_layout_has_arc_ref_fields(info.class_data->instance_layout)) {
        fprintf(out, "    ");
        emit_class_native_dtor_name(out, class_prefix, info.class_data);
        fprintf(out, "(&_ci%u);\n", info.ctor_call->id);
    }
    fprintf(out, "    return _ret%u;\n", info.return_call->id);
    return true;
}

static void emit_deferred_calls(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
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
            const XiValue *callee = cg_unwrap_identity_value(dv->args[0]);
            if (callee &&
                (callee->op == XI_CLOSURE_NEW ||
                 (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW)) &&
                callee->aux)
                deferred_vals[ndeferred++] = callee;
        }
    }
    for (int di = ndeferred - 1; di >= 0; di--) {
        const XiValue *cv = deferred_vals[di];
        const XiFunc *cf = (const XiFunc *) cv->aux;
        fprintf(out, "    ");
        if (cg_func_uses_typed_abi(ctx, cf))
            emit_typed_abi_fname(ctx, out, prefix, cf);
        else
            emit_fname(ctx, out, prefix, cf);
        if (cf->ncaptures > 0) {
            fprintf(out, "((xrt_closure_t*)");
            emit_vref(out, cv);
            fprintf(out, ".ptr);\n");
        } else {
            fprintf(out, "(NULL);\n");
        }
    }
}

static void emit_default_return_for_abi(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    XrRep ret_rep = cg_func_return_abi_rep(ctx, f);
    if (ret_rep == XR_REP_TAGGED)
        fprintf(out, "XR_NULL_VAL");
    else
        fprintf(out, "0");
}

#include "xi_cgen_stmt_dispatch_helpers.inc.c"

/* Emit a complete value statement: type vN = <rhs>; */
static void emit_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    XR_DCHECK(v != NULL, "emit_value_stmt: NULL value");
    emit_value_source_line(ctx, out, v);

    /* Inlined struct: emit local anonymous C struct with native fields. */
    if (v->op == XI_STRUCT_NEW && cg_struct_can_inline(f, v)) {
        XrStructLayout *sl = (XrStructLayout *) v->aux;
        XR_DCHECK(sl != NULL, "inlined XI_STRUCT_NEW: missing layout");
        fprintf(out, "    struct { ");
        for (uint16_t i = 0; i < sl->field_count; i++) {
            char fname[32];
            snprintf(fname, sizeof(fname), "f%u", i);
            emit_struct_field_decl(out, sl, i, fname, prefix);
            fprintf(out, "; ");
        }
        fprintf(out, "} _st%u = {0};\n", v->id);
        return;
    }

    if (cg_value_is_elided_nested_struct_ref(f, v) || cg_value_is_elided_fixed_array_ref(f, v))
        return;
    if ((v->op == XI_COPY || v->op == XI_MOVE) && (cg_value_traces_to_inlined_struct(f, v) ||
                                                   cg_value_is_elided_heap_struct_alias(ctx, f, v)))
        return;

    if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1 &&
        (cg_value_traces_to_inlined_struct(f, v->args[0]) ||
         cg_value_is_elided_heap_struct_alias(ctx, f, v) ||
         cg_value_is_elided_nested_struct_ref(f, v->args[0]) ||
         cg_value_is_elided_fixed_array_ref(f, v->args[0])))
        return;
    if (cg_ownership_op_is_noop(v) || cg_shared_static_function_ownership_is_noop(ctx, f, v))
        return;
    if (cg_shared_static_function_value_is_elided(ctx, f, v) ||
        cg_class_descriptor_value_is_elided(ctx, f, v))
        return;
    if (xicgen_box_only_feeds_native_int_print(ctx, f, v))
        return;

    if (cg_class_native_value_stmt_is_elided(ctx, f, v))
        return;

    if (emit_class_shared_native_ctor_value_stmt(ctx, out, f, prefix, v))
        return;

    if (cg_class_shared_native_set_is_elided(ctx, f, v) ||
        cg_class_shared_native_value_is_elided(ctx, f, v))
        return;

    if (cg_array_class_field_alloc_value_is_elided(ctx, f, v))
        return;

    if (cg_array_class_field_value_is_elided(ctx, f, v)) {
        emit_typed_array_data_cache_decl(ctx, out, v);
        return;
    }
    if (cg_class_native_map_field_value_is_elided(ctx, f, v))
        return;
    if (cg_class_native_set_field_value_is_elided(ctx, f, v))
        return;

    if (emit_class_native_map_method_call_stmt(ctx, out, f, v))
        return;
    if (emit_class_native_set_method_call_stmt(ctx, out, f, v))
        return;
    if (cg_class_native_ref_stack_return_takes_value(ctx, f, v))
        return;

    if (cg_array_typed_push_value_is_elided(ctx, f, v)) {
        fprintf(out, "    ");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
        return;
    }

    if (emit_class_native_ctor_value_stmt(ctx, out, f, prefix, v))
        return;

    if ((v->op == XI_GET_SHARED && cg_value_only_used_by_inlined_struct_new(f, v)) ||
        cg_value_is_elided_heap_struct_alias(ctx, f, v))
        return;

    if (cg_array_closure_value_only_used_by_inline_map(ctx, f, prefix, v) ||
        cg_value_is_dead_aot_marker(ctx, f, v))
        return;

    if (emit_typed_array_class_field_alloc_store_stmt(ctx, out, f, v))
        return;

    if (xi_to_c_emit_stmt_generated(ctx, out, f, v, prefix))
        return;

    bool void_like = cg_is_void_like(v);

    if (void_like) {
        fprintf(out, "    ");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
        return;
    }

    if (emit_typed_array_map_inline_stmt(ctx, out, f, prefix, v) ||
        emit_typed_array_filter_inline_stmt(ctx, out, f, prefix, v))
        return;
    bool cell_origin = cg_value_is_cell_origin(ctx, v);
    bool cell_update = cg_value_has_cell(ctx, v) && !cell_origin;

    if (ctx->pre_decl_all) {
        /* Variable already declared at function top — emit assignment */
        fprintf(out, "    ");
        emit_vref(out, v);
        fprintf(out, " = ");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
    } else {
        fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, v));
        emit_vref(out, v);
        fprintf(out, " = ");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
    }
    emit_typed_array_data_cache_decl(ctx, out, v);
    if (cell_origin) {
        fprintf(out, "    ");
        emit_cell_ref(out, v->var_id);
        fprintf(out, " = xrt_cell_new(");
        emit_boxed_value_ref(out, v);
        fprintf(out, ");\n");
    } else if (cell_update) {
        fprintf(out, "    xrt_cell_set(");
        emit_cell_ref(out, v->var_id);
        fprintf(out, ", ");
        emit_boxed_value_ref(out, v);
        fprintf(out, ");\n");
    }
}

/* ========== PHI Elimination ========== */

/* Emit phi assignments for all phis in `target` whose predecessor
 * at index `pred_idx` is `pred_blk`. Called before the jump/branch. */
static void emit_phi_copies(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiBlock *target,
                            uint16_t pred_idx) {
    for (const XiPhi *phi = target->phis; phi; phi = phi->next) {
        if (cg_value_traces_to_inlined_struct(f, &phi->value))
            continue;
        if (!cg_func_needs_aot_coro_ctx(ctx, f) &&
            cg_value_is_elided_heap_struct_alias(ctx, f, &phi->value))
            continue;
        if (pred_idx < phi->value.nargs && phi->value.args[pred_idx]) {
            fprintf(out, "    ");
            emit_phi_ref(out, phi);
            fprintf(out, " = ");
            emit_value_as_rep_ctx(ctx, out, phi->value.args[pred_idx],
                                  cg_value_plan_storage_rep(ctx, &phi->value));
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

#include "xi_cgen_loop_helpers.inc.c"

/* ========== Block Emission ========== */

static void emit_block(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiBlock *blk,
                       const char *prefix) {
    XR_DCHECK(blk != NULL, "emit_block: NULL block");

    /* Label (skip for entry block b0 to reduce clutter) */
    if (blk->id != 0)
        fprintf(out, "L%u:;\n", blk->id);
    emit_typed_array_final_len_stores(ctx, out, f, blk);

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
            if (blk->control && blk->control->op == XI_ERR_RETURN)
                break;
            emit_value_source_line(ctx, out, blk->control);
            emit_class_field_cache_flush(ctx, out);
            emit_deferred_calls(ctx, out, f, prefix);
            if (emit_class_native_ref_stack_return_stmt(ctx, out, f, blk, prefix))
                break;
            if (emit_class_native_return_stmt(ctx, out, f, blk))
                break;
            if (blk->control) {
                fprintf(out, "    return ");
                emit_value_as_rep_ctx(ctx, out, blk->control, cg_func_return_abi_rep(ctx, f));
                fprintf(out, ";\n");
            } else {
                if (cg_func_return_abi_rep(ctx, f) == XR_REP_TAGGED)
                    fprintf(out, "    return XR_NULL_VAL;\n");
                else
                    fprintf(out, "    return 0;\n");
            }
            break;
        }

        case XI_BLOCK_PLAIN:
            if (blk->succs[0]) {
                if (emit_structured_counted_loop_stmt(ctx, out, f, blk, prefix))
                    break;
                emit_phi_copies(ctx, out, f, blk->succs[0], find_pred_idx(blk->succs[0], blk));
                fprintf(out, "    goto L%u;\n", blk->succs[0]->id);
            }
            break;

        case XI_BLOCK_IF:
            XR_DCHECK(blk->control != NULL, "IF block missing control");
            XR_DCHECK(blk->succs[0] != NULL, "IF block missing then");
            XR_DCHECK(blk->succs[1] != NULL, "IF block missing else");
            emit_value_source_line(ctx, out, blk->control);
            if (emit_structured_array_fill_loop_stmt(ctx, out, f, blk, prefix))
                break;
            if (emit_bool_accumulate_diamond_stmt(ctx, out, blk))
                break;
            /* Emit phi copies for both branches */
            fprintf(out, "    if (");
            emit_condition_expr_ctx(ctx, out, blk->control);
            fprintf(out, ") {\n");
            emit_phi_copies(ctx, out, f, blk->succs[0], find_pred_idx(blk->succs[0], blk));
            fprintf(out, "        goto L%u;\n", blk->succs[0]->id);
            fprintf(out, "    } else {\n");
            emit_phi_copies(ctx, out, f, blk->succs[1], find_pred_idx(blk->succs[1], blk));
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

#include "xi_cgen_coro.inc.c"

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

/* Whether an SSA value is skipped when pre-declaring all values at function
 * top (the exception-handling path).  A value is skipped here exactly when
 * emit_value_stmt does NOT introduce a plain `type vN = ...;` local for it —
 * void-like / exception markers, inlined structs, and the class-native /
 * array / shared-native special cases whose `*_is_elided` / `*_can_inline`
 * predicate is mirrored below.
 *
 * INVARIANT: keep this set in lockstep with emit_value_stmt.  A value
 * declared here but elided there is only an unused-variable warning; a value
 * skipped here but assigned there is a use-before-declaration C error. */
static bool cg_value_skips_predecl(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!v)
        return true;
    if (cg_is_void_like(v) || v->op == XI_TRY || v->op == XI_END_TRY)
        return true;
    if (v->op == XI_STRUCT_NEW && cg_struct_can_inline(f, v))
        return true;
    if ((v->op == XI_COPY || v->op == XI_MOVE) && (cg_value_traces_to_inlined_struct(f, v) ||
                                                   cg_value_is_elided_heap_struct_alias(ctx, f, v)))
        return true;
    if (cg_shared_static_function_value_is_elided(ctx, f, v) ||
        cg_class_descriptor_value_is_elided(ctx, f, v) ||
        xicgen_box_only_feeds_native_int_print(ctx, f, v) ||
        cg_class_native_value_stmt_is_elided(ctx, f, v) ||
        cg_class_native_ctor_can_inline(ctx, f, v) ||
        cg_class_shared_native_ctor_value_is_elided(ctx, f, v, NULL) ||
        cg_class_shared_native_set_is_elided(ctx, f, v) ||
        cg_class_shared_native_value_is_elided(ctx, f, v))
        return true;
    if (cg_array_class_field_alloc_value_is_elided(ctx, f, v))
        return true;
    if (cg_class_native_map_field_value_is_elided(ctx, f, v))
        return true;
    if (cg_class_native_set_field_value_is_elided(ctx, f, v))
        return true;
    if (cg_class_native_map_method_call_value_is_elided(ctx, f, v) ||
        cg_class_native_set_method_call_value_is_elided(ctx, f, v))
        return true;
    if (cg_class_native_ref_stack_return_takes_value(ctx, f, v))
        return true;
    if (cg_array_typed_push_value_is_elided(ctx, f, v))
        return true;
    if ((v->op == XI_GET_SHARED && cg_value_only_used_by_inlined_struct_new(f, v)) ||
        cg_value_is_elided_heap_struct_alias(ctx, f, v))
        return true;
    return false;
}

/* Collect all values and phis to declare at function top.
 * When the function contains exception handling (setjmp/goto),
 * ALL SSA values are pre-declared to avoid jumping over decls. */
static void emit_declarations(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    bool pre_decl_all = cg_has_exception_handling(f);

    for (uint32_t var_id = 0; var_id < ctx->cell_var_count; var_id++) {
        if (!ctx->cell_vars[var_id])
            continue;
        fprintf(out, "    XrValue ");
        emit_cell_ref(out, (XiVarId) var_id);
        fprintf(out, " = XR_NULL_VAL;\n");
    }

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        /* Phi variables (always pre-declared) */
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_value_traces_to_inlined_struct(f, &phi->value))
                continue;
            if (cg_value_is_elided_heap_struct_alias(ctx, f, &phi->value))
                continue;
            XrRep rep = cg_value_plan_storage_rep(ctx, &phi->value);
            fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, &phi->value));
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
                if (cg_value_skips_predecl(ctx, f, v))
                    continue;
                XrRep rep = cg_value_plan_storage_rep(ctx, v);
                fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, v));
                emit_vref(out, v);
                if (rep == XR_REP_TAGGED)
                    fprintf(out, " = XR_NULL_VAL;\n");
                else
                    fprintf(out, " = 0;\n");
            }
        }
    }
}

static void cg_record_ir_conversion_stats(XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f)
        return;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_BOX)
                ctx->stats.xi_box_ops++;
            else if (v->op == XI_UNBOX)
                ctx->stats.xi_unbox_ops++;
        }
    }
}

static void cg_record_function_stats(XiCgenCtx *ctx, const XiFunc *f, bool typed_abi,
                                     bool native_receiver, bool coro_abi) {
    if (!ctx || !f)
        return;
    ctx->stats.functions_total++;
    if (coro_abi) {
        ctx->stats.functions_coro_abi++;
    } else if (typed_abi || native_receiver) {
        ctx->stats.functions_native_abi++;
    } else {
        ctx->stats.functions_tagged_abi++;
    }
    cg_record_ir_conversion_stats(ctx, f);
}

static bool cg_func_is_shared_slot_value(const XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f)
        return false;
    int limit = ctx->nshared < ctx->shared_cap ? ctx->nshared : ctx->shared_cap;
    for (int i = 0; i < limit; i++) {
        if (ctx->shared_funcs[i] == f)
            return true;
    }
    return false;
}

static int cg_shared_slot_for_func(const XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !ctx->module || !ctx->module->slot_funcs || !f)
        return -1;
    for (uint16_t i = 0; i < ctx->module->nslots; i++) {
        if (ctx->module->slot_funcs[i] == f)
            return (int) i;
    }
    return -1;
}

static bool cg_value_allocates_closure_for_func(const XiValue *v, const XiFunc *target) {
    if (!v || !target || v->aux != target)
        return false;
    return v->op == XI_CLOSURE_NEW || (v->op == XI_STACK_ALLOC && v->aux_int == XI_CLOSURE_NEW);
}

static bool cg_func_has_unelided_closure_value_use(XiCgenCtx *ctx, const XiFunc *owner,
                                                   const XiFunc *target, const char *prefix) {
    if (!ctx || !owner || !target)
        return false;

    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!cg_value_allocates_closure_for_func(v, target))
                continue;
            if (!cg_func_needs_aot_coro_ctx(ctx, owner) &&
                cg_shared_static_function_closure_is_elided(ctx, owner, v))
                continue;
            if (!cg_array_closure_value_only_used_by_inline_map(ctx, owner, prefix, v))
                return true;
        }
    }

    for (uint16_t i = 0; i < owner->nchildren; i++) {
        if (cg_func_has_unelided_closure_value_use(ctx, owner->children[i], target, prefix))
            return true;
    }
    return false;
}

static bool cg_native_receiver_method_call_needs_boxed_adapter(XiCgenCtx *ctx, const XiFunc *owner,
                                                               const XiValue *call,
                                                               const XiFunc *target) {
    if (!ctx || !owner || !call || !target ||
        (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) || call->nargs < 1)
        return false;

    const char *method_prefix = NULL;
    const XiFunc *mfunc = cg_class_native_resolve_method_call(ctx, owner, call, &method_prefix);
    (void) method_prefix;
    if (mfunc != target)
        return false;

    CgClassNativeFunc target_info = cg_class_native_func(ctx, target);
    const XiClassData *source_info = cg_class_native_instance_data(ctx, owner, call->args[0]);
    return !(cg_class_native_instance_origin(ctx, owner, call->args[0]) &&
             cg_class_native_can_pass_instance_as(ctx, source_info, target_info.class_data));
}

static bool cg_native_receiver_ctor_call_needs_boxed_adapter(XiCgenCtx *ctx, const XiFunc *owner,
                                                             const XiValue *call,
                                                             const XiFunc *target) {
    if (!ctx || !owner || !call || call->op != XI_CALL || !target)
        return false;

    const XiFunc *ctor = NULL;
    const XiClassData *cd = cg_class_native_ctor_call_data(ctx, owner, call, &ctor, NULL);
    if (!cd || ctor != target)
        return false;

    int shared_slot = -1;
    if (cg_class_native_ctor_can_inline(ctx, owner, call) ||
        cg_class_shared_native_ctor_value_is_elided(ctx, owner, call, &shared_slot))
        return false;
    return true;
}

static bool cg_func_has_native_receiver_boxed_use(XiCgenCtx *ctx, const XiFunc *owner,
                                                  const XiFunc *target, const char *prefix) {
    if (!ctx || !owner || !target)
        return false;

    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (cg_value_allocates_closure_for_func(v, target) &&
                (cg_func_needs_aot_coro_ctx(ctx, owner) ||
                 !cg_shared_static_function_closure_is_elided(ctx, owner, v)) &&
                !cg_array_closure_value_only_used_by_inline_map(ctx, owner, prefix, v))
                return true;
            if (cg_native_receiver_method_call_needs_boxed_adapter(ctx, owner, v, target) ||
                cg_native_receiver_ctor_call_needs_boxed_adapter(ctx, owner, v, target))
                return true;
        }
    }

    for (uint16_t i = 0; i < owner->nchildren; i++) {
        if (cg_func_has_native_receiver_boxed_use(ctx, owner->children[i], target, prefix))
            return true;
    }
    return false;
}

static bool cg_func_has_native_receiver_boxed_use_in_bundle(XiCgenCtx *ctx, const XiFunc *target,
                                                            const char *prefix) {
    if (!ctx || !target)
        return false;

    bool scanned_current = false;
    for (int i = 0; i < ctx->all_nmodules; i++) {
        const XiModule *mod = ctx->all_modules ? ctx->all_modules[i] : NULL;
        if (!mod || !mod->init)
            continue;
        if (mod == ctx->module)
            scanned_current = true;
        if (cg_func_has_native_receiver_boxed_use(ctx, mod->init, target, prefix))
            return true;
    }

    const XiFunc *root = ctx->module ? ctx->module->init : NULL;
    return !scanned_current && cg_func_has_native_receiver_boxed_use(ctx, root, target, prefix);
}

static bool cg_func_needs_boxed_adapter(XiCgenCtx *ctx, const XiFunc *f, const char *prefix,
                                        bool typed_abi, bool native_receiver) {
    if (!typed_abi && !native_receiver)
        return false;

    const XiFunc *root = ctx && ctx->module ? ctx->module->init : NULL;
    if (cg_func_is_shared_slot_value(ctx, f)) {
        int func_slot = cg_shared_slot_for_func(ctx, f);
        if (func_slot >= 0) {
            if (!cg_shared_static_function_slot_can_elide(ctx, root, func_slot, f))
                return true;
        } else if (!native_receiver) {
            return true;
        }
    }

    if (native_receiver)
        return cg_func_has_native_receiver_boxed_use_in_bundle(ctx, f, prefix);

    if (cg_func_needs_sync_go_wrapper_ctx(ctx, f) && cg_func_has_native_class_ptr_param(ctx, f))
        return true;

    return cg_func_has_unelided_closure_value_use(ctx, root, f, prefix);
}

/* Emit the purity attribute proven by prepare (no-op without a plan).
 * Placed between `static` and the return type on definitions and forward
 * declarations so clang/gcc can CSE / LICM across call sites. */
static void emit_func_attr_qualifier(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    const XaotFuncAttrPlan *plan = xaot_bundle_find_func_attr_plan(cg_ctx_aot_bundle(ctx), f);
    if (!plan)
        return;
    if (plan->flags & XAOT_FN_ATTR_CONST)
        fprintf(out, "XRT_FN_CONST ");
    else if (plan->flags & XAOT_FN_ATTR_PURE)
        fprintf(out, "XRT_FN_PURE ");
}

static void xi_cgen_func(XiCgenCtx *ctx, FILE *out, XiFunc *f, const char *prefix) {
    XR_DCHECK(out != NULL, "xi_cgen_func: NULL output");
    XR_DCHECK(f != NULL, "xi_cgen_func: NULL func");
    /* Auto-lower if callers bypass the pipeline. */
    if (f->stage < XI_STAGE_REPPED) {
        XiRepPolicy policy = xi_rep_policy_native_boundary();
        xi_opt_select_rep_with_policy(f, &policy);
        xi_opt_box_elim(f);
    }
    if (f->stage < XI_STAGE_BACKEND)
        xi_backend_lower(f);

    /* Emit nested children first (forward declarations already emitted) */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_cgen_func(ctx, out, f->children[i], prefix);
    }

    cg_class_field_cache_reset(&ctx->class_field_cache);
    cg_array_data_cache_decls_reset(ctx);

    bool needs_aot_coro = cg_func_needs_aot_coro_ctx(ctx, f);
    bool typed_abi = cg_func_uses_typed_abi(ctx, f);
    bool native_receiver = cg_class_func_uses_native_receiver(ctx, f);
    bool boxed_adapter = cg_func_needs_boxed_adapter(ctx, f, prefix, typed_abi, native_receiver);
    cg_record_function_stats(ctx, f, typed_abi, native_receiver, needs_aot_coro);

    if (needs_aot_coro) {
        xi_cgen_coro_func(ctx, out, f, prefix);
        return;
    }

    /* Function signature.  Closure children with captures receive a hidden
     * first parameter xrt_closure_t *_cl for per-closure upvalue access. */
    bool has_cl = (f->ncaptures > 0);
    fprintf(out, "static ");
    emit_func_attr_qualifier(ctx, out, f);
    if (!emit_class_native_return_type(ctx, out, prefix, f))
        fprintf(out, "%s", cg_func_return_abi_c_type(ctx, f));
    fprintf(out, " ");
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "(");
    if (has_cl) {
        fprintf(out, "xrt_closure_t *_cl");
        for (uint16_t i = 0; i < f->nparams; i++)
            fprintf(out, ", "), emit_class_native_param_decl(ctx, out, prefix, f, i);
    } else if (f->nparams == 0) {
        fprintf(out, "xrt_closure_t *_cl");
    } else {
        fprintf(out, "xrt_closure_t *_cl");
        for (uint16_t i = 0; i < f->nparams; i++)
            fprintf(out, ", "), emit_class_native_param_decl(ctx, out, prefix, f, i);
    }
    fprintf(out, ") {\n");
    if (!has_cl)
        fprintf(out, "    (void)_cl;\n");

    ctx->pre_decl_all = cg_has_exception_handling(f);
    cg_prepare_cell_vars(ctx, f);
    cg_class_field_cache_collect(ctx, f);
    emit_declarations(ctx, out, f);
    emit_class_field_cache_decls(ctx, out);

    /* Blocks in order */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (f->blocks[bi] && !cg_structured_counted_loop_block_is_elided(f, f->blocks[bi]) &&
            !cg_structured_array_fill_loop_block_is_elided(ctx, f, f->blocks[bi]))
            emit_block(ctx, out, f, f->blocks[bi], prefix);
    }

    fprintf(out, "}\n\n");

    if (native_receiver && boxed_adapter) {
        ctx->stats.boxed_adapters++;
        emit_class_native_boxed_adapter(ctx, out, prefix, f);
    } else if (typed_abi && boxed_adapter) {
        ctx->stats.boxed_adapters++;
        if (!emit_class_native_typed_boxed_adapter(ctx, out, prefix, f)) {
            fprintf(out, "static XrValue ");
            emit_typed_abi_fname(ctx, out, prefix, f);
            fprintf(out, "(xrt_closure_t *_cl");
            for (uint16_t i = 0; i < f->nparams; i++)
                fprintf(out, ", XrValue p%u", i);
            fprintf(out, ") {\n");
            fprintf(out, "    return ");
            const char *conv_suffix = emit_conversion_prefix(
                out, f->return_type, cg_func_return_abi_rep(ctx, f), XR_REP_TAGGED);
            emit_fname(ctx, out, prefix, f);
            fprintf(out, "(_cl");
            for (uint16_t i = 0; i < f->nparams; i++) {
                fprintf(out, ", ");
                XrRep param_rep = cg_func_param_abi_rep(ctx, f, i);
                const XrType *param_type = f->params && f->params[i] ? f->params[i]->type : NULL;
                const char *param_suffix =
                    emit_conversion_prefix(out, param_type, XR_REP_TAGGED, param_rep);
                fprintf(out, "p%u", i);
                emit_conversion_suffix(out, param_suffix);
            }
            fprintf(out, ")");
            emit_conversion_suffix(out, conv_suffix);
            fprintf(out, ";\n");
            fprintf(out, "}\n\n");
        }
    }

    if (cg_func_needs_sync_go_wrapper_ctx(ctx, f)) {
        ctx->stats.sync_go_wrappers++;
        emit_sync_go_wrapper(ctx, out, f, prefix);
    }
}

/* ========== Forward Declarations ========== */

static void emit_forward_decls(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    /* Recurse children first */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            emit_forward_decls(ctx, out, f->children[i], prefix);
    }

    bool needs_aot_coro = cg_func_needs_aot_coro_ctx(ctx, f);
    fprintf(out, "static ");
    emit_func_attr_qualifier(ctx, out, f);
    if (needs_aot_coro) {
        fprintf(out, "XrValue");
    } else if (!emit_class_native_return_type(ctx, out, prefix, f)) {
        fprintf(out, "%s", cg_func_return_abi_c_type(ctx, f));
    }
    fprintf(out, " ");
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "(");
    fprintf(out, "xrt_closure_t *_cl");
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (needs_aot_coro) {
            fprintf(out, ", XrValue p%u", i);
        } else {
            fprintf(out, ", ");
            emit_class_native_param_decl(ctx, out, prefix, f, i);
        }
    }
    fprintf(out, ");\n");

    bool typed_abi = cg_func_uses_typed_abi(ctx, f);
    bool native_receiver = cg_class_func_uses_native_receiver(ctx, f);
    if (!needs_aot_coro &&
        cg_func_needs_boxed_adapter(ctx, f, prefix, typed_abi, native_receiver)) {
        fprintf(out, "static XrValue ");
        emit_typed_abi_fname(ctx, out, prefix, f);
        fprintf(out, "(xrt_closure_t *_cl");
        for (uint16_t i = 0; i < f->nparams; i++)
            fprintf(out, ", XrValue p%u", i);
        fprintf(out, ");\n");
    }

    bool needs_sync_go = !needs_aot_coro && cg_func_needs_sync_go_wrapper_ctx(ctx, f);
    if (needs_aot_coro || needs_sync_go) {
        fprintf(out, "static void *");
        emit_fname_suffix(ctx, out, prefix, f, "_aot_frame_new");
        fprintf(out, "(");
        emit_aot_frame_new_params(out, f, needs_sync_go);
        fprintf(out, ");\n");
        fprintf(out, "static XrAotResult ");
        emit_fname_suffix(ctx, out, prefix, f, "_aot_resume");
        fprintf(out, "(void *raw_frame, const XrAotContext *ctx);\n");
        fprintf(out, "static const XrAotCoroDesc ");
        emit_fname_suffix(ctx, out, prefix, f, "_aot_desc");
        fprintf(out, ";\n");
    }
}

#include "xi_cgen_import_helpers.inc.c"
#include "xi_cgen_program_entry.inc.c"
