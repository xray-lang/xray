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
    uint64_t *incident_offsets;
    uint64_t *change_epochs;
    uint64_t epoch;
    uint32_t *incident_edges;
    unsigned char *pending;
};
static void effect_capture_schedule_next_job(EffectCaptureSchedule *schedule) {
    if(schedule->epoch==UINT64_MAX) {
        memset(schedule->change_epochs,0,(size_t)schedule->nodes*sizeof(*schedule->change_epochs));
        schedule->epoch=0;
    }
    ++schedule->epoch;
}
static XrXirStatus effect_capture_schedule_push(EffectCaptureRead *read,uint32_t job) {
    EffectCaptureSchedule *schedule=read->schedule;
    if(!schedule || job>=schedule->capacity || schedule->count>schedule->capacity) return XR_XIR_BAD_STRUCTURE;

    if(schedule->pending[job]) return XR_XIR_OK;
    if(schedule->count==schedule->capacity || schedule->back>=schedule->capacity) return XR_XIR_BAD_STRUCTURE;

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
    /* No job is consumed while its clause runs. One notification per changed
     * node covers all facts emitted by that clause, including rank decreases. */
    if(schedule->change_epochs[node]==schedule->epoch) return XR_XIR_OK;
    schedule->change_epochs[node]=schedule->epoch;
    XrXirStatus status=effect_capture_schedule_push(read,node);if(status!=XR_XIR_OK) return status;
    uint64_t first=schedule->incident_offsets[node],end=schedule->incident_offsets[node+1];
    if(first>end || end>(uint64_t)schedule->edges*2) return XR_XIR_BAD_STRUCTURE;
    for(uint64_t at=first;at<end;++at) {
        uint32_t e=schedule->incident_edges[at];
        if(e>=schedule->edges) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge *edge=&read->final->edges[e];
        if(edge->caller>=schedule->nodes || edge->target>=schedule->nodes ||
            (edge->caller!=node && edge->target!=node)) return XR_XIR_BAD_STRUCTURE;
        status=effect_capture_schedule_push(read,schedule->nodes+e);if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_schedule_create(EffectCaptureRead *read,EffectCaptureSchedule **output) {
    if(!read || !output || *output || read->schedule || !read->final->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t nodes=read->final->count,edges=read->final->edge_count;
    uint64_t capacity=(uint64_t)nodes+edges;
    uint64_t bytes=sizeof(EffectCaptureSchedule)+capacity*(sizeof(uint32_t)+sizeof(unsigned char))+
        ((uint64_t)nodes*2+1)*sizeof(uint64_t)+(uint64_t)edges*2*sizeof(uint32_t);
    if(capacity>UINT32_MAX || bytes>SIZE_MAX) return XR_XIR_BUDGET;
    if(edges && !read->final->edges) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    EffectCaptureSchedule *schedule=xir_compile_calloc(read->work,1,(size_t)bytes,&status);
    if(!schedule) return status;
    if(read->final->count!=nodes || read->final->edge_count!=edges) {
        xr_compile_resources_free(schedule);return XR_XIR_BAD_STRUCTURE;
    }
    schedule->nodes=read->final->count;schedule->edges=read->final->edge_count;schedule->capacity=(uint32_t)capacity;
    schedule->incident_offsets=(uint64_t *)(schedule+1);
    schedule->change_epochs=schedule->incident_offsets+(uint64_t)nodes+1;
    schedule->queue=(uint32_t *)(schedule->change_epochs+nodes);
    schedule->incident_edges=schedule->queue+schedule->capacity;
    schedule->pending=(unsigned char *)(schedule->incident_edges+(uint64_t)edges*2);
    /* Only fact rows grow during clause closure. The fixed edge set owns an
     * ordinal index so fact reallocation never leaves borrowed edge pointers. */
    for(uint32_t e=0;e<edges;++e) {
        const EffectInvocationEdge *edge=&read->final->edges[e];
        if(edge->caller>=nodes || edge->target>=nodes) {
            xr_compile_resources_free(schedule);return XR_XIR_BAD_STRUCTURE;
        }
        ++schedule->incident_offsets[edge->caller+1];
        ++schedule->incident_offsets[edge->target+1];
    }
    for(uint32_t n=0;n<nodes;++n) schedule->incident_offsets[n+1]+=schedule->incident_offsets[n];
    for(uint32_t e=0;e<edges;++e) {
        const EffectInvocationEdge *edge=&read->final->edges[e];
        schedule->incident_edges[schedule->incident_offsets[edge->caller]++]=e;
        schedule->incident_edges[schedule->incident_offsets[edge->target]++]=e;
    }
    for(uint32_t n=nodes;n>0;--n) schedule->incident_offsets[n]=schedule->incident_offsets[n-1];
    schedule->incident_offsets[0]=0;
    *output=schedule;return XR_XIR_OK;
}
static XrXirStatus effect_capture_schedule_run(EffectCaptureRead *read,EffectCaptureSchedule *schedule) {
    if(!read || !schedule || !read->builder || read->selected || read->fixed_only || read->schedule ||
        read->final->count!=schedule->nodes || read->final->edge_count!=schedule->edges) return XR_XIR_BAD_STRUCTURE;

    memset(schedule->pending,0,schedule->capacity*sizeof(unsigned char));schedule->front=schedule->back=schedule->count=0;
    read->schedule=schedule;XrXirStatus status=XR_XIR_OK;
    for(uint32_t job=0;job<schedule->capacity && status==XR_XIR_OK;++job) status=effect_capture_schedule_push(read,job);
    while(status==XR_XIR_OK && schedule->count) {
        if(schedule->front>=schedule->capacity || read->final->count!=schedule->nodes || read->final->edge_count!=schedule->edges) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }

        uint32_t job=schedule->queue[schedule->front];
        if(job>=schedule->capacity || !schedule->pending[job]) { status=XR_XIR_BAD_STRUCTURE;break; }
        schedule->pending[job]=0;schedule->front=schedule->front+1==schedule->capacity?0:schedule->front+1;--schedule->count;
        effect_capture_schedule_next_job(schedule);
        if(job<schedule->nodes) status=effect_capture_node_clauses(read,job,false);
        else status=effect_capture_edge(read,job-schedule->nodes);
    }
    read->schedule=NULL;return status;
}
/* Exact-row Class gates can change as facts grow. Preserve each full sweep's
 * ordinal order; only a previously successful clause with unchanged local
 * dependencies may be omitted. A job dirtied during or after its visit stays
 * pending for the next sweep, including the job that emitted the change. */
static XrXirStatus effect_capture_schedule_ordered(EffectCaptureRead *read,EffectCaptureSchedule *schedule) {
    if(!read || !read->final || !schedule || read->builder!=&read->final->capture ||
        read->selected || read->fixed_only || read->schedule || read->complete_scope || read->body.current ||
        !effect_body_request_owned_matches(read->work,&read->body,read->final->material) ||
        !read->fallback || !read->class_results || !schedule->nodes || !schedule->queue || !schedule->pending ||
        (uint64_t)schedule->nodes+schedule->edges!=schedule->capacity ||
        read->final->count!=schedule->nodes || read->final->edge_count!=schedule->edges)
        return XR_XIR_BAD_STRUCTURE;
    memset(schedule->pending,0,schedule->capacity*sizeof(unsigned char));schedule->front=schedule->back=schedule->count=0;
    read->schedule=schedule;XrXirStatus status=XR_XIR_OK;
    for(uint32_t job=0;job<schedule->capacity && status==XR_XIR_OK;++job) status=effect_capture_schedule_push(read,job);
    while(status==XR_XIR_OK && schedule->count) {
        read->changed=false;
        for(uint32_t job=0;job<schedule->capacity && status==XR_XIR_OK;++job) {
            if(read->final->count!=schedule->nodes || read->final->edge_count!=schedule->edges ||
                (uint64_t)schedule->nodes+schedule->edges!=schedule->capacity ||
                schedule->count>schedule->capacity || schedule->back>=schedule->capacity) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            if(!schedule->pending[job]) continue;
            if(!schedule->count) { status=XR_XIR_BAD_STRUCTURE;break; }
            schedule->pending[job]=0;--schedule->count;
            effect_capture_schedule_next_job(schedule);
            if(job<schedule->nodes) status=effect_capture_node_clauses(read,job,false);
            else status=effect_capture_edge(read,job-schedule->nodes);
        }
    }
    /* Push still bounds the existing ring writes, but this walk consumes only
     * pending bits. No queue payload or borrowed semantic view survives a job. */
    read->schedule=NULL;if(status==XR_XIR_OK) read->changed=false;
    return status;
}
