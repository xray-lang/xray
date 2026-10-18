/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_schedule.inc.c - Constructor clause scheduling
 *
 * KEY CONCEPT:
 *   Pending ordinals select repeated construction work, never permission facts.
 *   The positive domain starts with every node and edge. Changes revisit the local
 *   node and each incident edge; the independent receiver reads no queue.
 */
struct EffectCaptureSchedule {
    uint32_t nodes,edges,capacity,front,back,count;
    uint32_t *queue;
    unsigned char *pending;
};
static XrXirStatus effect_capture_schedule_push(EffectCaptureRead *read,uint32_t job) {
    EffectCaptureSchedule *schedule=read->schedule;
    if(!schedule || job>=schedule->capacity || schedule->count>schedule->capacity) return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
    if(schedule->pending[job]) return XR_XIR_OK;
    if(schedule->count==schedule->capacity || schedule->back>=schedule->capacity) return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(read->work,4*sizeof(uint32_t)+sizeof(unsigned char))) return XR_XIR_BUDGET;
    schedule->queue[schedule->back]=job;schedule->pending[job]=1;
    schedule->back=schedule->back+1==schedule->capacity?0:schedule->back+1;
    ++schedule->count;return XR_XIR_OK;
}
/* Local value, reference and Cell clauses read only their node's facts,
 * including its independent initial rows. Cross-node reads belong to edges
 * at their current caller and target; every affected clause gets a new job. */
static XrXirStatus effect_capture_schedule_changed(EffectCaptureRead *read,uint32_t node) {
    EffectCaptureSchedule *schedule=read->schedule;if(!schedule) return XR_XIR_OK;
    if(node>=schedule->nodes || read->final->count!=schedule->nodes || read->final->edge_count!=schedule->edges)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_capture_schedule_push(read,node);if(status!=XR_XIR_OK) return status;
    for(uint32_t e=0;e<schedule->edges;++e) {
        if(!xir_compile_work(read->work,5)) return XR_XIR_BUDGET;
        const EffectInvocationEdge *edge=&read->final->edges[e];
        if(edge->caller>=schedule->nodes || edge->target>=schedule->nodes) return XR_XIR_BAD_STRUCTURE;
        if(edge->caller!=node && edge->target!=node) continue;
        status=effect_capture_schedule_push(read,schedule->nodes+e);if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_schedule_create(EffectCaptureRead *read,EffectCaptureSchedule **output) {
    if(!read || !output || *output || read->schedule || !read->final->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t nodes=read->final->count,edges=read->final->edge_count;
    uint64_t capacity=(uint64_t)nodes+edges;
    uint64_t bytes=sizeof(EffectCaptureSchedule)+capacity*(sizeof(uint32_t)+sizeof(unsigned char));
    if(capacity>UINT32_MAX || bytes>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    EffectCaptureSchedule *schedule=xir_compile_calloc(read->work,1,(size_t)bytes,&status);
    if(!schedule) return status;
    if(read->final->count!=nodes || read->final->edge_count!=edges) {
        xr_compile_resources_free(schedule);return XR_XIR_BAD_STRUCTURE;
    }
    schedule->nodes=read->final->count;schedule->edges=read->final->edge_count;schedule->capacity=(uint32_t)capacity;
    schedule->queue=(uint32_t *)(schedule+1);schedule->pending=(unsigned char *)(schedule->queue+schedule->capacity);
    *output=schedule;return XR_XIR_OK;
}
static XrXirStatus effect_capture_schedule_run(EffectCaptureRead *read,EffectCaptureSchedule *schedule) {
    if(!read || !schedule || !read->builder || read->selected || read->fixed_only || read->schedule ||
        read->final->count!=schedule->nodes || read->final->edge_count!=schedule->edges) return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(read->work,(uint64_t)schedule->capacity*sizeof(unsigned char)+3*sizeof(uint32_t))) return XR_XIR_BUDGET;
    memset(schedule->pending,0,schedule->capacity*sizeof(unsigned char));schedule->front=schedule->back=schedule->count=0;
    read->schedule=schedule;XrXirStatus status=XR_XIR_OK;
    for(uint32_t job=0;job<schedule->capacity && status==XR_XIR_OK;++job) status=effect_capture_schedule_push(read,job);
    while(status==XR_XIR_OK && schedule->count) {
        if(schedule->front>=schedule->capacity || read->final->count!=schedule->nodes || read->final->edge_count!=schedule->edges) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        if(!xir_compile_work(read->work,4*sizeof(uint32_t)+sizeof(unsigned char))) { status=XR_XIR_BUDGET;break; }
        uint32_t job=schedule->queue[schedule->front];
        if(job>=schedule->capacity || !schedule->pending[job]) { status=XR_XIR_BAD_STRUCTURE;break; }
        schedule->pending[job]=0;schedule->front=schedule->front+1==schedule->capacity?0:schedule->front+1;--schedule->count;
        if(job<schedule->nodes) status=effect_capture_node_clauses(read,job,false);
        else status=effect_capture_edge(read,job-schedule->nodes);
    }
    read->schedule=NULL;return status;
}
