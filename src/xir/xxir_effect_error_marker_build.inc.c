/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_marker_build.inc.c - Construction of owned type derivations
 */
typedef struct ErrorMarkerFrame { uint32_t node,last; } ErrorMarkerFrame;
typedef struct ErrorMarkerAppend { ErrorMarkerGoal goal;uint32_t parent,ordinal; } ErrorMarkerAppend;
static void error_markers_free(EffectErrorMarkers *markers) {
    if(!markers) return;
    xr_compile_resources_free(markers->nodes);xr_compile_resources_free(markers->roots);xr_compile_resources_free(markers);
}
static XrXirStatus error_marker_reserve_nodes(const XrXirCompileContext *work,EffectErrorMarkers *markers) {
    if(markers->count<markers->capacity) return XR_XIR_OK;
    if(markers->capacity>UINT32_MAX/2) return XR_XIR_BUDGET;
    uint32_t capacity=markers->capacity?markers->capacity*2:16;
    if((uint64_t)capacity>SIZE_MAX/sizeof(*markers->nodes)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;ErrorMarkerNode *nodes=xir_compile_alloc(work,(size_t)capacity*sizeof(*nodes),&status);
    if(!nodes) return status;
    if(!xir_compile_work(work,(uint64_t)markers->count*sizeof(*nodes))) { xr_compile_resources_free(nodes);return XR_XIR_BUDGET; }
    if(markers->count) memcpy(nodes,markers->nodes,(size_t)markers->count*sizeof(*nodes));
    xr_compile_resources_free(markers->nodes);markers->nodes=nodes;markers->capacity=capacity;return XR_XIR_OK;
}
static XrXirStatus error_marker_reserve_roots(const XrXirCompileContext *work,EffectErrorMarkers *markers) {
    if(markers->root_count<markers->root_capacity) return XR_XIR_OK;
    if(markers->root_capacity>UINT32_MAX/2) return XR_XIR_BUDGET;
    uint32_t capacity=markers->root_capacity?markers->root_capacity*2:8;
    if((uint64_t)capacity>SIZE_MAX/sizeof(*markers->roots)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;ErrorMarkerRoot *roots=xir_compile_alloc(work,(size_t)capacity*sizeof(*roots),&status);
    if(!roots) return status;
    if(!xir_compile_work(work,(uint64_t)markers->root_count*sizeof(*roots))) { xr_compile_resources_free(roots);return XR_XIR_BUDGET; }
    if(markers->root_count) memcpy(roots,markers->roots,(size_t)markers->root_count*sizeof(*roots));
    xr_compile_resources_free(markers->roots);markers->roots=roots;markers->root_capacity=capacity;return XR_XIR_OK;
}
static XrXirStatus error_marker_append(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    EffectErrorMarkers *markers,const ErrorMarkerAppend *request,uint32_t *output) {
    XrXirStatus status=error_marker_reserve_nodes(work,markers);if(status!=XR_XIR_OK) return status;
    uint32_t index=markers->count,reference=UINT32_MAX;
    for(uint32_t i=0;i<index;++i) {
        if(!xir_compile_work(work,sizeof(request->goal))) return XR_XIR_BUDGET;
        if(error_marker_goal_equal(&request->goal,&markers->nodes[i].goal)) { reference=i;break; }
    }
    ErrorMarkerNode node={.goal=request->goal,.parent=request->parent,.ordinal=request->ordinal,.reference=reference};
    if(reference!=UINT32_MAX) {
        const ErrorMarkerNode *other=&markers->nodes[reference];
        node.end=index+1;node.value=other->end?other->value:(request->goal.kind==ERROR_MARKER_PAIR?1u:0u);
    } else {
        ErrorMarkerRule rule;status=error_marker_rule(work,proof,&request->goal,&rule);if(status!=XR_XIR_OK) return status;
        if(rule.terminal) { node.end=index+1;node.value=rule.value; }
    }
    if(!xir_compile_work(work,sizeof(node)+sizeof(*output))) return XR_XIR_BUDGET;
    markers->nodes[index]=node;++markers->count;*output=index;return XR_XIR_OK;
}
static XrXirStatus error_marker_push(const XrXirCompileContext *work,ErrorMarkerFrame **frames,
    uint32_t *capacity,uint32_t depth,uint32_t node) {
    if(depth>=*capacity) {
        if(*capacity>UINT32_MAX/2) return XR_XIR_BUDGET;
        uint32_t count=*capacity?*capacity*2:16;
        if((uint64_t)count>SIZE_MAX/sizeof(**frames)) return XR_XIR_BUDGET;
        XrXirStatus status=XR_XIR_OK;ErrorMarkerFrame *next=xir_compile_alloc(work,(size_t)count*sizeof(*next),&status);
        if(!next) return status;
        if(!xir_compile_work(work,(uint64_t)depth*sizeof(*next))) { xr_compile_resources_free(next);return XR_XIR_BUDGET; }
        if(depth) memcpy(next,*frames,(size_t)depth*sizeof(*next));
        xr_compile_resources_free(*frames);*frames=next;*capacity=count;
    }
    if(!xir_compile_work(work,sizeof(**frames))) return XR_XIR_BUDGET;
    (*frames)[depth]=(ErrorMarkerFrame){node,UINT32_MAX};return XR_XIR_OK;
}
static XrXirStatus error_marker_tree(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    EffectErrorMarkers *markers,uint32_t root) {
    ErrorMarkerFrame *frames=NULL;uint32_t depth=0,capacity=0;
    XrXirStatus status=XR_XIR_OK;
    if(!markers->nodes[root].end) { status=error_marker_push(work,&frames,&capacity,depth,root);if(status==XR_XIR_OK) ++depth; }
    while(status==XR_XIR_OK && depth) {
        ErrorMarkerFrame *frame=&frames[depth-1];uint32_t index=frame->node;
        if(!xir_compile_work(work,6)) { status=XR_XIR_BUDGET;break; }
        if(frame->last!=UINT32_MAX) {
            const ErrorMarkerNode *child=&markers->nodes[frame->last];
            if(!child->end) { status=XR_XIR_BAD_STRUCTURE;break; }
            if(!child->value) {
                if(markers->nodes[index].goal.kind==ERROR_MARKER_PAIR) { status=XR_XIR_BAD_STRUCTURE;break; }
                markers->nodes[index].end=markers->count;markers->nodes[index].value=0;--depth;continue;
            }
            frame->last=UINT32_MAX;
        }
        ErrorMarkerGoal parent=markers->nodes[index].goal;ErrorMarkerRule rule;
        status=error_marker_rule(work,proof,&parent,&rule);if(status!=XR_XIR_OK) break;
        uint32_t ordinal=markers->nodes[index].children;
        if(ordinal==rule.children) {
            markers->nodes[index].end=markers->count;markers->nodes[index].value=1;--depth;continue;
        }
        ErrorMarkerAppend request={.parent=index,.ordinal=ordinal};uint32_t child=UINT32_MAX;
        status=error_marker_child(work,proof,&parent,ordinal,&request.goal);if(status!=XR_XIR_OK) break;
        ++markers->nodes[index].children;
        status=error_marker_append(work,proof,markers,&request,&child);if(status!=XR_XIR_OK) break;
        frames[depth-1].last=child;
        if(!markers->nodes[child].end) {
            status=error_marker_push(work,&frames,&capacity,depth,child);if(status==XR_XIR_OK) ++depth;
        }
    }
    xr_compile_resources_free(frames);return status;
}
static XrXirStatus error_marker_build(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    EffectErrorMarkers **output) {
    if(!output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;EffectErrorMarkers *markers=xir_compile_calloc(work,1,sizeof(*markers),&status);
    if(!markers) return status;
    markers->resources=work->resources;EffectErrorFiniteRead read={proof,work,UINT32_MAX,NULL};
    for(uint32_t f=0;f<proof->contexts && status==XR_XIR_OK;++f) {
        uint64_t word=0;status=error_finite_word(&read,proof->rows[f].end-(uint64_t)proof->words*64,64,&word);
        if(status!=XR_XIR_OK) break;
        if(word&3) continue;
        for(uint32_t a=0;a<proof->atom_count && status==XR_XIR_OK;++a) {
            status=error_finite_word(&read,proof->rows[f].end-(uint64_t)proof->words*64+(uint64_t)((a+2)/64)*64,64,&word);
            if(status!=XR_XIR_OK) break;
            if(!(word&(UINT64_C(1)<<((a+2)%64)))) continue;
            status=error_marker_reserve_roots(work,markers);if(status!=XR_XIR_OK) break;
            ErrorMarkerAppend request={.goal=error_marker_type_goal(f,ERROR_MARKER_ERROR,proof->atoms[a].type),
                .parent=UINT32_MAX,.ordinal=markers->root_count};uint32_t first=UINT32_MAX;
            status=error_marker_append(work,proof,markers,&request,&first);if(status!=XR_XIR_OK) break;
            if(!xir_compile_work(work,sizeof(ErrorMarkerRoot))) { status=XR_XIR_BUDGET;break; }
            markers->roots[markers->root_count++]=(ErrorMarkerRoot){f,a,first};
            status=error_marker_tree(work,proof,markers,first);if(status!=XR_XIR_OK) break;
            if(!markers->nodes[first].value) break;
        }
    }
    if(status!=XR_XIR_OK) { error_markers_free(markers);return status; }
    *output=markers;return XR_XIR_OK;
}
