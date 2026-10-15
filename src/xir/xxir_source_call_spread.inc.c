/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_call_spread.inc.c - Authentic tuple argument projections
 *
 * KEY CONCEPT:
 *   A spread owns one original expression plan. Logical argument projections
 *   share that result and enter the ordinary inference and conversion worker.
 */
typedef struct SourceCallSpread {
    SourceExpressionPlan *source;
    uint32_t first,count;
    XrXirType *known_fields;
    bool *field_known;
    bool prepared;
} SourceCallSpread;
typedef struct SourceCallProjection { SourceCallSpread *spread;uint32_t field; } SourceCallProjection;
typedef struct SourceCallEntry {
    AstNode *syntax;
    XrCallArgAccess access;
    SourceCallArgumentPlan plan;
    bool reference;
} SourceCallEntry;
typedef struct SourceCallExpansion {
    SourceCallEntry *entries;
    SourceCallSpread *spreads;
    uint32_t spread_count,next_spread;
} SourceCallExpansion;
static bool source_ground_type(SourceContext *ctx,AstNode *node,uint32_t depth,SourceExpectedType *output);
static AstNode *source_call_argument_syntax(const SourceCallSyntax *call,uint32_t index) {
    return call->expanded ? call->expanded->entries[index].syntax : call->arguments[index];
}
static XrCallArgAccess source_call_argument_access(const SourceCallSyntax *call,uint32_t index) {
    return call->expanded ? call->expanded->entries[index].access :
        call->arg_accesses ? call->arg_accesses[index] : XR_CALL_ARG_PLAIN;
}
static SourceName *source_call_spread_function(SourceContext *ctx,AstNode *callee) {
    SourceName *binding=NULL;
    if (callee && callee->type==AST_VARIABLE) binding=visible_name(ctx,callee->as.variable.name);
    else if (callee && callee->type==AST_MEMBER_ACCESS && callee->as.member_access.object &&
        callee->as.member_access.object->type==AST_VARIABLE) {
        SourceName *space=visible_name(ctx,callee->as.member_access.object->as.variable.name);
        if (space && space->kind==SOURCE_MODULE) binding=imported_declaration(ctx,space,callee->as.member_access.name);
    }
    if (binding && binding->kind==SOURCE_IMPORT) binding=imported_declaration(ctx,binding,binding->imported);
    return binding && binding->kind==SOURCE_FUNCTION ? binding : NULL;
}
/* Shape facts reserve operands only. Completion checks the real tuple type
 * and every declaration, conversion and access through the ordinary plan. */
static bool source_call_spread_shape(SourceContext *ctx,AstNode *node,uint32_t depth,uint32_t *arity) {
    if (!node || !source_work(ctx,node)) return false;
    if (depth>=128) return source_fail(ctx,node,XR_XIR_BUDGET,"call spread shape depth exhausted");
    if (node->type==AST_GROUPING) return source_call_spread_shape(ctx,node->as.grouping,depth+1,arity);
    if (node->type==AST_TUPLE_LITERAL) {
        const TupleLiteralNode *tuple=&node->as.tuple_literal;
        if (tuple->count<0 || (tuple->count && !tuple->elements))
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread Tuple entries are missing");
        *arity=0;
        for (int f=0;f<tuple->count;++f) {
            AstNode *field=tuple->elements[f];uint32_t count=1;
            if (!field) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread Tuple entry is missing");
            if (field->type==AST_SPREAD_EXPR && !source_call_spread_shape(ctx,field->as.spread_expr.expr,depth+1,&count)) return false;
            if (!source_work(ctx,field) || count>65536-*arity)
                return source_fail(ctx,node,XR_XIR_BUDGET,"call expanded arity exhausted");
            *arity+=count;
        }
        return true;
    }
    SourceExpectedType ground;
    if (!source_ground_type(ctx,node,depth,&ground)) return false;
    if (!ground.present && node->type==AST_CALL_EXPR) {
        const CallExprNode *call=&node->as.call_expr;
        SourceName *binding=source_call_spread_function(ctx,call->callee);
        if (ctx->diagnostic.status!=XR_XIR_OK) return false;
        if (binding) {
            XrXirType result=ctx->functions[binding->index].result;
            uint32_t parameters=ctx->generics[binding->index].parameter_count;
            if (call->type_arg_count && (uint32_t)call->type_arg_count==parameters && call->type_args) {
                XrXirType *types=source_alloc(ctx,parameters,sizeof(*types));
                if (!types) return false;
                for (uint32_t p=0;p<parameters;++p)
                    if (!source_work(ctx,node) || !source_type(ctx,call->type_args[p],&types[p])) return false;
                if (!source_substitute(ctx,&(SourceSubstitution){types,parameters},result,0,&result)) return false;
            } else if ((uint32_t)result>=XR_XIR_TYPE_PARAMETER_BASE &&
                (uint32_t)result-XR_XIR_TYPE_PARAMETER_BASE<parameters) {
                const XrXirFunction *function=&ctx->functions[binding->index];
                uint32_t count=call->arg_count>0 ? (uint32_t)call->arg_count : 0;
                for (uint32_t a=0;a<count && a<function->parameter_count;++a) {
                    if (!source_work(ctx,node)) return false;
                    if (function->parameters[a]==result && call->arguments[a]->type!=AST_SPREAD_EXPR)
                        return source_call_spread_shape(ctx,call->arguments[a],depth+1,arity);
                }
            }
            ground=(SourceExpectedType){true,result,false,false};
        }
    }
    if (ground.present && ground.type==XR_XIR_UNIT) { *arity=0;return true; }
    const XrXirTypeNode *tuple=ground.present ? xr_xir_tuple_signature(&ctx->types,ground.type) : NULL;
    if (!tuple) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"call spread requires a statically known Tuple");
    *arity=tuple->parameter_count;return true;
}
static bool source_call_spread_field_hints(SourceContext *ctx,AstNode *node,uint32_t count,
    uint32_t depth,XrXirType *types,bool *known) {
    if (!node || !source_work(ctx,node)) return false;
    if (depth>=128) return source_fail(ctx,node,XR_XIR_BUDGET,"call spread field depth exhausted");
    if (node->type==AST_GROUPING)
        return source_call_spread_field_hints(ctx,node->as.grouping,count,depth+1,types,known);
    if (node->type==AST_TUPLE_LITERAL) {
        const TupleLiteralNode *tuple=&node->as.tuple_literal;uint32_t at=0;
        for (int i=0;i<tuple->count;++i) {
            AstNode *child=tuple->elements[i];uint32_t width=1;
            if (!source_work(ctx,child)) return false;
            if (child->type==AST_SPREAD_EXPR) {
                if (!source_call_spread_shape(ctx,child->as.spread_expr.expr,depth+1,&width)) return false;
                if (width>count-at) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread field exceeds its static shape");
                if (width && !source_call_spread_field_hints(ctx,child->as.spread_expr.expr,width,depth+1,types+at,known+at)) return false;
            } else {
                if (at>=count) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread field exceeds its static shape");
                SourceExpectedType hint={0};
                if (child->type==AST_LITERAL_STRING || child->type==AST_LITERAL_RUNE ||
                    child->type==AST_LITERAL_TRUE || child->type==AST_LITERAL_FALSE)
                    hint=(SourceExpectedType){true,child->type==AST_LITERAL_STRING ? XR_XIR_STRING :
                        child->type==AST_LITERAL_RUNE ? XR_XIR_RUNE : XR_XIR_BOOL,false,false};
                else if (!source_ground_type(ctx,child,depth+1,&hint)) return false;
                if (hint.present && xr_xir_type_is_nullable(&ctx->types,hint.type) && child->type==AST_VARIABLE) {
                    SourceName *binding=visible_name(ctx,child->as.variable.name);
                    if (source_fact_depth(ctx,binding)) hint.present=false;
                }
                if (ctx->diagnostic.status!=XR_XIR_OK) return false;
                types[at]=hint.type;known[at]=hint.present;
            }
            at+=width;
        }
        return at==count || source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread field count changed");
    }
    SourceExpectedType ground;
    if (!source_ground_type(ctx,node,depth,&ground)) return false;
    const XrXirTypeNode *tuple=ground.present ? xr_xir_tuple_signature(&ctx->types,ground.type) : NULL;
    if (tuple && tuple->parameter_count==count) for (uint32_t f=0;f<count;++f) {
        if (!source_work(ctx,node)) return false;
        types[f]=tuple->parameters[f].type;known[f]=true;
    }
    return true;
}
static bool source_call_expand(SourceContext *ctx,AstNode *node,SourceCallSyntax *call,
    const SourceCallPlan *formal,uint32_t parameter_count) {
    bool present=false;
    for (int a=0;a<call->arg_count;++a) {
        if (!source_work(ctx,node)) return false;
        if (call->arguments[a] && call->arguments[a]->type==AST_SPREAD_EXPR) present=true;
    }
    if (!present) return true;
    SourceCallExpansion *expanded=source_recipe_storage(ctx,1,sizeof(*expanded));
    if (!expanded) return false;
    expanded->entries=parameter_count ? source_recipe_storage(ctx,parameter_count,sizeof(*expanded->entries)) : NULL;
    expanded->spreads=source_recipe_storage(ctx,(uint32_t)call->arg_count,sizeof(*expanded->spreads));
    if ((parameter_count && !expanded->entries) || !expanded->spreads) return false;
    SourceFact *facts=ctx->facts;SourceEpoch *epochs=ctx->epochs;uint32_t count=0;bool ok=false;
    for (int a=0;a<call->arg_count;++a) {
        AstNode *input=call->arguments[a];
        if (!input || !source_work(ctx,input)) goto done;
        XrCallArgAccess access=call->arg_accesses ? call->arg_accesses[a] : XR_CALL_ARG_PLAIN;
        SourceCallSpread *spread=NULL;uint32_t arity=1;
        if (input->type==AST_SPREAD_EXPR) {
            if (access!=XR_CALL_ARG_PLAIN) {source_fail(ctx,input,XR_XIR_BAD_TYPE,"call spread supplies ordinary READ values");goto done;}
            spread=&expanded->spreads[expanded->spread_count++];spread->first=count;
            if (!input->as.spread_expr.expr) {source_fail(ctx,input,XR_XIR_BAD_STRUCTURE,"call spread source is missing");goto done;}
            spread->source=source_plan_collect(ctx,input->as.spread_expr.expr,(SourceExpectedType){0});
            if (!spread->source) goto done;
            XrXirType ground=spread->source->ground_type;
            for (uint32_t claim=0;claim<spread->source->read_claims && xr_xir_type_is_nullable(&ctx->types,ground);++claim) {
                if (!source_work(ctx,input)) goto done;
                ground=xr_xir_nullable_element(&ctx->types,ground);
            }
            const XrXirTypeNode *known=spread->source->type_ready ? xr_xir_tuple_signature(&ctx->types,ground) : NULL;
            if (known) {
                arity=known->parameter_count;
            } else if (!source_call_spread_shape(ctx,input->as.spread_expr.expr,0,&arity)) goto done;
            spread->known_fields=arity ? source_recipe_storage(ctx,arity,sizeof(*spread->known_fields)) : NULL;
            spread->field_known=arity ? source_recipe_storage(ctx,arity,sizeof(*spread->field_known)) : NULL;
            if (arity && (!spread->known_fields || !spread->field_known)) goto done;
            for (uint32_t f=0;f<arity;++f) {
                if (!source_work(ctx,input)) goto done;
                if (known) {spread->known_fields[f]=known->parameters[f].type;spread->field_known[f]=true;}
            }
            if (!known && arity && !source_call_spread_field_hints(ctx,input->as.spread_expr.expr,arity,0,
                    spread->known_fields,spread->field_known)) goto done;
            spread->count=arity;
            if (!source_planned_effects(ctx,spread->source,0)) goto done;
        }
        if (formal->parameter_offset>parameter_count || arity>parameter_count-formal->parameter_offset-count) {
            source_fail(ctx,input,XR_XIR_BAD_TYPE,"too many call arguments");goto done;
        }
        for (uint32_t f=0;f<arity;++f) {
            uint32_t parameter=count+formal->parameter_offset;
            SourceCallEntry *entry=&expanded->entries[count++];entry->syntax=input;entry->access=access;
            entry->reference=(formal->family==SOURCE_CALL_FUNCTION || formal->family==SOURCE_CALL_INDIRECT) &&
                xr_xir_type_is_cell(&ctx->types,formal->function_parameters[parameter]);
            if (spread) {
                if (entry->reference) {source_fail(ctx,input,XR_XIR_BAD_TYPE,"call spread cannot satisfy a ref parameter");goto done;}
                SourceCallProjection *projection=source_recipe_storage(ctx,1,sizeof(*projection));
                SourceExpressionPlan *plan=source_plan_collect(ctx,input,(SourceExpectedType){0});
                if (!projection || !plan) goto done;
                *projection=(SourceCallProjection){spread,f};plan->call_projection=projection;
                if (spread->field_known[f]) {plan->type_ready=true;plan->ground_type=spread->known_fields[f];}
                entry->plan.value=plan;
            } else if (entry->reference) {
                if (!source_reference_effects(ctx,input,&entry->plan.reference)) goto done;
            } else {
                entry->plan.value=source_plan_collect(ctx,input,(SourceExpectedType){0});
                if (!entry->plan.value || !source_planned_effects(ctx,entry->plan.value,0)) goto done;
            }
        }
    }
    call->arg_count=(int)count;call->expanded=expanded;ok=true;
done:
    ctx->facts=facts;ctx->epochs=epochs;return ok;
}
static bool source_call_spread_read(SourceContext *ctx,SourceCallSpread *spread,SourceValue *value) {
    if (!source_plan_complete(ctx,spread->source,value)) return false;
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(&ctx->types,value->type);
    if ((!tuple && (value->type!=XR_XIR_UNIT || spread->count)) || (tuple && tuple->parameter_count!=spread->count))
        return source_fail(ctx,spread->source->syntax,XR_XIR_BAD_TYPE,"call spread arity differs from its static shape");
    return true;
}
static bool source_call_projection_complete(SourceContext *ctx,SourceExpressionPlan *plan,SourceValue *value) {
    SourceCallProjection projection=*plan->call_projection;SourceValue tuple;
    if (!source_call_spread_read(ctx,projection.spread,&tuple)) return false;
    const XrXirTypeNode *type=xr_xir_tuple_signature(&ctx->types,tuple.type);
    if (!type || projection.field>=type->parameter_count)
        return source_fail(ctx,plan->syntax,XR_XIR_BAD_STRUCTURE,"call spread projection exceeds its checked Tuple");
    XrXirType field=type->parameters[projection.field].type;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TUPLE_FIELD,field,
        {tuple.id,0},{0},projection.field,{0}},value);
}
static bool source_call_spread_events(SourceContext *ctx,AstNode *node,const SourceCallSyntax *call,
    const SourceCallPlan *formal,XrXirInferenceState *state,uint32_t position) {
    if (!call->expanded) return true;
    while (call->expanded->next_spread<call->expanded->spread_count) {
        SourceCallSpread *spread=&call->expanded->spreads[call->expanded->next_spread];
        if (!source_work(ctx,node)) return false;
        if (spread->first>position) break;
        if (spread->first!=position || spread->prepared)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call spread event order changed");
        ++call->expanded->next_spread;
        spread->prepared=true;
        if (!spread->count) {SourceValue empty;if (!source_call_spread_read(ctx,spread,&empty)) return false;continue;}
        SourceExpressionPlan *literal=spread->source;
        while (literal->syntax->type==AST_GROUPING && literal->left) {
            if (!source_work(ctx,literal->syntax)) return false;
            literal=literal->left;
        }
        if (literal->syntax->type!=AST_TUPLE_LITERAL) continue;
        XrXirCallableParameter *fields=source_recipe_storage(ctx,spread->count,sizeof(*fields));
        if (!fields) return false;
        bool complete=true;
        for (uint32_t f=0;f<spread->count;++f) {
            if (spread->field_known[f]) {fields[f].type=spread->known_fields[f];continue;}
            uint32_t parameter=spread->first+f+formal->parameter_offset;
            XrXirType wanted=formal->family==SOURCE_CALL_REQUIREMENT ? formal->requirement_parameters[parameter].type : formal->function_parameters[parameter];
            SourceSubstitution substitution=formal->substitution;
            if (state) {
                XrXirInferenceKnown known={0};
                XrXirStatus status=xr_xir_compile_inference_expected_known(state,&ctx->types,wanted,&known);
                if (status!=XR_XIR_OK) return source_fail(ctx,node,status,"call spread context is invalid");
                if (!known.known) {complete=false;break;}
                substitution=(SourceSubstitution){known.arguments,known.argument_count};
            }
            if (formal->family==SOURCE_CALL_INDIRECT) fields[f].type=wanted;
            else if (!source_substitute(ctx,&substitution,wanted,0,&fields[f].type)) return false;
        }
        if (complete) {
            SourceTuplePrefix *prefix=source_recipe_storage(ctx,1,sizeof(*prefix));
            if (!prefix) return false;
            *prefix=(SourceTuplePrefix){fields,spread->count,false};literal->tuple_prefix=prefix;
        }
    }
    return true;
}
