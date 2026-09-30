/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_call_plan.inc.c - One ordered argument inference driver
 *
 * KEY CONCEPT:
 *   Authentic consumers fix the declaration prefix; one private driver gathers
 *   value evidence and publishes a complete tuple before consumer-specific proof.
 */
#include "xxir_type_inference.h"
typedef enum SourceCallFamily { SOURCE_CALL_FUNCTION, SOURCE_CALL_REQUIREMENT } SourceCallFamily;
typedef struct SourceCallPlan {
    SourceCallFamily family;
    SourceSubstitution prefix, substitution;
    const XrXirType *function_parameters;
    const XrXirCallableParameter *requirement_parameters;
    XrXirType result, expected_result;
    SourceValue *values;
    uint32_t parameter_offset, value_offset;
} SourceCallPlan;
static bool source_call_plan_arguments(SourceContext *ctx, AstNode *node, const SourceCallPlan *plan) {
    CallExprNode *call=&node->as.call_expr;
    bool requirement=plan->family==SOURCE_CALL_REQUIREMENT;
    uint32_t prefix=plan->prefix.count, count=plan->substitution.count, own=count-prefix;
    XrXirType *types=(XrXirType *)plan->substitution.types;
    bool inferred=own && !call->type_arg_count, ok=false;
    XrXirInferenceState *state=NULL;
    if (inferred) {
        XrXirInferenceRequest begin={&ctx->types,plan->prefix.types,prefix,own,
            ctx->generics[ctx->function].parameter_count};
        XrXirStatus status=xr_xir_inference_begin(&begin,&ctx->budget,&state);
        if (status!=XR_XIR_OK) return source_fail(ctx,node,status,
            requirement ? "method inference could not begin" : "direct call inference could not begin");
    } else for (uint32_t p=0;p<own;++p)
        if (!source_work(ctx,node) || !source_type(ctx,call->type_args[p],&types[prefix+p])) return false;
    for (uint32_t a=0;a<(uint32_t)call->arg_count;++a) {
        if (call->arg_accesses && call->arg_accesses[a]!=XR_CALL_ARG_PLAIN) {
            source_fail(ctx,node,XR_XIR_BAD_TYPE,requirement ? "interface method argument must be read" :
                "direct call arguments must use the declared READ contract"); goto done;
        }
        uint32_t parameter=a+plan->parameter_offset;
        XrXirType formal=requirement ? plan->requirement_parameters[parameter].type : plan->function_parameters[parameter];
        XrXirType expected=XR_XIR_UNIT;
        if (inferred) {
            XrXirInferenceKnown known={0};
            XrXirStatus status=xr_xir_inference_expected_known(state,&ctx->types,formal,&known);
            if (status!=XR_XIR_OK) { source_fail(ctx,node,status,requirement ?
                "method inference context is invalid" : "direct call inference context is invalid"); goto done; }
            if (known.known) {
                SourceSubstitution partial={known.arguments,known.argument_count};
                if (!source_substitute(ctx,&partial,formal,0,&expected)) goto done;
            }
        } else if (!source_substitute(ctx,&plan->substitution,formal,0,&expected)) goto done;
        SourceValue *value=&plan->values[a+plan->value_offset];
        if (!expression_in(ctx,call->arguments[a],expected,value)) goto done;
        if (!requirement && value->type==XR_XIR_UNIT) {
            source_fail(ctx,node,XR_XIR_BAD_TYPE,"unit argument is not admitted"); goto done;
        }
        if (inferred) {
            XrXirType evidence=value->type;
            const XrXirTypeNode *wanted=xr_xir_callable_signature(&ctx->types,formal);
            const XrXirTypeNode *actual=xr_xir_callable_signature(&ctx->types,evidence);
            if (wanted && !wanted->flags && actual && actual->flags==XR_XIR_CALLABLE_NO_SUSPEND) {
                XrXirTypeNode ordinary=*actual; ordinary.flags=0;
                if (!source_intern_type(ctx,ordinary,&evidence)) goto done;
                XrXirStatus status=xr_xir_callable_weakening(&ctx->types,value->type,evidence,&ctx->budget.work);
                if (status!=XR_XIR_OK) { source_fail(ctx,node,status,requirement ?
                    "callable inference view is invalid" : "callable inference evidence is invalid"); goto done; }
            }
            XrXirStatus status=xr_xir_inference_observe(state,&ctx->types,(XrXirInferencePair){formal,evidence});
            if (status!=XR_XIR_OK) { source_fail(ctx,node,status,requirement ?
                "method type evidence is inconsistent" : "direct call type evidence is inconsistent"); goto done; }
        }
    }
    if (inferred) {
        if (!source_inference_result(ctx,node,state,plan->result,plan->expected_result)) goto done;
        XrXirStatus status=xr_xir_inference_finalize(state,&ctx->types,types,count);
        if (status!=XR_XIR_OK) { source_fail(ctx,node,status,requirement ?
            "cannot infer all method type arguments; supply an explicit list" :
            "cannot infer all declaration type arguments; supply an explicit list"); goto done; }
    }
    ok=true;
done:
    xr_xir_inference_dispose(state); return ok;
}

/* Temporary recipes are consumed before returning and never enter source queries. */
typedef struct SourceCallConversions {
    SourceCallFamily family;
    SourceSubstitution substitution;
    const XrXirType *formals;
    const XrXirCallableParameter *concrete;
    SourceValue *values;
    uint32_t count, value_offset;
} SourceCallConversions;
static bool source_call_conversions(SourceContext *ctx, AstNode *node, const SourceCallConversions *request) {
    if (!request->count) return true;
    if (request->count>ctx->budget.work)
        return source_fail(ctx,node,XR_XIR_BUDGET,"call conversion recipe budget exhausted");
    ctx->budget.work-=request->count;
    uint64_t bytes=0;
    uint32_t first=request->count;
    SourceConversionRecipe *recipes=NULL;
    bool ok=false;
    for (uint32_t p=0;p<request->count;++p) {
        XrXirType expected;
        if (request->family==SOURCE_CALL_FUNCTION) {
            if (!source_substitute(ctx,&request->substitution,request->formals[p],0,&expected)) goto done;
        } else expected=request->concrete[p].type;
        SourceConversionRecipe recipe;
        if (!source_conversion_plan(ctx,node,request->values[p+request->value_offset].type,
            (SourceExpectedType){true,expected},&recipe)) goto done;
        if (!recipes && recipe.needed) {
            uint64_t needed=(uint64_t)(request->count-p)*sizeof(*recipes);
            if (needed>SIZE_MAX || needed>ctx->budget.scratch_bytes) {
                source_fail(ctx,node,XR_XIR_BUDGET,"call conversion recipe budget exhausted"); goto done;
            }
            ctx->budget.scratch_bytes-=needed; bytes=needed;
            recipes=xr_calloc(request->count-p,sizeof(*recipes));
            if (!recipes) { source_fail(ctx,node,XR_XIR_OUT_OF_MEMORY,"call conversion recipe allocation failed"); goto done; }
            first=p;
        }
        if (recipes) recipes[p-first]=recipe;
    }
    for (uint32_t p=first;p<request->count;++p) {
        SourceValue *value=&request->values[p+request->value_offset];
        if (!source_conversion_emit(ctx,&recipes[p-first],value)) goto done;
        if (value->type!=recipes[p-first].target) {
            source_fail(ctx,node,XR_XIR_BAD_TYPE,request->family==SOURCE_CALL_FUNCTION ?
                "direct argument type mismatch" : "interface method argument type mismatch"); goto done;
        }
    }
    ok=true;
done:
    xr_free(recipes); ctx->budget.scratch_bytes+=bytes; return ok;
}
