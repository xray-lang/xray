/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_match_alternatives.inc.c - Ordered alternatives and owned joins
 *
 * KEY CONCEPT:
 *   Successful alternatives join before the single arm guard and body.
 */
static bool source_match_alternatives(SourceContext *ctx, SourceMatchArm *arm,
    SourceValue receiver, bool test, SourceMatchFailure **failures) {
    if (arm->count==1) return source_match_payload(ctx,arm->patterns,receiver,test,failures,0);
    SourceName *saved=ctx->locals;
    SourceName **branches=source_alloc(ctx,arm->count,sizeof(*branches));
    uint32_t *ends=source_alloc(ctx,arm->count,sizeof(*ends)), *jumps=source_alloc(ctx,arm->count,sizeof(*jumps));
    SourceValue *inputs=source_alloc(ctx,(size_t)arm->count*2,sizeof(*inputs));
    if (!branches || !ends || !jumps || !inputs) return false;
    SourceFunction *body=&ctx->bodies[ctx->function]; uint32_t used=0;
    for (uint32_t i=0;i<arm->count;++i) {
        if (!source_work(ctx,arm->patterns[i].node)) return false;
        ctx->locals=saved; *failures=NULL;
        if (!source_match_payload(ctx,&arm->patterns[i],receiver,test || i+1<arm->count,failures,0)) return false;
        if (!i && !*failures) return true;
        branches[i]=ctx->locals; ends[i]=body->current_block.identity; jumps[i]=body->count; used=i+1;
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0, {0}},NULL)) return false;
        if (!*failures || i+1==arm->count) break;
        uint32_t next=body->block_count;
        for (SourceMatchFailure *f=*failures;f;f=f->next) body->recipes[f->instruction].instruction.targets[f->target]=next;
        if (!begin_block(ctx)) return false;
    }
    uint32_t join=body->block_count;
    if (!begin_block(ctx)) return false;
    for (uint32_t i=0;i<used;++i) body->recipes[jumps[i]].instruction.targets[0]=join;
    ctx->locals=saved;
    for (SourceName *symbol=arm->bindings;symbol;symbol=symbol->next) {
        for (uint32_t i=0;i<used;++i) {
            SourceName *bound=branches[i];
            for (;bound!=saved;bound=bound->next) {
                if (!source_work(ctx,symbol->node)) return false;
                if (source_text_same(ctx, NULL, bound->name, symbol->name)) break;
            }
            if (bound==saved || bound->type!=symbol->type)
                return source_fail(ctx,symbol->node,XR_XIR_BAD_TYPE,"alternative binding join is inconsistent");
            inputs[i*2]=(SourceValue){ends[i],XR_XIR_UNIT}; inputs[i*2+1]=(SourceValue){bound->index,bound->type};
        }
        SourceValue value={0,XR_XIR_UNIT};
        if (symbol->type!=XR_XIR_UNIT && !source_recipe_group(ctx,(XrXirInstruction){XR_XIR_PHI,symbol->type,{0},{0},0, {0}},inputs,used*2,&value)) return false;
        if (!source_match_bind(ctx,symbol,value)) return false;
    }
    return true;
}
