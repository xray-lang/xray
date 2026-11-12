/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_select.inc.c - Once-evaluated channels and typed arm bindings
 *
 * KEY CONCEPT:
 *   The winner tag selects one lexical branch. Every payload keeps its actual
 *   Channel element type, including an additional outer Nullable layer.
 */
typedef struct SourceSelectArm {AstNode *node;uint32_t tag;} SourceSelectArm;
typedef struct SourceSelectPlan {
    SourceSelectArm *arms;
    SourceValue *operands;
    uint32_t count,channels,mode;
    XrXirType result;
} SourceSelectPlan;
static bool source_select_prepare(SourceContext *ctx,AstNode *node,SourceSelectPlan *plan) {
    SelectStmtNode *syntax=&node->as.select_stmt;
    if(syntax->case_count<=0||syntax->case_count>65536||!syntax->cases)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"select requires its actual arms");
    *plan=(SourceSelectPlan){.count=(uint32_t)syntax->case_count};
    plan->arms=source_alloc(ctx,plan->count,sizeof(*plan->arms));if(!plan->arms)return false;
    for(uint32_t a=0;a<plan->count;++a) {
        if(!(a%64u)&&!source_request_poll(ctx,node))return false;
        AstNode *arm_node=syntax->cases[a];
        if(!arm_node||arm_node->type!=AST_SELECT_CASE)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"select arm has no syntax owner");
        SelectCaseNode *arm=&arm_node->as.select_case;
        if(!arm->body||arm->body->type!=AST_BLOCK||
            (arm->is_default&&arm->is_timeout))
            return source_fail(ctx,arm_node,XR_XIR_BAD_STRUCTURE,"select arm requires a real block");
        if(arm->is_default||arm->is_timeout) {
            if(plan->mode||arm->channel||arm->var_name||(!arm->value!=!arm->is_timeout))
                return source_fail(ctx,arm_node,XR_XIR_BAD_STRUCTURE,"select requires at most one exact fallback");
            plan->mode=arm->is_timeout?2u:1u;
        } else {
            if(!arm->channel||(arm->is_send ? (!arm->value||arm->var_name) : (!arm->var_name||arm->value)))
                return source_fail(ctx,arm_node,XR_XIR_BAD_STRUCTURE,"select arm requires its channel and exact payload syntax");
            ++plan->channels;
        }
        plan->arms[a].node=arm_node;
    }
    if(!plan->channels||plan->channels>=65536u)
        return source_fail(ctx,node,XR_XIR_UNSUPPORTED,"select requires at least one supported receive arm");
    plan->operands=source_alloc(ctx,plan->channels+1,sizeof(*plan->operands));
    XrXirCallableParameter *fields=source_alloc(ctx,plan->channels+1,sizeof(*fields));
    if(!plan->operands||!fields)return false;
    fields[0]=(XrXirCallableParameter){XR_XIR_I64,XR_PARAM_READ,0};
    uint32_t channel=0;
    for(uint32_t a=0;a<plan->count;++a) {
        if(!(a%64u)&&!source_request_poll(ctx,node))return false;
        SelectCaseNode *arm=&plan->arms[a].node->as.select_case;
        if(arm->is_default||arm->is_timeout) {
            plan->arms[a].tag=plan->channels;
            if(arm->is_timeout&&!source_plan_expression(ctx,arm->value,
                (SourceExpectedType){true,XR_XIR_I64,false,false},plan->operands+plan->channels))return false;
            if(arm->is_timeout&&plan->operands[plan->channels].type!=XR_XIR_I64)
                return source_fail(ctx,arm->value,XR_XIR_BAD_TYPE,"select timeout requires i64 milliseconds");
        } else {
            SourceValue *value=plan->operands+channel;
            if(!expression(ctx,arm->channel,value))return false;
            if(!xr_xir_type_is_channel(&ctx->types,value->type))
                return source_fail(ctx,arm->channel,XR_XIR_BAD_TYPE,"select receive requires an actual Channel value");
            XrXirType element=xr_xir_channel_element(&ctx->types,value->type),nullable;
            if(!source_nullable_type(ctx,element,&nullable))return false;
            if(arm->is_send) {
                SourceValue payload[2]={*value};XrXirType pair;
                XrXirCallableParameter *members=source_alloc(ctx,2,sizeof(*members));
                if(!members)return false;
                members[0]=(XrXirCallableParameter){value->type,XR_PARAM_READ,0};
                members[1]=(XrXirCallableParameter){element,XR_PARAM_READ,0};
                if(!source_plan_expression(ctx,arm->value,(SourceExpectedType){true,element,false,false},payload+1)||
                    !source_hir_tuple_type(ctx,members,2,&pair)||
                    !source_tuple_construct(ctx,plan->arms[a].node,pair,payload,2,NULL,value))return false;
            }
            fields[channel+1]=(XrXirCallableParameter){nullable,XR_PARAM_READ,0};
            plan->arms[a].tag=channel++;
        }
    }
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,
        .parameters=fields,.parameter_count=plan->channels+1},&plan->result);
}
static bool source_select_arm(SourceContext *ctx,const SourceSelectArm *planned,SourceValue result) {
    AstNode *node=planned->node;SelectCaseNode *arm=&node->as.select_case;
    SourceName *saved=ctx->locals,*saved_scope=ctx->scope;XrHirBodyScope typed={0};
    if(!source_hir_scope_enter(ctx,node,&typed))return false;
    ctx->scope=saved;bool ok=true;
    if(!arm->is_default&&!arm->is_timeout&&!arm->is_send) {
        SourceValue nullable={0},value={0};
        ok=source_tuple_projection(ctx,node,result,planned->tag+1,&nullable)&&
            source_unwrap_value(ctx,node,nullable,&value);
        SourceName *binding=ok?source_alloc(ctx,1,sizeof(*binding)):NULL;
        if(ok&&!binding)ok=false;
        if(ok) {
            *binding=(SourceName){.name=arm->var_name,.node=node,.module=ctx->module,.type=value.type,
                .kind=value.type==XR_XIR_UNIT?SOURCE_UNIT_LOCAL:SOURCE_LOCAL,
                .index=value.type==XR_XIR_UNIT?UINT32_MAX:value.id};
            ok=source_query_declare(ctx,binding,XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,
                source_query_range(ctx,node,binding->name))&&source_hir_value_declared(ctx,node,binding,value);
            if(ok){binding->next=ctx->locals;ctx->locals=binding;source_query_binding_type(ctx,binding);}
        }
    }
    if(ok)ok=statement(ctx,arm->body,false);
    ctx->locals=saved;ctx->scope=saved_scope;source_hir_scope_leave(ctx,typed);return ok;
}
static bool source_select(SourceContext *ctx,AstNode *node) {
    SourceSelectPlan plan;SourceValue result,tag;
    if(!source_select_prepare(ctx,node,&plan)||
        !source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_CHANNEL_SELECT,.type=plan.result,
            .immediate=plan.mode},plan.operands,plan.channels+(plan.mode==2?1u:0u),&result)||
        !source_tuple_projection(ctx,node,result,0,&tag))return false;
    SourceFunction *body=ctx->bodies+ctx->function;
    SourceFact *entry=ctx->facts,*joined=NULL;SourceEpoch *epochs=ctx->epochs,*joined_epochs=NULL;
    SourcePatch *jumps=NULL;uint32_t continuing=0,frontier=body->frontier;
    for(uint32_t a=0;a<plan.count;++a) {
        if(!(a%64u)&&!source_request_poll(ctx,node))return false;
        ctx->facts=entry;ctx->epochs=epochs;body->frontier=frontier;
        uint32_t branch=UINT32_MAX;
        if(a+1<plan.count) {
            SourceValue ordinal,condition;
            if(!source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,
                .immediate=plan.arms[a].tag},&ordinal)||
                !source_binary_apply(ctx,node,AST_BINARY_EQ,tag,ordinal,&condition))return false;
            branch=body->count;
            if(!source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_BRANCH,.type=XR_XIR_UNIT,
                .args={condition.id},.targets={body->block_count,0}},NULL)||!begin_block(ctx))return false;
        }
        if(!source_select_arm(ctx,plan.arms+a,result))return false;
        if(!ctx->returned) {
            if(!continuing){joined=ctx->facts;joined_epochs=ctx->epochs;}
            else {
                if(!source_facts_join(ctx,joined,joined_epochs,ctx->facts,ctx->epochs,&joined))return false;
                joined_epochs=ctx->epochs;
            }
            SourcePatch *jump=source_alloc(ctx,1,sizeof(*jump));if(!jump)return false;
            *jump=(SourcePatch){jumps,body->count};jumps=jump;++continuing;
            if(!source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_JUMP,.type=XR_XIR_UNIT},NULL))return false;
        }
        if(branch!=UINT32_MAX) {
            body->recipes[branch].instruction.targets[1]=body->block_count;
            if(!begin_block(ctx))return false;
        }
    }
    body->frontier=frontier;ctx->returned=continuing==0;
    ctx->facts=continuing?joined:entry;ctx->epochs=continuing?joined_epochs:epochs;
    if(!continuing)return true;
    uint32_t join=body->block_count;
    return begin_block(ctx)&&patch_exits(ctx,jumps,join);
}
