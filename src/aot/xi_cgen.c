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
#include "xaot_struct_name.h"
#include "xi_to_c_dispatch_gen.h"
#include "xi_to_c_stmt_dispatch_gen.h"
#include "../ir/xi_analysis.h"
#include "../ir/xi_backend_lower.h"
#include "../shared/xr_hash_core.h"
#include "../ir/xi_op_name.h"
#include "../ir/xi_ops_gen.h"
#include "../ir/xi_opt.h"
#include "../ir/xi_own.h"
#include "../ir/xi_range.h"
#include "../ir/xi_value_query.h"
#include "../ir/xi_coro_analyze.h"
#include "../base/xdefs.h"
#include "../runtime/class/xenum.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/value/xstruct_layout.h"
#include "../base/xglobal_indices.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xffi_sig.h"
#include "../coro/xaot_coro.h"
#include "xrt_method_symbols.h"
#include "../base/xmemstream.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xtype_ref.h"
#include <string.h>
#include <inttypes.h>
#include <math.h>
/* ========== Representation Helpers ========== */
/* Read the stored representation set by select_rep.
 * select_rep always runs in the AOT pipeline before code generation. */
static inline XrRep cg_rep(const XiValue *v) {
    return v ? (XrRep) v->rep : XR_REP_TAGGED;
}

static bool cg_func_needs_sync_go_wrapper_ctx(XiCgenCtx *ctx, const XiFunc *f);
static bool cg_func_needs_sync_backedge_heartbeat_ctx(XiCgenCtx *ctx, const XiFunc *f);
static bool cg_tagged_array_index_get_can_borrow(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);
static bool cg_value_is_borrowed_array_slot_alias(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *v);
static bool cg_direct_ref_param_noescape(XiCgenCtx *ctx, const XiFunc *target, uint16_t param_index,
                                         uint8_t depth);
static bool cg_static_direct_function_closure_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *v);

static const char *ctype_str(XrRep rep) {
    switch (rep) {
        case XR_REP_I64:
            return "int64_t";
        case XR_REP_F64:
            return "double";
        case XR_REP_PTR:
        case XR_REP_RAWPTR:
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
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v) ||
            v->op == XI_MOVE) &&
           v->nargs >= 1) {
        v = v->args[0];
    }
    return v;
}

static bool cg_value_is_null_const(const XiValue *v) {
    return v && v->op == XI_CONST && v->type && v->type->kind == XR_KIND_NULL;
}

static bool cg_type_is_i64_optional(const XrType *type) {
    return type && type->is_nullable && type->kind == XR_KIND_INT;
}

static bool cg_value_is_i64_optional_blocking_result_root(const XiValue *v) {
    if (!v || !cg_type_is_i64_optional(v->type))
        return false;
    return xi_value_is_blocking_work_queue_method_call(v) ||
           xi_value_is_blocking_result_group_method_call(v);
}

static const XiValue *cg_i64_optional_blocking_result_root(const XiValue *v) {
    const XiValue *cur = v;
    for (uint8_t depth = 0; cur && depth < 16; depth++) {
        if (cg_value_is_i64_optional_blocking_result_root(cur))
            return cur;
        if (((xi_copy_is_identity_alias(cur) || cur->op == XI_MOVE) &&
             cg_rep(cur) == XR_REP_TAGGED && cur->nargs >= 1) ||
            (cur->op == XI_UNBOX && cg_rep(cur) == XR_REP_TAGGED && cur->nargs >= 1)) {
            cur = cur->args[0];
            continue;
        }
        return NULL;
    }
    return NULL;
}

static bool cg_i64_optional_null_compare(const XiValue *user, const XiValue *target) {
    if (!user || (user->op != XI_EQ && user->op != XI_NE) || user->nargs < 2)
        return false;
    return (user->args[0] == target && cg_value_is_null_const(user->args[1])) ||
           (user->args[1] == target && cg_value_is_null_const(user->args[0]));
}

static bool cg_i64_optional_value_uses_are_native(const XiFunc *f, const XiValue *target,
                                                  uint8_t depth) {
    if (!f || !target || depth > 16)
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
                switch ((XiOp) user->op) {
                    case XI_ISNULL:
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (ai != 0)
                            return false;
                        break;
                    case XI_UNBOX:
                        if (ai != 0)
                            return false;
                        if (cg_rep(user) == XR_REP_TAGGED &&
                            !cg_i64_optional_value_uses_are_native(f, user, (uint8_t) (depth + 1)))
                            return false;
                        break;
                    case XI_COPY:
                    case XI_MOVE:
                        if (ai != 0)
                            return false;
                        if (cg_rep(user) == XR_REP_TAGGED &&
                            !cg_i64_optional_value_uses_are_native(f, user, (uint8_t) (depth + 1)))
                            return false;
                        break;
                    default:
                        /* Null-compare of the optional value is native-safe;
                         * spelled as an if-test so the template lowering
                         * guard only sees real emitter cases. */
                        if (user->op == XI_EQ || user->op == XI_NE) {
                            if (!cg_i64_optional_null_compare(user, target))
                                return false;
                            break;
                        }
                        return false;
                }
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_i64_optional_blocking_result(const XiFunc *f, const XiValue *v) {
    const XiValue *root = cg_i64_optional_blocking_result_root(v);
    return root && cg_i64_optional_value_uses_are_native(f, root, 0);
}

static bool cg_type_has_no_aot_arc_header(const XrType *type) {
    /* Arrays/maps/sets carry an embedded unified XrObjHeader and are reclaimed by
     * xrt_retain/xrt_release. Fixed arrays are by-value aggregates with no
     * standalone ARC header. */
    return type && type->kind == XR_KIND_FIXED_ARRAY;
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
    if (v->op == XI_DEFER)
        return true;
    if (v->type && XR_TYPE_IS_UNIT(v->type))
        return true;

    uint8_t result_kind = xi_generated_op_result_kind(v->op);
    if (result_kind == XI_GEN_RESULT_VOID)
        return true;
    return false;
}

static bool cg_phi_has_storage(const XiPhi *phi) {
    return phi && !cg_is_void_like(&phi->value);
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
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v)) &&
           v->nargs >= 1)
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

/* ========== Codegen Context ========== */
/* Initial capacities for the per-module shared-slot / method / import tables.
 * These grow on demand (cg_reserve_shared / _methods / _imports), so AOT can
 * compile modules with more than the starting counts.  The Xi->C path is not
 * constrained by the VM bytecode register operand space. */
#define CG_INIT_SHARED 512
#define CG_INIT_METHODS 256
#define CG_INIT_IMPORTS 256
#define CG_MAX_SYNC_GO_TARGETS 512
#define CG_MAX_SYNC_HEARTBEAT_TARGETS 1024
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
    const XiEnumData *target_enum;   /* XiEnumData* if this export is an enum */
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
    /* When true, top-level functions and their declarations are emitted with
     * external (non-static) linkage so a multi-module bundle can be compiled as
     * one object per module and linked together (114 separate compilation).
     * Single-module bundles keep file-static linkage. */
    bool extern_linkage;
    /* While a translation unit's body is emitted (collect_xmod_refs gated),
     * every cross-module function referenced through emit_fname is recorded
     * here so only the imports a unit actually uses are forward-declared.  This
     * keeps an object's bytes independent of unrelated exports added or removed
     * in other modules (114 incremental caching). */
    bool collect_xmod_refs;
    const XiFunc **xmod_ref_funcs;
    const char **xmod_ref_prefixes;
    int n_xmod_refs;
    int xmod_refs_cap;
    bool *cell_vars;
    bool *cell_release_vars;
    bool *cell_heap_capture_vars;
    const XiValue **cell_origins;
    uint32_t cell_var_count;
    /* Per-function phi coalescing: value-id-indexed map from a phi's SSA id to
     * the SSA id of the C variable it shares (identity = its own). Built by
     * cg_build_phi_coalesce for the normal (non-coro) emission path; phi_repr_active
     * gates lookups so the coro path and unbuilt functions keep identity naming. */
    uint32_t *phi_repr;
    uint32_t phi_repr_cap;
    bool phi_repr_active;
    const char *shared_name;
    CgImportEntry *imports;
    int imports_cap;
    int nimports;
    XiModule **all_modules; /* full modules array for resolved-index lookups */
    int all_nmodules;
    bool emit_main;
    bool freestanding_profile;
    bool error; /* set on fatal codegen errors (unknown builtin, etc.) */
    XiCgenStats stats;
    XiCgenCoroFrameStats coro_frame_stats;
    const XaotBundle *aot_bundle;
    const XiFunc *sync_go_targets[CG_MAX_SYNC_GO_TARGETS];
    int nsync_go_targets;
    const XiFunc *sync_heartbeat_targets[CG_MAX_SYNC_HEARTBEAT_TARGETS];
    int nsync_heartbeat_targets;
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
    uint32_t bucket = (uint32_t) xr_hash_core_bytes(s, len) & (CG_STRLIT_BUCKETS - 1);
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

/* Drop the interned string pool so the next translation unit starts numbering
 * literals from zero.  Used between per-module objects (114) so each object
 * carries only its own _xstr_* definitions (file-static, no cross-TU clash). */
static void cg_reset_str_lits(XiCgenCtx *ctx) {
    for (int b = 0; b < CG_STRLIT_BUCKETS; b++) {
        CgStrLit *e = ctx->strlit_buckets[b];
        while (e) {
            CgStrLit *next = e->next;
            xr_free(e->str);
            xr_free(e);
            e = next;
        }
        ctx->strlit_buckets[b] = NULL;
    }
    ctx->nstrlit = 0;
}

/* Storage-class prefix for emitted top-level objects: external linkage in
 * multi-module separate-compilation mode, file-static otherwise.  Function
 * linkage is finer-grained (see cg_func_linkage): only symbols that form the
 * cross-module ABI stay external. */
static const char *cg_linkage(const XiCgenCtx *ctx) {
    return ctx->extern_linkage ? "" : "static ";
}

static bool cg_func_stable_cname(const XiCgenCtx *ctx, const XiFunc *f, const char *prefix,
                                 char *buf, size_t bufsz);
static bool cg_func_needs_external_linkage(const XiCgenCtx *ctx, const XiFunc *f,
                                           const char *prefix);

static bool cg_func_is_par_for_native_callback(const XiFunc *f) {
    return f && (f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_FOR_I64 ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_REDUCE_I64_BODY ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_REDUCE_I64_COMBINE ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_BODY ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_COMBINE ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_COLLECT_SCALAR_BODY ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_RANGE_I64);
}

static const char *cg_func_linkage(const XiCgenCtx *ctx, const XiFunc *f, const char *prefix) {
    if (cg_func_is_par_for_native_callback(f))
        return "static XR_AINLINE ";
    if (cg_func_needs_external_linkage(ctx, f, prefix))
        return "";
    if (ctx && ctx->extern_linkage)
        return "static XR_AINLINE ";
    return cg_linkage(ctx);
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

static bool cg_func_needs_aot_coro_ctx(XiCgenCtx *ctx, const XiFunc *f);

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
 * Uses arena-safe XiClassMethod array (no AST dependency). The child
 * indices are relative to the function that lowered the class; when a
 * caller probes a different parent, the index may land on an unrelated
 * child (e.g. a defer closure), so verify the resolved child really is
 * a constructor before trusting it. */
static const XiFunc *cg_find_constructor(const XiFunc *parent, const XiClassData *cd) {
    if (!cd || !cd->methods || !parent)
        return NULL;
    for (uint16_t ci = 0; ci < cd->nmethod; ci++) {
        if (cd->methods[ci].is_static_constructor)
            continue;
        if (cd->methods[ci].is_constructor) {
            if (cd->child_idx && ci < cd->ninst + cd->nstat) {
                uint16_t idx = cd->child_idx[ci];
                if (idx < parent->nchildren) {
                    const XiFunc *child = parent->children[idx];
                    if (child && child->name && strcmp(child->name, "constructor") == 0)
                        return child;
                }
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

static const XiClassData *cg_class_native_data_by_name(const XiCgenCtx *ctx, const char *name);

static bool cg_class_data_name_matches(const XiClassData *cd, const char *name) {
    if (!cd || !name)
        return false;
    if (cd->class_name && strcmp(cd->class_name, name) == 0)
        return true;
    if (cd->display_name && strcmp(cd->display_name, name) == 0)
        return true;
    return cd->generic_origin_name && strcmp(cd->generic_origin_name, name) == 0;
}

static const XiFunc *cg_lookup_method_in_class_data(const XiClassData *cd, const XiFunc *owner,
                                                    int method_idx) {
    if (!cd || !owner || method_idx < 0 || !cd->child_idx)
        return NULL;
    if ((uint16_t) method_idx >= cd->ninst || (uint16_t) method_idx >= cd->nmethod)
        return NULL;
    uint16_t child_idx = cd->child_idx[method_idx];
    if (child_idx >= owner->nchildren)
        return NULL;
    return owner->children[child_idx];
}

/* Lookup a class instance method by name and receiver class.
 * Builtin receivers must never fall through to a class method with the
 * same source-level name.
 * Walks the inheritance chain so a method inherited from a (possibly
 * cross-module) base class resolves on a derived receiver.
 * If out_prefix is non-NULL, stores the method's module prefix (for
 * cross-module class methods; NULL means current module). */
static const XiFunc *cg_lookup_method(XiCgenCtx *ctx, const char *name, const char *class_name,
                                      const char **out_prefix) {
    if (!name || !class_name)
        return NULL;
    const char *cur = class_name;
    for (int depth = 0; cur && depth < 16; depth++) {
        for (int i = 0; i < ctx->nmethod; i++) {
            if (!ctx->methods[i].name || strcmp(ctx->methods[i].name, name) != 0)
                continue;
            if (ctx->methods[i].class_name && strcmp(ctx->methods[i].class_name, cur) == 0) {
                if (out_prefix)
                    *out_prefix = ctx->methods[i].module_prefix;
                return ctx->methods[i].func;
            }
        }
        const XiClassData *cd = cg_class_native_data_by_name(ctx, cur);
        cur = cd ? cd->super_name : NULL;
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
        if (!cg_class_data_name_matches(cd, class_name))
            continue;
        const XiFunc *method = cg_lookup_method_in_class_data(cd, ctx->module->init, method_idx);
        if (!method)
            return NULL;
        if (out_prefix)
            *out_prefix = NULL;
        return method;
    }
    for (int i = 0; i < ctx->nimports; i++) {
        const CgImportEntry *imp = &ctx->imports[i];
        if (!cg_class_data_name_matches(imp->target_class, class_name) || !imp->exporter_func)
            continue;
        const XiFunc *method =
            cg_lookup_method_in_class_data(imp->target_class, imp->exporter_func, method_idx);
        if (!method)
            return NULL;
        if (out_prefix)
            *out_prefix = imp->target_mod_name;
        return method;
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

/* Write f's order-independent C name into buf when f is cross-module-visible in
 * separate-compilation mode, returning true.  Cross-module-visible functions are
 * a module init (`prefix_modinit`), an exported top-level function
 * (`prefix_name_exp`), or a constructor/method of an exported class
 * (`prefix_Class_method_m`).  These names omit the per-emission ordinal so a
 * module's object file stays cache-valid when unrelated functions are added or
 * removed elsewhere in the bundle (114 incremental caching).  Returns false for
 * internal functions (closures, private helpers), which are referenced only
 * within their own translation unit and keep a per-module ordinal suffix.
 *
 * Stable names always end in a letter (`_exp`/`_modinit`/`_m`) while the ordinal
 * form ends in `_<digits>`, so the two name spaces never collide. */
static bool cg_func_stable_cname(const XiCgenCtx *ctx, const XiFunc *f, const char *prefix,
                                 char *buf, size_t bufsz) {
    if (!ctx || !f || !prefix || !prefix[0] || !ctx->all_modules)
        return false;
    char pbuf[128];
    sanitize_c_ident_part(pbuf, sizeof(pbuf), prefix);
    for (int i = 0; i < ctx->all_nmodules; i++) {
        XiModule *mod = ctx->all_modules[i];
        if (!mod || !mod->name || strcmp(mod->name, prefix) != 0)
            continue;
        if (mod->init == f) {
            snprintf(buf, bufsz, "%s_modinit", pbuf);
            return true;
        }
        for (uint16_t e = 0; e < mod->nexports; e++) {
            if (mod->exports[e].function == f && mod->exports[e].name) {
                char nb[128];
                sanitize_c_ident_part(nb, sizeof(nb), mod->exports[e].name);
                snprintf(buf, bufsz, "%s_%s_exp", pbuf, nb);
                return true;
            }
        }
        /* Constructor / method of an exported class: keyed by class + method
         * name so the symbol is independent of sibling ordering. */
        for (uint16_t e = 0; e < mod->nexports; e++) {
            const XiClassData *cd = mod->exports[e].class_data;
            if (!cd || !cd->child_idx || !mod->init)
                continue;
            char cb[128];
            sanitize_c_ident_part(cb, sizeof(cb), cd->class_name ? cd->class_name : "cls");
            for (uint16_t mi = 0; mi < cd->nmethod; mi++) {
                uint16_t ci = cd->child_idx[mi];
                if (ci < mod->init->nchildren && mod->init->children[ci] == f) {
                    char mb[128];
                    sanitize_c_ident_part(mb, sizeof(mb),
                                          cd->methods[mi].name ? cd->methods[mi].name : "m");
                    snprintf(buf, bufsz, "%s_%s_%s_m", pbuf, cb, mb);
                    return true;
                }
            }
            if (cd->clinit_child_idx >= 0 &&
                (uint16_t) cd->clinit_child_idx < mod->init->nchildren &&
                mod->init->children[cd->clinit_child_idx] == f) {
                snprintf(buf, bufsz, "%s_%s_clinit_m", pbuf, cb);
                return true;
            }
        }
        return false; /* owning module found, but f is internal */
    }
    return false;
}

/* In separate-compilation mode, only the module ABI surface needs external
 * linkage: module init, exported top-level functions, and exported class
 * constructors/methods (all recognized by cg_func_stable_cname).  Private
 * helpers are referenced only inside their defining translation unit, so keep
 * them file-local and inlineable even when other module symbols are external. */
static bool cg_func_needs_external_linkage(const XiCgenCtx *ctx, const XiFunc *f,
                                           const char *prefix) {
    if (!ctx || !ctx->extern_linkage || !f)
        return false;
    if (!prefix || !prefix[0])
        return true;
    if (!ctx->module || !ctx->module->name || strcmp(prefix, ctx->module->name) != 0)
        return true;
    char stable[384];
    return cg_func_stable_cname(ctx, f, prefix, stable, sizeof(stable));
}

/* Record a cross-module function reference (deduplicated by pointer) so the
 * current translation unit forward-declares only the imported symbols it
 * actually uses.  prefix is the owning module's name (stable storage). */
static void cg_note_xmod_ref(XiCgenCtx *ctx, const XiFunc *f, const char *prefix) {
    for (int i = 0; i < ctx->n_xmod_refs; i++) {
        if (ctx->xmod_ref_funcs[i] == f)
            return;
    }
    if (ctx->n_xmod_refs >= ctx->xmod_refs_cap) {
        int nc = ctx->xmod_refs_cap > 0 ? ctx->xmod_refs_cap * 2 : 16;
        const XiFunc **nf =
            (const XiFunc **) xr_realloc(ctx->xmod_ref_funcs, (size_t) nc * sizeof(*nf));
        const char **np =
            (const char **) xr_realloc(ctx->xmod_ref_prefixes, (size_t) nc * sizeof(*np));
        if (!nf)
            ctx->error = true;
        else
            ctx->xmod_ref_funcs = nf;
        if (!np)
            ctx->error = true;
        else
            ctx->xmod_ref_prefixes = np;
        if (ctx->error)
            return;
        ctx->xmod_refs_cap = nc;
    }
    ctx->xmod_ref_funcs[ctx->n_xmod_refs] = f;
    ctx->xmod_ref_prefixes[ctx->n_xmod_refs] = prefix;
    ctx->n_xmod_refs++;
}

/* Write the C name for a function.
 *
 * Single-module / file-static mode: prefix_funcname_id, where the numeric id is
 * assigned on first use (cgen_id == 0 means unassigned) so anonymous closures
 * and same-named constructors stay distinct.
 *
 * Separate-compilation mode (ctx->extern_linkage): cross-module-visible
 * functions instead get an order-independent name (see cg_func_stable_cname) so
 * adding or removing an unrelated function elsewhere never changes this object's
 * bytes (114 incremental caching).  Internal functions keep the ordinal form. */
static void emit_fname(XiCgenCtx *ctx, FILE *out, const char *prefix, const XiFunc *f) {
    XR_DCHECK(f != NULL, "emit_fname: NULL func");

    bool have_prefix = prefix && prefix[0];
    char prefix_buf[128];
    if (have_prefix)
        sanitize_c_ident_part(prefix_buf, sizeof(prefix_buf), prefix);

    /* Record cross-module references so the unit forward-declares only the
     * imports it uses (114): a reference is cross-module when its owning prefix
     * differs from the module currently being emitted. */
    if (ctx->extern_linkage && ctx->collect_xmod_refs && have_prefix && ctx->module &&
        ctx->module->name && strcmp(prefix, ctx->module->name) != 0)
        cg_note_xmod_ref(ctx, f, prefix);

    if (ctx->extern_linkage && have_prefix) {
        char stable[384];
        if (cg_func_stable_cname(ctx, f, prefix, stable, sizeof(stable))) {
            fprintf(out, "%s", stable);
            return;
        }
    }

    const char *raw = f->name ? f->name : "anon";
    char buf[128];
    sanitize_c_ident_part(buf, sizeof(buf), raw);

    /* Assign a stable unique ID on first use (cgen_id == 0 means unassigned) */
    XiFunc *mf = (XiFunc *) (uintptr_t) f; /* cast away const for cgen_id write */
    if (mf->cgen_id == 0)
        mf->cgen_id = ++ctx->fname_counter;

    if (have_prefix)
        fprintf(out, "%s_%s_%d", prefix_buf, buf, f->cgen_id);
    else
        fprintf(out, "fn_%s_%d", buf, f->cgen_id);
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
    const XiClassData *class_data;
} CgStaticFunctionCall;

static bool cg_func_needs_aot_coro_ctx(XiCgenCtx *ctx, const XiFunc *f);
static const XiFunc *cg_class_native_resolve_method_call(XiCgenCtx *ctx, const XiFunc *current,
                                                         const XiValue *call,
                                                         const char **out_prefix);

static CgStaticFunctionCall cg_no_static_function_call(void) {
    CgStaticFunctionCall call;
    call.func = NULL;
    call.prefix = NULL;
    call.is_class_constructor = false;
    call.class_data = NULL;
    return call;
}

static CgStaticFunctionCall cg_static_function_call(const XiFunc *func, const char *prefix) {
    CgStaticFunctionCall call;
    call.func = func;
    call.prefix = prefix;
    call.is_class_constructor = false;
    call.class_data = NULL;
    return call;
}

static CgStaticFunctionCall cg_static_class_constructor_data_call(const XiFunc *func,
                                                                  const char *prefix,
                                                                  const XiClassData *class_data) {
    CgStaticFunctionCall call;
    call.func = func;
    call.prefix = prefix;
    call.is_class_constructor = true;
    call.class_data = class_data;
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
#include "xi_cgen_coro_resolver.inc.c"

static bool cg_shared_static_function_ownership_is_noop(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *v) {
    if (!ctx || !v || (v->op != XI_RETAIN && v->op != XI_RELEASE) || v->nargs < 1)
        return false;
    const XiValue *arg = cg_unwrap_identity_value(v->args[0]);
    if (arg && cg_static_direct_function_closure_is_elided(ctx, current, arg))
        return true;
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

static bool cg_static_direct_function_closure_is_elided(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *v) {
    if (!ctx || !current || !v ||
        (v->op != XI_CLOSURE_NEW && !(v->op == XI_STACK_ALLOC && v->aux_int == XI_CLOSURE_NEW)) ||
        !v->aux)
        return false;
    const XiFunc *target = (const XiFunc *) v->aux;
    return cg_shared_function_target_can_elide(ctx, target) &&
           cg_shared_static_function_value_uses_are_direct(ctx, current, v, target, 0);
}

static bool cg_import_ref_targets_func(XiCgenCtx *ctx, const XiImportRef *ref,
                                       const XiFunc *target) {
    if (!ctx || !ref || !target)
        return false;
    if (ref->resolved_mod_index >= 0 && ref->resolved_mod_index < ctx->all_nmodules &&
        ref->resolved_shared_slot >= 0) {
        const XiModule *mod = ctx->all_modules[ref->resolved_mod_index];
        if (mod && mod->slot_funcs && ref->resolved_shared_slot < (int) mod->nslots &&
            mod->slot_funcs[ref->resolved_shared_slot] == target)
            return true;
    }
    CgStaticFunctionCall call = cg_resolve_import_function_call(ctx, ref);
    return call.func == target && !call.is_class_constructor;
}

static bool cg_imported_static_function_uses_are_direct(XiCgenCtx *ctx, const XiFunc *owner,
                                                        const XiFunc *target) {
    if (!ctx || !owner || !target)
        return false;
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_IMPORT_REF || !v->aux)
                continue;
            if (!cg_import_ref_targets_func(ctx, (const XiImportRef *) v->aux, target))
                continue;
            if (!cg_shared_static_function_value_uses_are_direct(ctx, owner, v, target, 0))
                return false;
        }
    }
    for (uint16_t ci = 0; ci < owner->nchildren; ci++) {
        if (!cg_imported_static_function_uses_are_direct(ctx, owner->children[ci], target))
            return false;
    }
    return true;
}

static bool cg_imported_static_function_uses_are_direct_in_bundle(XiCgenCtx *ctx,
                                                                  const XiFunc *target) {
    if (!ctx || !target)
        return false;
    for (int i = 0; i < ctx->all_nmodules; i++) {
        const XiModule *mod = ctx->all_modules ? ctx->all_modules[i] : NULL;
        if (!mod || !mod->init)
            continue;
        if (!cg_imported_static_function_uses_are_direct(ctx, mod->init, target))
            return false;
    }
    return true;
}

static bool cg_debug_boxed_adapter_enabled(void) {
    return getenv("XRAY_CGEN_DEBUG_BOXED") != NULL;
}

static bool cg_shared_static_function_slot_can_elide(XiCgenCtx *ctx, const XiFunc *current,
                                                     int slot, const XiFunc *target) {
    bool dbg = cg_debug_boxed_adapter_enabled();
    if (!ctx || !ctx->module || !ctx->module->init || slot < 0 || !target)
        return false;
    const XiFunc *slot_target = cg_shared_function_slot_target(ctx, current, slot);
    if (slot_target != target) {
        if (dbg) {
            fprintf(stderr, "[xi_cgen][boxed] cannot elide %s slot=%d: slot target is %s\n",
                    target->name ? target->name : "?", slot,
                    slot_target && slot_target->name ? slot_target->name : "<null>");
        }
        return false;
    }
    if (!cg_shared_function_target_can_elide(ctx, target)) {
        if (dbg) {
            fprintf(stderr, "[xi_cgen][boxed] cannot elide %s slot=%d: captures=%u coro=%d\n",
                    target->name ? target->name : "?", slot, (unsigned) target->ncaptures,
                    cg_func_needs_aot_coro_ctx(ctx, target) ? 1 : 0);
        }
        return false;
    }
    const XiModule *owner_module = target->module ? target->module : ctx->module;
    const XiFunc *owner_init = owner_module ? owner_module->init : ctx->module->init;
    bool local_direct =
        cg_shared_static_function_slot_uses_are_direct(ctx, owner_init, slot, target);
    bool imports_direct = cg_imported_static_function_uses_are_direct_in_bundle(ctx, target);
    if (dbg && (!local_direct || !imports_direct)) {
        fprintf(stderr,
                "[xi_cgen][boxed] cannot elide %s slot=%d: local_direct=%d imports_direct=%d "
                "owner=%s module=%s\n",
                target->name ? target->name : "?", slot, local_direct ? 1 : 0,
                imports_direct ? 1 : 0, owner_init && owner_init->name ? owner_init->name : "?",
                owner_module && owner_module->name ? owner_module->name : "?");
    }
    return local_direct && imports_direct;
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
    if (cg_static_direct_function_closure_is_elided(ctx, current, v))
        return true;
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
    if (v->op == XI_PHI) {
        /* Resolve through the function's phi coalescing map so an operand use of
         * a coalesced phi prints the representative's C variable (the only place
         * its declaration exists). emit_phi_ref handles the decl/copy sites. */
        uint32_t id = v->id;
        const XiFunc *vf = v->block ? v->block->func : NULL;
        if (vf && vf->phi_coalesce && id < vf->phi_coalesce_count)
            id = vf->phi_coalesce[id];
        fprintf(out, "phi%u", id);
    } else {
        fprintf(out, "v%u", v->id);
    }
}

#include "xi_cgen_class_native_meta.inc.c"
static void emit_codegen_abort_expr(FILE *out);
static bool emit_struct_aggregate_box_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *value, const char *prefix);
static void emit_value_rhs(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix);
static bool emit_thread_spawn_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const XiValue *v, const char *prefix, bool in_coro);
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

static void emit_generated_line_reset(XiCgenCtx *ctx, FILE *out) {
    (void) ctx;
    if (!out)
        return;
    fprintf(out, "#line 1 \"<xray-generated>\"\n");
}

static void emit_value_generated_line_reset(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (v && v->line > 0)
        emit_generated_line_reset(ctx, out);
}

static void emit_block_terminator_source_line(XiCgenCtx *ctx, FILE *out, const XiBlock *blk) {
    if (!blk)
        return;
    uint32_t line = blk->line;
    if (line == 0 && blk->control)
        line = blk->control->line;
    emit_source_line_directive(ctx, out, line);
}

static void emit_block_terminator_generated_line_reset(XiCgenCtx *ctx, FILE *out,
                                                       const XiBlock *blk) {
    if (!blk)
        return;
    uint32_t line = blk->line;
    if (line == 0 && blk->control)
        line = blk->control->line;
    if (line > 0)
        emit_generated_line_reset(ctx, out);
}

static bool cg_block_is_return_like(const XiBlock *blk) {
    return blk && (blk->kind == XI_BLOCK_RETURN || blk->kind == XI_BLOCK_UNREACHABLE);
}

static void emit_likely_condition_expr(XiCgenCtx *ctx, FILE *out, const XiBlock *blk) {
    if (blk && xi_copy_is_branch_hint(blk->control)) {
        emit_condition_expr_ctx(ctx, out, blk->control);
        return;
    }
    bool true_returns = cg_block_is_return_like(blk ? blk->succs[0] : NULL);
    bool false_returns = cg_block_is_return_like(blk ? blk->succs[1] : NULL);
    if (true_returns == false_returns) {
        emit_condition_expr_ctx(ctx, out, blk->control);
        return;
    }
    fprintf(out, "%s(", true_returns ? "XR_UNLIKELY" : "XR_LIKELY");
    emit_condition_expr_ctx(ctx, out, blk->control);
    fprintf(out, ")");
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

/* Coro non-frame local declaration helpers. A coroutine resume body re-declares
 * every non-frame local up front, so the slot type must match the value's own
 * emission. This uses the planned storage rep (so a native class instance is a
 * PTR slot, matching its `void *`/`Conn *` value), with two deliberate
 * departures from the sync local helpers:
 *   - the array native-local override is dropped: coroutine bodies always emit
 *     arrays boxed (XrValue), never as a raw xrt_array_t* local;
 *   - a unit/void value keeps an XrValue slot, since declaring a `void` local is
 *     illegal C and such slots are never read as values. */
static const char *cg_coro_decl_ctype(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    (void) f;
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    const char *t = (plan && plan->rep.c_type) ? plan->rep.c_type : local_ctype_str(v);
    return (t && strcmp(t, "void") == 0) ? "XrValue" : t;
}

static XrRep cg_coro_decl_rep(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    (void) f;
    XrRep r = cg_value_plan_storage_rep(ctx, v);
    return (r == XR_REP_VOID) ? XR_REP_TAGGED : r;
}

/* Write a phi variable reference: phi<id>, mapped through the coalescing table
 * so phis that share a C variable (same source var_id, disjoint live ranges)
 * resolve to one representative name. */
static void emit_phi_ref(const XiCgenCtx *ctx, FILE *out, const XiPhi *phi) {
    uint32_t id = phi->value.id;
    if (ctx && ctx->phi_repr_active && ctx->phi_repr && id < ctx->phi_repr_cap)
        id = ctx->phi_repr[id];
    fprintf(out, "phi%u", id);
}

/* ========== Value Emission ========== */

static void emit_binop(FILE *out, const XiValue *v, const char *op) {
    emit_vref(out, v->args[0]);
    fprintf(out, " %s ", op);
    emit_vref(out, v->args[1]);
}

/* 122: narrow-width direct lowering for integer arithmetic.
 *
 * AOT carries every integer in the i64 value model, so a sub-word op normally
 * reads as widen(x) -> i64 op -> narrow(result). That is correct, but emits
 * noisy C. When the operands provably fit a width that wraps identically, emit a
 * native (T)(a op b) instead. The gate below only accepts cases where C integer
 * promotion cannot introduce signed overflow and the final cast preserves the
 * same low bits as the i64-wrap path. */
static bool cg_value_narrow_int_rep(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                    uint8_t *out_size, bool *out_signed) {
    if (!v || cg_array_value_uses_native_local(ctx, f, v))
        return false;
    XaotRep rep;
    bool have_rep = false;
    const XaotValuePlan *plan = cg_value_plan(ctx, v);
    if (plan) {
        rep = plan->rep.rep;
        have_rep = true;
    } else {
        uint8_t code = 0;
        if (cg_value_narrow_local_native_width(v, 0, &code) &&
            xaot_rep_from_native_type(code, &rep))
            have_rep = true;
    }
    if (!have_rep)
        return false;
    const XaotRepInfo *info = xaot_rep_info(rep);
    if (!info || !info->is_integer)
        return false;
    if (out_size)
        *out_size = info->size;
    if (out_signed)
        *out_signed = info->is_signed;
    return true;
}

static const XiValue *cg_arith_narrow_src(XiCgenCtx *ctx, const XiFunc *f, const XiValue *o,
                                          uint8_t *out_size, bool *out_signed) {
    if (!o)
        return NULL;
    uint8_t size = 0;
    bool sign = false;
    if (cg_value_narrow_int_rep(ctx, f, o, &size, &sign) && size <= 4) {
        if (out_size)
            *out_size = size;
        if (out_signed)
            *out_signed = sign;
        return o;
    }
    if (xi_to_c_template_width_kind(o->op) == AOT_WIDTH_TEMPLATE_CAST_I64 && o->nargs >= 1 &&
        o->args[0]) {
        const XiValue *src = o->args[0];
        if (cg_value_narrow_int_rep(ctx, f, src, &size, &sign) && size <= 4) {
            if (out_size)
                *out_size = size;
            if (out_signed)
                *out_signed = sign;
            return src;
        }
    }
    return NULL;
}

static bool cg_arith_is_clean_narrow(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!v || v->nargs < 2 || cg_rep(v) != XR_REP_I64)
        return false;
    if (v->op != XI_ADD && v->op != XI_SUB && v->op != XI_MUL)
        return false;
    uint8_t sa = 0, sb = 0;
    bool ga = false, gb = false;
    if (!cg_arith_narrow_src(ctx, f, v->args[0], &sa, &ga) ||
        !cg_arith_narrow_src(ctx, f, v->args[1], &sb, &gb))
        return false;
    if (v->op == XI_MUL) {
        unsigned eff_a = (unsigned) sa * 8u - (ga ? 1u : 0u);
        unsigned eff_b = (unsigned) sb * 8u - (gb ? 1u : 0u);
        if (eff_a + eff_b > 31u)
            return false;
    } else if (sa == 4 || sb == 4) {
        if ((sa == 4 && ga) || (sb == 4 && gb))
            return false;
        uint8_t rsize = 0;
        if (!cg_value_narrow_int_rep(ctx, f, v, &rsize, NULL) || rsize > 4)
            return false;
    }
    return true;
}

static void cg_emit_narrow_arith_operand(XiCgenCtx *ctx, const XiFunc *f, FILE *out,
                                         const XiValue *o) {
    const XiValue *src = cg_arith_narrow_src(ctx, f, o, NULL, NULL);
    emit_vref(out, src ? src : o);
}

static bool cg_widen_elided_into_narrow_arith(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!f || !v || xi_to_c_template_width_kind(v->op) != AOT_WIDTH_TEMPLATE_CAST_I64 ||
        v->nargs < 1 || !v->args[0])
        return false;
    uint8_t size = 0;
    bool sign = false;
    if (!cg_value_narrow_int_rep(ctx, f, v, &size, &sign) || size <= 2)
        return false;
    /* Elision is only sound when every operand use resolves through to
     * args[0] (cg_arith_narrow_src's CAST_I64 look-through branch). A value
     * whose own rep is narrow (size <= 4, e.g. NARROW_U32 with a u32 plan) is
     * returned as-is by the first branch of cg_arith_narrow_src, so eliding
     * its declaration would emit a reference to a C temp that was never
     * declared (repro: `let t: uint32 = seed + 1; return t + SHARED_CONST`
     * where select_rep unboxes the shared const and the add turns clean). */
    if (size <= 4)
        return false;
    if (!cg_arith_narrow_src(ctx, f, v, NULL, NULL))
        return false;
    bool any_user = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == v)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next)
            for (uint16_t k = 0; k < phi->value.nargs; k++)
                if (phi->value.args[k] == v)
                    return false;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *u = blk->values[vi];
            if (!u || u == v)
                continue;
            bool uses_v = false;
            for (uint16_t ai = 0; ai < u->nargs; ai++) {
                if (u->args[ai] == v) {
                    uses_v = true;
                    break;
                }
            }
            if (!uses_v)
                continue;
            if (!cg_arith_is_clean_narrow(ctx, f, u))
                return false;
            any_user = true;
        }
    }
    return any_user;
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

static bool cg_shift_const_int_value(const XiValue *value, int64_t *out) {
    const XiValue *unwrapped = cg_unwrap_identity_value(value);
    if (!unwrapped || unwrapped->op != XI_CONST || !unwrapped->type ||
        unwrapped->type->kind != XR_KIND_INT || !out)
        return false;
    *out = unwrapped->aux_int;
    return true;
}

static bool cg_type_is_unsigned_int(const XrType *type) {
    if (!type || type->kind != XR_KIND_INT || type->is_nullable)
        return false;
    switch (type->native_width) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
            return true;
        default:
            return false;
    }
}

static bool emit_native_unsigned_const_shift_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                  const char *op) {
    if (!v || v->nargs < 2 || cg_rep(v) != XR_REP_I64 || cg_rep(v->args[0]) != XR_REP_I64 ||
        cg_rep(v->args[1]) != XR_REP_I64 || !cg_type_is_unsigned_int(v->args[0]->type))
        return false;

    int64_t shift = 0;
    if (!cg_shift_const_int_value(v->args[1], &shift) || shift < 0 || shift >= 64)
        return false;

    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "XR_FROM_INT((int64_t)(");
    fprintf(out, "(((uint64_t)(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ")) %s UINT64_C(%" PRIu64 "))", op, (uint64_t) shift);
    if (boxed)
        fprintf(out, "))");
    return true;
}

static bool emit_native_nonnegative_const_shr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v) {
    if (!v || v->op != XI_SHR || v->nargs < 2 || cg_rep(v) != XR_REP_I64 ||
        cg_rep(v->args[0]) != XR_REP_I64 || cg_rep(v->args[1]) != XR_REP_I64)
        return false;

    int64_t shift = 0;
    if (!cg_shift_const_int_value(v->args[1], &shift) || shift < 0 || shift >= 64)
        return false;
    if (!xi_value_known_nonnegative_at(f, v->args[0], v->block))
        return false;

    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "XR_FROM_INT(");
    fprintf(out, "(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, " >> INT64_C(%" PRId64 "))", shift);
    if (boxed)
        fprintf(out, ")");
    return true;
}

static bool emit_native_range_safe_const_shl_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || v->op != XI_SHL || v->nargs < 2 || cg_rep(v) != XR_REP_I64 ||
        cg_rep(v->args[0]) != XR_REP_I64 || cg_rep(v->args[1]) != XR_REP_I64)
        return false;

    int64_t shift = 0;
    if (!cg_shift_const_int_value(v->args[1], &shift) || shift < 0 || shift >= 64)
        return false;

    XiRange lhs_range = xi_range_of(v->args[0]);
    if (lhs_range.is_top || lhs_range.is_bot || lhs_range.lo < 0)
        return false;
    if (lhs_range.hi > (INT64_MAX >> shift))
        return false;

    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "XR_FROM_INT(");
    fprintf(out, "(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, " << INT64_C(%" PRId64 "))", shift);
    if (boxed)
        fprintf(out, ")");
    return true;
}

/* Shifts cannot generally use raw C << / >>: out-of-range counts, negative
 * right-shift inputs, and shifting into the sign bit are not portable enough
 * for Xray's defined semantics. Proven nonnegative right shifts with a
 * 0..63 constant count, and left shifts whose range cannot overflow the
 * signed result, are equivalent and can use native C directly; all other
 * cases route through xrt_i64_shl / xrt_i64_shr. */
static void emit_shift_binop_ctx(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const char *fn) {
    if (emit_native_unsigned_const_shift_expr(ctx, out, v, v->op == XI_SHL ? "<<" : ">>"))
        return;
    if (emit_native_nonnegative_const_shr_expr(ctx, out, f, v))
        return;
    if (emit_native_range_safe_const_shl_expr(ctx, out, v))
        return;

    /* Unsigned lhs with a non-constant count: logical shift (matches the
     * VM's OP_SHR_U; the const-count case was handled above). */
    if (v->op == XI_SHR && v->nargs >= 1 && v->args[0] && cg_type_is_unsigned_int(v->args[0]->type))
        fn = "xrt_i64_shr_u";

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
    if (xi_copy_is_branch_hint(v) && v->nargs >= 1 && v->args[0]) {
        fprintf(out, "%s(", v->aux_int == XI_COPY_KIND_LIKELY ? "XR_LIKELY" : "XR_UNLIKELY");
        emit_condition_expr(out, v->args[0]);
        fprintf(out, ")");
        return;
    }
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
    if (xi_copy_is_branch_hint(v) && v->nargs >= 1 && v->args[0]) {
        fprintf(out, "%s(", v->aux_int == XI_COPY_KIND_LIKELY ? "XR_LIKELY" : "XR_UNLIKELY");
        emit_condition_expr_ctx(ctx, out, v->args[0]);
        fprintf(out, ")");
        return;
    }
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

static bool cg_value_aliases_value(const XiValue *value, const XiValue *target) {
    return value && target && cg_unwrap_identity_value(value) == target;
}

static bool cg_ref_alias_uses_noescape(XiCgenCtx *ctx, const XiFunc *f, const XiValue *alias,
                                       uint8_t depth);

static bool cg_ref_noescape_debug_enabled(void) {
    return getenv("XRAY_CGEN_REF_NOESCAPE_DUMP") != NULL;
}

static void cg_ref_noescape_debug_fail(const XiFunc *f, const XiValue *alias, const XiValue *user,
                                       const char *reason) {
    if (!cg_ref_noescape_debug_enabled())
        return;
    fprintf(stderr, "[xi_cgen] ref-noescape fail func=%s alias=v%u",
            f && f->name ? f->name : "<null>", alias ? alias->id : UINT32_MAX);
    if (user)
        fprintf(stderr, " user=v%u/%s", user->id, xi_op_name(user->op));
    fprintf(stderr, " reason=%s\n", reason ? reason : "<unknown>");
}

static bool cg_ref_alias_user_noescape(XiCgenCtx *ctx, const XiFunc *f, const XiValue *user,
                                       uint16_t arg_index, const XiValue *alias, uint8_t depth) {
    if (!ctx || !f || !user || !alias)
        return false;
    switch ((XiOp) user->op) {
        case XI_RELEASE:
        case XI_RETAIN:
            cg_ref_noescape_debug_fail(f, alias, user, "arc op on alias");
            return false;
        case XI_COPY:
        case XI_MOVE:
        case XI_BOX:
        case XI_UNBOX:
        case XI_CHECKTYPE:
            return arg_index == 0 &&
                   cg_ref_alias_uses_noescape(ctx, f, user, (uint8_t) (depth + 1));
        case XI_CALL: {
            if (arg_index == 0)
                return false;
            CgStaticFunctionCall static_call =
                cg_resolve_static_function_call(ctx, f, user->args[0]);
            if (!static_call.func || static_call.is_class_constructor) {
                cg_ref_noescape_debug_fail(f, alias, user, "unknown direct callee");
                return false;
            }
            if (!cg_direct_ref_param_noescape(ctx, static_call.func, (uint16_t) (arg_index - 1),
                                              (uint8_t) (depth + 1))) {
                cg_ref_noescape_debug_fail(f, alias, user, "callee param escapes");
                return false;
            }
            return true;
        }
        default:
            if (xi_own_value_arg_is_consuming(user, arg_index)) {
                cg_ref_noescape_debug_fail(f, alias, user, "consuming use");
                return false;
            }
            return true;
    }
}

static bool cg_ref_alias_uses_noescape(XiCgenCtx *ctx, const XiFunc *f, const XiValue *alias,
                                       uint8_t depth) {
    if (!ctx || !f || !alias || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (cg_value_aliases_value(blk->control, alias)) {
            if (blk->kind == XI_BLOCK_RETURN) {
                cg_ref_noescape_debug_fail(f, alias, blk->control, "returned");
                return false;
            }
        }
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (cg_value_aliases_value(phi->value.args[a], alias)) {
                    cg_ref_noescape_debug_fail(f, alias, &phi->value, "phi merge");
                    return false;
                }
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == alias)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (!cg_value_aliases_value(user->args[a], alias))
                    continue;
                if (!cg_ref_alias_user_noescape(ctx, f, user, a, alias, depth))
                    return false;
            }
        }
    }
    return true;
}

static bool cg_direct_ref_param_noescape(XiCgenCtx *ctx, const XiFunc *target, uint16_t param_index,
                                         uint8_t depth) {
    if (!ctx || !target || !target->params || param_index >= target->nparams || depth > 8)
        return false;
    const XiValue *param = target->params[param_index];
    if (!param || !xi_own_type_is_rc(param->type)) {
        cg_ref_noescape_debug_fail(target, param, NULL, "non-rc param");
        return false;
    }
    return cg_ref_alias_uses_noescape(ctx, target, param, depth);
}

static bool cg_direct_call_param_accepts_borrowed_ref(XiCgenCtx *ctx, const XiFunc *target,
                                                      uint16_t arg_index) {
    if (!ctx || !target)
        return false;
    const XaotFuncPlan *target_plan = cg_func_plan(ctx, target);
    if (!target_plan || arg_index >= target_plan->abi.nparams || !target_plan->abi.params) {
        if (cg_ref_noescape_debug_enabled())
            fprintf(stderr, "[xi_cgen] ref-noescape direct func=%s arg=%u reason=no abi\n",
                    target->name ? target->name : "<null>", arg_index);
        return false;
    }
    XrRep rep = cg_abi_slot_storage_rep(&target_plan->abi.params[arg_index]);
    if (rep != XR_REP_PTR && rep != XR_REP_RAWPTR) {
        if (cg_ref_noescape_debug_enabled())
            fprintf(stderr, "[xi_cgen] ref-noescape direct func=%s arg=%u reason=abi rep %d\n",
                    target->name ? target->name : "<null>", arg_index, (int) rep);
        return false;
    }
    if (target->arc_borrow_sig && target->arc_borrow_sig->valid &&
        arg_index < target->arc_borrow_sig->nparams &&
        target->arc_borrow_sig->param_own[arg_index] == XI_OWN_BORROWED) {
        if (cg_ref_noescape_debug_enabled())
            fprintf(stderr, "[xi_cgen] ref-noescape direct func=%s arg=%u reason=arc borrowed\n",
                    target->name ? target->name : "<null>", arg_index);
        return true;
    }
    bool ok = cg_direct_ref_param_noescape(ctx, target, arg_index, 0);
    if (cg_ref_noescape_debug_enabled())
        fprintf(stderr, "[xi_cgen] ref-noescape direct func=%s arg=%u reason=scan %s\n",
                target->name ? target->name : "<null>", arg_index, ok ? "ok" : "fail");
    return ok;
}

static bool cg_method_receiver_accepts_borrowed_ref(const XiValue *user, uint16_t arg_index) {
    if (!user || arg_index != 0 ||
        (user->op != XI_CALL_METHOD && user->op != XI_CALL_METHOD_DIRECT) || !user->aux)
        return false;
    const char *method = (const char *) user->aux;
    return strcmp(method, "resize") == 0 || strcmp(method, "reserve") == 0 ||
           strcmp(method, "clear") == 0 || strcmp(method, "appendFromUnchecked") == 0 ||
           strcmp(method, "writeFromUnchecked") == 0 ||
           strcmp(method, "wildCopyFromNonOverlappingUnchecked") == 0 ||
           strcmp(method, "wildRepeatAtUnchecked") == 0 ||
           strcmp(method, "setLengthUnchecked") == 0 || strcmp(method, "pushUnchecked") == 0 ||
           strcmp(method, "repeatAtUnchecked") == 0 || strcmp(method, "repeatUnchecked") == 0;
}

static bool cg_borrowed_array_slot_alias_uses_are_borrowed(XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *alias, uint8_t depth);

static bool cg_value_is_array_slot_forwarding_or_arc(const XiValue *v) {
    if (!v)
        return false;
    switch ((XiOp) v->op) {
        case XI_RETAIN:
        case XI_RELEASE:
        case XI_MOVE:
        case XI_BOX:
        case XI_UNBOX:
        case XI_CHECKTYPE:
            return true;
        case XI_COPY:
            return xi_copy_is_identity_alias(v);
        default:
            return false;
    }
}

static const XiValue *cg_unwrap_array_slot_forwarding_or_arc(const XiValue *v) {
    uint8_t depth = 0;
    while (v && v->nargs >= 1 && cg_value_is_array_slot_forwarding_or_arc(v) && depth++ < 8)
        v = v->args[0];
    return v;
}

static bool cg_value_aliases_array_slot_forwarding_or_arc(const XiValue *value,
                                                          const XiValue *target) {
    return value && target && cg_unwrap_array_slot_forwarding_or_arc(value) == target;
}

static bool cg_block_value_index(const XiBlock *blk, const XiValue *value, uint32_t *out_index) {
    if (!blk || !value)
        return false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] == value) {
            if (out_index)
                *out_index = i;
            return true;
        }
    }
    return false;
}

static bool cg_array_slot_direct_await_window_is_clean(const XiValue *alias, const XiValue *await) {
    if (!alias || !await || alias->block != await->block)
        return false;
    const XiBlock *blk = alias->block;
    uint32_t alias_idx = 0;
    uint32_t await_idx = 0;
    if (!cg_block_value_index(blk, alias, &alias_idx) ||
        !cg_block_value_index(blk, await, &await_idx) || alias_idx >= await_idx)
        return false;
    for (uint32_t i = alias_idx + 1; i < await_idx; i++) {
        const XiValue *cur = blk->values[i];
        if (!cur)
            continue;
        bool aliases_slot = false;
        for (uint16_t a = 0; a < cur->nargs; a++) {
            if (cg_value_aliases_array_slot_forwarding_or_arc(cur->args[a], alias)) {
                aliases_slot = true;
                break;
            }
        }
        if (aliases_slot && cg_value_is_array_slot_forwarding_or_arc(cur))
            continue;
        return false;
    }
    return true;
}

static bool cg_await_is_plain_task_borrow_use(const XiValue *user, uint16_t arg_index) {
    if (!user || user->op != XI_AWAIT || arg_index != 0)
        return false;
    int flags = (int) user->aux_int;
    int disallowed = XI_AWAIT_AUX_ANY | XI_AWAIT_AUX_ALL | XI_AWAIT_AUX_ANY_SUCCESS |
                     XI_AWAIT_AUX_AGGREGATE_ONE_SHOT | XI_AWAIT_AUX_INTO_RESULT;
    return (flags & disallowed) == 0;
}

static bool cg_tagged_array_index_get_uses_only_direct_plain_await(const XiFunc *f,
                                                                   const XiValue *alias) {
    if (!f || !alias)
        return false;
    bool saw_await = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (cg_value_aliases_array_slot_forwarding_or_arc(blk->control, alias)) {
            cg_ref_noescape_debug_fail(f, alias, blk->control,
                                       "array slot direct await block control");
            return false;
        }
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (cg_value_aliases_array_slot_forwarding_or_arc(phi->value.args[a], alias)) {
                    cg_ref_noescape_debug_fail(f, alias, &phi->value,
                                               "array slot direct await phi");
                    return false;
                }
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == alias)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (!cg_value_aliases_array_slot_forwarding_or_arc(user->args[a], alias))
                    continue;
                if (cg_await_is_plain_task_borrow_use(user, a)) {
                    if (!cg_array_slot_direct_await_window_is_clean(alias, user)) {
                        cg_ref_noescape_debug_fail(f, alias, user,
                                                   "array slot direct await window not clean");
                        return false;
                    }
                    saw_await = true;
                    continue;
                }
                if (a == 0 && cg_value_is_array_slot_forwarding_or_arc(user))
                    continue;
                cg_ref_noescape_debug_fail(f, alias, user,
                                           "array slot direct await unsupported use");
                return false;
            }
        }
    }
    if (!saw_await)
        cg_ref_noescape_debug_fail(f, alias, NULL, "array slot direct await not found");
    return saw_await;
}

static bool cg_borrowed_array_slot_user_is_borrow(const XiCgenCtx *ctx_ro, const XiFunc *f_ro,
                                                  const XiValue *user, uint16_t arg_index,
                                                  const XiValue *alias, uint8_t depth) {
    XiCgenCtx *ctx = (XiCgenCtx *) ctx_ro;
    const XiFunc *f = f_ro;
    if (!ctx || !f || !user || !alias)
        return false;
    switch ((XiOp) user->op) {
        case XI_RETAIN:
        case XI_RELEASE:
            return arg_index == 0;
        case XI_COPY:
        case XI_MOVE:
        case XI_BOX:
        case XI_UNBOX:
        case XI_CHECKTYPE:
            return arg_index == 0 && cg_borrowed_array_slot_alias_uses_are_borrowed(
                                         ctx, f, user, (uint8_t) (depth + 1));
        case XI_LOAD_FIELD: {
            const char *field = (const char *) user->aux;
            return arg_index == 0 && field &&
                   (strcmp(field, "length") == 0 || strcmp(field, "size") == 0 ||
                    strcmp(field, "capacity") == 0 || strcmp(field, "isEmpty") == 0);
        }
        case XI_INDEX_GET:
        case XI_ARRAY_DATA_PTR:
        case XI_BYTES_LOAD_U16_LE:
        case XI_BYTES_LOAD_U32_LE:
        case XI_BYTES_LOAD_U64_LE:
            return arg_index == 0;
        case XI_CALL: {
            if (arg_index == 0)
                return false;
            CgStaticFunctionCall static_call =
                cg_resolve_static_function_call(ctx, f, user->args[0]);
            bool ok = static_call.func && !static_call.is_class_constructor &&
                      cg_direct_call_param_accepts_borrowed_ref(ctx, static_call.func,
                                                                (uint16_t) (arg_index - 1));
            if (!ok)
                cg_ref_noescape_debug_fail(f, alias, user, "array slot direct call consumes");
            return ok;
        }
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            if (xi_own_value_arg_is_consuming(user, arg_index) &&
                !cg_method_receiver_accepts_borrowed_ref(user, arg_index)) {
                cg_ref_noescape_debug_fail(f, alias, user, "array slot method consumes");
                return false;
            }
            return true;
        default:
            if (xi_own_value_arg_is_consuming(user, arg_index)) {
                cg_ref_noescape_debug_fail(f, alias, user, "array slot unsupported consuming use");
                return false;
            }
            return true;
    }
}

static bool cg_borrowed_array_slot_alias_uses_are_borrowed(XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *alias, uint8_t depth) {
    if (!ctx || !f || !alias || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (cg_value_aliases_value(blk->control, alias))
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (cg_value_aliases_value(phi->value.args[a], alias)) {
                    cg_ref_noescape_debug_fail(f, alias, &phi->value, "array slot phi");
                    return false;
                }
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == alias)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (!cg_value_aliases_value(user->args[a], alias))
                    continue;
                if (!cg_borrowed_array_slot_user_is_borrow(ctx, f, user, a, alias, depth))
                    return false;
            }
        }
    }
    return true;
}

static bool cg_tagged_array_index_get_can_borrow(XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *v) {
    CgArrayElemInfo info;
    if (!ctx || !f || !v || v->op != XI_INDEX_GET || v->nargs < 2)
        return false;
    if (cg_value_plan_storage_rep(ctx, v) != XR_REP_TAGGED) {
        cg_ref_noescape_debug_fail(f, v, NULL, "array slot borrow non-tagged value rep");
        return false;
    }
    if (!cg_array_value_storage_info(ctx, f, v->args[0], &info, CG_ARRAY_STORAGE_READ)) {
        cg_ref_noescape_debug_fail(f, v, NULL, "array slot borrow no array storage");
        return false;
    }
    if (info.rep != XR_REP_TAGGED) {
        cg_ref_noescape_debug_fail(f, v, NULL, "array slot borrow non-tagged element rep");
        return false;
    }
    bool direct_plain_await = cg_tagged_array_index_get_uses_only_direct_plain_await(f, v);
    if (direct_plain_await && xi_value_type_is_task(v))
        return true;
    if (!xi_own_type_is_rc(v->type)) {
        cg_ref_noescape_debug_fail(f, v, NULL,
                                   direct_plain_await ? "array slot direct await non-task type"
                                                      : "array slot borrow non-rc type");
        return false;
    }
    if (!cg_array_index_access_bounds_proven(ctx, f, v))
        return false;
    return cg_borrowed_array_slot_alias_uses_are_borrowed(ctx, f, v, 0);
}

static bool cg_value_is_borrowed_array_slot_alias(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *v) {
    const XiValue *origin = cg_unwrap_identity_value(v);
    return origin && origin->op == XI_INDEX_GET &&
           cg_tagged_array_index_get_can_borrow(ctx, f, origin);
}

/* Emit the RHS expression for a single value. */
static void emit_value_rhs(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix) {
    XR_DCHECK(ctx != NULL, "emit_value_rhs: NULL ctx");
    XR_DCHECK(v != NULL, "emit_value_rhs: NULL value");

    if (xi_op_is_coroutine(v->op)) {
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

/* Run this function's pending defers at an exit point. The IR lowers every
 * `defer` into a zero-arg closure pushed onto a stack-local XrtDeferScope at the
 * defer site (xicgen_stmt_defer); here we unlink that scope and run it LIFO.
 * Doing this dynamically — rather than statically unrolling each defer site at
 * every exit — is what makes loops, conditional registration, and early returns
 * run exactly the defers that executed, matching the VM. The panic path is
 * handled by xrt_throw_exc (xrt_defer_unwind_to); ordering and Go-style error
 * replacement live in the runtime (xrt_defer.h). */
static void emit_deferred_calls(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    (void) ctx;
    (void) prefix;
    if (cg_func_has_defer_stmt(f))
        fprintf(out, "    xrt_defer_leave(&_xrt_ds);\n");
}

static void emit_cell_var_releases(XiCgenCtx *ctx, FILE *out) {
    if (!ctx || !ctx->cell_release_vars)
        return;
    for (uint32_t var_id = 0; var_id < ctx->cell_var_count; var_id++) {
        if (!ctx->cell_release_vars[var_id])
            continue;
        fprintf(out, "    xrt_release(");
        emit_cell_ref(out, (XiVarId) var_id);
        fprintf(out, ");\n");
    }
}

static void emit_default_return_for_abi(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    if (cg_func_return_abi_is_aggregate(ctx, f)) {
        emit_aggregate_zero_expr(out, cg_func_return_abi_value_rep(ctx, f));
        return;
    }
    XrRep ret_rep = cg_func_return_abi_rep(ctx, f);
    if (ret_rep == XR_REP_TAGGED)
        fprintf(out, "XR_NULL_VAL");
    else if (ret_rep == XR_REP_PTR || ret_rep == XR_REP_RAWPTR)
        fprintf(out, "NULL");
    else
        fprintf(out, "0");
}

static void emit_default_return_stmt_for_abi(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    if (cg_func_return_abi_rep(ctx, f) == XR_REP_VOID) {
        fprintf(out, "    return;\n");
        return;
    }
    fprintf(out, "    return ");
    emit_default_return_for_abi(ctx, out, f);
    fprintf(out, ";\n");
}

static void emit_fallthrough_return(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                    const char *prefix) {
    emit_class_field_cache_flush(ctx, out);
    emit_deferred_calls(ctx, out, f, prefix);
    emit_cell_var_releases(ctx, out);
    if (cg_func_return_abi_rep(ctx, f) == XR_REP_VOID) {
        fprintf(out, "    return;\n");
    } else {
        emit_default_return_stmt_for_abi(ctx, out, f);
    }
}

static bool func_needs_fallthrough_return(const XiFunc *f) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk || blk->kind != XI_BLOCK_PLAIN)
            continue;
        if (!blk->succs[0])
            return true;
    }
    return false;
}

#include "xi_cgen_stmt_dispatch_helpers.inc.c"

typedef struct CgDebugSourceVarInfo {
    const char *name;
    const char *ctype;
    char ctype_buf[160];
    XrRep rep;
    XiVarId var_id;
    uint32_t shadow_index;
} CgDebugSourceVarInfo;

static bool cg_debug_is_ident_start(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static bool cg_debug_is_ident_part(char ch) {
    return cg_debug_is_ident_start(ch) || (ch >= '0' && ch <= '9');
}

static bool cg_debug_name_has_digit_suffix(const char *name, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n) != 0)
        return false;
    if (name[n] == '\0')
        return false;
    for (const char *p = name + n; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
    }
    return true;
}

static bool cg_debug_source_name_is_c_keyword(const char *name) {
    static const char *keywords[] = {
        "auto",     "break",      "case",     "char",   "const",        "continue", "default",
        "do",       "double",     "else",     "enum",   "extern",       "float",    "for",
        "goto",     "if",         "inline",   "int",    "long",         "register", "restrict",
        "return",   "short",      "signed",   "sizeof", "static",       "struct",   "switch",
        "typedef",  "union",      "unsigned", "void",   "volatile",     "while",    "_Bool",
        "_Complex", "_Imaginary", "bool",     "true",   "false",        "null",     "NULL",
        "XrValue",  "int64_t",    "uint64_t", "size_t", "xrt_closure_t"};
    size_t n = sizeof(keywords) / sizeof(keywords[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(name, keywords[i]) == 0)
            return true;
    }
    return false;
}

static bool cg_debug_source_name_is_safe(const char *name) {
    if (!name || !name[0])
        return false;
    if (name[0] == '_')
        return false;
    if (!cg_debug_is_ident_start(name[0]))
        return false;
    for (const char *p = name + 1; *p; p++) {
        if (!cg_debug_is_ident_part(*p))
            return false;
    }
    if (cg_debug_source_name_is_c_keyword(name))
        return false;
    if (cg_debug_name_has_digit_suffix(name, "v") || cg_debug_name_has_digit_suffix(name, "p") ||
        cg_debug_name_has_digit_suffix(name, "phi") ||
        cg_debug_name_has_digit_suffix(name, "cell_"))
        return false;
    if (strcmp(name, "ctx") == 0 || strcmp(name, "f") == 0 || strcmp(name, "_cl") == 0)
        return false;
    return true;
}

static bool cg_debug_source_var_name_is_usable(const XiFunc *f, XiVarId var_id) {
    if (!f || !xi_var_id_is_valid(var_id) || var_id >= f->source_var_count || !f->source_var_names)
        return false;
    const char *name = f->source_var_names[var_id];
    if (!cg_debug_source_name_is_safe(name))
        return false;
    if (f->name && strcmp(f->name, name) == 0)
        return false;
    return true;
}

static uint32_t cg_debug_source_var_shadow_index(const XiFunc *f, XiVarId var_id) {
    if (!cg_debug_source_var_name_is_usable(f, var_id))
        return 0;
    const char *name = f->source_var_names[var_id];
    uint32_t shadow_index = 0;
    for (uint32_t i = 0; i < (uint32_t) var_id; i++) {
        if (f->source_var_names[i] && strcmp(f->source_var_names[i], name) == 0)
            shadow_index++;
    }
    return shadow_index;
}

static const XaotValuePlan *cg_debug_value_plan(XiCgenCtx *ctx, const XiValue *v) {
    if (!ctx || !v)
        return NULL;
    return xaot_bundle_find_value_plan(ctx->aot_bundle, v);
}

static const char *cg_debug_value_ctype(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (cg_array_value_uses_native_local(ctx, f, v))
        return "xrt_array_t *";
    const XaotValuePlan *plan = cg_debug_value_plan(ctx, v);
    if (plan && plan->rep.c_type)
        return plan->rep.c_type;
    return local_ctype_str(v);
}

static XrRep cg_debug_value_decl_storage_rep(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (cg_array_value_uses_native_local(ctx, f, v))
        return XR_REP_PTR;
    const XaotValuePlan *plan = cg_debug_value_plan(ctx, v);
    return plan ? xaot_value_storage_rep(plan->rep) : XR_REP_VOID;
}

static const XrStructLayout *cg_debug_type_struct_layout(const XrType *type) {
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE) ||
        !type->instance.class_ref)
        return NULL;
    return type->instance.class_ref->struct_layout;
}

static const XrStructLayout *cg_debug_value_struct_layout(XiCgenCtx *ctx, const XiFunc *f,
                                                          const XiValue *v) {
    if (!v)
        return NULL;

    const XiValue *cur = v;
    for (int depth = 0; cur && depth <= 8; depth++) {
        if (cur->op == XI_STRUCT_NEW)
            return (const XrStructLayout *) cur->aux;
        if ((cur->op == XI_COPY || cur->op == XI_MOVE || cur->op == XI_RETAIN) && cur->nargs >= 1) {
            cur = cur->args[0];
            continue;
        }
        break;
    }

    const XrStructLayout *shared_layout = NULL;
    if (cg_value_traces_to_heap_struct_shared(ctx, f, v, &shared_layout, NULL))
        return shared_layout;

    return cg_debug_type_struct_layout(v->type);
}

static bool cg_debug_value_struct_ptr_ctype(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                            char *buf, size_t buflen) {
    if (!buf || buflen == 0)
        return false;
    const XrStructLayout *sl = cg_debug_value_struct_layout(ctx, f, v);
    if (!cg_struct_native_heap_supported(sl))
        return false;
    char type_name[128];
    cg_struct_heap_type_name(type_name, sizeof(type_name),
                             ctx && ctx->module ? ctx->module->name : NULL, sl);
    snprintf(buf, buflen, "%s *", type_name);
    return true;
}

static bool cg_debug_value_source_decl_info(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                            char *ctype_buf, size_t ctype_buf_len,
                                            const char **out_ctype, XrRep *out_rep) {
    if (!v || !out_ctype || !out_rep)
        return false;

    const char *ctype = cg_debug_value_ctype(ctx, f, v);
    XrRep rep = cg_debug_value_decl_storage_rep(ctx, f, v);

    /*
     * Heap-native structs are useful as typed pointers in debug locals, but
     * value-ABI structs are real C aggregates. Declaring an aggregate local as
     * a pointer makes -g builds emit invalid assignments such as `totals = v0`.
     */
    if (!cg_value_plan_is_struct_aggregate(ctx, v) &&
        cg_debug_value_struct_ptr_ctype(ctx, f, v, ctype_buf, ctype_buf_len)) {
        ctype = ctype_buf;
        rep = XR_REP_PTR;
    }

    if (!ctype || strcmp(ctype, "void") == 0 || rep == XR_REP_VOID)
        return false;

    *out_ctype = ctype;
    *out_rep = rep;
    return true;
}

static const XiValue *cg_debug_source_var_storage_value(const XiFunc *f, const XiValue *v) {
    if (!v)
        return NULL;
    const XiValue *slot_value = NULL;
    if (v->op == XI_AWAIT)
        slot_value = xi_coro_typed_await_unbox_user(f, v);
    if (!slot_value)
        slot_value = xi_coro_typed_recv_unbox_user(f, v);
    return slot_value ? slot_value : v;
}

static bool cg_debug_value_has_storage_for_source(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *v) {
    if (!v)
        return false;
    if (cg_is_void_like(v) || v->op == XI_TRY || v->op == XI_END_TRY)
        return false;
    if (cg_value_is_elided_i64_optional_blocking_result(f, v))
        return false;
    if (xicgen_slice_value_only_used_by_stack_slice_direct_call(ctx, f, v))
        return false;
    if (!cg_debug_value_plan(ctx, v))
        return false;
    if (v->op == XI_PHI) {
        const XiPhi *phi = (const XiPhi *) v;
        if (!cg_phi_has_storage(phi))
            return false;
        if (cg_value_traces_to_inlined_struct(f, v) ||
            cg_value_is_elided_heap_struct_alias(ctx, f, v))
            return false;
        return true;
    }
    if (v->op == XI_STRUCT_NEW && cg_struct_can_inline(f, v))
        return false;
    if ((v->op == XI_COPY || v->op == XI_MOVE) && (cg_value_traces_to_inlined_struct(f, v) ||
                                                   cg_value_is_elided_heap_struct_alias(ctx, f, v)))
        return false;
    if (cg_value_traces_to_inlined_struct(f, v) ||
        cg_value_is_elided_heap_struct_alias(ctx, f, v) ||
        cg_value_is_elided_nested_struct_ref(f, v) || cg_value_is_elided_fixed_array_ref(f, v) ||
        cg_value_is_elided_layout_struct_type_load(f, v))
        return false;
    if (cg_ownership_op_is_noop(v) || cg_shared_static_function_ownership_is_noop(ctx, f, v))
        return false;
    if (cg_shared_static_function_value_is_elided(ctx, f, v) ||
        cg_class_descriptor_value_is_elided(ctx, f, v) ||
        xicgen_box_only_feeds_native_int_print(ctx, f, v) ||
        cg_class_native_value_stmt_is_elided(ctx, f, v) ||
        cg_class_native_ctor_can_inline(ctx, f, v) ||
        cg_class_shared_native_ctor_value_is_elided(ctx, f, v, NULL) ||
        cg_class_shared_native_set_is_elided(ctx, f, v) ||
        cg_class_shared_native_value_is_elided(ctx, f, v) ||
        cg_array_class_field_alloc_value_is_elided(ctx, f, v) ||
        cg_array_class_field_value_is_elided(ctx, f, v) ||
        cg_class_native_map_field_value_is_elided(ctx, f, v) ||
        cg_class_native_set_field_value_is_elided(ctx, f, v) ||
        cg_class_native_map_method_call_value_is_elided(ctx, f, v) ||
        cg_class_native_set_method_call_value_is_elided(ctx, f, v) ||
        cg_class_native_ref_stack_return_takes_value(ctx, f, v) ||
        cg_array_typed_push_value_is_elided(ctx, f, v) ||
        cg_class_native_array_method_call_value_is_elided(ctx, f, v) ||
        cg_value_is_dead_aot_marker(ctx, f, v))
        return false;
    if (v->op == XI_GET_SHARED && cg_value_only_used_by_layout_struct_new(f, v))
        return false;
    return true;
}

static bool cg_debug_value_has_source_storage(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!v || !xi_var_id_is_valid(v->var_id))
        return false;
    if (!cg_debug_source_var_name_is_usable(f, v->var_id))
        return false;
    return cg_debug_value_has_storage_for_source(ctx, f, cg_debug_source_var_storage_value(f, v));
}

static bool cg_debug_source_var_storage_info(XiCgenCtx *ctx, const XiFunc *f, XiVarId var_id,
                                             CgDebugSourceVarInfo *out_info) {
    if (!out_info || !cg_debug_source_var_name_is_usable(f, var_id))
        return false;

    const char *ctype = NULL;
    XrRep rep = XR_REP_VOID;
    bool found = false;

    for (uint16_t i = 0; i < f->nparams; i++) {
        const XiValue *v = f->params ? f->params[i] : NULL;
        if (!v || v->var_id != var_id || !cg_debug_value_has_source_storage(ctx, f, v))
            continue;
        const XiValue *storage_v = cg_debug_source_var_storage_value(f, v);
        char cur_ctype_buf[160];
        const char *cur_ctype = NULL;
        XrRep cur_rep = XR_REP_VOID;
        if (!cg_debug_value_source_decl_info(ctx, f, storage_v, cur_ctype_buf,
                                             sizeof(cur_ctype_buf), &cur_ctype, &cur_rep))
            return false;
        if (!found) {
            snprintf(out_info->ctype_buf, sizeof(out_info->ctype_buf), "%s", cur_ctype);
            ctype = out_info->ctype_buf;
            rep = cur_rep;
            found = true;
        } else if (strcmp(ctype, cur_ctype) != 0 || rep != cur_rep) {
            return false;
        }
    }

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            const XiValue *v = &phi->value;
            if (v->var_id != var_id || !cg_debug_value_has_source_storage(ctx, f, v))
                continue;
            const XiValue *storage_v = cg_debug_source_var_storage_value(f, v);
            char cur_ctype_buf[160];
            const char *cur_ctype = NULL;
            XrRep cur_rep = XR_REP_VOID;
            if (!cg_debug_value_source_decl_info(ctx, f, storage_v, cur_ctype_buf,
                                                 sizeof(cur_ctype_buf), &cur_ctype, &cur_rep))
                return false;
            if (!found) {
                snprintf(out_info->ctype_buf, sizeof(out_info->ctype_buf), "%s", cur_ctype);
                ctype = out_info->ctype_buf;
                rep = cur_rep;
                found = true;
            } else if (strcmp(ctype, cur_ctype) != 0 || rep != cur_rep) {
                return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->var_id != var_id || !cg_debug_value_has_source_storage(ctx, f, v))
                continue;
            const XiValue *storage_v = cg_debug_source_var_storage_value(f, v);
            char cur_ctype_buf[160];
            const char *cur_ctype = NULL;
            XrRep cur_rep = XR_REP_VOID;
            if (!cg_debug_value_source_decl_info(ctx, f, storage_v, cur_ctype_buf,
                                                 sizeof(cur_ctype_buf), &cur_ctype, &cur_rep))
                return false;
            if (!found) {
                snprintf(out_info->ctype_buf, sizeof(out_info->ctype_buf), "%s", cur_ctype);
                ctype = out_info->ctype_buf;
                rep = cur_rep;
                found = true;
            } else if (strcmp(ctype, cur_ctype) != 0 || rep != cur_rep) {
                return false;
            }
        }
    }

    if (!found)
        return false;
    out_info->name = f->source_var_names[var_id];
    out_info->ctype = ctype;
    out_info->rep = rep;
    out_info->var_id = var_id;
    out_info->shadow_index = cg_debug_source_var_shadow_index(f, var_id);
    return true;
}

static void emit_debug_source_var_c_name(FILE *out, const CgDebugSourceVarInfo *info) {
    if (!info)
        return;
    if (info->shadow_index == 0) {
        fprintf(out, "%s", info->name);
        return;
    }
    fprintf(out, "_xray_dbg_shadow_%u_%s", (unsigned) info->shadow_index, info->name);
}

static void emit_debug_source_var_declarations(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    if (!f || f->source_var_count == 0 || !f->source_var_names)
        return;

    bool emitted_guard = false;
    for (uint32_t i = 0; i < f->source_var_count; i++) {
        CgDebugSourceVarInfo info;
        if (!cg_debug_source_var_storage_info(ctx, f, (XiVarId) i, &info))
            continue;
        if (!emitted_guard) {
            fprintf(out, "#if defined(XRAY_AOT_DEBUG_LOCALS)\n");
            emitted_guard = true;
        }
        fprintf(out, "    %s ", info.ctype);
        emit_debug_source_var_c_name(out, &info);
        fprintf(out, " = ");
        if (strcmp(info.ctype, "XrAotAdtValue") == 0)
            fprintf(out, "xrt_adt_value_zero()");
        else if (strncmp(info.ctype, "xrt_struct_", 11) == 0)
            fprintf(out, "((%s){0})", info.ctype);
        else if (info.rep == XR_REP_TAGGED)
            fprintf(out, "XR_NULL_VAL");
        else
            fprintf(out, "0");
        fprintf(out, ";\n");
    }
    if (emitted_guard)
        fprintf(out, "#endif\n");
}

static void emit_debug_source_var_sync(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                       const XiValue *v) {
    if (!cg_debug_value_has_source_storage(ctx, f, v))
        return;
    const XiValue *storage_v = cg_debug_source_var_storage_value(f, v);
    CgDebugSourceVarInfo info;
    if (!cg_debug_source_var_storage_info(ctx, f, v->var_id, &info))
        return;

    fprintf(out, "#if defined(XRAY_AOT_DEBUG_LOCALS)\n");
    fprintf(out, "    ");
    emit_debug_source_var_c_name(out, &info);
    fprintf(out, " = ");
    if (cg_value_plan_is_struct_aggregate(ctx, storage_v)) {
        emit_vref(out, storage_v);
    } else if (strcmp(info.ctype, "XrAotAdtValue") == 0) {
        if (cg_value_plan_is_aggregate(ctx, storage_v)) {
            emit_vref(out, storage_v);
        } else {
            fprintf(out, "xrt_adt_value_from_boxed(");
            emit_value_as_rep_ctx(ctx, out, storage_v, XR_REP_TAGGED);
            fprintf(out, ")");
        }
    } else if (strcmp(info.ctype, "XrValue") == 0) {
        emit_value_as_rep_ctx(ctx, out, storage_v, info.rep);
    } else {
        fprintf(out, "(%s)", info.ctype);
        emit_value_as_rep_ctx(ctx, out, storage_v, info.rep);
    }
    fprintf(out, ";\n");
    fprintf(out, "#endif\n");
}

#define CG_INLINE_AWAIT_ALL_TASK_MAX 4

typedef struct CgInlineAwaitAllLiteral {
    const XiValue *array;
    const XiValue *tasks[CG_INLINE_AWAIT_ALL_TASK_MAX];
    uint32_t count;
} CgInlineAwaitAllLiteral;

typedef struct CgAwaitAllScalarResult {
    const XiValue *index_values[CG_INLINE_AWAIT_ALL_TASK_MAX];
    uint32_t count;
} CgAwaitAllScalarResult;

static bool cg_inline_await_all_value_traces_to_array(const XiValue *value, const XiValue *array);

static bool cg_await_all_value_is_array_push(const XiValue *v, const XiValue *array,
                                             const XiValue **out_task) {
    if (!v || !array)
        return false;
    const XiValue *task = NULL;
    if (v->op == XI_ARRAY_PUSH && v->nargs >= 2 &&
        cg_inline_await_all_value_traces_to_array(v->args[0], array)) {
        task = v->args[1];
    } else if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) && v->nargs >= 2 &&
               cg_inline_await_all_value_traces_to_array(v->args[0], array)) {
        const char *method = (const char *) v->aux;
        if (method && (strcmp(method, "push") == 0 || strcmp(method, "pushUnchecked") == 0))
            task = v->args[1];
    }
    if (!task)
        return false;
    if (out_task)
        *out_task = task;
    return true;
}

static bool cg_inline_await_all_value_is_array_new(const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_ARRAY_NEW)
        return true;
    return v->op == XI_STACK_ALLOC && v->aux_int == XI_ARRAY_NEW;
}

static bool cg_inline_await_all_value_traces_to_array(const XiValue *value, const XiValue *array) {
    return value && array && cg_unwrap_identity_value(value) == array;
}

static bool cg_inline_await_all_uses_array_arg(const XiValue *v, const XiValue *array) {
    if (!v || !array)
        return false;
    for (uint16_t i = 0; i < v->nargs; i++) {
        if (cg_inline_await_all_value_traces_to_array(v->args[i], array))
            return true;
    }
    return false;
}

static bool cg_await_all_inline_literal_collect(const XiFunc *f, const XiValue *await,
                                                CgInlineAwaitAllLiteral *out) {
    if (!f || !await || await->op != XI_AWAIT || await->nargs < 1 ||
        (await->aux_int & XI_AWAIT_AUX_ALL) == 0 ||
        (await->aux_int & XI_AWAIT_AUX_AGGREGATE_ONE_SHOT) == 0)
        return false;
    if ((await->aux_int & (XI_AWAIT_AUX_ANY | XI_AWAIT_AUX_ANY_SUCCESS)) != 0)
        return false;

    const XiValue *array = cg_unwrap_identity_value(await->args[0]);
    if (!cg_inline_await_all_value_is_array_new(array) || array->nargs < 1)
        return false;

    int64_t capacity = 0;
    if (!cg_const_int_value(array->args[0], &capacity) || capacity < 0 ||
        capacity > CG_INLINE_AWAIT_ALL_TASK_MAX)
        return false;

    CgInlineAwaitAllLiteral lit;
    memset(&lit, 0, sizeof(lit));
    lit.array = array;
    lit.count = capacity > 0 ? (uint32_t) capacity : 0;

    const XiBlock *await_block = await->block;
    uint32_t await_index = UINT32_MAX;
    if (await_block) {
        for (uint32_t i = 0; i < await_block->nvalues; i++) {
            if (await_block->values[i] == await) {
                await_index = i;
                break;
            }
        }
    }

    bool saw_await = false;
    bool saw_push = false;
    bool saw_index_set = false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *v = blk->values[i];
            if (!v || !cg_inline_await_all_uses_array_arg(v, array))
                continue;
            if (v == await) {
                saw_await = true;
                continue;
            }
            if (v->nargs == 1 && (v->op == XI_BOX || v->op == XI_UNBOX ||
                                  xi_copy_is_identity_alias(v) || v->op == XI_MOVE))
                continue;
            if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1 &&
                cg_inline_await_all_value_traces_to_array(v->args[0], array))
                continue;
            if (v->op == XI_INDEX_SET && v->nargs >= 3 &&
                cg_inline_await_all_value_traces_to_array(v->args[0], array)) {
                if (saw_push || capacity <= 0)
                    return false;
                saw_index_set = true;
                int64_t index = 0;
                if (!cg_const_int_value(v->args[1], &index) || index < 0 || index >= capacity)
                    return false;
                if (lit.tasks[index])
                    return false;
                lit.tasks[index] = v->args[2];
                continue;
            }
            const XiValue *pushed_task = NULL;
            if (cg_await_all_value_is_array_push(v, array, &pushed_task)) {
                if (saw_index_set || !await_block || blk != await_block || i >= await_index ||
                    lit.count >= CG_INLINE_AWAIT_ALL_TASK_MAX)
                    return false;
                saw_push = true;
                lit.tasks[lit.count++] = pushed_task;
                continue;
            }
            return false;
        }
    }

    if (!saw_await)
        return false;
    if (lit.count == 0)
        return false;
    for (uint32_t i = 0; i < lit.count; i++) {
        if (!lit.tasks[i])
            return false;
    }
    if (out)
        *out = lit;
    return true;
}

static bool cg_await_all_inline_literal_array_is_elided(const XiFunc *f, const XiValue *array) {
    array = cg_unwrap_identity_value(array);
    if (!f || !cg_inline_await_all_value_is_array_new(array))
        return false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *v = blk->values[i];
            if (v && v->op == XI_AWAIT && v->nargs >= 1 &&
                cg_inline_await_all_value_traces_to_array(v->args[0], array) &&
                cg_await_all_inline_literal_collect(f, v, NULL))
                return true;
        }
    }
    return false;
}

static bool cg_await_all_inline_literal_value_is_elided(const XiFunc *f, const XiValue *v) {
    if (!f || !v)
        return false;
    const XiValue *root = cg_unwrap_identity_value(v);
    if (cg_inline_await_all_value_is_array_new(root))
        return cg_await_all_inline_literal_array_is_elided(f, root);
    if (v->op == XI_INDEX_SET && v->nargs >= 3 && v->args[0])
        return cg_await_all_inline_literal_array_is_elided(f, v->args[0]);
    if (v->op == XI_ARRAY_PUSH && v->nargs >= 2 && v->args[0])
        return cg_await_all_inline_literal_array_is_elided(f, v->args[0]);
    if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) && v->nargs >= 2 &&
        v->args[0]) {
        const char *method = (const char *) v->aux;
        if (method && (strcmp(method, "push") == 0 || strcmp(method, "pushUnchecked") == 0))
            return cg_await_all_inline_literal_array_is_elided(f, v->args[0]);
    }
    if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1 && v->args[0])
        return cg_await_all_inline_literal_array_is_elided(f, v->args[0]);
    return false;
}

static void emit_await_all_inline_task_vector(XiCgenCtx *ctx, FILE *out,
                                              const CgInlineAwaitAllLiteral *literal,
                                              uint32_t await_id, const char *name_prefix) {
    XR_DCHECK(literal != NULL, "emit_await_all_inline_task_vector: NULL literal");
    fprintf(out, "    XrValue %s%u[%u] = {", name_prefix, await_id, literal->count);
    for (uint32_t i = 0; i < literal->count; i++) {
        if (i > 0)
            fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, literal->tasks[i], XR_REP_TAGGED);
    }
    fprintf(out, "};\n");
}

static bool cg_value_traces_to_value_through_identity(const XiValue *value, const XiValue *target) {
    const XiValue *cur = value;
    for (uint8_t depth = 0; cur && depth < 8; depth++) {
        if (cur == target)
            return true;
        if (cur->nargs == 1 && (cur->op == XI_BOX || cur->op == XI_UNBOX ||
                                xi_copy_is_identity_alias(cur) || cur->op == XI_MOVE)) {
            cur = cur->args[0];
            continue;
        }
        return false;
    }
    return false;
}

static bool cg_await_all_index_get_from_await(const XiValue *v, const XiValue *await,
                                              int64_t *out_index) {
    if (!v || v->op != XI_INDEX_GET || v->nargs < 2)
        return false;
    if (!cg_value_traces_to_value_through_identity(v->args[0], await))
        return false;
    return cg_const_int_value(v->args[1], out_index);
}

static bool cg_await_all_scalar_result_allowed_user(const XiValue *user, uint16_t arg_index,
                                                    const XiValue *await,
                                                    CgAwaitAllScalarResult *out) {
    if (!user || !await)
        return false;

    int64_t index = 0;
    if (arg_index == 0 && cg_await_all_index_get_from_await(user, await, &index)) {
        if (!out || index < 0 || index >= (int64_t) out->count)
            return false;
        if (out->index_values[index] && out->index_values[index] != user)
            return false;
        out->index_values[index] = user;
        return true;
    }

    if (arg_index == 0 && user->nargs == 1 &&
        (user->op == XI_BOX || user->op == XI_UNBOX || xi_copy_is_identity_alias(user) ||
         user->op == XI_MOVE))
        return true;

    if (arg_index == 0 && user->nargs >= 1 && (user->op == XI_RETAIN || user->op == XI_RELEASE))
        return true;

    return false;
}

static bool cg_await_all_inline_scalar_result_collect(const XiFunc *f, const XiValue *await,
                                                      CgAwaitAllScalarResult *out) {
    CgInlineAwaitAllLiteral literal;
    if (!cg_await_all_inline_literal_collect(f, await, &literal))
        return false;

    CgAwaitAllScalarResult scalar;
    memset(&scalar, 0, sizeof(scalar));
    scalar.count = literal.count;

    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        if (cg_value_traces_to_value_through_identity(blk->control, await))
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (cg_value_traces_to_value_through_identity(phi->value.args[a], await))
                    return false;
            }
        }
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *user = blk->values[i];
            if (!user || user == await)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (!cg_value_traces_to_value_through_identity(user->args[a], await))
                    continue;
                if (!cg_await_all_scalar_result_allowed_user(user, a, await, &scalar))
                    return false;
            }
        }
    }

    for (uint32_t i = 0; i < scalar.count; i++) {
        if (!scalar.index_values[i])
            return false;
    }
    if (out)
        *out = scalar;
    return true;
}

static const XiValue *cg_find_scalarized_inline_await_all_for_value(const XiFunc *f,
                                                                    const XiValue *value) {
    if (!f || !value)
        return NULL;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *await = blk->values[i];
            if (!await || await->op != XI_AWAIT)
                continue;
            CgAwaitAllScalarResult scalar;
            if (!cg_await_all_inline_scalar_result_collect(f, await, &scalar))
                continue;
            for (uint32_t j = 0; j < scalar.count; j++) {
                if (scalar.index_values[j] == value)
                    return await;
            }
            if (value->nargs >= 1 &&
                cg_value_traces_to_value_through_identity(value->args[0], await) &&
                (value->op == XI_BOX || value->op == XI_UNBOX || xi_copy_is_identity_alias(value) ||
                 value->op == XI_MOVE || value->op == XI_RETAIN || value->op == XI_RELEASE))
                return await;
        }
    }
    return NULL;
}

static bool cg_await_all_scalar_result_value_is_elided(const XiFunc *f, const XiValue *v) {
    return cg_find_scalarized_inline_await_all_for_value(f, v) != NULL;
}

static bool cg_native_box_direct_call_arg_is_native(XiCgenCtx *ctx, const XiFunc *f,
                                                    const XiValue *call, uint16_t arg_index) {
    if (!ctx || !call || call->op != XI_CALL || arg_index == 0 || !call->args)
        return false;
    if (cg_func_needs_aot_coro_ctx(ctx, f))
        return false;

    CgStaticFunctionCall static_call = cg_resolve_static_function_call(ctx, f, call->args[0]);
    if (!static_call.func || static_call.is_class_constructor)
        return false;

    const XaotFuncPlan *target_plan = cg_func_plan(ctx, static_call.func);
    uint16_t param_index = (uint16_t) (arg_index - 1);
    if (!target_plan || param_index >= target_plan->abi.nparams || !target_plan->abi.params)
        return false;

    return cg_abi_slot_storage_rep(&target_plan->abi.params[param_index]) != XR_REP_TAGGED;
}

static bool cg_call_method_is_typed_array_resize_zero_specialization(XiCgenCtx *ctx,
                                                                     const XiFunc *f,
                                                                     const XiValue *call) {
    if (!ctx || !call || call->op != XI_CALL_METHOD || call->nargs < 2 || call->nargs > 3 ||
        !call->aux || strcmp((const char *) call->aux, "resize") != 0)
        return false;

    const XiValue *len = cg_unwrap_identity_value(call->args[1]);
    if (!len || len->op != XI_CONST || !len->type || len->type->kind != XR_KIND_INT ||
        len->aux_int != 0)
        return false;

    CgArrayElemInfo info;
    return cg_array_value_storage_info(ctx, f, call->args[0], &info, CG_ARRAY_STORAGE_MUTABLE) &&
           cg_array_value_has_fresh_owned_origin(ctx, call->args[0]);
}

static bool cg_native_box_use_consumes_native_rep(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *user, uint16_t arg_index) {
    if (!ctx || !user)
        return false;

    CgArrayElemInfo info;
    switch ((XiOp) user->op) {
        case XI_INDEX_GET:
            return arg_index == 1 && user->nargs >= 2 &&
                   cg_array_value_storage_info(ctx, f, user->args[0], &info, CG_ARRAY_STORAGE_READ);
        case XI_INDEX_SET:
            if (arg_index == 1)
                return user->nargs >= 3 && cg_array_value_storage_info(ctx, f, user->args[0], &info,
                                                                       CG_ARRAY_STORAGE_MUTABLE);
            if (arg_index == 2 && user->nargs >= 3 &&
                cg_array_value_storage_info(ctx, f, user->args[0], &info, CG_ARRAY_STORAGE_MUTABLE))
                return info.rep != XR_REP_TAGGED;
            return false;
        case XI_SLICE:
            return (arg_index == 1 || arg_index == 2) &&
                   xicgen_slice_value_only_used_by_stack_slice_direct_call(ctx, f, user);
        case XI_CALL_METHOD:
            return arg_index >= 1 &&
                   cg_call_method_is_typed_array_resize_zero_specialization(ctx, f, user);
        case XI_CALL:
            return cg_native_box_direct_call_arg_is_native(ctx, f, user, arg_index);
        default:
            return false;
    }
}

static bool cg_native_box_value_is_elided_in_aot(XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *v) {
    if (!ctx || !f || cg_value_has_cell(ctx, v))
        return false;
    if (cg_func_needs_aot_coro_ctx(ctx, f))
        return false;
    if (v->flags &
        (XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND))
        return false;
    if (!cg_value_box_inner_native_rep(ctx, v, NULL))
        return false;

    bool seen_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == v)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == v)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == v)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != v)
                    continue;
                seen_use = true;
                if (!cg_native_box_use_consumes_native_rep(ctx, f, user, a))
                    return false;
            }
        }
    }
    return seen_use;
}

static bool cg_pure_value_only_feeds_aot_elided_values(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *v) {
    if (!ctx || !f || !v || cg_value_has_cell(ctx, v))
        return false;
    if (cg_func_needs_aot_coro_ctx(ctx, f))
        return false;
    if (v->flags &
        (XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND))
        return false;

    switch ((XiOp) v->op) {
        case XI_CONST:
        case XI_COPY:
        case XI_MOVE:
            break;
        default:
            return false;
    }

    bool seen_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == v)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == v)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == v)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != v)
                    continue;
                seen_use = true;
                if (!cg_native_box_value_is_elided_in_aot(ctx, f, user))
                    return false;
            }
        }
    }
    return seen_use;
}

/* Emit a complete value statement: type vN = <rhs>; */
static void emit_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    XR_DCHECK(v != NULL, "emit_value_stmt: NULL value");
    emit_value_source_line(ctx, out, v);

    /* defer registers its closure onto the function's defer scope at this site
     * (xicgen_stmt_defer); the scope runs LIFO at exit. */
    if (v->op == XI_DEFER) {
        xi_to_c_emit_stmt_generated(ctx, out, f, v, prefix);
        return;
    }

    if (cg_widen_elided_into_narrow_arith(ctx, f, v))
        return;
    if (xicgen_slice_value_only_used_by_stack_slice_direct_call(ctx, f, v))
        return;
    if (cg_await_all_inline_literal_value_is_elided(f, v))
        return;
    if (cg_await_all_scalar_result_value_is_elided(f, v))
        return;
    if (cg_native_box_value_is_elided_in_aot(ctx, f, v))
        return;
    if (cg_pure_value_only_feeds_aot_elided_values(ctx, f, v))
        return;

    /* Inlined struct: emit local anonymous C struct with native fields. */
    if (v->op == XI_STRUCT_NEW && cg_struct_can_inline(f, v)) {
        XrStructLayout *sl = (XrStructLayout *) v->aux;
        XR_DCHECK(sl != NULL, "inlined XI_STRUCT_NEW: missing layout");
        fprintf(out, "    struct { ");
        for (uint16_t i = 0; i < sl->field_count; i++) {
            char fname[128];
            cg_struct_field_c_name(sl, i, fname, sizeof(fname));
            emit_struct_field_decl(out, sl, i, fname, prefix);
            fprintf(out, "; ");
        }
        fprintf(out, "} _st%u = {0};\n", v->id);
        emit_value_generated_line_reset(ctx, out, v);
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
         cg_value_is_elided_fixed_array_ref(f, v->args[0]) ||
         cg_value_is_elided_layout_struct_type_load(f, v) ||
         cg_value_is_borrowed_array_slot_alias(ctx, f, v->args[0]) ||
         xicgen_slice_value_only_used_by_stack_slice_direct_call(ctx, f, v->args[0])))
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
        emit_value_generated_line_reset(ctx, out, v);
        return;
    }
    if (v && v->uses == 0 && cg_array_call_is_unchecked_bytes_trusted_nothrow(ctx, f, v)) {
        fprintf(out, "    ");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
        emit_value_generated_line_reset(ctx, out, v);
        return;
    }
    if (emit_class_native_array_method_call_stmt(ctx, out, f, v))
        return;

    if (emit_class_native_ctor_value_stmt(ctx, out, f, prefix, v))
        return;

    if (emit_thread_spawn_value_stmt(ctx, out, f, v, prefix, false))
        return;

    if ((v->op == XI_GET_SHARED && cg_value_only_used_by_layout_struct_new(f, v)) ||
        cg_value_is_elided_heap_struct_alias(ctx, f, v))
        return;

    if (cg_array_closure_value_only_used_by_inline_map(ctx, f, prefix, v) ||
        xicgen_par_for_stack_closure_value_is_elided(ctx, f, v) ||
        cg_value_is_dead_aot_marker(ctx, f, v))
        return;

    if (emit_typed_array_class_field_alloc_store_stmt(ctx, out, f, v))
        return;

    if (xi_to_c_emit_stmt_generated(ctx, out, f, v, prefix))
        return;

    bool void_like = cg_is_void_like(v);

    if (void_like) {
        fprintf(out, "    (void)(");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
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
    emit_value_generated_line_reset(ctx, out, v);
    emit_debug_source_var_sync(ctx, out, f, v);
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
static bool cg_block_has_defer_run_to(const XiBlock *blk) {
    if (!blk)
        return false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *v = blk->values[i];
        if (v && v->op == XI_DEFER_RUN_TO)
            return true;
    }
    return false;
}

static bool cg_phi_copy_should_emit(XiCgenCtx *ctx, const XiFunc *f, const XiPhi *phi,
                                    uint16_t pred_idx) {
    if (!phi || !cg_phi_has_storage(phi))
        return false;
    if (cg_value_traces_to_inlined_struct(f, &phi->value))
        return false;
    if (!cg_func_needs_aot_coro_ctx(ctx, f) &&
        cg_value_is_elided_heap_struct_alias(ctx, f, &phi->value))
        return false;
    return pred_idx < phi->value.nargs && phi->value.args[pred_idx] != NULL;
}

static void emit_phi_incoming_as_rep(XiCgenCtx *ctx, FILE *out, const XiPhi *phi, uint16_t pred_idx,
                                     bool pred_ran_defer) {
    const XiValue *incoming =
        (phi && pred_idx < phi->value.nargs) ? phi->value.args[pred_idx] : NULL;
    if (pred_ran_defer && cg_value_has_cell(ctx, &phi->value)) {
        char cell_expr[64];
        snprintf(cell_expr, sizeof(cell_expr), "cell_%u", (unsigned) phi->value.var_id);
        emit_cell_get_for_rep(out, &phi->value, cell_expr);
        return;
    }
    if (cg_value_plan_is_aggregate(ctx, &phi->value)) {
        if (cg_value_plan_is_aggregate(ctx, incoming)) {
            emit_vref(out, incoming);
            return;
        }
        if (ctx) {
            const XiFunc *vf = incoming && incoming->block ? incoming->block->func : NULL;
            fprintf(stderr,
                    "[xi_cgen] ERROR: aggregate PHI v%u incoming v%u (%s in %s) is not "
                    "aggregate\n",
                    (unsigned) phi->value.id, incoming ? (unsigned) incoming->id : 0,
                    incoming ? xi_op_name((XiOp) incoming->op) : "?",
                    vf && vf->name ? vf->name : "?");
            ctx->error = true;
        }
        emit_codegen_abort_expr(out);
        return;
    }
    emit_value_as_rep_ctx(ctx, out, incoming, cg_value_plan_storage_rep(ctx, &phi->value));
}

static void emit_phi_tmp_ref(FILE *out, const XiBlock *target, const XiPhi *phi,
                             uint16_t pred_idx) {
    fprintf(out, "_phi_tmp_b%u_p%u_%u", target ? target->id : 0u, (unsigned) pred_idx,
            phi ? phi->value.id : 0u);
}

static void emit_phi_copies(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiBlock *target,
                            uint16_t pred_idx) {
    if (!target)
        return;
    const XiBlock *pred = (pred_idx < target->npreds) ? target->preds[pred_idx] : NULL;
    bool pred_ran_defer = cg_block_has_defer_run_to(pred);

    /* PHI edge updates are parallel assignments. Read every incoming value before
     * writing any target phi so loops like (ip, anchor) = (ip + 1, anchor), and
     * coalesced phi representatives, cannot clobber one another. */
    bool any = false;
    for (const XiPhi *phi = target->phis; phi; phi = phi->next) {
        if (!cg_phi_copy_should_emit(ctx, f, phi, pred_idx))
            continue;
        any = true;
        fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, &phi->value));
        emit_phi_tmp_ref(out, target, phi, pred_idx);
        fprintf(out, " = ");
        emit_phi_incoming_as_rep(ctx, out, phi, pred_idx, pred_ran_defer);
        fprintf(out, ";\n");
    }
    if (!any)
        return;

    for (const XiPhi *phi = target->phis; phi; phi = phi->next) {
        if (!cg_phi_copy_should_emit(ctx, f, phi, pred_idx))
            continue;
        fprintf(out, "    ");
        emit_phi_ref(ctx, out, phi);
        fprintf(out, " = ");
        emit_phi_tmp_ref(out, target, phi, pred_idx);
        fprintf(out, ";\n");
        emit_debug_source_var_sync(ctx, out, f, &phi->value);
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

static bool cg_edge_is_backedge(const XiBlock *from, const XiBlock *to) {
    return from && to && from->rpo > 0 && to->rpo > 0 && to->rpo <= from->rpo;
}

static bool cg_block_has_backedge(const XiBlock *blk) {
    if (!blk)
        return false;
    switch (blk->kind) {
        case XI_BLOCK_PLAIN:
            return cg_edge_is_backedge(blk, blk->succs[0]);
        case XI_BLOCK_IF:
            return cg_edge_is_backedge(blk, blk->succs[0]) ||
                   cg_edge_is_backedge(blk, blk->succs[1]);
        default:
            return false;
    }
}

static bool cg_sync_backedge_heartbeat_enabled(XiCgenCtx *ctx, const XiFunc *f) {
    return ctx && f && cg_func_needs_sync_backedge_heartbeat_ctx(ctx, f);
}

static bool cg_func_emits_sync_backedge_heartbeat(XiCgenCtx *ctx, XiFunc *f) {
    if (!cg_sync_backedge_heartbeat_enabled(ctx, f))
        return false;
    xi_ensure_rpo(f);
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (cg_block_has_backedge(f->blocks[bi]))
            return true;
    }
    return false;
}

static void emit_sync_backedge_heartbeat_stmt(FILE *out, const char *indent) {
    fprintf(out,
            "%sif (XR_UNLIKELY(++_xr_aot_sync_backedge_count >= "
            "XR_AOT_LOOP_POLL_INTERVAL)) {\n",
            indent);
    fprintf(out, "%s    _xr_aot_sync_backedge_count = 0;\n", indent);
    fprintf(out, "%s    xr_aot_sync_backedge_heartbeat();\n", indent);
    fprintf(out, "%s}\n", indent);
}

static void emit_sync_backedge_heartbeat_if_edge(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                 const XiBlock *from, const XiBlock *to,
                                                 const char *indent) {
    if (cg_sync_backedge_heartbeat_enabled(ctx, f) && cg_edge_is_backedge(from, to))
        emit_sync_backedge_heartbeat_stmt(out, indent);
}

#include "xi_cgen_loop_helpers.inc.c"

/* ========== Block Emission ========== */

static bool cg_value_terminates_c_path(const XiValue *v) {
    return v && (v->op == XI_ERR_RETURN || v->op == XI_THROW);
}

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
        if (cg_value_terminates_c_path(v))
            return;
    }

    /* Terminator */
    switch (blk->kind) {
        case XI_BLOCK_RETURN: {
            if (blk->control && blk->control->op == XI_ERR_RETURN)
                break;
            emit_block_terminator_source_line(ctx, out, blk);
            emit_class_field_cache_flush(ctx, out);
            emit_deferred_calls(ctx, out, f, prefix);
            if (emit_class_native_ref_stack_return_stmt(ctx, out, f, blk, prefix))
                break;
            if (emit_class_native_return_stmt(ctx, out, f, blk))
                break;
            emit_cell_var_releases(ctx, out);
            if (blk->control) {
                XrRep ret_rep = cg_func_return_abi_rep(ctx, f);
                if (ret_rep == XR_REP_VOID) {
                    fprintf(out, "    return;\n");
                } else if (cg_func_return_abi_is_aggregate(ctx, f)) {
                    fprintf(out, "    return ");
                    if (cg_value_plan_is_aggregate(ctx, blk->control)) {
                        emit_vref(out, blk->control);
                    } else if (cg_func_return_abi_is_struct_aggregate(ctx, f)) {
                        fprintf(stderr,
                                "[xi_cgen] ERROR: struct aggregate return from non-aggregate v%u\n",
                                blk->control ? blk->control->id : 0);
                        ctx->error = true;
                        emit_aggregate_zero_expr(out, cg_func_return_abi_value_rep(ctx, f));
                    } else {
                        fprintf(out, "xrt_adt_value_from_boxed(");
                        emit_value_as_rep_ctx(ctx, out, blk->control, XR_REP_TAGGED);
                        fprintf(out, ")");
                    }
                    fprintf(out, ";\n");
                } else {
                    fprintf(out, "    return ");
                    emit_value_as_rep_ctx(ctx, out, blk->control, ret_rep);
                    fprintf(out, ";\n");
                }
            } else {
                if (cg_func_return_abi_is_aggregate(ctx, f)) {
                    fprintf(out, "    return ");
                    emit_aggregate_zero_expr(out, cg_func_return_abi_value_rep(ctx, f));
                    fprintf(out, ";\n");
                } else if (cg_func_return_abi_rep(ctx, f) == XR_REP_VOID)
                    fprintf(out, "    return;\n");
                else if (cg_func_return_abi_rep(ctx, f) == XR_REP_TAGGED)
                    fprintf(out, "    return XR_NULL_VAL;\n");
                else
                    fprintf(out, "    return 0;\n");
            }
            emit_block_terminator_generated_line_reset(ctx, out, blk);
            break;
        }

        case XI_BLOCK_PLAIN:
            if (blk->succs[0]) {
                if (emit_structured_counted_loop_stmt(ctx, out, f, blk, prefix))
                    break;
                emit_phi_copies(ctx, out, f, blk->succs[0], find_pred_idx(blk->succs[0], blk));
                emit_sync_backedge_heartbeat_if_edge(ctx, out, f, blk, blk->succs[0], "    ");
                fprintf(out, "    goto L%u;\n", blk->succs[0]->id);
            }
            break;

        case XI_BLOCK_IF:
            XR_DCHECK(blk->control != NULL, "IF block missing control");
            XR_DCHECK(blk->succs[0] != NULL, "IF block missing then");
            XR_DCHECK(blk->succs[1] != NULL, "IF block missing else");
            emit_block_terminator_source_line(ctx, out, blk);
            if (emit_structured_array_fill_loop_stmt(ctx, out, f, blk, prefix))
                break;
            if (emit_bool_accumulate_diamond_stmt(ctx, out, blk))
                break;
            /* Emit phi copies for both branches */
            fprintf(out, "    if (");
            emit_likely_condition_expr(ctx, out, blk);
            fprintf(out, ") {\n");
            emit_phi_copies(ctx, out, f, blk->succs[0], find_pred_idx(blk->succs[0], blk));
            emit_sync_backedge_heartbeat_if_edge(ctx, out, f, blk, blk->succs[0], "        ");
            fprintf(out, "        goto L%u;\n", blk->succs[0]->id);
            fprintf(out, "    } else {\n");
            emit_phi_copies(ctx, out, f, blk->succs[1], find_pred_idx(blk->succs[1], blk));
            emit_sync_backedge_heartbeat_if_edge(ctx, out, f, blk, blk->succs[1], "        ");
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
    if (xicgen_slice_value_only_used_by_stack_slice_direct_call(ctx, f, v))
        return true;
    if (cg_await_all_inline_literal_value_is_elided(f, v))
        return true;
    if (cg_await_all_scalar_result_value_is_elided(f, v))
        return true;
    if (cg_native_box_value_is_elided_in_aot(ctx, f, v))
        return true;
    if (cg_pure_value_only_feeds_aot_elided_values(ctx, f, v))
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
    if (cg_class_native_array_method_call_value_is_elided(ctx, f, v))
        return true;
    if ((v->op == XI_GET_SHARED && cg_value_only_used_by_layout_struct_new(f, v)) ||
        cg_value_is_elided_heap_struct_alias(ctx, f, v))
        return true;
    if (xicgen_par_for_stack_closure_value_is_elided(ctx, f, v))
        return true;
    return false;
}

/* ========== Phi Coalescing ========== */

#define CG_PHI_COALESCE_MAX 256

/* A phi eligible to share a C variable with another phi: any non-tagged local
 * (the declaration the int64-phi audit counts) that gets an ordinary declaration
 * (not an inlined-struct / heap-alias phi). A source var_id is deliberately NOT
 * required: loop-lowered induction phis can carry no var_id at all, and merge
 * correctness rests on the liveness non-interference test plus an exact declared
 * C-type match (see cg_build_phi_coalesce), not on any source-variable identity. */
static bool cg_phi_coalesce_candidate(XiCgenCtx *ctx, const XiFunc *f, const XiPhi *phi) {
    const XiValue *v = &phi->value;
    if (cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED)
        return false;
    if (cg_value_traces_to_inlined_struct(f, v) || cg_value_is_elided_heap_struct_alias(ctx, f, v))
        return false;
    return true;
}

/* Two phis interfere if their live ranges overlap: same defining block (both
 * live from block entry), or one is live at the other's definition point
 * (Chaitin's live-at-def test against per-block liveness). */
static bool cg_phis_interfere(const XiLiveness *l, const XiPhi *a, const XiPhi *b) {
    const XiValue *va = &a->value;
    const XiValue *vb = &b->value;
    if (va->block == vb->block)
        return true;
    if (vb->block && xi_is_live_in(l, vb->block, va))
        return true;
    if (va->block && xi_is_live_in(l, va->block, vb))
        return true;
    return false;
}

/* Build the per-function phi coalescing map: phis that share the exact same
 * declared C type and that provably never interfere are merged onto one
 * representative C variable. Liveness-based non-interference is the correctness
 * guarantee; it generalizes the VM's var_id register coalescing (e.g. the
 * induction variables of two sequential loops collapse to one local) to any
 * pair of disjoint-lifetime phis. The exact C-type match is required because a
 * coalesce class shares one declaration: the coarse storage rep is too weak
 * (int32_t and int64_t both map to XR_REP_I64 yet differ in width/wraparound).
 * On any allocation/liveness failure the map stays identity (no coalescing). */
static void cg_build_phi_coalesce(XiCgenCtx *ctx, XiFunc *f) {
    ctx->phi_repr_active = false;
    if (f) {
        f->phi_coalesce = NULL;
        f->phi_coalesce_count = 0;
    }
    if (!f || f->nblocks == 0)
        return;

    uint32_t max_id = 0;
    bool any = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (phi->value.id > max_id)
                max_id = phi->value.id;
            any = true;
        }
    }
    if (!any)
        return;

    uint32_t need = max_id + 1;
    if (need > ctx->phi_repr_cap) {
        uint32_t *grown = (uint32_t *) xr_realloc(ctx->phi_repr, (size_t) need * sizeof(*grown));
        if (!grown)
            return;
        ctx->phi_repr = grown;
        ctx->phi_repr_cap = need;
    }
    for (uint32_t i = 0; i < need; i++)
        ctx->phi_repr[i] = i;

    xi_ensure_rpo(f);
    XiLiveness *live = xi_compute_liveness(f);
    if (!live)
        return;

    const XiPhi *reps[CG_PHI_COALESCE_MAX];
    int nreps = 0;
    bool merged_any = false;
    bool dbg = getenv("XRAY_DBG_PHI_COALESCE") != NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (dbg)
                fprintf(stderr, "[phi-coalesce] phi%u ctype=%s cand=%d blk=%u\n", phi->value.id,
                        local_ctype_str_ctx(ctx, f, &phi->value),
                        (int) cg_phi_coalesce_candidate(ctx, f, phi),
                        phi->value.block ? phi->value.block->id : 9999u);
            if (!cg_phi_coalesce_candidate(ctx, f, phi))
                continue;
            int join = -1;
            for (int r = 0; r < nreps && join < 0; r++) {
                const XiPhi *rep = reps[r];
                /* A coalesce class shares one C declaration, so every member must
                 * have the identical declared C type. Storage rep is too coarse
                 * (i32 and i64 both map to XR_REP_I64 yet differ in width). */
                if (strcmp(local_ctype_str_ctx(ctx, f, &rep->value),
                           local_ctype_str_ctx(ctx, f, &phi->value)) != 0)
                    continue;
                /* phi may join rep's class only if it interferes with no member
                 * already mapped to rep (rep included via identity). */
                bool ok = true;
                for (uint32_t bj = 0; bj < f->nblocks && ok; bj++) {
                    const XiBlock *b2 = f->blocks[bj];
                    if (!b2)
                        continue;
                    for (const XiPhi *m = b2->phis; m; m = m->next) {
                        if (m->value.id >= need || ctx->phi_repr[m->value.id] != rep->value.id)
                            continue;
                        if (cg_phis_interfere(live, phi, m)) {
                            ok = false;
                            break;
                        }
                    }
                }
                if (ok)
                    join = r;
            }
            if (join >= 0) {
                ctx->phi_repr[phi->value.id] = reps[join]->value.id;
                merged_any = true;
            } else if (nreps < CG_PHI_COALESCE_MAX) {
                reps[nreps++] = phi;
            }
        }
    }

    xi_liveness_free(live);
    ctx->phi_repr_active = merged_any;
    if (merged_any) {
        /* Publish a non-owning view so emit_vref (which has no ctx) can resolve
         * coalesced phi operands via v->block->func. Valid for this function's
         * emission window; the ctx buffer only grows, so it never moves before
         * this function finishes emitting. */
        f->phi_coalesce = ctx->phi_repr;
        f->phi_coalesce_count = need;
    }
}

static int cg_block_emit_index(const XiFunc *f, const XiBlock *needle) {
    if (!f || !needle)
        return -1;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (f->blocks[bi] == needle)
            return (int) bi;
    }
    return -1;
}

static bool cg_value_defined_after_use_block(const XiFunc *f, const XiValue *value,
                                             const XiBlock *use_block) {
    if (!f || !value || !use_block || !value->block)
        return false;
    if (value->op == XI_PHI)
        return false;
    int def_idx = cg_block_emit_index(f, value->block);
    int use_idx = cg_block_emit_index(f, use_block);
    return def_idx >= 0 && use_idx >= 0 && def_idx > use_idx;
}

static bool cg_value_args_have_forward_c_use(const XiFunc *f, const XiBlock *use_block,
                                             const XiValue *user) {
    if (!f || !use_block || !user)
        return false;
    for (uint16_t ai = 0; ai < user->nargs; ai++) {
        if (cg_value_defined_after_use_block(f, user->args[ai], use_block))
            return true;
    }
    return false;
}

static bool cg_has_forward_c_value_use(const XiFunc *f) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (cg_value_defined_after_use_block(f, blk->control, blk))
            return true;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            if (cg_value_args_have_forward_c_use(f, blk, blk->values[vi]))
                return true;
        }
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs && ai < blk->npreds; ai++) {
                if (cg_value_defined_after_use_block(f, phi->value.args[ai], blk->preds[ai]))
                    return true;
            }
        }
    }
    return false;
}

static bool cg_needs_predecl_all(const XiFunc *f) {
    return cg_has_exception_handling(f) || cg_has_forward_c_value_use(f);
}

/* Collect all values and phis to declare at function top.
 * Functions with exception handling or CFG emission-order forward uses
 * pre-declare SSA values to avoid jumping over C declarations. */
static void emit_declarations(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    bool pre_decl_all = ctx->pre_decl_all;

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
            if (!cg_phi_has_storage(phi))
                continue;
            if (cg_value_traces_to_inlined_struct(f, &phi->value))
                continue;
            if (cg_value_is_elided_heap_struct_alias(ctx, f, &phi->value))
                continue;
            /* Coalesced non-representative phis share the representative's C
             * variable; the representative emits the single declaration. */
            if (ctx->phi_repr_active && phi->value.id < ctx->phi_repr_cap &&
                ctx->phi_repr[phi->value.id] != phi->value.id)
                continue;
            XrRep rep = cg_value_plan_storage_rep(ctx, &phi->value);
            fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, &phi->value));
            emit_phi_ref(ctx, out, phi);
            if (cg_value_plan_is_aggregate(ctx, &phi->value)) {
                fprintf(out, " = ");
                emit_value_plan_zero_expr(ctx, out, &phi->value);
                fprintf(out, ";\n");
            } else if (rep == XR_REP_TAGGED)
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
                if (cg_value_plan_is_aggregate(ctx, v)) {
                    fprintf(out, " = ");
                    emit_value_plan_zero_expr(ctx, out, v);
                    fprintf(out, ";\n");
                } else if (rep == XR_REP_TAGGED)
                    fprintf(out, " = XR_NULL_VAL;\n");
                else
                    fprintf(out, " = 0;\n");
            }
        }
    }

    /* Map has/get probe fusion: pre-declare the shared slot-index temp for each
     * fusable has so its single probe result is visible at the guarded get. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && cg_map_fusable_get_for_has(ctx, v))
                fprintf(out, "    int64_t _mf%u = 0;\n", v->id);
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

static bool cg_debug_boxed_adapter_enabled(void);

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
            bool dbg = cg_debug_boxed_adapter_enabled();
            if (cg_shared_static_function_closure_is_elided(ctx, owner, v))
                continue;
            if (!cg_func_needs_aot_coro_ctx(ctx, owner) &&
                xicgen_par_for_stack_closure_value_is_elided(ctx, owner, v))
                continue;
            if (!cg_array_closure_value_only_used_by_inline_map(ctx, owner, prefix, v)) {
                if (dbg) {
                    fprintf(stderr,
                            "[xi_cgen][boxed] unelided closure target=%s owner=%s v%u op=%s "
                            "owner_coro=%d shared_elided=%d par_elided=%d inline_map=%d\n",
                            target->name ? target->name : "?", owner->name ? owner->name : "?",
                            (unsigned) v->id, xi_op_name((XiOp) v->op),
                            cg_func_needs_aot_coro_ctx(ctx, owner) ? 1 : 0,
                            cg_shared_static_function_closure_is_elided(ctx, owner, v) ? 1 : 0,
                            xicgen_par_for_stack_closure_value_is_elided(ctx, owner, v) ? 1 : 0,
                            cg_array_closure_value_only_used_by_inline_map(ctx, owner, prefix, v)
                                ? 1
                                : 0);
                }
                return true;
            }
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

static void emit_cfn_stub_signature(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                    const char *prefix) {
    fprintf(out, "%s%s ", cg_linkage(ctx), cg_cfn_value_c_type(f->return_type, true));
    emit_cfn_stub_fname(ctx, out, prefix, f);
    fprintf(out, "(");
    if (f->nparams == 0) {
        fprintf(out, "void");
    } else {
        for (uint16_t i = 0; i < f->nparams; i++) {
            if (i > 0)
                fprintf(out, ", ");
            const XrType *pt = f->params && f->params[i] ? f->params[i]->type : NULL;
            fprintf(out, "%s p%u", cg_cfn_value_c_type(pt, false), i);
        }
    }
    fprintf(out, ")");
}

static void emit_cfn_c_param_storage_expr(FILE *out, const XrType *type, uint16_t index) {
    if (type && type->kind == XR_KIND_POINTER) {
        fprintf(out, "p%u", index);
    } else if (type && type->kind == XR_KIND_FLOAT) {
        fprintf(out, "(double)p%u", index);
    } else {
        fprintf(out, "(int64_t)p%u", index);
    }
}

static void emit_cfn_target_call_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                      const char *prefix) {
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "(NULL");
    for (uint16_t i = 0; i < f->nparams; i++) {
        const XrType *pt = f->params && f->params[i] ? f->params[i]->type : NULL;
        XrRep from_rep = cg_cfn_value_storage_rep(pt, false);
        XrRep to_rep = cg_func_param_abi_rep(ctx, f, i);
        const char *suffix;

        fprintf(out, ", ");
        suffix = emit_conversion_prefix(out, pt, from_rep, to_rep);
        emit_cfn_c_param_storage_expr(out, pt, i);
        emit_conversion_suffix(out, suffix);
    }
    fprintf(out, ")");
}

static void emit_cfn_stub_definition(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                     const char *prefix) {
    const XrType *ret_type;
    const char *ret_c_type;
    XrRep ret_rep;
    XrRep c_ret_rep;

    if (!cg_func_can_have_cfn_stub(ctx, f))
        return;

    emit_cfn_stub_signature(ctx, out, f, prefix);
    fprintf(out, " {\n");

    ret_type = f->return_type;
    if (!ret_type || ret_type->kind == XR_KIND_UNIT) {
        fprintf(out, "    ");
        emit_cfn_target_call_expr(ctx, out, f, prefix);
        fprintf(out, ";\n");
        fprintf(out, "}\n\n");
        return;
    }

    ret_c_type = cg_cfn_value_c_type(ret_type, true);
    ret_rep = cg_func_return_abi_rep(ctx, f);
    c_ret_rep = cg_cfn_value_storage_rep(ret_type, true);

    fprintf(out, "    return ");
    if (ret_type->kind == XR_KIND_POINTER) {
        if (ret_rep == XR_REP_PTR || ret_rep == XR_REP_RAWPTR) {
            fprintf(out, "(%s)", ret_c_type);
            emit_cfn_target_call_expr(ctx, out, f, prefix);
        } else {
            fprintf(out, "(%s)(uintptr_t)(", ret_c_type);
            emit_cfn_target_call_expr(ctx, out, f, prefix);
            fprintf(out, ")");
        }
    } else {
        const char *suffix;
        fprintf(out, "(%s)(", ret_c_type);
        suffix = emit_conversion_prefix(out, ret_type, ret_rep, c_ret_rep);
        emit_cfn_target_call_expr(ctx, out, f, prefix);
        emit_conversion_suffix(out, suffix);
        fprintf(out, ")");
    }
    fprintf(out, ";\n");
    fprintf(out, "}\n\n");
}

static bool cg_func_can_have_c_export_stub(XiCgenCtx *ctx, const XiFunc *f) {
    if (!f || !f->c_export || !f->c_export_symbol || !f->c_export_symbol[0] || f->is_extern ||
        !cg_cfn_func_has_module_level_storage(ctx, f) || f->ncaptures > 0 ||
        cg_func_needs_aot_coro_ctx(ctx, f))
        return false;
    return cg_cfn_xray_func_signature_supported(f);
}

static void emit_c_export_stub_signature(FILE *out, const XiFunc *f) {
    fprintf(out, "%s %s(", cg_cfn_value_c_type(f->return_type, true), f->c_export_symbol);
    if (f->nparams == 0) {
        fprintf(out, "void");
    } else {
        for (uint16_t i = 0; i < f->nparams; i++) {
            if (i > 0)
                fprintf(out, ", ");
            const XrType *pt = f->params && f->params[i] ? f->params[i]->type : NULL;
            fprintf(out, "%s p%u", cg_cfn_value_c_type(pt, false), i);
        }
    }
    fprintf(out, ")");
}

static void emit_c_export_header_func(XiCgenCtx *ctx, FILE *out, const XiFunc *f, uint32_t *count) {
    if (!f)
        return;
    if (f->c_export) {
        if (!cg_func_can_have_c_export_stub(ctx, f)) {
            fprintf(stderr,
                    "[xi_cgen] ERROR: @c_export function '%s' must be a top-level "
                    "noncapturing non-coroutine function with a supported C ABI signature\n",
                    f->name ? f->name : "<anonymous>");
            ctx->error = true;
            return;
        }
        emit_c_export_stub_signature(out, f);
        fprintf(out, ";\n");
        if (count)
            (*count)++;
    }
    for (uint16_t i = 0; i < f->nchildren; i++)
        emit_c_export_header_func(ctx, out, f->children[i], count);
}

XR_FUNC void xi_cgen_c_export_header(XiCgenCtx *ctx, FILE *out, struct XiModule **modules,
                                     int nmodules, const char *guard) {
    const char *header_guard = (guard && guard[0]) ? guard : "XRAY_AOT_C_EXPORTS_H";
    uint32_t count = 0;

    XR_DCHECK(ctx != NULL, "xi_cgen_c_export_header: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_c_export_header: NULL out");

    fprintf(out, "#ifndef %s\n", header_guard);
    fprintf(out, "#define %s\n\n", header_guard);
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <stddef.h>\n\n");
    fprintf(out, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");

    for (int i = 0; i < nmodules; i++) {
        if (!modules || !modules[i] || !modules[i]->init)
            continue;
        emit_c_export_header_func(ctx, out, modules[i]->init, &count);
    }
    if (count == 0)
        fprintf(out, "/* No @c_export symbols. */\n");

    fprintf(out, "\n#ifdef __cplusplus\n}\n#endif\n\n");
    fprintf(out, "#endif /* %s */\n", header_guard);
}

static void emit_c_export_stub_definition(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const char *prefix) {
    const XrType *ret_type;
    const char *ret_c_type;
    XrRep ret_rep;
    XrRep c_ret_rep;

    if (!f || !f->c_export)
        return;
    if (!cg_func_can_have_c_export_stub(ctx, f)) {
        fprintf(stderr,
                "[xi_cgen] ERROR: @c_export function '%s' must be a top-level noncapturing "
                "non-coroutine function with a supported C ABI signature\n",
                f->name ? f->name : "<anonymous>");
        ctx->error = true;
        return;
    }

    emit_c_export_stub_signature(out, f);
    fprintf(out, " {\n");

    ret_type = f->return_type;
    if (!ret_type || ret_type->kind == XR_KIND_UNIT) {
        fprintf(out, "    ");
        emit_cfn_target_call_expr(ctx, out, f, prefix);
        fprintf(out, ";\n");
        fprintf(out, "}\n\n");
        return;
    }

    ret_c_type = cg_cfn_value_c_type(ret_type, true);
    ret_rep = cg_func_return_abi_rep(ctx, f);
    c_ret_rep = cg_cfn_value_storage_rep(ret_type, true);

    fprintf(out, "    return ");
    if (ret_type->kind == XR_KIND_POINTER) {
        if (ret_rep == XR_REP_PTR || ret_rep == XR_REP_RAWPTR) {
            fprintf(out, "(%s)", ret_c_type);
            emit_cfn_target_call_expr(ctx, out, f, prefix);
        } else {
            fprintf(out, "(%s)(uintptr_t)(", ret_c_type);
            emit_cfn_target_call_expr(ctx, out, f, prefix);
            fprintf(out, ")");
        }
    } else {
        const char *suffix;
        fprintf(out, "(%s)(", ret_c_type);
        suffix = emit_conversion_prefix(out, ret_type, ret_rep, c_ret_rep);
        emit_cfn_target_call_expr(ctx, out, f, prefix);
        emit_conversion_suffix(out, suffix);
        fprintf(out, ")");
    }
    fprintf(out, ";\n");
    fprintf(out, "}\n\n");
}

static void xi_cgen_func(XiCgenCtx *ctx, FILE *out, XiFunc *f, const char *prefix) {
    XR_DCHECK(out != NULL, "xi_cgen_func: NULL output");
    XR_DCHECK(f != NULL, "xi_cgen_func: NULL func");
    /* FFI: @extern functions have no Xray definition. Only the `extern Ret
     * sym(...)` forward declaration is emitted (see emit_one_forward_decl);
     * call sites emit a direct C call. Never emit a body. */
    if (f->is_extern)
        return;
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
    xicgen_emit_par_for_range_wrappers(ctx, out, f, prefix);
    xicgen_emit_par_collect_range_wrappers(ctx, out, f, prefix);
    xicgen_emit_par_reduce_range_wrappers(ctx, out, f, prefix);

    cg_class_field_cache_reset(&ctx->class_field_cache);
    cg_array_data_cache_decls_reset(ctx);

    bool needs_aot_coro = cg_func_needs_aot_coro_ctx(ctx, f);
    bool typed_abi = cg_func_uses_typed_abi(ctx, f);
    bool native_receiver = cg_class_func_uses_native_receiver(ctx, f);
    bool boxed_adapter = cg_func_needs_boxed_adapter(ctx, f, prefix, typed_abi, native_receiver);
    cg_record_function_stats(ctx, f, typed_abi, native_receiver, needs_aot_coro);

    /* Default to identity phi naming; the coro path and unbuilt functions never
     * consult the (possibly stale) coalescing map from a previously emitted
     * sibling/child function. */
    ctx->phi_repr_active = false;
    f->phi_coalesce = NULL;
    f->phi_coalesce_count = 0;

    if (needs_aot_coro) {
        xi_cgen_coro_func(ctx, out, f, prefix);
        return;
    }

    /* Function signature.  Closure children with captures receive a hidden
     * first parameter xrt_closure_t *_cl for per-closure upvalue access. A
     * vararg function carries one extra trailing Array<T> parameter (the rest
     * slot); direct call sites collect the variadic arguments into it. */
    bool has_cl = (f->ncaptures > 0);
    uint16_t sig_nparams = (uint16_t) (f->nparams + (f->is_vararg ? 1 : 0));
    fprintf(out, "%s", cg_func_linkage(ctx, f, prefix));
    emit_func_attr_qualifier(ctx, out, f);
    if (!emit_class_native_return_type(ctx, out, prefix, f))
        fprintf(out, "%s", cg_func_return_abi_c_type(ctx, f));
    fprintf(out, " ");
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "(");
    fprintf(out, "xrt_closure_t *_cl");
    for (uint16_t i = 0; i < sig_nparams; i++)
        fprintf(out, ", "), emit_class_native_param_decl(ctx, out, prefix, f, i);
    fprintf(out, ") {\n");
    if (!has_cl)
        fprintf(out, "    (void)_cl;\n");

    ctx->pre_decl_all = cg_has_exception_handling(f);
    cg_prepare_cell_vars(ctx, f);
    cg_build_phi_coalesce(ctx, f);
    cg_class_field_cache_collect(ctx, f);
    ctx->pre_decl_all = cg_needs_predecl_all(f);
    emit_declarations(ctx, out, f);
    emit_debug_source_var_declarations(ctx, out, f);
    emit_class_field_cache_decls(ctx, out);

    /* Function-scoped defer: own a stack-local scope and link it onto the global
     * defer chain at entry. Defers (lowered to zero-arg closures) are pushed
     * here and run LIFO at every exit (emit_deferred_calls -> xrt_defer_leave)
     * or during a panic unwind (xrt_throw_exc -> xrt_defer_unwind_to). */
    if (cg_func_has_defer_stmt(f))
        fprintf(out, "    XrtDeferScope _xrt_ds; xrt_defer_enter(&_xrt_ds);\n");

    if (cg_func_emits_sync_backedge_heartbeat(ctx, f))
        fprintf(out, "    uint32_t _xr_aot_sync_backedge_count = 0;\n");

    /* Blocks in order */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (f->blocks[bi] && !cg_structured_counted_loop_block_is_elided(f, f->blocks[bi]) &&
            !cg_structured_array_fill_loop_block_is_elided(ctx, f, f->blocks[bi]))
            emit_block(ctx, out, f, f->blocks[bi], prefix);
    }
    if (func_needs_fallthrough_return(f))
        emit_fallthrough_return(ctx, out, f, prefix);

    fprintf(out, "}\n\n");

    emit_cfn_stub_definition(ctx, out, f, prefix);
    emit_c_export_stub_definition(ctx, out, f, prefix);

    if (native_receiver && boxed_adapter) {
        ctx->stats.boxed_adapters++;
        emit_class_native_boxed_adapter(ctx, out, prefix, f);
    } else if (typed_abi && boxed_adapter) {
        ctx->stats.boxed_adapters++;
        if (!emit_class_native_typed_boxed_adapter(ctx, out, prefix, f)) {
            fprintf(out, "%sXrValue ", cg_linkage(ctx));
            emit_typed_abi_fname(ctx, out, prefix, f);
            fprintf(out, "(xrt_closure_t *_cl");
            uint16_t boxed_total = (uint16_t) (f->nparams + (f->is_vararg ? 1 : 0));
            for (uint16_t i = 0; i < boxed_total; i++)
                fprintf(out, ", XrValue p%u", i);
            fprintf(out, ") {\n");
            bool ret_is_aggregate = cg_func_return_abi_is_aggregate(ctx, f);
            XrRep ret_rep = cg_func_return_abi_rep(ctx, f);
            if (ret_rep == XR_REP_VOID) {
                fprintf(out, "    ");
            } else {
                fprintf(out, "    return ");
            }
            const char *conv_suffix = NULL;
            if (ret_is_aggregate && cg_func_return_abi_is_struct_aggregate(ctx, f)) {
                fprintf(stderr,
                        "[xi_cgen] ERROR: boxed adapter for struct aggregate return '%s' needs "
                        "the planned value-struct boxing bridge\n",
                        f->name ? f->name : "?");
                ctx->error = true;
                fprintf(out, "XR_NULL_VAL /* unsupported struct aggregate adapter */");
                fprintf(out, ";\n");
                fprintf(out, "}\n\n");
                return;
            } else if (ret_is_aggregate)
                fprintf(out, "xrt_adt_value_box(");
            else if (ret_rep != XR_REP_VOID)
                conv_suffix = emit_conversion_prefix(out, f->return_type, ret_rep, XR_REP_TAGGED);
            emit_fname(ctx, out, prefix, f);
            fprintf(out, "(_cl");
            for (uint16_t i = 0; i < boxed_total; i++) {
                fprintf(out, ", ");
                XrRep param_rep = cg_func_param_abi_rep(ctx, f, i);
                const XrType *param_type = f->params && f->params[i] ? f->params[i]->type : NULL;
                const char *param_suffix =
                    emit_conversion_prefix(out, param_type, XR_REP_TAGGED, param_rep);
                fprintf(out, "p%u", i);
                emit_conversion_suffix(out, param_suffix);
            }
            fprintf(out, ")");
            if (ret_is_aggregate)
                fprintf(out, ")");
            else if (ret_rep != XR_REP_VOID)
                emit_conversion_suffix(out, conv_suffix);
            fprintf(out, ";\n");
            if (ret_rep == XR_REP_VOID)
                fprintf(out, "    return XR_NULL_VAL;\n");
            fprintf(out, "}\n\n");
        }
    }

    if (cg_func_needs_sync_go_wrapper_ctx(ctx, f)) {
        ctx->stats.sync_go_wrappers++;
        emit_sync_go_wrapper(ctx, out, f, prefix);
    }
}

/* ========== Forward Declarations ========== */

/* Emit the forward declaration(s) for a single function (no recursion): the
 * function prototype plus any boxed adapter / coroutine frame declarations it
 * needs.  Used both for a unit's own functions (via emit_forward_decls) and for
 * the imported cross-module functions a unit references (114). */
static void emit_one_forward_decl(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    /* FFI: @extern function — declare the foreign C symbol directly (no name
     * mangling, no hidden _cl). The linker resolves it from the process / a
     * linked library; call sites emit `sym(args)`. */
    if (f->is_extern) {
        const char *sym = f->extern_symbol ? f->extern_symbol : (f->name ? f->name : "");
        const char *ret_ptr = cg_extern_ptr_boundary_c_type(f->return_type);
        /* Bind a fresh `xr_ffi_<sym>` alias to the real C symbol via an asm label.
         * Using a distinct name avoids libc fortify macros (memcpy, ...) and any
         * mismatch with system prototypes (e.g. size_t vs uint64_t), while still
         * calling the exact symbol. */
        fprintf(out, "extern ");
        if (ret_ptr)
            fprintf(out, "%s", ret_ptr);
        else if (!emit_class_native_return_type(ctx, out, prefix, f))
            fprintf(out, "%s", cg_func_return_abi_c_type(ctx, f));
        fprintf(out, " xr_ffi_%s(", sym);
        if (f->nparams == 0) {
            fprintf(out, "void");
        } else {
            for (uint16_t i = 0; i < f->nparams; i++) {
                if (i > 0)
                    fprintf(out, ", ");
                const XrType *pt = (f->params && f->params[i]) ? f->params[i]->type : NULL;
                const char *p_ptr = cg_extern_ptr_boundary_c_type(pt);
                if (cg_type_is_c_callback(pt))
                    emit_cfn_pointer_type(ctx, out, pt, NULL);
                else if (p_ptr)
                    fprintf(out, "%s", p_ptr);
                else
                    fprintf(out, "%s", cg_func_param_abi_c_type(ctx, f, i));
            }
        }
        fprintf(out, ") __asm__(XR_FFI_ASMNAME(\"%s\"));\n", sym);
        return;
    }
    bool needs_aot_coro = cg_func_needs_aot_coro_ctx(ctx, f);
    /* Coroutine functions are emitted (definition) with file-static linkage by
     * the coro codegen, so their forward declaration must match; only plain
     * functions participate in cross-module external linkage. */
    fprintf(out, "%s", needs_aot_coro ? "static " : cg_func_linkage(ctx, f, prefix));
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
    uint16_t fwd_nparams = (uint16_t) (f->nparams + (f->is_vararg ? 1 : 0));
    for (uint16_t i = 0; i < fwd_nparams; i++) {
        if (needs_aot_coro) {
            fprintf(out, ", XrValue p%u", i);
        } else {
            fprintf(out, ", ");
            emit_class_native_param_decl(ctx, out, prefix, f, i);
        }
    }
    fprintf(out, ");\n");

    if (cg_func_can_have_cfn_stub(ctx, f)) {
        emit_cfn_stub_signature(ctx, out, f, prefix);
        fprintf(out, ";\n");
    }
    if (cg_func_can_have_c_export_stub(ctx, f)) {
        emit_c_export_stub_signature(out, f);
        fprintf(out, ";\n");
    }

    bool typed_abi = cg_func_uses_typed_abi(ctx, f);
    bool native_receiver = cg_class_func_uses_native_receiver(ctx, f);
    if (!needs_aot_coro &&
        cg_func_needs_boxed_adapter(ctx, f, prefix, typed_abi, native_receiver)) {
        fprintf(out, "%sXrValue ", cg_linkage(ctx));
        emit_typed_abi_fname(ctx, out, prefix, f);
        fprintf(out, "(xrt_closure_t *_cl");
        uint16_t boxed_total = (uint16_t) (f->nparams + (f->is_vararg ? 1 : 0));
        for (uint16_t i = 0; i < boxed_total; i++)
            fprintf(out, ", XrValue p%u", i);
        fprintf(out, ");\n");
    }

    bool needs_sync_go = !needs_aot_coro && cg_func_needs_sync_go_wrapper_ctx(ctx, f);
    if (needs_aot_coro || needs_sync_go) {
        /* The frame factory, resume entry and descriptor carry cross-module
         * linkage so a coroutine spawned from another module's translation unit
         * resolves at link time (matches the definitions in xi_cgen_coro).  A
         * const object at file scope without `extern` is a tentative definition,
         * so the descriptor declaration must use `extern` (not bare const). */
        fprintf(out, "%svoid *", cg_linkage(ctx));
        emit_fname_suffix(ctx, out, prefix, f, "_aot_frame_new");
        fprintf(out, "(");
        emit_aot_frame_new_params(out, f, needs_sync_go);
        fprintf(out, ");\n");
        if (needs_aot_coro) {
            fprintf(out, "%sbool ", cg_linkage(ctx));
            emit_fname_suffix(ctx, out, prefix, f, "_aot_frame_init");
            fprintf(out, "(void *raw_frame");
            if (cg_func_frame_needs_cl(f) || f->nparams > 0)
                fprintf(out, ", ");
            emit_aot_frame_new_params(out, f, false);
            fprintf(out, ");\n");
        }
        fprintf(out, "%sXrAotResult ", cg_linkage(ctx));
        emit_fname_suffix(ctx, out, prefix, f, "_aot_resume");
        fprintf(out, "(void *raw_frame, const XrAotContext *ctx);\n");
        fprintf(out, "%svoid ", cg_linkage(ctx));
        emit_fname_suffix(ctx, out, prefix, f, "_aot_trace");
        fprintf(out, "(void *frame, void *visitor);\n");
        fprintf(out, "%svoid ", cg_linkage(ctx));
        emit_fname_suffix(ctx, out, prefix, f, "_aot_release");
        fprintf(out, "(void *frame, struct XrCoroHeap *heap);\n");
        fprintf(out, "%sconst XrAotCoroDesc ", ctx->extern_linkage ? "extern " : "static ");
        emit_fname_suffix(ctx, out, prefix, f, "_aot_desc");
        fprintf(out, ";\n");
    }
}

/* Forward-declare a function and all its nested functions (children first). */
static void emit_forward_decls(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            emit_forward_decls(ctx, out, f->children[i], prefix);
    }
    emit_one_forward_decl(ctx, out, f, prefix);
}

#include "xi_cgen_import_helpers.inc.c"
#include "xi_cgen_stdlib_helpers.inc.c"
#include "xi_cgen_program_entry.inc.c"
