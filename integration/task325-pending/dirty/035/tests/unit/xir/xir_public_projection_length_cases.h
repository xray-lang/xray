/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_public_projection_length_cases.h - Independent scalar trace and real projection refusals
 */
#ifndef XIR_PUBLIC_PROJECTION_LENGTH_CASES_H
#define XIR_PUBLIC_PROJECTION_LENGTH_CASES_H

/* Genuine owned CONTEXT_CALL terminal; the producer and raw fixture have
 * died. This verifies the opcode-only cause path and its canonical boundaries,
 * without using the new reader as the expected instruction oracle. */
static void conditional_compact_cause_opcode(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    uint32_t selected=certificate->final->roots[1],atom=6;
    CHECK(selected<certificate->final->count &&
        certificate->final->nodes[selected].body==1 &&
        certificate->final->nodes[selected].causes[atom].witness.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL);
    EffectBodyFunction *function=&certificate->body->functions[1];
    CHECK(function->runs && function->instruction_count==2 && function->run_count &&
        function->runs[0].first==0 && function->runs[0].base.op==XR_XIR_CALL_INDIRECT);
    CHECK(effect_invocation_certificate_cause(context,certificate,selected,atom)==XR_XIR_OK);
    uint32_t first=function->runs[0].first,count=function->runs[0].count,mask=function->runs[0].mask,opcode=function->runs[0].base.op;
    uint32_t words=function->run_count,capacity=function->run_capacity,runs=function->run_count;
    EffectInvocationTraceRequest request={.function=1,.domain=1};
    for (uint32_t damage=0;damage<8;++damage) {
        if (!damage) function->runs[0].base.op=XR_XIR_RETURN;
        else if (damage==1) function->runs[0].base.op=XR_XIR_INVALID;
        else if (damage==2) function->runs[0].first=1;
        else if (damage==3) function->runs[0].count=0;
        else if (damage==4) function->runs[0].mask|=UINT32_C(1024);
        else if (damage==5) function->run_count=0;
        else if (damage==6) function->run_capacity=words-1;
        else function->run_count=0;
        CHECK(effect_invocation_certificate_cause(context,certificate,selected,atom)==XR_XIR_BAD_STRUCTURE);
        uint32_t length=77;
        CHECK(effect_invocation_public_walk(context,certificate,&request,&length,NULL)==XR_XIR_BAD_STRUCTURE && length==77);
        function->runs[0].first=first;function->runs[0].count=count;function->runs[0].mask=mask;function->runs[0].base.op=(XrXirOp)opcode;
        function->run_count=words;function->run_capacity=capacity;function->run_count=runs;
        CHECK(effect_invocation_certificate_cause(context,certificate,selected,atom)==XR_XIR_OK);
    }
    uint32_t length=77;
    CHECK(effect_invocation_public_walk(context,certificate,&request,&length,NULL)==XR_XIR_OK && length==1);
}

static XrXirStatus conditional_projection_length_reference(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectInvocationTraceRequest *request,
    uint32_t *length,const EffectInvocationPublicTraceOutput *output) {
    if (!length || (output && !!output->steps!=!!output->count)) return XR_XIR_BAD_STRUCTURE;
    uint32_t count=0;EffectInvocationTraceSelection selection={UINT32_MAX,{0}};
    XrXirStatus status=effect_invocation_trace_walk_selected(work,certificate,request,&count,&selection,NULL);
    if (status!=XR_XIR_OK) return status;
    bool symbolic=false;
    if (!xir_compile_work(work,sizeof(EffectInvocationProjectedCause))) return XR_XIR_BUDGET;
    const EffectInvocationProjectedCause projected={selection.atom,&selection.witness};
    XrXirRootEffectWitness terminal={0};
    status=effect_invocation_public_transform(work,certificate,request->function,&projected,
        output?&terminal:NULL,&symbolic);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationFinal *equations=certificate->final;
    uint32_t limit=equations->count;
    if (!limit || limit>equations->capacity) return XR_XIR_BAD_STRUCTURE;
    uint32_t published=symbolic && count?1:count;
    if (output && output->count!=published) return XR_XIR_BAD_STRUCTURE;
    if (!count) { *length=0;return XR_XIR_OK; }
    if (symbolic) {
        if (output) {
            if (!xir_compile_work(work,sizeof(*output->steps))) return XR_XIR_BUDGET;
            output->steps[0]=(XrXirRootCauseStep){certificate->origins[request->function].function,
                terminal.instruction,terminal.callee,terminal.slot,terminal.distance,terminal.cause};
        }
        *length=1;return XR_XIR_OK;
    }
    if (count>limit) return XR_XIR_BAD_STRUCTURE;
    uint32_t atom=selection.atom;
    uint32_t current=equations->roots[request->function];
    for (uint32_t at=0;at<count;++at) {
        if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&equations->nodes[current];
        const EffectInvocationCause *cause=&node->causes[atom];
        XrXirRootEffectWitness witness=effect_invocation_public_witness(certificate,node,cause);
        if (!witness.cause || witness.distance>=limit || witness.distance!=count-at-1)
            return XR_XIR_BAD_STRUCTURE;
        if (output) {
            if (!xir_compile_work(work,sizeof(*output->steps))) return XR_XIR_BUDGET;
            output->steps[at]=(XrXirRootCauseStep){certificate->origins[node->body].function,
                witness.instruction,witness.callee,witness.slot,witness.distance,witness.cause};
        }
        if (witness.distance) {
            if ((witness.cause!=XR_XIR_ROOT_CAUSE_CALL && witness.cause!=XR_XIR_ROOT_CAUSE_CLEANUP &&
                witness.cause!=XR_XIR_ROOT_CAUSE_REQUIREMENT) || witness.callee==UINT32_MAX ||
                witness.slot!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            current=cause->next;continue;
        }
        /* The current public Cell terminal belongs to its emitted function.
         * A foreign physical namespace requires a separate semantic cutover;
         * its private proof is retained, never relabeled as this terminal. */
        if ((witness.cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS ||
            witness.cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER) && witness.callee!=UINT32_MAX)
            return XR_XIR_BAD_STRUCTURE;
        if (witness.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL &&
            (!(request->domain&1) || witness.callee!=UINT32_MAX || witness.slot!=UINT32_MAX))
            return XR_XIR_BAD_STRUCTURE;
    }
    *length=count;return XR_XIR_OK;
}

/* Every public domain is proved before publication. Function and environment
 * identities are separate: several keys may execute one recursive body. */
static void conditional_projection_length_reject(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    EffectInvocationTraceRequest request={.function=1,.domain=1};uint32_t length=73;
    CHECK(conditional_projection_length_reference(context,certificate,&request,&length,NULL)==XR_XIR_BAD_STRUCTURE && length==73);
    CHECK(effect_invocation_public_walk(context,certificate,&request,&length,NULL)==XR_XIR_BAD_STRUCTURE && length==73);
    XrXirRootCauseStep step;memset(&step,0xa5,sizeof(step));XrXirRootCauseStep before=step;
    EffectInvocationPublicTraceOutput output={&step,1};
    CHECK(effect_invocation_public_walk(context,certificate,&request,&length,&output)==XR_XIR_BAD_STRUCTURE &&
        length==73 && !memcmp(&step,&before,sizeof(step)));
    CHECK(effect_invocation_public_gate(context,certificate)==XR_XIR_BAD_STRUCTURE);
}
static void conditional_projection_length_attacks(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    EffectFinalFunctions *summary=&certificate->final->summary;
    CHECK(summary->count==4 && summary->functions[1].capacity && summary->functions[1].storage);
    EffectFinalFunction *record=&summary->functions[1],saved=*record;
    XrXirRootTerm term=record->storage[0];XrXirRootEffects facts=summary->facts[1];
    XrXirRootEffectWitness witness=summary->witnesses[summary->count+1];
    for(uint32_t attack=0;attack<8;++attack) {
        if(attack==0)--record->formula.term_count;
        if(attack==1)--record->capacity;
        if(attack==2)record->formula.terms=NULL;
        if(attack==3)record->storage[0].kind=term.kind==XR_XIR_ROOT_TERM_PARAMETER?
            XR_XIR_ROOT_TERM_CONTEXT_CALL:XR_XIR_ROOT_TERM_PARAMETER;
        if(attack==4)record->storage[0].index=UINT32_MAX;
        if(attack==5)record->formula.constant_mask^=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        if(attack==6)summary->witnesses[summary->count+1].slot=0;
        if(attack==7)summary->facts[1].unresolved=false;
        conditional_projection_length_reject(context,certificate);
        *record=saved;record->storage[0]=term;summary->facts[1]=facts;summary->witnesses[summary->count+1]=witness;
        CHECK(effect_invocation_public_gate(context,certificate)==XR_XIR_OK);
        EffectInvocationTraceRequest request={.function=1,.domain=1};uint32_t length=73,reference=74;
        CHECK(conditional_projection_length_reference(context,certificate,&request,&reference,NULL)==XR_XIR_OK && reference==1);
        CHECK(effect_invocation_public_walk(context,certificate,&request,&length,NULL)==XR_XIR_OK && length==reference);
    }
    for(uint32_t f=0;f<4;++f)for(uint32_t domain=0;domain<4;++domain) {
        EffectInvocationTraceRequest request={.function=f,.domain=domain};uint32_t length=73,reference=74;
        CHECK(conditional_projection_length_reference(context,certificate,&request,&reference,NULL)==XR_XIR_OK);
        CHECK(effect_invocation_public_walk(context,certificate,&request,&length,NULL)==XR_XIR_OK && length==reference);
    }
    puts("PUBLIC_LENGTH real four-domain/scalar reference, 8 projection/term/witness refusals, unchanged outputs and restored controls PASS");
}

/* Literal domains of the genuine four-function Checked producer. ROOT
 * and UNKNOWN are independent, and constant domains exclude symbolic atoms. */
static void conditional_compact_projection_domains(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    const uint32_t expected[4][4]={
        {UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX},
        {UINT32_MAX,6,UINT32_MAX,UINT32_MAX},
        {UINT32_MAX,3,UINT32_MAX,UINT32_MAX},
        {0,UINT32_MAX,0,UINT32_MAX}};
    for (uint32_t f=0;f<4;++f) {
        uint32_t selected[4];
        CHECK(effect_invocation_projection_domains(context,certificate,f,0,4,selected)==XR_XIR_OK);
        for (uint32_t domain=0;domain<4;++domain) {
            CHECK(selected[domain]==expected[f][domain]);
            XrXirRootEffectWitness chosen;
            CHECK(effect_invocation_selected_witness(context,certificate,f,selected[domain],&chosen)==XR_XIR_OK);
            if (expected[f][domain]==UINT32_MAX) CHECK(!chosen.cause);
            else CHECK(chosen.distance==0 && chosen.callee==UINT32_MAX &&
                chosen.cause==(f==1?XR_XIR_ROOT_CAUSE_CONTEXT_CALL:
                    f==2?XR_XIR_ROOT_CAUSE_CELL_PARAMETER:XR_XIR_ROOT_CAUSE_INITIALIZER));
            uint32_t atom=UINT32_C(77);XrXirRootEffectWitness witness={0};
            CHECK(effect_invocation_projection_select(context,certificate,f,domain,&atom,&witness)==XR_XIR_OK &&
                atom==expected[f][domain] && effect_invocation_witness_equal(&witness,&chosen));
        }
    }
    uint32_t selected[4];
    uint32_t root=certificate->final->roots[1];EffectInvocationNode *node=&certificate->final->nodes[root];
    EffectInvocationCause *cause=&node->causes[6],*local=&effect_invocation_local_causes(node)[6];
    XrXirRootEffectWitness saved=cause->witness,local_saved=local->witness;
    uint32_t stream_first=certificate->body->functions[1].runs[0].first;
    for (uint32_t damage=0;damage<5;++damage) {
        if (!damage) cause->witness.slot=0;
        else if (damage==1) local->witness.cause=XR_XIR_ROOT_CAUSE_PARAMETER;
        else if (damage==2) certificate->body->functions[1].runs[0].first=1;
        else if (damage==3) certificate->final->roots[1]=certificate->final->roots[0];
        CHECK(effect_invocation_projection_domains(context,certificate,1,damage==4?1u:0u,4,selected)==
            XR_XIR_BAD_STRUCTURE);
        /* Private scratch has no failure value. The public request still
         * preserves its output when the same damaged proof is rejected. */
        EffectInvocationTraceRequest request={.function=1,.domain=damage==4?4u:1u};
        uint32_t length=77;
        CHECK(effect_invocation_public_walk(context,certificate,&request,&length,NULL)==
            XR_XIR_BAD_STRUCTURE && length==77);
        cause->witness=saved;local->witness=local_saved;
        certificate->body->functions[1].runs[0].first=stream_first;certificate->final->roots[1]=root;
        CHECK(effect_invocation_projection_domains(context,certificate,1,0,4,selected)==XR_XIR_OK &&
            selected[1]==6 && selected[0]==UINT32_MAX && selected[2]==UINT32_MAX &&
            selected[3]==UINT32_MAX);
    }
    conditional_projection_length_attacks(context,certificate);
}

#endif // XIR_PUBLIC_PROJECTION_LENGTH_CASES_H
