/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_role_proof.inc.c - Positive local role closure and independent reads
 *
 * Construction owns reverse indices and a queue. Verification reads actual
 * Input edges and Final claims without allocating or releasing any scratch.
 */
static void effect_role_proof_clear(EffectRoleProof *proof) {
    xr_compile_resources_free(proof->rows);xr_compile_resources_free(proof->facts);
    *proof=(EffectRoleProof){0};
}
static XrXirStatus effect_role_value_count(EffectRoleCheck *check,uint32_t f,uint32_t *count) {
    const EffectBodyFunction *head=&check->material->functions[f];bool callable=false;
    if (effect_body_variable_roles(check->material)) {
        for (uint32_t p=0;p<head->parameter_count;++p) {
            if (!xir_compile_work(check->work,3)) return XR_XIR_BUDGET;
            callable|=xr_xir_callable_signature(check->material->domain.types,head->parameters[p])!=NULL;
        }
    }
    uint64_t values=callable?(uint64_t)head->parameter_count+head->instruction_count:0;
    if (values>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    *count=(uint32_t)values;return XR_XIR_OK;
}
static XrXirStatus effect_role_proof_geometry(EffectRoleCheck *check) {
    XrXirStatus status=effect_role_frame(check);if (status!=XR_XIR_OK) return status;
    const EffectRoleProof *proof=check->proof;
    status=effect_role_range(check->work,proof,1,sizeof(*proof));if (status!=XR_XIR_OK) return status;
    uint64_t total=0;
    for (uint32_t f=0;f<check->functions;++f) {
        uint32_t count=0;status=effect_role_value_count(check,f,&count);if (status!=XR_XIR_OK) return status;
        total+=count;if (total>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    }
    if (!total) return !proof->resources && !proof->functions && !proof->total &&
        !proof->rows && !proof->facts?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if (proof->resources!=check->work->resources || proof->functions!=check->functions || proof->total!=total)
        return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(check->work,proof->rows,proof->functions,sizeof(*proof->rows));
    if (status==XR_XIR_OK) status=effect_role_range(check->work,proof->facts,proof->total,sizeof(*proof->facts));
    if (status!=XR_XIR_OK) return status;
    uint64_t first=0,formal=0;
    for (uint32_t f=0;f<check->functions;++f) {
        if (!xir_compile_work(check->work,8)) return XR_XIR_BUDGET;
        uint32_t count=0;status=effect_role_value_count(check,f,&count);if (status!=XR_XIR_OK) return status;
        const EffectBodyFunction *head=&check->material->functions[f];const EffectRoleFunction *row=&proof->rows[f];
        if (row->parameters!=head->parameter_count || row->instructions!=head->instruction_count ||
            row->count!=count || row->first!=first || row->formal_first!=formal) return XR_XIR_BAD_STRUCTURE;
        first+=count;formal+=head->parameter_count;
    }
    return first==proof->total && formal==check->total?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
/* Targets occupy the actual eligible SSA inventory. A non-callable callee
 * formal is a direct fixed seed and requires no unused value row. */
static XrXirStatus effect_role_target(EffectRoleCheck *check,uint32_t f,uint32_t target,
    uint32_t action,uint32_t *node,uint32_t *seed) {
    const EffectRoleProof *proof=check->proof;*node=UINT32_MAX;*seed=0;
    if (action==EFFECT_ROLE_FIXED || action==EFFECT_ROLE_CONDITIONAL) {
        *seed=action==EFFECT_ROLE_FIXED?2:1;return XR_XIR_OK;
    }
    if (action==EFFECT_ROLE_ALIAS) {
        if (target>=proof->rows[f].count) return XR_XIR_BAD_STRUCTURE;
        *node=proof->rows[f].first+target;return XR_XIR_OK;
    }
    if (action!=EFFECT_ROLE_CALLEE) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t callee=0;callee<proof->functions;++callee) {
        if (!xir_compile_work(check->work,3)) return XR_XIR_BUDGET;
        const EffectRoleFunction *row=&proof->rows[callee];
        if (target<row->formal_first || target-row->formal_first>=row->parameters) continue;
        if (row->count) *node=row->first+target-row->formal_first;
        else *seed=2;
        return XR_XIR_OK;
    }
    return XR_XIR_BAD_STRUCTURE;
}
typedef struct EffectRoleReverse {
    uint32_t source,next,instruction,ordinal;
} EffectRoleReverse;
typedef struct EffectRoleBuilder {
    EffectRoleCheck check;
    EffectRoleProof *proof;
    uint32_t *heads,*queue;
    uint8_t *pending;
    EffectRoleReverse *edges;
    uint32_t used,capacity;
} EffectRoleBuilder;
static bool effect_role_fact_join(EffectRoleFact *fact,uint32_t level,uint32_t distance,
    uint32_t instruction,uint32_t ordinal) {
    if (level<fact->level || (level==fact->level && fact->distance<=distance)) return false;
    *fact=(EffectRoleFact){level,distance,instruction,ordinal};return true;
}
static XrXirStatus effect_role_build_edge(EffectRoleCheck *check,uint32_t source,
    uint32_t target,uint32_t action) {
    EffectRoleBuilder *builder=check->receiver;EffectRoleProof *proof=builder->proof;
    uint32_t node=UINT32_MAX,seed=0;XrXirStatus status=effect_role_target(check,check->function,target,action,&node,&seed);
    if (status!=XR_XIR_OK) return status;
    uint32_t origin=proof->rows[check->function].first+source;
    if (origin>=proof->total || (node!=UINT32_MAX && node>=proof->total)) return XR_XIR_BAD_STRUCTURE;
    if (seed) {
        if (!xir_compile_work(check->work,sizeof(EffectRoleFact))) return XR_XIR_BUDGET;
        effect_role_fact_join(&proof->facts[origin],seed,1,check->instruction,check->ordinal-1);return XR_XIR_OK;
    }
    if (builder->used==builder->capacity) {
        uint64_t capacity=builder->capacity?(uint64_t)builder->capacity*2:8;
        if (capacity>UINT32_MAX || capacity>SIZE_MAX/sizeof(*builder->edges)) return XR_XIR_BUDGET;
        EffectRoleReverse *fresh=xir_compile_alloc(check->work,(size_t)capacity*sizeof(*fresh),&status);
        if (!fresh) return status;
        status=effect_role_proof_geometry(check);
        if (status!=XR_XIR_OK) { xr_compile_resources_free(fresh);return status; }
        if (!xir_compile_work(check->work,(uint64_t)builder->used*sizeof(*fresh)+3*sizeof(void *))) {
            xr_compile_resources_free(fresh);return XR_XIR_BUDGET;
        }
        if (builder->used) memcpy(fresh,builder->edges,(size_t)builder->used*sizeof(*fresh));
        xr_compile_resources_free(builder->edges);builder->edges=fresh;builder->capacity=(uint32_t)capacity;
        status=effect_role_proof_geometry(check);if (status!=XR_XIR_OK) return status;
    }
    if (!xir_compile_work(check->work,sizeof(EffectRoleReverse)+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
    builder->edges[builder->used]=(EffectRoleReverse){origin,builder->heads[node],check->instruction,check->ordinal-1};
    builder->heads[node]=builder->used++;return XR_XIR_OK;
}
static XrXirStatus effect_role_build_closure(EffectRoleBuilder *builder) {
    EffectRoleProof *proof=builder->proof;const XrXirCompileContext *work=builder->check.work;
    uint32_t front=0,back=0,pending=0;
    for (uint32_t n=0;n<proof->total;++n) {
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if (!proof->facts[n].level) continue;
        builder->queue[back]=n;back=back+1==proof->total?0:back+1;++pending;builder->pending[n]=1;
    }
    while (pending) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        uint32_t target=builder->queue[front];front=front+1==proof->total?0:front+1;--pending;builder->pending[target]=0;
        EffectRoleFact fact=proof->facts[target];
        if (!fact.distance || fact.distance>proof->total) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t e=builder->heads[target];e!=UINT32_MAX;) {
            if (!xir_compile_work(work,6+sizeof(EffectRoleFact))) return XR_XIR_BUDGET;
            if (e>=builder->used) return XR_XIR_BAD_STRUCTURE;
            EffectRoleReverse edge=builder->edges[e];
            if (edge.source>=proof->total || (edge.next!=UINT32_MAX && edge.next>=e)) return XR_XIR_BAD_STRUCTURE;
            if (effect_role_fact_join(&proof->facts[edge.source],fact.level,fact.distance+1,edge.instruction,edge.ordinal) &&
                !builder->pending[edge.source]) {
                if (pending>=proof->total) return XR_XIR_BAD_STRUCTURE;
                builder->queue[back]=edge.source;back=back+1==proof->total?0:back+1;
                ++pending;builder->pending[edge.source]=1;
            }
            e=edge.next;
        }
    }
    return XR_XIR_OK;
}
typedef struct EffectRoleVerify {
    uint32_t wanted;
    bool support,found;
} EffectRoleVerify;
static XrXirStatus effect_role_verify_edge(EffectRoleCheck *check,uint32_t source,
    uint32_t target,uint32_t action) {
    EffectRoleVerify *verify=check->receiver;const EffectRoleProof *proof=check->proof;
    uint32_t origin=proof->rows[check->function].first+source,node=UINT32_MAX,seed=0;
    if (origin>=proof->total) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_target(check,check->function,target,action,&node,&seed);
    if (status!=XR_XIR_OK) return status;
    EffectRoleFact fact=proof->facts[origin],required={0};
    if (seed) required=(EffectRoleFact){.level=seed};
    else {
        if (node>=proof->total) return XR_XIR_BAD_STRUCTURE;
        required=proof->facts[node];
    }
    if (!xir_compile_work(check->work,8)) return XR_XIR_BUDGET;
    if (!verify->support) return fact.level>=required.level?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    verify->found=true;
    return origin==verify->wanted && required.level && fact.level==required.level &&
        fact.distance==(seed?1:(uint64_t)required.distance+1)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus effect_role_proof_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectRoleProof *proof) {
    EffectRoleCheck check={.work=work,.material=material,.proof=proof,.selected=UINT32_MAX};
    XrXirStatus status=effect_role_proof_geometry(&check);if (status!=XR_XIR_OK) return status;
    for (uint32_t f=0;f<check.functions;++f) {
        uint32_t count=proof->total?proof->rows[f].count:0;
        if (!count) continue;
        EffectBodyRequest request=effect_body_owned_request(work,material);EffectBodyFunctionHead head={0};
        status=effect_body_request_head(&request,f,&head);if (status!=XR_XIR_OK) return status;
        const EffectRoleFunction *row=&proof->rows[f];
        for (uint32_t v=0;v<count;++v) {
            if (!xir_compile_work(work,sizeof(EffectRoleFact)+3)) return XR_XIR_BUDGET;
            EffectRoleFact fact=proof->facts[row->first+v];
            if (fact.level>2 || (fact.level?(!fact.distance || fact.distance>proof->total):
                (fact.distance || fact.instruction || fact.ordinal))) return XR_XIR_BAD_STRUCTURE;
            bool fixed=v<head.parameter_count && !xr_xir_callable_signature(material->domain.types,head.parameters[v]);
            if (fixed && fact.level!=2) return XR_XIR_BAD_STRUCTURE;
            if (!fact.level) continue;
            if (fact.instruction==UINT32_MAX) {
                if (!fixed || fact.level!=2 || fact.distance!=1 || fact.ordinal!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
                continue;
            }
            if (fact.instruction>=head.instruction_count) return XR_XIR_BAD_STRUCTURE;
            EffectRoleVerify verify={.wanted=row->first+v,.support=true};
            check.receiver=&verify;check.emit=effect_role_verify_edge;check.selected=fact.ordinal;
            status=effect_role_project(&check,f,&head,fact.instruction);
            if (status!=XR_XIR_OK || !verify.found) return status!=XR_XIR_OK?status:XR_XIR_BAD_STRUCTURE;
        }
        EffectRoleVerify verify={0};check.receiver=&verify;check.emit=effect_role_verify_edge;check.selected=UINT32_MAX;
        for (uint32_t i=0;i<head.instruction_count;++i) {
            status=effect_role_project(&check,f,&head,i);if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_role_proof_build(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,EffectRoleProof *proof) {
    if (!proof || proof->resources || proof->rows || proof->facts || proof->functions || proof->total) return XR_XIR_BAD_STRUCTURE;
    EffectRoleBuilder builder={.check={.work=work,.material=material,.proof=proof,.selected=UINT32_MAX},.proof=proof};
    EffectRoleCheck *check=&builder.check;XrXirStatus status=effect_role_frame(check);uint64_t total=0;
    for (uint32_t f=0;status==XR_XIR_OK && f<check->functions;++f) {
        uint32_t count=0;status=effect_role_value_count(check,f,&count);total+=count;
        if (total>UINT32_MAX) status=XR_XIR_BUDGET;
    }
    if (status!=XR_XIR_OK || !total) return status;
    proof->resources=work->resources;proof->functions=check->functions;
    proof->rows=xir_compile_calloc(work,proof->functions,sizeof(*proof->rows),&status);
    if (status==XR_XIR_OK) status=effect_role_frame(check);
    uint32_t first=0,formal=0;
    for (uint32_t f=0;status==XR_XIR_OK && f<check->functions;++f) {
        uint32_t count=0;status=effect_role_value_count(check,f,&count);
        if (status!=XR_XIR_OK) break;
        if (count>UINT32_MAX-first || !xir_compile_work(work,sizeof(EffectRoleFunction))) { status=XR_XIR_BUDGET;break; }
        const EffectBodyFunction *head=&material->functions[f];
        proof->rows[f]=(EffectRoleFunction){head->parameter_count,head->instruction_count,first,count,formal};
        first+=count;formal+=head->parameter_count;
    }
    proof->total=first;
    if (status==XR_XIR_OK) proof->facts=xir_compile_calloc(work,proof->total,sizeof(*proof->facts),&status);
    if (status==XR_XIR_OK) status=effect_role_proof_geometry(check);
    if (status==XR_XIR_OK) builder.heads=xir_compile_alloc(work,(size_t)proof->total*sizeof(*builder.heads),&status);
    if (status==XR_XIR_OK) status=effect_role_proof_geometry(check);
    if (status==XR_XIR_OK) builder.queue=xir_compile_alloc(work,(size_t)proof->total*sizeof(*builder.queue),&status);
    if (status==XR_XIR_OK) status=effect_role_proof_geometry(check);
    if (status==XR_XIR_OK) builder.pending=xir_compile_calloc(work,proof->total,sizeof(*builder.pending),&status);
    if (status==XR_XIR_OK) status=effect_role_proof_geometry(check);
    if (status==XR_XIR_OK && !xir_compile_work(work,(uint64_t)proof->total*sizeof(*builder.heads))) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) for (uint32_t n=0;n<proof->total;++n) builder.heads[n]=UINT32_MAX;
    check->receiver=&builder;check->emit=effect_role_build_edge;
    for (uint32_t f=0;status==XR_XIR_OK && f<check->functions;++f) {
        if (!proof->rows[f].count) continue;
        EffectBodyRequest request=effect_body_owned_request(work,material);EffectBodyFunctionHead head={0};
        status=effect_body_request_head(&request,f,&head);if (status!=XR_XIR_OK) break;
        check->active_head=&head;check->active_function=f;
        for (uint32_t p=0;p<head.parameter_count;++p) {
            if (!xir_compile_work(work,3+sizeof(EffectRoleFact))) { status=XR_XIR_BUDGET;break; }
            if (!xr_xir_callable_signature(material->domain.types,head.parameters[p]))
                proof->facts[proof->rows[f].first+p]=(EffectRoleFact){2,1,UINT32_MAX,UINT32_MAX};
        }
        for (uint32_t i=0;status==XR_XIR_OK && i<head.instruction_count;++i) status=effect_role_project(check,f,&head,i);
        check->active_head=NULL;
    }
    if (status==XR_XIR_OK) status=effect_role_build_closure(&builder);
    xr_compile_resources_free(builder.heads);xr_compile_resources_free(builder.queue);
    xr_compile_resources_free(builder.pending);xr_compile_resources_free(builder.edges);
    if (status==XR_XIR_OK) status=effect_role_proof_match(work,material,proof);
    return status;
}
