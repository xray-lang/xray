/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_pipeline.c - Unified Xi IR compilation pipeline
 *
 * Orchestrates: AST -> canon -> xi_lower -> xi_verify -> xi_opt -> xi_emit -> XrProto
 */

#include "xi_pipeline.h"
#include "../base/xglobal_indices.h"
#include "xi_semantic_snapshot.h"
#include "../frontend/analyzer/xa_typed_program.h"
#include "../frontend/analyzer/xanalyzer_builtins.h"
#include "xi_lower.h"
#include "xi_verify.h"
#include "xi_opt.h"
#include "xi_pass.h"
#include "xi_emit.h"
#include "xi_backend_lower.h"
#include "xi_escape.h"
#include "xi_own.h"
#include "xi_arc.h"
#include "xi_arc_verify.h"
#include "xi_import_resolve.h"
#include "xi_coro_analyze.h"
#include "xi_source_move_verify.h"
#include "xi_stage.h"
#include "xi_value_query.h"
#include "xi_module.h"
#include "../analysis/xglobal_summary.h"
#include "../plan/semantic/xr_semantic_builder.h"
#include "../frontend/canonical/xcanon.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/parser/xast.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/value/xchunk.h"
#include "../toolchain/xcompiler_session.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static bool xi_env_is_enabled(const char *name) {
    const char *env = getenv(name);
    return env && env[0] == '1' && env[1] == '\0';
}

/* Debug helper: dump ownership analysis for a function and all its nested
 * children (methods, closures). Gated by XRAY_XI_OWN_DUMP=1. */
static void xi_own_dump_recursive(XiFunc *f) {
    if (!f)
        return;
    XiOwnResult own;
    if (xi_own_analyze(f, &own)) {
        xi_own_dump(f, &own);
        xi_own_free(&own);
    }
    for (uint16_t i = 0; i < f->nchildren; i++)
        xi_own_dump_recursive(f->children[i]);
}

/* Sum RC ops that survive xi_arc_elim across a function tree. This is the
 * backend-independent static dup/drop budget consumed by regression gates. */
static void xi_count_rc_ops_recursive(const XiFunc *f, uint64_t *retain, uint64_t *release) {
    if (!f)
        return;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->op == XI_RETAIN)
                (*retain)++;
            else if (v->op == XI_RELEASE)
                (*release)++;
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++)
        xi_count_rc_ops_recursive(f->children[i], retain, release);
}

static void xi_set_source_file_recursive(XiFunc *f, const char *source_file) {
    if (!f)
        return;
    f->source_file = source_file;
    for (uint16_t i = 0; i < f->nchildren; i++)
        xi_set_source_file_recursive(f->children[i], source_file);
}

typedef struct XiPipelineCoroResolverCtx {
    const XiPipelineConfig *cfg;
    XiFunc *root;
} XiPipelineCoroResolverCtx;

static const XiFunc *xi_pipeline_coro_find_func_tree(const XiFunc *func, XgFuncId func_id) {
    if (!func || func_id == XG_NO_ID)
        return NULL;
    if ((XgFuncId) func->xg_body_func_id == func_id)
        return func;
    for (uint16_t i = 0; i < func->nchildren; i++) {
        const XiFunc *match = xi_pipeline_coro_find_func_tree(func->children[i], func_id);
        if (match)
            return match;
    }
    return NULL;
}

static const XiFunc *xi_pipeline_coro_find_func(XiPipelineCoroResolverCtx *ctx,
                                                XgFuncId func_id) {
    const XiFunc *match;
    if (!ctx || func_id == XG_NO_ID)
        return NULL;
    match = xi_pipeline_coro_find_func_tree(ctx->root, func_id);
    if (match)
        return match;
    if (!ctx->cfg || !ctx->cfg->graph_modules)
        return NULL;
    for (int i = 0; i < ctx->cfg->graph_module_count; i++) {
        const XiModule *module = ctx->cfg->graph_modules[i];
        match = module ? xi_pipeline_coro_find_func_tree(module->init, func_id) : NULL;
        if (match)
            return match;
    }
    return NULL;
}

static const XiFunc *xi_pipeline_coro_class_method(const XiModule *module,
                                                    const XiClassData *class_data,
                                                    const char *member, bool expect_static) {
    if (!module || !module->init || !class_data || !class_data->methods ||
        !class_data->child_idx || !member)
        return NULL;
    for (uint16_t method_index = 0; method_index < class_data->nmethod; method_index++) {
        const XiClassMethod *method = &class_data->methods[method_index];
        if (method->is_static != expect_static || !method->name ||
            strcmp(method->name, member) != 0)
            continue;
        uint16_t child_index = class_data->child_idx[method_index];
        return child_index < module->init->nchildren ? module->init->children[child_index] : NULL;
    }
    return NULL;
}

static const XiClassData *xi_pipeline_coro_current_method_class(
    const XiPipelineCoroResolverCtx *ctx, const XiFunc *current,
    const XiModule **owner_module) {
    if (owner_module)
        *owner_module = NULL;
    if (!ctx || !ctx->cfg || !ctx->cfg->graph_modules || !current)
        return NULL;
    for (int module_index = 0; module_index < ctx->cfg->graph_module_count; module_index++) {
        const XiModule *module = ctx->cfg->graph_modules[module_index];
        if (!module || !module->init)
            continue;
        for (uint16_t class_index = 0; class_index < module->nclasses; class_index++) {
            const XiClassData *class_data = module->classes[class_index];
            if (!class_data || !class_data->methods || !class_data->child_idx)
                continue;
            for (uint16_t method_index = 0; method_index < class_data->nmethod;
                 method_index++) {
                uint16_t child_index = class_data->child_idx[method_index];
                if (child_index >= module->init->nchildren ||
                    module->init->children[child_index] != current)
                    continue;
                if (owner_module)
                    *owner_module = module;
                return class_data;
            }
        }
    }
    return NULL;
}

static const XiFunc *xi_pipeline_coro_resolve_callee(void *ud, const XiFunc *current,
                                                      const XiValue *callee) {
    XiPipelineCoroResolverCtx *ctx = (XiPipelineCoroResolverCtx *) ud;
    const XiImportRef *ref = xi_value_import_ref(current, callee);
    if (ref && ref->resolved_func)
        return ref->resolved_func;
    if (!ctx || !ctx->cfg || !ctx->cfg->global_evidence)
        return NULL;
    /* Ordinary calls carry their stable callsite on the call, not on the
     * callee value.  Local closure/shared-slot targets are handled by the
     * IR analysis itself; evidence-only targets are resolved per call below. */
    return NULL;
}

static const XgCallsiteSummary *xi_pipeline_coro_callsite(
    const XiPipelineCoroResolverCtx *ctx, const XiFunc *current, const XiValue *call) {
    const XgCallsiteSummary *row;
    if (!ctx || !ctx->cfg || !ctx->cfg->global_evidence || !current || !call ||
        call->xg_callsite_id == XG_NO_ID)
        return NULL;
    row = xg_global_evidence_find_callsite(ctx->cfg->global_evidence,
                                           (XgCallsiteId) call->xg_callsite_id);
    if (!row || row->owner_func_id != (XgFuncId) current->xg_body_func_id)
        return NULL;
    return row;
}

static const XiFunc *xi_pipeline_coro_resolve_method(void *ud, const XiFunc *current,
                                                     const XiValue *call) {
    XiPipelineCoroResolverCtx *ctx = (XiPipelineCoroResolverCtx *) ud;
    const XgCallsiteSummary *row = xi_pipeline_coro_callsite(ctx, current, call);
    const XiFunc *target =
        row ? xi_pipeline_coro_find_func(ctx, row->static_target_func_id) : NULL;
    if (target)
        return target;

    /* Computed-property reads are method invocations but do not have an
     * AST_CALL callsite row.  Resolve those and other row-less source methods
     * through the dependency-complete Xi class table, using the stable global
     * class id rather than a class or member spelling.  Dependency modules may
     * already have detached their analyzer pointers, but XiClassData keeps the
     * same xg_class_id carried by the current receiver's class_ref. */
    if (!ctx || !ctx->cfg || !ctx->cfg->graph_modules || !call ||
        (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs < 1 || !call->args[0] || !call->aux)
        return NULL;
    const XiValue *receiver = call->args[0];
    while (xi_copy_is_identity_alias(receiver) && receiver->nargs > 0)
        receiver = receiver->args[0];
    const char *member = (const char *) call->aux;

    /* An open generic method body has a canonical erased receiver type whose
     * analyzer class identity is intentionally not executable.  `this` still
     * has an exact Xi identity: parameter zero of a function selected by one
     * frozen XiClassData member row.  Resolve a sibling method through that
     * row before consulting receiver layout metadata. */
    if (current && current->params && current->nparams > 0 &&
        receiver == current->params[0]) {
        const XiModule *owner_module = NULL;
        const XiClassData *owner_class =
            xi_pipeline_coro_current_method_class(ctx, current, &owner_module);
        if (owner_class) {
            target = xi_pipeline_coro_class_method(owner_module, owner_class, member, false);
            if (target)
                return target;
        }
    }
    const XiImportRef *import_ref = xi_value_import_ref(current, receiver);
    if (import_ref && import_ref->resolved_module && import_ref->resolved_shared_slot >= 0 &&
        import_ref->resolved_shared_slot < import_ref->resolved_module->nslots &&
        import_ref->resolved_module->slot_classes) {
        const XiClassData *imported_class =
            import_ref->resolved_module->slot_classes[import_ref->resolved_shared_slot];
        if (imported_class)
            return xi_pipeline_coro_class_method(import_ref->resolved_module, imported_class,
                                                 member, true);
    }
    const XrType *receiver_type = receiver ? receiver->type : NULL;
    if (!receiver_type ||
        (receiver_type->kind != XR_KIND_INSTANCE && receiver_type->kind != XR_KIND_CLASS) ||
        !receiver_type->instance.class_ref || receiver_type->instance.class_ref->xg_class_id == 0)
        return NULL;
    const XgClassId receiver_class_id =
        (XgClassId) receiver_type->instance.class_ref->xg_class_id;
    const bool expect_static = receiver_type->kind == XR_KIND_CLASS;
    for (int module_index = 0; module_index < ctx->cfg->graph_module_count; module_index++) {
        const XiModule *module = ctx->cfg->graph_modules[module_index];
        for (uint16_t class_index = 0; module && class_index < module->nclasses; class_index++) {
            const XiClassData *class_data = module->classes[class_index];
            if (!class_data || class_data->xg_class_id != receiver_class_id ||
                !class_data->methods || !class_data->child_idx || !module->init)
                continue;
            target = xi_pipeline_coro_class_method(module, class_data, member, expect_static);
            if (target)
                return target;
            /* Exact class identity matched.  A missing method is unresolved;
             * never continue into an unrelated class with the same spelling. */
            return NULL;
        }
    }
    return NULL;
}

static int xi_pipeline_coro_func_suspendability(void *ud, const XiFunc *func) {
    (void) ud;
    if (!func || func->analyzer_effect_fingerprint == 0)
        return -1;
    /* Effect-summary completeness is dimensional.  An unresolved error or
     * native-allocation contract must not turn a function into a coroutine
     * when the analyzer has nevertheless closed the scheduler/generator
     * suspension dimensions.  Conversely, an unknown suspension bit is not a
     * synchronous proof and remains fail-closed. */
    if ((func->unknown_semantic_effects & XA_SEM_EFFECT_ANY_SUSPEND) != 0)
        return -1;
    return (func->semantic_effects & XA_SEM_EFFECT_ANY_SUSPEND) != 0 ? 1 : 0;
}

static const XiValue *xi_pipeline_coro_unwrap_identity(const XiValue *value) {
    while (value && xi_copy_is_identity_alias(value) && value->nargs > 0)
        value = value->args[0];
    return value;
}

static bool xi_pipeline_coro_is_sealed_builtin_constructor(const XiValue *call) {
    if (!call || call->op != XI_CALL || call->nargs < 1 ||
        !xi_value_is_constructor_call(call))
        return false;
    const XiValue *callee = xi_pipeline_coro_unwrap_identity(call->args[0]);
    return callee && callee->op == XI_GET_BUILTIN &&
           callee->aux_int > XR_GLOBAL_VAR_RESERVED0 &&
           callee->aux_int < XR_USER_GLOBALS_START &&
           callee->aux_int != XR_GLOBAL_VAR_RESERVED30;
}

static int xi_pipeline_coro_call_suspendability(void *ud, const XiFunc *current,
                                                 const XiValue *call) {
    XiPipelineCoroResolverCtx *ctx = (XiPipelineCoroResolverCtx *) ud;
    const XgGlobalEvidence *evidence = ctx && ctx->cfg ? ctx->cfg->global_evidence : NULL;
    const XgCallsiteSummary *row = xi_pipeline_coro_callsite(ctx, current, call);
    const XiImportRef *ref = NULL;
    const char *member = NULL;
    uint32_t effects = 0;
    /* Builtin constructors are sealed synchronous allocation boundaries.  A
     * source callsite may conservatively be shaped like a closure call before
     * lowering, so the Xi constructor proof and reserved global identity must
     * take precedence over graph-wide call-effect composition. */
    if (xi_pipeline_coro_is_sealed_builtin_constructor(call))
        return 0;
    /* Source-level callsite composition does not carry the private/public ABI
     * distinction of embedded module members.  Consume the sealed native ABI
     * registry first, including internal stdlib primitives such as
     * `os.__sleep`; a source module shadow stays on the Xi target path.
     *
     * The registry answer only becomes a yieldability proof once the import
     * reference is grounded by the module-graph resolver.  Until then the
     * semantic plan classifies the reference as unresolved and refuses the
     * call any target authority, so the callsite is answered as identified but
     * non-suspending rather than as a native suspension point.  A member the
     * registry does not declare at all gets the same answer for the same
     * reason: an unresolved reference has no target authority to grant, no
     * matter how the member is spelled. */
    if (current && call && call->nargs >= 1) {
        ref = xi_value_import_ref(current, call->args[0]);
        if (ref && ref->module_path && !ref->resolved_module && !ref->resolved_func) {
            if (call->op == XI_CALL && ref->member_name) {
                member = ref->member_name;
            } else if ((call->op == XI_CALL_METHOD ||
                        call->op == XI_CALL_METHOD_DIRECT) &&
                       !ref->member_name && call->aux) {
                member = (const char *) call->aux;
            }
            if (member &&
                xa_builtin_get_module_func_abi_signature(ref->module_path, member))
                return xi_import_ref_is_grounded_native(ref) &&
                               xa_builtin_module_func_is_yieldable(ref->module_path, member)
                           ? 1
                           : 0;
            if (xi_import_ref_is_unresolved(ref))
                return 0;
        }
    }
    if (row && xg_callsite_effects_compose_closed_world_calls(evidence, row, &effects))
        return (effects & XG_BODY_MAY_SUSPEND) != 0 ? 1 : 0;
    /* A frozen function-value callsite without one static target is not an
     * unresolved source call: the later callable TargetPlan closes its exact
     * target set.  Until that target-neutral boundary is refined, treating the
     * invocation as a suspension point is the only fail-closed answer.  The
     * shared plan records an indirect child edge, and AOT must subsequently
     * prove a non-empty callable plan before it may emit either a sync entry or
     * a child frame. */
    if (row && row->kind == XG_CALL_CLOSURE) {
        /* Statically constructed local closures have already resolved to one
         * XiFunc above. Anything reaching this branch still has an open
         * function-value target set, even if the value passed through a local
         * or shared slot. Only TargetPlan may refine it to an all-sync set. */
        return 1;
    }

    /* Some erased builtin projections intentionally have no per-call target
     * row (for example `process.args[0].slice(...)` after the receiver has
     * become `any`).  A fingerprinted analyzer summary with both suspension
     * dimensions closed is still an exact proof that no call in this function
     * can suspend.  Unknown suspension bits never reach this branch, and a
     * known-suspending function cannot use its aggregate fact to classify an
     * individual unresolved call. */
    if (xi_pipeline_coro_func_suspendability(ud, current) == 0)
        return 0;

    /* Native-module calls have no Xi body or module object.  Classify them
     * only through the analyzer's sealed ABI registry; an absent declaration
     * remains unresolved.  A resolved source module must take the Xi/export
     * path instead, even if its path shadows a native module spelling. */
    return -1;
}

/* The coroutine intrinsics keyed on a module identity - time.sleep, the
 * test_yield probes, the netpoll TCP operations - are rewrites of one specific
 * module's members, so the reference must actually be that module rather than
 * merely spell its path.  An unresolved reference names a module the compile
 * cannot read; the semantic plan grants calls through it no target authority
 * and forbids a coroutine state at them, so the intrinsic must not claim one
 * either. */
static bool xi_pipeline_coro_import_names_module(const XiImportRef *ref, const char *module) {
    return ref && ref->module_path && module && strcmp(ref->module_path, module) == 0 &&
           !xi_import_ref_is_unresolved(ref);
}

static bool xi_pipeline_coro_value_is_module_import(void *ud, const XiFunc *func,
                                                    const XiValue *value, const char *module) {
    (void) ud;
    return xi_pipeline_coro_import_names_module(xi_value_import_ref(func, value), module);
}

static bool xi_pipeline_coro_call_is_module_member(void *ud, const XiFunc *func,
                                                   const XiValue *call, const char *module,
                                                   const char *member) {
    (void) ud;
    if (!func || !call || !module || !member || call->nargs == 0)
        return false;
    if ((call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT) && call->aux) {
        const XiImportRef *ref = xi_value_import_ref(func, call->args[0]);
        return xi_pipeline_coro_import_names_module(ref, module) && !ref->member_name &&
               strcmp((const char *) call->aux, member) == 0;
    }
    if (call->op == XI_CALL) {
        const XiImportRef *ref = xi_value_import_ref(func, call->args[0]);
        return xi_pipeline_coro_import_names_module(ref, module) && ref->member_name &&
               strcmp(ref->member_name, member) == 0;
    }
    return false;
}

static XiCoroResolver xi_pipeline_coro_resolver(XiPipelineCoroResolverCtx *ctx) {
    XiCoroResolver resolver;
    memset(&resolver, 0, sizeof(resolver));
    resolver.resolve_callee = xi_pipeline_coro_resolve_callee;
    resolver.resolve_method = xi_pipeline_coro_resolve_method;
    resolver.func_suspendability = xi_pipeline_coro_func_suspendability;
    resolver.call_suspendability = xi_pipeline_coro_call_suspendability;
    resolver.value_is_module_import = xi_pipeline_coro_value_is_module_import;
    resolver.call_is_module_member = xi_pipeline_coro_call_is_module_member;
    resolver.ud = ctx;
    return resolver;
}

static void xi_set_wide_vector_boundary_policy_recursive(XiFunc *f, bool preserve) {
    if (!f)
        return;
    f->preserve_wide_vector_boundaries = preserve;
    for (uint16_t i = 0; i < f->nchildren; i++)
        xi_set_wide_vector_boundary_policy_recursive(f->children[i], preserve);
}

static void xi_pipeline_set_error(XiPipelineResult *res, XiPipeStatus status, XiPipelineStage stage,
                                  XiVerifyCode code, const XiFunc *func, const XiValue *value,
                                  const char *pass_name, const char *detail) {
    if (!res || res->status != XI_PIPE_OK || res->error.detail[0] != '\0')
        return;
    res->status = status;
    res->error.stage = stage;
    res->error.code = code;
    res->error.func = func;
    res->error.value = value;
    res->error.source_line = value ? value->line : 0;
    res->error.pass_name = pass_name;
    snprintf(res->error.detail, sizeof(res->error.detail), "%s",
             detail && detail[0] ? detail : "pipeline stage failed without a diagnostic");
}

static bool xi_verify_tree(const XiFunc *f, const XiFunc **failed_func, char *errbuf,
                           size_t errbuf_size) {
    if (!f) {
        snprintf(errbuf, errbuf_size, "NULL Xi function");
        if (failed_func)
            *failed_func = NULL;
        return false;
    }
    if (!xi_verify_stage(f, f->stage, errbuf, (int) errbuf_size)) {
        if (failed_func)
            *failed_func = f;
        return false;
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i] && !xi_verify_tree(f->children[i], failed_func, errbuf, errbuf_size))
            return false;
    }
    return true;
}

static XiVerifyCode xi_verify_code_from_detail(const char *detail) {
    if (!detail)
        return XI_VERIFY_STRUCTURE;
    if (strstr(detail, "RETURN") || strstr(detail, "return"))
        return XI_VERIFY_RETURN;
    if (strstr(detail, "PTR_") || strstr(detail, "memory") || strstr(detail, "Endian"))
        return XI_VERIFY_MEMORY;
    if (strstr(detail, "ErrorType"))
        return XI_VERIFY_EXECUTABLE_TYPE;
    return XI_VERIFY_STRUCTURE;
}

static bool xi_pipeline_verify_barrier(XiPipelineResult *res, XiPipelineStage stage) {
    char errbuf[512];
    const XiFunc *failed_func = NULL;
    if (!xi_verify_tree(res->ir, &failed_func, errbuf, sizeof(errbuf))) {
        xi_pipeline_set_error(res, XI_PIPE_ERR_VERIFY, stage, xi_verify_code_from_detail(errbuf),
                              failed_func, NULL, NULL, errbuf);
        return false;
    }

    XiSourceMoveVerifyReport move_report;
    XiSourceMoveVerifyStatus move_status = xi_source_move_verify_tree(res->ir, &move_report);
    if (move_status == XI_SOURCE_MOVE_PASS)
        return true;
    xi_pipeline_set_error(res,
                          move_status == XI_SOURCE_MOVE_INTERNAL_ERROR ? XI_PIPE_ERR_INTERNAL
                                                                       : XI_PIPE_ERR_VERIFY,
                          stage, XI_VERIFY_STRUCTURE, move_report.func, move_report.move,
                          "xi_source_move_verify", move_report.message);
    return false;
}

static bool xi_pipeline_analyzer_gate(struct XaAnalyzer *analyzer, XiPipelineResult *res) {
    if (!analyzer)
        return true;
    int count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &count); diag;
         diag = diag->next) {
        if (diag->severity != XR_DIAG_SEV_ERROR)
            continue;
        xi_pipeline_set_error(res, XI_PIPE_ERR_ANALYZE, XI_PIPE_STAGE_ANALYZE,
                              XI_VERIFY_EXECUTABLE_TYPE, NULL, NULL, NULL,
                              diag->message ? diag->message : "semantic analysis failed");
        res->error.source_line = diag->location.line > 0 ? (uint32_t) diag->location.line : 0;
        return false;
    }
    return true;
}

static bool xi_pipeline_push_source_file(struct XaAnalyzer *analyzer, const XiPipelineConfig *cfg,
                                         XaAnalyzerFileScope *file_scope, XiPipelineResult *res) {
    if (!analyzer || !cfg || !cfg->source_file || !cfg->source_file[0])
        return true;
    if (xa_analyzer_push_file_scope(analyzer, cfg->source_file, file_scope))
        return true;

    char detail[512];
    snprintf(detail, sizeof(detail), "analyzer has no file scope for '%s'", cfg->source_file);
    xi_pipeline_set_error(res, XI_PIPE_ERR_INTERNAL, XI_PIPE_STAGE_ANALYZE, XI_VERIFY_STRUCTURE,
                          NULL, NULL, NULL, detail);
    return false;
}

/* ========== Configuration ========== */

/* Neither pipeline carries a wall-clock optimizer budget. A budget cuts the
 * pass sequence wherever the machine happened to be busy, so the same compiler
 * on the same source could emit different artifacts between two runs, and the
 * VM and the native backend could enter representation selection from programs
 * that were optimized to different depths for no stated reason. The two
 * pipelines still choose their own optimization level; that choice is a stated
 * property of each configuration rather than an accident of timing. */

XR_FUNC XiPipelineConfig xi_pipeline_default_config(void) {
    XiPipelineConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = XI_PIPE_VM;
    cfg.run_optimize = true;
    cfg.opt_level = XI_OPT_LIGHT;
    cfg.run_select_rep = false;
    /* The VM runs escape analysis + precise dup/drop insertion (xi_arc) but
     * NOT stack_alloc_rewrite (no XI_STACK_ALLOC handler in the VM emitter)
     * and NOT backend_lower. dup/drop execute as OP_DUP/OP_DROP. */
    cfg.run_escape = true;
    cfg.run_arc = true;
    cfg.run_emit = true;
    cfg.run_canonicalize = true;
    cfg.dump_ir_before = false;
    cfg.dump_ir_after = false;
    cfg.rep_policy = xi_rep_policy_tagged_boundary();
    return cfg;
}

XR_FUNC XiPipelineConfig xi_pipeline_aot_config(void) {
    XiPipelineConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = XI_PIPE_AOT;
    cfg.run_optimize = true;
    cfg.opt_level = XI_OPT_FULL;
    cfg.run_select_rep = true;
    cfg.run_backend_lower = true;
    cfg.run_escape = true;
    cfg.run_arc = true;
    cfg.run_emit = false;
    cfg.run_canonicalize = true;
    cfg.dump_ir_before = false;
    cfg.dump_ir_after = false;
    cfg.rep_policy = xi_rep_policy_native_boundary();
    cfg.disabled_opt_passes = XI_OPT_DISABLE_IVSR;
    return cfg;
}

/* ========== Internal Pipeline ========== */

static XiFunc *xi_pipeline_release_stage_handle(void *program, XiStage stage) {
    switch (stage) {
        case XI_STAGE_RAW:
            return xi_raw_program_release((XiRawProgram *) program);
        case XI_STAGE_CANONICAL:
            return xi_canonical_program_release((XiCanonicalProgram *) program);
        case XI_STAGE_CLOSED:
            return xi_closed_program_release((XiClosedProgram *) program);
        case XI_STAGE_OWNED:
            return xi_owned_program_release((XiOwnedProgram *) program);
        case XI_STAGE_SEMANTIC_LOWERED:
            return xi_semantic_lowered_program_release((XiSemanticLoweredProgram *) program);
        case XI_STAGE_CORO_LOWERED:
            return xi_coro_lowered_program_release((XiCoroLoweredProgram *) program);
        case XI_STAGE_OPTIMIZED:
            return xi_optimized_program_release((XiOptimizedProgram *) program);
        case XI_STAGE_SEMANTIC_PLANNED:
            return xi_semantic_planned_program_release((XiSemanticPlannedProgram *) program);
        case XI_STAGE_REPPED:
            return xi_repped_program_release((XiReppedProgram *) program);
        case XI_STAGE_BACKEND:
            return xi_backend_program_release((XiBackendProgram *) program);
        default:
            return NULL;
    }
}

static XiPipelineResult run_pipeline(XiFunc *ir, struct XrVMRuntime *X,
                                     const XiPipelineConfig *cfg) {
    XiPipelineResult res;
    XiPipelineCoroResolverCtx coro_resolver_ctx = {.cfg = cfg, .root = ir};
    XiCoroResolver coro_resolver;
    const XiCoroResolver *coro_resolver_ptr = NULL;
    memset(&res, 0, sizeof(res));
    res.ir = ir;

    coro_resolver = xi_pipeline_coro_resolver(&coro_resolver_ctx);
    coro_resolver_ptr = &coro_resolver;

    if (!ir) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_LOWER, XI_PIPE_STAGE_LOWER,
                              XI_VERIFY_EXECUTABLE_TYPE, NULL, NULL, NULL,
                              "lowering did not produce executable Xi IR");
        return res;
    }

    xi_set_wide_vector_boundary_policy_recursive(ir, cfg->preserve_wide_vector_boundaries);

    char transition_error[512] = {0};
    void *program = xi_stage_adopt_raw(ir, transition_error, sizeof(transition_error));
    XiStage current_stage = XI_STAGE_RAW;
    if (!program) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_VERIFY_RAW,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        return res;
    }

    void *next = xi_program_canonicalize((XiRawProgram *) program, transition_error,
                                         sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_VERIFY_RAW,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        xi_pipeline_release_stage_handle(program, current_stage);
        return res;
    }
    program = next;
    current_stage = XI_STAGE_CANONICAL;

    /* Closure pass: build XiClosureMeta and materialize environment layout. */
    xi_pass_close(ir);
    next = xi_program_close((XiCanonicalProgram *) program, transition_error,
                            sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_VERIFY_RAW,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        xi_pipeline_release_stage_handle(program, current_stage);
        return res;
    }
    program = next;
    current_stage = XI_STAGE_CLOSED;

    /* Optional: dump IR before optimization */
    if (cfg->dump_ir_before) {
        fprintf(stderr, "=== Xi IR (before optimization) ===\n");
        xi_func_dump(ir, stderr);
        fprintf(stderr, "===================================\n");
    }

    /* Verification barriers are part of the executable pipeline contract and
     * cannot be disabled by configuration. */
    if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_VERIFY_RAW))
        goto fail;

    if (cfg->run_backend_lower) {
        XiPassChange enum_type_lookup = xi_opt_compact_enum_payload_type_lookup(ir);
        if (enum_type_lookup.values_changed) {
            xi_opt_dce(ir);
            if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_OPTIMIZE))
                return res;
        }
    }

    /* A concrete EnumVariant<E>/EnumPayloadField<E> is a scalar while E is
     * statically known.  Crossing an erased identity boundary is a semantic
     * allocation, so materialize it before escape and ARC analysis for both
     * VM and AOT instead of hiding it in AOT representation selection. */
    xi_opt_materialize_enum_descriptor_erasure(ir);

    /* Escape analysis: compute escape levels for heap-allocating values.
     * Ownership is made explicit before semantic optimization. */
    if (cfg->run_escape) {
        xi_escape_analyze(ir);
    }
    if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_ESCAPE))
        goto fail;

    /* Backward ownership inference (analysis only — does not mutate IR).
     * Gated behind XRAY_XI_OWN_DUMP=1 for manual verification of dup/drop
     * decisions; the xi_arc rewrite consumes these annotations directly. */
    {
        if (xi_env_is_enabled("XRAY_XI_OWN_DUMP")) {
            xi_own_dump_recursive(ir);
        }
    }

    /* Stack alloc rewrite: replace NO_ESCAPE heap allocs with XI_STACK_ALLOC.
     * Must run after escape analysis and before ARC insertion (STACK_ALLOC
     * values don't need retain/release since they have frame lifetime).
     * Gated on run_backend_lower because only backend codegen (AOT)
     * consumes XI_STACK_ALLOC; the VM emitter has no handler for it. */
    if (cfg->run_escape && cfg->run_backend_lower) {
        xi_stack_alloc_rewrite(ir);
    }

    /* Dependency resolution and ownership summaries are semantic inputs to
     * SemanticPlan even when a diagnostic/prototype pipeline skips physical
     * retain/release insertion. */
    if (cfg->module_graph && cfg->graph_modules && cfg->graph_module_count > 0) {
        xi_resolve_imports(ir, cfg->module_graph, cfg->source_file, cfg->graph_modules,
                           cfg->graph_module_count);
    }
    xi_arc_analyze_contracts(ir);

    /* Precise dup/drop insertion (consumes ownership analysis).
     * MUST run BEFORE backend lowering, while ops are still semantic
     * (STORE_FIELD/ARRAY_NEW/...) — the owned/borrow split is keyed on
     * those ops. Gated on run_arc (independent of run_backend_lower) so the
     * VM can get dup/drop without stack_alloc_rewrite. */
    if (cfg->run_escape && cfg->run_arc) {
        /* Multi-module drivers compile in topological order, so every
         * dependency is already compiled and the semantic stage above has
         * resolved its frozen borrow signatures. ARC can therefore keep
         * ownership of arguments a callee only borrows and release them at
         * their death point; skipping resolution would move the argument into
         * a callee that never releases it. */
        xi_arc_insert(ir);
        if (getenv("XRAY_XI_ARC_DUMP")) {
            fprintf(stderr, "=== Xi IR after xi_arc_insert ===\n");
            xi_func_dump(ir, stderr);
            fprintf(stderr, "=================================\n");
        }
        /* Dup/drop elimination: remove redundant RETAIN+RELEASE pairs where
         * the value is merely forwarded (copy→move optimization). Runs on
         * all backends including VM (fewer RC ops = faster interpretation). */
        xi_arc_elim(ir);
        if (xi_env_is_enabled("XRAY_XI_RC_COUNT")) {
            uint64_t nret = 0, nrel = 0;
            xi_count_rc_ops_recursive(ir, &nret, &nrel);
            fprintf(stderr,
                    "[xi-rc-count] func=%s retain=%" PRIu64 " release=%" PRIu64 " total=%" PRIu64
                    "\n",
                    ir->name ? ir->name : "<anon>", nret, nrel, nret + nrel);
        }
        /* Task 219: independent RC-contract verifier. Runs FORCED once here,
         * right after ARC insertion, in every build mode. A violation means the
         * compiler mis-inferred ownership (use-after-release / double-free in
         * otherwise-safe user code) — a hard ICE with a counterexample path is
         * the correct response, not a silent miscompile. */
        xi_arc_verify_or_ice(ir, "xi_arc_insert");
    }

    next = xi_program_make_owned((XiClosedProgram *) program, transition_error,
                                 sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_OWNERSHIP,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        goto fail;
    }
    program = next;
    current_stage = XI_STAGE_OWNED;

    next = xi_program_lower_semantics((XiOwnedProgram *) program, transition_error,
                                      sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_OWNERSHIP,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        goto fail;
    }
    program = next;
    current_stage = XI_STAGE_SEMANTIC_LOWERED;

    if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_OWNERSHIP))
        goto fail;

    next = xi_program_lower_coroutines((XiSemanticLoweredProgram *) program, coro_resolver_ptr,
                                       transition_error, sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_OWNERSHIP,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        goto fail;
    }
    program = next;
    current_stage = XI_STAGE_CORO_LOWERED;

    /* Optimization consumes CoroLowered and produces a verified Optimized program. */
    if (cfg->run_optimize) {
        XiOptLevel level = cfg->opt_level;
        if (level == XI_OPT_NONE)
            level = XI_OPT_LIGHT;

        XiPipelineStats stats;
        XiOptResult opt = xi_opt_run_pipeline_ex_with_mask(ir, level, &stats, cfg->budget_ns,
                                                           cfg->disabled_opt_passes);
        if (!opt.ok) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_OPTIMIZE,
                                  XI_VERIFY_OPT_INVARIANT, ir, NULL, opt.pass_name, opt.detail);
            goto fail;
        }
        if (xi_env_is_enabled("XRAY_XI_STATS"))
            xi_pipeline_stats_dump(&stats, ir->name);
    }

    next = xi_program_finish_optimization((XiCoroLoweredProgram *) program, transition_error,
                                          sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_OPTIMIZE,
                              XI_VERIFY_OPT_INVARIANT, ir, NULL, NULL, transition_error);
        goto fail;
    }
    program = next;
    current_stage = XI_STAGE_OPTIMIZED;
    if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_OPTIMIZE))
        goto fail;

    if (xi_env_is_enabled("XRAY_XI_SEMANTIC_DUMP")) {
        fprintf(stderr, "=== Xi IR consumed by SemanticPlan ===\n");
        xi_func_dump(ir, stderr);
        fprintf(stderr, "=======================================\n");
    }
    uint32_t semantic_dependency_count =
        cfg->graph_modules && cfg->graph_module_count > 0
            ? (uint32_t) cfg->graph_module_count
            : 0;
    if (!xr_semantic_plan_build_and_attach_module_set(
            ir, cfg->graph_modules, semantic_dependency_count,
            transition_error, sizeof(transition_error))) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_SEMANTIC_PLAN,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        goto fail;
    }
    next = xi_program_freeze_semantics((XiOptimizedProgram *) program, transition_error,
                                       sizeof(transition_error));
    if (!next) {
        xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_SEMANTIC_PLAN,
                              XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
        goto fail;
    }
    program = next;
    current_stage = XI_STAGE_SEMANTIC_PLANNED;
    if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_SEMANTIC_PLAN))
        goto fail;

    /* SelectRepresentations: insert BOX/UNBOX at representation boundaries.
     * Run after general optimization so constants/copies are resolved first. */
    if (cfg->run_select_rep) {
        xi_opt_refresh_representations_with_policy(ir, &cfg->rep_policy);
        next = xi_program_select_reps((XiSemanticPlannedProgram *) program, transition_error,
                                      sizeof(transition_error));
        if (!next) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_REPRESENTATION,
                                  XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
            goto fail;
        }
        program = next;
        current_stage = XI_STAGE_REPPED;
        if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_REPRESENTATION))
            goto fail;
    }

    /* Backend lowering is legal only after target representation selection. */
    if (cfg->run_backend_lower) {
        if (current_stage != XI_STAGE_REPPED) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_INTERNAL, XI_PIPE_STAGE_BACKEND,
                                  XI_VERIFY_STRUCTURE, ir, NULL, NULL,
                                  "backend planning requires a Repped program");
            goto fail;
        }
        xi_backend_lower(ir);
        /* A unit ERR_CHECK has an implicit function-exit edge which ordinary
         * CFG liveness cannot represent.  Publish its cold-edge drop operands
         * only after optimization, representation selection, vector scalar
         * lowering, and every other value-cloning rewrite.  Earlier attachment
         * would turn error-only ownership into normal SSA uses and inhibit
         * dead-value/representation cleanup on the successful hot path. */
        if (cfg->run_escape && cfg->run_arc) {
            xi_arc_attach_error_cleanups(ir);
            xi_arc_verify_error_cleanups_or_ice(ir, "xi_arc_attach_error_cleanups");
        }
        next = xi_program_plan_backend((XiReppedProgram *) program, transition_error,
                                       sizeof(transition_error));
        if (!next) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_BACKEND,
                                  XI_VERIFY_AOT_PLAN, ir, NULL, NULL, transition_error);
            goto fail;
        }
        program = next;
        current_stage = XI_STAGE_BACKEND;
        if (!xi_pipeline_verify_barrier(&res, XI_PIPE_STAGE_BACKEND))
            goto fail;
    }

    /* Optional: dump IR after optimization */
    if (cfg->dump_ir_after) {
        fprintf(stderr, "=== Xi IR (after optimization) ===\n");
        xi_func_dump(ir, stderr);
        fprintf(stderr, "==================================\n");
    }

    /* Bytecode emission (skipped in AOT/CHECK mode) */
    if (cfg->run_emit) {
        struct XrProto *proto = NULL;
        XiEmitStatus emit_st = xi_emit(ir, X, &proto);
        if (emit_st != XI_EMIT_OK) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_EMIT, XI_PIPE_STAGE_EMIT, XI_VERIFY_EMISSION,
                                  ir, NULL, NULL, xi_emit_status_str(emit_st));
            goto fail;
        }
        res.proto = proto;

        /* The VM emitter consumes frozen target-neutral semantic IR. After bytecode is
         * fixed, select the VM/JIT tagged representation and close the Repped
         * transition for the IR retained by the proto. */
        if (current_stage == XI_STAGE_SEMANTIC_PLANNED) {
            XiRepPolicy vm_policy = xi_rep_policy_tagged_boundary();
            xi_opt_refresh_representations_with_policy(ir, &vm_policy);
            next = xi_program_select_reps((XiSemanticPlannedProgram *) program, transition_error,
                                          sizeof(transition_error));
            if (!next) {
                xi_pipeline_set_error(&res, XI_PIPE_ERR_VERIFY, XI_PIPE_STAGE_REPRESENTATION,
                                      XI_VERIFY_STRUCTURE, ir, NULL, NULL, transition_error);
                xr_instruction_unit_free(proto);
                res.proto = NULL;
                goto fail;
            }
            program = next;
            current_stage = XI_STAGE_REPPED;
        }

        ir = xi_pipeline_release_stage_handle(program, current_stage);
        program = NULL;
        res.ir = ir;
        char snapshot_error[256];
        if (!xi_semantic_snapshot_detach_ex(ir, snapshot_error, sizeof(snapshot_error))) {
            xr_instruction_unit_free(proto);
            res.proto = NULL;
            xi_pipeline_set_error(&res, XI_PIPE_ERR_INTERNAL, XI_PIPE_STAGE_EMIT,
                                  XI_VERIFY_EMISSION, ir, NULL, NULL, snapshot_error);
            return res;
        }
        /* Transfer Xi IR ownership to proto for AOT direct lowering.
         * Null res.ir so xi_pipeline_result_free won't double-free. */
        if (!xi_emit_attach_ir(proto, ir)) {
            xr_instruction_unit_free(proto);
            res.proto = NULL;
            res.ir = ir;
            xi_pipeline_set_error(&res, XI_PIPE_ERR_INTERNAL, XI_PIPE_STAGE_EMIT,
                                  XI_VERIFY_EMISSION, ir, NULL, NULL,
                                  "VM IR attachment requires a verified Repped program");
            return res;
        }
        res.ir = NULL;
    } else {
        ir = xi_pipeline_release_stage_handle(program, current_stage);
        program = NULL;
        res.ir = ir;
        char snapshot_error[256];
        if (!xi_semantic_snapshot_detach_ex(ir, snapshot_error, sizeof(snapshot_error))) {
            xi_pipeline_set_error(&res, XI_PIPE_ERR_INTERNAL, XI_PIPE_STAGE_BACKEND,
                                  XI_VERIFY_AOT_PLAN, ir, NULL, NULL, snapshot_error);
            return res;
        }
    }

    res.status = XI_PIPE_OK;
    return res;

fail:
    if (program)
        xi_pipeline_release_stage_handle(program, current_stage);
    return res;
}

/* ========== Public API ========== */

XR_FUNC XiPipelineResult xi_pipeline_compile_func(struct AstNode *func_node,
                                                  struct XaAnalyzer *analyzer,
                                                  struct XrVMRuntime *isolate,
                                                  const XiPipelineConfig *cfg) {
    XR_DCHECK(func_node != NULL, "xi_pipeline_compile_func: NULL func_node");
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);

    XiPipelineConfig default_cfg;
    if (!cfg) {
        default_cfg = xi_pipeline_default_config();
        cfg = &default_cfg;
    }

    XiPipelineResult gate;
    memset(&gate, 0, sizeof(gate));
    XaAnalyzerFileScope file_scope = {0};
    if (!xi_pipeline_push_source_file(analyzer, cfg, &file_scope, &gate))
        return gate;
    if (!xi_pipeline_analyzer_gate(analyzer, &gate)) {
        xa_analyzer_pop_file_scope(analyzer, &file_scope);
        return gate;
    }

    /* Canonicalize AST before lowering */
    if (cfg->run_canonicalize)
        xr_canon_func(func_node, analyzer, session);

    XaTypedProgramPublishResult typed = xa_typed_program_publish(analyzer, func_node, NULL, 0);
    if (!typed.program) {
        xi_pipeline_set_error(&gate, XI_PIPE_ERR_ANALYZE, XI_PIPE_STAGE_ANALYZE,
                              XI_VERIFY_EXECUTABLE_TYPE, NULL, NULL,
                              xa_typed_program_reason_name(typed.reason), typed.detail);
        gate.error.source_line = typed.source_line;
        xa_analyzer_pop_file_scope(analyzer, &file_scope);
        return gate;
    }
    XiFunc *ir = xi_lower_func(typed.program, isolate);
    xa_typed_program_free(typed.program);

    xa_analyzer_pop_file_scope(analyzer, &file_scope);

    if (ir)
        xi_set_source_file_recursive(ir, cfg->source_file);
    if (ir && ir->module)
        ir->module->path = cfg->source_file;

    return run_pipeline(ir, isolate, cfg);
}

XR_FUNC XiPipelineResult xi_pipeline_compile_program(struct AstNode *program_node,
                                                     struct XaAnalyzer *analyzer,
                                                     struct XrVMRuntime *isolate,
                                                     const XiPipelineConfig *cfg) {
    XR_DCHECK(program_node != NULL, "xi_pipeline_compile_program: NULL program_node");

    XiPipelineConfig default_cfg;
    if (!cfg) {
        default_cfg = xi_pipeline_default_config();
        cfg = &default_cfg;
    }

    XiPipelineResult gate;
    memset(&gate, 0, sizeof(gate));
    XaAnalyzerFileScope file_scope = {0};
    if (!xi_pipeline_push_source_file(analyzer, cfg, &file_scope, &gate))
        return gate;
    if (!xi_pipeline_analyzer_gate(analyzer, &gate)) {
        xa_analyzer_pop_file_scope(analyzer, &file_scope);
        return gate;
    }

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    XrCompilerSessionScope canon_scope;
    bool has_canon_scope = cfg->run_canonicalize && program_node->type == AST_PROGRAM &&
                           program_node->as.program.arena &&
                           xr_compiler_session_push_arena(session, program_node->as.program.arena,
                                                          cfg->source_file, &canon_scope);

    /* Canonicalize AST before lowering */
    if (cfg->run_canonicalize)
        xr_canon_program(program_node, analyzer, session);

    if (has_canon_scope)
        xr_compiler_session_pop_arena(&canon_scope);

    XaTypedProgramPublishResult typed = xa_typed_program_publish(
        analyzer, program_node, cfg->global_evidence, cfg->global_evidence_module_id);
    if (!typed.program) {
        xi_pipeline_set_error(&gate, XI_PIPE_ERR_ANALYZE, XI_PIPE_STAGE_ANALYZE,
                              XI_VERIFY_EXECUTABLE_TYPE, NULL, NULL,
                              xa_typed_program_reason_name(typed.reason), typed.detail);
        gate.error.source_line = typed.source_line;
        xa_analyzer_pop_file_scope(analyzer, &file_scope);
        return gate;
    }
    XiFunc *ir = xi_lower_program(typed.program, isolate, cfg->repl_mode);
    xa_typed_program_free(typed.program);

    xa_analyzer_pop_file_scope(analyzer, &file_scope);

    if (ir)
        xi_set_source_file_recursive(ir, cfg->source_file);
    if (ir && ir->module)
        ir->module->path = cfg->source_file;

    return run_pipeline(ir, isolate, cfg);
}

XR_FUNC struct XrProto *xi_pipeline_emit_ir(XiFunc *ir, struct XrVMRuntime *isolate) {
    XR_DCHECK(ir != NULL, "xi_pipeline_emit_ir: NULL ir");
    XR_DCHECK(isolate != NULL, "xi_pipeline_emit_ir: NULL isolate");

    struct XrProto *proto = NULL;
    XiEmitStatus emit_st = xi_emit(ir, isolate, &proto);
    if (emit_st != XI_EMIT_OK) {
        fprintf(stderr, "[xi_pipeline] emit_ir failed: %s\n", xi_emit_status_str(emit_st));
        return NULL;
    }

    char snapshot_error[256];
    if (!xi_semantic_snapshot_detach_ex(ir, snapshot_error, sizeof(snapshot_error))) {
        fprintf(stderr, "[xi_pipeline] semantic snapshot failed: %s\n", snapshot_error);
        xr_instruction_unit_free(proto);
        return NULL;
    }

    /* Transfer IR ownership to proto for AOT direct lowering */
    if (!xi_emit_attach_ir(proto, ir)) {
        xr_instruction_unit_free(proto);
        return NULL;
    }
    return proto;
}

XR_FUNC void xi_pipeline_result_free(XiPipelineResult *res) {
    if (!res)
        return;
    if (res->ir) {
        xi_func_free(res->ir);
        res->ir = NULL;
    }
    if (res->module) {
        xi_module_free(res->module);
        res->module = NULL;
    }
    /* proto is NOT freed — caller owns it */
}

XR_FUNC const char *xi_pipe_status_str(XiPipeStatus s) {
    switch (s) {
        case XI_PIPE_OK:
            return "OK";
        case XI_PIPE_ERR_ANALYZE:
            return "semantic analysis failed";
        case XI_PIPE_ERR_LOWER:
            return "AST lowering failed";
        case XI_PIPE_ERR_VERIFY:
            return "IR verification failed";
        case XI_PIPE_ERR_EMIT:
            return "bytecode emission failed";
        case XI_PIPE_ERR_INTERNAL:
            return "internal pipeline error";
    }
    return "unknown";
}

XR_FUNC const char *xi_pipeline_stage_str(XiPipelineStage stage) {
    switch (stage) {
        case XI_PIPE_STAGE_NONE:
            return "none";
        case XI_PIPE_STAGE_ANALYZE:
            return "analyze";
        case XI_PIPE_STAGE_LOWER:
            return "lower";
        case XI_PIPE_STAGE_VERIFY_RAW:
            return "verify-raw";
        case XI_PIPE_STAGE_OPTIMIZE:
            return "optimize";
        case XI_PIPE_STAGE_ESCAPE:
            return "escape";
        case XI_PIPE_STAGE_OWNERSHIP:
            return "ownership";
        case XI_PIPE_STAGE_SEMANTIC_PLAN:
            return "semantic-plan";
        case XI_PIPE_STAGE_REPRESENTATION:
            return "representation";
        case XI_PIPE_STAGE_BACKEND:
            return "backend";
        case XI_PIPE_STAGE_EMIT:
            return "emit";
    }
    return "unknown";
}
