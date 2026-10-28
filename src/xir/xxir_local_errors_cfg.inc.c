/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_local_errors_cfg.inc.c - Function-local monotone error value propagation
 */
static XrXirStatus local_error_enqueue(LocalErrorCfg *cfg,uint32_t block) {
    if (block>=cfg->head.block_count) return XR_XIR_BAD_STRUCTURE;
    if (cfg->queued[block]) return XR_XIR_OK;
    if (cfg->pending>=cfg->head.block_count) return XR_XIR_BAD_STRUCTURE;
    cfg->queue[cfg->back]=block;cfg->back=cfg->back+1==cfg->head.block_count?0:cfg->back+1;
    cfg->queued[block]=1;++cfg->pending;return XR_XIR_OK;
}
static XrXirStatus local_error_edge(LocalErrorCfg *cfg,uint32_t to,const XrXirInstruction *branch,bool yes) {
    if (to>=cfg->head.block_count) return XR_XIR_BAD_STRUCTURE;
    if (cfg->stride) memcpy(cfg->edge,cfg->row,cfg->stride*sizeof(uint64_t));
    if (branch && !local_error_filter(cfg,branch,yes)) return XR_XIR_OK;
    const XrXirBlock *block=&cfg->head.blocks[to];
    if (cfg->head.blocks[cfg->block].panic==to && cfg->head.blocks[cfg->block].frontier!=block->frontier)
        local_error_cells(cfg,cfg->edge,UINT32_MAX,NULL);
    if (cfg->stride) memcpy(cfg->snapshot,cfg->edge,cfg->stride*sizeof(uint64_t));
    for (uint32_t i=block->first;i<block->first+block->count;++i) {
        const XrXirInstruction *phi=&cfg->operations[i];if (phi->op!=XR_XIR_PHI) break;
        uint64_t *out=local_error_value(cfg,cfg->edge,cfg->head.parameter_count+i);if (!out) continue;
        for (uint32_t p=0;p<phi->args[1];p+=2) {
            if (cfg->head.operands[phi->args[0]+p]!=cfg->block) continue;
            uint32_t value=cfg->head.operands[phi->args[0]+p+1];
            memcpy(out,local_error_read(cfg,cfg->snapshot,value),(size_t)cfg->words*sizeof(*out));
        }
    }
    bool changed=cfg->stride?local_error_join(cfg->states+(size_t)to*cfg->stride,cfg->edge,cfg->stride):false;
    if (!cfg->reachable[to]) { changed=true;cfg->reachable[to]=1; }
    return changed?local_error_enqueue(cfg,to):XR_XIR_OK;
}
static XrXirStatus local_error_block(LocalErrorCfg *cfg,uint32_t b) {
    cfg->block=b;const XrXirBlock *block=&cfg->head.blocks[b];
    if (!block->count || block->first>=cfg->head.instruction_count ||
        block->count>cfg->head.instruction_count-block->first) return XR_XIR_BAD_STRUCTURE;
    if (cfg->stride) memcpy(cfg->row,cfg->states+(size_t)b*cfg->stride,cfg->stride*sizeof(uint64_t));
    for (uint32_t i=block->first;i<block->first+block->count;++i) {
        if (block->panic) {
            XrXirStatus status=local_error_edge(cfg,block->panic,NULL,true);if (status!=XR_XIR_OK) return status;
        }
        XrXirStatus status=local_error_instruction(cfg,i);if (status!=XR_XIR_OK) return status;
    }
    uint32_t last=block->first+block->count-1;const XrXirInstruction *end=&cfg->operations[last];
    if (end->op==XR_XIR_JUMP || end->op==XR_XIR_CLEANUP_REGISTER || end->op==XR_XIR_CLEANUP_LEAVE ||
        end->op==XR_XIR_CLEANUP_ERROR || end->op==XR_XIR_ITERATOR_CLOSE)
        return local_error_edge(cfg,end->targets[0],NULL,true);
    if (end->op==XR_XIR_BRANCH) {
        XrXirStatus status=local_error_edge(cfg,end->targets[0],end,true);
        return status==XR_XIR_OK?local_error_edge(cfg,end->targets[1],end,false):status;
    }
    if (end->op==XR_XIR_INVOKE || end->op==XR_XIR_INVOKE_DEFAULT || end->op==XR_XIR_INVOKE_INDIRECT ||
        end->op==XR_XIR_TASK_AWAIT || end->op==XR_XIR_ITERATOR_HAS_NEXT ||
        end->op==XR_XIR_ITERATOR_NEXT || end->op==XR_XIR_ITERATOR_NTH) {
        XrXirStatus status=local_error_edge(cfg,end->targets[0],NULL,true);
        return status==XR_XIR_OK && cfg->calls[last]!=XR_XIR_NEVER?
            local_error_edge(cfg,end->targets[1],NULL,true):status;
    }
    return XR_XIR_OK;
}
static XrXirStatus local_error_solve(LocalErrorCfg *cfg) {
    for (uint32_t p=0;p<cfg->head.parameter_count;++p) {
        uint64_t *set=local_error_value(cfg,cfg->states,p);
        if (set) local_error_type(cfg,cfg->types[p],set);
    }
    cfg->reachable[0]=1;XrXirStatus status=local_error_enqueue(cfg,0);
    while (status==XR_XIR_OK && cfg->pending) {
        uint32_t block=cfg->queue[cfg->front];cfg->front=cfg->front+1==cfg->head.block_count?0:cfg->front+1;
        --cfg->pending;cfg->queued[block]=0;status=local_error_block(cfg,block);
    }
    return status;
}
