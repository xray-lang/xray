/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_imported_constructor_normalize.c - Unique grounded constructor producer
 */

#include "xi_imported_constructor_normalize.h"
#include "xi.h"
#include "xi_evidence.h"
#include "xi_module.h"
#include "xi_semantic_snapshot.h"
#include "xi_value_query.h"
#include "../analysis/xglobal_summary.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/analyzer/xa_node_table.h"
#include "../plan/semantic/xr_semantic_builder.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/value/xtype.h"

#include <string.h>

typedef struct XiConstructorRewrite {
    XiFunc *function;
    XiValue *call;
    XiSemanticTypeReplacement nominal;
} XiConstructorRewrite;

typedef struct XiConstructorNormalizer {
    XrVMRuntime *runtime;
    const XiPipelineConfig *config;
    XiConstructorRewrite *rewrites;
    uint32_t count;
    uint32_t capacity;
} XiConstructorNormalizer;

static const XgClassSummary *constructor_class_summary(const XgGlobalEvidence *evidence,
                                                       uint32_t class_id) {
    const XgClassSummary *match = NULL;
    for (uint32_t i = 0; evidence && i < evidence->nclasses; i++) {
        const XgClassSummary *candidate = &evidence->classes[i];
        if (candidate->class_id != class_id)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static bool constructor_grounded_specialization(XiConstructorNormalizer *context,
                                                 XiFunc *caller, const XrType *instance,
                                                 const XgClassSummary *klass,
                                                 const XaGenericSpecializationFact *fact) {
    XaAnalyzer *analyzer = caller->analyzer;
    if (!analyzer || !klass || !fact || !xa_generic_specialization_fact_valid(fact) ||
        fact->owner_decl || fact->receiver_type_arg_count ||
        fact->declaration_type_arg_count == 0 ||
        fact->declaration_type_arg_count != (uint32_t) instance->instance.type_arg_count ||
        !(klass->flags & XG_CLASS_MONOMORPHIZED) ||
        klass->generic_type_arg_count != fact->declaration_type_arg_count)
        return false;
    XaScope *scope = xa_scope_find_by_node(analyzer->global_scope, (void *) fact->generic_decl);
    const XaSymbol *origin = scope ? scope->class_symbol : NULL;
    const XrType *origin_type = origin ? origin->links.type : NULL;
    if (!origin_type || origin_type->kind != XR_KIND_CLASS || !origin_type->instance.class_ref ||
        origin_type->instance.class_ref->xg_class_id != klass->generic_origin_class_id ||
        origin_type->instance.class_ref->xg_nominal_key != klass->generic_origin_nominal_key)
        return false;
    for (uint32_t i = 0; i < fact->declaration_type_arg_count; i++) {
        const XrType *expected = xa_analyzer_get_type_ref_type(analyzer,
                                                              fact->declaration_type_args[i]);
        if (!expected || !instance->instance.type_args ||
            !xr_semantic_source_types_equal(caller, expected, instance->instance.type_args[i],
                                            context->config->graph_modules,
                                            (uint32_t) context->config->graph_module_count))
            return false;
    }
    const XgGenericInstSummary *match = NULL;
    const XgGlobalEvidence *evidence = context->config->global_evidence;
    for (uint32_t i = 0; evidence && i < evidence->ngeneric_insts; i++) {
        const XgGenericInstSummary *candidate = &evidence->generic_insts[i];
        if (candidate->kind != XG_GENERIC_INST_CLASS ||
            candidate->specialized_class_id != klass->class_id ||
            candidate->root_callsite_id != XG_NO_ID)
            continue;
        if (match)
            return false;
        match = candidate;
    }
    return match && match->origin_class_id == klass->generic_origin_class_id &&
        match->origin_nominal_key == klass->generic_origin_nominal_key &&
        match->declaration_type_key == klass->generic_type_key &&
        match->declaration_type_arg_key_start == klass->generic_type_arg_key_start &&
        match->declaration_type_arg_count == klass->generic_type_arg_count &&
        match->specialization_effect == (uint8_t) fact->effect;
}

static bool constructor_nominal_declaration_is_exact(XiConstructorNormalizer *context,
                                                     XiFunc *caller, const XiValue *call) {
    const XrType *instance = call->type;
    const XrClassInfo *info = instance->instance.class_ref;
    const XaSymbol *symbol = info->declaration_symbol;
    const AstNode *declaration = symbol ? symbol->links.nominal_decl_node : NULL;
    const XgClassSummary *klass = constructor_class_summary(context->config->global_evidence,
                                                            info->xg_class_id);
    if (!caller->analyzer || !symbol || !declaration || !klass || symbol->links.class_info != info ||
        info->xg_decl_id != klass->decl_id || instance->is_nullable || instance->is_const ||
        instance->is_value_type || instance->is_literal || instance->alias_name ||
        instance->instance.superclass)
        return false;
    XaGenericSpecializationFact fact = {0};
    if (xa_analyzer_get_generic_specialization(caller->analyzer, declaration, &fact))
        return constructor_grounded_specialization(context, caller, instance, klass, &fact);
    return instance->instance.type_arg_count == 0 &&
        !(klass->flags & XG_CLASS_MONOMORPHIZED) && klass->generic_type_arg_count == 0;
}

static int constructor_prepare_rewrite(XiConstructorNormalizer *context, XiFunc *caller,
                                       XiValue *call) {
    if (!call || call->op != XI_CALL_METHOD || !xi_value_is_constructor_call(call))
        return 0;
    XiImportedConstructorAuthority authority = {0};
    if (!xi_value_imported_constructor_authority(caller, call, &authority) ||
        !constructor_nominal_declaration_is_exact(context, caller, call) ||
        (call->call_plan && (!call->call_plan->verified || call->call_plan->has_receiver ||
                            call->call_plan->nargs != call->nargs - 1u)))
        return 0;
    const XrSemanticFunctionRecord *constructor =
        xr_semantic_plan_function(authority.plan, authority.constructor);
    if (!constructor || constructor->parameter_count != call->nargs)
        return 0;
    XrType *canonical = xr_type_new_instance(context->runtime, call->type->instance.class_ref);
    if (!canonical)
        return -1;
    XiModule *const *modules = context->config->graph_modules;
    uint32_t count = (uint32_t) context->config->graph_module_count;
    if (!xr_semantic_source_type_admits_parameter(caller, canonical, authority.plan,
                                                  constructor->parameter_begin, modules, count))
        return 0;
    if (context->count == context->capacity) {
        uint32_t capacity = context->capacity ? context->capacity * 2u : 8u;
        if (capacity > UINT32_MAX / sizeof(*context->rewrites))
            return -1;
        XiConstructorRewrite *rewrites = (XiConstructorRewrite *)
            xr_realloc(context->rewrites, (size_t) capacity * sizeof(*rewrites));
        if (!rewrites)
            return -1;
        context->rewrites = rewrites;
        context->capacity = capacity;
    }
    for (uint32_t i = 0; i < context->count; i++)
        if (context->rewrites[i].nominal.source == call->type) {
            const XrType *prior = context->rewrites[i].nominal.canonical;
            if (!xr_semantic_source_types_equal(caller, prior, canonical, modules, count))
                return 0;
            canonical = (XrType *) prior;
            break;
        }
    context->rewrites[context->count++] =
        (XiConstructorRewrite) {caller, call, {call->type, canonical}};
    return 1;
}

/* Parameter proofs use the complete candidate map, so a nested grounded
 * constructor is checked in canonical form before anything is published. */
static bool constructor_parameters_are_exact(XiConstructorNormalizer *context) {
    for (uint32_t r = 0; r < context->count; r++) {
        const XiConstructorRewrite *rewrite = &context->rewrites[r];
        XiImportedConstructorAuthority authority = {0};
        if (!xi_value_imported_constructor_authority(rewrite->function, rewrite->call, &authority))
            return false;
        const XrSemanticFunctionRecord *function =
            xr_semantic_plan_function(authority.plan, authority.constructor);
        if (!function)
            return false;
        for (uint16_t a = 1; a < rewrite->call->nargs; a++) {
            const XiValue *source = rewrite->call->args[a];
            if (!source)
                return false;
            XiValue argument = *source;
            for (uint32_t i = 0; i < context->count; i++)
                if (argument.type == context->rewrites[i].nominal.source) {
                    argument.type = (XrType *) context->rewrites[i].nominal.canonical;
                    break;
                }
            if (!xr_semantic_source_constructor_argument_admits(
                    rewrite->function, &argument, authority.plan, function->parameter_begin + a,
                    context->config->graph_modules, (uint32_t) context->config->graph_module_count))
                return false;
        }
    }
    return true;
}

static bool constructor_collect_rewrites(XiConstructorNormalizer *context, XiFunc *function) {
    if (!function)
        return false;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        XiBlock *block = function->blocks ? function->blocks[b] : NULL;
        if (!block)
            return false;
        for (uint32_t v = 0; v < block->nvalues; v++)
            if (!block->values || constructor_prepare_rewrite(context, function, block->values[v]) < 0)
                return false;
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        if (!function->children || !constructor_collect_rewrites(context, function->children[i]))
            return false;
    return true;
}

XR_FUNC bool xi_imported_constructor_normalize(XiFunc *root, XrVMRuntime *runtime,
                                                const XiPipelineConfig *config) {
    if (!root || !runtime || !config)
        return false;
    if (!config->graph_modules || config->graph_module_count <= 0)
        return true;
    XiConstructorNormalizer context = {.runtime = runtime, .config = config};
    bool ok = constructor_collect_rewrites(&context, root) &&
              constructor_parameters_are_exact(&context);
    XiSemanticTypeReplacement *types = NULL;
    if (ok && context.count) {
        types = (XiSemanticTypeReplacement *) xr_calloc(context.count, sizeof(*types));
        ok = types != NULL;
        for (uint32_t i = 0; ok && i < context.count; i++)
            types[i] = context.rewrites[i].nominal;
        ok = ok && xi_semantic_type_rewrite_atomic(root, types, context.count);
    }
    if (ok)
        for (uint32_t i = 0; i < context.count; i++) {
            XiConstructorRewrite *rewrite = &context.rewrites[i];
            rewrite->call->op = XI_CALL;
            rewrite->call->aux = NULL;
            rewrite->call->aux_int = 0;
            xi_evidence_note_rewrite(rewrite->function, false, true, false,
                                      XI_EVD_CALL_TARGET | XI_EVD_EFFECT | XI_EVD_ALIAS | XI_EVD_ESCAPE);
        }
    xr_free(types);
    xr_free(context.rewrites);
    return ok;
}
