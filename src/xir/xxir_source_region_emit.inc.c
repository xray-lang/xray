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
static bool source_hir_default_projection(SourceContext *,SourceValue,XrHirBodyProjection *);
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
#include "xxir_source_throw_map.inc.c"
#include "xxir_source_handler_map.inc.c"
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
    _Alignof(SourceMemory)>=_Alignof(XrXirSourceDebugPoint) &&
    sizeof(SourceMemory)%_Alignof(XrXirInstruction)==0,
    "source arena must align instruction storage");
typedef struct SourceRegionOutput {
    XrXirInstruction *instructions;
    XrXirBlock *blocks;
    uint32_t *operands;
    XrXirSourceDebugPoint *points;
} SourceRegionOutput;
static bool source_region_output_allocate(SourceContext *ctx,
    const SourceFunction *body,SourceRegionOutput *output) {
    /* All published arrays share one arena owner and retain distinct views. */
    size_t output_bytes=0,instruction_offset=0,block_offset=0,operand_offset=0,point_offset=0;
    if (!source_region_span(&output_bytes,body->count,sizeof(XrXirInstruction),
            _Alignof(XrXirInstruction),&instruction_offset) ||
        !source_region_span(&output_bytes,body->block_count,sizeof(XrXirBlock),
            _Alignof(XrXirBlock),&block_offset) ||
        !source_region_span(&output_bytes,body->operand_count,sizeof(uint32_t),
            _Alignof(uint32_t),&operand_offset) ||
        !source_region_span(&output_bytes,body->count,sizeof(XrXirSourceDebugPoint),
            _Alignof(XrXirSourceDebugPoint),&point_offset)) {
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"source region output size exhausted");
    }
    unsigned char *storage=output_bytes ? source_alloc(ctx,1,output_bytes) : NULL;
    if (output_bytes && !storage) return false;
    output->instructions=body->count ? (XrXirInstruction *)(storage+instruction_offset) : NULL;
    output->blocks=body->block_count ? (XrXirBlock *)(storage+block_offset) : NULL;
    output->operands=body->operand_count ? (uint32_t *)(storage+operand_offset) : NULL;
    output->points=body->count ? (XrXirSourceDebugPoint *)(storage+point_offset) : NULL;
    return true;
}
#include "xxir_source_region_seal.inc.c"
static bool source_region_emit(SourceContext *ctx,XrXirFunction *output,bool seal) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    uint32_t parameters=ctx->functions[ctx->function].parameter_count;
    if (body->region_sealed || body->count>UINT32_MAX-parameters)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source region is not open");
    if (!source_region_expressions(ctx,seal)) return false;
    uint32_t value_count=parameters+body->count;
    uint64_t cells=(uint64_t)value_count+(uint64_t)body->block_count*2;
    if (cells>SIZE_MAX/sizeof(uint32_t))
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"source region identity map exhausted");
    uint32_t *map=cells ? source_scratch(ctx,(size_t)cells,sizeof(*map),true) : NULL;
    bool ok=false;
    if (cells && !map) goto done;
    uint32_t *blocks=map ? map+value_count : NULL,*layout=blocks ? blocks+body->block_count : NULL;
    for (uint32_t i=0;i<value_count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
        map[i]=i<parameters ? i : UINT32_MAX;
    }
    for (uint32_t b=0;b<body->block_count;++b) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
        blocks[b]=layout[b]=UINT32_MAX;
    }
    for (uint32_t b=0;b<body->block_count;++b) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
        SourceBlockRecipe *recipe=&body->blocks[b];
        if (recipe->owner!=ctx->function || recipe->identity>=body->block_count ||
            recipe->runtime>=body->block_count || blocks[recipe->identity]!=UINT32_MAX || layout[recipe->runtime]!=UINT32_MAX) goto invalid;
        blocks[recipe->identity]=recipe->runtime;layout[recipe->runtime]=b;
    }
    uint32_t original_end=0;
    for (uint32_t i=0;i<body->count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
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
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
        XrXirBlock block=body->blocks[layout[b]].block;
        if (block.first>body->count || block.count>body->count-block.first || block.count>body->count-emitted) goto invalid;
        for (uint32_t a=0;a<block.count;++a) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
            SourceInstructionRecipe *recipe=&body->recipes[block.first+a];
            if (recipe->owner!=ctx->function || recipe->value<parameters || recipe->value>=value_count || map[recipe->value]!=UINT32_MAX) goto invalid;
            map[recipe->value]=parameters+emitted++;
        }
    }
    if (emitted!=body->count) goto invalid;
    SourceRegionOutput views={0};
    if (!source_region_output_allocate(ctx,body,&views)) goto done;
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
    XrXirInstruction *instructions=views.instructions;
    XrXirBlock *emitted_blocks=views.blocks;
    uint32_t *operands=views.operands;
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
    if (body->operand_count) memcpy(operands,body->operands,body->operand_count*sizeof(*operands));
    emitted=0;
    for (uint32_t b=0;b<body->block_count;++b) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
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
    SourceRootValue *root_values = seal && body->count ? source_alloc(ctx,body->count,sizeof(*root_values)) : NULL;
    if (seal && body->count && !root_values) goto done;
    uint32_t operand_end=0;emitted=0;
    for (uint32_t b=0;b<body->block_count;++b) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
      XrXirBlock block=body->blocks[layout[b]].block;
      for (uint32_t i=block.first;i<block.first+block.count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
        XrXirInstruction op=body->recipes[i].instruction;
        if (op.op<=XR_XIR_INVALID || op.op>=XR_XIR_OP_COUNT) goto invalid;
        SourceRecipeRole role=source_recipe_roles[op.op];
        if (xr_xir_op_uses_operand_table(op.op)) {
            if (op.args[0]>body->operand_count || op.args[1]>body->operand_count-op.args[0] ||
                op.args[1]>body->operand_count-operand_end) goto invalid;
            uint32_t original=op.args[0];op.args[0]=op.args[1] ? operand_end : 0;
            for (uint32_t a=0;a<op.args[1];++a) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
                uint32_t id=body->operands[original+a];
                bool block_id=op.op==XR_XIR_PHI && !(a%2);
                if (block_id ? !source_region_id(blocks,body->block_count,id,&operands[operand_end+a]) :
                    !source_region_value(map,value_count,id,&operands[operand_end+a],seal)) goto invalid;
            }
            operand_end+=op.args[1];
        } else {
            uint32_t count=op.op==XR_XIR_RETURN ? output->result!=XR_XIR_UNIT : role.values;
            if (op.op==XR_XIR_NULLABLE_SOME && xr_xir_nullable_is_unit(&ctx->types,op.type)) count=0;
            if (op.op==XR_XIR_SLOT_INIT || op.op==XR_XIR_SLOT_STORE)
                count=xr_xir_slot_payload_operands(ctx->slots,ctx->slot_count,&op);
            if (op.op==XR_XIR_CELL_NEW || op.op==XR_XIR_CELL_WRITE || op.op==XR_XIR_CELL_LOCAL_WRITE) {
                XrXirType cell=op.type;
                if (op.op!=XR_XIR_CELL_NEW) {
                    uint32_t id=op.args[0];
                    if (id>=value_count) goto invalid;
                    cell=id<parameters ? ctx->functions[ctx->function].parameters[id] :
                        body->recipes[id-parameters].instruction.type;
                }
                count=xr_xir_cell_payload_operands(&ctx->types,cell,op.op);
            }
            for (uint32_t a=0;a<count;++a) {
                if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
                if (!source_region_value(map,value_count,op.args[a],&op.args[a],seal)) goto invalid;
            }
        }
        for (uint32_t e=0;e<role.edges;++e) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
            if (!source_region_id(blocks,body->block_count,op.targets[e],&op.targets[e])) goto invalid;
        }
        if (op.op==XR_XIR_INVOKE_RESULT || op.op==XR_XIR_INVOKE_ERROR || op.op==XR_XIR_INVOKE_DISCARD ||
            ((op.op==XR_XIR_CLEANUP_LEAVE || op.op==XR_XIR_CLEANUP_ERROR) && op.immediate)) {
            bool frontier=op.op==XR_XIR_CLEANUP_LEAVE || op.op==XR_XIR_CLEANUP_ERROR;
            uint32_t value;
            if (op.immediate<0 || (uint64_t)op.immediate>UINT32_MAX) goto invalid;
            uint32_t index=(uint32_t)op.immediate-(frontier ? 1u : 0u);
            if (index>=body->count || !source_region_id(map,value_count,body->recipes[index].value,&value)) goto invalid;
            op.immediate=value-parameters+(frontier ? 1u : 0u);
        }
        if (root_values) {
            root_values[emitted] = body->recipes[i].root;
            if (root_values[emitted].peer != UINT32_MAX &&
                !source_region_id(map,value_count,root_values[emitted].peer,&root_values[emitted].peer)) goto invalid;
        }
        views.points[emitted]=body->recipes[i].debug;
        instructions[emitted++]=op;
      }
    }
    if (operand_end!=body->operand_count) goto invalid;
    output->instructions=instructions;output->instruction_count=body->count;
    output->blocks=emitted_blocks;output->block_count=body->block_count;
    output->operands=operands;output->operand_count=body->operand_count;
    if(seal)for(SourceHirProjection *use=body->typed_projections;use;use=use->next) {
        uint32_t value=UINT32_MAX,block=UINT32_MAX,recipe=UINT32_MAX;
        if(use->has_value&&!source_region_id(map,value_count,use->value,&value))goto invalid;
        if(use->instruction&&!source_region_id(map,value_count,use->recipe,&recipe))goto invalid;
        if(use->block!=UINT32_MAX&&!source_region_id(blocks,body->block_count,use->block,&block))goto invalid;
        if(use->instruction&&(recipe==UINT32_MAX||recipe<parameters))goto invalid;
        XrHirBodyProjection projection={.function=ctx->function,
            .instruction=use->instruction?recipe-parameters:UINT32_MAX,.block=block,
            .value=value,.source_value=use->value,.formal=use->formal,.argument=use->argument,
            .source_value_map=map,.source_value_count=value_count,
            .type=use->type,.has_instruction=use->instruction,.has_value=use->has_value};
        if(use->instruction) {
            if(use->recipe<parameters||use->recipe-parameters>=body->count)goto invalid;
            XrXirInstruction raw_recipe=body->recipes[use->recipe-parameters].instruction;
            projection.operation=raw_recipe.op;projection.source_operands[0]=raw_recipe.args[0];
            projection.source_operands[1]=raw_recipe.args[1];projection.immediate=raw_recipe.immediate;
            projection.type=raw_recipe.type;
            if(raw_recipe.args[0]>=parameters&&raw_recipe.args[0]-parameters<body->count) {
                XrXirInstruction producer=body->recipes[raw_recipe.args[0]-parameters].instruction;
                projection.operand_operation=producer.op;projection.operand_immediate=producer.immediate;projection.operand_type=producer.type;
            }
            XrXirInstruction call=raw_recipe;
            if(call.op==XR_XIR_INVOKE_RESULT&&call.immediate>=0&&(uint64_t)call.immediate<body->count)
                call=body->recipes[(uint32_t)call.immediate].instruction;
            projection.call=call;projection.has_call=call.op==XR_XIR_CALL||call.op==XR_XIR_INVOKE||
                call.op==XR_XIR_CALL_INDIRECT||call.op==XR_XIR_INVOKE_INDIRECT||call.op==XR_XIR_CALL_REQUIREMENT||
                call.op==XR_XIR_GO||call.op==XR_XIR_GENERATOR_NEW;
            if(xr_xir_op_uses_operand_table(raw_recipe.op)) {
                if(raw_recipe.args[0]>body->operand_count||raw_recipe.args[1]>body->operand_count-raw_recipe.args[0])goto invalid;
                projection.source_arguments=raw_recipe.args[1]?body->operands+raw_recipe.args[0]:NULL;projection.source_argument_count=raw_recipe.args[1];
            }
        }
        if(use->defaulted&&!source_hir_default_projection(ctx,(SourceValue){.id=use->value,.type=use->type},&projection))goto done;
        const char *reason=NULL;
        XrXirStatus status=xr_hir_body_project(ctx->typed_body,use->use,&projection,ctx->request,&reason);
        if(status!=XR_XIR_OK) {
            source_fail(ctx,NULL,status,reason?reason:"typed projection has no current owner");goto done;
        }
    }
    if (seal && !source_region_seal(ctx,map,value_count,root_values,views.points)) goto done;
    ok=true;goto done;
invalid:
    source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source region identity is invalid");
done:
    xr_compile_resources_free(map);return ok;
}
