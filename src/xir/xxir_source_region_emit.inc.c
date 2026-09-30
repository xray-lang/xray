/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_region_emit.inc.c - Private region identities become XIR once
 *
 * KEY CONCEPT:
 *   The source checker owns recipes and patches only region identities. This
 *   consumer maps their value, control and cleanup references without rechecking AST.
 */
#include "xxir_operand_roles.h"
typedef struct SourceRecipeRole { uint8_t values, edges; } SourceRecipeRole;
static const SourceRecipeRole source_recipe_roles[XR_XIR_OP_COUNT]={
    {0,0},
#define XR_XIR_OP(name,stages,rule,values,edges,terminal) {values,edges},
#include "xxir_ops.def"
#undef XR_XIR_OP
};
static bool source_region_id(const uint32_t *map,uint32_t count,uint32_t id,uint32_t *out) {
    if (id>=count || map[id]==UINT32_MAX) return false;
    *out=map[id]; return true;
}
/* Dead catch bindings have no runtime predecessor. Their sentinel is retained
 * only in the private initialization snapshot; executable sealing never accepts
 * it. Control edges and recipe definitions always require real identities. */
static bool source_region_value(const uint32_t *map,uint32_t count,uint32_t id,uint32_t *out,bool seal) {
    if (!seal && id==UINT32_MAX) { *out=id; return true; }
    return source_region_id(map,count,id,out);
}
/* Checked arena layout; empty views consume neither padding nor storage. */
static bool source_region_span(size_t *bytes,uint32_t count,size_t element,
    size_t alignment,size_t *offset) {
    size_t padding=count ? (alignment-*bytes%alignment)%alignment : 0;
    if (*bytes>SIZE_MAX-padding) return false;
    *offset=*bytes+padding;
    if (count>(SIZE_MAX-*offset)/element) return false;
    *bytes=*offset+(size_t)count*element;
    return true;
}
_Static_assert(_Alignof(SourceMemory)>=_Alignof(XrXirInstruction) &&
    _Alignof(SourceMemory)>=_Alignof(XrXirBlock) &&
    _Alignof(SourceMemory)>=_Alignof(uint32_t) &&
    sizeof(SourceMemory)%_Alignof(XrXirInstruction)==0,
    "source arena must align instruction storage");
typedef struct SourceRegionOutput {
    XrXirInstruction *instructions;
    XrXirBlock *blocks;
    uint32_t *operands;
} SourceRegionOutput;
static bool source_region_output_allocate(SourceContext *ctx,
    const SourceFunction *body,SourceRegionOutput *output) {
    /* All published arrays share one arena owner and retain distinct views. */
    size_t output_bytes=0,instruction_offset=0,block_offset=0,operand_offset=0;
    if (!source_region_span(&output_bytes,body->count,sizeof(XrXirInstruction),
            _Alignof(XrXirInstruction),&instruction_offset) ||
        !source_region_span(&output_bytes,body->block_count,sizeof(XrXirBlock),
            _Alignof(XrXirBlock),&block_offset) ||
        !source_region_span(&output_bytes,body->operand_count,sizeof(uint32_t),
            _Alignof(uint32_t),&operand_offset)) {
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"source region output size exhausted");
    }
    unsigned char *storage=output_bytes ? source_alloc(ctx,1,output_bytes) : NULL;
    if (output_bytes && !storage) return false;
    output->instructions=body->count ? (XrXirInstruction *)(storage+instruction_offset) : NULL;
    output->blocks=body->block_count ? (XrXirBlock *)(storage+block_offset) : NULL;
    output->operands=body->operand_count ? (uint32_t *)(storage+operand_offset) : NULL;
    return true;
}
static bool source_region_emit(SourceContext *ctx,XrXirFunction *output,bool seal) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    uint32_t parameters=ctx->functions[ctx->function].parameter_count;
    if (body->region_sealed || body->count>UINT32_MAX-parameters)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source region is not open");
    if (body->expression_count>ctx->budget.work)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"expression region validation exhausted");
    ctx->budget.work-=body->expression_count;
    uint32_t expressions=0;
    for (SourceExpressionStorage *storage=body->expressions;storage;storage=storage->next) {
        if (!storage->count || storage->count>32 || storage->count>body->expression_count-expressions)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"expression region storage is invalid");
        for (uint32_t e=0;e<storage->count;++e) {
            SourceExpressionPlan *plan=&storage->expressions[e];
            if (plan->owner!=ctx->function || (seal && plan->state!=SOURCE_TERM_GROUND && plan->state!=SOURCE_TERM_DISCARDED) ||
                plan->identity>=body->expression_count || plan->entry.owner!=ctx->function ||
                (plan->state==SOURCE_TERM_GROUND && (plan->exit.owner!=ctx->function ||
                 plan->entry.identity>=body->block_count || plan->exit.identity>=body->block_count)) ||
                (plan->parent && plan->parent->owner!=ctx->function))
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"expression region is unresolved or has a foreign owner");
        }
        expressions+=storage->count;
    }
    if (expressions!=body->expression_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"expression region count is invalid");
    uint32_t value_count=parameters+body->count;
    uint64_t cells=(uint64_t)value_count+(uint64_t)body->block_count*2;
    uint64_t bytes=cells*sizeof(uint32_t);
    uint64_t work=1+(uint64_t)value_count+3*(uint64_t)body->count+4*(uint64_t)body->block_count+body->operand_count;
    if (bytes>SIZE_MAX || bytes>ctx->budget.scratch_bytes || work>ctx->budget.work)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"source region identity map exhausted");
    ctx->budget.work-=work;ctx->budget.scratch_bytes-=bytes;
    uint32_t *map=cells ? xr_calloc((size_t)cells,sizeof(*map)) : NULL;
    bool ok=false;
    if (cells && !map) {source_fail(ctx,NULL,XR_XIR_OUT_OF_MEMORY,"source region identity allocation failed");goto done;}
    uint32_t *blocks=map ? map+value_count : NULL;
    uint32_t *layout=blocks ? blocks+body->block_count : NULL;
    for (uint32_t i=0;i<value_count;++i) map[i]=i<parameters ? i : UINT32_MAX;
    for (uint32_t b=0;b<body->block_count;++b) blocks[b]=layout[b]=UINT32_MAX;
    for (uint32_t b=0;b<body->block_count;++b) {
        SourceBlockRecipe *recipe=&body->blocks[b];
        if (recipe->owner!=ctx->function || recipe->identity>=body->block_count ||
            recipe->runtime>=body->block_count || blocks[recipe->identity]!=UINT32_MAX || layout[recipe->runtime]!=UINT32_MAX) goto invalid;
        blocks[recipe->identity]=recipe->runtime;layout[recipe->runtime]=b;
    }
    uint32_t original_end=0;
    for (uint32_t i=0;i<body->count;++i) {
        const XrXirInstruction *op=&body->recipes[i].instruction;
        if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) goto invalid;
        if (xr_xir_op_uses_operand_table(op->op)) {
            if (op->args[1]>body->operand_count-original_end ||
                op->args[0]!=(op->args[1] ? original_end : 0)) goto invalid;
            original_end+=op->args[1];
        }
    }
    if (original_end!=body->operand_count) goto invalid;
    uint32_t emitted=0;
    for (uint32_t b=0;b<body->block_count;++b) {
        XrXirBlock block=body->blocks[layout[b]].block;
        if (block.first>body->count || block.count>body->count-block.first || block.count>body->count-emitted) goto invalid;
        for (uint32_t a=0;a<block.count;++a) {
            SourceInstructionRecipe *recipe=&body->recipes[block.first+a];
            if (recipe->owner!=ctx->function || recipe->value<parameters || recipe->value>=value_count || map[recipe->value]!=UINT32_MAX) goto invalid;
            map[recipe->value]=parameters+emitted++;
        }
    }
    if (emitted!=body->count) goto invalid;
    SourceRegionOutput views={0};
    if (!source_region_output_allocate(ctx,body,&views)) goto done;
    XrXirInstruction *instructions=views.instructions;
    XrXirBlock *emitted_blocks=views.blocks;
    uint32_t *operands=views.operands;
    if (body->operand_count) memcpy(operands,body->operands,body->operand_count*sizeof(*operands));
    emitted=0;
    for (uint32_t b=0;b<body->block_count;++b) {
        XrXirBlock block=body->blocks[layout[b]].block;
        if (block.first>body->count || block.count>body->count-block.first) goto invalid;
        if (block.panic && !source_region_id(blocks,body->block_count,block.panic,&block.panic)) goto invalid;
        if (block.frontier) {
            uint32_t value;
            if (block.frontier-1>=body->count || !source_region_id(map,value_count,body->recipes[block.frontier-1].value,&value)) goto invalid;
            block.frontier=value-parameters+1;
        }
        block.first=emitted;emitted+=block.count;emitted_blocks[b]=block;
    }
    uint32_t operand_end=0;emitted=0;
    for (uint32_t b=0;b<body->block_count;++b) {
      XrXirBlock block=body->blocks[layout[b]].block;
      for (uint32_t i=block.first;i<block.first+block.count;++i) {
        XrXirInstruction op=body->recipes[i].instruction;
        if (op.op<=XR_XIR_INVALID || op.op>=XR_XIR_OP_COUNT) goto invalid;
        SourceRecipeRole role=source_recipe_roles[op.op];
        if (xr_xir_op_uses_operand_table(op.op)) {
            if (op.args[0]>body->operand_count || op.args[1]>body->operand_count-op.args[0] ||
                op.args[1]>body->operand_count-operand_end) goto invalid;
            uint32_t original=op.args[0];op.args[0]=op.args[1] ? operand_end : 0;
            for (uint32_t a=0;a<op.args[1];++a) {
                uint32_t id=body->operands[original+a];
                bool block_id=op.op==XR_XIR_PHI && !(a%2);
                if (block_id ? !source_region_id(blocks,body->block_count,id,&operands[operand_end+a]) :
                    !source_region_value(map,value_count,id,&operands[operand_end+a],seal)) goto invalid;
            }
            operand_end+=op.args[1];
        } else {
            uint32_t count=op.op==XR_XIR_RETURN ? output->result!=XR_XIR_UNIT : role.values;
            for (uint32_t a=0;a<count;++a)
                if (!source_region_value(map,value_count,op.args[a],&op.args[a],seal)) goto invalid;
        }
        for (uint32_t e=0;e<role.edges;++e)
            if (!source_region_id(blocks,body->block_count,op.targets[e],&op.targets[e])) goto invalid;
        if (op.op==XR_XIR_INVOKE_RESULT || op.op==XR_XIR_INVOKE_ERROR ||
            ((op.op==XR_XIR_CLEANUP_LEAVE || op.op==XR_XIR_CLEANUP_ERROR) && op.immediate)) {
            bool frontier=op.op==XR_XIR_CLEANUP_LEAVE || op.op==XR_XIR_CLEANUP_ERROR;
            uint32_t value;
            if (op.immediate<0 || (uint64_t)op.immediate>UINT32_MAX) goto invalid;
            uint32_t index=(uint32_t)op.immediate-(frontier ? 1u : 0u);
            if (index>=body->count || !source_region_id(map,value_count,body->recipes[index].value,&value)) goto invalid;
            op.immediate=value-parameters+(frontier ? 1u : 0u);
        }
        instructions[emitted++]=op;
      }
    }
    if (operand_end!=body->operand_count) goto invalid;
    output->instructions=instructions;output->instruction_count=body->count;
    output->blocks=emitted_blocks;output->block_count=body->block_count;
    output->operands=operands;output->operand_count=body->operand_count;
    if (seal) {
        body->region_sealed=true;
        while (body->recipe_storage) {
            SourceRecipeStorage *storage=body->recipe_storage;body->recipe_storage=storage->next;
            source_release_private(storage);
        }
        body->expressions=NULL;body->expression_count=0;
        body->recipes=NULL;body->blocks=NULL;body->capacity=0;body->block_capacity=0;
    }
    ok=true;goto done;
invalid:
    source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source region identity is invalid");
done:
    xr_free(map);ctx->budget.scratch_bytes+=bytes;return ok;
}
