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
typedef struct SourceCallStorage {
    XrXirType *types;
    SourceValue *values;
} SourceCallStorage;
_Static_assert(_Alignof(SourceMemory)>=_Alignof(XrXirType) &&
    _Alignof(SourceMemory)>=_Alignof(SourceValue) &&
    sizeof(SourceMemory)%_Alignof(SourceValue)==0 &&
    sizeof(XrXirType)%_Alignof(SourceValue)==0,"call storage view alignment");
/* Both views retain the existing source-arena lifetime; expression plans keep
 * their separate region owner and are released when that region is sealed. */
static bool source_call_storage(SourceContext *ctx,size_t types,uint64_t values,
    SourceCallStorage *output) {
    *output=(SourceCallStorage){0};
    if (types>SIZE_MAX/sizeof(XrXirType))
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"source metadata budget exhausted");
    size_t offset=(size_t)types*sizeof(XrXirType);
    if (values>(SIZE_MAX-offset)/sizeof(SourceValue))
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"source metadata budget exhausted");
    size_t bytes=offset+(size_t)values*sizeof(SourceValue);
    unsigned char *storage=bytes ? source_alloc(ctx,1,bytes) : NULL;
    if (bytes && !storage) return false;
    output->types=types ? (XrXirType *)storage : NULL;
    output->values=values ? (SourceValue *)(storage+offset) : NULL;
    return true;
}
typedef struct SourceCallPlan {
    SourceCallFamily family;
    SourceSubstitution prefix, substitution;
    const XrXirType *function_parameters;
    const XrXirCallableParameter *requirement_parameters;
    XrXirType result;
    SourceExpectedType expected_result;
    SourceValue *values;
    uint32_t parameter_offset, value_offset;
    const uint32_t *parameter_kinds;
} SourceCallPlan;
static bool source_call_evidence_view(SourceContext *ctx,XrXirType formal,XrXirType actual_type,XrXirType *evidence) {
    *evidence=actual_type;
    if (xr_xir_type_is_nullable(&ctx->types,formal) && !xr_xir_type_is_nullable(&ctx->types,actual_type))
        return source_nullable_type(ctx,actual_type,evidence);
    const XrXirTypeNode *wanted=xr_xir_callable_signature(&ctx->types,formal);
    const XrXirTypeNode *actual=xr_xir_callable_signature(&ctx->types,actual_type);
    if (wanted && !wanted->flags && actual && actual->flags==XR_XIR_CALLABLE_NO_SUSPEND) {
        XrXirTypeNode ordinary=*actual;ordinary.flags=0;
        return source_intern_type(ctx,ordinary,evidence);
    }
    return true;
}
static bool source_call_observe(SourceContext *ctx,AstNode *node,XrXirInferenceState *state,
    XrXirType formal,XrXirType actual_type,bool requirement) {
            XrXirType evidence=actual_type;
            if (!source_call_evidence_view(ctx,formal,actual_type,&evidence)) return false;
            if (evidence!=actual_type && !xr_xir_type_is_nullable(&ctx->types,evidence)) {
                XrXirStatus status=xr_xir_compile_callable_weakening(&ctx->compile, &ctx->types, actual_type, evidence);
                if (status!=XR_XIR_OK) { source_fail(ctx,node,status,requirement ?
                    "callable inference view is invalid" : "callable inference evidence is invalid"); return false; }
            }
            XrXirStatus status=xr_xir_compile_inference_observe(state, &ctx->types, (XrXirInferencePair){formal,evidence});
            if (status!=XR_XIR_OK) { source_fail(ctx,node,status,requirement ?
                "method type evidence is inconsistent" : "direct call type evidence is inconsistent"); return false; }
    return true;
}
static bool source_call_plan_arguments(SourceContext *ctx, AstNode *node, const SourceCallPlan *plan) {
    CallExprNode *call=&node->as.call_expr;
    bool requirement=plan->family==SOURCE_CALL_REQUIREMENT;
    uint32_t prefix=plan->prefix.count, count=plan->substitution.count, own=count-prefix;
    XrXirType *types=(XrXirType *)plan->substitution.types;
    bool inferred=own && !call->type_arg_count, ok=false;
    XrXirInferenceState *state=NULL;
    if (inferred) {
        XrXirInferenceRequest begin={&ctx->types,plan->prefix.types,prefix,own,
            ctx->generics[ctx->function].parameter_count,plan->parameter_kinds};
        XrXirStatus status=xr_xir_compile_inference_begin(&ctx->compile, &begin, &state);
        if (status!=XR_XIR_OK) return source_fail(ctx,node,status,
            requirement ? "method inference could not begin" : "direct call inference could not begin");
    } else for (uint32_t p=0;p<own;++p)
        if (!source_work(ctx,node) || !source_type(ctx,call->type_args[p],&types[prefix+p])) return false;
    uint32_t argument_count=(uint32_t)call->arg_count;
    SourceExpressionPlan **arguments=argument_count ? source_recipe_storage(ctx,argument_count,sizeof(*arguments)) : NULL;
    if (argument_count && !arguments) goto done;
    for (uint32_t a=0;a<argument_count;++a) {
        if (!source_work(ctx,node)) goto done;
        arguments[a]=source_plan_collect(ctx,call->arguments[a],(SourceExpectedType){false,XR_XIR_UNIT});
        if (!arguments[a]) goto done;
    }
    if (inferred) {
        /* Nullable evidence fixes an ordinary binder before narrower values
         * acquire their conversion recipes. This visits types only: all value
         * evaluation remains in the ordered completion loop below. */
        for (uint32_t a=0;a<argument_count;++a) {
            SourceExpressionPlan *argument=arguments[a];
            uint32_t parameter=a+plan->parameter_offset;
            XrXirType formal=requirement ? plan->requirement_parameters[parameter].type : plan->function_parameters[parameter];
            uint32_t id=(uint32_t)formal;
            if (!source_work(ctx,node)) goto done;
            if (!argument->type_ready || !xr_xir_type_is_nullable(&ctx->types,argument->ground_type) ||
                id<XR_XIR_TYPE_PARAMETER_BASE+prefix || id>=XR_XIR_TYPE_PARAMETER_BASE+count) continue;
            if (!source_call_observe(ctx,node,state,formal,argument->ground_type,requirement)) goto done;
        }
        for (uint32_t a=0;a<argument_count;++a) {
            SourceExpressionPlan *argument=arguments[a];
            if (!argument->type_ready) continue;
            uint32_t parameter=a+plan->parameter_offset;
            XrXirType formal=requirement ? plan->requirement_parameters[parameter].type : plan->function_parameters[parameter];
            XrXirInferenceKnown known={0};
            XrXirStatus status=xr_xir_compile_inference_expected_known(state, &ctx->types, formal, &known);
            if (status!=XR_XIR_OK) {source_fail(ctx,node,status,"ready argument context is invalid");goto done;}
            SourceExpectedType expected={false,XR_XIR_UNIT};
            if (known.known) {
                SourceSubstitution partial={known.arguments,known.argument_count};expected.present=true;
                if (!source_substitute(ctx,&partial,formal,0,&expected.type)) goto done;
            }
            if (!known.known) {
                XrXirType evidence;
                if (!source_call_evidence_view(ctx,formal,argument->ground_type,&evidence)) goto done;
                if (evidence!=argument->ground_type) expected=(SourceExpectedType){true,evidence};
            }
            if (!source_conversion_plan(ctx,argument->syntax,argument->ground_type,expected,&argument->conversion)) goto done;
            argument->conversion_ready=true;
            if (!source_call_observe(ctx,node,state,formal,argument->conversion.target,requirement)) goto done;
        }
        if (!source_inference_result(ctx,node,state,plan->result,plan->expected_result)) goto done;
        for (uint32_t a=0;a<argument_count;++a) {
            SourceExpressionPlan *argument=arguments[a];
            if (argument->conversion_ready) continue;
            bool soft=false;
            if (!source_plan_soft_numeric(ctx,argument,&soft)) goto done;
            if (!soft) continue;
            uint32_t parameter=a+plan->parameter_offset;
            XrXirType formal=requirement ? plan->requirement_parameters[parameter].type : plan->function_parameters[parameter];
            XrXirInferenceKnown known={0};
            XrXirStatus status=xr_xir_compile_inference_expected_known(state, &ctx->types, formal, &known);
            if (status!=XR_XIR_OK) {source_fail(ctx,node,status,"literal argument context is invalid");goto done;}
            SourceExpectedType expected={false,XR_XIR_UNIT};
            if (known.known) {
                SourceSubstitution partial={known.arguments,known.argument_count};expected.present=true;
                if (!source_substitute(ctx,&partial,formal,0,&expected.type)) goto done;
            }
            if (!source_plan_binary_prepare(ctx,argument,expected,true) ||
                !source_plan_numeric_prepare(ctx,argument,expected,true)) goto done;
            if (!argument->type_ready) continue;
            if (!source_conversion_plan(ctx,argument->syntax,argument->ground_type,expected,&argument->conversion)) goto done;
            argument->conversion_ready=true;
            if (!source_call_observe(ctx,node,state,formal,argument->conversion.target,requirement)) goto done;
        }
    }
    for (uint32_t a=0;a<argument_count;++a) {
        if (call->arg_accesses && call->arg_accesses[a]!=XR_CALL_ARG_PLAIN) {
            source_fail(ctx,node,XR_XIR_BAD_TYPE,requirement ? "interface method argument must be read" :
                "direct call arguments must use the declared READ contract"); goto done;
        }
        uint32_t parameter=a+plan->parameter_offset;
        XrXirType formal=requirement ? plan->requirement_parameters[parameter].type : plan->function_parameters[parameter];
        SourceExpectedType expected={false,XR_XIR_UNIT};
        if (inferred) {
            XrXirInferenceKnown known={0};
            XrXirStatus status=xr_xir_compile_inference_expected_known(state, &ctx->types, formal, &known);
            if (status!=XR_XIR_OK) { source_fail(ctx,node,status,requirement ?
                "method inference context is invalid" : "direct call inference context is invalid"); goto done; }
            if (known.known) {
                SourceSubstitution partial={known.arguments,known.argument_count};
                expected.present=true;
                if (!source_substitute(ctx,&partial,formal,0,&expected.type)) goto done;
            }
        } else {
            expected.present=true;
            if (!source_substitute(ctx,&plan->substitution,formal,0,&expected.type)) goto done;
        }
        SourceValue *value=&plan->values[a+plan->value_offset];
        arguments[a]->expected=expected;
        if (!source_plan_complete(ctx,arguments[a],value)) goto done;
        if (!requirement && value->type==XR_XIR_UNIT) {
            source_fail(ctx,node,XR_XIR_BAD_TYPE,"unit argument is not admitted"); goto done;
        }
        if (inferred && !arguments[a]->conversion_ready &&
            !source_call_observe(ctx,node,state,formal,value->type,requirement)) goto done;
    }
    if (inferred) {
        XrXirStatus status=xr_xir_compile_inference_finalize(state, &ctx->types, types, count);
        if (status!=XR_XIR_OK) { source_fail(ctx,node,status,requirement ?
            "cannot infer all method type arguments; supply an explicit list" :
            "cannot infer all declaration type arguments; supply an explicit list"); goto done; }
    }
    ok=true;
done:
    xr_xir_compile_inference_dispose(state); return ok;
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
    uint32_t first=request->count;
    SourceConversionRecipe *recipes=NULL;
    bool ok=false;
    for (uint32_t p=0;p<request->count;++p) {
        if (!source_work(ctx,node)) goto done;
        XrXirType expected;
        if (request->family==SOURCE_CALL_FUNCTION) {
            if (!source_substitute(ctx,&request->substitution,request->formals[p],0,&expected)) goto done;
        } else expected=request->concrete[p].type;
        SourceConversionRecipe recipe;
        if (!source_conversion_plan(ctx,node,request->values[p+request->value_offset].type,
            (SourceExpectedType){true,expected},&recipe)) goto done;
        if (!recipes && recipe.needed) {
            recipes=source_scratch(ctx,request->count-p,sizeof(*recipes),true);
            if (!recipes) goto done;
            first=p;
        }
        if (recipes) recipes[p-first]=recipe;
    }
    for (uint32_t p=first;p<request->count;++p) {
        if (!source_work(ctx,node)) goto done;
        SourceValue *value=&request->values[p+request->value_offset];
        if (!source_conversion_emit(ctx,&recipes[p-first],value)) goto done;
        if (value->type!=recipes[p-first].target) {
            source_fail(ctx,node,XR_XIR_BAD_TYPE,request->family==SOURCE_CALL_FUNCTION ?
                "direct argument type mismatch" : "interface method argument type mismatch"); goto done;
        }
    }
    ok=true;
done:
    xr_compile_resources_free(recipes); return ok;
}
