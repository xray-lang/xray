/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_comptime_array.inc.c - Independent capacity-preserving Array recipes
 *
 * KEY CONCEPT:
 *   Ordinary private Cell/push recipes preserve actual observable capacity.
 *   Element observation and receiving use the original compiler ledger.
 */
#include "xxir_compile_domain.h"
static bool source_comptime_array_measure(SourceContext *ctx,AstNode *node,const XrXirValue *value,
    XrXirType target,int64_t *length,int64_t *capacity) {
    if (!source_work_units(ctx,node,32)) return false;
    const XrXirTypeArena *arena=xr_xir_value_arena(value);XrXirValueAdmission admission={.arena=arena,.work=UINT64_MAX};
    /* Metadata observation requires the authentic Domain. The value remains
     * borrowed and this receiving Domain owns only admission scratch. */
    XrXirDomain *domain=NULL;XrXirValueStatus status=xr_xir_compile_domain_new(ctx->compile.resources,UINT64_MAX,&domain);
    if (status!=XR_XIR_VALUE_OK) return source_fail(ctx,node,status==XR_XIR_VALUE_OOM?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET,"comptime Array observation failed");
    admission.domain=domain;status=xr_xir_array_len(value,&admission,length);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_array_capacity(value,&admission,capacity);
    xr_xir_domain_drop(domain);
    if (status!=XR_XIR_VALUE_OK) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"comptime Array is invalid");
    if ((uint64_t)*length>UINT32_MAX) return source_fail(ctx,node,XR_XIR_BUDGET,"comptime Array recipe is too large");
    XrXirCtfeTypeShape shape={0};XrXirCtfeStatus checked=xr_xir_compile_ctfe_type_shape(&ctx->compile,&ctx->types,
        xr_xir_array_element(&ctx->types,target),&shape);
    if (checked!=XR_XIR_CTFE_OK || shape.dynamic || shape.work>UINT64_MAX/32 || (uint64_t)*length>UINT64_MAX/(shape.work*32))
        return source_fail(ctx,node,checked==XR_XIR_CTFE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET,"comptime Array element receiving is unbounded");
    return source_work_units(ctx,node,(uint64_t)*length*shape.work*32);
}
static bool source_comptime_array_begin(SourceContext *ctx,AstNode *node,SourceComptimeTupleFrame *frame) {
    XrXirType cell=XR_XIR_UNIT;SourceValue capacity={0},empty={0};
    if (!source_work_units(ctx,node,64) || !source_cell_type(ctx,frame->target,&cell) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},frame->capacity,{0}},&capacity) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_WITH_CAPACITY,frame->target,{capacity.id},{0},0,{0}},&empty) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell,{empty.id},{0},0,{0}},&frame->store) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_PLACE,frame->target,{frame->store.id},{0},0,{0}},&frame->place)) return false;
    return true;
}
static bool source_comptime_array_get(SourceContext *ctx,AstNode *node,SourceComptimeTupleFrame *frame,
    uint32_t index,XrXirValue *output) {
    int64_t length=0,capacity=0;
    if (!source_comptime_array_measure(ctx,node,&frame->value,frame->target,&length,&capacity)) return false;
    const XrXirTypeArena *arena=xr_xir_value_arena(&frame->value);XrXirDomain *domain=NULL;
    XrXirValueStatus status=xr_xir_compile_domain_new(ctx->compile.resources,UINT64_MAX,&domain);
    if (status!=XR_XIR_VALUE_OK) return source_fail(ctx,node,status==XR_XIR_VALUE_OOM?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET,"comptime Array receiving allocation failed");
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=UINT64_MAX,.scratch_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES};XrXirFaultDetail fault={0};
    status=xr_xir_array_get(&frame->value,index,&admission,output,&fault);xr_xir_domain_drop(domain);
    return status==XR_XIR_VALUE_OK || source_fail(ctx,node,status==XR_XIR_VALUE_OOM?XR_XIR_OUT_OF_MEMORY:
        status==XR_XIR_VALUE_LIMIT?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE,"comptime Array element receiving failed");
}
