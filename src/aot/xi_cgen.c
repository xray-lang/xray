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
#include "xaot_callable.h"
#include "xaot_link.h"
#include "xaot_class_native.h"
#include "xaot_rep_gen.h"
#include "xaot_abi_gen.h"
#include "xaot_layout_gen.h"
#include "xaot_struct_name.h"
#include "xi_backend_plan_contract.h"
#include "xi_to_c_dispatch_gen.h"
#include "xi_to_c_stmt_dispatch_gen.h"
#include "../ir/xi_analysis.h"
#include "../ir/xi_backend_lower.h"
#include "../shared/xr_array_core.h"
#include "../shared/xr_derive_flags.h"
#include "../shared/xr_hash_core.h"
#include "../shared/xr_swiss_index.h"
#include "../ir/xi_op_name.h"
#include "../ir/xi_ops_gen.h"
#include "../ir/xi_opt.h"
#include "../ir/xi_own.h"
#include "../ir/xi_escape.h"
#include "../ir/xi_range.h"
#include "../ir/xi_value_query.h"
#include "../ir/xi_coro_analyze.h"
#include "../base/xdefs.h"
#include "../module/xnative_package.h"
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
#include "../base/xutf8.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xtype_ref.h"
#include "../frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../frontend/analyzer/xanalyzer_builtins.h"
#include "../frontend/analyzer/xa_alloc_effect.h"
#include "../frontend/analyzer/xa_intrinsic_registry.h"
#include "../frontend/analyzer/xconsteval.h"
#include "../frontend/analyzer/xa_selection.h"
#include "../stdlib/xstdlib_defs_generated.h"
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
static bool cg_value_skips_predecl(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);

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

static const char *cg_native_int_ctype(uint8_t scalar_rep) {
    return xaot_c_type_for_native_int_type(scalar_rep);
}

static uint8_t cg_narrow_int_scalar_rep(const XiValue *v) {
    if (!v || !xi_generated_op_result_native_type(v->op) || !v->type ||
        v->type->kind != XR_KIND_INT || v->type->scalar_rep == XR_NATIVE_I64)
        return XR_SCALAR_REP_NONE;
    return v->type->scalar_rep;
}

static bool cg_const_int_fits_scalar_rep(int64_t value, uint8_t scalar_rep) {
    return xaot_native_int_const_fits(scalar_rep, value);
}

static bool cg_value_narrow_local_scalar_rep(const XiValue *v, uint8_t depth,
                                             uint8_t *out_scalar_rep) {
    if (!v || cg_rep(v) != XR_REP_I64 || depth > 8)
        return false;

    uint8_t op_width = cg_narrow_int_scalar_rep(v);
    if (op_width != XR_SCALAR_REP_NONE) {
        if (out_scalar_rep)
            *out_scalar_rep = op_width;
        return true;
    }

    if (v->op != XI_PHI)
        return false;

    uint8_t phi_width = XR_SCALAR_REP_NONE;
    if (v->type && v->type->kind == XR_KIND_INT && v->type->scalar_rep != XR_NATIVE_I64 &&
        cg_native_int_ctype(v->type->scalar_rep))
        phi_width = v->type->scalar_rep;

    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg)
            return false;
        if (arg->op == XI_CONST)
            continue;
        uint8_t arg_width = 0;
        if (!cg_value_narrow_local_scalar_rep(arg, (uint8_t) (depth + 1), &arg_width) ||
            !cg_native_int_ctype(arg_width))
            return false;
        if (phi_width == XR_SCALAR_REP_NONE)
            phi_width = arg_width;
        else if (arg_width != phi_width)
            return false;
    }

    if (phi_width == XR_SCALAR_REP_NONE)
        return false;

    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (arg && arg->op == XI_CONST && !cg_const_int_fits_scalar_rep(arg->aux_int, phi_width))
            return false;
    }

    if (out_scalar_rep)
        *out_scalar_rep = phi_width;
    return true;
}

static const char *local_ctype_str(const XiValue *v) {
    uint8_t scalar_rep = XR_SCALAR_REP_NONE;
    if (cg_value_narrow_local_scalar_rep(v, 0, &scalar_rep)) {
        const char *ctype = cg_native_int_ctype(scalar_rep);
        if (ctype)
            return ctype;
    }
    return ctype_str(cg_rep(v));
}

static const XiValue *cg_unwrap_identity_value(const XiValue *v) {
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v) ||
            xi_op_is_identity_forward(v->op)) &&
           v->nargs >= 1) {
        v = v->args[0];
    }
    return v;
}

static const char *cg_unsigned_narrow_cast_ctype(uint16_t op) {
    const char *ctype = xi_to_c_template_width_cast_type(op);
    if (!ctype || !*ctype)
        return NULL;
    return (op == XI_NARROW_U8 || op == XI_NARROW_U16 || op == XI_NARROW_U32) ? ctype : NULL;
}

static bool cg_op_is_lowbits_binop(uint16_t op) {
    const char *arith = xi_to_c_template_arith_native_op(op);
    if (arith && *arith)
        return true;
    const char *bitwise = xi_to_c_template_bitwise_binary_op(op);
    return bitwise && *bitwise;
}

static const XiValue *cg_unsigned_narrow_lowbits_binop_arg(const XiValue *v) {
    if (!v || v->nargs < 1 || !v->args[0] || !cg_unsigned_narrow_cast_ctype(v->op))
        return NULL;

    const XiValue *arg = cg_unwrap_identity_value(v->args[0]);
    if (!arg || !cg_op_is_lowbits_binop(arg->op) || arg->nargs < 2 || cg_rep(arg) != XR_REP_I64 ||
        cg_rep(arg->args[0]) != XR_REP_I64 || cg_rep(arg->args[1]) != XR_REP_I64)
        return NULL;
    return arg;
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
        if (((xi_copy_is_identity_alias(cur) || xi_op_is_identity_forward(cur->op)) &&
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
                    case XI_SOURCE_MOVE:
                    case XI_OWNER_FORWARD:
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

static bool cg_ownership_op_is_noop(bool freestanding_profile, const XiValue *v) {
    if (!v || (v->op != XI_RETAIN && v->op != XI_RELEASE) || v->nargs < 1)
        return false;
    const XiValue *arg = cg_unwrap_identity_value(v->args[0]);
    if (freestanding_profile && arg && arg->type)
        return !xr_type_is_builtin_named_class(arg->type, "Buffer");
    return arg && cg_type_has_no_aot_arc_header(arg->type);
}

static bool cg_method_name_is(const XiValue *v, const char *name, int symbol) {
    if (!v || !name)
        return false;
    const char *method = v->aux ? (const char *) v->aux : NULL;
    if (method && strcmp(method, name) == 0)
        return true;
    if (v->aux_int <= 0)
        return false;
    return (int) (v->aux_int >> 1) == symbol;
}

static bool cg_builtin_receiver_pod_span_elem(const XrType *type) {
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

static bool cg_builtin_receiver_registry_matches(const XrType *receiver_type,
                                                 XaBuiltinReceiverKind kind) {
    switch (kind) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
            return receiver_type && receiver_type->kind == XR_KIND_INT &&
                   !receiver_type->is_nullable;
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return xr_type_is_exact_unsigned_integer(receiver_type);
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(receiver_type);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return receiver_type && receiver_type->kind == XR_KIND_ARRAY;
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(receiver_type);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return receiver_type && receiver_type->kind == XR_KIND_SLICE &&
                   cg_builtin_receiver_pod_span_elem(receiver_type->container.element_type);
    }
    return false;
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

/* Xi use counters are optimization metadata and can conservatively retain an
 * old use after DCE.  C emission decisions need the final graph, so query the
 * actual control/phi/value edges instead of treating `uses` as authoritative. */
static bool cg_value_has_actual_ir_use(const XiFunc *f, const XiValue *target) {
    if (!f || !target)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return true;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] == target)
                    return true;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == target)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] == target)
                    return true;
            }
        }
    }
    return false;
}

static const XaotBundle *cg_ctx_aot_bundle(const XiCgenCtx *ctx);
static void cg_ctx_set_error(XiCgenCtx *ctx);

static const XaotSliceAccessPlan *cg_span_access_plan(XiCgenCtx *ctx, const XiValue *value,
                                                      uint8_t kind) {
    const XiValue *origin = cg_unwrap_identity_value(value);
    const XaotSliceAccessPlan *plan =
        xaot_bundle_find_span_access_plan(cg_ctx_aot_bundle(ctx), origin);
    return plan && plan->kind == kind ? plan : NULL;
}

static bool cg_span_plan_drops(XiCgenCtx *ctx, const XiValue *value, uint8_t kind, uint32_t drops) {
    const XaotSliceAccessPlan *plan = cg_span_access_plan(ctx, value, kind);
    return plan && (plan->eliminated_checks & drops) == drops;
}

static bool cg_span_plan_readonly_proven(XiCgenCtx *ctx, const XiValue *value, uint8_t kind) {
    const XaotSliceAccessPlan *plan = cg_span_access_plan(ctx, value, kind);
    return plan && (plan->evidence & XAOT_SLICE_EV_READONLY) != 0;
}

static bool cg_emit_span_readonly_void_trap(XiCgenCtx *ctx, FILE *out, const XiValue *value,
                                            uint8_t kind) {
    if (!cg_span_plan_readonly_proven(ctx, value, kind))
        return false;
    fprintf(out, "({ xrt_throw_error(XR_ERR_CMP_CONST_ASSIGN, "
                 "XR_ERROR_CORE_BYTE_SLICE_READONLY_MSG); XR_NULL_VAL; })");
    return true;
}

static bool cg_emit_span_readonly_span_trap(XiCgenCtx *ctx, FILE *out, const XiValue *value,
                                            uint8_t kind) {
    if (!cg_span_plan_readonly_proven(ctx, value, kind))
        return false;
    fprintf(out, "({ xrt_throw_error(XR_ERR_CMP_CONST_ASSIGN, "
                 "XR_ERROR_CORE_BYTE_SLICE_READONLY_MSG); (xr_span_t){0}; })");
    return true;
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

static const XiImportRef *cg_shared_slot_import_ref_depth(const XiFunc *f, int slot, int depth) {
    if (!f || slot < 0 || depth > 8)
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
            const XiValue *source = cg_unwrap_identity_value(v->args[0]);
            if (source && source->op == XI_GET_SHARED && (int) source->aux_int != slot) {
                ref = cg_shared_slot_import_ref_depth(f, (int) source->aux_int, depth + 1);
                if (ref)
                    return ref;
            }
        }
    }
    return NULL;
}

static const XiImportRef *cg_shared_slot_import_ref(const XiFunc *f, int slot) {
    return cg_shared_slot_import_ref_depth(f, slot, 0);
}

static const XiConstLiteral *cg_module_const_literal(const XiModule *module, int64_t slot);

static const XiImportRef *cg_module_slot_import_ref(const XiModule *module, int slot) {
    if (!module || slot < 0 || slot >= module->nslots || !module->slot_imports)
        return NULL;
    return module->slot_imports[slot];
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
    const char *class_name; /* owned: XiClassData.class_name (Xi arena); owning class "Rect" */
    const char *name;       /* owned: XiFunc/method name (Xi arena); e.g. "area" */
    const XiFunc *func;
    const char *module_prefix; /* owned: CgImportEntry.target_mod_name (Xi arena) or NULL */
    const XiClassData *class_data;
    const XrAggregateLayout *instance_layout;
} CgMethodEntry;

typedef struct {
    const char *module_path;         /* owned: XiCgenCtx shared import-path storage */
    const char *member_name;         /* owned: XiModuleExport.name (Xi arena) */
    const char *target_mod_name;     /* owned: XiModule.name (Xi arena); C ident prefix */
    int shared_slot;                 /* slot in target's xrt_shared_<mod>[] */
    const XiFunc *target_func;       /* XiFunc* if this export is a function (for direct calls) */
    const XiClassData *target_class; /* XiClassData* if this export is a class */
    const XiEnumData *target_enum;   /* XiEnumData* if this export is an enum */
    const XiFunc *exporter_func;     /* exporter module XiFunc (for class child resolution) */
} CgImportEntry;

typedef struct {
    const char
        *name; /* owned: XrAggregateLayout field name (type pool); cache reset per function */
    const XrType *type;
    XrRep rep;
    bool dirty;
    int16_t layout_index;
} CgClassFieldCacheEntry;

typedef struct {
    bool active;
    bool native_receiver;
    const XiValue *receiver;
    const XrAggregateLayout *layout;
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
    const char *ctor_prefix; /* owned: XiModule prefix (Xi arena) or NULL; see ctor_call_data */
    const XiValue *ctor_call;
} CgSharedNativeInstance;

typedef struct {
    bool active;
    const XiModule *module;
    const char *module_name; /* owned: n/a (zero-initialized, never assigned a live borrow) */
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

typedef struct CgFuncReachMemo {
    const XiFunc *func;
    uint8_t state; /* 1 = computing, 2 = done */
    bool reachable;
} CgFuncReachMemo;

typedef struct CgSharedSlotReachMemo {
    const XiModule *module;
    int slot;
    uint8_t state; /* 1 = computing, 2 = done */
    bool has_get;
} CgSharedSlotReachMemo;

typedef enum CgWriterPhase {
    CG_WRITER_PHASE_IDLE = 0,
    CG_WRITER_PHASE_COLLECT,
    CG_WRITER_PHASE_INCLUDES,
    CG_WRITER_PHASE_TYPES,
    CG_WRITER_PHASE_EXTERN_DECLS,
    CG_WRITER_PHASE_INTERNAL_DECLS,
    CG_WRITER_PHASE_STATIC_DATA,
    CG_WRITER_PHASE_BODIES,
    CG_WRITER_PHASE_GLUE,
    CG_WRITER_PHASE_FINALIZED,
} CgWriterPhase;

/* Phase guard for the declaration paths migrated by task 208.  The broader
 * emitter still uses FILE helpers, but extern/type declarations can no longer
 * be discovered or written while a body is streaming. */
typedef struct CgWriter {
    FILE *out;
    CgWriterPhase phase;
    const XiFunc *current_func;
    int brace_depth;
    bool error;
} CgWriter;

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
    const XiFunc **emitted_funcs;
    char **emitted_func_names;
    int nemitted_funcs;
    int emitted_funcs_cap;
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
    /* Static "xrt_shared" literal, or a function-local buffer that is reset back
     * to the static literal before that function returns (see
     * cg_emit_module_definitions); never escapes as a dangling stack pointer. */
    const char *shared_name; /* owned: static literal / contained fn-local buffer (see above) */
    CgImportEntry *imports;
    int imports_cap;
    int nimports;
    XiModule **all_modules; /* full modules array for resolved-index lookups */
    int all_nmodules;
    bool emit_main;
    bool freestanding_profile;
    XiCgenCDialect c_dialect;
    XiCgenTypeNameProfile type_name_profile;
    bool error; /* set on fatal codegen errors (unknown builtin, etc.) */
    XiCgenStats stats;
    /* Per-function abstraction-cost residue records (task 217 P2).  Grown as
     * each body is emitted; consumed by --dump-residue and contract verification.
     * want_residue gates the per-function C capture (off = no overhead). */
    bool want_residue;
    XiFuncResidue *func_residues;
    size_t nfunc_residues;
    size_t func_residues_cap;
    XiCgenCoroFrameStats coro_frame_stats;
    const XaotBundle *aot_bundle;
    const XaotTarget *target;
    bool simd_active;
    CgWriter writer;
    uint8_t *used_extern_decls;    /* stable_id - 1, reset for each translation unit */
    uint8_t *extern_decl_adapters; /* used declarations needing a boxed closure entry */
    uint32_t used_extern_decl_cap;
    /* Per-translation-unit unit-enum sidecars.  Body emission marks the
     * concrete enum plans that actually cross a tagged boundary; static-data
     * emission then writes exactly one immutable layout for each marked plan. */
    uint8_t *enum_scalar_sidecar_used;
    uint32_t enum_scalar_sidecar_cap;
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
    CgFuncReachMemo *func_reach_memo;
    int nfunc_reach_memo;
    int func_reach_memo_cap;
    bool func_reachability_valid;
    bool func_reachability_computing;
    CgSharedSlotReachMemo *shared_slot_reach_memo;
    int nshared_slot_reach_memo;
    int shared_slot_reach_memo_cap;
};

static void cg_reachability_cache_clear(XiCgenCtx *ctx) {
    if (!ctx)
        return;
    ctx->nfunc_reach_memo = 0;
    ctx->nshared_slot_reach_memo = 0;
    ctx->func_reachability_valid = false;
    ctx->func_reachability_computing = false;
}

static void cg_ctx_set_error(XiCgenCtx *ctx) {
    if (ctx)
        ctx->error = true;
}

static const XiImportRef *cg_shared_slot_import_ref_ctx(const XiCgenCtx *ctx, const XiFunc *f,
                                                        int slot) {
    const XiImportRef *ref = cg_shared_slot_import_ref(f, slot);
    if (!ref && f && f->module && f->module->init != f)
        ref = cg_shared_slot_import_ref(f->module->init, slot);
    if (!ref && f && f->module)
        ref = cg_module_slot_import_ref(f->module, slot);
    if (!ref && ctx && ctx->module && ctx->module->init && ctx->module->init != f)
        ref = cg_shared_slot_import_ref(ctx->module->init, slot);
    if (!ref && ctx && ctx->module)
        ref = cg_module_slot_import_ref(ctx->module, slot);
    return ref;
}

static const XiModule *cg_import_ref_target_module(const XiCgenCtx *ctx, const XiImportRef *ref,
                                                   int64_t *out_slot) {
    if (out_slot)
        *out_slot = -1;
    if (!ctx || !ref || ref->resolved_mod_index < 0 || ref->resolved_shared_slot < 0 ||
        ref->resolved_mod_index >= ctx->all_nmodules || !ctx->all_modules)
        return NULL;
    const XiModule *module = ctx->all_modules[ref->resolved_mod_index];
    if (!module || ref->resolved_shared_slot >= module->nslots)
        return NULL;
    if (out_slot)
        *out_slot = ref->resolved_shared_slot;
    return module;
}

static const XiConstLiteral *cg_import_ref_target_const_literal(const XiCgenCtx *ctx,
                                                                const XiImportRef *ref,
                                                                const XiModule **out_module,
                                                                int64_t *out_slot) {
    if (out_module)
        *out_module = NULL;
    int64_t slot = -1;
    const XiModule *module = cg_import_ref_target_module(ctx, ref, &slot);
    const XiConstLiteral *lit = cg_module_const_literal(module, slot);
    if (!lit || lit->kind == XI_CONST_LITERAL_NONE)
        return NULL;
    if (out_module)
        *out_module = module;
    if (out_slot)
        *out_slot = slot;
    return lit;
}

static const XiConstLiteral *cg_import_slot_const_literal(const XiCgenCtx *ctx, const XiFunc *f,
                                                          int slot, const XiModule **out_module,
                                                          int64_t *out_slot) {
    const XiImportRef *ref = cg_shared_slot_import_ref_ctx(ctx, f, slot);
    return cg_import_ref_target_const_literal(ctx, ref, out_module, out_slot);
}

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

static bool cg_reserve_emitted_funcs(XiCgenCtx *ctx, int need) {
    if (need <= ctx->emitted_funcs_cap)
        return true;
    int nc = ctx->emitted_funcs_cap > 0 ? ctx->emitted_funcs_cap : 64;
    while (nc < need)
        nc *= 2;
    const XiFunc **items =
        (const XiFunc **) xr_realloc(ctx->emitted_funcs, (size_t) nc * sizeof(*items));
    if (!items) {
        ctx->error = true;
        return false;
    }
    ctx->emitted_funcs = items;
    char **names = (char **) xr_realloc(ctx->emitted_func_names, (size_t) nc * sizeof(*names));
    if (!names) {
        ctx->error = true;
        return false;
    }
    memset(&items[ctx->emitted_funcs_cap], 0,
           (size_t) (nc - ctx->emitted_funcs_cap) * sizeof(*items));
    memset(&names[ctx->emitted_funcs_cap], 0,
           (size_t) (nc - ctx->emitted_funcs_cap) * sizeof(*names));
    ctx->emitted_func_names = names;
    ctx->emitted_funcs_cap = nc;
    return true;
}

static bool cg_func_c_name(XiCgenCtx *ctx, const char *prefix, const XiFunc *f, char *buf,
                           size_t bufsz);

static void cg_reset_emitted_funcs(XiCgenCtx *ctx) {
    if (!ctx)
        return;
    for (int i = 0; i < ctx->nemitted_funcs; i++) {
        xr_free(ctx->emitted_func_names ? ctx->emitted_func_names[i] : NULL);
        if (ctx->emitted_func_names)
            ctx->emitted_func_names[i] = NULL;
        if (ctx->emitted_funcs)
            ctx->emitted_funcs[i] = NULL;
    }
    ctx->nemitted_funcs = 0;
}

static bool cg_mark_func_emitted(XiCgenCtx *ctx, const XiFunc *f, const char *prefix) {
    if (!ctx || !f)
        return false;
    char cname[384];
    bool have_cname = cg_func_c_name(ctx, prefix, f, cname, sizeof(cname));
    for (int i = 0; i < ctx->nemitted_funcs; i++) {
        if (ctx->emitted_funcs[i] == f)
            return false;
        if (have_cname && ctx->emitted_func_names && ctx->emitted_func_names[i] &&
            strcmp(ctx->emitted_func_names[i], cname) == 0)
            return false;
        const XiFunc *seen = ctx->emitted_funcs[i];
        if (seen && f->cgen_id > 0 && seen->cgen_id == f->cgen_id &&
            ((seen->name == f->name) ||
             (seen->name && f->name && strcmp(seen->name, f->name) == 0)))
            return false;
    }
    if (!cg_reserve_emitted_funcs(ctx, ctx->nemitted_funcs + 1))
        return false;
    char *saved_name = have_cname ? xr_strdup(cname) : NULL;
    if (have_cname && !saved_name) {
        ctx->error = true;
        return false;
    }
    int slot = ctx->nemitted_funcs++;
    ctx->emitted_funcs[slot] = f;
    ctx->emitted_func_names[slot] = saved_name;
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
static bool cg_func_contains_stack_array(const XiFunc *f);
static bool cg_func_stack_arrays_force_inline_safe(const XiFunc *f);
static bool cg_func_should_noinline(const XiFunc *f);
static bool cg_func_should_force_inline(XiCgenCtx *ctx, const XiFunc *f);
static bool cg_func_has_native_vector_width(const XiCgenCtx *ctx, const XiFunc *f, uint8_t width);
static bool cg_func_requires_x86_vector_target(const XiCgenCtx *ctx, const XiFunc *f,
                                               uint8_t width);
static bool cg_func_requires_static_x86_inline_target(const XiCgenCtx *ctx, const XiFunc *f);

static bool cg_func_is_par_for_native_callback(const XiFunc *f) {
    return f && (f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_FOR_I64 ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_REDUCE_I64_BODY ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_REDUCE_I64_COMBINE ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_BODY ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_COMBINE ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_MAP_SCALAR_BODY ||
                 f->native_callback_kind == XI_NATIVE_CALLBACK_PAR_RANGE_I64);
}

static const char *cg_func_linkage(XiCgenCtx *ctx, const XiFunc *f, const char *prefix) {
    if (cg_func_requires_x86_vector_target(ctx, f, 64) ||
        cg_func_requires_x86_vector_target(ctx, f, 32) ||
        cg_func_requires_static_x86_inline_target(ctx, f)) {
        if (cg_func_needs_external_linkage(ctx, f, prefix)) {
            if (cg_func_should_noinline(f))
                return "XRT_INTERNAL XR_NOINLINE ";
            /* In a static SIMD build every caller is compiled for the selected
             * target, so an explicit source-level @inline contract is safe to
             * preserve across module ABI boundaries.  Runtime dispatch must
             * keep the externally linkable ISA island: inlining its body into
             * a baseline caller would leak target-specific instructions. */
            if (ctx && ctx->target && ctx->target->simd_mode != XAOT_SIMD_DISPATCH && f &&
                f->inline_policy == XI_INLINE_PREFER)
                return "XRT_INTERNAL XR_FORCEINLINE ";
            return "XRT_INTERNAL ";
        }
        if (cg_func_should_noinline(f))
            return "static XR_NOINLINE ";
        /* A direct-vector leaf is already inside the selected ISA island.
         * Preserve its normal always-inline policy so hot stripe kernels fold
         * into same-target loops.  Propagation-only callers remain ordinary
         * functions: they are the stable boundary a baseline caller may enter. */
        if (ctx && ctx->target && ctx->target->simd_mode != XAOT_SIMD_DISPATCH &&
            (cg_func_has_native_vector_width(ctx, f, 64) ||
             cg_func_has_native_vector_width(ctx, f, 32)) &&
            cg_func_should_force_inline(ctx, f))
            return "static XR_AINLINE ";
        return "static ";
    }
    if (cg_func_is_par_for_native_callback(f))
        return "static XR_AINLINE ";
    if (cg_func_needs_external_linkage(ctx, f, prefix)) {
        if (cg_func_should_noinline(f))
            return "XRT_INTERNAL XR_NOINLINE ";
        return cg_func_should_force_inline(ctx, f) ? "XRT_INTERNAL XR_FORCEINLINE "
                                                   : "XRT_INTERNAL ";
    }
    if (ctx && ctx->extern_linkage) {
        if (cg_func_should_noinline(f))
            return "static XR_NOINLINE ";
        if (f && f->inline_policy == XI_INLINE_PREFER)
            return "static XR_AINLINE ";
        if (cg_func_contains_stack_array(f) && !cg_func_stack_arrays_force_inline_safe(f))
            return "static XR_NOINLINE ";
        return cg_func_should_force_inline(ctx, f) ? "static XR_AINLINE " : "static ";
    }
    if (cg_func_should_noinline(f))
        return "static XR_NOINLINE ";
    if (f && f->inline_policy == XI_INLINE_PREFER)
        return "static XR_AINLINE ";
    return cg_linkage(ctx);
}

/* A declaration emitted into a different translation unit cannot promise
 * always_inline: the function body is intentionally unavailable there and GCC
 * diagnoses a call through such a declaration at -O0.  Keep the definition's
 * inline policy in its owning unit, but expose only the stable cross-unit ABI
 * linkage to importers. */
static const char *cg_func_forward_linkage(XiCgenCtx *ctx, const XiFunc *f, const char *prefix,
                                           bool cross_module) {
    const char *linkage = cg_func_linkage(ctx, f, prefix);
    if (cross_module && strcmp(linkage, "XRT_INTERNAL XR_FORCEINLINE ") == 0)
        return "XRT_INTERNAL ";
    return linkage;
}

/* Emit a string literal value expression: a pointer to the module-level
 * static xrt_str_t emitted by xi_cgen_emit_str_literal_defs. */
static void cg_emit_str_value(XiCgenCtx *ctx, FILE *out, const char *s) {
    fprintf(out, "xr_str_lit(&_xstr_%d)", cg_intern_str_lit(ctx, s));
}

static void cg_emit_static_str_value_initializer(XiCgenCtx *ctx, FILE *out, const char *s) {
    fprintf(out, "(XrValue){.tag = XR_TAG_STR, .ptr = (void *)&_xstr_%d}",
            cg_intern_str_lit(ctx, s));
}

static const XaotBundle *cg_ctx_aot_bundle(const XiCgenCtx *ctx) {
    return ctx ? ctx->aot_bundle : NULL;
}

static void cg_writer_reset(XiCgenCtx *ctx) {
    if (!ctx)
        return;
    memset(&ctx->writer, 0, sizeof(ctx->writer));
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    uint32_t need = bundle ? bundle->nextern_decls : 0;
    if (need > ctx->used_extern_decl_cap) {
        uint8_t *used =
            (uint8_t *) xr_realloc(ctx->used_extern_decls, sizeof(uint8_t) * (size_t) need);
        uint8_t *adapters =
            (uint8_t *) xr_realloc(ctx->extern_decl_adapters, sizeof(uint8_t) * (size_t) need);
        if (!used || !adapters) {
            if (used)
                ctx->used_extern_decls = used;
            if (adapters)
                ctx->extern_decl_adapters = adapters;
            ctx->error = true;
            ctx->writer.error = true;
            return;
        }
        ctx->used_extern_decls = used;
        ctx->extern_decl_adapters = adapters;
        ctx->used_extern_decl_cap = need;
    }
    if (ctx->used_extern_decls && ctx->used_extern_decl_cap > 0) {
        memset(ctx->used_extern_decls, 0, ctx->used_extern_decl_cap);
        memset(ctx->extern_decl_adapters, 0, ctx->used_extern_decl_cap);
    }
    ctx->writer.phase = CG_WRITER_PHASE_COLLECT;
}

static bool cg_writer_enter(XiCgenCtx *ctx, FILE *out, CgWriterPhase phase) {
    if (!ctx || !out || ctx->error || ctx->writer.error || phase <= CG_WRITER_PHASE_COLLECT ||
        phase >= CG_WRITER_PHASE_FINALIZED ||
        (ctx->writer.phase > CG_WRITER_PHASE_COLLECT && phase < ctx->writer.phase)) {
        if (ctx) {
            ctx->error = true;
            ctx->writer.error = true;
        }
        return false;
    }
    ctx->writer.out = out;
    ctx->writer.phase = phase;
    return true;
}

static bool cg_mark_extern_decl_used(XiCgenCtx *ctx, const XiFunc *func,
                                     const XaotExternDecl **out_decl) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XaotExternDecl *decl = xaot_bundle_find_extern_decl_for_func(bundle, func);
    if (out_decl)
        *out_decl = decl;
    if (!ctx || !decl || decl->stable_id == 0 || decl->stable_id > ctx->used_extern_decl_cap) {
        if (ctx)
            ctx->error = true;
        return false;
    }
    ctx->used_extern_decls[decl->stable_id - 1] = 1;
    return true;
}

static bool cg_mark_extern_decl_adapter_used(XiCgenCtx *ctx, const XiFunc *func,
                                             const XaotExternDecl **out_decl) {
    const XaotExternDecl *decl = NULL;
    if (!cg_mark_extern_decl_used(ctx, func, &decl))
        return false;
    ctx->extern_decl_adapters[decl->stable_id - 1] = 1;
    if (out_decl)
        *out_decl = decl;
    return true;
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
                    /* ctx->methods storage is reused across module emission.
                     * Local registrations must clear a prefix left by an
                     * imported class in the previous module, otherwise a
                     * same-named method is emitted with the stale module
                     * owner. Imported registrations set the real prefix in
                     * cg_register_imported_classes immediately afterwards. */
                    ctx->methods[ctx->nmethod].module_prefix = NULL;
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

static int cg_find_class_slot_by_data(const XiCgenCtx *ctx, const XiClassData *cd) {
    if (!ctx || !ctx->module || !cd || !ctx->module->slot_classes)
        return -1;
    for (uint16_t s = 0; s < ctx->module->nslots; s++) {
        const XiClassData *slot_cd = ctx->module->slot_classes[s];
        if (slot_cd == cd)
            return (int) s;
        if (slot_cd && slot_cd->class_name && cd->class_name &&
            strcmp(slot_cd->class_name, cd->class_name) == 0)
            return (int) s;
    }
    return -1;
}

static bool cg_class_data_is_exported(const XiCgenCtx *ctx, const XiClassData *cd) {
    int slot = cg_find_class_slot_by_data(ctx, cd);
    if (!ctx || !ctx->module || !cd)
        return false;
    for (uint16_t e = 0; e < ctx->module->nexports; e++) {
        const XiModuleExport *exp = &ctx->module->exports[e];
        if (exp->class_data == cd)
            return true;
        if (slot >= 0 && exp->shared_slot == (uint16_t) slot && exp->name)
            return true;
    }
    return false;
}

static bool cg_emit_type_name_for_class(const XiCgenCtx *ctx, const XiClassData *cd) {
    if (!ctx || !cd)
        return false;
    switch (ctx->type_name_profile) {
        case XI_CGEN_TYPE_NAMES_ALL:
            return true;
        case XI_CGEN_TYPE_NAMES_PUBLIC:
            return cg_class_data_is_exported(ctx, cd);
        case XI_CGEN_TYPE_NAMES_NONE:
        default:
            return false;
    }
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

static bool cg_func_c_name(XiCgenCtx *ctx, const char *prefix, const XiFunc *f, char *buf,
                           size_t bufsz) {
    if (!ctx || !f || !buf || bufsz == 0)
        return false;

    bool have_prefix = prefix && prefix[0];
    if (ctx->extern_linkage && have_prefix && cg_func_stable_cname(ctx, f, prefix, buf, bufsz))
        return true;

    char prefix_buf[128];
    if (have_prefix)
        sanitize_c_ident_part(prefix_buf, sizeof(prefix_buf), prefix);

    const char *raw = f->name ? f->name : "anon";
    char name_buf[128];
    sanitize_c_ident_part(name_buf, sizeof(name_buf), raw);

    XiFunc *mf = (XiFunc *) (uintptr_t) f;
    if (mf->cgen_id == 0)
        mf->cgen_id = ++ctx->fname_counter;

    if (have_prefix)
        snprintf(buf, bufsz, "%s_%s_%d", prefix_buf, name_buf, f->cgen_id);
    else
        snprintf(buf, bufsz, "fn_%s_%d", name_buf, f->cgen_id);
    return true;
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

    /* Record cross-module references so the unit forward-declares only the
     * imports it uses (114): a reference is cross-module when its owning prefix
     * differs from the module currently being emitted. */
    if (ctx->extern_linkage && ctx->collect_xmod_refs && have_prefix && ctx->module &&
        ctx->module->name && strcmp(prefix, ctx->module->name) != 0)
        cg_note_xmod_ref(ctx, f, prefix);

    char cname[384];
    if (cg_func_c_name(ctx, prefix, f, cname, sizeof(cname)))
        fprintf(out, "%s", cname);
}

static void emit_fname_suffix(XiCgenCtx *ctx, FILE *out, const char *prefix, const XiFunc *f,
                              const char *suffix) {
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "%s", suffix ? suffix : "");
}

typedef struct {
    const XiFunc *func;
    const char *prefix; /* owned: C-name prefix (Xi arena/static); local struct, emit-scope only */
    bool is_class_constructor;
    const XiClassData *class_data;
} CgStaticFunctionCall;

static bool cg_func_needs_aot_coro_ctx(XiCgenCtx *ctx, const XiFunc *f);
static bool cg_coro_value_needs_frame(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);
static bool cg_coro_value_needs_frame_arc_release(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *v);
static const XiFunc *cg_class_native_resolve_method_call(XiCgenCtx *ctx, const XiFunc *current,
                                                         const XiValue *call,
                                                         const char **out_prefix);
static const XiFunc *cg_lookup_class_ctor_global(XiCgenCtx *ctx, const char *class_name,
                                                 const char **out_prefix);
static bool cg_aot_stdlib_receiver_call_is_direct(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *call);
static bool cg_aot_stdlib_import_call_is_direct(XiCgenCtx *ctx, const XiFunc *f,
                                                const XiValue *call);

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

static const XiModule *cg_module_for_func(const XiCgenCtx *ctx, const XiFunc *target) {
    if (!ctx || !target || !ctx->all_modules || ctx->all_nmodules <= 0)
        return target->module;
    for (int i = 0; i < ctx->all_nmodules; i++) {
        const XiModule *mod = ctx->all_modules[i];
        if (mod && mod->init && cg_func_tree_contains(mod->init, target))
            return mod;
    }
    return target->module;
}

static const char *cg_module_prefix_for_func(const XiCgenCtx *ctx, const XiFunc *target) {
    const XiModule *mod = cg_module_for_func(ctx, target);
    return mod ? mod->name : NULL;
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

static bool cg_static_function_value_use_is_direct_parallel_callback(const XiValue *user,
                                                                     uint16_t arg_idx,
                                                                     const XiFunc *target) {
    if (!user || !target)
        return false;
    if (user->op == XI_PAR_FOR && arg_idx == 3 && user->aux_kind == XI_AUX_KIND_PAR_FOR) {
        const XiParallelForData *data = (const XiParallelForData *) user->aux;
        return data && data->body_func == target;
    }
    if (user->op == XI_PAR_MAP && arg_idx == 3 && user->aux_kind == XI_AUX_KIND_PAR_MAP) {
        const XiParallelMapData *data = (const XiParallelMapData *) user->aux;
        return data && data->body_func == target;
    }
    if (user->op == XI_PAR_REDUCE && (arg_idx == 4 || arg_idx == 5) &&
        user->aux_kind == XI_AUX_KIND_PAR_REDUCE) {
        const XiParallelReduceData *data = (const XiParallelReduceData *) user->aux;
        return data && ((arg_idx == 4 && data->body_func == target) ||
                        (arg_idx == 5 && data->combine_func == target));
    }
    return false;
}

static bool cg_shared_static_function_slot_uses_are_direct(XiCgenCtx *ctx, const XiFunc *owner,
                                                           int slot, const XiFunc *target);
static bool cg_shared_slot_has_reachable_get(XiCgenCtx *ctx, const XiModule *owner_mod, int slot);

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
                    case XI_SOURCE_MOVE:
                    case XI_OWNER_FORWARD:
                        if (ai != 0 || !cg_shared_static_function_value_uses_are_direct(
                                           ctx, owner, user, target, depth + 1))
                            return false;
                        break;
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (ai != 0)
                            return false;
                        break;
                    case XI_SET_SHARED: {
                        if (ai != 0)
                            return false;
                        int slot = (int) user->aux_int;
                        const XiModule *owner_mod = cg_module_for_func(ctx, owner);
                        if (owner_mod && owner_mod->init &&
                            !cg_shared_slot_has_reachable_get(ctx, owner_mod, slot))
                            break;
                        /* Imported bindings get a local shared slot, but that
                         * slot intentionally has no local declaration entry in
                         * shared_slot_funcs.  The IMPORT_REF already resolved
                         * `target`; prove every local read is a direct call
                         * against that target instead of requiring a duplicate
                         * slot-to-function mapping. */
                        if (!cg_shared_static_function_slot_uses_are_direct(ctx, owner, slot,
                                                                            target))
                            return false;
                        break;
                    }
                    case XI_PAR_FOR:
                    case XI_PAR_MAP:
                    case XI_PAR_REDUCE:
                        if (!cg_static_function_value_use_is_direct_parallel_callback(user, ai,
                                                                                      target))
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

static bool cg_static_function_value_uses_are_parallel_callbacks(const XiFunc *owner,
                                                                 const XiValue *value,
                                                                 const XiFunc *target) {
    if (!owner || !value || !target)
        return false;
    bool saw_callback = false;
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
                if (cg_static_function_value_use_is_direct_parallel_callback(user, ai, target)) {
                    saw_callback = true;
                    continue;
                }
                if ((user->op == XI_RETAIN || user->op == XI_RELEASE) && ai == 0)
                    continue;
                return false;
            }
        }
    }
    return saw_callback;
}

/* Canonical parallel lowering materializes a stack closure with an
 * XrAotCallableDesc even when the worker wrapper calls the typed body directly.
 * The descriptor is executable metadata, so a typed callback whose descriptor
 * names the boxed entry must retain that adapter. */
static bool cg_func_has_parallel_callback_descriptor_use(const XiFunc *owner,
                                                         const XiFunc *target) {
    if (!owner || !target)
        return false;
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            if (value && value->aux == target &&
                (value->op == XI_CLOSURE_NEW ||
                 (value->op == XI_STACK_ALLOC && value->aux_int == XI_CLOSURE_NEW)) &&
                cg_static_function_value_uses_are_parallel_callbacks(owner, value, target))
                return true;
        }
    }
    for (uint16_t ci = 0; ci < owner->nchildren; ci++) {
        if (cg_func_has_parallel_callback_descriptor_use(owner->children[ci], target))
            return true;
    }
    return false;
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
                                                                  const XiFunc *target);

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
    const XiModule *owner_mod = cg_module_for_func(ctx, current);
    if (owner_mod && owner_mod->init &&
        !cg_shared_slot_has_reachable_get(ctx, owner_mod, (int) v->aux_int))
        return true;
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
    const XiValue *origin = cg_unwrap_identity_value(v);
    if (origin && origin != v && cg_static_direct_function_closure_is_elided(ctx, current, origin))
        return true;
    return cg_shared_static_function_get_is_elided(ctx, current, v) ||
           cg_shared_static_function_set_is_elided(ctx, current, v) ||
           cg_shared_static_function_closure_is_elided(ctx, current, v);
}

/* Write a value reference: v<id> or phi<id> for phi nodes.  The active
 * function map also redirects representation-identical SSA aliases to their
 * canonical immutable C local. */
static void emit_vref(FILE *out, const XiValue *v) {
    uint32_t id = v->id;
    const XiFunc *vf = v->block ? v->block->func : NULL;
    if (vf && vf->phi_coalesce && id < vf->phi_coalesce_count)
        id = vf->phi_coalesce[id];
    if (v->op == XI_PHI) {
        fprintf(out, "phi%u", id);
    } else {
        fprintf(out, "v%u", id);
    }
}

#include "xi_cgen_class_native_meta.inc.c"
static void emit_codegen_abort_expr(FILE *out);
static void emit_c_string_literal(FILE *out, const char *s);
static bool emit_struct_aggregate_box_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *value, const char *prefix);
static bool emit_struct_aggregate_box_c_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                             const char *value_expr, const char *prefix);
static void emit_value_rhs(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                           const char *prefix);
static bool emit_thread_spawn_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const XiValue *v, const char *prefix, bool in_coro);
static XrRep cg_value_decl_storage_rep(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);
#include "xi_cgen_abi_helpers.inc.c"

static bool cg_closure_new_value_can_emit_null_for_unreachable_body(
    XiCgenCtx *ctx, const XiFunc *owner, const XiValue *ref, const XiFunc *target, int depth) {
    if (!ctx || !owner || !ref || !target)
        return false;
    if (depth > 16)
        return false;
    const XiModule *owner_mod = cg_module_for_func(ctx, owner);
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == ref)
            return false;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != ref)
                    continue;
                switch ((XiOp) user->op) {
                    case XI_SET_SHARED:
                        if (ai != 0)
                            return false;
                        if (cg_shared_static_function_set_is_elided(ctx, owner, user))
                            break;
                        if (!owner_mod || !owner_mod->init || user->aux_int < 0 ||
                            user->aux_int >= owner_mod->init->nshared ||
                            cg_shared_slot_has_reachable_get(ctx, owner_mod, (int) user->aux_int))
                            return false;
                        break;
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (ai != 0)
                            return false;
                        break;
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_COPY:
                    case XI_SOURCE_MOVE:
                    case XI_OWNER_FORWARD:
                        if (ai != 0 || !cg_closure_new_value_can_emit_null_for_unreachable_body(
                                           ctx, owner, user, target, depth + 1))
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

static void emit_aot_const_data_attrs(FILE *out, const XiConstLiteral *lit) {
    if (!out || !lit)
        return;
    if (lit->data_section) {
        fprintf(out, " XRT_ATTR_SECTION(");
        emit_c_string_literal(out, lit->data_section);
        fprintf(out, ")");
    }
    if (lit->data_weak)
        fprintf(out, " XRT_ATTR_WEAK");
    if (lit->data_used)
        fprintf(out, " XRT_ATTR_USED");
}

static bool cg_const_literal_has_data_attrs(const XiConstLiteral *lit) {
    return lit && (lit->data_section || lit->data_weak || lit->data_used);
}

static bool cg_const_literal_is_static_scalar_kind(const XiConstLiteral *lit) {
    if (!lit)
        return false;
    switch (lit->kind) {
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_FLOAT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
        case XI_CONST_LITERAL_STRING:
        case XI_CONST_LITERAL_NULL:
            return true;
        default:
            return false;
    }
}

static bool cg_const_literal_is_static_raw_scalar_kind(const XiConstLiteral *lit) {
    if (!lit || (lit->type && lit->type->is_nullable))
        return false;
    switch (lit->kind) {
        case XI_CONST_LITERAL_INT:
            return lit->type && lit->type->kind == XR_KIND_INT &&
                   lit->type->scalar_rep == XR_NATIVE_I64;
        case XI_CONST_LITERAL_FLOAT:
            return lit->type && lit->type->kind == XR_KIND_FLOAT &&
                   lit->type->scalar_rep != XR_NATIVE_F32;
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
            return true;
        default:
            return false;
    }
}

static void cg_emit_static_const_storage(FILE *out, const XiConstLiteral *lit) {
    if (lit && lit->data_mutable) {
        fprintf(out, "%s", lit->data_weak ? "" : "static ");
        return;
    }
    fprintf(out, "%s", lit && lit->data_weak ? "const " : "static const ");
}

static const XiConstLiteral *cg_module_const_literal(const XiModule *module, int64_t slot) {
    if (!module || !module->slot_const_literals || slot < 0 || slot >= module->nslots)
        return NULL;
    return &module->slot_const_literals[slot];
}

static const XiConstLiteral *cg_module_shared_slot_literal(const XiModule *module, int64_t slot) {
    if (!module || !module->slot_shared_initializers || slot < 0 || slot >= module->nslots)
        return NULL;
    const XiConstLiteral *lit = &module->slot_shared_initializers[slot];
    return lit && lit->kind != XI_CONST_LITERAL_NONE ? lit : NULL;
}

static const XiConstLiteral *cg_module_static_data_literal(const XiModule *module, int64_t slot) {
    const XiConstLiteral *lit = cg_module_const_literal(module, slot);
    if (lit && lit->kind != XI_CONST_LITERAL_NONE)
        return lit;
    return cg_module_shared_slot_literal(module, slot);
}

static const char *cg_module_const_slot_name(const XiModule *module, int64_t slot) {
    if (!module || !module->init || !module->init->slot_owned_names || slot < 0 ||
        slot >= module->init->nshared)
        return NULL;
    return module->init->slot_owned_names[slot];
}

static bool cg_shared_initializer_literal_supported(const XiConstLiteral *lit) {
    if (!lit)
        return false;
    switch (lit->kind) {
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_FLOAT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
        case XI_CONST_LITERAL_STRING:
        case XI_CONST_LITERAL_NULL:
        case XI_CONST_LITERAL_COMPTIME_AGGREGATE:
            return true;
        default:
            return false;
    }
}

static bool cg_shared_initializer_has_xrvalue_initializer(const XiConstLiteral *lit) {
    if (!lit)
        return false;
    switch (lit->kind) {
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_FLOAT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
        case XI_CONST_LITERAL_STRING:
        case XI_CONST_LITERAL_NULL:
            return true;
        default:
            return false;
    }
}

static const XiConstLiteral *cg_module_shared_initializer_literal(const XiModule *module,
                                                                  int64_t slot) {
    if (!module || !module->slot_shared_initializers || slot < 0 || slot >= module->nslots)
        return NULL;
    const XiConstLiteral *lit = &module->slot_shared_initializers[slot];
    return cg_shared_initializer_literal_supported(lit) ? lit : NULL;
}

static const XiConstLiteral *cg_freestanding_shared_initializer_literal(XiCgenCtx *ctx,
                                                                        int64_t slot) {
    if (!ctx || !ctx->freestanding_profile)
        return NULL;
    return cg_module_shared_initializer_literal(ctx->module, slot);
}

static void cg_emit_static_const_fallback_name(FILE *out, const XiModule *module,
                                               const char *prefix, int64_t slot) {
    const char *base = prefix ? prefix : "_xctvalue";
    if (module && module->name && module->name[0]) {
        char module_buf[128];
        sanitize_c_ident_part(module_buf, sizeof(module_buf), module->name);
        fprintf(out, "%s_%s_%" PRId64, base, module_buf, slot);
        return;
    }
    fprintf(out, "%s_%" PRId64, base, slot);
}

static void cg_emit_static_data_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                     int64_t slot, const XiConstLiteral *lit,
                                     const char *fallback_prefix, const char *external_prefix) {
    const XiModule *mod = module ? module : (ctx ? ctx->module : NULL);
    if (!lit || !lit->data_weak) {
        cg_emit_static_const_fallback_name(out, mod, fallback_prefix, slot);
        return;
    }

    char module_buf[128];
    char name_buf[128];
    const char *module_name = mod && mod->name && mod->name[0] ? mod->name : "mod";
    const char *decl_name = cg_module_const_slot_name(mod, slot);
    sanitize_c_ident_part(module_buf, sizeof(module_buf), module_name);
    if (decl_name && decl_name[0]) {
        sanitize_c_ident_part(name_buf, sizeof(name_buf), decl_name);
    } else {
        snprintf(name_buf, sizeof(name_buf), "slot_%" PRId64, slot);
    }
    fprintf(out, "%s_%s_%s", external_prefix && external_prefix[0] ? external_prefix : "xray_const",
            module_buf, name_buf);
}

static void cg_emit_static_const_data_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                           int64_t slot, const char *fallback_prefix) {
    const XiModule *mod = module ? module : (ctx ? ctx->module : NULL);
    cg_emit_static_data_name(ctx, out, mod, slot, cg_module_const_literal(mod, slot),
                             fallback_prefix, "xray_const");
}

static void cg_emit_static_var_data_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                         int64_t slot, const char *fallback_prefix) {
    const XiModule *mod = module ? module : (ctx ? ctx->module : NULL);
    cg_emit_static_data_name(ctx, out, mod, slot, cg_module_shared_slot_literal(mod, slot),
                             fallback_prefix, "xray_var");
}

static bool cg_imported_static_const_needs_weak_symbol(const XiCgenCtx *ctx, const XiModule *module,
                                                       const XiConstLiteral *lit) {
    return ctx && ctx->module && module && module != ctx->module && lit && !lit->data_weak;
}

static void cg_report_imported_static_const_requires_weak(const XiCgenCtx *ctx,
                                                          const XiModule *module, int64_t slot) {
    const char *module_name = module && module->name ? module->name : "?";
    const char *slot_name = cg_module_const_slot_name(module, slot);
    fprintf(stderr,
            "[xi_cgen] ERROR: freestanding imported static const '%s.%s' requires a weak "
            "link-symbol plan for cross-module data access\n",
            module_name, slot_name && slot_name[0] ? slot_name : "?");
    (void) ctx;
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

static void emit_likely_condition_expr(XiCgenCtx *ctx, FILE *out, const XiBlock *blk) {
    /* A return edge is not inherently cold: dispatch-heavy code commonly
     * returns directly from every case. Guessing a branch probability from
     * CFG shape mislabels those hot paths and can inhibit loop-invariant code
     * motion after inlining. Preserve neutral C semantics unless the source
     * explicitly used likely(...) or unlikely(...). */
    emit_condition_expr_ctx(ctx, out, blk ? blk->control : NULL);
}

static bool cg_value_is_elided_static_fixed_struct_array_index_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                                   const XiValue *v);

#include "xi_cgen_struct_helpers.inc.c"
static bool cg_class_native_field_is_ref(const XrAggregateFieldLayout *field);
static const char *cg_class_native_ref_field_tag_name(uint8_t native_type);
static bool cg_class_native_field_plan_has_release_drop(XiCgenCtx *ctx, const XiClassData *cd,
                                                        uint32_t slot,
                                                        const XrAggregateFieldLayout *field);
#include "xi_cgen_class_helpers.inc.c"
static bool cg_has_exception_handling(const XiFunc *f);

static const XaotHashEqPlan *cg_key_access_hash_eq_plan(XiCgenCtx *ctx,
                                                        const XaotKeyAccessPlan *key_plan) {
    if (!key_plan || key_plan->action != XAOT_KEY_ACCESS_SPECIALIZED_HASH_LOOKUP)
        return NULL;
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    return bundle ? xaot_bundle_find_hash_eq_plan(bundle, key_plan->key_type_key) : NULL;
}

static bool cg_key_access_plan_uses_builtin_hash_eq_backend(XiCgenCtx *ctx,
                                                            const XaotKeyAccessPlan *key_plan) {
    const XaotHashEqPlan *plan = cg_key_access_hash_eq_plan(ctx, key_plan);
    return plan && plan->action == XAOT_HASH_EQ_BUILTIN_INLINE;
}

static const XaotBulkPlan *cg_required_bulk_plan(XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *call, uint8_t op_kind,
                                                 const char *label) {
    (void) f;
    if (!call || call->xg_bulk_op_id == XG_NO_ID)
        return NULL;
    const XaotBulkPlan *plan =
        xaot_bundle_find_bulk_plan(cg_ctx_aot_bundle(ctx), (XgBulkOpId) call->xg_bulk_op_id);
    if (!plan || plan->op_kind != op_kind) {
        cg_ctx_set_error(ctx);
        fprintf(stderr, "[xi_cgen] ERROR: missing or mismatched AOT bulk plan for %s (id=%u)\n",
                label ? label : "bulk op", call->xg_bulk_op_id);
        return NULL;
    }
    return plan;
}

/* The native-class effect walk is defined before the general dispatch helpers,
 * but it must consume the same verified low-level lowering contracts when
 * deciding whether a direct method can publish through xrt_pending_error. */
static bool xicgen_value_is_proven_nothrow(XiCgenCtx *ctx, const XiFunc *current,
                                           const XiValue *value, uint8_t depth);
static bool cg_const_int_value(const XiValue *value, int64_t *out);

#include "xi_cgen_class_native_helpers.inc.c"

#include "xi_cgen_array_helpers.inc.c"

static void cg_emit_static_scalar_const_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                             int64_t slot) {
    cg_emit_static_const_data_name(ctx, out, module, slot, "_xctscalar");
}

static void cg_emit_static_scalar_var_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                           int64_t slot) {
    cg_emit_static_var_data_name(ctx, out, module, slot, "_xctscalar");
}

static void cg_emit_static_string_const_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                             int64_t slot) {
    cg_emit_static_const_data_name(ctx, out, module, slot, "_xctstr");
}

static void cg_emit_static_value_const_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                            int64_t slot) {
    cg_emit_static_const_data_name(ctx, out, module, slot, "_xctvalue");
}

static bool cg_const_literal_is_static_scalar_object(const XiConstLiteral *lit) {
    return cg_const_literal_has_data_attrs(lit) && cg_const_literal_is_static_scalar_kind(lit);
}

static bool cg_func_tree_takes_static_addr_slot(const XiFunc *func, int64_t slot) {
    if (!func || slot < 0)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && v->op == XI_STATIC_ADDR && v->aux_int == slot)
                return true;
        }
    }
    for (uint16_t ci = 0; ci < func->nchildren; ci++) {
        if (cg_func_tree_takes_static_addr_slot(func->children[ci], slot))
            return true;
    }
    return false;
}

static bool cg_module_slot_is_import(const XiModule *module, int64_t slot) {
    return module && module->slot_imports && slot >= 0 && slot < module->nslots &&
           module->slot_imports[slot];
}

static bool cg_freestanding_static_scalar_slot_is_materialized(XiCgenCtx *ctx,
                                                               const XiModule *module, int64_t slot,
                                                               const XiConstLiteral *lit) {
    if (!ctx || !ctx->freestanding_profile || !module ||
        !cg_const_literal_is_static_scalar_kind(lit))
        return false;
    if (cg_const_literal_has_data_attrs(lit))
        return true;
    if (module != ctx->module || cg_module_slot_is_import(module, slot))
        return false;
    return cg_func_tree_takes_static_addr_slot(module->init, slot);
}

static bool cg_freestanding_static_scalar_const_literal_in_module(XiCgenCtx *ctx,
                                                                  const XiModule *module,
                                                                  int64_t slot,
                                                                  const XiConstLiteral **out_lit) {
    if (out_lit)
        *out_lit = NULL;
    if (!ctx || !ctx->freestanding_profile || !module || !module->slot_const_literals || slot < 0 ||
        slot >= module->nslots)
        return false;
    const XiConstLiteral *lit = &module->slot_const_literals[slot];
    if (!cg_freestanding_static_scalar_slot_is_materialized(ctx, module, slot, lit))
        return false;
    if (out_lit)
        *out_lit = lit;
    return true;
}

static bool cg_freestanding_static_scalar_const_literal(XiCgenCtx *ctx, int64_t slot,
                                                        const XiConstLiteral **out_lit) {
    return cg_freestanding_static_scalar_const_literal_in_module(ctx, ctx ? ctx->module : NULL,
                                                                 slot, out_lit);
}

static bool cg_freestanding_static_scalar_var_slot_is_materialized(XiCgenCtx *ctx,
                                                                   const XiModule *module,
                                                                   int64_t slot,
                                                                   const XiConstLiteral *lit) {
    if (!ctx || !ctx->freestanding_profile || !module || !lit || !lit->data_mutable ||
        !cg_const_literal_is_static_raw_scalar_kind(lit))
        return false;
    if (module != ctx->module || cg_module_slot_is_import(module, slot))
        return false;
    if (cg_const_literal_has_data_attrs(lit))
        return true;
    return cg_func_tree_takes_static_addr_slot(module->init, slot);
}

static bool cg_freestanding_static_scalar_var_literal_in_module(XiCgenCtx *ctx,
                                                                const XiModule *module,
                                                                int64_t slot,
                                                                const XiConstLiteral **out_lit) {
    if (out_lit)
        *out_lit = NULL;
    if (!ctx || !ctx->freestanding_profile || !module || !module->slot_shared_initializers ||
        slot < 0 || slot >= module->nslots)
        return false;
    const XiConstLiteral *lit = &module->slot_shared_initializers[slot];
    if (!cg_freestanding_static_scalar_var_slot_is_materialized(ctx, module, slot, lit))
        return false;
    if (out_lit)
        *out_lit = lit;
    return true;
}

static bool cg_freestanding_static_scalar_var_literal(XiCgenCtx *ctx, int64_t slot,
                                                      const XiConstLiteral **out_lit) {
    return cg_freestanding_static_scalar_var_literal_in_module(ctx, ctx ? ctx->module : NULL, slot,
                                                               out_lit);
}

static void cg_emit_static_scalar_i64(FILE *out, int64_t value) {
    if (value == INT64_MIN)
        fprintf(out, "INT64_MIN");
    else
        fprintf(out, "INT64_C(%" PRId64 ")", value);
}

static void cg_emit_shared_string_initializer_name(FILE *out, const char *shared_name,
                                                   uint16_t slot) {
    fprintf(out, "%s_init_str_%u", shared_name && shared_name[0] ? shared_name : "xrt_shared",
            (unsigned) slot);
}

static void cg_emit_static_string_header_initializer(FILE *out, const char *s);

static void cg_emit_xrvalue_literal_initializer(FILE *out, const XiConstLiteral *lit,
                                                const char *shared_name, uint16_t slot) {
    switch (lit ? lit->kind : XI_CONST_LITERAL_NONE) {
        case XI_CONST_LITERAL_INT:
            fprintf(out, "XR_FROM_INT(");
            cg_emit_static_scalar_i64(out, lit->int_value);
            fprintf(out, ")");
            break;
        case XI_CONST_LITERAL_FLOAT:
            fprintf(out, "XR_FROM_FLOAT(");
            emit_c_float_literal(out, lit->float_value);
            fprintf(out, ")");
            break;
        case XI_CONST_LITERAL_BOOL:
            fprintf(out, "%s", lit->bool_value ? "XR_TRUE_VAL" : "XR_FALSE_VAL");
            break;
        case XI_CONST_LITERAL_CHAR:
            fprintf(out, "XR_FROM_RUNE(UINT32_C(%" PRIu32 "))", (uint32_t) lit->int_value);
            break;
        case XI_CONST_LITERAL_STRING:
            fprintf(out, "{.tag = XR_TAG_STR, .ptr = (void *) &");
            cg_emit_shared_string_initializer_name(out, shared_name, slot);
            fprintf(out, "}");
            break;
        case XI_CONST_LITERAL_NULL:
        default:
            fprintf(out, "XR_NULL_VAL");
            break;
    }
}

static bool cg_const_int_value_matches_bits(const XiValue *value, uint64_t bits) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || !v->type || v->type->kind != XR_KIND_INT)
        return false;
    if (v->op == XI_CONST)
        return (uint64_t) v->aux_int == bits;
    if (v->op == XI_NEG && v->nargs >= 1) {
        const XiValue *arg = cg_unwrap_identity_value(v->args[0]);
        return arg && arg->op == XI_CONST && arg->type && arg->type->kind == XR_KIND_INT &&
               (UINT64_C(0) - (uint64_t) arg->aux_int) == bits;
    }
    return false;
}

static bool cg_const_float_value_matches_literal(const XiValue *value, double expected) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || !v->type || v->type->kind != XR_KIND_FLOAT)
        return false;
    double actual = 0.0;
    if (v->op == XI_CONST) {
        memcpy(&actual, &v->aux_int, sizeof(double));
    } else if (v->op == XI_NEG && v->nargs >= 1) {
        const XiValue *arg = cg_unwrap_identity_value(v->args[0]);
        if (!arg || arg->op != XI_CONST || !arg->type || arg->type->kind != XR_KIND_FLOAT)
            return false;
        memcpy(&actual, &arg->aux_int, sizeof(double));
        actual = -actual;
    } else {
        return false;
    }
    return memcmp(&actual, &expected, sizeof(double)) == 0;
}

static bool cg_const_value_matches_literal(const XiValue *value, const XiConstLiteral *lit) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || !v->type || !cg_shared_initializer_literal_supported(lit))
        return false;
    switch (lit->kind) {
        case XI_CONST_LITERAL_INT:
            return cg_const_int_value_matches_bits(v, (uint64_t) lit->int_value);
        case XI_CONST_LITERAL_FLOAT:
            return cg_const_float_value_matches_literal(v, lit->float_value);
        case XI_CONST_LITERAL_BOOL:
            return v->op == XI_CONST && v->type->kind == XR_KIND_BOOL &&
                   (v->aux_int != 0) == lit->bool_value;
        case XI_CONST_LITERAL_CHAR:
            return v->op == XI_CONST && v->type->kind == XR_KIND_RUNE &&
                   v->aux_int == lit->int_value;
        case XI_CONST_LITERAL_STRING: {
            const char *value_s = v->aux ? (const char *) v->aux : "";
            const char *lit_s = lit->string_value ? lit->string_value : "";
            return v->op == XI_CONST && v->type->kind == XR_KIND_STRING &&
                   strcmp(value_s, lit_s) == 0;
        }
        case XI_CONST_LITERAL_NULL:
            return v->op == XI_CONST && v->type->kind == XR_KIND_NULL;
        default:
            return false;
    }
}

static bool cg_emit_shared_string_initializer_defs(FILE *out, const char *name,
                                                   const XiModule *module, uint16_t nshared) {
    if (!out || !name || !module || !module->slot_shared_initializers)
        return false;
    bool emitted = false;
    for (uint16_t slot = 0; slot < nshared && slot < module->nslots; slot++) {
        const XiConstLiteral *lit = cg_module_shared_initializer_literal(module, slot);
        if (!lit || lit->kind != XI_CONST_LITERAL_STRING)
            continue;
        fprintf(out, "static const xrt_str_t ");
        cg_emit_shared_string_initializer_name(out, name, slot);
        fprintf(out, " = ");
        cg_emit_static_string_header_initializer(out, lit->string_value);
        fprintf(out, ";\n");
        emitted = true;
    }
    if (emitted)
        fprintf(out, "\n");
    return emitted;
}

static bool cg_emit_shared_array_definition(XiCgenCtx *ctx, FILE *out, const char *linkage,
                                            const char *name, const XiModule *module,
                                            uint16_t nshared) {
    if (!out || !name || nshared == 0)
        return false;
    bool has_static_init = false;
    if (ctx && ctx->freestanding_profile && module && module->slot_shared_initializers) {
        for (uint16_t slot = 0; slot < nshared && slot < module->nslots; slot++) {
            const XiConstLiteral *lit = cg_module_shared_initializer_literal(module, slot);
            if (cg_shared_initializer_has_xrvalue_initializer(lit)) {
                has_static_init = true;
                break;
            }
        }
    }
    if (has_static_init)
        cg_emit_shared_string_initializer_defs(out, name, module, nshared);
    fprintf(out, "%sXrValue %s[%u]", linkage ? linkage : "", name, nshared);
    if (!has_static_init) {
        fprintf(out, ";\n\n");
        return true;
    }
    fprintf(out, " = {\n");
    for (uint16_t slot = 0; slot < nshared && slot < module->nslots; slot++) {
        const XiConstLiteral *lit = cg_module_shared_initializer_literal(module, slot);
        if (!cg_shared_initializer_has_xrvalue_initializer(lit))
            continue;
        fprintf(out, "    [%u] = ", slot);
        cg_emit_xrvalue_literal_initializer(out, lit, name, slot);
        fprintf(out, ",\n");
    }
    fprintf(out, "};\n\n");
    return true;
}

static void cg_emit_static_string_header_initializer(FILE *out, const char *s) {
    if (!s)
        s = "";
    size_t len = strlen(s);
    fprintf(out, "{INT64_C(%zu), INT64_C(%zu), 0x%08xu, XRT_STR_LITERAL, (char *) ", len,
            xr_utf8_strlen(s, len), xr_hash_core_str_hash_bytes(s, len));
    emit_c_string_literal_bytes(out, s, len);
    fprintf(out, "}");
}

static bool cg_emit_freestanding_static_scalar_const_defs(XiCgenCtx *ctx, FILE *out,
                                                          const XiModule *module) {
    if (!ctx || !out || !module || !ctx->freestanding_profile)
        return false;
    bool emitted = false;
    if (module->slot_const_literals) {
        for (uint16_t slot = 0; slot < module->nslots; slot++) {
            const XiConstLiteral *lit = &module->slot_const_literals[slot];
            if (!cg_freestanding_static_scalar_slot_is_materialized(ctx, module, slot, lit))
                continue;
            switch (lit->kind) {
                case XI_CONST_LITERAL_INT:
                case XI_CONST_LITERAL_BOOL:
                case XI_CONST_LITERAL_CHAR:
                    cg_emit_static_const_storage(out, lit);
                    fprintf(out, "int64_t ");
                    cg_emit_static_scalar_const_name(ctx, out, module, slot);
                    emit_aot_const_data_attrs(out, lit);
                    fprintf(out, " = ");
                    cg_emit_static_scalar_i64(out, lit->kind == XI_CONST_LITERAL_BOOL
                                                       ? (lit->bool_value ? 1 : 0)
                                                       : lit->int_value);
                    fprintf(out, ";\n");
                    emitted = true;
                    break;
                case XI_CONST_LITERAL_FLOAT:
                    cg_emit_static_const_storage(out, lit);
                    fprintf(out, "double ");
                    cg_emit_static_scalar_const_name(ctx, out, module, slot);
                    emit_aot_const_data_attrs(out, lit);
                    fprintf(out, " = ");
                    emit_c_float_literal(out, lit->float_value);
                    fprintf(out, ";\n");
                    emitted = true;
                    break;
                case XI_CONST_LITERAL_STRING:
                    cg_emit_static_const_storage(out, lit);
                    fprintf(out, "xrt_str_t ");
                    cg_emit_static_string_const_name(ctx, out, module, slot);
                    emit_aot_const_data_attrs(out, lit);
                    fprintf(out, " = ");
                    cg_emit_static_string_header_initializer(out, lit->string_value);
                    fprintf(out, ";\n");
                    emitted = true;
                    break;
                case XI_CONST_LITERAL_NULL:
                    cg_emit_static_const_storage(out, lit);
                    fprintf(out, "XrValue ");
                    cg_emit_static_value_const_name(ctx, out, module, slot);
                    emit_aot_const_data_attrs(out, lit);
                    fprintf(out, " = XR_NULL_VAL;\n");
                    emitted = true;
                    break;
                default:
                    break;
            }
        }
    }
    if (module->slot_shared_initializers) {
        for (uint16_t slot = 0; slot < module->nslots; slot++) {
            const XiConstLiteral *lit = &module->slot_shared_initializers[slot];
            if (!cg_freestanding_static_scalar_var_slot_is_materialized(ctx, module, slot, lit))
                continue;
            switch (lit->kind) {
                case XI_CONST_LITERAL_INT:
                case XI_CONST_LITERAL_BOOL:
                case XI_CONST_LITERAL_CHAR:
                    cg_emit_static_const_storage(out, lit);
                    fprintf(out, "int64_t ");
                    cg_emit_static_scalar_var_name(ctx, out, module, slot);
                    emit_aot_const_data_attrs(out, lit);
                    fprintf(out, " = ");
                    cg_emit_static_scalar_i64(out, lit->kind == XI_CONST_LITERAL_BOOL
                                                       ? (lit->bool_value ? 1 : 0)
                                                       : lit->int_value);
                    fprintf(out, ";\n");
                    emitted = true;
                    break;
                case XI_CONST_LITERAL_FLOAT:
                    cg_emit_static_const_storage(out, lit);
                    fprintf(out, "double ");
                    cg_emit_static_scalar_var_name(ctx, out, module, slot);
                    emit_aot_const_data_attrs(out, lit);
                    fprintf(out, " = ");
                    emit_c_float_literal(out, lit->float_value);
                    fprintf(out, ";\n");
                    emitted = true;
                    break;
                default:
                    break;
            }
        }
    }
    if (emitted)
        fprintf(out, "\n");
    return emitted;
}

static XrRep cg_static_scalar_const_source_rep(const XiConstLiteral *lit) {
    if (!lit)
        return XR_REP_TAGGED;
    switch (lit->kind) {
        case XI_CONST_LITERAL_FLOAT:
            return XR_REP_F64;
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
            return XR_REP_I64;
        case XI_CONST_LITERAL_STRING:
        case XI_CONST_LITERAL_NULL:
        default:
            return XR_REP_TAGGED;
    }
}

static bool cg_emit_freestanding_static_scalar_const_ref_in_module(XiCgenCtx *ctx, FILE *out,
                                                                   const XiModule *module,
                                                                   int64_t slot, const XiValue *v,
                                                                   const XiConstLiteral *lit) {
    if (!ctx || !out || !v ||
        !cg_freestanding_static_scalar_slot_is_materialized(ctx, module, slot, lit))
        return false;
    XrRep from_rep = cg_static_scalar_const_source_rep(lit);
    XrRep to_rep = cg_value_plan_storage_rep(ctx, v);
    const XrType *type = lit->type ? lit->type : v->type;
    const char *suffix = emit_conversion_prefix(out, type, from_rep, to_rep);
    switch (lit->kind) {
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
        case XI_CONST_LITERAL_FLOAT:
            cg_emit_static_scalar_const_name(ctx, out, module, slot);
            break;
        case XI_CONST_LITERAL_STRING:
            fprintf(out, "xr_str_lit(&");
            cg_emit_static_string_const_name(ctx, out, module, slot);
            fprintf(out, ")");
            break;
        case XI_CONST_LITERAL_NULL:
            cg_emit_static_value_const_name(ctx, out, module, slot);
            break;
        default:
            emit_conversion_suffix(out, suffix);
            return false;
    }
    emit_conversion_suffix(out, suffix);
    return true;
}

static bool cg_emit_freestanding_static_scalar_const_ref(XiCgenCtx *ctx, FILE *out,
                                                         const XiValue *v,
                                                         const XiConstLiteral *lit) {
    return cg_emit_freestanding_static_scalar_const_ref_in_module(
        ctx, out, ctx ? ctx->module : NULL, v ? v->aux_int : -1, v, lit);
}

static bool cg_emit_freestanding_static_scalar_var_ref_in_module(XiCgenCtx *ctx, FILE *out,
                                                                 const XiModule *module,
                                                                 int64_t slot, const XiValue *v,
                                                                 const XiConstLiteral *lit) {
    if (!ctx || !out || !v ||
        !cg_freestanding_static_scalar_var_slot_is_materialized(ctx, module, slot, lit))
        return false;
    XrRep from_rep = cg_static_scalar_const_source_rep(lit);
    XrRep to_rep = cg_value_plan_storage_rep(ctx, v);
    const XrType *type = lit->type ? lit->type : v->type;
    const char *suffix = emit_conversion_prefix(out, type, from_rep, to_rep);
    cg_emit_static_scalar_var_name(ctx, out, module, slot);
    emit_conversion_suffix(out, suffix);
    return true;
}

static bool cg_emit_freestanding_static_scalar_var_ref(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                       const XiConstLiteral *lit) {
    return cg_emit_freestanding_static_scalar_var_ref_in_module(ctx, out, ctx ? ctx->module : NULL,
                                                                v ? v->aux_int : -1, v, lit);
}

static bool cg_emit_freestanding_static_scalar_var_store(XiCgenCtx *ctx, FILE *out,
                                                         const XiModule *module, int64_t slot,
                                                         const XiFunc *f, const XiValue *value,
                                                         const XiConstLiteral *lit) {
    if (!ctx || !out || !value ||
        !cg_freestanding_static_scalar_var_slot_is_materialized(ctx, module, slot, lit))
        return false;
    (void) f;
    switch (lit->kind) {
        case XI_CONST_LITERAL_INT:
            fprintf(out, "(");
            cg_emit_static_scalar_var_name(ctx, out, module, slot);
            fprintf(out, " = ");
            emit_value_as_rep_ctx(ctx, out, value, XR_REP_I64);
            fprintf(out, ", XR_FROM_INT(");
            cg_emit_static_scalar_var_name(ctx, out, module, slot);
            fprintf(out, "))");
            return true;
        case XI_CONST_LITERAL_BOOL:
            fprintf(out, "(");
            cg_emit_static_scalar_var_name(ctx, out, module, slot);
            fprintf(out, " = ");
            emit_value_as_rep_ctx(ctx, out, value, XR_REP_I64);
            fprintf(out, ", ");
            cg_emit_static_scalar_var_name(ctx, out, module, slot);
            fprintf(out, " ? XR_TRUE_VAL : XR_FALSE_VAL)");
            return true;
        case XI_CONST_LITERAL_CHAR:
            fprintf(out, "(");
            cg_emit_static_scalar_var_name(ctx, out, module, slot);
            fprintf(out, " = ");
            emit_value_as_rep_ctx(ctx, out, value, XR_REP_I64);
            fprintf(out, ", XR_FROM_RUNE((uint32_t)");
            cg_emit_static_scalar_var_name(ctx, out, module, slot);
            fprintf(out, "))");
            return true;
        case XI_CONST_LITERAL_FLOAT:
            fprintf(out, "(");
            cg_emit_static_scalar_var_name(ctx, out, module, slot);
            fprintf(out, " = ");
            emit_value_as_rep_ctx(ctx, out, value, XR_REP_F64);
            fprintf(out, ", XR_FROM_FLOAT(");
            cg_emit_static_scalar_var_name(ctx, out, module, slot);
            fprintf(out, "))");
            return true;
        default:
            return false;
    }
}

static bool cg_emit_imported_static_scalar_const_decl(XiCgenCtx *ctx, FILE *out,
                                                      const XiModule *module, int64_t slot,
                                                      const XiConstLiteral *lit) {
    if (!ctx || !out || !module || !lit || !lit->data_weak ||
        !cg_const_literal_is_static_scalar_object(lit))
        return false;
    switch (lit->kind) {
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
            fprintf(out, "extern const int64_t ");
            cg_emit_static_scalar_const_name(ctx, out, module, slot);
            fprintf(out, ";\n");
            return true;
        case XI_CONST_LITERAL_FLOAT:
            fprintf(out, "extern const double ");
            cg_emit_static_scalar_const_name(ctx, out, module, slot);
            fprintf(out, ";\n");
            return true;
        case XI_CONST_LITERAL_STRING:
            fprintf(out, "extern const xrt_str_t ");
            cg_emit_static_string_const_name(ctx, out, module, slot);
            fprintf(out, ";\n");
            return true;
        case XI_CONST_LITERAL_NULL:
            fprintf(out, "extern const XrValue ");
            cg_emit_static_value_const_name(ctx, out, module, slot);
            fprintf(out, ";\n");
            return true;
        default:
            return false;
    }
}

static bool cg_emit_imported_static_fixed_array_const_decl(XiCgenCtx *ctx, FILE *out,
                                                           const XiModule *module, int64_t slot,
                                                           const XiConstLiteral *lit) {
    if (!ctx || !out || !module || !lit || !lit->data_weak)
        return false;
    CgFixedArrayLaneInfo info;
    if (!cg_static_fixed_array_literal_in_module(ctx, module, slot, &info, NULL))
        return false;
    fprintf(out, "extern const %s ", info.ctype);
    cg_emit_static_fixed_array_name(ctx, out, module, slot);
    fprintf(out, "[%u];\n", (unsigned) info.count);
    return true;
}

static bool cg_emit_imported_static_fixed_matrix_const_decl(XiCgenCtx *ctx, FILE *out,
                                                            const XiModule *module, int64_t slot,
                                                            const XiConstLiteral *lit) {
    if (!ctx || !out || !module || !lit || !lit->data_weak)
        return false;
    CgStaticFixedMatrixInfo info;
    if (!cg_freestanding_static_fixed_matrix_literal_in_module(ctx, module, slot, &info, NULL))
        return false;
    fprintf(out, "extern const %s ", info.lane.ctype);
    cg_emit_static_fixed_array_name(ctx, out, module, slot);
    fprintf(out, "[%u][%u];\n", (unsigned) info.outer_count, (unsigned) info.inner_count);
    return true;
}

static bool cg_emit_imported_static_fixed_cube_const_decl(XiCgenCtx *ctx, FILE *out,
                                                          const XiModule *module, int64_t slot,
                                                          const XiConstLiteral *lit) {
    if (!ctx || !out || !module || !lit || !lit->data_weak)
        return false;
    CgStaticFixedCubeInfo info;
    if (!cg_freestanding_static_fixed_cube_literal_in_module(ctx, module, slot, &info, NULL))
        return false;
    fprintf(out, "extern const %s ", info.lane.ctype);
    cg_emit_static_fixed_array_name(ctx, out, module, slot);
    fprintf(out, "[%u][%u][%u];\n", (unsigned) info.outer_count, (unsigned) info.middle_count,
            (unsigned) info.inner_count);
    return true;
}

static bool cg_emit_imported_static_tuple_const_decl(XiCgenCtx *ctx, FILE *out,
                                                     const XiModule *module, int64_t slot,
                                                     const XiConstLiteral *lit) {
    if (!ctx || !out || !module || !lit || !lit->data_weak)
        return false;
    XrType *tuple_type = NULL;
    if (!cg_freestanding_static_tuple_literal_in_module(ctx, module, slot, &tuple_type, NULL))
        return false;
    fprintf(out, "extern const struct { ");
    int count = cg_static_tuple_type_count(tuple_type);
    for (uint16_t i = 0; i < (uint16_t) count; i++)
        cg_emit_static_tuple_field_decl(out, tuple_type, i);
    fprintf(out, "} ");
    cg_emit_static_tuple_name(ctx, out, module, slot);
    fprintf(out, ";\n");
    return true;
}

static bool cg_emit_imported_static_struct_const_decl(XiCgenCtx *ctx, FILE *out,
                                                      const XiModule *module, int64_t slot,
                                                      const XiConstLiteral *lit) {
    if (!ctx || !out || !module || !lit || !lit->data_weak)
        return false;
    const XrAggregateLayout *sl = NULL;
    if (!cg_freestanding_static_struct_literal_in_module(ctx, module, slot, &sl, NULL))
        return false;
    fprintf(out, "extern const ");
    emit_static_aggregate_decl_head(out, sl);
    const char *prefix = module->name ? module->name : "mod";
    for (uint16_t i = 0; sl && i < sl->field_count; i++) {
        char fname[128];
        cg_struct_field_c_name(sl, i, fname, sizeof(fname));
        emit_static_struct_field_decl(out, sl, i, fname, prefix);
        fprintf(out, "; ");
    }
    fprintf(out, "} ");
    cg_emit_static_struct_name(ctx, out, module, slot);
    fprintf(out, ";\n");
    return true;
}

static bool cg_imported_static_const_decl_seen(const XiModule *module, int upto_slot,
                                               const XiImportRef *ref) {
    if (!module || !module->slot_imports || !ref)
        return false;
    for (int slot = 0; slot < upto_slot; slot++) {
        const XiImportRef *prev = module->slot_imports[slot];
        if (!prev)
            continue;
        if (prev->resolved_mod_index == ref->resolved_mod_index &&
            prev->resolved_shared_slot == ref->resolved_shared_slot)
            return true;
    }
    return false;
}

static bool cg_emit_freestanding_imported_static_const_decls(XiCgenCtx *ctx, FILE *out,
                                                             const XiModule *module) {
    if (!ctx || !out || !module || !ctx->freestanding_profile || !module->slot_imports)
        return false;
    bool emitted = false;
    for (uint16_t slot = 0; slot < module->nslots; slot++) {
        const XiImportRef *ref = module->slot_imports[slot];
        if (!ref || cg_imported_static_const_decl_seen(module, (int) slot, ref))
            continue;
        const XiModule *target_module = NULL;
        int64_t target_slot = -1;
        const XiConstLiteral *lit =
            cg_import_ref_target_const_literal(ctx, ref, &target_module, &target_slot);
        if (!lit || !target_module || target_module == module || !lit->data_weak)
            continue;
        if (cg_emit_imported_static_scalar_const_decl(ctx, out, target_module, target_slot, lit) ||
            cg_emit_imported_static_fixed_array_const_decl(ctx, out, target_module, target_slot,
                                                           lit) ||
            cg_emit_imported_static_fixed_matrix_const_decl(ctx, out, target_module, target_slot,
                                                            lit) ||
            cg_emit_imported_static_fixed_cube_const_decl(ctx, out, target_module, target_slot,
                                                          lit) ||
            cg_emit_imported_static_fixed_struct_array_const_decl(ctx, out, target_module,
                                                                  target_slot, lit) ||
            cg_emit_imported_static_fixed_tuple_array_const_decl(ctx, out, target_module,
                                                                 target_slot, lit) ||
            cg_emit_imported_static_tuple_const_decl(ctx, out, target_module, target_slot, lit) ||
            cg_emit_imported_static_struct_const_decl(ctx, out, target_module, target_slot, lit))
            emitted = true;
    }
    if (emitted)
        fprintf(out, "\n");
    return emitted;
}

static bool cg_class_native_ref_stack_return_consumes_ctor(XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *ctor_call);

static bool cg_class_descriptor_slot_can_elide_depth(XiCgenCtx *ctx, const XiFunc *current,
                                                     int slot, const XiClassData *cd, int depth);
static bool cg_class_descriptor_create_is_elided_depth(XiCgenCtx *ctx, const XiFunc *current,
                                                       const XiValue *v, int depth);
static const XiFunc *cg_lookup_static_method(XiCgenCtx *ctx, const char *class_name,
                                             const char *method, const char **out_prefix);

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

static const XrAggregateLayout *cg_class_descriptor_layout_data(const XiClassData *cd) {
    if (!cd)
        return NULL;
    return cd->instance_layout ? cd->instance_layout : cd->struct_layout;
}

static bool cg_class_descriptor_native_stack_only_data(const XiClassData *cd) {
    const XrAggregateLayout *layout = cg_class_descriptor_layout_data(cd);
    return cd && layout && !cd->is_monomorphized && !cg_class_native_layout_has_ref_fields(layout);
}

static bool cg_class_descriptor_elidable_native_data(const XiClassData *cd) {
    return cd && cg_class_descriptor_layout_data(cd) && !cd->is_monomorphized;
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
    const XrAggregateLayout *call_layout = cg_class_descriptor_layout_data(call_cd);
    return call_layout && cg_class_native_layout_has_ref_fields(call_layout) &&
           cg_class_native_ref_stack_return_consumes_ctor(ctx, owner, call);
}

static bool cg_class_descriptor_static_method_call_is_elidable(XiCgenCtx *ctx, const XiValue *call,
                                                               const XiClassData *cd) {
    if (!ctx || !call || !cd || call->op != XI_CALL_METHOD || call->nargs < 1 || !call->aux)
        return false;
    const char *class_name = cd->class_name ? cd->class_name : cd->display_name;
    if (!class_name)
        return false;
    const XiFunc *target = cg_lookup_static_method(ctx, class_name, (const char *) call->aux, NULL);
    uint16_t call_argc = (uint16_t) (call->nargs - 1);
    return target && !target->is_vararg && target->nparams == call_argc &&
           !cg_func_needs_aot_coro_ctx(ctx, target);
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
                    case XI_CALL_METHOD:
                        if (ai != 0 ||
                            !cg_class_descriptor_static_method_call_is_elidable(ctx, user, cd))
                            return false;
                        if (saw_elidable_use)
                            *saw_elidable_use = true;
                        break;
                    case XI_AGG_NEW:
                        if (ai != 0 || !user->aux)
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
                    case XI_SOURCE_MOVE:
                    case XI_OWNER_FORWARD:
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
    if (!ctx || !ctx->module || !ctx->module->init || slot < 0 || depth > 8)
        return false;
    if (ctx->all_nmodules > 1 && cg_class_shared_native_slot_is_exported(ctx, slot))
        return false;
    const XiClassData *slot_cd = cg_class_descriptor_slot_data(ctx, slot);
    if (!slot_cd)
        slot_cd = cd;
    if (!cg_class_native_data_matches(slot_cd, cd) ||
        !cg_class_descriptor_elidable_native_data(slot_cd))
        return false;
    bool saw_elidable_use = false;
    (void) saw_elidable_use;
    return cg_class_descriptor_slot_uses_are_elidable(ctx, ctx->module->init, slot, slot_cd,
                                                      depth + 1, &saw_elidable_use);
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
    if (!ctx || !current || !v || v->op != XI_GET_SHARED)
        return false;
    int slot = (int) v->aux_int;
    const XiClassData *cd = cg_class_descriptor_slot_data(ctx, slot);
    if (cg_class_descriptor_elidable_native_data(cd)) {
        bool saw_elidable_use = false;
        if (cg_class_descriptor_value_uses_are_elidable(ctx, current, v, cd, 0,
                                                        &saw_elidable_use) &&
            saw_elidable_use)
            return true;
    }
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
 * everything else uses its planned storage rep. Identity ops (XI_COPY/XI_OWNER_FORWARD)
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
 *   - bool values keep an int64_t slot, matching their XR_REP_I64 suspend-result
 *     metadata; using a narrower plan c_type would let blocking resume helpers
 *     overwrite the following frame fields;
 *   - a unit/void value keeps an XrValue slot, since declaring a `void` local is
 *     illegal C and such slots are never read as values. */
static const char *cg_coro_decl_ctype(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    (void) f;
    if (cg_value_type_is_bool(v) && cg_value_plan_storage_rep(ctx, v) == XR_REP_I64)
        return ctype_str(XR_REP_I64);
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
        if (cg_value_narrow_local_scalar_rep(v, 0, &code) && xaot_rep_from_native_type(code, &rep))
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

static bool cg_lowbits_binop_elided_into_unsigned_narrow(const XiFunc *f, const XiValue *v) {
    if (!f || !v || !cg_op_is_lowbits_binop(v->op) || v->nargs < 2 || cg_rep(v) != XR_REP_I64 ||
        cg_rep(v->args[0]) != XR_REP_I64 || cg_rep(v->args[1]) != XR_REP_I64)
        return false;

    bool any_user = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == v)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == v)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == v)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != v)
                    continue;
                any_user = true;
                if (cg_unsigned_narrow_lowbits_binop_arg(user) != v)
                    return false;
            }
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
    switch (type->scalar_rep) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
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

    const char *ctype = NULL;
    int width = 64;
    switch (v->args[0]->type->scalar_rep) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
            ctype = "uint32_t";
            width = 32;
            break;
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
            ctype = "uint64_t";
            width = 64;
            break;
        default:
            return false;
    }
    if (shift >= width)
        ctype = "uint64_t";

    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "XR_FROM_INT((int64_t)(");
    fprintf(out, "(((%s)(", ctype);
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

static bool emit_native_const_shr_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || v->op != XI_SHR || v->nargs < 2 || cg_rep(v) != XR_REP_I64 ||
        cg_rep(v->args[0]) != XR_REP_I64 || cg_rep(v->args[1]) != XR_REP_I64)
        return false;

    int64_t shift = 0;
    if (!cg_shift_const_int_value(v->args[1], &shift) || shift < 0 || shift >= 64)
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

static bool emit_native_wrapping_const_shl_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || v->op != XI_SHL || v->nargs < 2 || cg_rep(v) != XR_REP_I64 ||
        cg_rep(v->args[0]) != XR_REP_I64 || cg_rep(v->args[1]) != XR_REP_I64)
        return false;

    int64_t shift = 0;
    if (!cg_shift_const_int_value(v->args[1], &shift) || shift < 0 || shift >= 64)
        return false;

    bool boxed = cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED;
    if (boxed)
        fprintf(out, "XR_FROM_INT(");
    fprintf(out, "((int64_t)((uint64_t)(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ") << UINT64_C(%" PRIu64 ")))", (uint64_t) shift);
    if (boxed)
        fprintf(out, ")");
    return true;
}

/* Shifts cannot generally use raw C << / >> because dynamic counts are taken
 * mod 64 in Xray. Const-count arithmetic right shifts use the same C operation
 * as xr_i64_shr_wrap after the mask is folded away. Const left shifts use the
 * same unsigned-cast wrapping expression as the runtime helper. */
static void emit_shift_binop_ctx(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const char *fn) {
    if (emit_native_unsigned_const_shift_expr(ctx, out, v, v->op == XI_SHL ? "<<" : ">>"))
        return;
    if (emit_native_nonnegative_const_shr_expr(ctx, out, f, v))
        return;
    if (emit_native_const_shr_expr(ctx, out, v))
        return;
    if (emit_native_range_safe_const_shl_expr(ctx, out, v))
        return;
    if (emit_native_wrapping_const_shl_expr(ctx, out, v))
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

static bool cg_aot_compare_present_bool_map_get_const(XiCgenCtx *ctx, const XiValue *compare,
                                                      const XiValue **out_get,
                                                      bool *out_const_value);

static bool cg_func_body_is_reachable_from_roots(XiCgenCtx *ctx, const XiFunc *target, int depth);

static CgFuncReachMemo *cg_func_reach_memo_entry(XiCgenCtx *ctx, const XiFunc *func, bool create) {
    if (!ctx || !func)
        return NULL;
    for (int i = 0; i < ctx->nfunc_reach_memo; i++) {
        if (ctx->func_reach_memo[i].func == func)
            return &ctx->func_reach_memo[i];
    }
    if (!create)
        return NULL;
    if (ctx->nfunc_reach_memo >= ctx->func_reach_memo_cap) {
        int new_cap = ctx->func_reach_memo_cap ? ctx->func_reach_memo_cap * 2 : 64;
        CgFuncReachMemo *new_items = (CgFuncReachMemo *) xr_realloc(
            ctx->func_reach_memo, (size_t) new_cap * sizeof(CgFuncReachMemo));
        if (!new_items)
            return NULL;
        memset(new_items + ctx->func_reach_memo_cap, 0,
               (size_t) (new_cap - ctx->func_reach_memo_cap) * sizeof(CgFuncReachMemo));
        ctx->func_reach_memo = new_items;
        ctx->func_reach_memo_cap = new_cap;
    }
    CgFuncReachMemo *entry = &ctx->func_reach_memo[ctx->nfunc_reach_memo++];
    memset(entry, 0, sizeof(*entry));
    entry->func = func;
    return entry;
}

static CgSharedSlotReachMemo *
cg_shared_slot_reach_memo_entry(XiCgenCtx *ctx, const XiModule *module, int slot, bool create) {
    if (!ctx || !module || slot < 0)
        return NULL;
    for (int i = 0; i < ctx->nshared_slot_reach_memo; i++) {
        if (ctx->shared_slot_reach_memo[i].module == module &&
            ctx->shared_slot_reach_memo[i].slot == slot)
            return &ctx->shared_slot_reach_memo[i];
    }
    if (!create)
        return NULL;
    if (ctx->nshared_slot_reach_memo >= ctx->shared_slot_reach_memo_cap) {
        int new_cap = ctx->shared_slot_reach_memo_cap ? ctx->shared_slot_reach_memo_cap * 2 : 64;
        CgSharedSlotReachMemo *new_items = (CgSharedSlotReachMemo *) xr_realloc(
            ctx->shared_slot_reach_memo, (size_t) new_cap * sizeof(CgSharedSlotReachMemo));
        if (!new_items)
            return NULL;
        memset(new_items + ctx->shared_slot_reach_memo_cap, 0,
               (size_t) (new_cap - ctx->shared_slot_reach_memo_cap) *
                   sizeof(CgSharedSlotReachMemo));
        ctx->shared_slot_reach_memo = new_items;
        ctx->shared_slot_reach_memo_cap = new_cap;
    }
    CgSharedSlotReachMemo *entry = &ctx->shared_slot_reach_memo[ctx->nshared_slot_reach_memo++];
    memset(entry, 0, sizeof(*entry));
    entry->module = module;
    entry->slot = slot;
    return entry;
}

static bool cg_func_body_gets_shared_slot(const XiFunc *f, int slot) {
    if (!f || slot < 0)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && v->op == XI_GET_SHARED && (int) v->aux_int == slot)
                return true;
        }
    }
    return false;
}

static bool cg_reachable_func_tree_gets_shared_slot(XiCgenCtx *ctx, const XiModule *owner_mod,
                                                    const XiFunc *source, int slot, int depth) {
    if (!ctx || !owner_mod || !source || slot < 0)
        return false;
    if (depth > 64)
        return true;
    const XiFunc *slot_target = (owner_mod->slot_funcs && slot < (int) owner_mod->nslots)
                                    ? owner_mod->slot_funcs[slot]
                                    : NULL;
    if (slot_target && cg_func_tree_contains(slot_target, source))
        return false;
    bool source_reachable = cg_func_body_is_reachable_from_roots(ctx, source, depth + 1);
    if (!source_reachable)
        return false;
    if (cg_module_for_func(ctx, source) == owner_mod && cg_func_body_gets_shared_slot(source, slot))
        return true;
    for (uint16_t ci = 0; ci < source->nchildren; ci++) {
        if (cg_reachable_func_tree_gets_shared_slot(ctx, owner_mod, source->children[ci], slot,
                                                    depth + 1))
            return true;
    }
    return false;
}

static bool cg_shared_slot_has_reachable_get(XiCgenCtx *ctx, const XiModule *owner_mod, int slot) {
    if (!owner_mod || !owner_mod->init || slot < 0)
        return false;
    CgSharedSlotReachMemo *memo = cg_shared_slot_reach_memo_entry(ctx, owner_mod, slot, true);
    if (!memo)
        return true;
    if (memo->state == 2)
        return memo->has_get;
    if (memo->state == 1)
        return false;
    memo->state = 1;
    bool has_get =
        cg_reachable_func_tree_gets_shared_slot(ctx, owner_mod, owner_mod->init, slot, 0);
    memo = cg_shared_slot_reach_memo_entry(ctx, owner_mod, slot, false);
    if (!memo)
        return has_get;
    memo->has_get = has_get;
    memo->state = 2;
    return memo->has_get;
}

static bool cg_import_ref_has_verified_link_dependency(const XiCgenCtx *ctx,
                                                       const XiImportRef *ref) {
    char name[XG_LINK_DEP_NAME_MAX];
    uint8_t expected_kind;
    int written;
    if (!ctx || !ctx->aot_bundle || !ref || !ref->module_path || !ref->module_path[0])
        return false;
    if (ref->member_name && ref->member_name[0]) {
        expected_kind = XG_LINK_DEP_STDLIB_SYMBOL;
        written = snprintf(name, sizeof(name), "%s.%s", ref->module_path, ref->member_name);
    } else {
        expected_kind = XG_LINK_DEP_STDLIB_MODULE;
        written = snprintf(name, sizeof(name), "%s", ref->module_path);
    }
    if (written <= 0 || (size_t) written >= sizeof(name))
        return false;
    for (uint32_t i = 0; i < ctx->aot_bundle->nlink_dependency_plans; i++) {
        const XaotLinkDependencyPlan *plan = &ctx->aot_bundle->link_dependency_plans[i];
        if (plan->kind == expected_kind && plan->evidence == XAOT_LINK_DEP_EV_GLOBAL_SUMMARY &&
            plan->unproven_reason == XAOT_LINK_DEP_UNPROVEN_NONE && strcmp(plan->name, name) == 0)
            return true;
    }
    return false;
}

static bool cg_import_ref_value_use_requires_runtime_value(XiCgenCtx *ctx, const XiFunc *owner,
                                                           const XiValue *ref, int depth) {
    if (!ctx || !owner || !ref)
        return false;
    if (depth > 16)
        return true;
    const XiModule *owner_mod = cg_module_for_func(ctx, owner);
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != ref)
                    continue;
                switch ((XiOp) user->op) {
                    case XI_SET_SHARED:
                        if (ai != 0)
                            return true;
                        if (!owner_mod || !owner_mod->init || user->aux_int < 0 ||
                            user->aux_int >= owner_mod->init->nshared ||
                            cg_shared_slot_has_reachable_get(ctx, owner_mod, (int) user->aux_int))
                            return true;
                        break;
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (ai != 0)
                            return true;
                        break;
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_COPY:
                    case XI_SOURCE_MOVE:
                    case XI_OWNER_FORWARD:
                        if (ai != 0)
                            return true;
                        if (cg_import_ref_value_use_requires_runtime_value(ctx, owner, user,
                                                                           depth + 1))
                            return true;
                        break;
                    default:
                        return true;
                }
            }
        }
    }
    return false;
}

static bool cg_import_ref_value_is_dead_for_aot(XiCgenCtx *ctx, const XiFunc *owner,
                                                const XiValue *v) {
    if (!ctx || !owner || !v || v->op != XI_IMPORT_REF || !v->aux)
        return false;
    return !cg_import_ref_value_use_requires_runtime_value(ctx, owner, v, 0);
}

static bool cg_aot_frame_new_can_supply_cl_arg(const XiFunc *current, const XiValue *callee,
                                               const XiFunc *target);
static bool cg_func_needs_sync_go_wrapper_ctx(XiCgenCtx *ctx, const XiFunc *f);
static void emit_aot_frame_new_call_args(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                         const XiValue *callee, const XiFunc *target,
                                         bool typed_params, XiValue *const *args,
                                         uint16_t arg_start, uint16_t nargs,
                                         const XiValue *transfer_owner);
static const XaotTransferPlan *cg_required_transfer_plan(XiCgenCtx *ctx, const XiValue *site,
                                                         uint16_t transfer_index,
                                                         const XiValue *expected_value,
                                                         const char *context);
static const char *cg_aot_stdlib_module_of_receiver(const XiCgenCtx *ctx, const XiFunc *f,
                                                    const XiValue *recv);

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
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
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
                cg_ref_noescape_debug_fail(f, alias, user, "unresolved direct callee");
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
        (user->op != XI_CALL_METHOD && user->op != XI_CALL_METHOD_DIRECT))
        return false;
    return cg_call_method_matches_receiver_registry_id(user,
                                                       XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESIZE) ||
           cg_call_method_matches_receiver_registry_id(user,
                                                       XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESERVE) ||
           cg_call_method_matches_receiver_registry_id(user,
                                                       XA_BUILTIN_RECEIVER_METHOD_ARRAY_CLEAR) ||
           cg_call_method_matches_receiver_registry_id(
               user, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM) ||
           cg_call_method_matches_receiver_registry_id(
               user, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM);
}

static bool cg_borrowed_array_slot_alias_uses_are_borrowed(XiCgenCtx *ctx, const XiFunc *f,
                                                           const XiValue *alias, uint8_t depth);

static bool cg_value_is_array_slot_forwarding_or_arc(const XiValue *v) {
    if (!v)
        return false;
    switch ((XiOp) v->op) {
        case XI_RETAIN:
        case XI_RELEASE:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
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
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
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
        case XI_BYTE_SLICE_LOAD_U16:
        case XI_BYTE_SLICE_LOAD_U32:
        case XI_BYTE_SLICE_LOAD_U64:
        case XI_BYTE_SLICE_LOAD_F32:
        case XI_BYTE_SLICE_LOAD_F64:
        case XI_BYTE_SLICE_STORE_U16:
        case XI_BYTE_SLICE_STORE_U32:
        case XI_BYTE_SLICE_STORE_U64:
        case XI_BYTE_SLICE_STORE_F32:
        case XI_BYTE_SLICE_STORE_F64:
        case XI_BYTE_SLICE_FILL:
        case XI_BYTE_SLICE_REPEAT:
        case XI_SLICE_AS_BYTES:
        case XI_SLICE_FILL:
        case XI_SLICE_REINTERPRET:
            return arg_index == 0;
        case XI_SLICE_COPY:
        case XI_SLICE_COMPARE:
        case XI_BYTE_SLICE_COPY:
        case XI_BYTE_SLICE_COMPARE:
        case XI_BYTE_SLICE_COMMON_PREFIX:
            return arg_index == 0 || arg_index == 1;
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

    if (v->op == XI_GO) {
        xicgen_go(ctx, out, f, v, prefix);
        return;
    }

    if (v->op == XI_CHAN_NEW) {
        XrRep rep = cg_value_plan_storage_rep(ctx, v);
        if (rep == XR_REP_PTR)
            fprintf(out, "(");
        else if (rep != XR_REP_TAGGED) {
            fprintf(stderr, "[xi_cgen] ERROR: unsupported channel storage representation %d\n",
                    (int) rep);
            emit_codegen_abort_expr(out);
            ctx->error = true;
            return;
        }
        fprintf(out, "xr_aot_channel_new(%s, ", xicgen_aot_context_expr(ctx, f));
        if (v->nargs >= 1)
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
        else
            fprintf(out, "0");
        fprintf(out, ")");
        if (rep == XR_REP_PTR)
            fprintf(out, ").ptr");
        return;
    }

    if (v->op == XI_CHAN_RECV_STATUS) {
        if (v->nargs < 1) {
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_RECV_STATUS missing recv value\n");
            emit_codegen_abort_expr(out);
            ctx->error = true;
            return;
        }
        XrRep rep = cg_value_decl_storage_rep(ctx, f, v);
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_I64, rep);
        if (v->args[0] && v->args[0]->op == XI_CHAN_TRY_RECV)
            fprintf(out, "xr_aot_recv_is_value(_chan_try_%u)", v->args[0]->id);
        else {
            fprintf(out, "xr_aot_recv_is_value(");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ")");
        }
        emit_conversion_suffix(out, suffix);
        return;
    }

    if (v->op == XI_CHAN_IS_CLOSED) {
        if (v->nargs < 1) {
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_IS_CLOSED missing channel\n");
            emit_codegen_abort_expr(out);
            ctx->error = true;
            return;
        }
        XrRep rep = cg_value_decl_storage_rep(ctx, f, v);
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, rep);
        fprintf(out, "xr_aot_chan_is_closed_sync(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ")");
        emit_conversion_suffix(out, suffix);
        return;
    }

    if (v->op == XI_CHAN_TRY_SEND) {
        if (v->nargs < 2) {
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_TRY_SEND missing operands\n");
            emit_codegen_abort_expr(out);
            ctx->error = true;
            return;
        }
        XrRep rep = cg_value_decl_storage_rep(ctx, f, v);
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, rep);
        fprintf(out, "xr_aot_chan_try_send_ready_transfer(%s, ", xicgen_aot_context_expr(ctx, f));
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", %u)", (unsigned) xi_chan_send_transfer_mode(v));
        emit_conversion_suffix(out, suffix);
        return;
    }

    if (v->op == XI_CORO_OP) {
        XrRep rep = cg_value_decl_storage_rep(ctx, f, v);
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, rep);
        fprintf(out, "xr_aot_coro_op(%s, %d, ", xicgen_aot_context_expr(ctx, f), (int) v->aux_int);
        if (v->nargs == 0) {
            fprintf(out, "NULL");
        } else {
            fprintf(out, "(XrValue[]){");
            for (uint16_t i = 0; i < v->nargs; i++) {
                if (i > 0)
                    fprintf(out, ", ");
                emit_boxed_value_ref(out, v->args[i]);
            }
            fprintf(out, "}");
        }
        fprintf(out, ", %u)", (unsigned) v->nargs);
        emit_conversion_suffix(out, suffix);
        return;
    }

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
    const char *ctor_prefix; /* owned: XiModule prefix (Xi arena)/NULL; local struct, emit-scope */
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
                if ((user->op == XI_COPY || xi_op_is_identity_forward(user->op)) && ai == 0) {
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
        emit_value_as_direct_call_arg(ctx, out, f, info.ctor_call, info.ctor, i,
                                      info.ctor_call->args[i]);
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

static bool cg_return_value_needs_owned_array_ref(const XiValue *value, XrRep ret_rep) {
    const XiValue *v = cg_unwrap_identity_value(value);
    return ret_rep == XR_REP_TAGGED && v && v->type && v->type->kind == XR_KIND_FIXED_ARRAY;
}

static bool cg_return_value_is_borrowed_fixed_array_storage(const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    CgFixedArrayLaneInfo info;
    if (!v || !cg_fixed_array_lane_info_from_value(v, &info))
        return false;
    /* XI_PLACE_LOAD constructs an array-ref view over the place's raw lane
     * pointer; that wrapper is always borrowed even when the place itself came
     * through a ref parameter. */
    return info.stack_origin || v->op == XI_PLACE_LOAD;
}

static void emit_return_value_as_rep_ctx(XiCgenCtx *ctx, FILE *out, const XiFunc *func,
                                         const XiValue *value, XrRep ret_rep) {
    if (cg_return_value_needs_owned_array_ref(value, ret_rep)) {
        /* A fixed array backed by a local C lane array is provably borrowed.
         * Clone it directly instead of emitting
         * a runtime ownership branch: the direct form makes the stack escape
         * boundary explicit to analyzers and still performs the mandatory
         * copy before the function returns.  Fixed-array values from calls or
         * other potentially-owned sources keep the conditional conversion so
         * an already-owned result is not cloned or leaked. */
        if (cg_return_value_is_borrowed_fixed_array_storage(value)) {
            fprintf(out, "xrt_array_ref_clone_value(");
            emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
            fprintf(out, ")");
            return;
        }
        fprintf(out, "({ XrValue _xrv = ");
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        fprintf(out,
                "; (XR_IS_ARRAY_REF(_xrv) && (_xrv.flags & XRT_VALUE_FLAG_ARRAY_REF_OWNED) == 0) "
                "? xrt_array_ref_to_owned(_xrv) : _xrv; })");
        return;
    }
    XrRep value_rep = cg_value_plan_storage_rep(ctx, value);
    if (value_rep != ret_rep && func && cg_unit_enum_scalar_plan(ctx, func->return_type)) {
        const char *suffix =
            emit_conversion_prefix_ctx(ctx, out, func->return_type, value_rep, ret_rep);
        emit_vref(out, value);
        emit_conversion_suffix(out, suffix);
        return;
    }
    emit_value_as_rep_ctx(ctx, out, value, ret_rep);
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

static bool cg_await_all_inline_literal_value_is_elided(const XiFunc *f, const XiValue *v);

#include "xi_cgen_stmt_dispatch_helpers.inc.c"

typedef struct CgDebugSourceVarInfo {
    const char *name;  /* owned: Xi source-var name (Xi arena); local struct, emit-scope only */
    const char *ctype; /* owned: static ctype literal or points into ctype_buf below */
    char ctype_buf[160];
    XrRep rep;
    bool is_vector;
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

static const XrAggregateLayout *cg_debug_type_struct_layout(const XrType *type) {
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE) ||
        !type->instance.class_ref)
        return NULL;
    return type->instance.class_ref->struct_layout;
}

static const XrAggregateLayout *cg_debug_value_struct_layout(XiCgenCtx *ctx, const XiFunc *f,
                                                             const XiValue *v) {
    if (!v)
        return NULL;

    const XiValue *cur = v;
    for (int depth = 0; cur && depth <= 8; depth++) {
        if (cur->op == XI_AGG_NEW)
            return (const XrAggregateLayout *) cur->aux;
        if ((cur->op == XI_COPY || xi_op_is_identity_forward(cur->op) || cur->op == XI_RETAIN) &&
            cur->nargs >= 1) {
            cur = cur->args[0];
            continue;
        }
        break;
    }

    const XrAggregateLayout *shared_layout = NULL;
    if (cg_value_traces_to_heap_struct_shared(ctx, f, v, &shared_layout, NULL))
        return shared_layout;

    return cg_debug_type_struct_layout(v->type);
}

static bool cg_debug_value_struct_ptr_ctype(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                            char *buf, size_t buflen) {
    if (!buf || buflen == 0)
        return false;
    const XrAggregateLayout *sl = cg_debug_value_struct_layout(ctx, f, v);
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
            cg_value_traces_to_static_struct_whole_store(ctx, f, v) ||
            cg_value_is_elided_heap_struct_alias(ctx, f, v))
            return false;
        return true;
    }
    if (v->op == XI_AGG_NEW && cg_struct_inline_local_storage(ctx, f, v))
        return false;
    if ((v->op == XI_COPY || xi_op_is_identity_forward(v->op)) &&
        (cg_value_traces_to_inlined_struct(f, v) ||
         cg_value_traces_to_static_struct_whole_store(ctx, f, v) ||
         cg_value_is_elided_heap_struct_alias(ctx, f, v)))
        return false;
    if (cg_value_traces_to_inlined_struct(f, v) ||
        cg_value_traces_to_static_struct_whole_store(ctx, f, v) ||
        cg_value_is_elided_heap_struct_alias(ctx, f, v) ||
        cg_value_is_elided_nested_struct_ref(f, v) || cg_value_is_elided_fixed_array_ref(f, v) ||
        cg_value_is_elided_static_struct_nested_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_struct_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_struct_nested_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_array_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_matrix_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_matrix_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_cube_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_cube_outer_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_cube_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_nested_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_nested_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_tuple_array_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_tuple_array_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_tuple_array_tuple_ref(ctx, f, v) ||
        cg_value_is_elided_static_tuple_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_struct_const_ref(ctx, f, v) ||
        cg_value_is_elided_layout_struct_type_load(f, v))
        return false;
    if (cg_ownership_op_is_noop(ctx && ctx->freestanding_profile, v) ||
        cg_shared_static_function_ownership_is_noop(ctx, f, v))
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
    bool is_vector = false;
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
            is_vector = cg_value_plan_is_vector(ctx, storage_v);
            found = true;
        } else if (strcmp(ctype, cur_ctype) != 0 || rep != cur_rep ||
                   is_vector != cg_value_plan_is_vector(ctx, storage_v)) {
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
                is_vector = cg_value_plan_is_vector(ctx, storage_v);
                found = true;
            } else if (strcmp(ctype, cur_ctype) != 0 || rep != cur_rep ||
                       is_vector != cg_value_plan_is_vector(ctx, storage_v)) {
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
                is_vector = cg_value_plan_is_vector(ctx, storage_v);
                found = true;
            } else if (strcmp(ctype, cur_ctype) != 0 || rep != cur_rep ||
                       is_vector != cg_value_plan_is_vector(ctx, storage_v)) {
                return false;
            }
        }
    }

    if (!found)
        return false;
    out_info->name = f->source_var_names[var_id];
    out_info->ctype = ctype;
    out_info->rep = rep;
    out_info->is_vector = is_vector;
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
        if (strcmp(info.ctype, "xr_span_t") == 0)
            fprintf(out, "xrt_span_empty()");
        else if (strcmp(info.ctype, "XrAotEnumAggregate") == 0)
            fprintf(out, "xrt_enum_aggregate_zero()");
        else if (strncmp(info.ctype, "xrt_enum_", 9) == 0)
            fprintf(out, "%s_from_base(xrt_enum_aggregate_zero())", info.ctype);
        else if (strncmp(info.ctype, "xrt_struct_", 11) == 0)
            fprintf(out, "((%s){0})", info.ctype);
        else if (info.is_vector)
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
    if (strcmp(info.ctype, "xr_span_t") == 0) {
        if (cg_value_plan_is_span_aggregate(ctx, storage_v)) {
            emit_vref(out, storage_v);
        } else {
            fprintf(out, "xrt_span_empty()");
        }
    } else if (cg_value_plan_is_struct_aggregate(ctx, storage_v)) {
        emit_vref(out, storage_v);
    } else if (strcmp(info.ctype, "XrAotEnumAggregate") == 0 ||
               strncmp(info.ctype, "xrt_enum_", 9) == 0) {
        if (cg_value_plan_is_aggregate(ctx, storage_v)) {
            emit_vref(out, storage_v);
        } else {
            const XaotValuePlan *plan = cg_value_plan(ctx, storage_v);
            XaotValueRep rep = plan ? plan->rep : (XaotValueRep) {0};
            rep.kind = XAOT_VALUE_AGGREGATE;
            rep.flags |= XAOT_VALUE_FLAG_ENUM;
            rep.c_type = info.ctype;
            emit_adt_base_to_value_rep_prefix(out, rep);
            fprintf(out, "xrt_enum_aggregate_from_boxed(");
            emit_value_as_rep_ctx(ctx, out, storage_v, XR_REP_TAGGED);
            fprintf(out, ")");
            emit_adt_base_to_value_rep_suffix(out, rep);
        }
    } else if (info.is_vector) {
        if (cg_value_plan_is_vector(ctx, storage_v))
            emit_vref(out, storage_v);
        else
            fprintf(out, "((%s){0})", info.ctype);
    } else if (strcmp(info.ctype, "XrValue") == 0) {
        emit_value_as_rep_ctx(ctx, out, storage_v, info.rep);
    } else {
        fprintf(out, "(%s)", info.ctype);
        emit_value_as_rep_ctx(ctx, out, storage_v, info.rep);
    }
    fprintf(out, ";\n");
    fprintf(out, "#endif\n");
}

/*
 * A value-ABI struct loaded through a place is sometimes consumed only by
 * direct field operations.  Those field emitters deliberately address the
 * original place so a scalar access does not copy the whole aggregate first.
 * In that case the aggregate temporary has no non-debug C consumer: it exists
 * only so XRAY_AOT_DEBUG_LOCALS can show the source-level receiver/local.
 *
 * Keep this predicate deliberately narrow.  A control/phi use, an identity
 * chain, or any non-field consumer still requires the ordinary aggregate
 * temporary and therefore fails closed.
 */
static bool cg_struct_place_load_only_feeds_direct_fields(XiCgenCtx *ctx, const XiFunc *f,
                                                          const XiValue *target) {
    if (!ctx || !f || !target || target->op != XI_PLACE_LOAD || target->nargs != 1 ||
        !target->args[0] || !cg_value_plan_is_struct_aggregate(ctx, target) ||
        cg_value_has_cell(ctx, target))
        return false;
    if (target->flags &
        (XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND | XI_FLAG_SIDE_EFFECT))
        return false;

    bool seen_field_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == target)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != target)
                    continue;
                if (a == 0 && (user->op == XI_AGG_GET || user->op == XI_AGG_SET) && user->aux) {
                    seen_field_use = true;
                    continue;
                }
                return false;
            }
        }
    }
    return seen_field_use;
}

/*
 * `unsafe { ptr.deref().method() }` retains an XI_PTR_LOAD so the frontend can
 * type-check the value receiver, but lowering also records that the following
 * XI_LOCAL_ADDR is the address of the original raw pointer target.  The native
 * address emitter therefore bypasses the aggregate result entirely.  Do not
 * copy a potentially large value-ABI struct merely to preserve an SSA value
 * whose only release-C consumer is that provenance-qualified address.
 *
 * A load used as a value, carried through a phi/control edge, or addressed by
 * any ordinary local-address operation still materializes and fails closed.
 */
static bool cg_struct_ptr_load_only_feeds_raw_deref_address(XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiValue *target) {
    if (!ctx || !f || !target || target->op != XI_PTR_LOAD || target->nargs < 1 ||
        !target->args[0] || !cg_value_plan_is_struct_aggregate(ctx, target) ||
        cg_value_has_cell(ctx, target))
        return false;
    if (target->flags &
        (XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND | XI_FLAG_SIDE_EFFECT))
        return false;

    bool seen_raw_deref_address = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == target)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != target)
                    continue;
                if (a == 0 && user->op == XI_LOCAL_ADDR &&
                    (user->aux_int & XI_LOCAL_ADDR_AUX_RAW_DEREF) != 0) {
                    seen_raw_deref_address = true;
                    continue;
                }
                if (a == 0 && (user->op == XI_RETAIN || user->op == XI_RELEASE))
                    continue;
                if (user->op == XI_ERR_CHECK && xi_err_check_has_arc_cleanups(user))
                    continue;
                return false;
            }
        }
    }
    return seen_raw_deref_address;
}

static bool cg_value_only_feeds_slice_from_ptr_owner(const XiFunc *f, const XiValue *target,
                                                     uint8_t depth) {
    if (!f || !target || depth > 8)
        return false;
    bool seen_owner = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == target)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != target)
                    continue;
                if (a == 2 && user->op == XI_SLICE_FROM_PTR && user->nargs == 3) {
                    seen_owner = true;
                    continue;
                }
                if (a == 0 && (user->op == XI_RETAIN || user->op == XI_RELEASE))
                    continue;
                if (a == 0 &&
                    (cg_is_identity_copy_or_move(user) || user->op == XI_BOX ||
                     user->op == XI_UNBOX) &&
                    cg_value_only_feeds_slice_from_ptr_owner(f, user, (uint8_t) (depth + 1))) {
                    seen_owner = true;
                    continue;
                }
                return false;
            }
        }
    }
    return seen_owner;
}

/*
 * A native AGG_GET can likewise exist only to carry a scalar field's exact
 * place into XI_LOCAL_ADDR, or a pointer field solely as mem.slice lifetime
 * evidence.  The direct-projection address emitter borrows the field inside
 * the original aggregate place, while mem.slice omits its owner operand from
 * C.  The receiver predicate ties both shapes to the already-proven value-ABI
 * place path; arbitrary or observable field reads do not qualify.
 */
static bool cg_struct_scalar_field_load_has_no_release_value_use(XiCgenCtx *ctx, const XiFunc *f,
                                                                 const XiValue *target) {
    if (!ctx || !f || !target || target->op != XI_AGG_GET || target->nargs < 1 ||
        !target->args[0] || !target->aux || cg_value_has_cell(ctx, target))
        return false;
    const XrAggregateLayout *layout = (const XrAggregateLayout *) target->aux;
    const XrAggregateFieldLayout *field = cg_struct_field(layout, target->aux_int);
    bool scalar_projection = field && cg_static_struct_native_scalar_supported(field->native_type);
    bool pointer_lifetime = field && target->type && XR_TYPE_IS_POINTER(target->type);
    if (!scalar_projection && !pointer_lifetime)
        return false;
    if (scalar_projection &&
        !cg_struct_place_load_only_feeds_direct_fields(ctx, f, target->args[0]))
        return false;
    if (pointer_lifetime)
        return cg_value_only_feeds_slice_from_ptr_owner(f, target, 0);
    if (target->flags &
        (XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND | XI_FLAG_SIDE_EFFECT))
        return false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == target)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != target)
                    continue;
                if (a == 0 && user->op == XI_LOCAL_ADDR &&
                    (user->aux_int & XI_LOCAL_ADDR_AUX_DIRECT_PROJECTION) != 0 && scalar_projection)
                    continue;
                /* The third mem.slice operand is compile-time lifetime
                 * evidence. xicgen_slice_from_ptr deliberately emits only
                 * the pointer and count, so this field value has no C use. */
                if (a == 2 && user->op == XI_SLICE_FROM_PTR && user->nargs == 3)
                    continue;
                if (a == 0 && (user->op == XI_RETAIN || user->op == XI_RELEASE))
                    continue;
                return false;
            }
        }
    }
    return true;
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
        if (method && strcmp(method, "push") == 0)
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
            /* Unit ERR_CHECK operands are ARC owners used only on the cold
             * pending-error exit.  They are not runtime consumers of the task
             * array and must not defeat scalarization after ARC finalization. */
            if (v->op == XI_ERR_CHECK && !cg_value_type_is_bool(v) &&
                xi_err_check_has_arc_cleanups(v))
                continue;
            if (v->nargs == 1 && (v->op == XI_BOX || v->op == XI_UNBOX ||
                                  xi_copy_is_identity_alias(v) || xi_op_is_identity_forward(v->op)))
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
        if (method && strcmp(method, "push") == 0)
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
        if (cur->nargs == 1 &&
            (cur->op == XI_BOX || cur->op == XI_UNBOX || xi_copy_is_identity_alias(cur) ||
             xi_op_is_identity_forward(cur->op))) {
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
         xi_op_is_identity_forward(user->op)))
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
                 xi_op_is_identity_forward(value->op) || value->op == XI_RETAIN ||
                 value->op == XI_RELEASE))
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

static bool cg_direct_call_arg_consumes_int_widen_inner(XiCgenCtx *ctx, const XiFunc *f,
                                                        const XiValue *call, uint16_t arg_index,
                                                        const XiValue *widen) {
    if (!ctx || !call || call->op != XI_CALL || arg_index == 0 || !call->args || !widen)
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

    return cg_int_widen_can_use_inner_for_slot(
        ctx, f, widen, xaot_abi_slot_value_rep(&target_plan->abi.params[param_index]), NULL, NULL);
}

static bool cg_int_widen_use_consumes_inner(XiCgenCtx *ctx, const XiFunc *f, const XiValue *widen,
                                            const XiValue *user, uint16_t arg_index) {
    if (!ctx || !f || !widen || !user)
        return false;
    const XiValue *inner = NULL;
    if (!cg_int_widen_inner_value_plan(ctx, widen, &inner, NULL))
        return false;
    if (cg_direct_call_arg_consumes_int_widen_inner(ctx, f, user, arg_index, widen))
        return true;
    if (cg_lowbits_binop_elided_into_unsigned_narrow(f, user) &&
        cg_arith_narrow_src(ctx, f, widen, NULL, NULL) == inner)
        return true;
    return false;
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

static bool cg_aot_const_bool_value(const XiValue *value, bool *out) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_CONST || !v->type || v->type->kind != XR_KIND_BOOL || !out)
        return false;
    *out = v->aux_int != 0;
    return true;
}

static const XiValue *cg_aot_present_direct_bool_map_get(XiCgenCtx *ctx, const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (v && v->op == XI_BOX && v->nargs >= 1)
        v = cg_unwrap_identity_value(v->args[0]);
    if (!v || !cg_value_is_map_method(v, "get") || cg_rep(v) != XR_REP_I64)
        return NULL;

    CgMapElemInfo info;
    if (!cg_map_type_direct_info_ctx(ctx, v->args[0] ? v->args[0]->type : NULL, &info))
        return NULL;
    if (strcmp(info.value.elem_name, "XR_ELEM_BOOL") != 0)
        return NULL;
    return cg_map_get_fusion_has(ctx, v) ? v : NULL;
}

static bool cg_aot_compare_present_bool_map_get_const(XiCgenCtx *ctx, const XiValue *compare,
                                                      const XiValue **out_get,
                                                      bool *out_const_value) {
    if (!ctx || !compare || (compare->op != XI_EQ && compare->op != XI_NE) || compare->nargs < 2)
        return false;

    const XiValue *get = cg_aot_present_direct_bool_map_get(ctx, compare->args[0]);
    bool const_value = false;
    if (get && cg_aot_const_bool_value(compare->args[1], &const_value)) {
        if (out_get)
            *out_get = get;
        if (out_const_value)
            *out_const_value = const_value;
        return true;
    }

    get = cg_aot_present_direct_bool_map_get(ctx, compare->args[1]);
    if (get && cg_aot_const_bool_value(compare->args[0], &const_value)) {
        if (out_get)
            *out_get = get;
        if (out_const_value)
            *out_const_value = const_value;
        return true;
    }
    return false;
}

static bool cg_fixed_array_index_use_consumes_native(const XiValue *user, uint16_t arg_index) {
    if (!user || (user->op != XI_INDEX_GET && user->op != XI_INDEX_SET) || user->nargs < 2)
        return false;

    CgFixedArrayLaneInfo info;
    bool have_info = cg_fixed_array_lane_info_from_value(user->args[0], &info);
    if (!have_info) {
        const XiValue *ref = cg_trace_fixed_array_field_ref(user->args[0]);
        const XrAggregateLayout *layout = ref ? (const XrAggregateLayout *) ref->aux : NULL;
        const XrAggregateFieldLayout *field = ref ? cg_struct_field(layout, ref->aux_int) : NULL;
        if (!field || field->native_type != XR_NATIVE_ARRAY)
            return false;
        info.rep = cg_struct_native_rep(field->elem_native_type);
        have_info = true;
    }
    if (!have_info)
        return false;
    if (user->op == XI_INDEX_GET)
        return arg_index == 1;
    if (user->nargs < 3)
        return false;
    return arg_index == 1 || (arg_index == 2 && info.rep != XR_REP_TAGGED);
}

static bool cg_byte_slice_load_use_consumes_native(XiCgenCtx *ctx, const XiValue *user,
                                                   uint16_t arg_index) {
    if (!ctx || !user || user->nargs != 3 || (arg_index != 1 && arg_index != 2))
        return false;
    switch ((XiOp) user->op) {
        case XI_BYTE_SLICE_LOAD_U16:
        case XI_BYTE_SLICE_LOAD_U32:
        case XI_BYTE_SLICE_LOAD_U64:
        case XI_BYTE_SLICE_LOAD_F32:
        case XI_BYTE_SLICE_LOAD_F64:
            break;
        default:
            return false;
    }
    CgArrayElemInfo info;
    return cg_span_value_u8_info(ctx, user->args[0], &info) &&
           cg_span_plan_drops(ctx, user, XAOT_SLICE_ACCESS_BYTE_LOAD, XAOT_SLICE_DROP_HELPER);
}

static bool cg_byte_slice_store_use_consumes_native(XiCgenCtx *ctx, const XiValue *user,
                                                    uint16_t arg_index) {
    if (!ctx || !user || user->nargs != 4 || arg_index < 1 || arg_index > 3)
        return false;
    switch ((XiOp) user->op) {
        case XI_BYTE_SLICE_STORE_U16:
        case XI_BYTE_SLICE_STORE_U32:
        case XI_BYTE_SLICE_STORE_U64:
        case XI_BYTE_SLICE_STORE_F32:
        case XI_BYTE_SLICE_STORE_F64:
            break;
        default:
            return false;
    }
    CgArrayElemInfo info;
    return cg_span_value_u8_info(ctx, user->args[0], &info);
}

static bool cg_full_fixed_array_slice_use_consumes_native(XiCgenCtx *ctx, const XiValue *user,
                                                          uint16_t arg_index) {
    if (!ctx || !user || user->op != XI_SLICE || user->nargs < 3 ||
        (arg_index != 1 && arg_index != 2) || !cg_value_plan_is_span_aggregate(ctx, user))
        return false;

    const XiValue *source = cg_unwrap_identity_value(user->args[0]);
    CgFixedArrayLaneInfo fixed;
    const XiModule *static_module = NULL;
    int64_t static_slot = -1;
    int64_t start = 0;
    int64_t end = 0;
    bool static_source =
        cg_static_fixed_array_value_ex(ctx, source, &fixed, &static_slot, &static_module);
    return source && (static_source || cg_fixed_array_lane_info_from_value(source, &fixed)) &&
           fixed.ctype && cg_const_int_value(user->args[1], &start) &&
           cg_const_int_value(user->args[2], &end) && start == 0 &&
           (end == INT64_MAX || end == (int64_t) fixed.count);
}

static bool cg_slice_from_ptr_use_consumes_native(XiCgenCtx *ctx, const XiValue *user,
                                                  uint16_t arg_index) {
    if (!ctx || !user || user->op != XI_SLICE_FROM_PTR || user->nargs != 3 || arg_index > 2 ||
        !cg_value_plan_is_span_aggregate(ctx, user))
        return false;
    uint16_t elem_size = (uint16_t) ((user->aux_int >> 8) & 0xffff);
    uint16_t alignment = (uint16_t) ((user->aux_int >> 32) & 0xffff);
    return elem_size > 0 && alignment > 0;
}

static bool cg_vec_scalar_use_consumes_native(XiCgenCtx *ctx, const XiValue *user,
                                              uint16_t arg_index) {
    if (!ctx || !user || !xi_vec_shape_is_explicit(user->aux_int))
        return false;
    uint8_t lanes = xi_vec_shape_lanes(user->aux_int);
    uint8_t native_type = xi_vec_shape_native_type(user->aux_int);
    if (lanes == 0 || lanes > 64 ||
        (native_type != XR_NATIVE_U8 && native_type != XR_NATIVE_U32 &&
         native_type != XR_NATIVE_U64))
        return false;

    const char *result_type = NULL;
    bool result_ok = xicgen_vec_result_native(ctx, user, &result_type) ||
                     xicgen_vec_result_aggregate(ctx, user, &result_type);
    switch ((XiOp) user->op) {
        case XI_VEC_LOAD:
            return arg_index == 1 && user->nargs == 2 && result_ok &&
                   xicgen_vec_span_access_uses_direct_trap(ctx, user);
        case XI_VEC_STORE:
            return arg_index == 2 && user->nargs == 3 &&
                   xicgen_vec_span_access_uses_direct_trap(ctx, user);
        case XI_VEC_SPLAT:
            return arg_index == 0 && user->nargs == 1 && result_ok;
        case XI_VEC_REPLACE:
            return (arg_index == 1 || arg_index == 2) && user->nargs == 3 && result_ok;
        case XI_VEC_SHL:
        case XI_VEC_SHR:
            return arg_index == 1 && user->nargs == 2 && result_ok;
        default:
            return false;
    }
}

static bool cg_native_box_use_consumes_native_rep(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *user, uint16_t arg_index) {
    if (!ctx || !user)
        return false;

    XiOp op = (XiOp) user->op;
    if (op == XI_ASSERT && arg_index == 0 && user->nargs >= 1 && user->args[0] &&
        user->args[0]->op == XI_BOX && user->args[0]->type &&
        user->args[0]->type->kind == XR_KIND_BOOL)
        return true;
    if (op == XI_EQ || op == XI_NE) {
        const XiValue *get = NULL;
        bool const_value = false;
        if (!cg_aot_compare_present_bool_map_get_const(ctx, user, &get, &const_value))
            return false;
        return arg_index < user->nargs && user->args[arg_index] &&
               user->args[arg_index]->op == XI_BOX && user->args[arg_index]->nargs >= 1 &&
               cg_unwrap_identity_value(user->args[arg_index]->args[0]) == get;
    }
    if (cg_fixed_array_index_use_consumes_native(user, arg_index))
        return true;
    if (cg_byte_slice_load_use_consumes_native(ctx, user, arg_index))
        return true;
    if (cg_byte_slice_store_use_consumes_native(ctx, user, arg_index))
        return true;
    if (cg_full_fixed_array_slice_use_consumes_native(ctx, user, arg_index))
        return true;
    if (user->op == XI_SLICE_WINDOW && (arg_index == 1 || arg_index == 2) &&
        xicgen_span_window_uses_direct_trap(ctx, user))
        return true;
    if (cg_slice_from_ptr_use_consumes_native(ctx, user, arg_index))
        return true;
    if (cg_vec_scalar_use_consumes_native(ctx, user, arg_index))
        return true;

    CgArrayElemInfo info;
    switch (op) {
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
        case XI_STR_CONCAT:
            /* Only unsigned interpolation selects the direct u64 part
             * emitter.  Other scalar parts request TAGGED and therefore still
             * need their BOX local even in a multi-part concat. */
            return arg_index < user->nargs && cg_value_type_is_unsigned_int(user->args[arg_index]);
        case XI_CALL:
            if (arg_index == 0 && user->nargs >= 1 && user->args[0] && user->args[0]->type &&
                XR_TYPE_IS_C_FUNCTION(user->args[0]->type))
                return true;
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

static bool emit_structured_loop_condition_expr_ctx(XiCgenCtx *ctx, FILE *out,
                                                    const XiValue *control);
static bool cg_structured_counted_loop_block_is_elided(const XiFunc *f, const XiBlock *blk);

/* A scalar/null constant, or a string literal used only by a multi-part
 * concat, has no required C storage when every consumer
 * prints the literal through emit_value_as_rep_ctx() (or an equivalent
 * literal-aware helper).  This is deliberately a lowering-shape predicate,
 * not generic DCE: several native emitters still call emit_vref() and
 * therefore require the constant's local even though the Xi value itself is
 * pure. */
static bool cg_const_use_emits_immediate(XiCgenCtx *ctx, const XiFunc *f, const XiValue *user,
                                         uint16_t arg_index) {
    if (!ctx || !f || !user || arg_index >= user->nargs)
        return false;

    const char *template_op = xi_to_c_template_arith_native_op(user->op);
    if (template_op && *template_op) {
        if (user->nargs >= 2 && cg_rep(user) == XR_REP_RAWPTR &&
            (user->op == XI_ADD || user->op == XI_SUB)) {
            XrRep r0 = cg_rep(user->args[0]);
            XrRep r1 = cg_rep(user->args[1]);
            if (r0 == XR_REP_RAWPTR && (r1 == XR_REP_I64 || r1 == XR_REP_TAGGED))
                return arg_index == 1;
            if (user->op == XI_ADD && r1 == XR_REP_RAWPTR &&
                (r0 == XR_REP_I64 || r0 == XR_REP_TAGGED))
                return arg_index == 0;
        }
        return arg_index < 2 && user->nargs >= 2 && cg_rep(user) == XR_REP_I64 &&
               cg_rep(user->args[0]) == XR_REP_I64 && cg_rep(user->args[1]) == XR_REP_I64 &&
               !cg_arith_is_clean_narrow(ctx, f, user) &&
               !cg_lowbits_binop_elided_into_unsigned_narrow(f, user);
    }

    const char *template_fn = xi_to_c_template_div_mod_runtime_fn(user->op);
    if (template_fn && *template_fn) {
        int64_t divisor = 0;
        if (arg_index == 1 && user->nargs >= 2 && cg_rep(user) == XR_REP_I64 &&
            cg_rep(user->args[0]) == XR_REP_I64 &&
            cg_const_int_value_in_func(ctx, f, user->args[1], &divisor) && divisor != 0)
            return true;
        return arg_index < 2 && user->nargs >= 2 && cg_rep(user) == XR_REP_I64 &&
               cg_type_is_unsigned_int(user->type);
    }

    template_op = xi_to_c_template_bitwise_binary_op(user->op);
    if (template_op && *template_op)
        return arg_index < 2 && !cg_lowbits_binop_elided_into_unsigned_narrow(f, user);

    template_fn = xi_to_c_template_shift_fn(user->op);
    if (template_fn && *template_fn)
        return arg_index < 2;

    template_op = xi_to_c_template_bitwise_unary_op(user->op);
    if (template_op && *template_op)
        return arg_index == 0;

    template_op = xi_to_c_template_compare_native_op(user->op);
    if (template_op && *template_op) {
        if (arg_index >= 2 || user->nargs < 2)
            return false;
        /* A recognized counted-loop guard is not emitted as a value
         * statement.  Its comparison is reconstructed directly in the C
         * `while` condition, whose ctx emitter prints scalar operands through
         * emit_value_as_rep_ctx(). */
        if (user->block && user->block->kind == XI_BLOCK_IF && user->block->control == user &&
            cg_structured_counted_loop_block_is_elided(f, user->block) &&
            emit_structured_loop_condition_expr_ctx(ctx, NULL, user))
            return true;
        if (xicgen_compare_uses_unsigned(user))
            return true;
        if (xicgen_compare_uses_rawptr(user))
            return true;
        XrRep a0 = cg_value_plan_storage_rep(ctx, user->args[0]);
        XrRep a1 = cg_value_plan_storage_rep(ctx, user->args[1]);
        return a0 == XR_REP_TAGGED || a1 == XR_REP_TAGGED;
    }

    if (xi_to_c_template_width_kind(user->op) == AOT_WIDTH_TEMPLATE_CAST_I64) {
        if (arg_index != 0 || user->nargs < 1)
            return false;
        const char *cast_ctype = xi_to_c_template_width_cast_type(user->op);
        const char *arg_ctype = local_ctype_str_ctx(ctx, f, user->args[arg_index]);
        return cast_ctype && *cast_ctype && arg_ctype && strcmp(arg_ctype, cast_ctype) != 0;
    }

    switch ((XiOp) user->op) {
        case XI_BIT_POPCOUNT:
        case XI_BIT_CLZ:
        case XI_BIT_CTZ:
        case XI_BIT_BSWAP:
        case XI_BIT_MUL_HIGH:
        case XI_BIT_ROTL:
        case XI_BIT_ROTR:
            return true;
        case XI_AS:
            /* xicgen_as routes scalar operands through emit_value_as_rep_ctx()
             * for both representation-only and runtime-checked casts. */
            return arg_index == 0;
        case XI_CONVERT:
            /* Typed numeric conversions likewise consume scalar literals
             * directly; their conversion witness controls semantics without
             * requiring a separate source C local. */
            return arg_index == 0;
        case XI_PLACE_STORE:
            /* Scalar place stores convert the stored value with the
             * literal-aware representation emitter. */
            return arg_index == 1;
        case XI_SET_SHARED:
            /* Shared-slot stores convert their sole value argument through
             * emit_value_as_rep_ctx(), including the ownership handoff. */
            return arg_index == 0;
        case XI_PTR_LOAD: {
            int64_t endian = XR_ENDIAN_NATIVE;
            return arg_index == 1 && user->nargs == 2 &&
                   xicgen_value_is_const_endian(user->args[1], &endian);
        }
        case XI_PTR_STORE: {
            int64_t endian = XR_ENDIAN_NATIVE;
            return arg_index == 2 && user->nargs == 3 &&
                   xicgen_value_is_const_endian(user->args[2], &endian);
        }
        case XI_PTR_COPY_NONOVERLAP: {
            int64_t byte_count = 0;
            /* The memcpy emitter folds an identity-forwarded non-negative
             * byte count into its size_t literal and never references the
             * forwarding C local. */
            return arg_index == 2 && user->nargs == 3 &&
                   cg_const_int_value(user->args[2], &byte_count) && byte_count >= 0;
        }
        case XI_BYTE_SLICE_LOAD_U16:
        case XI_BYTE_SLICE_LOAD_U32:
        case XI_BYTE_SLICE_LOAD_U64:
        case XI_BYTE_SLICE_LOAD_F32:
        case XI_BYTE_SLICE_LOAD_F64:
            return user->nargs == 3 && (arg_index == 1 || arg_index == 2);
        case XI_BYTE_SLICE_STORE_U16:
        case XI_BYTE_SLICE_STORE_U32:
        case XI_BYTE_SLICE_STORE_U64:
        case XI_BYTE_SLICE_STORE_F32:
        case XI_BYTE_SLICE_STORE_F64:
            return user->nargs == 4 && arg_index >= 1 && arg_index <= 3;
        case XI_STORE_FIELD:
        case XI_AGG_SET:
            /* Every field-store backend converts the stored value with the
             * literal-aware representation emitter. */
            return arg_index == 1 && user->nargs >= 2;
        case XI_STR_CONCAT:
            /* Multi-part concat lowers every part through the literal-aware
             * representation emitter.  The one-part string fast path still
             * calls emit_vref() and therefore deliberately fails closed. */
            return user->nargs > 1;
        case XI_ARRAY_NEW:
            /* Array constructors fold a direct scalar capacity into the
             * selected typed or generic allocation expression. */
            return arg_index == 0 && user->nargs >= 1 && user->args[0] &&
                   user->args[0]->op == XI_CONST;
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT: {
            /* string.runes() is emitted as a dynamic method call whose
             * receiver is rendered through emit_value_as_rep_ctx(). */
            if (arg_index == 0 && user->nargs >= 1 && user->args[0] && user->args[0]->type &&
                user->args[0]->type->kind == XR_KIND_STRING && user->aux &&
                strcmp((const char *) user->aux, "runes") == 0)
                return true;
            /* The runtime string-slice helper and float formatting helper
             * render their scalar arguments with emit_value_as_rep_ctx(). */
            if (arg_index >= 1 && user->args[0] && user->args[0]->type && user->aux &&
                (user->args[0]->type->kind == XR_KIND_STRING ||
                 user->args[0]->type->kind == XR_KIND_UNKNOWN) &&
                strcmp((const char *) user->aux, "slice") == 0 && user->nargs <= 3)
                return true;
            if (arg_index == 1 && user->nargs == 2 && user->args[0] && user->args[0]->type &&
                user->args[0]->type->kind == XR_KIND_FLOAT && user->aux &&
                strcmp((const char *) user->aux, "toFixed") == 0)
                return true;
            CgArrayElemInfo array_info;
            if (arg_index == 1 && user->nargs == 2 &&
                cg_call_method_matches_receiver_registry_id(
                    user, XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH) &&
                cg_array_value_storage_info(ctx, f, user->args[0], &array_info,
                                            CG_ARRAY_STORAGE_MUTABLE))
                return true;
            if (arg_index >= 1 && user->nargs > 1 && user->aux) {
                const XiEnumData *recv_enum = cg_enum_for_namespace_value(user->args[0]);
                if (!recv_enum)
                    recv_enum = cg_enum_for_shared_value_in_func(ctx, f, user->args[0]);
                if (!recv_enum)
                    recv_enum = cg_resolve_imported_enum_value(ctx, f, user->args[0]);
                if (!recv_enum)
                    recv_enum = xicgen_adt_enum_for_type(ctx, user->type);
                int member = cg_enum_member_index(recv_enum, (const char *) user->aux);
                if (recv_enum && recv_enum->is_adt && member >= 0 &&
                    (uint32_t) member < recv_enum->member_count && recv_enum->members &&
                    recv_enum->members[member].payload_count > 0)
                    return true;
            }
            return false;
        }
        case XI_INDEX_GET:
        case XI_INDEX_SET: {
            CgArrayElemInfo span_info;
            if (user->nargs >= 2 && arg_index == 1 &&
                cg_value_plan_is_span_aggregate(ctx, user->args[0]) &&
                cg_span_elem_info_from_value(ctx, user->args[0], &span_info))
                return true;
            CgArrayElemInfo array_info;
            CgArrayStorageUse array_use =
                user->op == XI_INDEX_GET ? CG_ARRAY_STORAGE_READ : CG_ARRAY_STORAGE_MUTABLE;
            if (cg_array_value_storage_info(ctx, f, user->args[0], &array_info, array_use)) {
                if (user->op == XI_INDEX_GET)
                    return arg_index == 1;
                return user->nargs >= 3 && (arg_index == 1 || arg_index == 2);
            }
            CgFixedArrayLaneInfo info;
            if (cg_trace_fixed_array_field_ref(user->args[0]) ||
                !cg_fixed_array_lane_info_from_value(user->args[0], &info))
                return false;
            if (user->op == XI_INDEX_GET)
                return arg_index == 1;
            return user->nargs >= 3 && (arg_index == 1 || arg_index == 2);
        }
        default:
            return false;
    }
}

static bool cg_forwarded_const_only_emits_immediate(XiCgenCtx *ctx, const XiFunc *f,
                                                    const XiValue *forward, uint8_t depth) {
    if (!ctx || !f || !forward || depth > 8 || forward->nargs != 1 ||
        (!xi_copy_is_identity_alias(forward) && !xi_op_is_identity_forward(forward->op)) ||
        !f->phi_coalesce || forward->id >= f->phi_coalesce_count ||
        f->phi_coalesce[forward->id] == forward->id)
        return false;

    bool seen_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == forward)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == forward)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == forward)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != forward)
                    continue;
                seen_use = true;
                if (a == 0 &&
                    (xi_copy_is_identity_alias(user) || xi_op_is_identity_forward(user->op)) &&
                    cg_forwarded_const_only_emits_immediate(ctx, f, user, (uint8_t) (depth + 1)))
                    continue;
                if (!cg_const_use_emits_immediate(ctx, f, user, a))
                    return false;
            }
        }
    }
    return seen_use || !cg_debug_value_has_source_storage(ctx, f, forward);
}

static bool cg_const_only_emits_immediate(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!ctx || !f || !v || v->op != XI_CONST || !v->type || cg_value_has_cell(ctx, v))
        return false;
    if (v->type->kind != XR_KIND_INT && v->type->kind != XR_KIND_BOOL &&
        v->type->kind != XR_KIND_RUNE && v->type->kind != XR_KIND_FLOAT &&
        v->type->kind != XR_KIND_NULL && v->type->kind != XR_KIND_STRING)
        return false;
    if (v->flags &
        (XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND))
        return false;

    bool seen_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == v) {
            /* Return lowering prints scalar/null constants through
             * emit_return_value_as_rep_ctx(), so the Xi control edge does not
             * require a C local for the literal.  Other terminators still
             * reference their control value by name.  String returns remain
             * materialized until their ownership contract is proven
             * independently. */
            if (blk->kind != XI_BLOCK_RETURN || v->type->kind == XR_KIND_STRING)
                return false;
            seen_use = true;
        }
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] != v)
                    continue;
                seen_use = true;
                if (cg_value_has_cell(ctx, &phi->value) ||
                    cg_value_plan_is_aggregate(ctx, &phi->value) ||
                    cg_value_plan_is_vector(ctx, &phi->value))
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
                if (v->type->kind == XR_KIND_STRING) {
                    bool allowed_string_use =
                        (user->op == XI_STR_CONCAT && user->nargs > 1) ||
                        (user->op == XI_SET_SHARED && a == 0) ||
                        ((user->op == XI_CALL_METHOD || user->op == XI_CALL_METHOD_DIRECT) &&
                         a == 0 && user->aux && strcmp((const char *) user->aux, "runes") == 0);
                    if (!allowed_string_use)
                        return false;
                }
                if (!cg_const_use_emits_immediate(ctx, f, user, a) &&
                    !cg_forwarded_const_only_emits_immediate(ctx, f, user, 0))
                    return false;
            }
        }
    }
    /* A dead literal has no release-mode C representation at all.  Preserve a
     * source-bound literal only for XRAY_AOT_DEBUG_LOCALS; otherwise the
     * all-values C/C++ predeclaration pass must omit it together with the
     * statement emitter. */
    return seen_use || !cg_debug_value_has_source_storage(ctx, f, v);
}

static bool cg_static_enum_namespace_uses_are_elidable(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *value, int depth);

static bool cg_module_namespace_field_ignores_receiver(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *load, uint16_t arg_index) {
    if (!ctx || !f || !load || arg_index != 0 || load->op != XI_LOAD_FIELD || load->nargs < 1 ||
        !load->args[0] || !load->aux || !cg_value_is_module_import_ctx(ctx, f, load->args[0], "os"))
        return false;
    const char *field = (const char *) load->aux;
    return strcmp(field, "platform") == 0 || strcmp(field, "arch") == 0 ||
           strcmp(field, "sep") == 0 || strcmp(field, "eol") == 0;
}

static bool cg_import_ref_has_no_emitted_c_use(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!ctx || !f || !v || v->op != XI_IMPORT_REF || !v->aux || cg_value_has_cell(ctx, v))
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
                if (a != 0 || user->op != XI_CALL ||
                    !cg_aot_stdlib_import_call_is_direct(ctx, f, user))
                    return false;
            }
        }
    }
    /* A dead native BOX can survive with a conservative Xi use count after
     * DCE.  It is a pure representation boundary, so omit it unless it owns a
     * source debugger slot that must remain observable in debug-local builds. */
    return seen_use || !cg_debug_value_has_source_storage(ctx, f, v);
}

/* A shared slot load is a semantic memory read in Xi, but the AOT C target is
 * an ordinary non-volatile array access.  It needs no C local when it is either
 * unused or appears only as the callee/namespace operand of a fully resolved
 * capture-free direct call: those emitters print the target symbol and omit
 * the slot value entirely.  Source-bound values are emitted only behind the
 * debug-local guard so their source synchronization remains exact. */
static bool cg_shared_load_has_no_emitted_c_use(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!ctx || !f || !v || v->op != XI_GET_SHARED || v->aux_int < 0 || cg_value_has_cell(ctx, v))
        return false;
    const XiModule *owner_mod = cg_module_for_func(ctx, f);
    if (!owner_mod || v->aux_int >= owner_mod->nslots)
        return false;
    if (cg_static_enum_namespace_uses_are_elidable(ctx, f, v, 0))
        return true;

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
                if (cg_module_namespace_field_ignores_receiver(ctx, f, user, a))
                    continue;
                if (a == 0 && (user->op == XI_CALL || user->op == XI_TAIL_CALL)) {
                    CgStaticFunctionCall call =
                        cg_resolve_static_function_call(ctx, f, user->args[0]);
                    if (call.func && call.func->ncaptures == 0)
                        continue;
                    const XiClassData *cd = cg_class_native_class_value_data(ctx, f, v);
                    if (cd && cg_class_descriptor_ctor_call_is_elidable(ctx, f, user, cd))
                        continue;
                    const XiFunc *ctor = NULL;
                    const XiClassData *ctor_cd =
                        cg_class_native_ctor_call_data(ctx, f, user, &ctor, NULL);
                    if (ctor_cd && ctor && !cg_func_needs_aot_coro_ctx(ctx, ctor))
                        continue;
                    const char *result_class =
                        user->type ? xr_type_get_class_name(user->type) : NULL;
                    const XiFunc *result_ctor =
                        cd && result_class ? cg_lookup_class_ctor_global(ctx, result_class, NULL)
                                           : NULL;
                    if (result_ctor && !result_ctor->is_vararg &&
                        result_ctor->nparams == user->nargs &&
                        !cg_func_needs_aot_coro_ctx(ctx, result_ctor))
                        continue;
                }
                if (a == 0 && (user->op == XI_CALL_METHOD || user->op == XI_CALL_METHOD_DIRECT) &&
                    user->aux) {
                    if (cg_time_module_helper_ctx(ctx, f, user))
                        continue;
                    const char *method = (const char *) user->aux;
                    CgStaticFunctionCall call = cg_resolve_module_member_call(ctx, f, user, method);
                    if (call.func && call.func->ncaptures == 0)
                        continue;
                    if (cg_aot_stdlib_receiver_call_is_direct(ctx, f, user))
                        continue;
                    const XiEnumData *ed = cg_enum_for_shared_value_in_func(ctx, f, v);
                    if (ed && cg_enum_member_index(ed, method) >= 0)
                        continue;
                    const XiFunc *target = cg_class_native_resolve_method_call(ctx, f, user, NULL);
                    uint16_t call_argc = (uint16_t) (user->nargs - 1);
                    if (target && !target->is_vararg && target->nparams == call_argc &&
                        !cg_func_needs_aot_coro_ctx(ctx, target))
                        continue;
                    const XiClassData *cd = cg_class_native_class_value_data(ctx, f, v);
                    if (cd && cg_class_descriptor_static_method_call_is_elidable(ctx, user, cd))
                        continue;
                }
                return false;
            }
        }
    }
    return true;
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

    bool int_widen = cg_int_widen_inner_value_plan(ctx, v, NULL, NULL);
    if (!int_widen) {
        switch ((XiOp) v->op) {
            case XI_CONST:
            case XI_COPY:
            case XI_SOURCE_MOVE:
            case XI_OWNER_FORWARD:
                break;
            default:
                return false;
        }
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
                if (int_widen && cg_int_widen_use_consumes_inner(ctx, f, v, user, a))
                    continue;
                if (!cg_native_box_value_is_elided_in_aot(ctx, f, user))
                    return false;
                /* emit_value_as_rep_ctx() bypasses an elided BOX to its immediate
                 * native input. Constants are re-emitted as expressions, but a
                 * COPY/cast/widen input is still referenced by its C temporary
                 * and therefore must retain that declaration. */
                if (user->op == XI_BOX && user->nargs >= 1 && user->args[0] == v &&
                    v->op != XI_CONST)
                    return false;
            }
        }
    }
    return seen_use;
}

static uint32_t cg_coalesced_c_value_id(const XiFunc *f, const XiValue *v) {
    if (!v)
        return UINT32_MAX;
    if (f && f->phi_coalesce && v->id < f->phi_coalesce_count)
        return f->phi_coalesce[v->id];
    return v->id;
}

/* A native fixed-array local always needs its `_faN` lane storage, but its
 * tagged xr_array_ref wrapper is unnecessary when every release-mode use is
 * already emitted against those lanes.  Coalesced BOX/UNBOX/COPY boundaries
 * are skipped by emit_value_stmt; fixed-array index and address operations
 * resolve the same backing storage; and ownership bookkeeping for native-lane
 * arrays is a C no-op.  Any control, phi, aggregate store, slice, or other use fails this
 * proof closed because it can require the tagged wrapper itself. */
static bool cg_fixed_array_wrapper_has_no_release_use(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *v) {
    if (!ctx || !f || !v || (v->op != XI_FIXED_ARRAY_NEW && v->op != XI_FIXED_BYTES_CONST) ||
        cg_func_needs_aot_coro_ctx(ctx, f) || cg_value_has_cell(ctx, v))
        return false;

    uint32_t root_id = cg_coalesced_c_value_id(f, v);
    if (root_id != v->id)
        return false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (cg_coalesced_c_value_id(f, blk->control) == root_id)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (cg_coalesced_c_value_id(f, phi->value.args[a]) == root_id)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == v)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                const XiValue *arg = user->args[a];
                if (cg_coalesced_c_value_id(f, arg) != root_id)
                    continue;

                /* The alias statement itself is absent and every later use
                 * is inspected through the same representative id below. */
                if (user->op != XI_PHI && user->id < f->phi_coalesce_count &&
                    cg_coalesced_c_value_id(f, user) == root_id)
                    continue;

                CgFixedArrayLaneInfo fixed;
                if (a == 0 &&
                    (user->op == XI_INDEX_GET || user->op == XI_INDEX_SET ||
                     user->op == XI_LOCAL_ADDR) &&
                    cg_fixed_array_lane_info_from_value(arg, &fixed))
                    continue;
                if (a == 0 && (user->op == XI_RETAIN || user->op == XI_RELEASE) &&
                    cg_fixed_array_lane_info_from_value(arg, &fixed) && fixed.rep != XR_REP_TAGGED)
                    continue;
                return false;
            }
        }
    }
    return true;
}

/*
 * A source-level Slice snapshot after a phi remains distinct in Xi because a
 * later edge may overwrite the phi's C slot.  When that snapshot has no
 * release-mode consumer, however, its aggregate copy exists only to preserve
 * the debugger's source-variable value at this statement.  Keep the snapshot
 * under XRAY_AOT_DEBUG_LOCALS and omit it from release C.
 */
static bool cg_span_phi_snapshot_has_no_release_use(XiCgenCtx *ctx, const XiFunc *f,
                                                    const XiValue *target) {
    if (!ctx || !f || !target || target->nargs != 1 || !target->args[0] ||
        target->args[0]->op != XI_PHI || !cg_value_plan_is_span_aggregate(ctx, target) ||
        !cg_value_plan_is_span_aggregate(ctx, target->args[0]) ||
        cg_func_needs_aot_coro_ctx(ctx, f) || cg_value_has_cell(ctx, target))
        return false;
    if (target->op != XI_COPY && target->op != XI_SOURCE_MOVE && target->op != XI_OWNER_FORWARD &&
        target->op != XI_BOX && target->op != XI_UNBOX)
        return false;
    if (target->flags & (XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW |
                         XI_FLAG_MAY_SUSPEND | XI_FLAG_SIDE_EFFECT))
        return false;
    const XaotValuePlan *target_plan = cg_value_plan(ctx, target);
    const XaotValuePlan *source_plan = cg_value_plan(ctx, target->args[0]);
    if (!target_plan || !source_plan || !xaot_value_reps_equal(target_plan->rep, source_plan->rep))
        return false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == target)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != target)
                    continue;
                if (a == 0 && (user->op == XI_RETAIN || user->op == XI_RELEASE))
                    continue;
                /* ERR_CHECK args are ARC cleanup owners, not the may-throw
                 * producer.  Its cold-edge synthetic RELEASE is exactly a C
                 * no-op for aggregate/vector plans, matching
                 * xicgen_ownership_call(). */
                if (user->op == XI_ERR_CHECK && xi_err_check_has_arc_cleanups(user) &&
                    (cg_value_plan_is_aggregate(ctx, target) ||
                     cg_value_plan_is_vector(ctx, target)))
                    continue;
                return false;
            }
        }
    }
    return true;
}

/* Prelude enum namespaces are compile-time type tokens when every use is a
 * static member load. The member emitter materializes the immutable enum
 * singleton directly, so constructing a temporary runtime Map here would be
 * both semantically redundant and an unexpected allocation in expressions
 * such as `flag ? Endian.LE : Endian.BE`. */
static bool cg_static_prelude_enum_namespace_is_elided(const XiFunc *f, const XiValue *v) {
    if (!f || !v || v->op != XI_GET_BUILTIN)
        return false;
    const CgPreludeEnumData *enum_data = cg_prelude_enum_data((int) v->aux_int);
    if (!enum_data || cg_prelude_enum_has_payload_member(enum_data))
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
                const char *member = user->aux ? (const char *) user->aux : NULL;
                if (a != 0 || user->op != XI_LOAD_FIELD || !member ||
                    cg_prelude_enum_member_index(enum_data, member) < 0)
                    return false;
                seen_use = true;
            }
        }
    }
    return seen_use;
}

/* PanicInfo is a compile-time class token in hosted AOT.  Its dedicated
 * constructor emitter recognizes the receiver identity and materializes the
 * exception directly, so the token itself has no C representation.  Keep the
 * proof deliberately exact: every use must be arg0 of PanicInfo.constructor. */
static bool cg_panicinfo_constructor_token_is_elided(const XiFunc *f, const XiValue *v) {
    if (!f || !v || v->op != XI_GET_BUILTIN || v->aux_int != XR_GLOBAL_VAR_PANIC_INFO)
        return false;

    bool seen_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == v)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                if (phi->value.args[ai] == v)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == v)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != v)
                    continue;
                const char *method = user->aux ? (const char *) user->aux : NULL;
                if (ai != 0 || (user->op != XI_CALL_METHOD && user->op != XI_CALL_METHOD_DIRECT) ||
                    !method || strcmp(method, "constructor") != 0)
                    return false;
                seen_use = true;
            }
        }
    }
    return seen_use;
}

/* User enum namespaces are type-domain tokens as well.  The frontend never
 * exposes them as ordinary collection values; their legal Xi consumers are
 * static member loads, enum-case iteration, and typed metadata operations.
 * When every use stays in that closed set, do not materialize the historical
 * Map-backed namespace or its shared slot in hosted AOT. */
static const XiEnumData *cg_static_enum_namespace_data(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!ctx || !v)
        return NULL;
    if (v->op == XI_GET_SHARED) {
        const XiEnumData *ed = cg_enum_for_shared_value_in_func(ctx, f, v);
        return ed ? ed : cg_resolve_imported_enum_value(ctx, f, v);
    }
    if (v->op == XI_IMPORT_REF)
        return cg_resolve_imported_enum_value(ctx, f, v);
    if (v->op == XI_CONST && v->aux)
        return cg_enum_for_runtime_type(ctx, v->aux);
    return NULL;
}

static bool cg_static_enum_namespace_uses_are_elidable(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *value, int depth) {
    if (!ctx || !f || !value || depth > 8 || !cg_static_enum_namespace_data(ctx, f, value))
        return false;

    bool seen_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == value)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == value)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == value)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != value)
                    continue;
                seen_use = true;
                if (a == 0 && user->op == XI_LOAD_FIELD && user->aux)
                    continue;
                if (a == 0 && user->op == XI_INDEX_GET && user->aux_kind == XI_AUX_KIND_ENUM_CASE)
                    continue;
                if (a == 0 && user->op == XI_ENUM_META_GET)
                    continue;
                if (a == 0 && (user->op == XI_CALL_METHOD || user->op == XI_CALL_METHOD_DIRECT) &&
                    user->aux &&
                    cg_enum_member_index(cg_static_enum_namespace_data(ctx, f, value),
                                         (const char *) user->aux) >= 0)
                    continue;
                if (a == 0 && (user->op == XI_RETAIN || user->op == XI_RELEASE))
                    continue;
                if (a == 0 && (user->op == XI_COPY || xi_op_is_identity_forward(user->op)) &&
                    cg_static_enum_namespace_uses_are_elidable(ctx, f, user, depth + 1))
                    continue;
                if (a == 0 && user->op == XI_SET_SHARED && user->aux_int >= 0) {
                    if ((f->module && f->module->slot_enums && user->aux_int < f->module->nslots &&
                         f->module->slot_enums[user->aux_int]) ||
                        (user->nargs >= 1 && cg_static_enum_namespace_data(ctx, f, user->args[0])))
                        continue;
                }
                return false;
            }
        }
    }
    return seen_use;
}

static bool cg_static_enum_namespace_value_is_elided(XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *v) {
    if (!ctx || !f || !v)
        return false;
    /* Enum declarations themselves are canonical schema records.  Their Xi
     * CONST and enum-owned shared store cannot denote a user-observable value,
     * so they are always compile-time-only in AOT. */
    if (v->op == XI_CONST && v->aux && cg_enum_for_runtime_type(ctx, v->aux))
        return true;
    if (v->op == XI_SET_SHARED && v->nargs >= 1 && v->aux_int >= 0 && f->module &&
        f->module->slot_enums && v->aux_int < f->module->nslots &&
        f->module->slot_enums[v->aux_int])
        return true;
    if (v->op == XI_SET_SHARED && v->nargs >= 1 &&
        cg_static_enum_namespace_data(ctx, f, v->args[0]))
        return true;
    if ((v->op == XI_GET_SHARED || v->op == XI_IMPORT_REF || v->op == XI_COPY ||
         xi_op_is_identity_forward(v->op)) &&
        cg_static_enum_namespace_uses_are_elidable(ctx, f, v, 0))
        return true;
    if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1 &&
        cg_static_enum_namespace_data(ctx, f, v->args[0]))
        return true;
    return false;
}

/* Calls whose Xi result has no consumer still have to execute for their side
 * effects and pending-error state, but release C does not need a dead result
 * local.  This is equally valid on the CFG pre-declaration path: an expression
 * statement introduces no local that a C++ jump can bypass.  Source-bound and
 * cell-backed values retain their local so XRAY_AOT_DEBUG_LOCALS remains exact. */
static bool cg_unused_call_result_emits_statement(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *v) {
    if (!ctx || !f || !v || cg_value_has_actual_ir_use(f, v) || cg_is_void_like(v) ||
        cg_value_has_cell(ctx, v) || cg_debug_value_has_source_storage(ctx, f, v))
        return false;
    return v->op == XI_CALL || v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT ||
           v->op == XI_CALL_BUILTIN;
}

/* All C vector backends, including the scalar aggregate fallback, fuse
 *
 *     widenMulEven(x, x.swapAdjacent())
 *
 * by selecting both 32-bit halves directly from x.  The Xi shuffle remains a
 * semantic operand of the widening multiply, but it has no release-C use.
 * Elide it only when every final-graph edge is that exact fused RHS.  Any
 * control, phi, identity, debug, cell, or additional value use retains the
 * materialized shuffle. */
static bool cg_vec_shuffle_use_tree_is_fused(XiCgenCtx *ctx, const XiFunc *f, const XiValue *target,
                                             uint8_t depth, bool *saw_fused) {
    if (!ctx || !f || !target || !saw_fused || depth >= 16)
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
                if (ai == 0 && (user->op == XI_RETAIN || user->op == XI_RELEASE) &&
                    (cg_value_plan_is_aggregate(ctx, cg_unwrap_identity_value(target)) ||
                     cg_value_plan_is_vector(ctx, cg_unwrap_identity_value(target))))
                    continue;
                if (ai == 0 && (user->op == XI_COPY || xi_op_is_identity_forward(user->op)) &&
                    f->phi_coalesce && user->id < f->phi_coalesce_count &&
                    f->phi_coalesce[user->id] != user->id) {
                    if (!cg_vec_shuffle_use_tree_is_fused(ctx, f, user, (uint8_t) (depth + 1),
                                                          saw_fused))
                        return false;
                    continue;
                }
                if (ai != 1 || !xicgen_vec_widen_mul_is_adjacent_pair(user))
                    return false;
                *saw_fused = true;
            }
        }
    }
    return true;
}

static bool cg_vec_shuffle_only_feeds_fused_widen_mul(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *v) {
    if (!ctx || !f || !v || v->op != XI_VEC_SHUFFLE || cg_value_has_cell(ctx, v) ||
        cg_debug_value_has_source_storage(ctx, f, v))
        return false;
    bool saw_fused = false;
    return cg_vec_shuffle_use_tree_is_fused(ctx, f, v, 0, &saw_fused) && saw_fused;
}

static bool cg_u64_mul_wide_operand_equivalent(const XiValue *a, const XiValue *b) {
    a = cg_unwrap_identity_value(a);
    b = cg_unwrap_identity_value(b);
    if (a == b)
        return true;
    return a && b && a->op == XI_CONST && b->op == XI_CONST && a->type && b->type &&
           a->type->kind == XR_KIND_INT && b->type->kind == XR_KIND_INT && a->aux_int == b->aux_int;
}

static bool cg_u64_mul_wide_operand_is_constant(const XiValue *v) {
    v = cg_unwrap_identity_value(v);
    return v && v->op == XI_CONST && v->type && v->type->kind == XR_KIND_INT &&
           cg_rep(v) == XR_REP_I64;
}

static bool cg_u64_mul_wide_value_is_eligible(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!ctx || !f || !v || (v->op != XI_MUL && v->op != XI_BIT_MUL_HIGH) || v->nargs != 2 ||
        !v->args[0] || !v->args[1] || !v->type || v->type->kind != XR_KIND_INT ||
        v->type->is_nullable || v->type->scalar_rep != XR_NATIVE_U64 || cg_rep(v) != XR_REP_I64 ||
        cg_rep(v->args[0]) != XR_REP_I64 || cg_rep(v->args[1]) != XR_REP_I64 ||
        cg_value_plan_storage_rep(ctx, v) != XR_REP_I64 || cg_value_has_cell(ctx, v) ||
        strcmp(local_ctype_str_ctx(ctx, f, v), "uint64_t") != 0)
        return false;
    if (f->phi_coalesce && v->id < f->phi_coalesce_count && f->phi_coalesce[v->id] != v->id)
        return false;
    return true;
}

/* Pair exactly one low-half XI_MUL with exactly one XI_BIT_MUL_HIGH in the
 * same basic block when one operand is constant.  Windows x86-64 evidence
 * shows that this narrow profitability shape removes a redundant multiply
 * without the register-pressure regressions seen for general operand pairs.
 * Ambiguous repeated products, different operands, non-u64 values, cells, and
 * cross-CFG pairs retain their ordinary independent lowering. */
static bool cg_u64_mul_wide_pair(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                 const XiValue **out_partner, bool *out_is_first) {
    if (!out_partner || !out_is_first || !cg_u64_mul_wide_value_is_eligible(ctx, f, v) ||
        !v->block ||
        (!cg_u64_mul_wide_operand_is_constant(v->args[0]) &&
         !cg_u64_mul_wide_operand_is_constant(v->args[1])))
        return false;

    const XiValue *partner = NULL;
    uint32_t equivalent_count = 0;
    uint32_t partner_count = 0;
    uint32_t value_index = UINT32_MAX;
    uint32_t partner_index = UINT32_MAX;
    uint16_t counterpart_op = v->op == XI_MUL ? XI_BIT_MUL_HIGH : XI_MUL;
    for (uint32_t i = 0; i < v->block->nvalues; i++) {
        const XiValue *candidate = v->block->values[i];
        if (!candidate || !cg_u64_mul_wide_value_is_eligible(ctx, f, candidate))
            continue;
        bool same_order = cg_u64_mul_wide_operand_equivalent(v->args[0], candidate->args[0]) &&
                          cg_u64_mul_wide_operand_equivalent(v->args[1], candidate->args[1]);
        bool swapped_order = cg_u64_mul_wide_operand_equivalent(v->args[0], candidate->args[1]) &&
                             cg_u64_mul_wide_operand_equivalent(v->args[1], candidate->args[0]);
        if (!same_order && !swapped_order)
            continue;
        equivalent_count++;
        if (candidate == v) {
            value_index = i;
            continue;
        }
        if (candidate->op != counterpart_op)
            continue;
        partner = candidate;
        partner_index = i;
        partner_count++;
    }
    if (equivalent_count != 2 || partner_count != 1 || value_index == UINT32_MAX ||
        partner_index == UINT32_MAX)
        return false;
    uint32_t low_index = v->op == XI_MUL ? value_index : partner_index;
    uint32_t high_index = v->op == XI_BIT_MUL_HIGH ? value_index : partner_index;
    if (low_index >= high_index)
        return false;
    *out_partner = partner;
    *out_is_first = v->op == XI_MUL;
    return true;
}

static void emit_u64_mul_wide_pair_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const XiValue *partner) {
    const XiValue *low = v->op == XI_MUL ? v : partner;
    const XiValue *high = v->op == XI_BIT_MUL_HIGH ? v : partner;
    if (!ctx->pre_decl_all) {
        fprintf(out, "    uint64_t ");
        emit_vref(out, high);
        fprintf(out, ";\n    uint64_t ");
        emit_vref(out, low);
        fprintf(out, " = ");
    } else {
        fprintf(out, "    ");
        emit_vref(out, low);
        fprintf(out, " = ");
    }
    fprintf(out, "xr_u64_mul_wide((uint64_t) ");
    emit_value_as_rep_ctx(ctx, out, low->args[0], XR_REP_I64);
    fprintf(out, ", (uint64_t) ");
    emit_value_as_rep_ctx(ctx, out, low->args[1], XR_REP_I64);
    fprintf(out, ", &");
    emit_vref(out, high);
    fprintf(out, ");\n");
    emit_value_generated_line_reset(ctx, out, v);
    emit_debug_source_var_sync(ctx, out, f, v);
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

    if (cg_lowbits_binop_elided_into_unsigned_narrow(f, v))
        return;
    if (xicgen_slice_value_only_used_by_stack_slice_direct_call(ctx, f, v))
        return;
    if (cg_await_all_inline_literal_value_is_elided(f, v))
        return;
    if (cg_await_all_scalar_result_value_is_elided(f, v))
        return;
    if (cg_native_box_value_is_elided_in_aot(ctx, f, v))
        return;
    if (cg_vec_shuffle_only_feeds_fused_widen_mul(ctx, f, v))
        return;
    if (cg_const_only_emits_immediate(ctx, f, v)) {
        emit_debug_source_var_sync(ctx, out, f, v);
        return;
    }
    if (cg_shared_load_has_no_emitted_c_use(ctx, f, v)) {
        if (cg_debug_value_has_source_storage(ctx, f, v)) {
            fprintf(out, "#if defined(XRAY_AOT_DEBUG_LOCALS)\n");
            fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, v));
            emit_vref(out, v);
            fprintf(out, " = ");
            emit_value_rhs(ctx, out, f, v, prefix);
            fprintf(out, ";\n");
            emit_value_generated_line_reset(ctx, out, v);
            emit_debug_source_var_sync(ctx, out, f, v);
            fprintf(out, "#endif\n");
        }
        return;
    }
    if (cg_pure_value_only_feeds_aot_elided_values(ctx, f, v))
        return;
    if (v->op != XI_PHI && f->phi_coalesce && v->id < f->phi_coalesce_count &&
        f->phi_coalesce[v->id] != v->id) {
        emit_debug_source_var_sync(ctx, out, f, v);
        return;
    }
    if (cg_static_prelude_enum_namespace_is_elided(f, v))
        return;
    if (cg_panicinfo_constructor_token_is_elided(f, v))
        return;
    if (cg_static_enum_namespace_value_is_elided(ctx, f, v))
        return;
    if (cg_fixed_array_value_clone_place_store(f, v))
        return;

    const XiValue *wide_mul_partner = NULL;
    bool wide_mul_is_first = false;
    if (cg_u64_mul_wide_pair(ctx, f, v, &wide_mul_partner, &wide_mul_is_first)) {
        if (wide_mul_is_first) {
            emit_u64_mul_wide_pair_stmt(ctx, out, f, v, wide_mul_partner);
        } else {
            emit_value_generated_line_reset(ctx, out, v);
            emit_debug_source_var_sync(ctx, out, f, v);
        }
        return;
    }

    /* Inlined struct: emit local anonymous C struct with native fields. */
    if (v->op == XI_AGG_NEW && cg_struct_inline_local_storage(ctx, f, v)) {
        XrAggregateLayout *sl = (XrAggregateLayout *) v->aux;
        XR_DCHECK(sl != NULL, "inlined XI_AGG_NEW: missing layout");
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

    if (v->op == XI_FIXED_ARRAY_NEW || v->op == XI_FIXED_BYTES_CONST) {
        uint8_t native = 0;
        uint32_t count = 0;
        if (!xicgen_fixed_array_new_info(v, &native, &count)) {
            ctx->error = true;
            fprintf(out, "    XrValue ");
            emit_vref(out, v);
            fprintf(out, " = XR_NULL_VAL;\n");
            return;
        }
        if (!ctx->pre_decl_all) {
            fprintf(out, "    %s _fa%u[%u];\n", cg_struct_native_c_type(native), v->id,
                    (unsigned) (count > 0 ? count : 1));
        }
        const XaotFixedBytesPlan *fixed_bytes_plan = NULL;
        const XaotFixedBytesBlob *fixed_bytes_blob = NULL;
        if (v->op == XI_FIXED_BYTES_CONST) {
            fixed_bytes_plan = xaot_bundle_find_fixed_bytes_plan(cg_ctx_aot_bundle(ctx), v);
            fixed_bytes_blob = fixed_bytes_plan
                                   ? xaot_bundle_find_fixed_bytes_blob(cg_ctx_aot_bundle(ctx),
                                                                       fixed_bytes_plan->blob_id)
                                   : NULL;
            if (!fixed_bytes_plan || !fixed_bytes_blob ||
                fixed_bytes_plan->action != XAOT_FIXED_BYTES_VALUE_COPY ||
                fixed_bytes_plan->length != count || fixed_bytes_blob->length != count) {
                fprintf(stderr, "[xi_cgen] ERROR: fixed byte value has no verified copy plan\n");
                ctx->error = true;
                return;
            }
        }
        if (v->op == XI_FIXED_BYTES_CONST && count > 0) {
            fprintf(out, "    memcpy(_fa%u, _xbytes_%u, %u);\n", v->id, fixed_bytes_plan->blob_id,
                    (unsigned) count);
        } else {
            fprintf(out, "    memset(_fa%u, 0, sizeof(_fa%u));\n", v->id, v->id);
        }
        bool debug_only_wrapper = cg_fixed_array_wrapper_has_no_release_use(ctx, f, v);
        if (debug_only_wrapper)
            fprintf(out, "#if defined(XRAY_AOT_DEBUG_LOCALS)\n");
        if (ctx->pre_decl_all) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = ");
        } else {
            fprintf(out, "    XrValue ");
            emit_vref(out, v);
            fprintf(out, " = ");
        }
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
        if (debug_only_wrapper)
            fprintf(out, "#endif\n");
        emit_value_generated_line_reset(ctx, out, v);
        emit_debug_source_var_sync(ctx, out, f, v);
        return;
    }

    uint8_t fixed_clone_native = 0;
    uint32_t fixed_clone_count = 0;
    if (xicgen_fixed_array_stack_copy_info(v, &fixed_clone_native, &fixed_clone_count)) {
        CgFixedArrayLaneInfo source_info;
        if (!cg_fixed_array_lane_info_from_value(v->args[0], &source_info)) {
            fprintf(stderr, "[xi_cgen] ERROR: fixed-array clone v%u has no lane plan\n",
                    (unsigned) v->id);
            ctx->error = true;
            return;
        }
        if (!ctx->pre_decl_all)
            fprintf(out, "    %s _fa%u[%u];\n", cg_struct_native_c_type(fixed_clone_native), v->id,
                    (unsigned) (fixed_clone_count > 0 ? fixed_clone_count : 1));
        fprintf(out, "    memmove(_fa%u, ", v->id);
        emit_fixed_array_lane_ptr_expr(ctx, out, v->args[0], &source_info);
        fprintf(out, ", sizeof(%s) * %u);\n", cg_struct_native_c_type(fixed_clone_native),
                (unsigned) fixed_clone_count);
        if (ctx->pre_decl_all) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = ");
        } else {
            fprintf(out, "    XrValue ");
            emit_vref(out, v);
            fprintf(out, " = ");
        }
        fprintf(out, "xr_array_ref(_fa%u, %u, %u);\n", v->id, (unsigned) fixed_clone_native,
                (unsigned) fixed_clone_count);
        emit_value_generated_line_reset(ctx, out, v);
        return;
    }

    if (cg_value_is_elided_nested_struct_ref(f, v) || cg_value_is_elided_fixed_array_ref(f, v) ||
        cg_value_is_elided_static_struct_nested_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_struct_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_struct_nested_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_array_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_matrix_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_matrix_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_cube_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_cube_outer_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_cube_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_nested_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_nested_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_tuple_array_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_tuple_array_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_tuple_array_tuple_ref(ctx, f, v) ||
        cg_value_is_elided_static_tuple_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_struct_const_ref(ctx, f, v))
        return;
    if ((v->op == XI_COPY || xi_op_is_identity_forward(v->op)) &&
        (cg_value_traces_to_inlined_struct(f, v) ||
         cg_value_traces_to_static_struct_whole_store(ctx, f, v) ||
         cg_value_is_elided_heap_struct_alias(ctx, f, v)))
        return;

    if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1 &&
        (cg_value_traces_to_inlined_struct(f, v->args[0]) ||
         cg_value_traces_to_static_struct_whole_store(ctx, f, v->args[0]) ||
         cg_value_is_elided_heap_struct_alias(ctx, f, v) ||
         cg_value_is_elided_nested_struct_ref(f, v->args[0]) ||
         cg_value_is_elided_fixed_array_ref(f, v->args[0]) ||
         cg_value_is_elided_static_struct_nested_field_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_struct_fixed_array_field_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_struct_nested_fixed_array_field_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_array_const_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_matrix_const_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_matrix_index_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_cube_const_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_cube_outer_index_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_cube_index_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_struct_array_const_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_struct_array_index_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_struct_array_fixed_array_field_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_struct_array_nested_fixed_array_field_ref(ctx, f,
                                                                                   v->args[0]) ||
         cg_value_is_elided_static_fixed_struct_array_nested_field_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_tuple_array_const_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_tuple_array_index_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_fixed_tuple_array_tuple_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_tuple_const_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_static_struct_const_ref(ctx, f, v->args[0]) ||
         cg_value_is_elided_layout_struct_type_load(f, v) ||
         cg_value_is_borrowed_array_slot_alias(ctx, f, v->args[0]) ||
         xicgen_slice_value_only_used_by_stack_slice_direct_call(ctx, f, v->args[0])))
        return;
    if (cg_ownership_op_is_noop(ctx && ctx->freestanding_profile, v) ||
        cg_shared_static_function_ownership_is_noop(ctx, f, v))
        return;
    if (cg_shared_static_function_value_is_elided(ctx, f, v) ||
        cg_class_descriptor_value_is_elided(ctx, f, v))
        return;
    if (xicgen_box_only_feeds_native_int_print(ctx, f, v))
        return;

    if (cg_class_native_value_stmt_is_elided(ctx, f, v))
        return;

    if (v->op == XI_CHAN_TRY_RECV) {
        if (v->nargs < 1) {
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_TRY_RECV missing channel\n");
            ctx->error = true;
            return;
        }
        fprintf(out, "    XrValue _chan_try_%u = xr_aot_chan_try_recv_sync(", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ");\n");
        if (ctx->pre_decl_all) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = ");
        } else {
            fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, v));
            emit_vref(out, v);
            fprintf(out, " = ");
        }
        XrRep rep = cg_value_decl_storage_rep(ctx, f, v);
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, rep);
        fprintf(out, "xr_aot_bridge_value_to_xrt(xr_aot_recv_payload(_chan_try_%u))", v->id);
        emit_conversion_suffix(out, suffix);
        fprintf(out, ";\n");
        emit_value_generated_line_reset(ctx, out, v);
        return;
    }

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
    if (v && v->uses == 0 &&
        cg_array_call_is_direct_byte_array_mutator_trusted_nothrow(ctx, f, v)) {
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

    if (cg_unused_call_result_emits_statement(ctx, f, v)) {
        fprintf(out, "    (void)(");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
        return;
    }
    if (cg_import_ref_has_no_emitted_c_use(ctx, f, v))
        return;

    bool release_only_value = cg_struct_place_load_only_feeds_direct_fields(ctx, f, v) ||
                              cg_struct_ptr_load_only_feeds_raw_deref_address(ctx, f, v) ||
                              cg_struct_scalar_field_load_has_no_release_value_use(ctx, f, v) ||
                              cg_span_phi_snapshot_has_no_release_use(ctx, f, v);
    bool debug_only_release_value =
        release_only_value && cg_debug_value_has_source_storage(ctx, f, v);
    if (release_only_value && !debug_only_release_value)
        return;
    bool debug_only_value = debug_only_release_value;
    if (debug_only_value)
        fprintf(out, "#if defined(XRAY_AOT_DEBUG_LOCALS)\n");

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

    if (ctx->pre_decl_all && !debug_only_value) {
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
    if (debug_only_value)
        fprintf(out, "#endif\n");
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
    if (cg_value_plan_is_vector(ctx, &phi->value)) {
        const XaotValuePlan *phi_plan = cg_value_plan(ctx, &phi->value);
        const XaotValuePlan *incoming_plan = cg_value_plan(ctx, incoming);
        if (phi_plan && incoming_plan && xaot_value_reps_equal(phi_plan->rep, incoming_plan->rep)) {
            emit_vref(out, incoming);
            return;
        }
        if (ctx) {
            fprintf(stderr, "[xi_cgen] ERROR: vector PHI v%u incoming v%u has mismatched plan\n",
                    (unsigned) phi->value.id, incoming ? (unsigned) incoming->id : 0);
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

/* ARC promotes a phi input with RETAINs at the predecessor tail when the
 * original owner remains live. If there are fewer promotions than consuming
 * phi inputs, one input transfers the frame-held owner and that source slot
 * must be cleared after the parallel copy. Otherwise coroutine teardown would
 * release the moved reference a second time. */
static bool cg_phi_edge_moves_frame_owner(XiCgenCtx *ctx, const XiFunc *f, const XiBlock *target,
                                          uint16_t pred_idx, const XiValue *incoming) {
    if (!ctx || !f || !target || !incoming || pred_idx >= target->npreds ||
        !cg_func_needs_aot_coro_ctx(ctx, f) || !cg_coro_value_needs_frame(ctx, f, incoming) ||
        !cg_coro_value_needs_frame_arc_release(ctx, f, incoming))
        return false;

    const XiBlock *pred = target->preds[pred_idx];
    if (!pred)
        return false;

    uint32_t consumes = 0;
    for (const XiPhi *phi = target->phis; phi; phi = phi->next) {
        if (pred_idx < phi->value.nargs && phi->value.args[pred_idx] == incoming)
            consumes++;
    }
    if (consumes == 0)
        return false;

    uint32_t retains = 0;
    uint32_t vi = pred->nvalues;
    while (vi > 0 && pred->values[vi - 1] && pred->values[vi - 1]->op == XI_RELEASE)
        vi--;
    while (vi > 0 && pred->values[vi - 1] && pred->values[vi - 1]->op == XI_RETAIN) {
        const XiValue *retain = pred->values[--vi];
        if (retain->nargs >= 1 && retain->args[0] == incoming)
            retains++;
    }
    return consumes > retains;
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
        fprintf(out, "    ");
        if (!ctx->pre_decl_all)
            fprintf(out, "%s ", local_ctype_str_ctx(ctx, f, &phi->value));
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

    /* Parallel copies have captured every incoming value, so transferred
     * coroutine-frame owners can now be invalidated without clobbering another
     * phi assignment on the same edge. */
    for (const XiPhi *phi = target->phis; phi; phi = phi->next) {
        if (!cg_phi_copy_should_emit(ctx, f, phi, pred_idx))
            continue;
        const XiValue *incoming = phi->value.args[pred_idx];
        if (!cg_phi_edge_moves_frame_owner(ctx, f, target, pred_idx, incoming))
            continue;
        fprintf(out, "    ");
        emit_vref(out, incoming);
        fprintf(out, " = %s;\n", cg_rep(incoming) == XR_REP_PTR ? "NULL" : "XR_NULL_VAL");
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

static uint32_t cg_func_value_count(const XiFunc *f) {
    if (!f)
        return 0;
    uint32_t total = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        total += blk->nvalues;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next)
            total++;
    }
    return total;
}

static bool cg_func_has_backedge(const XiFunc *f) {
    if (!f)
        return false;
    xi_ensure_rpo((XiFunc *) f);
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (cg_block_has_backedge(f->blocks[bi]))
            return true;
    }
    return false;
}

static bool cg_func_contains_vector_op(const XiFunc *f) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            if (value && xi_generated_op_class(value->op) == XI_GEN_CLASS_VECTOR)
                return true;
        }
    }
    return false;
}

static bool cg_value_materializes_stack_array(const XiValue *value) {
    if (!value)
        return false;
    if ((value->op == XI_STACK_ALLOC && value->aux_int == XI_ARRAY_NEW) ||
        value->op == XI_ARRAY_NEW || value->op == XI_FIXED_ARRAY_NEW)
        return true;
    /* Module/global arrays and borrowed field/parameter places reference
     * already-materialized storage. Ownership bookkeeping also preserves the
     * same storage; none of these operations introduces a caller-visible
     * stack aggregate. */
    if (value->op == XI_PARAM || value->op == XI_PLACE_LOAD || value->op == XI_AGG_GET ||
        value->op == XI_GET_SHARED || value->op == XI_GET_GLOBAL || value->op == XI_RETAIN ||
        value->op == XI_RELEASE || value->op == XI_OWNER_FORWARD)
        return false;
    return value->type && value->type->kind == XR_KIND_FIXED_ARRAY;
}

static bool cg_func_contains_stack_array(const XiFunc *f) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            if (cg_value_materializes_stack_array(blk->values[vi]))
                return true;
        }
    }
    return false;
}

static uint8_t cg_fixed_width_native_size(uint8_t native) {
    switch (native) {
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_F64:
            return 8;
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_F32:
            return 4;
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return 2;
        case XR_NATIVE_BOOL:
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
            return 1;
        default:
            return 0;
    }
}

static bool cg_func_stack_arrays_force_inline_safe(const XiFunc *f) {
    enum {
        CG_FORCE_INLINE_STACK_ARRAY_BYTE_LIMIT = 64
    };
    if (!f)
        return false;
    uint32_t total_bytes = 0;
    bool found = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *value = blk->values[vi];
            if (!value)
                continue;
            if (!cg_value_materializes_stack_array(value))
                continue;
            /* Only a directly materialized, fixed-width scalar array is small
             * enough to expose to a caller. Dynamic/tagged/nested arrays keep
             * the hard no-inline boundary, as do fixed-array temporaries whose
             * storage origin is not explicit at this Xi value. */
            if (value->op != XI_FIXED_ARRAY_NEW)
                return false;
            uint8_t native = 0;
            uint32_t count = 0;
            if (!xicgen_fixed_array_new_info(value, &native, &count))
                return false;
            uint8_t lane_bytes = cg_fixed_width_native_size(native);
            if (!lane_bytes || count > CG_FORCE_INLINE_STACK_ARRAY_BYTE_LIMIT / lane_bytes)
                return false;
            uint32_t bytes = count * lane_bytes;
            if (bytes > CG_FORCE_INLINE_STACK_ARRAY_BYTE_LIMIT - total_bytes)
                return false;
            total_bytes += bytes;
            found = true;
        }
    }
    return found;
}

static uint32_t cg_func_inlineable_call_block_count_up_to(XiCgenCtx *ctx, const XiFunc *f,
                                                          uint32_t limit) {
    if (!f || limit == 0)
        return 0;
    uint32_t call_blocks = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (!value || (value->op != XI_CALL && value->op != XI_TAIL_CALL))
                continue;
            const XiFunc *target =
                value->nargs > 0 && value->args
                    ? cg_resolve_static_function_call(ctx, f, value->args[0]).func
                    : NULL;
            /* An explicit native boundary cannot be flattened transitively,
             * so it does not contribute to the fanout hazard. Unresolved and
             * ordinary direct targets remain conservatively counted. */
            if (!target || target->inline_policy != XI_INLINE_PRESERVE_CALL) {
                if (++call_blocks >= limit)
                    return call_blocks;
                break;
            }
        }
    }
    return call_blocks;
}

static bool cg_func_has_branching_call_fanout(XiCgenCtx *ctx, const XiFunc *f) {
    enum {
        CG_NATIVE_INLINER_FANOUT_VALUE_MIN = 20,
        CG_NATIVE_INLINER_FANOUT_VALUE_MAX = 48
    };
    if (!f || f->nblocks < 4 || !f->return_type || f->return_type->kind == XR_KIND_UNIT)
        return false;
    uint32_t value_count = cg_func_value_count(f);
    if (value_count < CG_NATIVE_INLINER_FANOUT_VALUE_MIN ||
        value_count > CG_NATIVE_INLINER_FANOUT_VALUE_MAX)
        return false;
    return cg_func_inlineable_call_block_count_up_to(ctx, f, 3) >= 3;
}

static bool cg_func_should_noinline(const XiFunc *f) {
    return f && f->inline_policy == XI_INLINE_PRESERVE_CALL;
}

static bool cg_func_should_force_inline(XiCgenCtx *ctx, const XiFunc *f) {
    enum {
        CG_FORCE_INLINE_VALUE_LIMIT = 48,
        CG_FORCE_INLINE_PROVEN_NOALLOC_VALUE_LIMIT = 192,
        CG_FORCE_INLINE_VECTOR_LEAF_VALUE_LIMIT = 256
    };
    if (!f)
        return false;
    if (cg_func_should_noinline(f))
        return false;
    if (f->inline_policy == XI_INLINE_PREFER)
        return true;
    uint32_t value_count = cg_func_value_count(f);
    bool proven_noalloc =
        f->allocation_effect_complete && f->allocation_state == XA_ALLOC_PROVEN_NONE;
    bool vector_kernel = cg_func_contains_vector_op(f);
    bool has_stack_array = cg_func_contains_stack_array(f);
    bool small_fixed_stack = has_stack_array && cg_func_stack_arrays_force_inline_safe(f);
    /* A stack array is cheap inside its owner but inlining the owner into a
     * dispatcher can hoist its frame/canary prologue onto unrelated hot
     * branches. Keep the hard boundary except for one cache line of explicit,
     * fixed-width scalar storage that a native optimizer can scalar-replace. */
    if (has_stack_array && !small_fixed_stack)
        return false;
    /* A small dispatcher can hide a large transitive body behind each branch.
     * always_inline then flattens every target into an enclosing loop, where a
     * native optimizer may create and update induction variables for branches
     * that are not taken. Leave three-way call fanout to the native inliner;
     * ordinary leaf wrappers and one/two-way fast paths retain the explicit
     * force-inline policy below. */
    if (cg_func_has_branching_call_fanout(ctx, f))
        return false;
    /* Keep separate-compilation helpers inlineable when they are tiny, but do
     * not force arbitrary loops into large callers/coroutine resume functions.
     * A bounded-size, proven-noalloc leaf loop is the narrow exception:
     * inlining it once exposes caller range/alias facts without source
     * duplication or forced unrolling.  Vector kernels receive the wider
     * budget because one lane operation expands to several Xi values. */
    if (cg_func_has_backedge(f)) {
        if (!proven_noalloc)
            return false;
        uint32_t loop_limit = (vector_kernel || small_fixed_stack)
                                  ? CG_FORCE_INLINE_VECTOR_LEAF_VALUE_LIMIT
                                  : CG_FORCE_INLINE_PROVEN_NOALLOC_VALUE_LIMIT;
        return value_count <= loop_limit;
    }
    if (value_count <= CG_FORCE_INLINE_VALUE_LIMIT)
        return true;

    /* A leaf proven allocation-free by the whole-program effect summary has no
     * hidden allocation slow path and is a substantially safer inlining
     * candidate than an arbitrary helper. An external no-allocation contract is
     * useful as a verifier promise, but must not be required for optimization:
     * inferred proof carries the same semantic fact.
     * Native-vector Xi ops additionally represent whole-lane instructions,
     * while their checked-span, error-edge, copy, and release bookkeeping can
     * expand one straight-line lane operation to several IR values.  Keep a
     * bounded 256-value budget so a complete 64-byte SIMD stripe remains one
     * compiler-inlineable unit; unknown loops were rejected above, and neither
     * decision relies on source names. */
    if (!proven_noalloc)
        return false;
    uint32_t limit = vector_kernel ? CG_FORCE_INLINE_VECTOR_LEAF_VALUE_LIMIT
                                   : CG_FORCE_INLINE_PROVEN_NOALLOC_VALUE_LIMIT;
    return value_count <= limit;
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

/* A CFn indirect call in tail position (`return f(...)`) is emitted as a musttail
 * return: a constant-stack tail jump with native ABI — the wasm3 threaded-operation
 * topology. Only the err_check on the tail call's OWN result is elided (the callee
 * propagates its error via the pending-error global, checked at the chain head);
 * err_checks for this op's own operations (array indexing, etc.) are kept, so trap
 * semantics are preserved. Requires no defer / exception / cell-var cleanup, since
 * nothing may run between the call and the return under musttail, and the caller's
 * scalar return rep must match the call so the call is returned without conversion. */
static bool cg_func_has_cell_releases(const XiCgenCtx *ctx) {
    if (!ctx || !ctx->cell_release_vars)
        return false;
    for (uint32_t i = 0; i < ctx->cell_var_count; i++) {
        if (ctx->cell_release_vars[i])
            return true;
    }
    return false;
}

static bool cg_block_owns_final_call(const XiBlock *blk, const XiValue *call) {
    if (!blk || !call || call->block != blk)
        return false;

    bool saw_call = false;
    bool saw_error_check = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *value = blk->values[i];
        if (!value)
            continue;
        if (value == call) {
            if (saw_call)
                return false;
            saw_call = true;
            continue;
        }
        if (!saw_call)
            continue;
        if (cg_unwrap_identity_value(value) == call)
            continue;
        /* ERR_CHECK deliberately has no producer operand. With every other
         * post-call instruction rejected, this operand-free check belongs to
         * the call by Xi's constructive nearest-producer contract. */
        if (value->op == XI_ERR_CHECK && !saw_error_check &&
            !xi_err_check_has_arc_cleanups(value)) {
            saw_error_check = true;
            continue;
        }
        return false;
    }
    return saw_call;
}

static bool cg_cfn_musttail_abi_compatible(XiCgenCtx *ctx, const XiFunc *caller,
                                           const XrType *callee_type, const XiValue *call) {
    if (!ctx || !caller || !callee_type || !call || !XR_TYPE_IS_C_FUNCTION(callee_type) ||
        caller->is_vararg || callee_type->function.is_variadic)
        return false;

    int callee_nparams = callee_type->function.param_count;
    if (callee_nparams < 0 || caller->nparams != (uint16_t) callee_nparams ||
        call->nargs != (uint16_t) (callee_nparams + 1))
        return false;

    const char *caller_return = cg_func_return_abi_c_type(ctx, caller);
    const char *callee_return = cg_cfn_value_c_type(callee_type->function.return_type, true);
    if (!caller_return || !callee_return || strcmp(caller_return, callee_return) != 0)
        return false;

    for (uint16_t i = 0; i < caller->nparams; i++) {
        const char *caller_param = cg_func_param_abi_c_type(ctx, caller, i);
        const XrType *callee_param = xr_type_function_param_type(callee_type, i);
        const char *callee_param_c = cg_cfn_value_c_type(callee_param, false);
        if (!caller_param || !callee_param_c || strcmp(caller_param, callee_param_c) != 0)
            return false;
    }
    return true;
}

static const XiValue *cg_block_musttail_call(XiCgenCtx *ctx, const XiFunc *f, const XiBlock *blk) {
    if (!ctx || !f || !blk || blk->kind != XI_BLOCK_RETURN || !blk->control)
        return NULL;
    const XiValue *call = blk->control;
    if ((call->op != XI_CALL && call->op != XI_TAIL_CALL) || call->nargs < 1 || !call->args[0])
        return NULL;
    const XiValue *callee = cg_unwrap_identity_value(call->args[0]);
    if (!callee || !callee->type || !XR_TYPE_IS_C_FUNCTION(callee->type))
        return NULL;
    /* Re-emitting a call defined in a predecessor duplicates its side effects.
     * Clang musttail additionally requires caller/callee native signatures to
     * match exactly, including the hidden closure parameter and every C type. */
    if (!cg_block_owns_final_call(blk, call) ||
        !cg_cfn_musttail_abi_compatible(ctx, f, callee->type, call))
        return NULL;
    /* Nothing may run between the tail call and the return under musttail. */
    if (cg_has_exception_handling(f) || cg_func_has_defer_stmt(f) || cg_func_has_cell_releases(ctx))
        return NULL;
    XrRep ret_rep = cg_func_return_abi_rep(ctx, f);
    if (ret_rep == XR_REP_VOID || cg_func_return_abi_is_aggregate(ctx, f))
        return NULL;
    /* The call must be returned directly (no rep conversion around it). */
    if (cg_value_plan_storage_rep(ctx, call) != ret_rep)
        return NULL;
    return call;
}

static void emit_block(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiBlock *blk,
                       const char *prefix) {
    XR_DCHECK(blk != NULL, "emit_block: NULL block");

    /* Label (skip for entry block b0 to reduce clutter) */
    if (blk->id != 0)
        fprintf(out, "L%u:;\n", blk->id);
    emit_typed_array_final_len_stores(ctx, out, f, blk);

    /* A tail-position CFn call is fused into a musttail return below; skip its
     * value statement and the err_check on its result. */
    const XiValue *mt_call = cg_block_musttail_call(ctx, f, blk);

    /* Instructions */
    bool after_mt_call = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (!v)
            continue;
        if (mt_call && v == mt_call) {
            after_mt_call = true;
            continue;
        }
        if (mt_call && after_mt_call &&
            (cg_unwrap_identity_value(v) == mt_call ||
             (v->op == XI_ERR_CHECK && !xi_err_check_has_arc_cleanups(v))))
            continue;
        xicgen_emit_stringbuilder_literal_append_reserve(ctx, out, blk, i);
        emit_value_stmt(ctx, out, f, v, prefix);
        if (cg_value_terminates_c_path(v))
            return;
    }

    /* Terminator */
    switch (blk->kind) {
        case XI_BLOCK_RETURN: {
            if (blk->control && blk->control->op == XI_ERR_RETURN)
                break;
            if (mt_call) {
                /* Tail-threaded CFn call: constant-stack tail jump (musttail). No
                 * cleanup runs between (guarded in cg_block_musttail_call), and the
                 * call is returned directly so reps match without conversion. */
                emit_block_terminator_source_line(ctx, out, blk);
                fprintf(out, "    __attribute__((musttail)) return ");
                emit_value_rhs(ctx, out, f, mt_call, prefix);
                fprintf(out, ";\n");
                emit_block_terminator_generated_line_reset(ctx, out, blk);
                break;
            }
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
                        XaotValueRep ret_value_rep = cg_func_return_abi_value_rep(ctx, f);
                        emit_adt_base_to_value_rep_prefix(out, ret_value_rep);
                        fprintf(out, "xrt_enum_aggregate_from_boxed(");
                        emit_value_as_rep_ctx(ctx, out, blk->control, XR_REP_TAGGED);
                        fprintf(out, ")");
                        emit_adt_base_to_value_rep_suffix(out, ret_value_rep);
                    }
                    fprintf(out, ";\n");
                } else {
                    fprintf(out, "    return ");
                    emit_return_value_as_rep_ctx(ctx, out, f, blk->control, ret_rep);
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
    if (cg_lowbits_binop_elided_into_unsigned_narrow(f, v))
        return true;
    if (cg_await_all_inline_literal_value_is_elided(f, v))
        return true;
    if (cg_await_all_scalar_result_value_is_elided(f, v))
        return true;
    if (cg_native_box_value_is_elided_in_aot(ctx, f, v))
        return true;
    if (cg_unused_call_result_emits_statement(ctx, f, v))
        return true;
    if (cg_vec_shuffle_only_feeds_fused_widen_mul(ctx, f, v))
        return true;
    if (cg_const_only_emits_immediate(ctx, f, v))
        return true;
    if (cg_shared_load_has_no_emitted_c_use(ctx, f, v))
        return true;
    if (cg_import_ref_has_no_emitted_c_use(ctx, f, v))
        return true;
    if (cg_pure_value_only_feeds_aot_elided_values(ctx, f, v))
        return true;
    if (v->op != XI_PHI && f->phi_coalesce && v->id < f->phi_coalesce_count &&
        f->phi_coalesce[v->id] != v->id)
        return true;
    if (cg_struct_place_load_only_feeds_direct_fields(ctx, f, v))
        return true;
    if (cg_struct_ptr_load_only_feeds_raw_deref_address(ctx, f, v))
        return true;
    if (cg_struct_scalar_field_load_has_no_release_value_use(ctx, f, v))
        return true;
    if (cg_span_phi_snapshot_has_no_release_use(ctx, f, v))
        return true;
    if (cg_static_prelude_enum_namespace_is_elided(f, v))
        return true;
    if (cg_panicinfo_constructor_token_is_elided(f, v))
        return true;
    if (cg_static_enum_namespace_value_is_elided(ctx, f, v))
        return true;
    if (cg_fixed_array_value_clone_place_store(f, v))
        return true;
    if (v->op == XI_AGG_NEW && cg_struct_inline_local_storage(ctx, f, v))
        return true;
    if ((v->op == XI_COPY || xi_op_is_identity_forward(v->op)) &&
        (cg_value_traces_to_inlined_struct(f, v) ||
         cg_value_traces_to_static_struct_whole_store(ctx, f, v) ||
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
    if (cg_value_is_elided_static_fixed_array_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_matrix_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_matrix_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_cube_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_cube_outer_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_cube_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_nested_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_struct_array_nested_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_struct_nested_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_struct_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_struct_nested_fixed_array_field_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_tuple_array_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_tuple_array_index_ref(ctx, f, v) ||
        cg_value_is_elided_static_fixed_tuple_array_tuple_ref(ctx, f, v) ||
        cg_value_is_elided_static_tuple_const_ref(ctx, f, v) ||
        cg_value_is_elided_static_struct_const_ref(ctx, f, v) ||
        (v->op == XI_GET_SHARED && cg_value_only_used_by_layout_struct_new(f, v)) ||
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
 * (not an inlined-struct / heap-alias phi).  Source var_id is not required:
 * loop-lowered induction phis can carry no var_id at all.  When source var_ids
 * are present, though, they form destructive-update domains; different source
 * variables must not be collapsed onto the same C slot even if their block-level
 * liveness appears disjoint. */
static bool cg_phi_coalesce_candidate(XiCgenCtx *ctx, const XiFunc *f, const XiPhi *phi) {
    const XiValue *v = &phi->value;
    if (cg_value_plan_storage_rep(ctx, v) == XR_REP_TAGGED)
        return false;
    if (cg_value_traces_to_inlined_struct(f, v) || cg_value_is_elided_heap_struct_alias(ctx, f, v))
        return false;
    return true;
}

static bool cg_phis_share_var_domain(const XiPhi *a, const XiPhi *b) {
    bool a_has_var = a && xi_var_id_is_valid(a->value.var_id);
    bool b_has_var = b && xi_var_id_is_valid(b->value.var_id);
    if (a_has_var != b_has_var)
        return false;
    if (a_has_var && a->value.var_id != b->value.var_id)
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

static bool cg_c_value_alias_is_address_taken(const XiFunc *f, const XiValue *alias) {
    if (!f || !alias)
        return true;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user->op != XI_LOCAL_ADDR || user->nargs < 1)
                continue;
            if (user->args[0] == alias) {
                /* A fixed-array place is emitted as the address of its native
                 * `_faN` lane storage, not as `&vN`.  Its tagged XrValue alias
                 * therefore remains an immutable representation boundary and
                 * can share the backing value's C local. */
                CgFixedArrayLaneInfo fixed;
                if (cg_fixed_array_lane_info_from_value(alias, &fixed))
                    continue;
                return true;
            }
        }
    }
    return false;
}

static bool cg_value_traces_to_explicit_vector(const XiValue *value) {
    const XiValue *cur = value;
    for (uint8_t depth = 0; cur && depth < 16; depth++) {
        switch ((XiOp) cur->op) {
            case XI_VEC_LOAD:
            case XI_VEC_SPLAT:
            case XI_VEC_REPLACE:
            case XI_VEC_ADD:
            case XI_VEC_SUB:
            case XI_VEC_MUL:
            case XI_VEC_BIT_AND:
            case XI_VEC_BIT_OR:
            case XI_VEC_BIT_XOR:
            case XI_VEC_BIT_NOT:
            case XI_VEC_SHL:
            case XI_VEC_SHR:
            case XI_VEC_REINTERPRET:
            case XI_VEC_SHUFFLE:
            case XI_VEC_WIDEN_MUL:
            case XI_VEC_UNZIP:
            case XI_VEC_WIDEN_MUL_HALF:
                return xi_vec_shape_is_explicit(cur->aux_int);
            default:
                break;
        }
        if (cur->nargs != 1 || !cur->args[0] ||
            (cur->op != XI_BOX && cur->op != XI_UNBOX && !xi_copy_is_value_clone(cur) &&
             !xi_copy_is_identity_alias(cur) && !xi_op_is_identity_forward(cur->op)))
            return false;
        cur = cur->args[0];
    }
    return false;
}

/*
 * A source-variable COPY/MOVE boundary must remain distinct in Xi because the
 * VM register allocator may destructively update variable domains.  BOX/UNBOX
 * are also semantic boundaries even when the AOT representation planner proves
 * that no physical conversion remains.  Slice and SIMD VALUE_CLONE boundaries
 * similarly denote independent language values, but their native C forms are
 * plain immutable value structs and copying them has no ownership side effect.
 * Native C SSA locals can therefore share the source local when:
 *   - both AOT plans have the exact same value representation and C type;
 *   - the source is not a mutable phi/cell;
 *   - no ref call takes the alias's address.
 *
 * Debug source variables still synchronize at the alias statement.  Only the
 * redundant C declaration/assignment disappears.
 */
static bool cg_rep_identical_alias_can_share_c_local(XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *alias, const XiValue *source) {
    if (!ctx || !f || !alias || !source || alias->nargs != 1 || alias->args[0] != source)
        return false;
    if (source->op == XI_PHI || cg_value_has_cell(ctx, alias) || cg_value_has_cell(ctx, source))
        return false;
    if (alias->flags & (XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW |
                        XI_FLAG_MAY_SUSPEND | XI_FLAG_SIDE_EFFECT))
        return false;
    const XaotValuePlan *alias_plan = cg_value_plan(ctx, alias);
    const XaotValuePlan *source_plan = cg_value_plan(ctx, source);
    if (!alias_plan || !source_plan || !xaot_value_reps_equal(alias_plan->rep, source_plan->rep))
        return false;
    bool forwarding_boundary = alias->op == XI_BOX || alias->op == XI_UNBOX ||
                               xi_copy_is_identity_alias(alias) ||
                               xi_op_is_identity_forward(alias->op);
    bool trivial_value_clone =
        xi_copy_is_value_clone(alias) &&
        (cg_value_plan_is_span_aggregate(ctx, alias) || cg_value_traces_to_explicit_vector(source));
    if (!forwarding_boundary && !trivial_value_clone)
        return false;
    const char *alias_ctype = local_ctype_str_ctx(ctx, f, alias);
    const char *source_ctype = local_ctype_str_ctx(ctx, f, source);
    if (!alias_ctype || !source_ctype || strcmp(alias_ctype, source_ctype) != 0)
        return false;
    return !cg_c_value_alias_is_address_taken(f, alias);
}

/* Build the per-function C-value coalescing map. Phis that share the exact
 * same declared C type and that provably never interfere are merged first;
 * representation-identical immutable SSA aliases are then mapped onto their
 * source local. Liveness-based non-interference protects phi mutation, while
 * cg_rep_identical_alias_can_share_c_local protects addressability and exact
 * representation. On allocation failure the map stays identity. */
static void cg_build_phi_coalesce(XiCgenCtx *ctx, XiFunc *f) {
    ctx->phi_repr_active = false;
    if (f) {
        f->phi_coalesce = NULL;
        f->phi_coalesce_count = 0;
    }
    if (!f || f->nblocks == 0)
        return;

    bool any_phi = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            any_phi = true;
        }
    }
    uint32_t need = f->next_value_id;
    if (need == 0)
        return;
    if (need > ctx->phi_repr_cap) {
        uint32_t *grown = (uint32_t *) xr_realloc(ctx->phi_repr, (size_t) need * sizeof(*grown));
        if (!grown)
            return;
        ctx->phi_repr = grown;
        ctx->phi_repr_cap = need;
    }
    for (uint32_t i = 0; i < need; i++)
        ctx->phi_repr[i] = i;

    bool merged_any = false;
    if (any_phi) {
        xi_ensure_rpo(f);
        XiLiveness *live = xi_compute_liveness(f);
        if (live) {
            const XiPhi *reps[CG_PHI_COALESCE_MAX];
            int nreps = 0;
            bool dbg = getenv("XRAY_DBG_PHI_COALESCE") != NULL;
            for (uint32_t bi = 0; bi < f->nblocks; bi++) {
                const XiBlock *blk = f->blocks[bi];
                if (!blk)
                    continue;
                for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
                    if (dbg)
                        fprintf(stderr, "[phi-coalesce] phi%u ctype=%s var=%u cand=%d blk=%u\n",
                                phi->value.id, local_ctype_str_ctx(ctx, f, &phi->value),
                                xi_var_id_is_valid(phi->value.var_id) ? (unsigned) phi->value.var_id
                                                                      : UINT_MAX,
                                (int) cg_phi_coalesce_candidate(ctx, f, phi),
                                phi->value.block ? phi->value.block->id : 9999u);
                    if (!cg_phi_coalesce_candidate(ctx, f, phi))
                        continue;
                    int join = -1;
                    for (int r = 0; r < nreps && join < 0; r++) {
                        const XiPhi *rep = reps[r];
                        if (strcmp(local_ctype_str_ctx(ctx, f, &rep->value),
                                   local_ctype_str_ctx(ctx, f, &phi->value)) != 0)
                            continue;
                        if (!cg_phis_share_var_domain(rep, phi))
                            continue;
                        bool ok = true;
                        for (uint32_t bj = 0; bj < f->nblocks && ok; bj++) {
                            const XiBlock *b2 = f->blocks[bj];
                            if (!b2)
                                continue;
                            for (const XiPhi *m = b2->phis; m; m = m->next) {
                                if (m->value.id >= need ||
                                    ctx->phi_repr[m->value.id] != rep->value.id)
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
        }
    }

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *alias = blk->values[vi];
            if (!alias || alias->id >= need || alias->nargs != 1 || !alias->args[0])
                continue;
            const XiValue *source = alias->args[0];
            if (source->id >= need ||
                !cg_rep_identical_alias_can_share_c_local(ctx, f, alias, source))
                continue;
            uint32_t source_id = ctx->phi_repr[source->id];
            if (source_id >= need)
                continue;
            ctx->phi_repr[alias->id] = source_id;
            merged_any = true;
        }
    }

    ctx->phi_repr_active = merged_any;
    if (merged_any) {
        /* Publish a non-owning view so emit_vref (which has no ctx) can resolve
         * coalesced operands via v->block->func. */
        f->phi_coalesce = ctx->phi_repr;
        f->phi_coalesce_count = need;
    }
}

/* Collect all values and phis to declare at function top.
 * All synchronous functions pre-declare SSA values so the generated translation
 * unit is valid in both C and C++: arbitrary CFG edges may not jump over a C++
 * local declaration, even when the initializer is scalar and side-effect free. */
static void emit_declarations(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    bool pre_decl_all = ctx->pre_decl_all;
    bool needs_defensive_init = cg_has_exception_handling(f);

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
            if (!needs_defensive_init) {
                fprintf(out, ";\n");
            } else if (cg_value_plan_is_aggregate(ctx, &phi->value) ||
                       cg_value_plan_is_vector(ctx, &phi->value)) {
                fprintf(out, " = ");
                emit_value_plan_zero_expr(ctx, out, &phi->value);
                fprintf(out, ";\n");
            } else if (rep == XR_REP_TAGGED)
                fprintf(out, " = XR_NULL_VAL;\n");
            else
                fprintf(out, " = 0;\n");
        }

        /* Each predecessor captures all incoming PHI values before publishing
         * any of them.  Put those edge temporaries at function scope as well:
         * C++ rejects a jump that bypasses their inline declarations. */
        for (uint16_t pred_idx = 0; pred_idx < blk->npreds; pred_idx++) {
            for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
                if (!cg_phi_copy_should_emit(ctx, f, phi, pred_idx))
                    continue;
                fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, &phi->value));
                emit_phi_tmp_ref(out, blk, phi, pred_idx);
                fprintf(out, ";\n");
            }
        }

        /* SSA values (pre-declared for C/C++ CFG compatibility). */
        if (pre_decl_all) {
            for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
                const XiValue *v = blk->values[vi];
                if (cg_value_skips_predecl(ctx, f, v))
                    continue;
                bool debug_only_fixed_wrapper = false;
                if (v->op == XI_FIXED_ARRAY_NEW || v->op == XI_FIXED_BYTES_CONST) {
                    uint8_t native = 0;
                    uint32_t count = 0;
                    if (xicgen_fixed_array_new_info(v, &native, &count)) {
                        fprintf(out, "    %s _fa%u[%u];\n", cg_struct_native_c_type(native), v->id,
                                (unsigned) (count > 0 ? count : 1));
                    }
                    debug_only_fixed_wrapper = cg_fixed_array_wrapper_has_no_release_use(ctx, f, v);
                } else {
                    uint8_t native = 0;
                    uint32_t count = 0;
                    if (xicgen_fixed_array_stack_copy_info(v, &native, &count))
                        fprintf(out, "    %s _fa%u[%u];\n", cg_struct_native_c_type(native), v->id,
                                (unsigned) (count > 0 ? count : 1));
                }
                if (debug_only_fixed_wrapper)
                    fprintf(out, "#if defined(XRAY_AOT_DEBUG_LOCALS)\n");
                XrRep rep = cg_value_plan_storage_rep(ctx, v);
                fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, v));
                emit_vref(out, v);
                if (!needs_defensive_init) {
                    fprintf(out, ";\n");
                } else if (cg_value_plan_is_aggregate(ctx, v) || cg_value_plan_is_vector(ctx, v)) {
                    fprintf(out, " = ");
                    emit_value_plan_zero_expr(ctx, out, v);
                    fprintf(out, ";\n");
                } else if (rep == XR_REP_TAGGED)
                    fprintf(out, " = XR_NULL_VAL;\n");
                else
                    fprintf(out, " = 0;\n");
                if (debug_only_fixed_wrapper)
                    fprintf(out, "#endif\n");
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

/* ===== task 217 P2: per-function abstraction-cost residue (R1–R6) =====
 *
 * Residue is a property of the *generated code*.  Each emitted function body is
 * captured into a scratch stream (xi_cgen_func) and scanned here.  Measuring the
 * C text — rather than re-deriving the emitter's elision decisions from the IR —
 * is the same methodology the port shape gate uses, so the counts reflect
 * exactly what lands in the C unit: a bounds/error/view check the emitter proved
 * away is simply not present to be counted. This is a pre-clang, conservative
 * backend-shape view consumed by `xray verify --contract`. */

static XiFuncResidue *cg_residue_begin(XiCgenCtx *ctx, const XiFunc *f) {
    if (ctx->nfunc_residues == ctx->func_residues_cap) {
        size_t ncap = ctx->func_residues_cap ? ctx->func_residues_cap * 2 : 32;
        XiFuncResidue *grown =
            (XiFuncResidue *) xr_realloc(ctx->func_residues, ncap * sizeof(*grown));
        if (!grown)
            return NULL;
        ctx->func_residues = grown;
        ctx->func_residues_cap = ncap;
    }
    XiFuncResidue *r = &ctx->func_residues[ctx->nfunc_residues++];
    memset(r, 0, sizeof(*r));
    r->func_name = f->name ? f->name : "?";
    r->source_file = (ctx->module && ctx->module->path) ? ctx->module->path : f->source_file;
    return r;
}

static void cg_residue_add(XiFuncResidue *r, XiResidueCategory cat, uint32_t count, uint32_t line,
                           const char *reason) {
    if (!r || cat >= XI_RESIDUE_CATEGORY_COUNT || count == 0)
        return;
    r->counts[cat] += count;
    if (r->nentries == r->entries_cap) {
        uint32_t ncap = r->entries_cap ? r->entries_cap * 2 : 8;
        XiResidueEntry *grown = (XiResidueEntry *) xr_realloc(r->entries, ncap * sizeof(*grown));
        if (!grown)
            return; /* count already bumped; drop per-entry detail under OOM */
        r->entries = grown;
        r->entries_cap = ncap;
    }
    XiResidueEntry *e = &r->entries[r->nentries++];
    e->category = (uint8_t) cat;
    e->line = line;
    e->reason = reason;
}

static bool cg_residue_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Count NUL-terminated substring occurrences. */
static uint32_t cg_scan_count(const char *text, const char *needle) {
    uint32_t n = 0;
    size_t nl = strlen(needle);
    if (!nl)
        return 0;
    for (const char *p = text; (p = strstr(p, needle)) != NULL; p += nl)
        n++;
    return n;
}

static bool cg_name_matches(const char *s, size_t n, const char *lit) {
    return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

/* Runtime helpers that lower to native instructions or belong to another
 * residue category (alloc/bounds/pending/box) or the whitelisted panic cold
 * path / mem.* primitives.  Everything else is genuine R1 residue (fail-closed:
 * an unrecognised xrt_* call still counts). */
static bool cg_r1_call_is_whitelisted(const char *s, size_t n) {
    static const char *const wl[] = {
        /* scalar arithmetic / comparison / bit helpers -> native insns */
        "xrt_div",
        "xrt_int_div",
        "xrt_mod",
        "xrt_int_mod",
        "xrt_neg",
        "xrt_abs",
        "xrt_le",
        "xrt_lt",
        "xrt_ge",
        "xrt_gt",
        "xrt_eq",
        "xrt_ne",
        "xrt_cmp",
        "xrt_shl",
        "xrt_shr",
        "xrt_sar",
        /* canonical signed-width shift helpers are header-inline native
         * operations (the source semantics mask the dynamic shift count) */
        "xrt_i64_shl",
        "xrt_i64_shr",
        "xrt_i64_shr_u",
        "xrt_rotl",
        "xrt_rotr",
        "xrt_min",
        "xrt_max",
        "xrt_pow_int",
        /* span descriptor field accessors (not runtime views) */
        "xrt_span_empty",
        "xrt_span_len",
        "xrt_span_length",
        "xrt_span_data",
        "xrt_value_native_type_size",
        /* fixed-width byte Slice loads are header-inline raw operations; the
         * checked branch, when present, is accounted as R4 below */
        "xrt_byte_slice_load_u16_le_unchecked_raw",
        "xrt_byte_slice_load_u32_le_unchecked_raw",
        "xrt_byte_slice_load_u64_le_unchecked_raw",
        /* accounted in dedicated categories (kept out of R1 double-counting) */
        "xrt_has_pending_error",
        "xrt_index_oob",
        "xrt_fixed_index_oob",
        "xrt_box_obj",
        /* whitelisted panic cold path + explicit mem.* primitives (doc §3.3) */
        "xrt_throw_error",
        "xrt_panic",
        "xrt_abort",
        "xrt_type_no_index",
        "xrt_mem_copy",
        "xrt_mem_move",
        "xrt_mem_set",
        "xrt_mem_compare",
    };
    if (n < 4 || memcmp(s, "xrt_", 4) != 0)
        return true; /* not an xrt_ helper: never R1 */
    for (size_t i = 0; i < sizeof(wl) / sizeof(wl[0]); i++)
        if (cg_name_matches(s, n, wl[i]))
            return true;
    return false;
}

/* R1: non-whitelisted xrt_* runtime-helper call tokens in the body. */
static uint32_t cg_scan_r1_runtime_calls(const char *text) {
    uint32_t n = 0;
    for (const char *p = strstr(text, "xrt_"); p; p = strstr(p + 4, "xrt_")) {
        /* Token must start at a non-identifier boundary. */
        if (p != text && cg_residue_ident_char(p[-1]))
            continue;
        const char *q = p;
        while (cg_residue_ident_char(*q))
            q++;
        if (*q != '(')
            continue; /* a type/name reference, not a call */
        size_t len = (size_t) (q - p);
        /* Allocation helpers belong to R2, not R1. */
        if (cg_name_matches(p, len, "xrt_array_new") ||
            cg_name_matches(p, len, "xrt_array_with_capacity") ||
            cg_name_matches(p, len, "xrt_map_new") || cg_name_matches(p, len, "xrt_set_new") ||
            cg_name_matches(p, len, "xrt_tuple_new") ||
            cg_name_matches(p, len, "xrt_closure_new") || cg_name_matches(p, len, "xrt_json_new") ||
            cg_name_matches(p, len, "xrt_str_concat") || cg_name_matches(p, len, "xrt_gc_alloc") ||
            cg_name_matches(p, len, "xrt_alloc"))
            continue;
        if (!cg_r1_call_is_whitelisted(p, len))
            n++;
    }
    return n;
}

/* Scan one function's captured C body for residue and record it against f. */
static void cg_scan_function_residue(XiCgenCtx *ctx, const XiFunc *f, const char *body) {
    if (!ctx || !f || !body)
        return;
    XiFuncResidue *r = cg_residue_begin(ctx, f);
    if (!r)
        return;
    uint32_t line = (f->nblocks && f->blocks && f->blocks[0] && f->blocks[0]->nvalues &&
                     f->blocks[0]->values && f->blocks[0]->values[0])
                        ? f->blocks[0]->values[0]->line
                        : 0;

    /* R2 heap allocation. */
    uint32_t r2 = cg_scan_count(body, "xrt_array_new(") +
                  cg_scan_count(body, "xrt_array_with_capacity(") +
                  cg_scan_count(body, "xrt_map_new(") + cg_scan_count(body, "xrt_set_new(") +
                  cg_scan_count(body, "xrt_tuple_new(") + cg_scan_count(body, "xrt_closure_new(") +
                  cg_scan_count(body, "xrt_json_new(") + cg_scan_count(body, "xrt_str_concat(") +
                  cg_scan_count(body, "xrt_gc_alloc(") + cg_scan_count(body, "xrt_alloc(");
    cg_residue_add(r, XI_RESIDUE_R2_HEAP_ALLOC, r2, line,
                   "heap allocation not elided (escape evidence incomplete)");

    /* R3 pending-error check. */
    cg_residue_add(r, XI_RESIDUE_R3_PENDING_ERROR, cg_scan_count(body, "xrt_has_pending_error"),
                   line, "callee throw effect not proven absent");

    /* R4 bounds-panic branch. */
    uint32_t r4 = cg_scan_count(body, "xrt_index_oob") +
                  cg_scan_count(body, "xrt_fixed_index_oob") +
                  cg_scan_count(body, "XR_ERROR_CORE_BYTE_SLICE_LOAD_") +
                  cg_scan_count(body, "XR_ERROR_CORE_BYTE_SLICE_STORE_");
    cg_residue_add(r, XI_RESIDUE_R4_BOUNDS_PANIC, r4, line,
                   "index lacks range evidence (window proof missing)");

    /* R5 XrValue box/unbox (out-of-line boxing helpers). */
    uint32_t r5 =
        cg_scan_count(body, "xrt_box_obj(") + cg_scan_count(body, "xrt_enum_aggregate_box(");
    cg_residue_add(r, XI_RESIDUE_R5_BOX_UNBOX, r5, line, "value boxed into XrValue (tagged ABI)");

    /* R6 aggregate<->native vector round-trip. */
    cg_residue_add(r, XI_RESIDUE_R6_LANES_ROUNDTRIP, cg_scan_count(body, "_lanes"), line,
                   "vector value spilled to lane aggregate (native SSA not planned)");

    /* R1 non-whitelisted runtime-helper call. */
    cg_residue_add(r, XI_RESIDUE_R1_RUNTIME_CALL, cg_scan_r1_runtime_calls(body), line,
                   "runtime-helper call not elided (effect/escape evidence incomplete)");
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
static bool cg_func_is_exported_class_member(const XiCgenCtx *ctx, const XiFunc *f);

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
            if (!cg_func_needs_aot_coro_ctx(ctx, owner) &&
                cg_static_function_value_uses_are_parallel_callbacks(owner, v, target))
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

static const XiClassData *cg_native_receiver_target_class_data(XiCgenCtx *ctx,
                                                               const XiFunc *target) {
    CgClassNativeFunc info = cg_class_native_func(ctx, target);
    return info.class_data ? info.class_data : cg_func_param_native_class_data(ctx, target, 0);
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

    const XiClassData *target_class = cg_native_receiver_target_class_data(ctx, target);
    const XiClassData *source_info = cg_class_native_instance_data(ctx, owner, call->args[0]);
    return !(cg_class_native_instance_origin(ctx, owner, call->args[0]) &&
             cg_class_native_can_pass_instance_as(ctx, source_info, target_class));
}

static bool cg_native_receiver_dispatch_plan_needs_boxed_adapter(XiCgenCtx *ctx,
                                                                 const XiFunc *owner,
                                                                 const XiValue *call,
                                                                 const XiFunc *target) {
    const XaotBundle *bundle;
    const XaotMethodDispatchPlan *plan;
    const XiClassData *target_class;
    const XiClassData *source_info;

    if (!ctx || !owner || !call || !target ||
        (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) || call->nargs < 1)
        return false;
    bundle = cg_ctx_aot_bundle(ctx);
    plan = xaot_bundle_find_method_dispatch_plan_for_xi_call(bundle, call);
    if (!bundle || !plan ||
        (plan->kind != XAOT_DISPATCH_DIRECT && plan->kind != XAOT_DISPATCH_TYPE_SWITCH) ||
        plan->target_count == 0 || plan->target_start == 0 ||
        plan->target_start - 1 + plan->target_count > bundle->ndispatch_target_cases)
        return false;

    for (uint16_t i = 0; i < plan->target_count; i++) {
        const XaotDispatchTargetCase *target_case =
            &bundle->dispatch_target_cases[plan->target_start - 1 + i];
        const XiFunc *target_func =
            xaot_bundle_find_dispatch_target_func(bundle, target_case, NULL);
        if (target_func == target) {
            target_class = cg_native_receiver_target_class_data(ctx, target);
            source_info = cg_class_native_instance_data(ctx, owner, call->args[0]);
            return !(cg_class_native_instance_origin(ctx, owner, call->args[0]) &&
                     cg_class_native_can_pass_instance_as(ctx, source_info, target_class));
        }
    }
    return false;
}

static bool cg_native_receiver_getter_field_needs_boxed_adapter(XiCgenCtx *ctx, const XiFunc *owner,
                                                                const XiValue *load,
                                                                const XiFunc *target) {
    if (!ctx || !owner || !load || !target || load->op != XI_LOAD_FIELD || load->nargs < 1)
        return false;

    const XiClassData *source_info = NULL;
    const XiFunc *mfunc =
        cg_class_native_resolve_getter_field_method(ctx, owner, load, &source_info, NULL);
    if (mfunc != target)
        return false;

    const XiClassData *target_class = cg_native_receiver_target_class_data(ctx, target);
    return !(cg_class_native_instance_origin(ctx, owner, load->args[0]) &&
             cg_class_native_can_pass_instance_as(ctx, source_info, target_class));
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
    if (cg_value_plan_storage_rep(ctx, call) == XR_REP_PTR ||
        cg_class_native_ctor_can_inline(ctx, owner, call) ||
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
            if (cg_native_receiver_dispatch_plan_needs_boxed_adapter(ctx, owner, v, target) ||
                cg_native_receiver_method_call_needs_boxed_adapter(ctx, owner, v, target) ||
                cg_native_receiver_getter_field_needs_boxed_adapter(ctx, owner, v, target) ||
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

typedef struct {
    XiModule *module;
    int nshared;
    int nmethod;
    int shared_cap;
    int methods_cap;
    XiFunc **shared_funcs;
    XiClassData **shared_class;
    XiEnumData **shared_enum;
    CgSharedNativeInstance *shared_native_instances;
    CgMethodEntry *methods;
} CgModuleScanSnapshot;

/* Boxed-adapter discovery scans other modules' IR, but shared slots and method
 * tables are interpreted through the current CGen context.  Snapshot the
 * emitter context while a scanned module is made current. */
static bool cg_module_scan_snapshot_save(XiCgenCtx *ctx, CgModuleScanSnapshot *snap) {
    if (!ctx || !snap)
        return false;
    memset(snap, 0, sizeof(*snap));
    snap->module = ctx->module;
    snap->nshared = ctx->nshared;
    snap->nmethod = ctx->nmethod;
    snap->shared_cap = ctx->shared_cap;
    snap->methods_cap = ctx->methods_cap;
    if (ctx->shared_cap > 0) {
        size_t shared_bytes = (size_t) ctx->shared_cap;
        snap->shared_funcs = (XiFunc **) xr_malloc(shared_bytes * sizeof(*snap->shared_funcs));
        snap->shared_class = (XiClassData **) xr_malloc(shared_bytes * sizeof(*snap->shared_class));
        snap->shared_enum = (XiEnumData **) xr_malloc(shared_bytes * sizeof(*snap->shared_enum));
        snap->shared_native_instances = (CgSharedNativeInstance *) xr_malloc(
            shared_bytes * sizeof(*snap->shared_native_instances));
        if (!snap->shared_funcs || !snap->shared_class || !snap->shared_enum ||
            !snap->shared_native_instances)
            return false;
        memcpy(snap->shared_funcs, ctx->shared_funcs, shared_bytes * sizeof(*snap->shared_funcs));
        memcpy(snap->shared_class, ctx->shared_class, shared_bytes * sizeof(*snap->shared_class));
        memcpy(snap->shared_enum, ctx->shared_enum, shared_bytes * sizeof(*snap->shared_enum));
        memcpy(snap->shared_native_instances, ctx->shared_native_instances,
               shared_bytes * sizeof(*snap->shared_native_instances));
    }
    if (ctx->methods_cap > 0) {
        size_t method_bytes = (size_t) ctx->methods_cap;
        snap->methods = (CgMethodEntry *) xr_malloc(method_bytes * sizeof(*snap->methods));
        if (!snap->methods)
            return false;
        memcpy(snap->methods, ctx->methods, method_bytes * sizeof(*snap->methods));
    }
    return true;
}

static void cg_module_scan_snapshot_free(CgModuleScanSnapshot *snap) {
    if (!snap)
        return;
    xr_free(snap->shared_funcs);
    xr_free(snap->shared_class);
    xr_free(snap->shared_enum);
    xr_free(snap->shared_native_instances);
    xr_free(snap->methods);
    memset(snap, 0, sizeof(*snap));
}

static void cg_module_scan_snapshot_restore(XiCgenCtx *ctx, CgModuleScanSnapshot *snap) {
    if (!ctx || !snap)
        return;
    ctx->module = snap->module;
    ctx->nshared = snap->nshared;
    ctx->nmethod = snap->nmethod;
    if (ctx->shared_cap > 0) {
        size_t current_shared = (size_t) ctx->shared_cap;
        memset(ctx->shared_funcs, 0, current_shared * sizeof(*ctx->shared_funcs));
        memset(ctx->shared_class, 0, current_shared * sizeof(*ctx->shared_class));
        memset(ctx->shared_enum, 0, current_shared * sizeof(*ctx->shared_enum));
        memset(ctx->shared_native_instances, 0,
               current_shared * sizeof(*ctx->shared_native_instances));
    }
    if (snap->shared_cap > 0) {
        size_t saved_shared = (size_t) snap->shared_cap;
        memcpy(ctx->shared_funcs, snap->shared_funcs, saved_shared * sizeof(*ctx->shared_funcs));
        memcpy(ctx->shared_class, snap->shared_class, saved_shared * sizeof(*ctx->shared_class));
        memcpy(ctx->shared_enum, snap->shared_enum, saved_shared * sizeof(*ctx->shared_enum));
        memcpy(ctx->shared_native_instances, snap->shared_native_instances,
               saved_shared * sizeof(*ctx->shared_native_instances));
    }
    if (ctx->methods_cap > 0)
        memset(ctx->methods, 0, (size_t) ctx->methods_cap * sizeof(*ctx->methods));
    if (snap->methods_cap > 0)
        memcpy(ctx->methods, snap->methods, (size_t) snap->methods_cap * sizeof(*ctx->methods));
    cg_module_scan_snapshot_free(snap);
}

static bool cg_imported_static_function_uses_are_direct_in_module(XiCgenCtx *ctx,
                                                                  const XiModule *module,
                                                                  const XiFunc *target) {
    if (!ctx || !module || !module->init || !target)
        return false;
    if (module == ctx->module)
        return cg_imported_static_function_uses_are_direct(ctx, module->init, target);

    CgModuleScanSnapshot snap;
    if (!cg_module_scan_snapshot_save(ctx, &snap)) {
        cg_module_scan_snapshot_free(&snap);
        return false;
    }
    cg_init_from_module(ctx, (XiModule *) module);
    cg_register_imported_classes(ctx);
    bool direct = cg_imported_static_function_uses_are_direct(ctx, module->init, target);
    cg_module_scan_snapshot_restore(ctx, &snap);
    return direct;
}

static bool cg_imported_static_function_uses_are_direct_in_bundle(XiCgenCtx *ctx,
                                                                  const XiFunc *target) {
    if (!ctx || !target)
        return false;
    for (int i = 0; i < ctx->all_nmodules; i++) {
        const XiModule *mod = ctx->all_modules ? ctx->all_modules[i] : NULL;
        if (!mod || !mod->init)
            continue;
        if (!cg_imported_static_function_uses_are_direct_in_module(ctx, mod, target))
            return false;
    }
    return true;
}

static bool cg_func_has_native_receiver_boxed_use_in_module(XiCgenCtx *ctx, const XiModule *module,
                                                            const XiFunc *target,
                                                            const char *prefix) {
    if (!ctx || !module || !module->init || !target)
        return false;
    if (module == ctx->module)
        return cg_func_has_native_receiver_boxed_use(ctx, module->init, target, prefix);

    CgModuleScanSnapshot snap;
    if (!cg_module_scan_snapshot_save(ctx, &snap)) {
        cg_module_scan_snapshot_free(&snap);
        return false;
    }
    cg_init_from_module(ctx, (XiModule *) module);
    cg_register_imported_classes(ctx);
    bool found = cg_func_has_native_receiver_boxed_use(ctx, module->init, target, prefix);
    cg_module_scan_snapshot_restore(ctx, &snap);
    return found;
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
        if (cg_func_has_native_receiver_boxed_use_in_module(ctx, mod, target, prefix))
            return true;
    }

    const XiFunc *root = ctx->module ? ctx->module->init : NULL;
    return !scanned_current && cg_func_has_native_receiver_boxed_use(ctx, root, target, prefix);
}

static bool cg_func_needs_boxed_adapter(XiCgenCtx *ctx, const XiFunc *f, const char *prefix,
                                        bool typed_abi, bool native_receiver) {
    if (xicgen_func_is_boxed_dispatch_target(ctx, f))
        return true;
    if (!typed_abi && !native_receiver)
        return false;
    /* Exported class methods are an open dynamic ABI surface even in an
     * executable: values may cross an `unknown` boundary in another module and
     * dispatch through the boxed receiver contract. */
    if (native_receiver && cg_func_is_exported_class_member(ctx, f))
        return true;

    bool native_class_ptr_param = cg_func_has_native_class_ptr_param(ctx, f);
    const XiFunc *root = ctx && ctx->module ? ctx->module->init : NULL;
    if (typed_abi && !cg_func_is_par_for_native_callback(f) &&
        cg_func_has_parallel_callback_descriptor_use(root, f))
        return true;
    if (cg_func_is_shared_slot_value(ctx, f)) {
        int func_slot = cg_shared_slot_for_func(ctx, f);
        if (func_slot >= 0) {
            if (!native_receiver &&
                !cg_shared_static_function_slot_can_elide(ctx, root, func_slot, f))
                return true;
        } else if (!native_receiver) {
            return true;
        }
    }

    if (native_receiver)
        return cg_func_has_native_receiver_boxed_use_in_bundle(ctx, f, prefix);

    if (typed_abi && native_class_ptr_param) {
        bool needs = cg_func_has_native_receiver_boxed_use_in_bundle(ctx, f, prefix);
        if (needs)
            return true;
    }

    if (cg_func_needs_sync_go_wrapper_ctx(ctx, f) && native_class_ptr_param)
        return true;

    return cg_func_has_unelided_closure_value_use(ctx, root, f, prefix);
}

/* Emit the purity attribute proven by prepare (no-op without a plan).
 * Placed between `static` and the return type on definitions and forward
 * declarations so clang/gcc can CSE / LICM across call sites. */
static void emit_func_attr_qualifier(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    /* GCC/Clang ignore pure/const on a void-returning function and warn.
     * Keep the semantic plan for verification, but do not emit a C attribute
     * that cannot enable value-based CSE. */
    if (!f || !f->return_type || f->return_type->kind == XR_KIND_UNIT)
        return;
    const XaotFuncAttrPlan *plan = xaot_bundle_find_func_attr_plan(cg_ctx_aot_bundle(ctx), f);
    if (!plan)
        return;
    if (plan->flags & XAOT_FN_ATTR_CONST)
        fprintf(out, "XRT_FN_CONST ");
    else if (plan->flags & XAOT_FN_ATTR_PURE)
        fprintf(out, "XRT_FN_PURE ");
}

static bool cg_func_has_runtime_simd_dispatch_local(const XiFunc *f) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            if (block->values[vi] && (block->values[vi]->op == XI_TARGET_SIMD_BYTES ||
                                      block->values[vi]->op == XI_TARGET_SIMD_ACCELERATED ||
                                      block->values[vi]->op == XI_TARGET_SIMD_RUNTIME_SELECTED))
                return true;
        }
    }
    return false;
}

/* Resolve feature-propagation edges from the callee's owning module, never
 * from the C unit currently being emitted. Forward declarations for imported
 * functions are generated under a different current module than definitions;
 * consulting ctx->imports there can otherwise assign different target attrs
 * to the same symbol. */
static const XiFunc *cg_target_resolve_static_call(const XiCgenCtx *ctx, const XiFunc *current,
                                                   const XiValue *callee) {
    callee = cg_unwrap_identity_value(callee);
    if (!ctx || !callee)
        return NULL;
    if ((callee->op == XI_CLOSURE_NEW ||
         (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW)) &&
        callee->aux)
        return (const XiFunc *) callee->aux;
    const XiImportRef *ref = NULL;
    if (callee->op == XI_GET_SHARED) {
        int slot = (int) callee->aux_int;
        for (const XiFunc *owner = current; owner; owner = owner->parent_func) {
            if (owner->shared_slot_funcs && slot >= 0 &&
                slot < (int) owner->shared_slot_func_count && owner->shared_slot_funcs[slot]) {
                return owner->shared_slot_funcs[slot];
            }
        }
        const XiModule *owner_module = cg_module_for_func(ctx, current);
        if (owner_module && owner_module->slot_funcs && slot >= 0 &&
            slot < (int) owner_module->nslots && owner_module->slot_funcs[slot]) {
            return owner_module->slot_funcs[slot];
        }
        const XiFunc *module_init = owner_module ? owner_module->init : current;
        ref = cg_shared_slot_import_ref(module_init, slot);
    } else if (callee->op == XI_IMPORT_REF && callee->aux) {
        ref = (const XiImportRef *) callee->aux;
    }
    if (!ref || !ref->member_name)
        return NULL;
    return cg_resolve_module_export_static_call((XiCgenCtx *) ctx, ref, ref->member_name).func;
}

static bool cg_func_has_native_vector_width(const XiCgenCtx *ctx, const XiFunc *f, uint8_t width) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!bundle || !f)
        return false;
    for (uint32_t i = 0; i < bundle->nvalue_plans; i++) {
        const XaotValuePlan *plan = &bundle->value_plans[i];
        if (plan->func == f && plan->rep.kind == XAOT_VALUE_VECTOR &&
            plan->rep.vector_width_bytes == width)
            return true;
    }
    return false;
}

/* Propagate the target feature through direct calls until an explicit runtime
 * width query. This lets a whole AVX2 or AVX-512 loop absorb its leaf kernels
 * while the query-owning dispatcher remains baseline SSE2. */
static bool cg_func_requires_x86_vector_target_depth(const XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiFunc *origin, uint8_t depth,
                                                     uint8_t width) {
    if (!ctx || !f || depth > 16)
        return false;
    /* @noinline is also an explicit optimization/ABI boundary.  The function
     * itself may own a target island, but callers must not inherit its ISA just
     * because a cold or length-guarded path enters that island. */
    if (depth > 0 && cg_func_should_noinline(f))
        return false;
    /* A width selector is the island entry.  In a static AVX build it may be
     * target-qualified when evaluated as the origin (so its compatible leaf
     * kernels can inline), but the target must not propagate through it into
     * public short-path callers.  Runtime dispatch keeps the selector itself
     * baseline because it owns the CPU/OS capability check. */
    if (cg_func_has_runtime_simd_dispatch_local(f) &&
        ((!ctx->target || ctx->target->simd_mode == XAOT_SIMD_DISPATCH) || depth > 0))
        return false;
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!bundle)
        return false;
    if (cg_func_has_native_vector_width(ctx, f, width))
        return true;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (!value || value->op != XI_CALL || value->nargs < 1)
                continue;
            const XiFunc *target = cg_target_resolve_static_call(ctx, f, value->args[0]);
            if (!target || target == origin)
                continue;
            if (cg_func_requires_x86_vector_target_depth(ctx, target, origin, (uint8_t) (depth + 1),
                                                         width))
                return true;
        }
    }
    return false;
}

/* Keep every selected AVX2/AVX-512 function or same-feature caller behind its
 * target attribute.  Runtime-dispatch callers enter these islands only after
 * the OS/CPU capability check.  Static AVX builds may absorb the full reachable
 * SIMD call chain while unrelated scalar functions remain baseline. */
static bool cg_func_requires_x86_vector_target(const XiCgenCtx *ctx, const XiFunc *f,
                                               uint8_t width) {
    uint32_t required_feature = width == 64 ? XAOT_SIMD_FEATURE_AVX512 : XAOT_SIMD_FEATURE_AVX2;
    if (!ctx || !ctx->target || !f || (ctx->target->simd_features & required_feature) == 0)
        return false;
    return cg_func_requires_x86_vector_target_depth(ctx, f, f, 0, width);
}

/* A static AVX2 build may intentionally inline a portable 128-bit SIMD helper
 * into its callers.  Compile that complete direct-call island for the selected
 * target as well: this gives the native optimizer permission to combine
 * adjacent baseline vectors without changing the source-level vector width.
 * Runtime dispatch must keep these helpers baseline because no capability
 * check guards their ordinary public callers. */
static bool cg_func_requires_static_x86_inline_target_depth(const XiCgenCtx *ctx, const XiFunc *f,
                                                            const XiFunc *origin, uint8_t depth) {
    if (!ctx || !f || depth > 16)
        return false;
    if (depth > 0 && cg_func_should_noinline(f))
        return false;
    if (f->inline_policy == XI_INLINE_PREFER && cg_func_has_native_vector_width(ctx, f, 16))
        return true;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *block = f->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (!value || value->op != XI_CALL || value->nargs < 1)
                continue;
            const XiFunc *target = cg_target_resolve_static_call(ctx, f, value->args[0]);
            if (!target || target == origin)
                continue;
            if (cg_func_requires_static_x86_inline_target_depth(ctx, target, origin,
                                                                (uint8_t) (depth + 1)))
                return true;
        }
    }
    return false;
}

static bool cg_func_requires_static_x86_inline_target(const XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !ctx->target || !f || ctx->target->simd_mode == XAOT_SIMD_DISPATCH ||
        (ctx->target->simd_features & XAOT_SIMD_FEATURE_AVX2) == 0)
        return false;
    return cg_func_requires_static_x86_inline_target_depth(ctx, f, f, 0);
}

static void emit_func_target_qualifier(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    if (cg_func_requires_x86_vector_target(ctx, f, 64))
        fprintf(out, "XRT_TARGET_AVX512 ");
    else if (cg_func_requires_x86_vector_target(ctx, f, 32) ||
             cg_func_requires_static_x86_inline_target(ctx, f))
        fprintf(out, "XRT_TARGET_AVX2 ");
}

static bool cg_func_attrs_apply_to_internal(const XiFunc *f) {
    return f && !f->export_plan && !f->entry_plan && f->link_plan;
}

static void emit_aot_symbol_attrs(FILE *out, const XiFunc *f, bool c_export_wrapper) {
    if (!f)
        return;
    const char *section =
        f->link_plan ? f->link_plan->section : (f->entry_plan ? f->entry_plan->section : NULL);
    if (section) {
        fprintf(out, "XRT_ATTR_SECTION(");
        emit_c_string_literal(out, section);
        fprintf(out, ") ");
    }
    if (c_export_wrapper && f->link_plan && f->link_plan->weak)
        fprintf(out, "XRT_ATTR_WEAK ");
    if ((f->link_plan && f->link_plan->used) || f->entry_plan)
        fprintf(out, "XRT_ATTR_USED ");
}

static void emit_aot_entry_attrs(FILE *out, const XiFunc *f) {
    if (!f || !f->entry_plan)
        return;
    if (f->entry_plan->section) {
        fprintf(out, "XRT_ATTR_SECTION(");
        emit_c_string_literal(out, f->entry_plan->section);
        fprintf(out, ") ");
    }
    fprintf(out, "XRT_ATTR_USED ");
}

static void emit_cfn_stub_signature(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix,
                                    bool cross_module) {
    fprintf(out, "%s%s ", cg_func_forward_linkage(ctx, f, prefix, cross_module),
            cg_cfn_value_c_type(f->return_type, true));
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
        suffix = emit_conversion_prefix_ctx(ctx, out, pt, from_rep, to_rep);
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

    emit_cfn_stub_signature(ctx, out, f, prefix, false);
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
        suffix = emit_conversion_prefix_ctx(ctx, out, ret_type, ret_rep, c_ret_rep);
        emit_cfn_target_call_expr(ctx, out, f, prefix);
        emit_conversion_suffix(out, suffix);
        fprintf(out, ")");
    }
    fprintf(out, ";\n");
    fprintf(out, "}\n\n");
}

static bool cg_c_export_native_scalar_supported(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I64:
        case XR_NATIVE_F64:
        case XR_NATIVE_BOOL:
        case XR_NATIVE_I8:
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
        case XR_NATIVE_POINTER:
        case XR_NATIVE_F32:
            return true;
        default:
            return false;
    }
}

static bool cg_c_export_struct_layout_supported_depth(const XrAggregateLayout *layout, int depth) {
    if (!layout || !xr_aggregate_layout_is_headerless(layout) || depth > 8 ||
        layout->field_count == 0 || layout->field_count > XR_MAX_AGG_FIELDS ||
        !cg_struct_native_heap_supported(layout))
        return false;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrAggregateFieldLayout *field = &layout->fields[i];
        if (cg_c_export_native_scalar_supported(field->native_type))
            continue;
        if (field->native_type == XR_NATIVE_ARRAY && field->elem_count > 0 &&
            cg_c_export_native_scalar_supported(field->elem_native_type))
            continue;
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE &&
            cg_c_export_struct_layout_supported_depth(field->sub_layout, depth + 1))
            continue;
        return false;
    }
    return true;
}

static const XrAggregateLayout *cg_c_export_struct_layout_for_type(XiCgenCtx *ctx,
                                                                   const XrType *type) {
    const XrAggregateLayout *layout = cg_type_struct_layout(type);
    if (cg_c_export_struct_layout_supported_depth(layout, 0))
        return layout;
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE) ||
        !type->instance.class_name)
        return NULL;
    const XiClassData *cd = cg_class_native_data_by_name(ctx, type->instance.class_name);
    if (cd && cg_c_export_struct_layout_supported_depth(cd->struct_layout, 0))
        return cd->struct_layout;
    for (int mi = 0; ctx && mi < ctx->all_nmodules; mi++) {
        const XiModule *module = ctx->all_modules ? ctx->all_modules[mi] : NULL;
        if (!module || !module->classes)
            continue;
        for (uint16_t ci = 0; ci < module->nclasses; ci++) {
            cd = module->classes[ci];
            if (cd && cd->class_name && strcmp(cd->class_name, type->instance.class_name) == 0 &&
                cg_c_export_struct_layout_supported_depth(cd->struct_layout, 0))
                return cd->struct_layout;
        }
    }
    return NULL;
}

static bool cg_c_export_abi_slot_is_struct_aggregate(const XaotAbiSlot *slot) {
    return slot && cg_value_rep_is_struct_aggregate(xaot_abi_slot_value_rep(slot));
}

static bool cg_c_export_xray_func_signature_supported(XiCgenCtx *ctx, const XiFunc *f) {
    if (!f || f->is_vararg)
        return false;
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    if (!plan)
        return false;
    if (!cg_cfn_value_type_supported(f->return_type, true) &&
        !cg_c_export_abi_slot_is_struct_aggregate(&plan->abi.ret))
        return false;
    for (uint16_t i = 0; i < f->nparams; i++) {
        const XrType *pt = f->params && f->params[i] ? f->params[i]->type : NULL;
        if (cg_cfn_value_type_supported(pt, false))
            continue;
        if (i >= plan->abi.nparams || !plan->abi.params ||
            !cg_c_export_abi_slot_is_struct_aggregate(&plan->abi.params[i]))
            return false;
    }
    return true;
}

static const char *cg_c_export_func_prefix(XiCgenCtx *ctx, const XiFunc *f) {
    const XiModule *module = cg_cfn_module_for_func(ctx, f);
    return module && module->name && module->name[0] ? module->name : "mod";
}

static const char *cg_c_export_scalar_c_type(XiCgenCtx *ctx, const XrType *type, bool is_return) {
    const char *scalar_type = cg_cfn_value_c_type(type, is_return);
    if (!scalar_type || !ctx || ctx->c_dialect != XI_CGEN_C_DIALECT_C90)
        return scalar_type;

    /* The restricted C90 lane is LP64 with a 32-bit int.  Spell public
     * prototypes using C90 primitive types so the generated header does not
     * depend on C99 <stdint.h>.  The target/header guards freeze the widths. */
    if (strcmp(scalar_type, "bool") == 0)
        return "int";
    if (strcmp(scalar_type, "int8_t") == 0)
        return "signed char";
    if (strcmp(scalar_type, "uint8_t") == 0)
        return "unsigned char";
    if (strcmp(scalar_type, "int16_t") == 0)
        return "signed short";
    if (strcmp(scalar_type, "uint16_t") == 0)
        return "unsigned short";
    if (strcmp(scalar_type, "int32_t") == 0)
        return "signed int";
    if (strcmp(scalar_type, "uint32_t") == 0)
        return "unsigned int";
    if (strcmp(scalar_type, "int64_t") == 0 || strcmp(scalar_type, "intptr_t") == 0)
        return "signed long";
    if (strcmp(scalar_type, "uint64_t") == 0 || strcmp(scalar_type, "uintptr_t") == 0)
        return "unsigned long";
    return scalar_type;
}

static void emit_c_export_value_c_type(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                       const char *prefix, const XrType *type, bool is_return) {
    const char *scalar_type = cg_c_export_scalar_c_type(ctx, type, is_return);
    if (scalar_type) {
        fprintf(out, "%s", scalar_type);
        return;
    }
    const XrAggregateLayout *layout = cg_c_export_struct_layout_for_type(ctx, type);
    if (layout) {
        char tname[128];
        cg_struct_heap_type_name(tname, sizeof(tname),
                                 prefix ? prefix : cg_c_export_func_prefix(ctx, f), layout);
        fprintf(out, "%s", tname);
        return;
    }
    if (ctx)
        ctx->error = true;
    fprintf(out, "%s", is_return ? "void" : "int64_t");
}

static void emit_c_export_return_c_type(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const char *prefix) {
    const char *scalar_type = cg_c_export_scalar_c_type(ctx, f ? f->return_type : NULL, true);
    if (scalar_type) {
        fprintf(out, "%s", scalar_type);
        return;
    }
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    if (plan && cg_c_export_abi_slot_is_struct_aggregate(&plan->abi.ret) && plan->abi.ret.c_type) {
        fprintf(out, "%s", plan->abi.ret.c_type);
        return;
    }
    emit_c_export_value_c_type(ctx, out, f, prefix, f ? f->return_type : NULL, true);
}

static void emit_c_export_param_c_type(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                       const char *prefix, uint16_t index) {
    const XrType *pt = f && f->params && f->params[index] ? f->params[index]->type : NULL;
    const char *scalar_type = cg_c_export_scalar_c_type(ctx, pt, false);
    if (scalar_type) {
        fprintf(out, "%s", scalar_type);
        return;
    }
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    if (plan && index < plan->abi.nparams && plan->abi.params &&
        cg_c_export_abi_slot_is_struct_aggregate(&plan->abi.params[index]) &&
        plan->abi.params[index].c_type) {
        fprintf(out, "%s", plan->abi.params[index].c_type);
        return;
    }
    emit_c_export_value_c_type(ctx, out, f, prefix, pt, false);
}

static bool cg_func_can_have_c_export_stub(XiCgenCtx *ctx, const XiFunc *f) {
    if (!f || !f->export_plan || !f->export_plan->symbol || !f->export_plan->symbol[0] ||
        f->is_extern || !cg_cfn_func_has_module_level_storage(ctx, f) || f->ncaptures > 0 ||
        cg_func_needs_aot_coro_ctx(ctx, f))
        return false;
    return cg_c_export_xray_func_signature_supported(ctx, f);
}

static bool cg_func_can_have_entry_stub(XiCgenCtx *ctx, const XiFunc *f) {
    if (!f || !f->entry_plan || !f->entry_plan->symbol || !f->entry_plan->symbol[0] ||
        f->is_extern || !cg_cfn_func_has_module_level_storage(ctx, f) || f->ncaptures > 0 ||
        cg_func_needs_aot_coro_ctx(ctx, f))
        return false;
    return cg_c_export_xray_func_signature_supported(ctx, f);
}

static void emit_c_export_stub_signature(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const char *prefix, bool with_attrs) {
    if (with_attrs) {
        const char *visibility = f && f->export_plan ? f->export_plan->visibility : NULL;
        fprintf(out, "%s ",
                visibility && strcmp(visibility, "hidden") == 0 ? "XRT_INTERNAL" : "XR_EXPORT_SYM");
        emit_aot_symbol_attrs(out, f, true);
    }
    emit_c_export_return_c_type(ctx, out, f, prefix);
    fprintf(out, " %s(", f->export_plan->symbol);
    if (f->nparams == 0) {
        fprintf(out, "void");
    } else {
        for (uint16_t i = 0; i < f->nparams; i++) {
            if (i > 0)
                fprintf(out, ", ");
            emit_c_export_param_c_type(ctx, out, f, prefix, i);
            fprintf(out, " p%u", i);
        }
    }
    fprintf(out, ")");
}

static void emit_entry_stub_signature(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                      const char *prefix, bool with_attrs) {
    if (with_attrs)
        emit_aot_entry_attrs(out, f);
    emit_c_export_return_c_type(ctx, out, f, prefix);
    fprintf(out, " %s(", f->entry_plan->symbol);
    if (f->nparams == 0) {
        fprintf(out, "void");
    } else {
        for (uint16_t i = 0; i < f->nparams; i++) {
            if (i > 0)
                fprintf(out, ", ");
            emit_c_export_param_c_type(ctx, out, f, prefix, i);
            fprintf(out, " p%u", i);
        }
    }
    fprintf(out, ")");
}

typedef struct CgCExportStructTypedef {
    const XrAggregateLayout *layout;
    const char *prefix; /* owned: export C-name prefix (Xi arena/static); local, emit-scope only */
    char c_name[128];
} CgCExportStructTypedef;

static void cg_c_export_collect_struct_typedef(const char *prefix, const XrAggregateLayout *layout,
                                               CgCExportStructTypedef *items, int *count) {
    if (!prefix || !layout || !items || !count || *count >= CG_STRUCT_TYPEDEF_MAX ||
        !cg_c_export_struct_layout_supported_depth(layout, 0))
        return;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        if (layout->fields[i].native_type == XR_NATIVE_NESTED_AGGREGATE)
            cg_c_export_collect_struct_typedef(prefix, layout->fields[i].sub_layout, items, count);
    }

    char c_name[128];
    cg_struct_heap_type_name(c_name, sizeof(c_name), prefix, layout);
    for (int i = 0; i < *count; i++) {
        if (strcmp(items[i].c_name, c_name) == 0)
            return;
    }
    items[*count].layout = layout;
    items[*count].prefix = prefix;
    snprintf(items[*count].c_name, sizeof(items[*count].c_name), "%s", c_name);
    (*count)++;
}

static void cg_c_export_collect_signature_typedefs(XiCgenCtx *ctx, const XiFunc *f,
                                                   CgCExportStructTypedef *items, int *count) {
    if (!f)
        return;
    if (f->export_plan && f->export_plan->header && cg_func_can_have_c_export_stub(ctx, f)) {
        const char *prefix = cg_c_export_func_prefix(ctx, f);
        const XaotFuncPlan *plan = cg_func_plan(ctx, f);
        if (plan && cg_c_export_abi_slot_is_struct_aggregate(&plan->abi.ret)) {
            const XrAggregateLayout *layout =
                cg_c_export_struct_layout_for_type(ctx, f->return_type);
            cg_c_export_collect_struct_typedef(prefix, layout, items, count);
        }
        for (uint16_t i = 0; plan && i < f->nparams && i < plan->abi.nparams; i++) {
            if (!plan->abi.params ||
                !cg_c_export_abi_slot_is_struct_aggregate(&plan->abi.params[i]))
                continue;
            const XrType *pt = f->params && f->params[i] ? f->params[i]->type : NULL;
            const XrAggregateLayout *layout = cg_c_export_struct_layout_for_type(ctx, pt);
            cg_c_export_collect_struct_typedef(prefix, layout, items, count);
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++)
        cg_c_export_collect_signature_typedefs(ctx, f->children[i], items, count);
}

static void emit_c_export_header_func(XiCgenCtx *ctx, FILE *out, const XiFunc *f, uint32_t *count) {
    if (!f)
        return;
    if (f->export_plan && f->export_plan->header) {
        if (!cg_func_can_have_c_export_stub(ctx, f)) {
            fprintf(stderr,
                    "[xi_cgen] ERROR: export.c function '%s' must be a top-level "
                    "noncapturing non-coroutine function with a supported C ABI signature\n",
                    f->name ? f->name : "<anonymous>");
            ctx->error = true;
            return;
        }
        emit_c_export_stub_signature(ctx, out, f, cg_c_export_func_prefix(ctx, f), false);
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
    CgCExportStructTypedef typedefs[CG_STRUCT_TYPEDEF_MAX];
    int typedef_count = 0;

    XR_DCHECK(ctx != NULL, "xi_cgen_c_export_header: NULL ctx");
    XR_DCHECK(out != NULL, "xi_cgen_c_export_header: NULL out");

    ctx->all_modules = modules;
    ctx->all_nmodules = modules && nmodules > 0 ? nmodules : 0;
    cg_reachability_cache_clear(ctx);

    memset(typedefs, 0, sizeof(typedefs));
    for (int i = 0; i < nmodules; i++) {
        if (!modules || !modules[i] || !modules[i]->init)
            continue;
        cg_c_export_collect_signature_typedefs(ctx, modules[i]->init, typedefs, &typedef_count);
    }
    if (ctx->c_dialect == XI_CGEN_C_DIALECT_C90 && typedef_count > 0) {
        fprintf(stderr, "[xi_cgen] ERROR: restricted C90 does not support aggregate public C ABI "
                        "signatures\n");
        ctx->error = true;
        return;
    }

    fprintf(out, "#ifndef %s\n", header_guard);
    fprintf(out, "#define %s\n\n", header_guard);
    if (ctx->c_dialect == XI_CGEN_C_DIALECT_C90) {
        fprintf(out, "#include <limits.h>\n");
        fprintf(out, "#include <stddef.h>\n\n");
        fprintf(out, "#if UINT_MAX != 0xffffffffU || ULONG_MAX <= 0xffffffffUL\n");
        fprintf(out, "#error \"restricted C90 exports require a 32-bit int and 64-bit long\"\n");
        fprintf(out, "#endif\n\n");
    } else {
        fprintf(out, "#include <stdint.h>\n");
        fprintf(out, "#include <stddef.h>\n\n");
    }
    for (int i = 0; i < typedef_count; i++)
        emit_struct_native_typedef(out, typedefs[i].layout, typedefs[i].prefix);
    if (typedef_count > 0)
        fprintf(out, "\n");
    fprintf(out, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");

    for (int i = 0; i < nmodules; i++) {
        if (!modules || !modules[i] || !modules[i]->init)
            continue;
        emit_c_export_header_func(ctx, out, modules[i]->init, &count);
    }
    if (count == 0)
        fprintf(out, "/* No export.c header symbols. */\n");

    fprintf(out, "\n#ifdef __cplusplus\n}\n#endif\n\n");
    fprintf(out, "#endif /* %s */\n", header_guard);
}

static void emit_c_export_c_param_storage_expr(FILE *out, const XiFunc *f, uint16_t index) {
    const XrType *type = f->params && f->params[index] ? f->params[index]->type : NULL;
    emit_cfn_c_param_storage_expr(out, type, index);
}

static void emit_c_export_target_call_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const char *prefix) {
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "(NULL");
    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    for (uint16_t i = 0; i < f->nparams; i++) {
        const XrType *pt = f->params && f->params[i] ? f->params[i]->type : NULL;
        fprintf(out, ", ");
        if (plan && i < plan->abi.nparams && plan->abi.params &&
            cg_c_export_abi_slot_is_struct_aggregate(&plan->abi.params[i])) {
            fprintf(out, "p%u", i);
            continue;
        }
        XrRep from_rep = cg_cfn_value_storage_rep(pt, false);
        XrRep to_rep = cg_func_param_abi_rep(ctx, f, i);
        const char *suffix = emit_conversion_prefix_ctx(ctx, out, pt, from_rep, to_rep);
        emit_c_export_c_param_storage_expr(out, f, i);
        emit_conversion_suffix(out, suffix);
    }
    fprintf(out, ")");
}

static void emit_c_export_stub_definition(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const char *prefix) {
    const XrType *ret_type;
    const char *ret_c_type;
    XrRep ret_rep;
    XrRep c_ret_rep;

    if (!f || !f->export_plan)
        return;
    if (!cg_func_can_have_c_export_stub(ctx, f)) {
        fprintf(stderr,
                "[xi_cgen] ERROR: export.c function '%s' must be a top-level noncapturing "
                "non-coroutine function with a supported C ABI signature\n",
                f->name ? f->name : "<anonymous>");
        ctx->error = true;
        return;
    }

    emit_c_export_stub_signature(ctx, out, f, prefix, true);
    fprintf(out, " {\n");

    ret_type = f->return_type;
    if (!ret_type || ret_type->kind == XR_KIND_UNIT) {
        fprintf(out, "    ");
        emit_c_export_target_call_expr(ctx, out, f, prefix);
        fprintf(out, ";\n");
        fprintf(out, "}\n\n");
        return;
    }

    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    if (plan && cg_c_export_abi_slot_is_struct_aggregate(&plan->abi.ret)) {
        fprintf(out, "    return ");
        emit_c_export_target_call_expr(ctx, out, f, prefix);
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
            emit_c_export_target_call_expr(ctx, out, f, prefix);
        } else {
            fprintf(out, "(%s)(uintptr_t)(", ret_c_type);
            emit_c_export_target_call_expr(ctx, out, f, prefix);
            fprintf(out, ")");
        }
    } else {
        const char *suffix;
        fprintf(out, "(%s)(", ret_c_type);
        suffix = emit_conversion_prefix_ctx(ctx, out, ret_type, ret_rep, c_ret_rep);
        emit_c_export_target_call_expr(ctx, out, f, prefix);
        emit_conversion_suffix(out, suffix);
        fprintf(out, ")");
    }
    fprintf(out, ";\n");
    fprintf(out, "}\n\n");
}

static void emit_entry_stub_definition(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                       const char *prefix) {
    const XrType *ret_type;
    const char *ret_c_type;
    XrRep ret_rep;
    XrRep c_ret_rep;

    if (!f || !f->entry_plan)
        return;
    if (!cg_func_can_have_entry_stub(ctx, f)) {
        fprintf(stderr,
                "[xi_cgen] ERROR: freestanding.entry function '%s' must be a top-level "
                "noncapturing non-coroutine function with a supported C ABI signature\n",
                f->name ? f->name : "<anonymous>");
        ctx->error = true;
        return;
    }

    emit_entry_stub_signature(ctx, out, f, prefix, true);
    fprintf(out, " {\n");

    ret_type = f->return_type;
    if (!ret_type || ret_type->kind == XR_KIND_UNIT) {
        fprintf(out, "    ");
        emit_c_export_target_call_expr(ctx, out, f, prefix);
        fprintf(out, ";\n}\n\n");
        return;
    }

    const XaotFuncPlan *plan = cg_func_plan(ctx, f);
    if (plan && cg_c_export_abi_slot_is_struct_aggregate(&plan->abi.ret)) {
        fprintf(out, "    return ");
        emit_c_export_target_call_expr(ctx, out, f, prefix);
        fprintf(out, ";\n}\n\n");
        return;
    }

    ret_c_type = cg_cfn_value_c_type(ret_type, true);
    ret_rep = cg_func_return_abi_rep(ctx, f);
    c_ret_rep = cg_cfn_value_storage_rep(ret_type, true);

    fprintf(out, "    return ");
    if (ret_type->kind == XR_KIND_POINTER) {
        if (ret_rep == XR_REP_PTR || ret_rep == XR_REP_RAWPTR) {
            fprintf(out, "(%s)", ret_c_type);
            emit_c_export_target_call_expr(ctx, out, f, prefix);
        } else {
            fprintf(out, "(%s)(uintptr_t)(", ret_c_type);
            emit_c_export_target_call_expr(ctx, out, f, prefix);
            fprintf(out, ")");
        }
    } else {
        const char *suffix;
        fprintf(out, "(%s)(", ret_c_type);
        suffix = emit_conversion_prefix_ctx(ctx, out, ret_type, ret_rep, c_ret_rep);
        emit_c_export_target_call_expr(ctx, out, f, prefix);
        emit_conversion_suffix(out, suffix);
        fprintf(out, ")");
    }
    fprintf(out, ";\n}\n\n");
}

static bool cg_func_is_module_export(const XiCgenCtx *ctx, const XiFunc *f) {
    const XiModule *mod = cg_module_for_func(ctx, f);
    if (!mod || !f)
        return false;
    for (uint16_t ei = 0; ei < mod->nexports; ei++) {
        if (mod->exports[ei].function == f)
            return true;
    }
    return false;
}

static bool cg_func_is_member_of_class_data(const XiModule *mod, const XiClassData *cd,
                                            const XiFunc *f) {
    if (!mod || !mod->init || !cd || !f)
        return false;
    if (cd->child_idx) {
        uint16_t total = (uint16_t) (cd->ninst + cd->nstat);
        if (total > cd->nmethod)
            total = cd->nmethod;
        for (uint16_t mi = 0; mi < total; mi++) {
            uint16_t child_idx = cd->child_idx[mi];
            if (child_idx < mod->init->nchildren && mod->init->children[child_idx] == f)
                return true;
        }
    }
    return cd->clinit_child_idx >= 0 && cd->clinit_child_idx < mod->init->nchildren &&
           mod->init->children[cd->clinit_child_idx] == f;
}

static bool cg_func_is_class_member(const XiCgenCtx *ctx, const XiFunc *f) {
    const XiModule *mod = cg_module_for_func(ctx, f);
    if (!mod || !mod->init || !f)
        return false;
    for (uint16_t ci = 0; ci < mod->nclasses; ci++) {
        const XiClassData *cd = mod->classes ? mod->classes[ci] : NULL;
        if (cg_func_is_member_of_class_data(mod, cd, f))
            return true;
    }
    return false;
}

static bool cg_func_is_exported_class_member(const XiCgenCtx *ctx, const XiFunc *f) {
    const XiModule *mod = cg_module_for_func(ctx, f);
    if (!mod || !f)
        return false;
    for (uint16_t ei = 0; ei < mod->nexports; ei++) {
        const XiClassData *cd = mod->exports[ei].class_data;
        if (cd && cg_func_is_member_of_class_data(mod, cd, f))
            return true;
    }
    return false;
}

static const XiFunc *cg_shared_function_slot_target_for_func(const XiCgenCtx *ctx,
                                                             const XiFunc *owner, int slot) {
    if (slot < 0)
        return NULL;
    if (owner && owner->shared_slot_funcs && slot < (int) owner->shared_slot_func_count)
        return owner->shared_slot_funcs[slot];
    const XiModule *mod = cg_module_for_func(ctx, owner);
    if (mod && mod->slot_funcs && slot < (int) mod->nslots)
        return mod->slot_funcs[slot];
    return cg_shared_function_slot_target((XiCgenCtx *) ctx, owner, slot);
}

static const XiFunc *cg_value_static_func_target(XiCgenCtx *ctx, const XiFunc *owner,
                                                 const XiValue *value) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!ctx || !v)
        return NULL;
    if ((v->op == XI_CLOSURE_NEW || (v->op == XI_STACK_ALLOC && v->aux_int == XI_CLOSURE_NEW)) &&
        v->aux)
        return (const XiFunc *) v->aux;
    if (v->op == XI_GET_SHARED)
        return cg_shared_function_slot_target_for_func(ctx, owner, (int) v->aux_int);
    if (v->op == XI_IMPORT_REF && v->aux) {
        const XiImportRef *ref = (const XiImportRef *) v->aux;
        if (ref->resolved_mod_index >= 0 && ref->resolved_mod_index < ctx->all_nmodules &&
            ref->resolved_shared_slot >= 0) {
            const XiModule *mod = ctx->all_modules[ref->resolved_mod_index];
            if (mod && mod->slot_funcs && ref->resolved_shared_slot < (int) mod->nslots &&
                mod->slot_funcs[ref->resolved_shared_slot])
                return mod->slot_funcs[ref->resolved_shared_slot];
        }
        CgStaticFunctionCall call = cg_resolve_import_function_call(ctx, ref);
        return call.is_class_constructor ? NULL : call.func;
    }
    return NULL;
}

static bool cg_parallel_op_targets_func(const XiValue *user, const XiFunc *target) {
    if (!user || !target)
        return false;
    switch ((XiOp) user->op) {
        case XI_PAR_FOR: {
            const XiParallelForData *data = (user->aux_kind == XI_AUX_KIND_PAR_FOR)
                                                ? (const XiParallelForData *) user->aux
                                                : NULL;
            return data && data->body_func == target;
        }
        case XI_PAR_MAP: {
            const XiParallelMapData *data = (user->aux_kind == XI_AUX_KIND_PAR_MAP)
                                                ? (const XiParallelMapData *) user->aux
                                                : NULL;
            return data && data->body_func == target;
        }
        case XI_PAR_REDUCE: {
            const XiParallelReduceData *data = (user->aux_kind == XI_AUX_KIND_PAR_REDUCE)
                                                   ? (const XiParallelReduceData *) user->aux
                                                   : NULL;
            return data && (data->body_func == target || data->combine_func == target);
        }
        default:
            return false;
    }
}

static bool cg_static_func_ref_use_requires_body(XiCgenCtx *ctx, const XiFunc *owner,
                                                 const XiValue *ref, const XiFunc *target,
                                                 int depth) {
    if (!ctx || !owner || !ref || !target)
        return false;
    if (depth > 16)
        return true;
    const XiModule *owner_mod = cg_module_for_func(ctx, owner);
    for (uint32_t bi = 0; bi < owner->nblocks; bi++) {
        const XiBlock *blk = owner->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            if (cg_parallel_op_targets_func(user, target))
                return true;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != ref)
                    continue;
                switch ((XiOp) user->op) {
                    case XI_CALL:
                    case XI_TAIL_CALL:
                        return true;
                    case XI_BOX:
                    case XI_UNBOX:
                    case XI_COPY:
                    case XI_SOURCE_MOVE:
                    case XI_OWNER_FORWARD:
                        if (ai != 0)
                            return true;
                        if (cg_static_func_ref_use_requires_body(ctx, owner, user, target,
                                                                 depth + 1))
                            return true;
                        break;
                    case XI_SET_SHARED:
                        if (ai != 0)
                            return true;
                        if (cg_shared_slot_has_reachable_get(ctx, owner_mod, (int) user->aux_int))
                            return true;
                        break;
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (ai != 0)
                            return true;
                        break;
                    case XI_PAR_FOR:
                    case XI_PAR_MAP:
                    case XI_PAR_REDUCE:
                        return true;
                    default:
                        return true;
                }
            }
        }
    }
    return false;
}

static bool cg_func_body_is_reachable_from_roots(XiCgenCtx *ctx, const XiFunc *target, int depth);

static bool cg_func_has_forced_body_root(XiCgenCtx *ctx, const XiFunc *f) {
    const XiModule *mod = cg_module_for_func(ctx, f);
    if (!f)
        return false;
    if (!xaot_callable_func_has_executable_body_plan(cg_ctx_aot_bundle(ctx), f))
        return false;
    if (mod && mod->init == f)
        return ctx->c_dialect != XI_CGEN_C_DIALECT_C90;
    if (f->export_plan || f->link_plan || f->entry_plan)
        return true;
    /* Hosted shared-library class descriptors are open-world ABI tables.
     * Freestanding images have no dynamic Xray module/class ABI: their public
     * surface is the explicit C/link/entry manifest handled above, so language
     * exports stay closed-world just like executable bodies. */
    if (cg_func_is_class_member(ctx, f)) {
        if (ctx->freestanding_profile)
            return false;
        return !ctx->emit_main || cg_func_is_exported_class_member(ctx, f);
    }
    /* Executables are closed-world: a language-level export is retained only
     * when a reachable import/shared-slot read consumes it. Hosted shared
     * libraries remain open-world; freestanding shared images expose only
     * manifest-owned C/link/entry symbols and therefore remain closed-world. */
    return !ctx->emit_main && !ctx->freestanding_profile && cg_func_is_module_export(ctx, f);
}

static void cg_func_reach_collect_tree(XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f)
        return;
    CgFuncReachMemo *memo = cg_func_reach_memo_entry(ctx, f, true);
    if (memo && cg_func_has_forced_body_root(ctx, f)) {
        memo->reachable = true;
        memo->state = 2;
    }
    for (uint16_t i = 0; i < f->nchildren; i++)
        cg_func_reach_collect_tree(ctx, f->children[i]);
}

static void cg_func_reach_mark_root(XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f || !xaot_callable_func_has_executable_body_plan(cg_ctx_aot_bundle(ctx), f))
        return;
    cg_func_reach_collect_tree(ctx, f);
    CgFuncReachMemo *memo = cg_func_reach_memo_entry(ctx, f, true);
    if (memo) {
        memo->reachable = true;
        memo->state = 2;
    }
}

static void cg_func_reach_mark_hash_eq_roots(XiCgenCtx *ctx) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!ctx || !bundle)
        return;
    for (uint32_t i = 0; i < bundle->nhash_eq_plans; i++) {
        const XaotHashEqPlan *plan = &bundle->hash_eq_plans[i];
        if (plan->action != XAOT_HASH_EQ_DIRECT_CALL)
            continue;
        cg_func_reach_mark_root(ctx, xaot_bundle_find_body_func(bundle, plan->hash_func_id, NULL));
        cg_func_reach_mark_root(ctx, xaot_bundle_find_body_func(bundle, plan->eq_func_id, NULL));
    }
    /* Also seed hash() / operator == for every class that declares them. The
     * runtime type table dispatches to these by class for a user-Hashable key
     * (xrt_type_set_user_hash_eq), an edge no static plan or direct call site
     * exposes, so without seeding they would be pruned and the type-table setup
     * would reference undefined symbols. */
    for (int mi = 0; mi < ctx->all_nmodules; mi++) {
        const XiModule *mod = ctx->all_modules ? ctx->all_modules[mi] : NULL;
        if (!mod || !mod->classes)
            continue;
        for (uint16_t ci = 0; ci < mod->nclasses; ci++) {
            const XiClassData *cd = mod->classes[ci];
            const XiFunc *hash_fn = cg_class_instance_method_func(ctx, cd, "hash");
            const XiFunc *eq_fn = cg_class_instance_method_func(ctx, cd, "==");
            if (hash_fn && eq_fn) {
                cg_func_reach_mark_root(ctx, hash_fn);
                cg_func_reach_mark_root(ctx, eq_fn);
            }
        }
    }
}

static void cg_func_reach_mark_dispatch_roots(XiCgenCtx *ctx) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!ctx || !bundle)
        return;
    for (uint32_t i = 0; i < bundle->nmethod_dispatch_plans; i++) {
        const XaotMethodDispatchPlan *plan = &bundle->method_dispatch_plans[i];
        if (plan->target_count == 0 || plan->target_start == 0 ||
            plan->target_start - 1 + plan->target_count > bundle->ndispatch_target_cases)
            continue;
        for (uint16_t ti = 0; ti < plan->target_count; ti++) {
            const XaotDispatchTargetCase *target =
                &bundle->dispatch_target_cases[plan->target_start - 1 + ti];
            cg_func_reach_mark_root(ctx,
                                    xaot_bundle_find_dispatch_target_func(bundle, target, NULL));
        }
    }
}

static void cg_func_reach_mark_generic_body_roots(XiCgenCtx *ctx) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!ctx || !bundle)
        return;
    for (uint32_t i = 0; i < bundle->ngeneric_body_plans; i++) {
        const XaotGenericBodyPlan *plan = &bundle->generic_body_plans[i];
        XgFuncId body_func_id = XG_NO_ID;
        switch ((XaotGenericBodyAction) plan->action) {
            case XAOT_GENERIC_BODY_CLONE:
                body_func_id = plan->specialized_body_func_id;
                break;
            case XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY:
            case XAOT_GENERIC_BODY_DIRECT_CONSTRAINT_CALL:
                body_func_id = plan->origin_body_func_id;
                break;
            case XAOT_GENERIC_BODY_REJECT:
                break;
        }
        cg_func_reach_mark_root(ctx, xaot_bundle_find_body_func(bundle, body_func_id, NULL));
    }
}

static bool cg_func_reach_mark_edge(XiCgenCtx *ctx, const XiFunc *target) {
    if (!ctx || !target ||
        !xaot_callable_func_has_executable_body_plan(cg_ctx_aot_bundle(ctx), target))
        return false;
    CgFuncReachMemo *memo = cg_func_reach_memo_entry(ctx, target, false);
    if (!memo || memo->reachable)
        return false;
    memo->reachable = true;
    memo->state = 2;
    return true;
}

static bool cg_func_reach_mark_value_edges(XiCgenCtx *ctx, const XiFunc *owner, const XiValue *v) {
    if (!ctx || !owner || !v)
        return false;
    bool changed = false;
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);

    if (v->op == XI_PAR_FOR && v->aux_kind == XI_AUX_KIND_PAR_FOR) {
        const XiParallelForData *data = (const XiParallelForData *) v->aux;
        changed |= cg_func_reach_mark_edge(ctx, data ? data->body_func : NULL);
    } else if (v->op == XI_PAR_MAP && v->aux_kind == XI_AUX_KIND_PAR_MAP) {
        const XiParallelMapData *data = (const XiParallelMapData *) v->aux;
        changed |= cg_func_reach_mark_edge(ctx, data ? data->body_func : NULL);
    } else if (v->op == XI_PAR_REDUCE && v->aux_kind == XI_AUX_KIND_PAR_REDUCE) {
        const XiParallelReduceData *data = (const XiParallelReduceData *) v->aux;
        changed |= cg_func_reach_mark_edge(ctx, data ? data->body_func : NULL);
        changed |= cg_func_reach_mark_edge(ctx, data ? data->combine_func : NULL);
    }

    if (bundle && (v->op == XI_CALL || v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT))
        changed |= cg_func_reach_mark_edge(
            ctx, xaot_boundary_resolve_direct_call_target(bundle, owner, v, NULL));

    if ((v->op == XI_CALL || v->op == XI_TAIL_CALL) && v->nargs >= 1) {
        CgStaticFunctionCall call = cg_resolve_static_function_call(ctx, owner, v->args[0]);
        changed |= cg_func_reach_mark_edge(ctx, call.func);
        const char *class_name = v->type ? xr_type_get_class_name(v->type) : NULL;
        if (class_name)
            changed |=
                cg_func_reach_mark_edge(ctx, cg_lookup_class_ctor_global(ctx, class_name, NULL));
        const XiValue *callee = cg_unwrap_identity_value(v->args[0]);
        if (callee && callee->op == XI_CLASS_CREATE && callee->aux) {
            const XiClassData *cd = (const XiClassData *) callee->aux;
            for (const XiFunc *parent = owner; parent; parent = parent->parent_func)
                changed |= cg_func_reach_mark_edge(ctx, cg_find_constructor(parent, cd));
        }
    }

    if (v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) {
        const char *method = (const char *) v->aux;
        if (method) {
            CgStaticFunctionCall module_call = cg_resolve_module_member_call(ctx, owner, v, method);
            changed |= cg_func_reach_mark_edge(ctx, module_call.func);
        }
        if (method && strcmp(method, "constructor") == 0) {
            const char *class_name = v->type ? xr_type_get_class_name(v->type) : NULL;
            if (class_name)
                changed |= cg_func_reach_mark_edge(
                    ctx, cg_lookup_class_ctor_global(ctx, class_name, NULL));
        }
        if (method && v->nargs >= 1) {
            const XiValue *receiver = cg_unwrap_identity_value(v->args[0]);
            const XiModule *owner_mod = cg_module_for_func(ctx, owner);
            const XiClassData *receiver_class = NULL;
            if (receiver && receiver->op == XI_GET_SHARED && owner_mod && owner_mod->slot_classes &&
                receiver->aux_int >= 0 && receiver->aux_int < owner_mod->nslots)
                receiver_class = owner_mod->slot_classes[receiver->aux_int];
            if (!receiver_class)
                receiver_class = cg_class_native_class_value_data(ctx, owner, receiver);
            if (receiver_class && receiver_class->methods && receiver_class->child_idx) {
                const XiModule *class_mod = cg_class_native_module_for_data(ctx, receiver_class);
                if (class_mod && class_mod->init) {
                    for (uint16_t mi = 0; mi < receiver_class->nmethod; mi++) {
                        const XiClassMethod *candidate = &receiver_class->methods[mi];
                        if (!candidate->is_static || candidate->is_static_constructor ||
                            !candidate->name || strcmp(candidate->name, method) != 0)
                            continue;
                        uint16_t child_idx = receiver_class->child_idx[mi];
                        if (child_idx < class_mod->init->nchildren)
                            changed |=
                                cg_func_reach_mark_edge(ctx, class_mod->init->children[child_idx]);
                    }
                }
            }
        }
        changed |=
            cg_func_reach_mark_edge(ctx, cg_class_native_resolve_method_call(ctx, owner, v, NULL));
    }

    const XiFunc *ref_target = cg_value_static_func_target(ctx, owner, v);
    if (ref_target && cg_static_func_ref_use_requires_body(ctx, owner, v, ref_target, 0))
        changed |= cg_func_reach_mark_edge(ctx, ref_target);
    return changed;
}

static bool cg_func_reach_mark_body_edges(XiCgenCtx *ctx, const XiFunc *source) {
    if (!ctx || !source)
        return false;
    bool changed = false;
    for (uint32_t bi = 0; bi < source->nblocks; bi++) {
        const XiBlock *block = source->blocks[bi];
        if (!block)
            continue;
        changed |=
            cg_func_reach_mark_edge(ctx, cg_value_static_func_target(ctx, source, block->control));
        for (uint32_t vi = 0; vi < block->nvalues; vi++)
            changed |= cg_func_reach_mark_value_edges(ctx, source, block->values[vi]);
    }
    return changed;
}

static bool cg_func_reach_mark_body_edges_in_owner_module(XiCgenCtx *ctx, const XiFunc *source) {
    if (!ctx || !source)
        return false;
    const XiModule *owner = cg_module_for_func(ctx, source);
    if (!owner || owner == ctx->module)
        return cg_func_reach_mark_body_edges(ctx, source);

    CgModuleScanSnapshot snap;
    if (!cg_module_scan_snapshot_save(ctx, &snap)) {
        cg_module_scan_snapshot_free(&snap);
        return false;
    }
    cg_init_from_module(ctx, (XiModule *) owner);
    cg_register_imported_classes(ctx);
    bool changed = cg_func_reach_mark_body_edges(ctx, source);
    cg_module_scan_snapshot_restore(ctx, &snap);
    return changed;
}

/* Compute executable function reachability as a monotonic fixed point over the
 * resolved call/reference graph.  The previous per-target recursive query
 * cached provisional negative answers while callers were still in the
 * visiting state; later-discovered root paths could therefore leave a live C
 * call with its callee body pruned. */
static void cg_func_reachability_compute(XiCgenCtx *ctx) {
    if (!ctx || ctx->func_reachability_valid || ctx->func_reachability_computing)
        return;

    ctx->func_reachability_computing = true;
    ctx->nfunc_reach_memo = 0;
    if (ctx->all_modules && ctx->all_nmodules > 0) {
        for (int i = 0; i < ctx->all_nmodules; i++) {
            const XiModule *mod = ctx->all_modules[i];
            if (mod && mod->init)
                cg_func_reach_collect_tree(ctx, mod->init);
        }
    } else if (ctx->module && ctx->module->init) {
        cg_func_reach_collect_tree(ctx, ctx->module->init);
    }
    cg_func_reach_mark_hash_eq_roots(ctx);
    cg_func_reach_mark_dispatch_roots(ctx);
    cg_func_reach_mark_generic_body_roots(ctx);

    bool changed;
    do {
        changed = false;
        /* Shared-slot reachability depends on the current live-function set;
         * discard its provisional cache on each fixed-point round. */
        ctx->nshared_slot_reach_memo = 0;
        for (int si = 0; si < ctx->nfunc_reach_memo; si++) {
            const XiFunc *source = ctx->func_reach_memo[si].func;
            if (!ctx->func_reach_memo[si].reachable)
                continue;
            changed |= cg_func_reach_mark_body_edges_in_owner_module(ctx, source);
        }
    } while (changed);

    for (int i = 0; i < ctx->nfunc_reach_memo; i++)
        ctx->func_reach_memo[i].state = 2;
    ctx->func_reachability_computing = false;
    ctx->func_reachability_valid = true;
}

static bool cg_func_body_is_reachable_from_roots(XiCgenCtx *ctx, const XiFunc *target, int depth) {
    (void) depth;
    if (!ctx || !target)
        return false;
    if (!xaot_callable_func_has_executable_body_plan(cg_ctx_aot_bundle(ctx), target))
        return false;
    if (!ctx->func_reachability_valid && !ctx->func_reachability_computing)
        cg_func_reachability_compute(ctx);
    CgFuncReachMemo *memo = cg_func_reach_memo_entry(ctx, target, false);
    return memo && memo->reachable;
}

static void cg_report_mandatory_plan_contract_failure(XiCgenCtx *ctx, const XiFunc *func,
                                                      const XiValue *value, const char *plan_kind,
                                                      uint32_t stable_id,
                                                      XaotBackendContractIssue issue) {
    cg_ctx_set_error(ctx);
    fprintf(stderr,
            "[xi_cgen] ERROR: %s plan contract failed in function '%s' for Xi value v%u "
            "(stable_id=%u span=%u issue=%s)\n",
            plan_kind, func && func->name ? func->name : "?", value ? value->id : 0, stable_id,
            value ? value->line : 0, xaot_backend_contract_issue_name(issue));
}

static bool cg_json_codec_site_contract(const XiValue *value, uint8_t *out_kind,
                                        uint32_t *out_allowed_actions) {
    uint8_t kind = 0;
    uint32_t actions = 0;
    if (!value || !out_kind || !out_allowed_actions)
        return false;
    if (value->op == XI_JSON_DECODE) {
        kind = XG_JSON_CODEC_DECODE;
        actions = xaot_backend_json_codec_action_bit(XAOT_JSON_CODEC_DECODE_VALIDATE_COPY);
    } else if (value->op == XI_CALL_METHOD && value->nargs >= 1 && value->aux &&
               xicgen_receiver_is_builtin_global(value->args[0], XR_GLOBAL_VAR_JSON)) {
        const char *method = (const char *) value->aux;
        if (strcmp(method, "parse") == 0) {
            kind = XG_JSON_CODEC_PARSE;
            actions = xaot_backend_json_codec_action_bit(XAOT_JSON_CODEC_PARSE_RUNTIME_DIRECT);
        } else if (strcmp(method, "encode") == 0) {
            kind = XG_JSON_CODEC_ENCODE;
            actions = xaot_backend_json_codec_action_bit(XAOT_JSON_CODEC_ENCODE_FIELD_TABLE) |
                      xaot_backend_json_codec_action_bit(XAOT_JSON_CODEC_ENCODE_DERIVE_SIDECAR);
        } else if (strcmp(method, "stringify") == 0) {
            kind = XG_JSON_CODEC_STRINGIFY;
            actions = xaot_backend_json_codec_action_bit(XAOT_JSON_CODEC_STRINGIFY_DYNAMIC_WALK);
        }
    }
    if (kind == 0 || actions == 0)
        return false;
    *out_kind = kind;
    *out_allowed_actions = actions;
    return true;
}

static bool cg_mandatory_plans_preflight_value(XiCgenCtx *ctx, const XiFunc *func,
                                               const XiValue *value) {
    if (!value)
        return true;

    if (value->op == XI_IMPORT_REF &&
        !cg_import_ref_has_aot_resolution(ctx, func, value, cg_value_import_ref(value))) {
        const XiImportRef *ref = cg_value_import_ref(value);
        cg_ctx_set_error(ctx);
        fprintf(stderr,
                "[xi_cgen] ERROR: unresolved AOT import '%s.%s' in function '%s' for Xi value "
                "v%u\n",
                ref && ref->module_path ? ref->module_path : "?",
                ref && ref->member_name ? ref->member_name : "?",
                func && func->name ? func->name : "?", value->id);
        return false;
    }

    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    bool valid = true;

    uint8_t expected_kind = 0;
    uint32_t allowed_actions = 0;
    bool json_codec_site = cg_json_codec_site_contract(value, &expected_kind, &allowed_actions);
    if (json_codec_site) {
        if (value->xg_json_codec_id == XG_NO_ID) {
            issue = XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN;
            cg_report_mandatory_plan_contract_failure(ctx, func, value, "json-codec", XG_NO_ID,
                                                      issue);
            return false;
        }
        const XaotJsonCodecPlan *plan =
            xaot_bundle_find_json_codec_plan(bundle, value->xg_json_codec_id);
        if (!xaot_backend_contract_json_codec_plan_allowed(plan, expected_kind, allowed_actions,
                                                           &issue)) {
            cg_report_mandatory_plan_contract_failure(ctx, func, value, "json-codec",
                                                      value->xg_json_codec_id, issue);
            valid = false;
        }
    } else if (value->xg_json_codec_id != XG_NO_ID) {
        issue = XAOT_BACKEND_CONTRACT_MANDATORY_PLAN_IDENTITY_MISMATCH;
        cg_report_mandatory_plan_contract_failure(ctx, func, value, "json-codec",
                                                  value->xg_json_codec_id, issue);
        valid = false;
    }

    const bool record_merge_site = value->op == XI_JSON_MERGE && value->nargs >= 2 &&
                                   value->args[0] && value->args[0]->type &&
                                   value->args[0]->type->kind == XR_KIND_RECORD;
    const uint32_t record_merge_actions =
        xaot_backend_record_merge_action_bit(XAOT_RECORD_MERGE_COPY_WITH_OVERWRITE) |
        xaot_backend_record_merge_action_bit(XAOT_RECORD_MERGE_COPY_APPEND);
    if (record_merge_site) {
        if (value->xg_record_merge_id == XG_NO_ID) {
            issue = XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN;
            cg_report_mandatory_plan_contract_failure(ctx, func, value, "record-merge", XG_NO_ID,
                                                      issue);
            return false;
        }
        const XaotRecordMergePlan *plan =
            xaot_bundle_find_record_merge_plan(bundle, value->xg_record_merge_id);
        if (!xaot_backend_contract_record_merge_plan_allowed(plan, record_merge_actions, &issue)) {
            cg_report_mandatory_plan_contract_failure(ctx, func, value, "record-merge",
                                                      value->xg_record_merge_id, issue);
            valid = false;
        }
    } else if (value->xg_record_merge_id != XG_NO_ID) {
        issue = XAOT_BACKEND_CONTRACT_MANDATORY_PLAN_IDENTITY_MISMATCH;
        cg_report_mandatory_plan_contract_failure(ctx, func, value, "record-merge",
                                                  value->xg_record_merge_id, issue);
        valid = false;
    }
    return valid;
}

static bool cg_mandatory_plans_preflight_func_tree(XiCgenCtx *ctx, const XiFunc *func) {
    if (!func)
        return true;

    bool valid = true;
    for (uint16_t ci = 0; ci < func->nchildren; ci++) {
        if (!cg_mandatory_plans_preflight_func_tree(ctx, func->children[ci]))
            valid = false;
    }
    if (func->is_extern || !cg_func_body_is_reachable_from_roots(ctx, func, 0))
        return valid;

    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (!cg_mandatory_plans_preflight_value(ctx, func, &phi->value))
                valid = false;
        }
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            if (!cg_mandatory_plans_preflight_value(ctx, func, block->values[vi]))
                valid = false;
        }
    }
    return valid;
}

static void xi_cgen_func(XiCgenCtx *ctx, FILE *out, XiFunc *f, const char *prefix) {
    XR_DCHECK(out != NULL, "xi_cgen_func: NULL output");
    XR_DCHECK(f != NULL, "xi_cgen_func: NULL func");
    if (!cg_mark_func_emitted(ctx, f, prefix))
        return;
    /* Emit nested children first. A parent function can become unreachable
     * after inlining while one of its nested closure bodies is still referenced
     * from reachable code. */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_cgen_func(ctx, out, f->children[i], prefix);
    }
    /* FFI: extern functions have no Xray definition. Only the `extern Ret
     * sym(...)` forward declaration is emitted (see emit_one_forward_decl);
     * call sites emit a direct C call. Never emit a body. */
    if (f->is_extern)
        return;
    if (!cg_func_body_is_reachable_from_roots(ctx, f, 0))
        return;
    bool error_before_function = ctx->error;
    xicgen_emit_par_for_range_wrappers(ctx, out, f, prefix);
    xicgen_emit_par_map_range_wrappers(ctx, out, f, prefix);
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
        char *coro_buf = NULL;
        size_t coro_sz = 0;
        FILE *coro_cap = ctx->want_residue ? xr_open_memstream(&coro_buf, &coro_sz) : NULL;
        xi_cgen_coro_func(ctx, coro_cap ? coro_cap : out, f, prefix);
        if (coro_cap) {
            xr_close_memstream(coro_cap, &coro_buf, &coro_sz);
            cg_scan_function_residue(ctx, f, coro_buf ? coro_buf : "");
            if (coro_buf)
                fwrite(coro_buf, 1, coro_sz, out);
            xr_free(coro_buf);
        }
        if (!error_before_function && ctx->error)
            fprintf(stderr, "[xi_cgen] ERROR: C generation failed in coroutine '%s'\n",
                    f->name ? f->name : "?");
        return;
    }

    /* Capture the core function body (signature .. closing brace) into a scratch
     * stream so the residue scanner sees exactly this function's C, matching the
     * port shape gate's per-function extraction.  Adapters/stubs emitted after
     * the body go straight to the real stream and are not part of the core
     * function's residue. */
    FILE *body_real_out = out;
    char *body_cap_buf = NULL;
    size_t body_cap_sz = 0;
    FILE *body_cap = ctx->want_residue ? xr_open_memstream(&body_cap_buf, &body_cap_sz) : NULL;
    if (body_cap)
        out = body_cap;

    /* Function signature.  Closure children with captures receive a hidden
     * first parameter xrt_closure_t *_cl for per-closure upvalue access. A
     * vararg function carries one extra trailing Array<T> parameter (the rest
     * slot); direct call sites collect the variadic arguments into it. */
    bool has_cl = (f->ncaptures > 0);
    uint16_t sig_nparams = (uint16_t) (f->nparams + (f->is_vararg ? 1 : 0));
    fprintf(out, "%s", cg_func_linkage(ctx, f, prefix));
    if (cg_func_attrs_apply_to_internal(f))
        emit_aot_symbol_attrs(out, f, false);
    emit_func_target_qualifier(ctx, out, f);
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
    if (!has_cl && ctx->c_dialect != XI_CGEN_C_DIALECT_C90)
        fprintf(out, "    (void)_cl;\n");

    /* Keep non-linear control flow valid for both C and C++ compilation: a C++
     * jump may not bypass a local declaration.  Straight-line functions retain
     * declaration-at-definition code shape.  This must be decided before the
     * storage-planning helpers because they mirror declaration versus assignment
     * choices made by emit_value_stmt. */
    ctx->pre_decl_all =
        ctx->c_dialect == XI_CGEN_C_DIALECT_C90 || f->nblocks > 1 || cg_has_exception_handling(f);
    cg_prepare_cell_vars(ctx, f);
    cg_build_phi_coalesce(ctx, f);
    cg_class_field_cache_collect(ctx, f);
    emit_declarations(ctx, out, f);
    if (ctx->c_dialect != XI_CGEN_C_DIALECT_C90)
        emit_debug_source_var_declarations(ctx, out, f);
    emit_class_field_cache_decls(ctx, out);
    if (!has_cl && ctx->c_dialect == XI_CGEN_C_DIALECT_C90)
        fprintf(out, "    (void)_cl;\n");

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

    /* End core-body capture: scan for residue, then flush it through. */
    if (body_cap) {
        xr_close_memstream(body_cap, &body_cap_buf, &body_cap_sz);
        cg_scan_function_residue(ctx, f, body_cap_buf ? body_cap_buf : "");
        if (body_cap_buf)
            fwrite(body_cap_buf, 1, body_cap_sz, body_real_out);
        xr_free(body_cap_buf);
        out = body_real_out;
    }

    emit_cfn_stub_definition(ctx, out, f, prefix);
    emit_c_export_stub_definition(ctx, out, f, prefix);
    emit_entry_stub_definition(ctx, out, f, prefix);

    if (native_receiver && boxed_adapter) {
        ctx->stats.boxed_adapters++;
        emit_class_native_boxed_adapter(ctx, out, prefix, f);
    } else if (boxed_adapter) {
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
            XaotValueRep ret_value_rep = cg_func_return_abi_value_rep(ctx, f);
            bool ret_is_struct_aggregate =
                ret_is_aggregate && cg_func_return_abi_is_struct_aggregate(ctx, f);
            if (ret_is_struct_aggregate) {
                fprintf(out, "    %s _ret = ", cg_func_return_abi_c_type(ctx, f));
                emit_fname(ctx, out, prefix, f);
                fprintf(out, "(_cl");
                for (uint16_t i = 0; i < boxed_total; i++) {
                    fprintf(out, ", ");
                    char param_expr[32];
                    snprintf(param_expr, sizeof(param_expr), "p%u", i);
                    emit_boxed_value_as_func_param_abi(ctx, out, f, i, param_expr);
                }
                fprintf(out, ");\n    return ");
                if (!emit_struct_aggregate_box_c_expr(ctx, out, f, "_ret", prefix)) {
                    fprintf(stderr, "[xi_cgen] ERROR: cannot box struct aggregate return '%s'\n",
                            f->name ? f->name : "?");
                    ctx->error = true;
                    fprintf(out, "XR_NULL_VAL");
                }
                fprintf(out, ";\n}\n\n");
                goto boxed_adapter_done;
            }
            if (ret_rep == XR_REP_VOID) {
                fprintf(out, "    ");
            } else {
                fprintf(out, "    return ");
            }
            const char *conv_suffix = NULL;
            if (ret_is_aggregate && cg_value_rep_is_span_aggregate(ret_value_rep)) {
                fprintf(out, "xrt_span_box_value(");
            } else if (ret_is_aggregate) {
                fprintf(out, "xrt_enum_aggregate_box(");
                if (cg_value_rep_is_typed_adt_aggregate(ret_value_rep))
                    fprintf(out, "%s_to_base(", ret_value_rep.c_type);
            } else if (ret_rep != XR_REP_VOID)
                conv_suffix =
                    emit_conversion_prefix_ctx(ctx, out, f->return_type, ret_rep, XR_REP_TAGGED);
            emit_fname(ctx, out, prefix, f);
            fprintf(out, "(_cl");
            for (uint16_t i = 0; i < boxed_total; i++) {
                fprintf(out, ", ");
                char param_expr[32];
                snprintf(param_expr, sizeof(param_expr), "p%u", i);
                emit_boxed_value_as_func_param_abi(ctx, out, f, i, param_expr);
            }
            fprintf(out, ")");
            if (ret_is_aggregate && cg_value_rep_is_span_aggregate(ret_value_rep)) {
                fprintf(out, ")");
            } else if (ret_is_aggregate) {
                if (cg_value_rep_is_typed_adt_aggregate(ret_value_rep))
                    fprintf(out, ")");
                fprintf(out, ")");
            } else if (ret_rep != XR_REP_VOID)
                emit_conversion_suffix(out, conv_suffix);
            fprintf(out, ";\n");
            if (ret_rep == XR_REP_VOID)
                fprintf(out, "    return XR_NULL_VAL;\n");
            fprintf(out, "}\n\n");
        }
    }

boxed_adapter_done:
    if (cg_func_needs_sync_go_wrapper_ctx(ctx, f)) {
        ctx->stats.sync_go_wrappers++;
        emit_sync_go_wrapper(ctx, out, f, prefix);
    }
    if (!error_before_function && ctx->error)
        fprintf(stderr, "[xi_cgen] ERROR: C generation failed in function '%s'\n",
                f->name ? f->name : "?");
}

/* ========== Forward Declarations ========== */

static void emit_canonical_extern_decl(XiCgenCtx *ctx, FILE *out, const XaotExternDecl *decl) {
    if (!ctx || !out || !decl || ctx->writer.phase != CG_WRITER_PHASE_EXTERN_DECLS ||
        ctx->writer.out != out) {
        if (ctx) {
            ctx->error = true;
            ctx->writer.error = true;
        }
        return;
    }
    const char *ret_ptr = cg_extern_ptr_boundary_c_type(decl->ret_type);
    fprintf(out, "extern ");
    if ((decl->attributes & XAOT_EXTERN_ATTR_NAKED) != 0)
        fprintf(out, "XRT_ATTR_NAKED ");
    if ((decl->attributes & XAOT_EXTERN_ATTR_INTERRUPT) != 0) {
        fprintf(out, "XRT_ATTR_INTERRUPT(");
        emit_c_string_literal(out, decl->interrupt_abi);
        fprintf(out, ") ");
    }
    if (ret_ptr)
        fprintf(out, "%s", ret_ptr);
    else
        fprintf(out, "%s", decl->ret.c_type ? decl->ret.c_type : "void");
    fprintf(out, " xr_ffi_%s(", decl->link_symbol);
    if (decl->nparams == 0) {
        fprintf(out, "void");
    } else {
        for (uint16_t i = 0; i < decl->nparams; i++) {
            const XrType *type = decl->param_types ? decl->param_types[i] : NULL;
            const char *ptr_type = cg_extern_ptr_boundary_c_type(type);
            if (i > 0)
                fprintf(out, ", ");
            if (cg_type_is_c_callback(type))
                emit_cfn_pointer_type(ctx, out, type, NULL);
            else if (ptr_type)
                fprintf(out, "%s", ptr_type);
            else
                fprintf(out, "%s", decl->params[i].c_type ? decl->params[i].c_type : "XrValue");
        }
    }
    fprintf(out, ") __asm__(XR_FFI_ASMNAME(\"%s\"));\n", decl->link_symbol);
}

static void emit_canonical_extern_decls(XiCgenCtx *ctx, FILE *out) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!cg_writer_enter(ctx, out, CG_WRITER_PHASE_EXTERN_DECLS) || !bundle)
        return;
    for (uint32_t i = 0; i < bundle->nextern_decls; i++) {
        if (i < ctx->used_extern_decl_cap && ctx->used_extern_decls[i])
            emit_canonical_extern_decl(ctx, out, &bundle->extern_decls[i]);
    }
}

static void emit_extern_closure_adapter_decls(XiCgenCtx *ctx, FILE *out) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!ctx || !out || !bundle || ctx->writer.phase != CG_WRITER_PHASE_INTERNAL_DECLS ||
        ctx->writer.out != out) {
        if (ctx) {
            ctx->error = true;
            ctx->writer.error = true;
        }
        return;
    }
    for (uint32_t i = 0; i < bundle->nextern_decls; i++) {
        const XaotExternDecl *decl = &bundle->extern_decls[i];
        if (i >= ctx->used_extern_decl_cap || !ctx->extern_decl_adapters[i])
            continue;
        fprintf(out, "static XrValue xr_ffi_closure_%u(xrt_closure_t *_cl", decl->stable_id);
        for (uint16_t pi = 0; pi < decl->nparams; pi++)
            fprintf(out, ", XrValue p%u", pi);
        fprintf(out, ");\n");
    }
}

static void emit_extern_closure_adapter_defs(XiCgenCtx *ctx, FILE *out) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!ctx || !out || !bundle || ctx->writer.phase != CG_WRITER_PHASE_BODIES ||
        ctx->writer.out != out) {
        if (ctx) {
            ctx->error = true;
            ctx->writer.error = true;
        }
        return;
    }
    for (uint32_t i = 0; i < bundle->nextern_decls; i++) {
        const XaotExternDecl *decl = &bundle->extern_decls[i];
        if (i >= ctx->used_extern_decl_cap || !ctx->extern_decl_adapters[i])
            continue;
        fprintf(out, "static XrValue xr_ffi_closure_%u(xrt_closure_t *_cl", decl->stable_id);
        for (uint16_t pi = 0; pi < decl->nparams; pi++)
            fprintf(out, ", XrValue p%u", pi);
        fprintf(out, ") {\n    (void)_cl;\n    ");

        XrRep ret_rep = xaot_value_storage_rep(xaot_abi_slot_value_rep(&decl->ret));
        if (ret_rep != XR_REP_VOID)
            fprintf(out, "return ");
        const char *ret_suffix =
            emit_conversion_prefix_ctx(ctx, out, decl->ret_type, ret_rep, XR_REP_TAGGED);
        fprintf(out, "xr_ffi_%s(", decl->link_symbol);
        for (uint16_t pi = 0; pi < decl->nparams; pi++) {
            if (pi > 0)
                fprintf(out, ", ");
            char param_expr[32];
            snprintf(param_expr, sizeof(param_expr), "p%u", pi);
            emit_boxed_value_as_func_param_abi(ctx, out, decl->representative_func, pi, param_expr);
        }
        fprintf(out, ")");
        emit_conversion_suffix(out, ret_suffix);
        fprintf(out, ";\n");
        if (ret_rep == XR_REP_VOID)
            fprintf(out, "    return XR_NULL_VAL;\n");
        fprintf(out, "}\n\n");
    }
}

/* Adapter bodies are assembled after the translation unit's static-data
 * phase, but a unit-enum return converted to the boxed closure ABI needs its
 * immutable scalar-layout sidecar declared in that earlier phase.  Pre-mark
 * exactly those late conversions once reachability has selected the adapters. */
static void cg_mark_extern_adapter_enum_scalar_sidecars(XiCgenCtx *ctx) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!ctx || !bundle)
        return;
    for (uint32_t i = 0; i < bundle->nextern_decls; i++) {
        const XaotExternDecl *decl = &bundle->extern_decls[i];
        if (i >= ctx->used_extern_decl_cap || !ctx->extern_decl_adapters[i])
            continue;
        XrRep ret_rep = xaot_value_storage_rep(xaot_abi_slot_value_rep(&decl->ret));
        if (ret_rep != XR_REP_I64)
            continue;
        const XaotEnumPlan *plan = cg_unit_enum_scalar_plan(ctx, decl->ret_type);
        if (plan)
            (void) cg_mark_enum_scalar_sidecar(ctx, plan, NULL);
    }
}

/* Emit the forward declaration(s) for a single function (no recursion): the
 * function prototype plus any boxed adapter / coroutine frame declarations it
 * needs.  Used both for a unit's own functions (via emit_forward_decls) and for
 * the imported cross-module functions a unit references (114). */
static void emit_one_forward_decl(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix,
                                  bool cross_module) {
    /* Extern declarations have a dedicated registry phase.  Reaching this
     * internal-forward path for one is harmless but must never re-emit or
     * re-derive a second declaration. */
    if (f->is_extern)
        return;
    bool needs_aot_coro = cg_func_needs_aot_coro_ctx(ctx, f);
    /* Coroutine functions are emitted (definition) with file-static linkage by
     * the coro codegen, so their forward declaration must match; only plain
     * functions participate in cross-module external linkage. */
    if (!needs_aot_coro) {
        fprintf(out, "%s", cg_func_forward_linkage(ctx, f, prefix, cross_module));
        if (cg_func_attrs_apply_to_internal(f))
            emit_aot_symbol_attrs(out, f, false);
        emit_func_target_qualifier(ctx, out, f);
        emit_func_attr_qualifier(ctx, out, f);
        if (!emit_class_native_return_type(ctx, out, prefix, f))
            fprintf(out, "%s", cg_func_return_abi_c_type(ctx, f));
        fprintf(out, " ");
        emit_fname(ctx, out, prefix, f);
        fprintf(out, "(xrt_closure_t *_cl");
        uint16_t fwd_nparams = (uint16_t) (f->nparams + (f->is_vararg ? 1 : 0));
        for (uint16_t i = 0; i < fwd_nparams; i++) {
            fprintf(out, ", ");
            emit_class_native_param_decl(ctx, out, prefix, f, i);
        }
        fprintf(out, ");\n");
    }

    if (cg_func_can_have_cfn_stub(ctx, f)) {
        emit_cfn_stub_signature(ctx, out, f, prefix, cross_module);
        fprintf(out, ";\n");
    }
    if (cg_func_can_have_c_export_stub(ctx, f)) {
        emit_c_export_stub_signature(ctx, out, f, prefix, true);
        fprintf(out, ";\n");
    }
    if (cg_func_can_have_entry_stub(ctx, f)) {
        emit_entry_stub_signature(ctx, out, f, prefix, false);
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
            if (cg_func_frame_needs_cl(f) || cg_coro_param_count(f) > 0)
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
    if (cg_func_body_is_reachable_from_roots(ctx, f, 0))
        emit_one_forward_decl(ctx, out, f, prefix, false);
}

#include "xi_cgen_import_helpers.inc.c"
#include "xi_cgen_stdlib_helpers.inc.c"

static bool cg_aot_stdlib_receiver_call_is_direct(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *call) {
    if (!ctx || !call || (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs < 1 || !call->aux)
        return false;
    const char *module = cg_aot_stdlib_module_of_receiver(ctx, f, call->args[0]);
    return (module && cg_find_aot_stdlib_method(module, (const char *) call->aux,
                                                (uint16_t) (call->nargs - 1)) != NULL) ||
           cg_time_module_helper_ctx(ctx, f, call) != NULL;
}

static bool cg_aot_stdlib_import_call_is_direct(XiCgenCtx *ctx, const XiFunc *f,
                                                const XiValue *call) {
    if (!ctx || !f || !call || call->op != XI_CALL || call->nargs < 1)
        return false;
    const XiValue *callee = cg_unwrap_identity_value(call->args[0]);
    const XiImportRef *ref = (callee && callee->op == XI_IMPORT_REF && callee->aux)
                                 ? (const XiImportRef *) callee->aux
                                 : NULL;
    if (!ref || !ref->module_path || !ref->member_name)
        return false;
    return cg_find_aot_stdlib_method(ref->module_path, ref->member_name,
                                     (uint16_t) (call->nargs - 1)) != NULL;
}

#include "xi_cgen_program_entry.inc.c"
