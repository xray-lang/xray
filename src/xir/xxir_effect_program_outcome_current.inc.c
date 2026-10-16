/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_program_outcome_current.inc.c - Current terminal outcome obligations
 */
typedef struct EffectProgramSuspension {
    EffectControlCheck input;
    uint32_t function,first_local,minimum_distance,best_target,best_instruction;
    XrXirEffect local;
} EffectProgramSuspension;

static XrXirStatus effect_program_suspension_ancestor(const XrXirCompileContext *work,
    const EffectFinalOutcomes *final,uint32_t function,uint32_t steps,uint32_t *output) {
    for (uint32_t s=0;s<steps;++s) {
        if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
        if (function>=final->count) return XR_XIR_BAD_STRUCTURE;
        const XrXirEffectWitness *from=&final->witnesses[function];
        if (from->cause!=XR_XIR_EFFECT_CAUSE_CALL || from->callee>=final->count ||
            !from->distance || from->distance>=final->count ||
            final->functions[from->callee].suspend!=final->functions[function].suspend ||
            final->witnesses[from->callee].distance!=from->distance-1) return XR_XIR_BAD_STRUCTURE;
        function=from->callee;
    }
    *output=function;return XR_XIR_OK;
}

/* FIFO seed order is ascending function ID. At every later level, prepended
 * incoming Input edges visit larger caller IDs first. Comparing these real
 * descending cause paths reproduces that order without a retained queue. */
static XrXirStatus effect_program_suspension_order(const XrXirCompileContext *work,
    const EffectFinalOutcomes *final,uint32_t a,uint32_t b,uint32_t distance,int *order) {
    *order=0;
    if (a==b) return XR_XIR_OK;
    for (uint32_t level=0;level<=distance;++level) {
        uint32_t left=UINT32_MAX,right=UINT32_MAX;
        XrXirStatus status=effect_program_suspension_ancestor(work,final,a,distance-level,&left);
        if (status==XR_XIR_OK) status=effect_program_suspension_ancestor(work,final,b,distance-level,&right);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if (left!=right) {
            *order=level?(left>right?-1:1):(left<right?-1:1);
            return XR_XIR_OK;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_program_suspension_row(EffectProgramSuspension *read,
    uint32_t i,XrXirOp opcode,const XrXirInstruction *op,XrXirFunctionEffects seed) {
    const XrXirCompileContext *work=read->input.input.read.owner;
    const EffectFinalOutcomes *final=read->input.final;
    uint32_t f=read->function;
    XrXirEffect actual=final->functions[f].suspend;
    if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
    if (seed.suspend>actual) return XR_XIR_BAD_STRUCTURE;
    if (seed.suspend>read->local) read->local=seed.suspend;
    if (actual && seed.suspend==actual && read->first_local==UINT32_MAX)
        read->first_local=i;
    if (!effect_control_direct(opcode)) return XR_XIR_OK;
    if (!op) return XR_XIR_BAD_STRUCTURE;
    bool direct=false,cleanup=false;uint32_t target=UINT32_MAX;
    XrXirStatus status=effect_control_target(&read->input,op,&direct,&target,&cleanup);
    if (status!=XR_XIR_OK) return status;
    if (!direct) return XR_XIR_BAD_STRUCTURE;
    if (cleanup) return XR_XIR_OK;
    XrXirEffect callee=final->functions[target].suspend;
    if (callee>actual) return XR_XIR_BAD_STRUCTURE;
    if (actual && callee==actual) {
        uint32_t distance=final->witnesses[target].distance;
        if (distance>=final->count) return XR_XIR_BAD_STRUCTURE;
        ++distance;
        bool select=distance<read->minimum_distance;
        if (distance==read->minimum_distance) {
            int order=0;
            status=effect_program_suspension_order(work,final,target,read->best_target,distance-1,&order);
            if (status!=XR_XIR_OK) return status;
            select=order<0 || (!order && i>read->best_instruction);
        }
        if (select) {
            read->minimum_distance=distance;read->best_target=target;read->best_instruction=i;
        }
    }
    return XR_XIR_OK;
}

/* Every current seed and direct edge supplies a lower bound. A finite positive
 * result must have the authentic first shortest descending cause, so an
 * unseeded SCC cannot invent suspension or replace the canonical cause. */
static XrXirStatus effect_program_suspension_current(const XrXirCompileContext *work,
    const XrXirEffects *effects) {
    const EffectBodyMaterial *body=effects->invocations->body;
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    EffectBodyRequest input=effect_body_owned_request(work,body);
    for (uint32_t f=0;f<effects->count;++f) {
        XrXirEffect actual=final->functions[f].suspend;
        if (actual<XR_XIR_EFFECT_NONE || actual>XR_XIR_EFFECT_MAY) return XR_XIR_BAD_STRUCTURE;
        EffectProgramSuspension read={.input={.input=input,.final=final,.count=effects->count},
            .function=f,.first_local=UINT32_MAX,.minimum_distance=UINT32_MAX,
            .best_target=UINT32_MAX,.best_instruction=UINT32_MAX};
        if (!xir_compile_work(work,sizeof(read))) return XR_XIR_BUDGET;
        const EffectBodyFunction *head=&body->functions[f];
        EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
        for (uint32_t r=0;r<head->run_count;++r) {
            const EffectBodyRun *run=NULL;
            XrXirStatus status=effect_body_range_ref(&input.read,head,&cursor,&run);
            if (status!=XR_XIR_OK) return status;
            uint32_t opcode=XR_XIR_INVALID;
            status=effect_body_run_word(&input.read,run,run->first,0,&opcode);
            if (status!=XR_XIR_OK) return status;
            XrXirFunctionEffects seed={0};
            EffectSeedProjection projection=effect_seed_opcode((XrXirOp)opcode,&seed);
            if (!(run->mask&UINT32_C(1)) && projection==EFFECT_SEED_LOCAL &&
                !effect_control_direct((XrXirOp)opcode)) {
                status=effect_program_suspension_row(&read,run->first,(XrXirOp)opcode,NULL,seed);
                if (status!=XR_XIR_OK) return status;
                continue;
            }
            for (uint32_t i=run->first;i-run->first<run->count;++i) {
                XrXirInstruction op;
                status=effect_body_instruction_ref_value(&input.read,run,i,&op);
                if (status!=XR_XIR_OK) return status;
                XrXirType callable=XR_XIR_UNIT;
                if (op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_INVOKE_INDIRECT) {
                    if (op.immediate<0 || (uint64_t)op.immediate>=(uint64_t)head->parameter_count+head->instruction_count)
                        return XR_XIR_BAD_STRUCTURE;
                    status=effect_body_request_type(&input,f,(uint32_t)op.immediate,&callable);
                    if (status!=XR_XIR_OK) return status;
                }
                seed=(XrXirFunctionEffects){0};
                if (!effect_seed_type(input.domain.types,callable,&op,&seed)) return XR_XIR_BAD_STRUCTURE;
                status=effect_program_suspension_row(&read,i,op.op,&op,seed);
                if (status!=XR_XIR_OK) return status;
            }
        }
        if (cursor.first!=head->instruction_count || cursor.run!=head->run_count ||
            body->control[f].suspend!=read.local) return XR_XIR_BAD_STRUCTURE;
        const XrXirEffectWitness *witness=&final->witnesses[f];
        if (!actual) {
            if (!effect_control_cause_is(witness,XR_XIR_EFFECT_CAUSE_NONE,0,0,0)) return XR_XIR_BAD_STRUCTURE;
        } else if (read.first_local!=UINT32_MAX) {
            XrXirEffectCause cause=actual==XR_XIR_EFFECT_MAY?XR_XIR_EFFECT_CAUSE_SUSPEND:XR_XIR_EFFECT_CAUSE_INDIRECT;
            if (!effect_control_cause_is(witness,cause,read.first_local,UINT32_MAX,0)) return XR_XIR_BAD_STRUCTURE;
        } else if (read.minimum_distance==UINT32_MAX || witness->distance>=final->count ||
            !effect_control_cause_is(witness,XR_XIR_EFFECT_CAUSE_CALL,read.best_instruction,
                read.best_target,read.minimum_distance))
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* These are necessary public projections of the current error rows. Known
 * atoms still require the independent flow and marker proof; this comparison
 * does not turn their stored rows or qualification status into authority. */
static XrXirStatus effect_program_error_projection(const XrXirCompileContext *work,
    const XrXirEffects *effects) {
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    uint64_t bits=(uint64_t)final->atom_count+2;
    if (final->error_rows<effects->count || final->words!=(bits+63)/64)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<effects->count;++f) {
        const uint64_t *row=final->errors+(size_t)f*final->words;
        bool unknown=error_bit(row,0),untyped=error_bit(row,1),known=false;
        for (uint32_t a=0;a<final->atom_count;++a) {
            if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            known|=error_bit(row,a+2);
        }
        if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
        XrXirEffect throws=untyped || known?XR_XIR_EFFECT_MAY:
            unknown?XR_XIR_EFFECT_UNKNOWN:XR_XIR_EFFECT_NONE;
        if (final->functions[f].throws!=throws ||
            ((unknown || untyped) && final->task_errors[f]!=XR_XIR_BAD_TYPE) ||
            (!unknown && !untyped && !known && final->task_errors[f]!=XR_XIR_OK))
            return XR_XIR_BAD_STRUCTURE;
        if (bits%64 && (row[final->words-1]>>(bits%64))) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
