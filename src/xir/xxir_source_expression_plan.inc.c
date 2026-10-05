/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_expression_plan.inc.c - Collected expression plans with read provenance
 */
static bool source_plan_soft_numeric(SourceContext *ctx,SourceExpressionPlan *plan,bool *soft) {
    if (!source_work(ctx,plan->syntax)) return false;
    *soft=plan->integer.present || plan->decimal.node || plan->syntax->type==AST_TERNARY;
    if (*soft || !plan->left) return true;
    bool left=false,right=false;
    if (!source_plan_soft_numeric(ctx,plan->left,&left)) return false;
    if (plan->syntax->type==AST_GROUPING) {*soft=left;return true;}
    if (!plan->right || (plan->syntax->type>=AST_BINARY_EQ && plan->syntax->type<=AST_BINARY_GE)) return true;
    if (!source_plan_soft_numeric(ctx,plan->right,&right)) return false;
    *soft=left && right;return true;
}
static bool source_plan_numeric_prepare(SourceContext *ctx,SourceExpressionPlan *plan,SourceExpectedType hint,bool defaults) {
    if (plan->type_ready) return true;
    if (!plan->integer.present && !plan->decimal.node) return true;
    SourceNumericRequest request={plan->integer.present ? &plan->integer : NULL,
        plan->decimal.node ? &plan->decimal : NULL,hint,defaults};
    if (!source_numeric_plan(ctx,plan->syntax,&request,&plan->numeric)) return false;
    if (plan->numeric.ready) {plan->type_ready=true;plan->ground_type=plan->numeric.type;}
    return true;
}
static bool source_plan_binary_prepare(SourceContext *ctx,SourceExpressionPlan *plan,SourceExpectedType hint,bool defaults) {
    if (plan->syntax->type==AST_TERNARY && plan->left && plan->right) {
        if (plan->conditional) return true;
        if (!source_work(ctx,plan->syntax)) return false;
        if (!source_plan_binary_prepare(ctx,plan->left,hint,defaults) ||
            !source_plan_numeric_prepare(ctx,plan->left,hint,defaults) ||
            !source_plan_binary_prepare(ctx,plan->right,hint,defaults) ||
            !source_plan_numeric_prepare(ctx,plan->right,hint,defaults)) return false;
        if (!plan->left->type_ready || !plan->right->type_ready) return true;
        SourceConditionalRecipe recipe;
        if (!source_conditional_plan(ctx,plan->syntax,plan->left->ground_type,plan->right->ground_type,hint,&recipe)) return false;
        SourceConditionalRecipe *recorded_recipe=source_recipe_storage(ctx,1,sizeof(*recorded_recipe));
        if (!recorded_recipe) return false;
        *recorded_recipe=recipe;plan->conditional=recorded_recipe;plan->type_ready=true;plan->ground_type=recipe.result;
        return true;
    }
    if (plan->syntax->type==AST_GROUPING && plan->left) {
        if (!source_plan_binary_prepare(ctx,plan->left,hint,defaults) ||
            !source_plan_numeric_prepare(ctx,plan->left,hint,defaults)) return false;
        if (plan->left->type_ready) {plan->type_ready=true;plan->ground_type=plan->left->ground_type;}
        return true;
    }
    /* Logical and other structural plans own their type and control flow.
     * Only declared arithmetic, bitwise and comparison nodes take an operator recipe. */
    if (plan->syntax->type<AST_BINARY_ADD || plan->syntax->type>AST_BINARY_GE) return true;
    if (plan->binary || !plan->left || !plan->right) return true;
    if (!source_work(ctx,plan->syntax)) return false;
    SourceExpressionPlan *left=plan->left,*right=plan->right;
    bool left_literal=left->integer.present || left->decimal.node;
    bool right_literal=right->integer.present || right->decimal.node;
    if (defaults && !hint.present && !left->type_ready && !right->type_ready && left_literal!=right_literal)
        return true;
    bool shift=plan->syntax->type==AST_BINARY_LSHIFT || plan->syntax->type==AST_BINARY_RSHIFT;
    SourceExpectedType left_hint=hint,right_hint=hint;
    if (left->type_ready) right_hint=(SourceExpectedType){!shift,left->ground_type, false};
    if (right->type_ready && !shift) left_hint=(SourceExpectedType){true,right->ground_type, false};
    if (!hint.present && !left->type_ready && !right->type_ready && (left->decimal.node || right->decimal.node))
        left_hint=right_hint=(SourceExpectedType){defaults,XR_XIR_F64, false};
    if (!source_plan_binary_prepare(ctx,left,left_hint,defaults) ||
        !source_plan_numeric_prepare(ctx,left,left_hint,defaults)) return false;
    if (left->type_ready && !shift) right_hint=(SourceExpectedType){true,left->ground_type, false};
    if (!source_plan_binary_prepare(ctx,right,right_hint,defaults) ||
        !source_plan_numeric_prepare(ctx,right,right_hint,defaults)) return false;
    if (!left->type_ready || !right->type_ready) return true;
    SourceBinaryRecipe recipe;
    if (!source_binary_plan(ctx,plan->syntax,plan->syntax->type,left->ground_type,right->ground_type,&recipe)) return false;
    SourceBinaryRecipe *recorded_recipe=source_recipe_storage(ctx,1,sizeof(*recorded_recipe));
    if (!recorded_recipe) return false;
    *recorded_recipe=recipe;plan->binary=recorded_recipe;plan->type_ready=true;plan->ground_type=recipe.result;
    return true;
}
#include "xxir_source_ground_type.inc.c"
static SourceExpressionPlan *source_plan_collect(SourceContext *ctx, AstNode *node, SourceExpectedType context) {
    if (!node || !source_work(ctx,node)) return NULL;
    SourceFunction *body=&ctx->bodies[ctx->function];
    if (body->region_sealed || body->expression_count==UINT32_MAX) {
        source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"expression region is not open"); return NULL;
    }
    if (!body->expressions || body->expressions->count==32) {
        SourceExpressionStorage *storage=source_recipe_storage(ctx,1,sizeof(*storage));
        if (!storage) return NULL;
        storage->next=body->expressions;body->expressions=storage;
    }
    SourceExpressionPlan *parent=ctx->active_expression;
    SourceExpressionPlan *plan=&body->expressions->expressions[body->expressions->count++];
    plan->parent=parent && parent->owner==ctx->function ? parent : NULL;
    plan->syntax=node;plan->owner=ctx->function;plan->identity=body->expression_count++;
    if (node->type==AST_THIS_EXPR || node->type==AST_VARIABLE) {
        plan->binding=visible_name(ctx,node->type==AST_THIS_EXPR ? "this" : node->as.variable.name);
        if (source_name_ready(plan->binding)) {
            plan->type_ready=true;plan->ground_type=source_symbol_type(ctx,plan->binding);
            if (plan->binding->kind==SOURCE_LOCAL) {
                plan->read_binding=plan->binding;plan->read_declared=plan->binding->type;
                plan->read_prefix=source_fact_depth(ctx,plan->binding);plan->read_claims=plan->read_prefix;
                plan->read_epoch=source_epoch(ctx,ctx->epochs,plan->binding);
            }
        }
    } else if (node->type==AST_LITERAL_TRUE || node->type==AST_LITERAL_FALSE || node->type==AST_LITERAL_STRING || node->type==AST_LITERAL_RUNE) {
        plan->type_ready=true;plan->ground_type=node->type==AST_LITERAL_STRING ? XR_XIR_STRING : node->type==AST_LITERAL_RUNE ? XR_XIR_RUNE : XR_XIR_BOOL;
    } else if (node->type==AST_NEW_EXPR && node->as.new_expr.class_name) {
        plan->binding=visible_name(ctx,node->as.new_expr.module_name ?
            node->as.new_expr.module_name : node->as.new_expr.class_name);
    } else if (node->type==AST_CALL_EXPR && node->as.call_expr.callee && node->as.call_expr.callee->type==AST_VARIABLE) {
        plan->binding=visible_name(ctx,node->as.call_expr.callee->as.variable.name);
        if (plan->binding && plan->binding->kind==SOURCE_FUNCTION && !ctx->generics[plan->binding->index].parameter_count) {
            plan->type_ready=true;plan->ground_type=ctx->functions[plan->binding->index].result;
        }
    }
    if (!plan->type_ready) {
        SourceExpectedType ground;
        if (!source_ground_type(ctx,node,0,&ground)) return NULL;
        if (ground.present) {plan->type_ready=true;plan->ground_type=ground.type;}
    }
    plan->expected=context;
    plan->entry=body->block_count ? body->current_block : (SourceBlockReference){ctx->function,0};
    plan->state=SOURCE_TERM_UNRESOLVED;
    if (!source_direct_integer(ctx,node,&plan->integer) || !source_direct_decimal(ctx,node,&plan->decimal)) {
        plan->state=SOURCE_TERM_FAILED;return NULL;
    }
    switch(node->type) {
    case AST_ASSIGNMENT: {
        SourceName *binding=visible_name(ctx,node->as.assignment.name);plan->binding=binding;
        SourceExpectedType hint={binding!=NULL,binding?binding->type:XR_XIR_UNIT,false};
        ctx->active_expression=plan;
        plan->left=source_plan_collect(ctx,node->as.assignment.value,hint);
        ctx->active_expression=parent;
        if (!plan->left) return NULL;
        if (binding && !source_plan_numeric_prepare(ctx,plan->left,hint,true)) return NULL;
        if (binding && plan->left->type_ready) {
            SourceConversionRecipe recipe;
            if (!source_conversion_plan(ctx,node,plan->left->ground_type,hint,&recipe)) return NULL;
            plan->type_ready=true;plan->ground_type=binding->type;
            plan->present_prefix=recipe.needed?0:plan->left->present_prefix;
            if (recipe.needed && recipe.operation==XR_XIR_NULLABLE_SOME)
                plan->present_prefix=plan->left->present_prefix+1;
        }
        break;
    }
    case AST_FORCE_UNWRAP:case AST_NULLISH_COALESCE:case AST_UNARY_NOT:
    case AST_BINARY_AND:case AST_BINARY_OR: {
        if (ctx->depth>=128) {source_fail(ctx,node,XR_XIR_BUDGET,"expression collection depth exhausted");return NULL;}
        ctx->active_expression=plan;++ctx->depth;
        bool unary=node->type==AST_FORCE_UNWRAP || node->type==AST_UNARY_NOT;
        plan->left=source_plan_collect(ctx,unary?node->as.unary.operand:node->as.binary.left,(SourceExpectedType){0});
        if (plan->left && (node->type==AST_BINARY_AND || node->type==AST_BINARY_OR)) {
            SourceFact *saved=ctx->facts;SourceEpoch *saved_epochs=ctx->epochs;
            if (source_condition_enter(ctx,plan->left,node->type==AST_BINARY_AND))
                {
                plan->right=source_plan_collect(ctx,node->as.binary.right,(SourceExpectedType){0});
                if (plan->right && !source_condition_prepare(ctx,plan->right)) plan->right=NULL;
            }
            ctx->facts=saved;ctx->epochs=saved_epochs;
        }
        --ctx->depth;ctx->active_expression=parent;
        if (!plan->left || ((node->type==AST_BINARY_AND || node->type==AST_BINARY_OR) && !plan->right)) return NULL;
        if (node->type==AST_FORCE_UNWRAP && plan->left->type_ready) {
            source_read_forward(plan,plan->left);
            plan->type_ready=true;plan->ground_type=plan->left->ground_type;
            if (plan->read_claims) --plan->read_claims;
            else if (xr_xir_type_is_nullable(&ctx->types,plan->ground_type))
                plan->ground_type=xr_xir_nullable_element(&ctx->types,plan->ground_type);
        }else if (node->type!=AST_NULLISH_COALESCE && node->type!=AST_FORCE_UNWRAP) {
            plan->type_ready=true;plan->ground_type=XR_XIR_BOOL;
        }
        break;
    }
    case AST_MATCH_EXPR:
        if (ctx->depth>=128) {source_fail(ctx,node,XR_XIR_BUDGET,"expression collection depth exhausted");return NULL;}
        ctx->active_expression=plan;++ctx->depth;
        plan->left=source_plan_collect(ctx,node->as.match_expr.expr,(SourceExpectedType){false,XR_XIR_UNIT, false});
        bool match_ok=plan->left && source_match_prepare(ctx,plan);
        --ctx->depth;ctx->active_expression=parent;
        if (!match_ok) return NULL;
        break;
    case AST_TERNARY:
        if (ctx->depth>=128) {source_fail(ctx,node,XR_XIR_BUDGET,"expression collection depth exhausted");return NULL;}
        ctx->active_expression=plan;++ctx->depth;
        plan->condition=source_plan_collect(ctx,node->as.ternary.condition,(SourceExpectedType){false,XR_XIR_UNIT, false});
        {
            /* Each arm is typed under the facts of its own direction (N-7). */
            SourceFact *entry_facts=ctx->facts;SourceEpoch *entry_epochs=ctx->epochs;
            if (plan->condition && source_condition_enter(ctx,plan->condition,true))
                plan->left=source_plan_collect(ctx,node->as.ternary.true_expr,(SourceExpectedType){false,XR_XIR_UNIT, false});
            ctx->facts=entry_facts;ctx->epochs=entry_epochs;
            if (plan->left && source_condition_enter(ctx,plan->condition,false))
                plan->right=source_plan_collect(ctx,node->as.ternary.false_expr,(SourceExpectedType){false,XR_XIR_UNIT, false});
            ctx->facts=entry_facts;ctx->epochs=entry_epochs;
        }
        --ctx->depth;ctx->active_expression=parent;
        if (!plan->condition || !plan->left || !plan->right || !source_plan_binary_prepare(ctx,plan,(SourceExpectedType){false,XR_XIR_UNIT, false},false)) return NULL;
        break;
    case AST_GROUPING:
        if (plan->integer.present || plan->decimal.node) break;
        /* Nonliteral grouping owns its child; no re-collection during completion. */
        /* fall through */
    case AST_BINARY_ADD:case AST_BINARY_SUB:case AST_BINARY_MUL:case AST_BINARY_DIV:case AST_BINARY_MOD:
    case AST_BINARY_BAND:case AST_BINARY_BOR:case AST_BINARY_BXOR:case AST_BINARY_LSHIFT:case AST_BINARY_RSHIFT:
    case AST_BINARY_EQ:case AST_BINARY_NE:case AST_BINARY_LT:case AST_BINARY_LE:case AST_BINARY_GT:case AST_BINARY_GE:
        if (ctx->depth>=128) {source_fail(ctx,node,XR_XIR_BUDGET,"expression collection depth exhausted");return NULL;}
        ctx->active_expression=plan;++ctx->depth;
        plan->left=source_plan_collect(ctx,node->type==AST_GROUPING ? node->as.grouping : node->as.binary.left,(SourceExpectedType){false,XR_XIR_UNIT, false});
        if (plan->left && node->type!=AST_GROUPING) {
            SourceFact *facts=ctx->facts;SourceEpoch *epochs=ctx->epochs;
            if (source_planned_effects(ctx,plan->left,0))
                plan->right=source_plan_collect(ctx,node->as.binary.right,(SourceExpectedType){false,XR_XIR_UNIT,false});
            ctx->facts=facts;ctx->epochs=epochs;
        }
        --ctx->depth;ctx->active_expression=parent;
        if (!plan->left || (node->type!=AST_GROUPING && !plan->right) || !source_plan_binary_prepare(ctx,plan,(SourceExpectedType){false,XR_XIR_UNIT, false},false)) return NULL;
        if (node->type==AST_GROUPING) source_read_forward(plan,plan->left);
        if (node->type>=AST_BINARY_EQ && node->type<=AST_BINARY_GE) {plan->type_ready=true;plan->ground_type=XR_XIR_BOOL;}
        break;
    default:break;
    }
    return plan;
}
static bool source_plan_complete(SourceContext *ctx, SourceExpressionPlan *plan, SourceValue *value) {
    if (!plan || plan->owner!=ctx->function || ctx->bodies[ctx->function].region_sealed)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"expression plan has no active owner");
    if (plan->state==SOURCE_TERM_GROUND) { *value=plan->value; return true; }
    if (plan->state!=SOURCE_TERM_UNRESOLVED)
        return source_fail(ctx,plan->syntax,XR_XIR_BAD_STRUCTURE,"expression plan cannot be retried");
    AstNode *node=plan->syntax;
    if (ctx->depth>=128) return source_fail(ctx,node,XR_XIR_BUDGET,"source expression depth exhausted");
    SourceExpectedType context=plan->expected;
    SourceConversionRecipe checked_conversion={0};
    if (plan->read_binding && (node->type==AST_VARIABLE || node->type==AST_THIS_EXPR)) {
        uint64_t epoch=source_epoch(ctx,ctx->epochs,plan->read_binding);
        if (ctx->diagnostic.status!=XR_XIR_OK) return false;
        if (plan->read_epoch!=epoch && plan->read_prefix) {
            uint32_t actual=source_fact_depth(ctx,plan->read_binding);
            if (ctx->diagnostic.status!=XR_XIR_OK) return false;
            if (actual<plan->read_prefix)
                return source_fail(ctx,node,XR_XIR_BAD_TYPE,"nullable read facts invalidated by an earlier write");
        }
        plan->read_epoch=epoch;
    }
    SourceExpressionPlan *parent=ctx->active_expression;
    SourceFunction *body=&ctx->bodies[plan->owner];
    plan->entry=body->block_count ? body->current_block : (SourceBlockReference){ctx->function,0};
    plan->state=SOURCE_TERM_CHECKING;ctx->active_expression=plan;++ctx->depth;
    bool result=plan->numeric.ready ? source_numeric_emit(ctx,&plan->numeric,&plan->value) :
        (plan->integer.present ? source_integer(ctx,node,&plan->integer,context,&plan->value) : plan->decimal.node ?
        source_decimal(ctx,&plan->decimal,context,&plan->value) : expression_body(ctx,node,context,&plan->value));
    --ctx->depth;ctx->active_expression=parent;
    if (result && plan->conversion_ready) {
        checked_conversion=plan->conversion;
        if (context.present && context.type!=checked_conversion.target)
            result=source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"prepared expression context changed");
    } else if (result) result=source_conversion_plan(ctx,node,plan->value.type,context,&checked_conversion);
    if (result && checked_conversion.needed) {
        if (checked_conversion.operation==XR_XIR_NULLABLE_SOME) ++plan->present_prefix;
        else plan->present_prefix=0;
        plan->read_binding=NULL;plan->read_declared=XR_XIR_UNIT;plan->read_prefix=0;plan->read_claims=0;
    }
    result=result && source_conversion_emit(ctx,&checked_conversion,&plan->value) && source_query_expression(ctx,node,plan->value.type);
    if (result && node->type==AST_GROUPING && plan->left)
        plan->condition_flow=plan->left->condition_flow;
    if (result && plan->value.type==XR_XIR_BOOL && !plan->condition_flow)
        result=source_condition_capture(ctx,plan);
    if (result) {
        SourceFunction *owner=&ctx->bodies[plan->owner];
        plan->exit=owner->block_count ? owner->current_block : plan->entry;
        plan->state=SOURCE_TERM_GROUND;plan->type_ready=true;plan->ground_type=plan->value.type;*value=plan->value;
    } else plan->state=SOURCE_TERM_FAILED;
    return result;
}
static bool source_plan_expression(SourceContext *ctx, AstNode *node, SourceExpectedType context, SourceValue *value) {
    SourceExpressionPlan *plan=source_plan_collect(ctx,node,context);
    return plan && source_plan_complete(ctx,plan,value);
}

static bool source_plan_discard(SourceContext *ctx,uint32_t first) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    for (SourceExpressionStorage *storage=body->expressions;storage;storage=storage->next) {
        for (uint32_t e=storage->count;e;--e) {
            SourceExpressionPlan *plan=&storage->expressions[e-1];
            if (!source_work(ctx,plan->syntax)) return false;
            if (plan->identity<first) return true;
            if (plan->owner!=ctx->function || (plan->state!=SOURCE_TERM_GROUND && plan->state!=SOURCE_TERM_DISCARDED))
                return source_fail(ctx,plan->syntax,XR_XIR_BAD_STRUCTURE,"uncompleted expression cannot be discarded");
            plan->state=SOURCE_TERM_DISCARDED;
        }
    }
    return true;
}
