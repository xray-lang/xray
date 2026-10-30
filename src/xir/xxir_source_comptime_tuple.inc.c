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
#include "xxir_array.h"
#include "xxir_type_arena.h"
#include "xxir_enum.h"
#include "xxir_compile_domain.h"
static bool source_comptime_result_type(SourceContext *ctx,AstNode *node,XrXirType type) {
    if (ctx->generics[ctx->function].parameter_count && xr_xir_type_span(&ctx->types,type))
        return true;
    XrXirCtfeStatus status=xr_xir_compile_ctfe_type_admit(&ctx->compile,&ctx->types,type);
    if (status==XR_XIR_CTFE_OK) return true;
    if (status==XR_XIR_CTFE_COMPLEXITY_LIMIT || status==XR_XIR_CTFE_OUT_OF_MEMORY ||
        status==XR_XIR_CTFE_MEMORY_LIMIT || status==XR_XIR_CTFE_OVERFLOW ||
        status==XR_XIR_CTFE_CANCELLED || status==XR_XIR_CTFE_DEADLINE || status==XR_XIR_CTFE_CLOCK_FAILURE)
        return source_comptime_failure(ctx,node,status);
    return false;
}
static bool source_comptime_result_value(SourceContext *ctx,AstNode *node,
    const XrXirValue *value,uint32_t arity) {
    if (!source_request_poll(ctx,node) || !value) return false;
    XrXirType type=(XrXirType)value->type;
    if (type==XR_XIR_UNIT) return !value->reserved && !value->payload && !arity;
    if (!arity && type!=XR_XIR_STRING)
        return (type==XR_XIR_BOOL || type==XR_XIR_RUNE || xr_xir_type_is_number(type)) &&
            xr_xir_value_argument(value,NULL,type);
    const XrXirTypeArena *arena=xr_xir_value_arena(value);
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(types,type);
    bool shape=type==XR_XIR_STRING?!arity:tuple?tuple->parameter_count==arity:
        arity==1 && (xr_xir_type_is_nullable(types,type) || xr_xir_type_is_array(types,type) ||
            xr_xir_type_is_struct(types,type) || xr_xir_type_is_enum(types,type));
    if (!shape || !xr_xir_value_argument(value,arena,type)) return false;
    /* Empty enum carriers are owned by their immutable arena and have no
     * Domain or child graph. The real canonical enum borrow validates them. */
    if (xr_xir_type_is_enum(types,type)) {
        XrXirEnumBorrow borrowed={0};
        if (xr_xir_enum_borrow(value,&borrowed)!=XR_XIR_VALUE_OK) return false;
        if (!borrowed.field_count) return true;
    }
    XrXirDomain *domain=NULL;
    XrXirValueStatus status=xr_xir_compile_value_domain_borrow(ctx->compile.resources,value,arena,&domain);
    if (status!=XR_XIR_VALUE_OK)
        return source_fail(ctx,node,source_comptime_value_status(status),"comptime result has no authentic compiler owner");
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.scratch_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES};
    status=xr_xir_value_admit(value,type,&admission);
    return status==XR_XIR_VALUE_OK || source_fail(ctx,node,source_comptime_value_status(status),
        "comptime result graph admission failed");
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
    bool nullable,array;
    int64_t capacity;
    SourceValue store,place;
    bool nominal,enumeration;
    uint32_t variant;
} SourceComptimeTupleFrame;
#include "xxir_source_comptime_array.inc.c"
#include "xxir_source_comptime_nominal.inc.c"
#include "xxir_source_materialization.inc.c"
static bool source_comptime_constant_leaf(SourceContext *ctx,AstNode *node,
    const XrXirValue *input,XrXirType target,SourceValue *output) {
    if (target==XR_XIR_UNIT) { *output=(SourceValue){.id=0,.type=XR_XIR_UNIT,.place={0}};return !input->type && !input->reserved && !input->payload; }
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
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) { ok=false;break; }
        const XrXirTypeNode *tuple=xr_xir_type_node(&ctx->types,target);
        bool nullable=tuple && tuple->kind==XR_XIR_TYPE_NULLABLE;
        bool array=tuple && tuple->kind==XR_XIR_TYPE_ARRAY;
        bool nominal=tuple && tuple->kind==XR_XIR_TYPE_NOMINAL;
        bool enumeration=nominal && xr_xir_type_is_enum(&ctx->types,target);
        bool materialized=false;
        if (nominal && !source_materialization_receive(ctx,node,&current,target,&result,&materialized)) { ok=false;break; }
        if (materialized) {
            if (current_owned) xr_xir_value_drop(&current);current=(XrXirValue){0};current_owned=false;
            goto receive_parent;
        }
        if (nominal) tuple=xr_xir_type_node(&ctx->types,target);
        if (nullable) {
            bool some=false;const XrXirValue *payload=NULL;
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) { ok=false;break; }
            if (!xr_xir_nullable_view(&current,&some,&payload)) { ok=source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime Nullable is invalid");break; }
            if (!some) {
                if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NULLABLE_NONE,target,{0},{0},0,{0}},&result)) { ok=false;break; }
                if (current_owned) xr_xir_value_drop(&current);current=(XrXirValue){0};current_owned=false;
                goto receive_parent;
            }
        }
        if (array || nullable || nominal || (tuple && tuple->kind==XR_XIR_TYPE_TUPLE)) {
            if (depth==capacity) {
                if (capacity>UINT32_MAX/2) { ok=source_fail(ctx,node,XR_XIR_BUDGET,"comptime recipe stack exhausted");break; }
                uint32_t next=capacity?capacity*2:1;
                SourceComptimeTupleFrame *grown=source_alloc(ctx,next,sizeof(*grown));
                if (!grown || !source_copy_bytes(ctx,node,grown,stack,(size_t)depth*sizeof(*grown))) { ok=false;break; }
                /* Frame bytes transfer their existing owning references; the
                 * obsolete raw arena block has no independent destructor. */
                stack=grown;capacity=next;
            }
            uint32_t count=nullable?1:tuple->parameter_count,variant=0,begin=0;int64_t length=0,reserved=0;XrXirType element=tuple->element;
            if (nominal) {
                SourceComptimeNominalShape shape={0};if (!source_comptime_nominal_shape(ctx,node,&current,target,&shape)) { ok=false;break; }
                tuple=xr_xir_type_node(&ctx->types,target);
                count=shape.count;begin=shape.begin;variant=shape.variant;
            }
            if (array && !source_comptime_array_measure(ctx,node,&current,target,&length,&reserved)) { ok=false;break; }
            if (array) count=(uint32_t)length;
            XrXirCallableParameter *fields=source_alloc(ctx,count,sizeof(*fields));
            SourceValue *payloads=source_alloc(ctx,count,sizeof(*payloads));
            if (count && (!fields || !payloads)) { ok=false;break; }
            if (array) {
                for (uint32_t i=0;i<count;++i) fields[i]=(XrXirCallableParameter){element,0};
            } else if (nominal) {
                for (uint32_t i=0;i<count;++i) { if (!((ctx)->diagnostic.status == XR_XIR_OK)) { ok=false;goto done; }fields[i]=(XrXirCallableParameter){tuple->nominal.fields[begin+i],0}; }
            } else if (nullable) fields[0]=(XrXirCallableParameter){tuple->element,0};
            else if (!source_copy_bytes(ctx,node,fields,tuple->parameters,(size_t)count*sizeof(*fields))) { ok=false;break; }
            stack[depth++]=(SourceComptimeTupleFrame){current,target,fields,payloads,count,0,0,current_owned,nullable,array,reserved,{0},{0},nominal,enumeration,variant};
            current=(XrXirValue){0};current_owned=false;
            if (array && !source_comptime_array_begin(ctx,&stack[depth-1])) { ok=false;break; }
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
                if (frame->array) {
                    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_PUSH,XR_XIR_UNIT,{frame->place.id,result.id},{0},0,{0}},NULL)) { ok=false;goto done; }
                } else if (previous!=XR_XIR_UNIT || frame->nominal) frame->payloads[frame->payload++]=result;
            }
            if (frame->next<frame->count) {
                if (!((ctx)->diagnostic.status == XR_XIR_OK)) { ok=false;goto done; }
                XrXirValueStatus status;
                if (frame->array) {
                    if (!source_comptime_array_get(ctx,node,frame,frame->next,&current)) { ok=false;goto done; }
                    status=XR_XIR_VALUE_OK;
                } else if (frame->nominal) {
                    if (!source_comptime_nominal_get(ctx,node,frame,&current)) { ok=false;goto done; }
                    status=XR_XIR_VALUE_OK;
                } else if (frame->nullable) {
                    bool some=false;const XrXirValue *payload=NULL;
                    if (!xr_xir_nullable_view(&frame->value,&some,&payload) || !some || !payload) { ok=source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime Nullable payload is invalid");goto done; }
                    status=xr_xir_value_copy(payload,&current);
                } else status=xr_xir_tuple_get(&frame->value,frame->next,&current);
                if (status!=XR_XIR_VALUE_OK) { ok=source_fail(ctx,node,
                    status==XR_XIR_VALUE_REFCOUNT_LIMIT?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE,"comptime Tuple field receiving failed");goto done; }
                target=frame->fields[frame->next++].type;current_owned=true;break;
            }
            if (frame->array) {
                if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,frame->target,{frame->store.id},{0},0,{0}},&result)) { ok=false;goto done; }
            } else if (frame->nominal) {
                XrXirOp op=frame->enumeration?XR_XIR_ENUM_NEW:XR_XIR_STRUCT_NEW;
                if (!source_recipe_group(ctx,(XrXirInstruction){op,frame->target,{0},{0},frame->variant,{0}},frame->payloads,frame->payload,&result)) { ok=false;goto done; }
            } else if (frame->nullable) {
                if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NULLABLE_SOME,frame->target,{result.type==XR_XIR_UNIT?0:result.id},{0},0,{0}},&result)) { ok=false;goto done; }
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
