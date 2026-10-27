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
static bool source_comptime_observation(SourceContext *ctx,AstNode *node,const XrXirValue *value,
    XrXirValueAdmission *output) {
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    const XrXirTypeArena *arena=xr_xir_value_arena(value);XrXirDomain *domain=NULL;
    XrXirValueStatus status=xr_xir_compile_value_domain_borrow(ctx->compile.resources,value,arena,&domain);
    if (status!=XR_XIR_VALUE_OK) return source_fail(ctx,node,source_comptime_value_status(status),"comptime observation has no authentic compiler owner");
    *output=(XrXirValueAdmission){.arena=arena,.domain=domain,.scratch_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES};
    return true;
}
static bool source_comptime_array_measure(SourceContext *ctx,AstNode *node,const XrXirValue *value,
    XrXirType target,int64_t *length,int64_t *capacity) {
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    XrXirValueAdmission admission={0};if (!source_comptime_observation(ctx,node,value,&admission)) return false;
    XrXirValueStatus status=xr_xir_array_len(value,&admission,length);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_array_capacity(value,&admission,capacity);
    if (status!=XR_XIR_VALUE_OK) return source_fail(ctx,node,source_comptime_value_status(status),"comptime Array is invalid");
    if ((uint64_t)*length>UINT32_MAX) return source_fail(ctx,node,XR_XIR_BUDGET,"comptime Array recipe is too large");
    XrXirCtfeStatus checked=xr_xir_compile_ctfe_type_admit(&ctx->compile,&ctx->types,
        xr_xir_array_element(&ctx->types,target));
    if (checked!=XR_XIR_CTFE_OK) return source_comptime_failure(ctx,node,checked);
    /* Observe only the selected inline snapshot; receiving validates each child. */
    return ((ctx)->diagnostic.status == XR_XIR_OK);
}
static bool source_comptime_array_begin(SourceContext *ctx,SourceComptimeTupleFrame *frame) {
    XrXirType cell=XR_XIR_UNIT;SourceValue capacity={0},empty={0};
    if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_cell_type(ctx,frame->target,&cell) ||
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
    XrXirValueAdmission admission={0};if (!source_comptime_observation(ctx,node,&frame->value,&admission)) return false;
    XrXirFaultDetail fault={0};XrXirValueStatus status=xr_xir_array_observe(&frame->value,index,&admission,output,&fault);
    return status==XR_XIR_VALUE_OK || source_fail(ctx,node,source_comptime_value_status(status),"comptime Array element receiving failed");
}
