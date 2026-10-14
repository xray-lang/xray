/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_cfg.inc.c - Bounded dirty-block worklist for error propagation
 *
 * KEY CONCEPT:
 *   Each pending block has one queue entry. Clearing pending before transfer
 *   permits self edges and late contributions to schedule the block again.
 */
static XrXirStatus error_block(ErrorFlow *flow,uint32_t b,const uint8_t *zero_copy);
static XrXirStatus error_cfg_enqueue(ErrorFlow *flow,uint32_t block) {
    if(!flow || !flow->function || block>=flow->function->block_count ||
        !flow->queue || !flow->reachable) return XR_XIR_BAD_STRUCTURE;
    if(flow->reachable[block]&ERROR_BLOCK_PENDING)return XR_XIR_OK;
    uint32_t count=flow->function->block_count;
    if(flow->pending>=count || flow->back>=count)return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(flow->remaining,3+sizeof(*flow->queue)))return XR_XIR_BUDGET;
    flow->queue[flow->back]=block;
    flow->back=flow->back+1==count?0:flow->back+1;
    ++flow->pending;flow->reachable[block]|=ERROR_BLOCK_PENDING;return XR_XIR_OK;
}
static XrXirStatus error_cfg_solve(ErrorFlow *flow,const uint8_t *zero_copy) {
    flow->front=flow->back=flow->pending=0;flow->scheduling=true;
    XrXirStatus status=error_cfg_enqueue(flow,0);
    while(status==XR_XIR_OK && flow->pending && !flow->restart) {
        if(!xir_compile_work(flow->remaining,3)){status=XR_XIR_BUDGET;break;}
        uint32_t count=flow->function->block_count;
        if(flow->front>=count){status=XR_XIR_BAD_STRUCTURE;break;}
        uint32_t block=flow->queue[flow->front];
        if(block>=count || !(flow->reachable[block]&ERROR_BLOCK_PENDING)) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        flow->front=flow->front+1==count?0:flow->front+1;--flow->pending;
        flow->reachable[block]&=(uint8_t)~ERROR_BLOCK_PENDING;
        status=error_block(flow,block,zero_copy);
    }
    /* Failed or resized generations cannot leave an active scheduler behind. */
    flow->scheduling=false;flow->pending=0;flow->changed=false;return status;
}
