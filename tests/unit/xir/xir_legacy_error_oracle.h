/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_legacy_error_oracle.h - Independent allocating Error regression oracle
 *
 * These helpers never publish a permission or run in the production receiver.
 */
#ifndef XIR_LEGACY_ERROR_ORACLE_H
#define XIR_LEGACY_ERROR_ORACLE_H
typedef struct EffectControlCheck {
    EffectBodyRequest input;
    const EffectFinalOutcomes *final;
    uint32_t *heads,*queue;
    uint8_t *seen;
    EffectEdge *edges;
    uint32_t count,edge_count;
} EffectControlCheck;

static inline void effect_control_check_free(EffectControlCheck *check) {
    xr_compile_resources_free(check->heads);xr_compile_resources_free(check->queue);
    xr_compile_resources_free(check->seen);xr_compile_resources_free(check->edges);
}

static XrXirStatus effect_control_seed_apply(EffectControlCheck *check,uint32_t f,
    uint32_t instruction,XrXirOp opcode,uint32_t count,XrXirFunctionEffects local) {
    const XrXirCompileContext *work=check->input.read.owner;XrXirEffect actual=check->final->functions[f].suspend;
    if (!xir_compile_work(work,12)) return XR_XIR_BUDGET;
    if (local.suspend>actual) return XR_XIR_BAD_STRUCTURE;
    if (actual && local.suspend==actual && !(check->seen[f]&1)) {
        XrXirEffectCause cause=actual==XR_XIR_EFFECT_MAY?XR_XIR_EFFECT_CAUSE_SUSPEND:XR_XIR_EFFECT_CAUSE_INDIRECT;
        if (!effect_control_cause_is(&check->final->witnesses[f],cause,instruction,UINT32_MAX,0)) return XR_XIR_BAD_STRUCTURE;
        check->seen[f]|=1;
    }
    if (opcode==XR_XIR_GO) {
        if (check->final->task_creation[f]!=XR_XIR_EFFECT_MAY) return XR_XIR_BAD_STRUCTURE;
        check->seen[f]|=2;
    }
    if (opcode==XR_XIR_SLOT_PLACE) check->seen[f]|=4;
    if (effect_control_direct(opcode)) {
        if (count>UINT32_MAX-check->edge_count) return XR_XIR_BUDGET;
        check->edge_count+=count;
    }
    return XR_XIR_OK;
}

/* A constant opcode proves the same terminal for its entire canonical run.
 * Field-dependent calls still decode every actual ordinal. All logical edges
 * are counted here and receive their real targets in the incoming index. */
static XrXirStatus effect_control_seeds(EffectControlCheck *check,uint32_t f) {
    const EffectBodyMaterial *body=check->input.owner;const EffectBodyFunction *owned=&body->functions[f];
    XrXirEffect actual=check->final->functions[f].suspend;
    if (actual<XR_XIR_EFFECT_NONE || actual>XR_XIR_EFFECT_MAY ||
        (check->final->task_creation[f]!=XR_XIR_EFFECT_NONE && check->final->task_creation[f]!=XR_XIR_EFFECT_MAY))
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    for (uint32_t r=0;r<owned->run_count;++r) {
        const EffectBodyRun *run=NULL;XrXirStatus status=effect_body_range_ref(&check->input.read,owned,&cursor,&run);
        if (status!=XR_XIR_OK) return status;
        uint32_t opcode=XR_XIR_INVALID;status=effect_body_run_word(&check->input.read,run,run->first,0,&opcode);
        if (status!=XR_XIR_OK) return status;
        XrXirFunctionEffects local={0};EffectSeedProjection projection=effect_seed_opcode((XrXirOp)opcode,&local);
        if (!(run->mask&UINT32_C(1)) && projection==EFFECT_SEED_LOCAL) {
            status=effect_control_seed_apply(check,f,run->first,(XrXirOp)opcode,run->count,local);
            if (status!=XR_XIR_OK) return status;
            continue;
        }
        for (uint32_t i=run->first;i<run->first+run->count;++i) {
            if (run->mask&UINT32_C(1)) {
                status=effect_body_run_word(&check->input.read,run,i,0,&opcode);
                if (status!=XR_XIR_OK) return status;
            }
            local=(XrXirFunctionEffects){0};projection=effect_seed_opcode((XrXirOp)opcode,&local);
            if (projection==EFFECT_SEED_INVALID) return XR_XIR_BAD_STRUCTURE;
            if (projection==EFFECT_SEED_FIELDS) {
                XrXirInstruction op;status=effect_body_instruction_ref_value(&check->input.read,run,i,&op);
                if (status!=XR_XIR_OK) return status;
                XrXirType callable=XR_XIR_UNIT;
                if (op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_INVOKE_INDIRECT) {
                    if (op.immediate<0 || (uint64_t)op.immediate>=(uint64_t)owned->parameter_count+owned->instruction_count)
                        return XR_XIR_BAD_STRUCTURE;
                    status=effect_body_request_type(&check->input,f,(uint32_t)op.immediate,&callable);
                    if (status!=XR_XIR_OK) return status;
                }
                if (!effect_seed_type(check->input.domain.types,callable,&op,&local)) return XR_XIR_BAD_STRUCTURE;
            }
            status=effect_control_seed_apply(check,f,i,(XrXirOp)opcode,1,local);
            if (status!=XR_XIR_OK) return status;
        }
    }
    if (cursor.first!=owned->instruction_count || cursor.run!=owned->run_count) return XR_XIR_BAD_STRUCTURE;
    if (!actual && !effect_control_cause_is(&check->final->witnesses[f],XR_XIR_EFFECT_CAUSE_NONE,0,0,0))
        return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

/* Prepending ascending Input IDs reproduces the published cause tie order.
 * Closure checks consume proposed values, never provisional solver values. */
static XrXirStatus effect_control_incoming(EffectControlCheck *check) {
    const XrXirCompileContext *work=check->input.read.owner;const EffectBodyMaterial *body=check->input.owner;uint32_t at=0;
    for (uint32_t f=0;f<check->count;++f) {
        const EffectBodyFunction *owned=&body->functions[f];EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
        for (uint32_t r=0;r<owned->run_count;++r) {
            const EffectBodyRun *run=NULL;XrXirStatus status=effect_body_range_ref(&check->input.read,owned,&cursor,&run);
            if (status!=XR_XIR_OK) return status;
            uint32_t opcode=XR_XIR_INVALID;status=effect_body_run_word(&check->input.read,run,run->first,0,&opcode);
            if (status!=XR_XIR_OK) return status;
            if (!(run->mask&UINT32_C(1)) && !effect_control_direct((XrXirOp)opcode)) continue;
            for (uint32_t i=run->first;i<run->first+run->count;++i) {
                if (run->mask&UINT32_C(1)) {
                    status=effect_body_run_word(&check->input.read,run,i,0,&opcode);
                    if (status!=XR_XIR_OK) return status;
                    if (!effect_control_direct((XrXirOp)opcode)) continue;
                }
                XrXirInstruction op;status=effect_body_instruction_ref_value(&check->input.read,run,i,&op);
                if (status!=XR_XIR_OK) return status;
                bool direct=false,cleanup=false;uint32_t target=UINT32_MAX;
                status=effect_control_target(&check->input,check->count,&op,&direct,&target,&cleanup);
                if (status!=XR_XIR_OK) return status;
                if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
                if (!direct || at>=check->edge_count || (!cleanup &&
                    (check->final->functions[f].suspend<check->final->functions[target].suspend ||
                     check->final->task_creation[f]<check->final->task_creation[target]))) return XR_XIR_BAD_STRUCTURE;
                check->edges[at]=(EffectEdge){f,check->heads[target],i,cleanup};check->heads[target]=at++;
            }
        }
        if (cursor.first!=owned->instruction_count || cursor.run!=owned->run_count) return XR_XIR_BAD_STRUCTURE;
    }
    return at==check->edge_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

/* Each positive suspension has the original first shortest Input cause.
 * Task creation and bad borrow shape must also reach real terminals. A closed
 * cycle with invented positive facts has no seed and cannot pass this walk. */
static XrXirStatus effect_control_reachable(EffectControlCheck *check,uint8_t mode) {
    const XrXirCompileContext *work=check->input.read.owner;uint32_t front=0,back=0;
    for (uint32_t f=0;f<check->count;++f) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (check->seen[f]&mode) check->queue[back++]=f;
    }
    while (front<back) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        uint32_t callee=check->queue[front++];
        for (uint32_t e=check->heads[callee];e!=UINT32_MAX;e=check->edges[e].next) {
            if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
            if (e>=check->edge_count) return XR_XIR_BAD_STRUCTURE;
            const EffectEdge *edge=&check->edges[e];uint32_t caller=edge->caller;
            if (caller>=check->count || (edge->next!=UINT32_MAX && edge->next>=e)) return XR_XIR_BAD_STRUCTURE;
            if ((mode!=4 && edge->cleanup) || (check->seen[caller]&mode)) continue;
            if (mode==1) {
                if (check->final->functions[caller].suspend!=check->final->functions[callee].suspend) continue;
                const XrXirEffectWitness *from=&check->final->witnesses[callee];
                if (from->distance==UINT32_MAX || !effect_control_cause_is(&check->final->witnesses[caller],
                    XR_XIR_EFFECT_CAUSE_CALL,edge->instruction,callee,from->distance+1)) return XR_XIR_BAD_STRUCTURE;
            }
            if (back>=check->count) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            check->seen[caller]|=mode;check->queue[back++]=caller;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_control_prepare_fields(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectFinalOutcomes *final,uint32_t count,
    EffectControlCheck *check) {
    if(!check || check->heads || check->queue || check->seen || check->edges || check->final ||
        !xir_compile_context_valid(work) || !effect_body_material_shape(material) ||
        material->resources!=work->resources || count!=effect_body_count(material) || !final)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(check->input)+sizeof(check->final)+sizeof(check->count))) return XR_XIR_BUDGET;
    check->input=effect_body_owned_request(work,material);check->final=final;check->count=count;
    XrXirStatus status=XR_XIR_OK;
    check->heads=xir_compile_calloc(work,check->count,sizeof(*check->heads),&status);
    if (status==XR_XIR_OK) check->queue=xir_compile_calloc(work,check->count,sizeof(*check->queue),&status);
    if (status==XR_XIR_OK) check->seen=xir_compile_calloc(work,check->count,sizeof(*check->seen),&status);
    if(status==XR_XIR_OK) check->input=effect_body_owned_request(work,material);
    for (uint32_t f=0;f<check->count && status==XR_XIR_OK;++f) {
        if (!xir_compile_work(work,1)) { status=XR_XIR_BUDGET;break; }
        check->heads[f]=UINT32_MAX;status=effect_control_seeds(check,f);
    }
    if (status==XR_XIR_OK && check->edge_count)
        check->edges=xir_compile_calloc(work,check->edge_count,sizeof(*check->edges),&status);
    if(status==XR_XIR_OK) check->input=effect_body_owned_request(work,material);
    if (status==XR_XIR_OK) status=effect_control_incoming(check);
    for (uint8_t mode=1;mode<=4 && status==XR_XIR_OK;mode<<=1) {
        status=effect_control_reachable(check,mode);
    }
    for (uint32_t f=0;f<check->count && status==XR_XIR_OK;++f) {
        if (!xir_compile_work(work,6)) { status=XR_XIR_BUDGET;break; }
        bool shape=(check->seen[f]&4)!=0,root=final->root[f].requires_root || final->root[f].unresolved;
        if (!!final->functions[f].suspend!=!!(check->seen[f]&1) ||
            (final->task_creation[f]==XR_XIR_EFFECT_MAY)!=!!(check->seen[f]&2) ||
            final->go_safe[f]!=(shape || root?XR_XIR_BAD_TYPE:XR_XIR_OK)) status=XR_XIR_BAD_STRUCTURE;
        else status=effect_control_entry(work,&check->input,check->final,f);
    }
    return status;
}

static inline void effect_legacy_error_oracle_free(XrXirEffects *fresh) {
    effect_outcomes_clear(&fresh->outcomes_work,fresh->count);
}
static inline XrXirStatus effect_legacy_error_oracle_prepare(const XrXirCompileContext *work,
    const XrXirEffects *effects,XrXirEffects *fresh) {
    const EffectFinalOutcomes *actual=effect_final_outcomes(effects);
    if (!actual) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(*fresh))) return XR_XIR_BUDGET;
    *fresh=(XrXirEffects){.resources=work->resources,.count=effects->count,.root_input=actual,
        .invocations=effects->invocations,
        .result_borrow=effects->result_borrow,.generation=effects->invocations->body,
        .cells=effects->cells};
    fresh->outcomes_work.source_type_count=actual->source_type_count;
    XrXirStatus status=XR_XIR_OK;
    fresh->outcomes_work.functions=xir_compile_calloc(work,fresh->count,sizeof(*fresh->outcomes_work.functions),&status);
    if (status==XR_XIR_OK) fresh->outcomes_work.task_errors=xir_compile_calloc(work,fresh->count,sizeof(*fresh->outcomes_work.task_errors),&status);
    return status;
}
static inline XrXirStatus effect_legacy_error_oracle_match(const XrXirCompileContext *work,
    const XrXirEffects *effects,const XrXirEffects *fresh) {
    const EffectFinalOutcomes *actual=effect_final_outcomes(effects);
    if (!actual) return XR_XIR_BAD_STRUCTURE;
    if (actual->error_rows!=fresh->outcomes_work.error_rows ||
        actual->atom_count!=fresh->outcomes_work.atom_count || actual->words!=fresh->outcomes_work.words ||
        actual->atom_count>actual->atom_capacity || !!actual->atoms!=!!actual->atom_capacity ||
        !actual->errors || !fresh->outcomes_work.errors) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t a=0;a<actual->atom_count;++a) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (actual->atoms[a].type!=fresh->outcomes_work.atoms[a].type || actual->atoms[a].variant!=fresh->outcomes_work.atoms[a].variant)
            return XR_XIR_BAD_STRUCTURE;
    }
    uint64_t words=(uint64_t)actual->error_rows*actual->words;
    for (uint64_t w=0;w<words;++w) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if (actual->errors[w]!=fresh->outcomes_work.errors[w]) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static inline XrXirStatus effect_legacy_error_dependencies_move(const XrXirCompileContext *work,
    EffectControlCheck *input,EffectGraph *graph) {
    if (!xir_compile_context_valid(work) || !input || !graph || !input->final ||
        input->final->resources!=work->resources || graph->material!=input->input.owner ||
        graph->physical_owned || graph->heads || graph->queue || graph->queued || graph->edges || graph->edge_count ||
        !input->heads || !input->queue || !input->seen || !!input->edges!=!!input->edge_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,8*sizeof(void *)+sizeof(graph->edge_count))) return XR_XIR_BUDGET;
    graph->heads=input->heads;graph->queue=input->queue;graph->queued=input->seen;
    graph->edges=input->edges;graph->edge_count=input->edge_count;
    input->heads=NULL;input->queue=NULL;input->seen=NULL;input->edges=NULL;return XR_XIR_OK;
}
static inline XrXirStatus effect_legacy_control_prepare(const XrXirCompileContext *work,
    const XrXirEffects *effects,EffectControlCheck *check) {
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    if(!final || !xir_effects_context_matches(work,effects,final->count)) return XR_XIR_BAD_STRUCTURE;
    return effect_control_prepare_fields(work,effects->invocations->body,final,final->count,check);
}
#endif // XIR_LEGACY_ERROR_ORACLE_H
