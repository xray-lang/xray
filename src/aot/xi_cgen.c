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
#include "xaot_rep_gen.h"
#include "xaot_abi_gen.h"
#include "xaot_layout_gen.h"
#include "xi_to_c_dispatch_gen.h"
#include "../ir/xi_analysis.h"
#include "../ir/xi_backend_lower.h"
#include "../ir/xi_op_name.h"
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
        default:
            return "XrValue";
    }
}

static const char *cg_native_int_ctype(uint8_t native_width) {
    return xaot_c_type_for_native_int_type(native_width);
}

static uint8_t cg_narrow_int_native_width(uint16_t op) {
    switch (op) {
        case XI_NARROW_I8:
            return XR_NATIVE_I8;
        case XI_NARROW_U8:
            return XR_NATIVE_U8;
        case XI_NARROW_I16:
            return XR_NATIVE_I16;
        case XI_NARROW_U16:
            return XR_NATIVE_U16;
        case XI_NARROW_I32:
            return XR_NATIVE_I32;
        case XI_NARROW_U32:
            return XR_NATIVE_U32;
        default:
            return 0;
    }
}

static bool cg_const_int_fits_native_width(int64_t value, uint8_t native_width) {
    return xaot_native_int_const_fits(native_width, value);
}

static bool cg_value_narrow_local_native_width(const XiValue *v, uint8_t depth,
                                               uint8_t *out_native_width) {
    if (!v || cg_rep(v) != XR_REP_I64 || depth > 8)
        return false;

    uint8_t op_width = cg_narrow_int_native_width(v->op);
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
            return v->aux_int == XI_CORO_SUB_LOCK_THREAD ||
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
#define CG_MAX_SYNC_GO_TARGETS 512
#define CG_MAX_CLASS_FIELD_CACHE 16
#define CG_MAX_CLASS_FIELD_CACHE_ALIASES 32
#define CG_MAX_SHARED_NATIVE_EXPORTS 512
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

/* All mutable codegen state for one C-generation session.
 * Heap-allocated via xi_cgen_ctx_new; no file-scope globals. */
struct XiCgenCtx {
    int fname_counter;
    const XiFunc *shared_funcs[CG_MAX_SHARED];
    const XiClassData *shared_class[CG_MAX_SHARED];
    const XiEnumData *shared_enum[CG_MAX_SHARED];
    CgSharedNativeInstance shared_native_instances[CG_MAX_SHARED];
    CgSharedNativeExport shared_native_exports[CG_MAX_SHARED_NATIVE_EXPORTS];
    int nshared_native_exports;
    int nshared;
    CgMethodEntry methods[CG_MAX_METHODS];
    int nmethod;
    XiModule *module; /* current module being emitted */
    bool pre_decl_all;
    bool cell_vars[256];
    const XiValue *cell_origins[256];
    const char *shared_name;
    CgImportEntry imports[CG_MAX_IMPORTS];
    int nimports;
    XiModule **all_modules; /* full modules array for resolved-index lookups */
    int all_nmodules;
    bool error; /* set on fatal codegen errors (unknown builtin, etc.) */
    XiCgenCoroFrameStats coro_frame_stats;
    const XiFunc *sync_go_targets[CG_MAX_SYNC_GO_TARGETS];
    int nsync_go_targets;
    CgClassFieldCache class_field_cache;
};

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
                if (idx < parent->nchildren && ctx->nmethod < CG_MAX_METHODS) {
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

    memset(ctx->shared_funcs, 0, sizeof(ctx->shared_funcs));
    memset(ctx->shared_class, 0, sizeof(ctx->shared_class));
    memset(ctx->shared_enum, 0, sizeof(ctx->shared_enum));
    memset(ctx->shared_native_instances, 0, sizeof(ctx->shared_native_instances));
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

/* Write a value reference: v<id> or phi<id> for phi nodes */
static void emit_vref(FILE *out, const XiValue *v) {
    if (v->op == XI_PHI)
        fprintf(out, "phi%u", v->id);
    else
        fprintf(out, "v%u", v->id);
}

#include "xi_cgen_class_native_meta.inc.c"
#include "xi_cgen_abi_helpers.inc.c"
#include "xi_cgen_value_helpers.inc.c"
#include "xi_cgen_method_symbols.inc.c"
static bool cg_array_same_value(const XiValue *a, const XiValue *b);
static bool cg_array_value_known_nonnegative(const XiValue *v, const XiValue *root, uint8_t depth);
static bool cg_array_block_has_no_side_effect_after(const XiBlock *blk, const XiValue *start);
static bool cg_array_block_has_no_side_effect_before(const XiBlock *blk, const XiValue *target);
static bool cg_array_index_access_bounds_proven(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);
static bool cg_array_index_set_counted_loop_bounds_proven(XiCgenCtx *ctx, const XiFunc *f,
                                                          const XiValue *v);
#include "xi_cgen_struct_helpers.inc.c"
#include "xi_cgen_class_helpers.inc.c"
static bool cg_has_exception_handling(const XiFunc *f);
#include "xi_cgen_class_native_helpers.inc.c"
#include "xi_cgen_array_helpers.inc.c"

static const char *local_ctype_str_ctx(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (cg_array_value_uses_native_local(ctx, f, v))
        return "xrt_array_t *";
    return local_ctype_str(v);
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

static void emit_bitwise_binop(FILE *out, const XiValue *v, const char *op) {
    bool boxed = cg_rep(v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "XR_FROM_INT(");
    fprintf(out, "(");
    emit_value_as_rep(out, v->args[0], XR_REP_I64);
    fprintf(out, ") %s (", op);
    emit_value_as_rep(out, v->args[1], XR_REP_I64);
    fprintf(out, ")");
    if (boxed)
        fprintf(out, ")");
}

static void emit_bitwise_unop(FILE *out, const XiValue *v, const char *op) {
    bool boxed = cg_rep(v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "XR_FROM_INT(");
    fprintf(out, "%s(", op);
    emit_value_as_rep(out, v->args[0], XR_REP_I64);
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

    switch (v->op) {
        case XI_PARAM:
            fprintf(out, "p%u", (unsigned) v->aux_int);
            if (cg_func_param_abi_rep(ctx, f, (uint16_t) v->aux_int) != XR_REP_TAGGED)
                break;
            if (cg_rep(v) == XR_REP_I64)
                fprintf(out, ".i");
            else if (cg_rep(v) == XR_REP_F64)
                fprintf(out, ".f");
            break;

        /* Arithmetic: use C operators when both operands are scalar.
         * When any operand is tagged (XrValue), must use runtime functions.
         * When result rep is scalar but operands are mixed, box each tagged
         * operand before the C operation, or fall back to runtime dispatch. */
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
                /* Typed scalar path.  Constant non-zero divisors can use native
                 * C operators; other integer DIV/MOD keeps the checked helper. */
                if ((v->op == XI_DIV || v->op == XI_MOD) && result_rep == XR_REP_I64) {
                    if (!emit_native_const_div_mod_expr(out, v)) {
                        const char *fn = (v->op == XI_DIV) ? "xrt_int_div" : "xrt_int_mod";
                        fprintf(out, "%s(", fn);
                        emit_vref(out, v->args[0]);
                        fprintf(out, ", ");
                        emit_vref(out, v->args[1]);
                        fprintf(out, ")");
                    }
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
                    emit_codegen_abort_expr(out);
                    ctx->error = true;
                }
            }
            break;
        }
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

        /* Convert */
        case XI_CONVERT: {
            XrRep dst_rep = cg_rep(v);
            XrRep src_rep = cg_rep(v->args[0]);
            if (v->type->kind == XR_KIND_FLOAT) {
                if (dst_rep == XR_REP_TAGGED) {
                    fprintf(out, "xrt_to_float(");
                    emit_boxed_value_ref(out, v->args[0]);
                    fprintf(out, ")");
                } else if (src_rep == XR_REP_TAGGED) {
                    fprintf(out, "XR_TO_FLOAT(xrt_to_float(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, "))");
                } else {
                    fprintf(out, "(double)");
                    emit_vref(out, v->args[0]);
                }
            } else if (v->type->kind == XR_KIND_INT) {
                if (dst_rep == XR_REP_TAGGED) {
                    fprintf(out, "xrt_to_int(");
                    emit_boxed_value_ref(out, v->args[0]);
                    fprintf(out, ")");
                } else if (src_rep == XR_REP_TAGGED) {
                    fprintf(out, "XR_TO_INT(xrt_to_int(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, "))");
                } else {
                    fprintf(out, "(int64_t)");
                    emit_vref(out, v->args[0]);
                }
            } else if (v->type->kind == XR_KIND_STRING) {
                fprintf(out, "xrt_to_string(");
                emit_boxed_value_ref(out, v->args[0]);
                fprintf(out, ")");
            } else if (v->type->kind == XR_KIND_BOOL) {
                if (dst_rep == XR_REP_TAGGED) {
                    fprintf(out, "xrt_to_bool(");
                    emit_boxed_value_ref(out, v->args[0]);
                    fprintf(out, ")");
                } else if (src_rep == XR_REP_TAGGED) {
                    fprintf(out, "XR_TO_INT(xrt_to_bool(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, "))");
                } else {
                    fprintf(out, "(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, " != 0)");
                }
            } else {
                emit_vref(out, v->args[0]);
            }
            break;
        }

        /* Function call: args[0]=callee, args[1..n]=params */
        case XI_CALL: {
            XiValue *callee = v->args[0];
            CgStaticFunctionCall static_call = cg_resolve_static_function_call(ctx, f, callee);
            const XiFunc *target = static_call.func;

            /* XI_IMPORT_REF callee → cross-module imported function or class.
             * Try resolved-index fast path first, then fall back to string scan. */
            const char *import_prefix = static_call.prefix;
            bool import_is_class = static_call.is_class_constructor;
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
                if (emit_class_native_constructor_boxed_expr(ctx, out, f, prefix, v, target,
                                                             import_prefix))
                    break;
                fprintf(out, "({ XrValue _inst = xrt_map_new(4); ");
                emit_fname(ctx, out, import_prefix ? import_prefix : prefix, target);
                fprintf(out, "(NULL, _inst");
                for (uint16_t a = 1; a < v->nargs; a++) {
                    fprintf(out, ", ");
                    emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
                }
                fprintf(out, "); _inst; })");
            } else if (target) {
                /* Use the exporter's module prefix for cross-module calls */
                XrRep actual_rep = cg_func_return_abi_rep(ctx, target);
                bool wrapped = emit_conversion_prefix(out, v->type, actual_rep, cg_rep(v));
                emit_fname(ctx, out, import_prefix ? import_prefix : prefix, target);
                fprintf(out, "(");
                emit_call_hidden_closure(out, f, target, callee);
                for (uint16_t a = 1; a < v->nargs; a++) {
                    fprintf(out, ", ");
                    emit_value_as_rep(out, v->args[a],
                                      cg_func_param_abi_rep(ctx, target, (uint16_t) (a - 1)));
                }
                fprintf(out, ")");
                emit_conversion_suffix(out, wrapped);
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
            emit_closure_new_expr(ctx, out, prefix, v);
            break;

        /* Upvalue access: reads/writes from the hidden _cl parameter.
         * Closure children with captures receive xrt_closure_t *_cl as
         * their first C parameter; upvals are stored in _cl->upvals[]. */
        case XI_LOAD_UPVAL:
            if (v->aux_int >= 0 && v->aux_int < f->ncaptures &&
                f->captures[v->aux_int].needs_cell) {
                char cell_expr[64];
                snprintf(cell_expr, sizeof(cell_expr), "_cl->upvals[%d]", (int) v->aux_int);
                emit_cell_get_for_rep(out, v, cell_expr);
            } else {
                fprintf(out, "_cl->upvals[%d]", (int) v->aux_int);
            }
            break;

        case XI_STORE_UPVAL:
            if (v->aux_int >= 0 && v->aux_int < f->ncaptures &&
                f->captures[v->aux_int].needs_cell) {
                fprintf(out, "(xrt_cell_set(_cl->upvals[%d], ", (int) v->aux_int);
                emit_boxed_value_ref(out, v->args[0]);
                fprintf(out, "), XR_NULL_VAL)");
            } else {
                fprintf(out, "(_cl->upvals[%d] = ", (int) v->aux_int);
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            }
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
            if (emit_struct_fixed_array_index_get_expr(ctx, out, f, v, prefix) ||
                emit_typed_array_index_get_expr(ctx, out, f, v, prefix))
                break;
            fprintf(out, "xrt_index_get(");
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
            fprintf(out, ")");
            break;

        /* Indexed write: args[0]=collection, args[1]=key, args[2]=value */
        case XI_INDEX_SET:
            XR_DCHECK(v->nargs >= 3, "XI_INDEX_SET: need obj+key+val");
            if (emit_struct_fixed_array_index_set_expr(ctx, out, f, v, prefix))
                break;
            if (emit_typed_array_index_set_expr(ctx, out, f, v, prefix))
                break;
            fprintf(out, "xrt_index_set(");
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
            fprintf(out, ", ");
            emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
            fprintf(out, ")");
            break;

        /* ============ Field Access ============ */

        /* Property read: args[0]=object, aux=field name string */
        case XI_LOAD_FIELD: {
            XR_DCHECK(v->nargs >= 1, "XI_LOAD_FIELD: need object");
            if (emit_class_cached_field_load_expr(ctx, out, v))
                break;
            if (emit_class_native_receiver_field_load_expr(ctx, out, f, v))
                break;
            const char *field = (const char *) v->aux;
            if (!field && v->aux_int >= 0) {
                fprintf(out, "xrt_index_get(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", XR_FROM_INT(%" PRId64 "))", v->aux_int);
                break;
            }
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
            if (field && (strcmp(field, "length") == 0 || strcmp(field, "size") == 0) &&
                emit_class_native_map_length_expr(ctx, out, f, v))
                break;
            if (field && (strcmp(field, "length") == 0 || strcmp(field, "size") == 0) &&
                emit_class_native_set_length_expr(ctx, out, f, v))
                break;
            if (field && (strcmp(field, "length") == 0 || strcmp(field, "size") == 0) &&
                emit_typed_array_length_expr(ctx, out, f, prefix, v))
                break;
            /* Use xrt_getprop with symbol lookup for builtin properties,
             * or xrt_map_get for map-like objects */
            int sym = cg_method_sym(field);
            if (sym >= 0) {
                bool wrapped = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
                fprintf(out, "xrt_getprop(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", %d)", sym);
                emit_conversion_suffix(out, wrapped);
            } else {
                /* Generic field: use map get with string key */
                bool wrapped = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
                fprintf(out, "xrt_map_get((xrt_map_t*)");
                emit_vref(out, v->args[0]);
                fprintf(out, ".ptr, xr_box_str(\"%s\"))", field ? field : "?");
                emit_conversion_suffix(out, wrapped);
            }
            break;
        }

        case XI_TUPLE_GET: {
            XR_DCHECK(v->nargs >= 1, "XI_TUPLE_GET: need tuple");
            bool wrapped = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
            fprintf(out, "xrt_tuple_get(");
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ", %" PRId64 ")", v->aux_int);
            emit_conversion_suffix(out, wrapped);
            break;
        }

        /* Property write: args[0]=object, args[1]=value, aux=field name string */
        case XI_STORE_FIELD: {
            XR_DCHECK(v->nargs >= 2, "XI_STORE_FIELD: need obj+val");
            if (emit_class_cached_field_store_expr(ctx, out, v))
                break;
            if (emit_class_native_receiver_field_store_expr(ctx, out, f, v))
                break;
            const char *field = (const char *) v->aux;
            fprintf(out, "(xrt_map_set((xrt_map_t*)");
            emit_vref(out, v->args[0]);
            fprintf(out, ".ptr, xr_box_str(\"%s\"), ", field ? field : "?");
            emit_boxed_value_ref(out, v->args[1]);
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
                emit_struct_fallback_new_expr(out, (XrStructLayout *) v->aux, prefix);
            }
            break;
        }
        case XI_STRUCT_GET: {
            XR_DCHECK(v->nargs >= 1, "XI_STRUCT_GET: need struct arg");
            const XiValue *origin = cg_trace_struct_new(v->args[0]);
            if (origin && cg_struct_can_inline(f, origin)) {
                emit_struct_inline_field_get_expr(out, (XrStructLayout *) origin->aux, origin,
                                                  v->aux_int);
            } else {
                XrStructLayout *sl = (XrStructLayout *) v->aux;
                emit_struct_fallback_field_get(ctx, out, f, sl, v->aux_int, v->args[0], v->type,
                                               cg_rep(v), prefix);
            }
            break;
        }
        case XI_STRUCT_SET: {
            XR_DCHECK(v->nargs >= 2, "XI_STRUCT_SET: need struct + val");
            const XiValue *origin = cg_trace_struct_new(v->args[0]);
            if (origin && cg_struct_can_inline(f, origin)) {
                emit_struct_inline_field_set_expr(out, (XrStructLayout *) origin->aux, origin,
                                                  v->aux_int, v->args[1]);
            } else {
                XrStructLayout *sl = (XrStructLayout *) v->aux;
                emit_struct_fallback_field_set(ctx, out, f, sl, v->aux_int, v->args[0], v->args[1],
                                               prefix);
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
            const char *time_helper = cg_time_module_helper_ctx(ctx, f, v);

            if (cg_is_time_module_call_ctx(ctx, f, v)) {
                if (!time_helper) {
                    ctx->error = true;
                    fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT time method '%s'\n",
                            method ? method : "?");
                    emit_codegen_abort_expr(out);
                    break;
                }
                if (cg_rep(v) == XR_REP_I64)
                    fprintf(out, "XR_TO_INT(");
                else if (cg_rep(v) == XR_REP_F64)
                    fprintf(out, "XR_TO_FLOAT(");
                fprintf(out, "%s()", time_helper);
                if (cg_rep(v) == XR_REP_I64 || cg_rep(v) == XR_REP_F64)
                    fprintf(out, ")");
                break;
            }

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
                const char *recv_class = cg_class_native_receiver_class_name(ctx, f, v->args[0]);
                if (v->op == XI_CALL_METHOD_DIRECT)
                    mfunc = cg_lookup_method_by_index(ctx, recv_class, (int) v->aux_int,
                                                      &method_prefix);
                else
                    mfunc = cg_lookup_method(ctx, method, recv_class, &method_prefix);
            }
            uint16_t nargs = (uint16_t) (v->nargs - 1);

            if (nargs == 0 && method && strcmp(method, "length") == 0 &&
                emit_typed_array_length_expr(ctx, out, f, prefix, v))
                break;
            if (nargs == 1 && method && strcmp(method, "push") == 0 &&
                emit_typed_array_push_expr(ctx, out, f, prefix, v, v->args[0], v->args[1]))
                break;
            if (method && ((nargs == 1 && strcmp(method, "map") == 0 &&
                            emit_typed_array_map_expr(ctx, out, f, prefix, v)) ||
                           (nargs == 1 && strcmp(method, "filter") == 0 &&
                            emit_typed_array_filter_expr(ctx, out, f, prefix, v)) ||
                           (nargs == 2 && strcmp(method, "reduce") == 0 &&
                            emit_typed_array_reduce_expr(ctx, out, f, prefix, v))))
                break;

            const XiEnumData *recv_enum = cg_enum_for_shared_value(ctx, v->args[0]);
            int enum_member = cg_enum_member_index(recv_enum, method);
            if (recv_enum && enum_member >= 0) {
                if (recv_enum->is_adt && recv_enum->members &&
                    recv_enum->members[enum_member].payload_count > 0) {
                    emit_adt_enum_construct_expr(out, enum_member, v);
                } else {
                    fprintf(out, "xrt_map_get((xrt_map_t*)");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ".ptr, xr_box_str(");
                    emit_c_string_literal(out, method);
                    fprintf(out, "))");
                }
                break;
            }

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
                if (emit_class_native_method_call_expr(ctx, out, f, prefix, v, mfunc,
                                                       method_prefix))
                    break;
                XrRep actual_rep = cg_func_return_abi_rep(ctx, mfunc);
                bool wrapped = emit_conversion_prefix(out, v->type, actual_rep, cg_rep(v));
                emit_fname(ctx, out, method_prefix ? method_prefix : prefix, mfunc);
                fprintf(out, "(NULL");
                for (uint16_t a = 0; a < v->nargs; a++) {
                    fprintf(out, ", ");
                    emit_value_as_rep(out, v->args[a],
                                      cg_func_param_abi_rep(ctx, mfunc, (uint16_t) a));
                }
                fprintf(out, ")");
                emit_conversion_suffix(out, wrapped);
            } else {
                int sym = cg_method_sym(method);
                const XrType *recv_type = v->nargs > 0 && v->args[0] ? v->args[0]->type : NULL;
                bool recv_is_stringbuilder =
                    recv_type && recv_type->kind == XR_KIND_INSTANCE &&
                    recv_type->instance.class_name &&
                    strcmp(recv_type->instance.class_name, "StringBuilder") == 0;
                if (sym < 0 && recv_is_stringbuilder && method && strcmp(method, "append") == 0 &&
                    nargs == 1) {
                    fprintf(out, "(xrt_strbuf_append(");
                    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                    fprintf(out, ", ");
                    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
                    fprintf(out, "), ");
                    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                    fprintf(out, ")");
                    break;
                }
                if (sym < 0) {
                    ctx->error = true;
                    fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT method '%s'\n",
                            method ? method : "?");
                    emit_codegen_abort_expr(out);
                    break;
                }
                if (emit_class_native_map_method_call_expr(ctx, out, f, v))
                    break;
                if (emit_class_native_set_method_call_expr(ctx, out, f, v))
                    break;
                bool wrapped = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_rep(v));
                if (nargs == 0) {
                    fprintf(out, "xrt_method_0(");
                    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                    fprintf(out, ", %d)", sym);
                } else if (nargs == 1) {
                    fprintf(out, "xrt_method_1(");
                    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                    fprintf(out, ", %d, ", sym);
                    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
                    fprintf(out, ")");
                } else if (nargs == 2) {
                    fprintf(out, "xrt_method_2(");
                    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                    fprintf(out, ", %d, ", sym);
                    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
                    fprintf(out, ", ");
                    emit_value_as_rep(out, v->args[2], XR_REP_TAGGED);
                    fprintf(out, ")");
                } else {
                    ctx->error = true;
                    fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT method call with %u args\n",
                            (unsigned) nargs);
                    emit_codegen_abort_expr(out);
                }
                emit_conversion_suffix(out, wrapped);
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
                if (!emit_typed_map_new_expr(out, v, cap))
                    fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
            } else if (orig_op == XI_SET_NEW) {
                int64_t cap = (v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CONST)
                                  ? v->args[0]->aux_int
                                  : 8;
                if (!emit_typed_set_new_expr(out, v, cap))
                    fprintf(out, "xrt_set_new(%" PRId64 ")", cap);
            } else if (orig_op == XI_STR_CONCAT) {
                emit_str_concat_expr(out, v);
            } else if (orig_op == XI_CLOSURE_NEW) {
                emit_closure_new_expr(ctx, out, prefix, v);
            } else {
                emit_codegen_abort_expr(out);
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

        case XI_TUPLE_NEW:
            if (v->nargs == 0) {
                fprintf(out, "xrt_tuple_new(0)");
            } else {
                fprintf(out, "xrt_tuple_make(%" PRIu16 ", (XrValue[]){", v->nargs);
                for (uint16_t a = 0; a < v->nargs; a++) {
                    if (a > 0)
                        fprintf(out, ", ");
                    emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
                }
                fprintf(out, "})");
            }
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
                emit_str_concat_expr(out, v);
            } else if (strcmp(bn, "array_new") == 0) {
                int64_t cap =
                    (v->nargs >= 1 && v->args[0]->op == XI_CONST) ? v->args[0]->aux_int : 4;
                if (cg_array_value_uses_native_local(ctx, f, v)) {
                    if (!emit_typed_array_new_ptr_expr(ctx, out, f, v, cap))
                        fprintf(out, "(xrt_array_t*)xrt_array_new(%" PRId64 ").ptr", cap);
                } else if (!emit_typed_array_new_expr(ctx, out, f, v, cap)) {
                    fprintf(out, "xrt_array_new(%" PRId64 ")", cap);
                }
            } else if (strcmp(bn, "Bytes") == 0) {
                if (cg_array_value_uses_native_local(ctx, f, v)) {
                    if (!emit_bytes_new_native_local_expr(out, v)) {
                        emit_codegen_abort_expr(out);
                        ctx->error = true;
                    }
                } else if (v->nargs == 0) {
                    fprintf(out, "xrt_bytes_new_len(0)");
                } else if (v->nargs == 1) {
                    if (v->args[0] && v->args[0]->type && v->args[0]->type->kind == XR_KIND_INT) {
                        fprintf(out, "xrt_bytes_new_len(");
                        emit_value_as_rep(out, v->args[0], XR_REP_I64);
                        fprintf(out, ")");
                    } else {
                        fprintf(out, "xrt_bytes_new_1(");
                        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                        fprintf(out, ")");
                    }
                } else if (v->nargs == 2) {
                    fprintf(out, "xrt_bytes_new_fill(");
                    emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
                    fprintf(out, ", ");
                    emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
                    fprintf(out, ")");
                } else {
                    emit_codegen_abort_expr(out);
                    ctx->error = true;
                }
            } else if (emit_array_bytes_builtin_expr(ctx, out, v, bn)) {
                /* Expression emitted by the array/bytes helper. */
            } else if (strcmp(bn, "StringBuilder") == 0) {
                fprintf(out, "xrt_strbuf_new()");
            } else if (strcmp(bn, "map_new") == 0) {
                int64_t cap =
                    (v->nargs >= 1 && v->args[0]->op == XI_CONST) ? v->args[0]->aux_int : 8;
                if (!emit_typed_map_new_expr(out, v, cap))
                    fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
            } else if (strcmp(bn, "set_new") == 0) {
                int64_t cap =
                    (v->nargs >= 1 && v->args[0]->op == XI_CONST) ? v->args[0]->aux_int : 8;
                if (!emit_typed_set_new_expr(out, v, cap))
                    fprintf(out, "xrt_set_new(%" PRId64 ")", cap);
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

        case XI_GET_BUILTIN:
            if (v->aux_int == XR_GLOBAL_VAR_PROCESS || v->aux_int == XR_GLOBAL_VAR_FILE ||
                v->aux_int == XR_GLOBAL_VAR_DIR) {
                fprintf(out, "xrt_builtins[%d]", (int) v->aux_int);
            } else {
                ctx->error = true;
                fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT builtin global '%s'\n",
                        v->aux ? (const char *) v->aux : "?");
                emit_codegen_abort_expr(out);
            }
            break;

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
                if (ref && ref->module_path && !ref->member_name &&
                    strcmp(ref->module_path, "time") == 0) {
                    fprintf(out, "XR_NULL_VAL /* builtin module: time */");
                } else {
                    ctx->error = true;
                    fprintf(stderr, "[xi_cgen] ERROR: unresolved AOT import '%s.%s'\n",
                            ref && ref->module_path ? ref->module_path : "?",
                            ref && ref->member_name ? ref->member_name : "?");
                    emit_codegen_abort_expr(out);
                }
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

/* Emit a complete value statement: type vN = <rhs>; */
static void emit_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    XR_DCHECK(v != NULL, "emit_value_stmt: NULL value");

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
    if (cg_ownership_op_is_noop(v))
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

    if (v->op == XI_ERR_SET) {
        XR_DCHECK(v->nargs >= 1, "XI_ERR_SET: missing error value");
        fprintf(out, "    xrt_pending_error = ");
        emit_vref(out, v->args[0]);
        fprintf(out, ";\n");
        return;
    }

    if (v->op == XI_ERR_RETURN) {
        XR_DCHECK(v->nargs >= 1, "XI_ERR_RETURN: missing error value");
        fprintf(out, "    xrt_pending_error = ");
        emit_vref(out, v->args[0]);
        fprintf(out, ";\n");
        emit_class_field_cache_flush(ctx, out);
        emit_deferred_calls(ctx, out, f, prefix);
        fprintf(out, "    return ");
        emit_default_return_for_abi(ctx, out, f);
        fprintf(out, ";\n");
        return;
    }

    if (v->op == XI_ERR_CHECK && cg_value_type_is_bool(v)) {
        XrRep rep = cg_rep(v);
        fprintf(out, "    ");
        if (!ctx->pre_decl_all) {
            fprintf(out, "%s ", ctype_str(rep));
            emit_vref(out, v);
            fprintf(out, " = ");
        } else {
            emit_vref(out, v);
            fprintf(out, " = ");
        }
        if (rep == XR_REP_TAGGED)
            fprintf(out, "XR_FROM_BOOL(xrt_has_pending_error());\n");
        else
            fprintf(out, "xrt_has_pending_error();\n");
        return;
    }

    if (v->op == XI_ERR_CHECK && (cg_array_err_check_after_unchecked_fill_push(ctx, f, v) ||
                                  cg_array_err_check_after_typed_push(ctx, f, v) ||
                                  cg_array_err_check_after_inline_hof(ctx, f, prefix, v) ||
                                  cg_class_native_err_check_after_nothrow_call(ctx, f, v)))
        return;

    if (v->op == XI_ERR_CHECK) {
        fprintf(out, "    if (xrt_has_pending_error()) {\n");
        emit_class_field_cache_flush(ctx, out);
        emit_deferred_calls(ctx, out, f, prefix);
        fprintf(out, "        return ");
        emit_default_return_for_abi(ctx, out, f);
        fprintf(out, ";\n");
        fprintf(out, "    }\n");
        return;
    }

    if (v->op == XI_ERR_CATCH) {
        fprintf(out, "    ");
        if (!ctx->pre_decl_all) {
            fprintf(out, "%s ", ctype_str(cg_rep(v)));
            emit_vref(out, v);
            fprintf(out, " = ");
        } else {
            emit_vref(out, v);
            fprintf(out, " = ");
        }
        fprintf(out, "xrt_pending_error;\n");
        fprintf(out, "    xrt_pending_error = XR_NULL_VAL;\n");
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
            emit_value_as_rep(out, phi->value.args[pred_idx], cg_rep(&phi->value));
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
            emit_class_field_cache_flush(ctx, out);
            emit_deferred_calls(ctx, out, f, prefix);
            if (emit_class_native_return_stmt(ctx, out, f, blk))
                break;
            if (blk->control) {
                fprintf(out, "    return ");
                emit_value_as_rep(out, blk->control, cg_func_return_abi_rep(ctx, f));
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
            if (emit_bool_accumulate_diamond_stmt(out, blk))
                break;
            /* Emit phi copies for both branches */
            fprintf(out, "    if (");
            emit_condition_expr(out, blk->control);
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

/* Collect all values and phis to declare at function top.
 * When the function contains exception handling (setjmp/goto),
 * ALL SSA values are pre-declared to avoid jumping over decls. */
static void emit_declarations(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    bool pre_decl_all = cg_has_exception_handling(f);

    for (uint16_t var_id = 0; var_id < 256; var_id++) {
        if (!ctx->cell_vars[var_id])
            continue;
        fprintf(out, "    XrValue ");
        emit_cell_ref(out, (uint8_t) var_id);
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
            XrRep rep = cg_rep(&phi->value);
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
                if (!v)
                    continue;
                /* Skip void-like ops, exception ops, and inlined structs */
                if (cg_is_void_like(v) || v->op == XI_TRY || v->op == XI_END_TRY)
                    continue;
                if (v->op == XI_STRUCT_NEW && cg_struct_can_inline(f, v))
                    continue;
                if ((v->op == XI_COPY || v->op == XI_MOVE) &&
                    (cg_value_traces_to_inlined_struct(f, v) ||
                     cg_value_is_elided_heap_struct_alias(ctx, f, v)))
                    continue;
                if (cg_class_native_value_stmt_is_elided(ctx, f, v) ||
                    cg_class_native_ctor_can_inline(ctx, f, v) ||
                    cg_class_shared_native_ctor_value_is_elided(ctx, f, v, NULL) ||
                    cg_class_shared_native_set_is_elided(ctx, f, v) ||
                    cg_class_shared_native_value_is_elided(ctx, f, v))
                    continue;
                if (cg_array_class_field_alloc_value_is_elided(ctx, f, v))
                    continue;
                if (cg_class_native_map_field_value_is_elided(ctx, f, v))
                    continue;
                if (cg_class_native_set_field_value_is_elided(ctx, f, v))
                    continue;
                if (cg_class_native_map_method_call_value_is_elided(ctx, f, v) ||
                    cg_class_native_set_method_call_value_is_elided(ctx, f, v))
                    continue;
                if (cg_array_typed_push_value_is_elided(ctx, f, v))
                    continue;
                if ((v->op == XI_GET_SHARED && cg_value_only_used_by_inlined_struct_new(f, v)) ||
                    cg_value_is_elided_heap_struct_alias(ctx, f, v))
                    continue;
                XrRep rep = cg_rep(v);
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

    if (cg_func_needs_aot_coro_ctx(ctx, f)) {
        xi_cgen_coro_func(ctx, out, f, prefix);
        return;
    }

    /* Function signature.  Closure children with captures receive a hidden
     * first parameter xrt_closure_t *_cl for per-closure upvalue access. */
    bool has_cl = (f->ncaptures > 0);
    bool typed_abi = cg_func_uses_typed_abi(ctx, f);
    fprintf(out, "static ");
    if (!emit_class_native_return_type(ctx, out, prefix, f))
        fprintf(out, "%s", ctype_str(cg_func_return_abi_rep(ctx, f)));
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
        if (f->blocks[bi] && !cg_structured_counted_loop_block_is_elided(f, f->blocks[bi]))
            emit_block(ctx, out, f, f->blocks[bi], prefix);
    }

    fprintf(out, "}\n\n");

    if (cg_class_func_uses_native_receiver(ctx, f)) {
        emit_class_native_boxed_adapter(ctx, out, prefix, f);
    } else if (typed_abi) {
        fprintf(out, "static XrValue ");
        emit_typed_abi_fname(ctx, out, prefix, f);
        fprintf(out, "(xrt_closure_t *_cl");
        for (uint16_t i = 0; i < f->nparams; i++)
            fprintf(out, ", XrValue p%u", i);
        fprintf(out, ") {\n");
        fprintf(out, "    return ");
        bool wrapped = emit_conversion_prefix(out, f->return_type, cg_func_return_abi_rep(ctx, f),
                                              XR_REP_TAGGED);
        emit_fname(ctx, out, prefix, f);
        fprintf(out, "(_cl");
        for (uint16_t i = 0; i < f->nparams; i++) {
            fprintf(out, ", ");
            XrRep param_rep = cg_func_param_abi_rep(ctx, f, i);
            if (param_rep == XR_REP_F64)
                fprintf(out, "XR_TO_FLOAT(p%u)", i);
            else if (param_rep == XR_REP_I64)
                fprintf(out, "XR_TO_INT(p%u)", i);
            else
                fprintf(out, "p%u", i);
        }
        fprintf(out, ")");
        emit_conversion_suffix(out, wrapped);
        fprintf(out, ";\n");
        fprintf(out, "}\n\n");
    }

    if (cg_func_needs_sync_go_wrapper_ctx(ctx, f))
        emit_sync_go_wrapper(ctx, out, f, prefix);
}

/* ========== Forward Declarations ========== */

static void emit_forward_decls(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    /* Recurse children first */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            emit_forward_decls(ctx, out, f->children[i], prefix);
    }

    fprintf(out, "static ");
    if (!emit_class_native_return_type(ctx, out, prefix, f))
        fprintf(out, "%s", ctype_str(cg_func_return_abi_rep(ctx, f)));
    fprintf(out, " ");
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "(");
    fprintf(out, "xrt_closure_t *_cl");
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, ", "), emit_class_native_param_decl(ctx, out, prefix, f, i);
    fprintf(out, ");\n");

    if (cg_class_func_uses_native_receiver(ctx, f) || cg_func_uses_typed_abi(ctx, f)) {
        fprintf(out, "static XrValue ");
        emit_typed_abi_fname(ctx, out, prefix, f);
        fprintf(out, "(xrt_closure_t *_cl");
        for (uint16_t i = 0; i < f->nparams; i++)
            fprintf(out, ", XrValue p%u", i);
        fprintf(out, ");\n");
    }

    bool needs_aot_coro = cg_func_needs_aot_coro_ctx(ctx, f);
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
