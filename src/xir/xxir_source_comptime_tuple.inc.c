/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_comptime_tuple.inc.c - Independent ordered constant recipes
 *
 * KEY CONCEPT:
 *   Receiving matches full type structure across owners and records ordinary
 *   target-pool recipes. No evaluator pointer or local type ID becomes data.
 */
#include "xxir_tuple.h"
#include "xxir_nullable.h"
#include "xxir_type_arena.h"
static bool source_comptime_result_type(SourceContext *ctx,AstNode *node,XrXirType type) {
    XrXirCtfeStatus status=xr_xir_compile_ctfe_type_admit(&ctx->compile,&ctx->types,type);
    if (status==XR_XIR_CTFE_OK) return true;
    if (status==XR_XIR_CTFE_BUDGET || status==XR_XIR_CTFE_OUT_OF_MEMORY)
        source_fail(ctx,node,status==XR_XIR_CTFE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET,"comptime type receiving failed");
    return false;
}
static bool source_comptime_result_value(SourceContext *ctx,AstNode *node,
    const XrXirValue *value,uint32_t arity) {
    if (!value) return false;
    if (value->type==XR_XIR_UNIT) return !value->reserved && !value->payload && !arity;
    if (!arity) return (value->type==XR_XIR_STRING || value->type==XR_XIR_BOOL || value->type==XR_XIR_RUNE ||
        xr_xir_type_is_number((XrXirType)value->type)) && xr_xir_value_argument(value,NULL,(XrXirType)value->type);
    if (!source_work_units(ctx,node,(uint64_t)arity*16)) return false;
    const XrXirTypeArena *arena=xr_xir_value_arena(value);
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(xr_xir_compile_type_arena_types(arena),(XrXirType)value->type);
    bool shape=tuple?tuple->parameter_count==arity:
        arity==1 && xr_xir_type_is_nullable(xr_xir_compile_type_arena_types(arena),(XrXirType)value->type);
    return shape && xr_xir_value_argument(value,arena,(XrXirType)value->type);
}
static bool source_comptime_type_matches(SourceContext *ctx,AstNode *node,
    const XrXirTypes *source_types,XrXirType source_type,XrXirType target) {
    if ((uint32_t)source_type<XR_XIR_CONSTRUCTED_TYPE_BASE && (uint32_t)target<XR_XIR_CONSTRUCTED_TYPE_BASE)
        return source_type==target;
    XrXirStatus status=xr_xir_compile_type_substitution_matches_between(&ctx->compile,
        source_types,&ctx->types,NULL,0,source_type,target);
    return status==XR_XIR_OK || source_fail(ctx,node,status,"comptime result changed its structural type");
}
typedef struct SourceComptimeTupleFrame {
    XrXirValue value;
    XrXirType target;
    XrXirCallableParameter *fields;
    SourceValue *payloads;
    uint32_t count,next,payload;
    bool owned;
    bool nullable;
} SourceComptimeTupleFrame;
static bool source_comptime_constant_leaf(SourceContext *ctx,AstNode *node,
    const XrXirValue *input,XrXirType target,SourceValue *output) {
    if (target==XR_XIR_UNIT) { *output=(SourceValue){0,XR_XIR_UNIT};return !input->type && !input->reserved && !input->payload; }
    if (target==XR_XIR_STRING) {
        const char *bytes=NULL;size_t length=0;
        if (!xr_xir_string_view(input,&bytes,&length)) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime string is invalid");
        char *owned=length?source_alloc(ctx,length,1):NULL;
        if ((length && !owned) || !source_copy_bytes(ctx,node,owned,bytes,length)) return false;
        return source_string_literal(ctx,node,owned,length,output);
    }
    if (xr_xir_float_bits(target) && !xr_xir_float_constant_payload_valid(target,input->payload))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime floating result has no canonical literal encoding");
    XrXirOp op=target==XR_XIR_BOOL?XR_XIR_CONST_BOOL:target==XR_XIR_RUNE?XR_XIR_CONST_RUNE:xr_xir_float_bits(target)?XR_XIR_CONST_FLOAT:XR_XIR_CONST_INT;
    return source_recipe_record(ctx,(XrXirInstruction){op,target,{0},{0},input->payload,{0}},output);
}
static bool source_comptime_constant(SourceContext *ctx,AstNode *node,
    const XrXirValue *input,XrXirType target,SourceValue *output) {
    SourceComptimeTupleFrame *stack=NULL;uint32_t depth=0,capacity=0;
    XrXirValue current=*input;bool current_owned=false,ok=true;SourceValue result={0};
    for (;;) {
        if (!source_work_units(ctx,node,sizeof(SourceComptimeTupleFrame)+8)) { ok=false;break; }
        const XrXirTypeNode *tuple=xr_xir_type_node(&ctx->types,target);
        bool nullable=tuple && tuple->kind==XR_XIR_TYPE_NULLABLE;
        if (nullable) {
            bool some=false;const XrXirValue *payload=NULL;
            if (!source_work_units(ctx,node,16)) { ok=false;break; }
            if (!xr_xir_nullable_view(&current,&some,&payload)) { ok=source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime Nullable is invalid");break; }
            if (!some) {
                if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NULLABLE_NONE,target,{0},{0},0,{0}},&result)) { ok=false;break; }
                if (current_owned) xr_xir_value_drop(&current);current=(XrXirValue){0};current_owned=false;
                goto receive_parent;
            }
        }
        if (nullable || (tuple && tuple->kind==XR_XIR_TYPE_TUPLE)) {
            if (depth==capacity) {
                if (capacity>UINT32_MAX/2) { ok=source_fail(ctx,node,XR_XIR_BUDGET,"comptime recipe stack exhausted");break; }
                uint32_t next=capacity?capacity*2:1;
                SourceComptimeTupleFrame *grown=source_alloc(ctx,next,sizeof(*grown));
                if (!grown || !source_copy_bytes(ctx,node,grown,stack,(size_t)depth*sizeof(*grown))) { ok=false;break; }
                /* Frame bytes transfer their existing owning references; the
                 * obsolete raw arena block has no independent destructor. */
                stack=grown;capacity=next;
            }
            uint32_t count=nullable?1:tuple->parameter_count;
            XrXirCallableParameter *fields=source_alloc(ctx,count,sizeof(*fields));
            SourceValue *payloads=source_alloc(ctx,count,sizeof(*payloads));
            if (!fields || !payloads) { ok=false;break; }
            if (nullable) fields[0]=(XrXirCallableParameter){tuple->element,0};
            else if (!source_copy_bytes(ctx,node,fields,tuple->parameters,(size_t)count*sizeof(*fields))) { ok=false;break; }
            stack[depth++]=(SourceComptimeTupleFrame){current,target,fields,payloads,count,0,0,current_owned,nullable};
            current=(XrXirValue){0};current_owned=false;
        } else {
            if (!source_comptime_constant_leaf(ctx,node,&current,target,&result)) { ok=false;break; }
            if (current_owned) xr_xir_value_drop(&current);current=(XrXirValue){0};current_owned=false;
        }
receive_parent:
        for (;;) {
            if (!depth) { *output=result;goto done; }
            SourceComptimeTupleFrame *frame=&stack[depth-1];
            if (frame->next) {
                XrXirType previous=frame->fields[frame->next-1].type;
                if (previous!=XR_XIR_UNIT) frame->payloads[frame->payload++]=result;
            }
            if (frame->next<frame->count) {
                const XrXirTypeNode *child=xr_xir_tuple_signature(&ctx->types,frame->fields[frame->next].type);
                uint64_t headers=(uint64_t)frame->count+(child?child->parameter_count:0);
                if (!source_work_units(ctx,node,headers*16)) { ok=false;goto done; }
                XrXirValueStatus status;
                if (frame->nullable) {
                    bool some=false;const XrXirValue *payload=NULL;
                    if (!xr_xir_nullable_view(&frame->value,&some,&payload) || !some || !payload) { ok=source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime Nullable payload is invalid");goto done; }
                    status=xr_xir_value_copy(payload,&current);
                } else status=xr_xir_tuple_get(&frame->value,frame->next,&current);
                if (status!=XR_XIR_VALUE_OK) { ok=source_fail(ctx,node,
                    status==XR_XIR_VALUE_REFCOUNT_LIMIT?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE,"comptime Tuple field receiving failed");goto done; }
                target=frame->fields[frame->next++].type;current_owned=true;break;
            }
            if (frame->nullable) {
                if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NULLABLE_SOME,frame->target,{result.id},{0},0,{0}},&result)) { ok=false;goto done; }
            } else if (!source_recipe_group(ctx,(XrXirInstruction){XR_XIR_TUPLE_NEW,frame->target,{0},{0},0,{0}},
                frame->payloads,frame->payload,&result)) { ok=false;goto done; }
            if (frame->owned) xr_xir_value_drop(&frame->value);--depth;
        }
    }
done:
    if (current_owned) xr_xir_value_drop(&current);
    while (depth) { if (stack[depth-1].owned) xr_xir_value_drop(&stack[depth-1].value);--depth; }
    return ok;
}
