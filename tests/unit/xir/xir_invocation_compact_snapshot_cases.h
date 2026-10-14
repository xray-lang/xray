/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_compact_snapshot_cases.h - Lossless final owning body boundaries
 */
#ifndef XIR_INVOCATION_COMPACT_SNAPSHOT_CASES_H
#define XIR_INVOCATION_COMPACT_SNAPSHOT_CASES_H

/* These storage-only rows deliberately contain unused words. They do not
 * claim IR admission; the independent Checked case below uses valid IR. */
static void conditional_compact_rows(XrXirInstruction *ops) {
    for (uint32_t i=0;i<8;++i) {
        uint64_t bits=i<4 ? UINT64_C(0xfffffff100000003)+i :
            UINT64_C(0x8000000000000000)+(i-4)*UINT64_C(0x0010000000000000);
        int64_t immediate;memcpy(&immediate,&bits,sizeof(immediate));
        ops[i]=(XrXirInstruction){.op=i<4?XR_XIR_CONST_INT:XR_XIR_CONST_FLOAT,
            .type=i<4?XR_XIR_I64:XR_XIR_F64,.args={UINT32_MAX-i,19+i*7},
            .targets={7+i*3,UINT32_MAX-i*5},.immediate=immediate,
            .type_arguments={65536+i,65537+i*2}};
    }
}
static bool conditional_compact_same(const XrXirInstruction *a,const XrXirInstruction *b) {
    uint64_t left,right;memcpy(&left,&a->immediate,sizeof(left));memcpy(&right,&b->immediate,sizeof(right));
    return a->op==b->op && a->type==b->type && a->args[0]==b->args[0] && a->args[1]==b->args[1] &&
        a->targets[0]==b->targets[0] && a->targets[1]==b->targets[1] && left==right &&
        a->type_arguments[0]==b->type_arguments[0] && a->type_arguments[1]==b->type_arguments[1];
}

/* A read refusal is injected only into this finite decoder request. Every
 * accepted read still consumes the original receiving ledger; neither the
 * owner nor its accumulated resources are reset for a second decode. */
typedef struct ConditionalCompactRead {
    const XrXirCompileContext *context;
    uint64_t calls,reject;
} ConditionalCompactRead;
static bool conditional_compact_read_work(const void *owner,uint64_t units) {
    ConditionalCompactRead *read=(ConditionalCompactRead *)owner;
    return read->calls++!=read->reject && xir_compile_work(read->context,units);
}
static void conditional_compact_read_failures(const XrXirCompileContext *context,
    const EffectBodyFunction *body,uint32_t instruction,const XrXirInstruction *expected) {
    ConditionalCompactRead observation={context,0,UINT64_MAX};
    EffectBodyRead read={&observation,conditional_compact_read_work};
    XrXirInstruction decoded={0};
    CHECK(effect_body_instruction_get(&read,body,instruction,&decoded)==XR_XIR_OK &&
        conditional_compact_same(&decoded,expected));
    uint64_t points=observation.calls;
    CHECK(points);
    for (uint64_t at=0;at<points;++at) {
        observation.calls=0;observation.reject=at;
        XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,
            .args={11,12},.targets={13,14},.immediate=INT64_MIN,.type_arguments={15,16}};
        decoded=sentinel;
        CHECK(effect_body_instruction_get(&read,body,instruction,&decoded)==XR_XIR_BUDGET &&
            conditional_compact_same(&decoded,&sentinel) && observation.calls==at+1);
    }
    observation.calls=0;observation.reject=UINT64_MAX;
    CHECK(effect_body_instruction_get(&read,body,instruction,&decoded)==XR_XIR_OK &&
        conditional_compact_same(&decoded,expected));
}

/* Narrow output retains every original decoder refusal. Partial words
 * never escape when any canonical read or output charge fails. */
static void conditional_compact_fields_failures(const XrXirCompileContext *context,
    const EffectBodyFunction *body,uint32_t instruction,const XrXirInstruction *expected) {
    ConditionalCompactRead observation={context,0,UINT64_MAX};
    EffectBodyRead read={&observation,conditional_compact_read_work};
    EffectBodyInstructionFields fields;
    CHECK(effect_body_instruction_fields_get(&read,body,instruction,&fields)==XR_XIR_OK &&
        fields.op==expected->op && fields.type==expected->type && fields.source==expected->args[0]);
    uint64_t points=observation.calls;CHECK(points);
    for (uint64_t at=0;at<points;++at) {
        observation.calls=0;observation.reject=at;
        EffectBodyInstructionFields sentinel={XR_XIR_RETURN,XR_XIR_BOOL,UINT32_MAX};fields=sentinel;
        CHECK(effect_body_instruction_fields_get(&read,body,instruction,&fields)==XR_XIR_BUDGET &&
            !memcmp(&fields,&sentinel,sizeof(fields)) && observation.calls==at+1);
    }
    observation.calls=0;observation.reject=UINT64_MAX;
    CHECK(effect_body_instruction_fields_get(&read,body,instruction,&fields)==XR_XIR_OK &&
        fields.op==expected->op && fields.type==expected->type && fields.source==expected->args[0]);
}

/* Each field is changed independently in a fresh source value. No expected
 * value is obtained from the writer's word selector or run decoder. */
static void conditional_compact_change(XrXirInstruction *op,uint32_t field) {
    switch (field) {
    case 0:op->op=XR_XIR_CONST_BOOL;break;
    case 1:op->type=XR_XIR_BOOL;break;
    case 2:op->args[0]^=UINT32_C(0x80000000);break;
    case 3:op->args[1]^=UINT32_C(0x40000000);break;
    case 4:op->targets[0]^=UINT32_C(0x20000000);break;
    case 5:op->targets[1]^=UINT32_C(0x10000000);break;
    case 6:op->immediate^=INT64_C(1);break;
    case 7:op->immediate^=INT64_C(0x4000000000000000);break;
    case 8:op->type_arguments[0]^=UINT32_C(0x08000000);break;
    case 9:op->type_arguments[1]^=UINT32_C(0x04000000);break;
    default:CHECK(false);
    }
}

static void conditional_compact_malformed(const XrXirCompileContext *context,
    EffectBodyFunction *function,const XrXirFunction *expected) {
    EffectBodyRead read=effect_body_compile_read(context);
    CHECK(function->runs && function->run_count==2 && function->run_capacity==2);
    EffectBodyRun *first=&function->runs[0],*second=&function->runs[1];
    uint32_t column=0;while (!(first->mask&(UINT32_C(1)<<column))) ++column;
    uint32_t begin=first->first,count=first->count,mask=first->mask,delta=first->delta[column];
    uint32_t neighbor=second->first,runs=function->run_count,capacity=function->run_capacity;
    for (uint32_t mode=0;mode<8;++mode) {
        if (!mode) first->first=1;
        else if (mode==1) first->count=0;
        else if (mode==2) first->mask|=UINT32_C(1024);
        else if (mode==3) function->run_count=1;
        else if (mode==4) function->run_count=0;
        else if (mode==5) function->run_capacity=function->run_count-1;
        else if (mode==6) first->delta[column]=0;
        else second->first=begin+count+1;
        for (uint32_t instruction=0;instruction<expected->instruction_count;++instruction) {
            XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_I64,.immediate=INT64_MIN},out=sentinel;
            XrXirOp opcode=XR_XIR_RETURN;XrXirType type=XR_XIR_UNIT;
            CHECK(effect_body_instruction_get(&read,function,instruction,&out)==XR_XIR_BAD_STRUCTURE &&
                conditional_compact_same(&out,&sentinel));
            EffectBodyInstructionFields narrow_sentinel={XR_XIR_RETURN,XR_XIR_BOOL,UINT32_MAX},narrow=narrow_sentinel;
            CHECK(effect_body_instruction_fields_get(&read,function,instruction,&narrow)==XR_XIR_BAD_STRUCTURE &&
                !memcmp(&narrow,&narrow_sentinel,sizeof(narrow)));
            CHECK(effect_body_instruction_op(&read,function,instruction,&opcode)==XR_XIR_BAD_STRUCTURE && opcode==XR_XIR_RETURN);
            CHECK(effect_body_operand_type(&read,function,instruction,&type)==XR_XIR_BAD_STRUCTURE && type==XR_XIR_UNIT);
            CHECK(effect_body_records_match(&read,function,expected)==XR_XIR_BAD_STRUCTURE);
        }
        first->first=begin;first->count=count;first->mask=mask;first->delta[column]=delta;
        second->first=neighbor;function->run_count=runs;function->run_capacity=capacity;
        CHECK(effect_body_records_match(&read,function,expected)==XR_XIR_OK);
    }
}

/* Poisoned inactive pending words are not physical fields. Every physical
 * base word, including signed and unused bits, still round-trips independently. */
static void conditional_compact_pending_state(const XrXirCompileContext *context,
    const XrXirInstruction *expected) {
    uint64_t held_bytes=rp_stats(context).live_bytes;
    EffectBodyRun run;memset(&run,0xA5,sizeof(run));
    EffectBodyFunction body={.instruction_count=1};
    CHECK(effect_body_run_seed(context,&run,expected,0)==XR_XIR_OK && run.first==0 && run.count==1 && !run.mask);
    CHECK(effect_body_run_append(context,&body,&run)==XR_XIR_OK && body.run_count==1 && body.run_capacity==1);
    EffectBodyRead read=effect_body_compile_read(context);XrXirInstruction decoded;
    CHECK(effect_body_instruction_get(&read,&body,0,&decoded)==XR_XIR_OK && conditional_compact_same(&decoded,expected));
    uint32_t capacity=body.run_capacity,runs=body.run_count;
    /* The ledger includes its allocation header. Alias refusal preserves the
     * actual charged owner, without treating payload size as total live bytes. */
    uint64_t retained_bytes=rp_stats(context).live_bytes;size_t retained_attempts=rp_attempts;
    CHECK(retained_bytes>held_bytes);
    CHECK(effect_body_run_append(context,&body,&body.runs[0])==XR_XIR_BAD_STRUCTURE &&
        body.run_capacity==capacity && body.run_count==runs &&
        rp_stats(context).live_bytes==retained_bytes && rp_attempts==retained_attempts);
    run.mask=1;
    CHECK(effect_body_run_append(context,&body,&run)==XR_XIR_BAD_STRUCTURE && body.run_capacity==capacity && body.run_count==runs);
    run.count=2;run.delta[0]=0;body.instruction_count=2;
    CHECK(effect_body_run_append(context,&body,&run)==XR_XIR_BAD_STRUCTURE && body.run_capacity==capacity && body.run_count==runs);
    xr_compile_resources_free(body.runs);CHECK(rp_stats(context).live_bytes==held_bytes);
}

#include "xir_invocation_single_column_cases.h"

static XrXirStatus conditional_compact_codec(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    if (!mode) {
        XrXirStatus sparse=conditional_compact_sparse_codec(context,oracle);
        if (sparse!=XR_XIR_OK) return sparse;
    }
    XrXirInstruction original[8],source[8];conditional_compact_rows(original);memcpy(source,original,sizeof(source));
    XrXirFunction expected={.instructions=original,.instruction_count=8};
    XrXirFunction input={.instructions=source,.instruction_count=8};
    EffectBodyFunction body={.instruction_count=8};
    XrXirStatus status=effect_body_records_copy(context,&input,&body);
    memset(source,0xCE,sizeof(source));EffectBodyRead read=effect_body_compile_read(context);
    for (uint32_t i=0;i<8 && status==XR_XIR_OK;++i) {
        XrXirInstruction decoded;status=effect_body_instruction_get(&read,&body,i,&decoded);
        if (status==XR_XIR_OK && oracle) {
            CHECK(conditional_compact_same(&decoded,&original[i]));
            EffectBodyInstructionFields fields;
            CHECK(effect_body_instruction_fields_get(&read,&body,i,&fields)==XR_XIR_OK &&
                fields.op==original[i].op && fields.type==original[i].type && fields.source==original[i].args[0]);
        }
    }
    if (status==XR_XIR_OK) status=effect_body_records_match(&read,&body,&expected);
    if (status==XR_XIR_OK && oracle) {
        CHECK(body.run_count>=2 && body.run_count<=body.run_capacity);
        conditional_compact_read_failures(context,&body,2,&original[2]);
        conditional_compact_read_failures(context,&body,7,&original[7]);
        conditional_compact_fields_failures(context,&body,2,&original[2]);
        conditional_compact_fields_failures(context,&body,7,&original[7]);
        for (uint32_t field=0;field<10;++field) {
            XrXirInstruction saved=original[2];conditional_compact_change(&original[2],field);
            CHECK(effect_body_records_match(&read,&body,&expected)==XR_XIR_BAD_STRUCTURE);
            original[2]=saved;CHECK(effect_body_records_match(&read,&body,&expected)==XR_XIR_OK);
        }
        XrXirInstruction sentinel={.op=XR_XIR_RETURN},out=sentinel;
        CHECK(effect_body_instruction_get(&read,&body,8,&out)==XR_XIR_BAD_STRUCTURE &&
            conditional_compact_same(&out,&sentinel));
        if (mode) conditional_compact_malformed(context,&body,&expected);
        else conditional_compact_pending_state(context,&original[0]);
    }
    xr_compile_resources_free(body.runs);return status;
}

static void conditional_compact_null_body(const XrXirCompileContext *context,XrXirEffects *effects) {
    EffectInvocationCertificate *certificate=effects->invocations;
    EffectBodyMaterial *owned=certificate->body;certificate->body=NULL;
    XirEffectProducerView producer={11,12,13,14,15,XR_XIR_I64},view=producer;
    CHECK(xir_effects_producer(context,effects,0,2,&view)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&producer,&view,sizeof(view)));
    uint32_t mask=UINT32_C(0x6a6a);
    CHECK(effect_invocation_certificate_edge_mask(context,certificate,0,3,1,&mask)==XR_XIR_BAD_STRUCTURE &&
        mask==UINT32_C(0x6a6a));
    InvocationDirectRead read={context,effects,2,false};
    XirEffectInvocationRequest request={.caller=0,.instruction=3,.target=1,.producer=UINT32_MAX,
        .node=UINT32_MAX,.count=2,.read={.owner=&read,.work=invocation_direct_work,
        .cell=invocation_direct_cell,.function=invocation_direct_function}};
    XirEffectInvocationSelection sentinel={effects,11,12,13,14,15,16,17},selection=sentinel;
    CHECK(xir_effects_execution_edge(effects,&request,&selection)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&selection,&sentinel,sizeof(selection)));
    CHECK(effect_invocation_public_gate(context,certificate,effects->count)==XR_XIR_BAD_STRUCTURE);
    XrXirRootCauseTrace *trace=NULL;
    CHECK(effect_invocation_public_trace(context,certificate,effects->count,0,&effects->root[0],&trace)==
        XR_XIR_BAD_STRUCTURE && !trace);
    certificate->body=owned;
    CHECK(effect_body_material_shape(owned) && effect_invocation_public_gate(context,certificate,effects->count)==XR_XIR_OK);
}

/* This genuine Checked producer has already died. Every trace request must
 * still replay the complete selected equation and reject independent damage. */
static void conditional_compact_public_selection(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    CHECK(certificate->final->summary.functions[1].formula.constant_mask==0 &&
        certificate->final->summary.functions[1].formula.term_count==1 &&
        certificate->final->summary.functions[1].formula.terms[0].kind==XR_XIR_ROOT_TERM_CONTEXT_CALL &&
        certificate->final->summary.functions[1].formula.terms[0].index==0);
    for (uint32_t f=0;f<2;++f) for (uint32_t d=0;d<4;++d) {
        EffectInvocationTraceRequest request={.function=f,.domain=d};uint32_t length=77;
        CHECK(effect_invocation_public_walk(context,certificate,&request,4,&length)==XR_XIR_OK &&
            length==((f==1 && d==1)?1u:0u));
    }
    EffectInvocationTraceRequest request={.function=1,.domain=1};
    EffectInvocationTraceSelection selected={UINT32_C(73),
        {XR_XIR_ROOT_CAUSE_MUTABLE_SLOT,11,12,13,14}},sentinel=selected;
    uint32_t length=77;
    CHECK(effect_invocation_trace_walk_selected(context,certificate,&request,&length,&selected)==XR_XIR_OK &&
        length==1 && selected.atom==6 && selected.witness.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL &&
        selected.witness.instruction==0 && selected.witness.distance==0);
    uint32_t at=certificate->final->summary.count+1,atom=selected.atom;
    EffectInvocationNode *entry=&certificate->final->nodes[certificate->final->roots[1]];
    EffectInvocationCause saved=entry->causes[atom];
    XrXirRootEffectWitness witness=certificate->final->summary.witnesses[at];
    bool fact=certificate->final->summary.facts[1].unresolved;
    XrXirRootTerm *term=(XrXirRootTerm *)certificate->final->summary.functions[1].formula.terms;
    uint32_t index=term[0].index;
    for (uint32_t damage=0;damage<4;++damage) {
        if (!damage) entry->causes[atom].witness.cause=XR_XIR_ROOT_CAUSE_NONE;
        else if (damage==1) certificate->final->summary.witnesses[at].distance^=1;
        else if (damage==2) certificate->final->summary.facts[1].unresolved=false;
        else term[0].index=1;
        length=77;
        CHECK(effect_invocation_public_walk(context,certificate,&request,4,&length)==XR_XIR_BAD_STRUCTURE && length==77);
        if (damage<3) {
            selected=sentinel;length=77;
            CHECK(effect_invocation_trace_walk_selected(context,certificate,&request,&length,&selected)==
                XR_XIR_BAD_STRUCTURE && length==77 && selected.atom==sentinel.atom &&
                effect_invocation_witness_equal(&selected.witness,&sentinel.witness));
        }
        entry->causes[atom]=saved;certificate->final->summary.witnesses[at]=witness;
        certificate->final->summary.facts[1].unresolved=fact;term[0].index=index;
        length=77;
        CHECK(effect_invocation_public_walk(context,certificate,&request,4,&length)==XR_XIR_OK && length==1);
    }
    length=77;
    CHECK(effect_invocation_public_walk(context,certificate,&request,0,&length)==XR_XIR_BAD_STRUCTURE && length==77);
}

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
        CHECK(effect_invocation_public_walk(context,certificate,&request,4,&length)==XR_XIR_BAD_STRUCTURE && length==77);
        function->runs[0].first=first;function->runs[0].count=count;function->runs[0].mask=mask;function->runs[0].base.op=(XrXirOp)opcode;
        function->run_count=words;function->run_capacity=capacity;function->run_count=runs;
        CHECK(effect_invocation_certificate_cause(context,certificate,selected,atom)==XR_XIR_OK);
    }
    uint32_t length=77;
    CHECK(effect_invocation_public_walk(context,certificate,&request,4,&length)==XR_XIR_OK && length==1);
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
        CHECK(effect_invocation_public_walk(context,certificate,&request,4,&length)==
            XR_XIR_BAD_STRUCTURE && length==77);
        cause->witness=saved;local->witness=local_saved;
        certificate->body->functions[1].runs[0].first=stream_first;certificate->final->roots[1]=root;
        CHECK(effect_invocation_projection_domains(context,certificate,1,0,4,selected)==XR_XIR_OK &&
            selected[1]==6 && selected[0]==UINT32_MAX && selected[2]==UINT32_MAX &&
            selected[3]==UINT32_MAX);
    }
}

/* The count oracle is literal Checked input, independent of both lifetime
 * callbacks. Partial requests retain every original owned head refusal. */
static void conditional_compact_dimensions(const XrXirCompileContext *context,
    const EffectBodyRequest *request) {
    const uint32_t parameters[4]={0,2,1,0},instructions[4]={5,2,2,3};
    for (uint32_t f=0;f<4;++f) {
        EffectBodyFunctionDimensions dimensions={77,78};
        CHECK(effect_body_request_dimensions(request,f,&dimensions)==XR_XIR_OK &&
            dimensions.parameter_count==parameters[f] && dimensions.instruction_count==instructions[f]);
    }
    EffectBodyFunctionDimensions sentinel={77,78},dimensions=sentinel;
    CHECK(effect_body_request_dimensions(request,4,&dimensions)==XR_XIR_BAD_STRUCTURE &&
        dimensions.parameter_count==77 && dimensions.instruction_count==78);
    EffectBodyRequest rejected=*request;rejected.head=NULL;
    CHECK(effect_body_request_dimensions(&rejected,0,&dimensions)==XR_XIR_BAD_STRUCTURE &&
        dimensions.parameter_count==77 && dimensions.instruction_count==78);
    ConditionalCompactRead refusal={context,0,0};rejected=*request;
    rejected.read=(EffectBodyRead){&refusal,conditional_compact_read_work};
    CHECK(effect_body_request_dimensions(&rejected,0,&dimensions)==XR_XIR_BUDGET &&
        dimensions.parameter_count==77 && dimensions.instruction_count==78 && refusal.calls==1);
}

static void conditional_compact_dimension_damage(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    EffectBodyRequest request=effect_body_owned_request(context,certificate->body);
    EffectBodyFunction *function=&certificate->body->functions[0],saved=*function;
    for (uint32_t damage=0;damage<7;++damage) {
        if (!damage) function->name=NULL;
        else if (damage==1) { function->parameter_count=1;function->parameters=NULL; }
        else if (damage==2) function->blocks=NULL;
        else if (damage==3) function->operands=NULL;
        else if (damage==4) function->runs=NULL;
        else if (damage==5) function->run_capacity=function->run_count-1;
        else function->run_count=0;
        EffectBodyFunctionDimensions dimensions={77,78};EffectBodyFunctionHead head={0};
        CHECK(effect_body_request_dimensions(&request,0,&dimensions)==XR_XIR_BAD_STRUCTURE &&
            dimensions.parameter_count==77 && dimensions.instruction_count==78);
        CHECK(effect_body_request_head(&request,0,&head)==XR_XIR_BAD_STRUCTURE && !head.name && !head.instruction_count);
        *function=saved;conditional_compact_dimensions(context,&request);
    }
}

/* Every genuine call/indirect/reference edge still proves full physical
 * identity. Only its already-decoded opcode is reused inside this request. */
static void conditional_compact_edge_reads(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    EffectInvocationFinal *equations=certificate->final;uint32_t matched=0,chosen=UINT32_MAX;
    for (uint32_t e=0;e<equations->edge_count;++e) {
        const EffectInvocationEdge *edge=&equations->edges[e];
        uint32_t body=equations->nodes[edge->caller].body;XrXirOp expected=XR_XIR_INVALID;
        if (body==0 && edge->instruction==3) expected=XR_XIR_CALL;
        else if (body==0 && edge->instruction==2) expected=XR_XIR_FUNCTION_REF;
        else if (body==1 && edge->instruction==0) expected=XR_XIR_CALL_INDIRECT;
        else continue;
        XrXirOp opcode=XR_XIR_RETURN;
        CHECK(effect_invocation_certificate_edge_selected(context,certificate,edge->caller,e,&opcode)==XR_XIR_OK &&
            opcode==expected);
        CHECK(effect_invocation_certificate_edge(context,certificate,edge->caller,e)==XR_XIR_OK);
        matched|=expected==XR_XIR_CALL?1u:expected==XR_XIR_FUNCTION_REF?2u:4u;
        if (expected==XR_XIR_CALL) chosen=e;
    }
    CHECK(matched==7 && chosen!=UINT32_MAX);
    EffectInvocationEdge *edge=&equations->edges[chosen];uint32_t target=edge->target;
    edge->target=equations->count;XrXirOp opcode=XR_XIR_RETURN;
    CHECK(effect_invocation_certificate_edge_selected(context,certificate,edge->caller,chosen,&opcode)==
        XR_XIR_BAD_STRUCTURE && opcode==XR_XIR_RETURN);
    edge->target=target;
    EffectBodyRequest request=effect_body_owned_request(context,certificate->body);
    ConditionalCompactRead refusal={context,0,UINT64_MAX};
    request.read=(EffectBodyRead){&refusal,conditional_compact_read_work};
    EffectInvocationEdgeRead read={edge->caller,chosen,&opcode};
    CHECK(effect_invocation_edge_read_selected(context,equations,&request,&certificate->terms.types,&read)==XR_XIR_OK &&
        opcode==XR_XIR_CALL && refusal.calls);
    uint64_t calls=refusal.calls;
    for (uint64_t at=0;at<calls;++at) {
        refusal.calls=0;refusal.reject=at;opcode=XR_XIR_RETURN;
        CHECK(effect_invocation_edge_read_selected(context,equations,&request,&certificate->terms.types,&read)==
            XR_XIR_BUDGET && opcode==XR_XIR_RETURN && refusal.calls==at+1);
    }
    refusal.calls=0;refusal.reject=UINT64_MAX;
    CHECK(effect_invocation_edge_read_selected(context,equations,&request,&certificate->terms.types,&read)==XR_XIR_OK &&
        opcode==XR_XIR_CALL);
}

/* Each access delegates to the sole physical reader. Fresh provider facts
 * remain distinct from full body/source correspondence and ROOT closure. */
/* Rejections consume genuine owned post-death heads. Row kind, ordinal,
 * actual type and opcode are all independent checks, with first failure sticky. */
static void conditional_compact_builder_rows(const XrXirCompileContext *context,
    XrXirCellProofBodyRead *request) {
    XrXirCellProofBodyHead head={0};XrXirInstruction actual;
    const XrXirInstruction *operation=NULL;
    CHECK(request->head(context,request->owner,0,&head)==XR_XIR_OK && !head.parameter_count && head.instruction_count);
    CHECK(request->instruction(context,request->owner,0,0,&actual,&operation)==XR_XIR_OK && operation==&actual &&
        operation->op==XR_XIR_CONST_INT && operation->type==XR_XIR_I64);
    for (uint32_t mode=0;mode<4;++mode) {
        uint64_t held_bytes=rp_stats(context).live_bytes;
        XrXirCellProofBuilder *builder=NULL;XrXirCellProvenance *output=NULL;
        CHECK(xir_cell_proof_builder_begin(context,request,&builder)==XR_XIR_OK && builder);
        CHECK(xir_cell_proof_builder_head(builder,0,&head)==XR_XIR_OK);
        XrXirStatus expected=mode==0 || mode==3?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE;
        XrXirStatus status;
        if (!mode) status=xir_cell_proof_builder_instruction(builder,0,1,operation->type,operation->op,operation->type);
        else if (mode==1) status=xir_cell_proof_builder_parameter(builder,0,0,operation->type);
        else status=xir_cell_proof_builder_instruction(builder,0,0,operation->type,
            mode==3?XR_XIR_INVALID:operation->op,mode==2?XR_XIR_F64:operation->type);
        CHECK(status==expected);
        XrCompileResourceStats before=rp_stats(context);
        CHECK(xir_cell_proof_builder_instruction(builder,0,0,operation->type,operation->op,operation->type)==expected);
        XrCompileResourceStats after=rp_stats(context);CHECK(!memcmp(&before,&after,sizeof(before)));
        CHECK(xir_cell_proof_builder_finish(&builder,&output)==expected && !builder && !output);
        CHECK(rp_stats(context).live_bytes==held_bytes);
    }
}

/* The genuine producer dies before every span attack. Ordinary head,
 * scalar, role and edge replay remains complete for all four functions. */
static void conditional_compact_builder_spans(const XrXirCompileContext *context,
    XrXirCellProofBodyRead *request) {
    CHECK(request->span);
    XrXirCellProofSpanCursor sentinel={.function=3,.shape={XR_XIR_RETURN,XR_XIR_F64}},cursor=sentinel;
    CHECK(request->span(context,request->owner,4,0,2,&cursor)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&sentinel,sizeof(cursor)));
    CHECK(request->span(context,request->owner,3,1,2,&cursor)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&sentinel,sizeof(cursor)));
    cursor.function=sentinel.function=0;
    CHECK(request->span(context,request->owner,0,0,2,&cursor)==XR_XIR_BAD_TYPE &&
        !memcmp(&cursor,&sentinel,sizeof(cursor)));
    /* A zero supplied head has no scalar row. It must still be rejected
     * against the genuine nonempty owned head before any provider escapes. */
    uint64_t empty_held=rp_stats(context).live_bytes;
    XrXirCellProofBodyHead empty={0};
    CHECK(request->head(context,request->owner,0,&empty)==XR_XIR_OK);
    empty.instruction_count=0;
    XrXirCellProofBuilder *empty_builder=NULL;XrXirCellProvenance *empty_output=NULL;
    CHECK(xir_cell_proof_builder_begin(context,request,&empty_builder)==XR_XIR_OK);
    CHECK(xir_cell_proof_builder_head(empty_builder,0,&empty)==XR_XIR_OK);
    CHECK(xir_cell_proof_builder_function(empty_builder,0)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats empty_retry=rp_stats(context);
    CHECK(xir_cell_proof_builder_function(empty_builder,0)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats empty_after=rp_stats(context);
    CHECK(!memcmp(&empty_retry,&empty_after,sizeof(empty_retry)));
    CHECK(xir_cell_proof_builder_finish(&empty_builder,&empty_output)==XR_XIR_BAD_STRUCTURE &&
        !empty_builder && !empty_output && rp_stats(context).live_bytes==empty_held);
    for (uint32_t mode=0;mode<9;++mode) {
        uint64_t held_bytes=rp_stats(context).live_bytes;
        XrXirCellProofBuilder *builder=NULL;XrXirCellProvenance *output=NULL;
        CHECK(xir_cell_proof_builder_begin(context,request,&builder)==XR_XIR_OK && builder);
        for (uint32_t f=0;f<3;++f) {
            XrXirCellProofBodyHead head={0};
            CHECK(request->head(context,request->owner,f,&head)==XR_XIR_OK);
            CHECK(xir_cell_proof_builder_head(builder,f,&head)==XR_XIR_OK);
            for (uint32_t v=0;v<head.parameter_count;++v)
                CHECK(xir_cell_proof_builder_parameter(builder,f,v,head.parameters[v])==XR_XIR_OK);
            for (uint32_t i=0;i<head.instruction_count;++i) {
                XrXirInstruction actual;const XrXirInstruction *operation=NULL;
                CHECK(request->instruction(context,request->owner,f,i,&actual,&operation)==XR_XIR_OK &&
                    operation==&actual);
                CHECK(xir_cell_proof_builder_instruction(builder,f,head.parameter_count+i,
                    operation->type,operation->op,operation->type)==XR_XIR_OK);
            }
            XrCompileResourceStats scalar_end=rp_stats(context);
            CHECK(xir_cell_proof_builder_function(builder,f)==XR_XIR_OK);
            CHECK(rp_stats(context).work-scalar_end.work==
                2+8+sizeof(uint64_t)+sizeof(uint32_t)+sizeof(bool));
        }
        XrXirCellProofBodyHead head={0};
        CHECK(request->head(context,request->owner,3,&head)==XR_XIR_OK && !head.parameter_count && head.instruction_count==3);
        CHECK(xir_cell_proof_builder_head(builder,3,&head)==XR_XIR_OK);
        XrXirCellProofInstructionShape shape={XR_XIR_CONST_INT,XR_XIR_I64};
        XrXirStatus status;
        if (mode<6) {
            if (mode==4) shape.type=XR_XIR_F64;
            if (mode==5) shape.op=XR_XIR_INVALID;
            status=xir_cell_proof_builder_span(builder,mode==0?2:3,mode==1?1:0,
                mode==2?0:mode==3?4:2,&shape);
            XrXirStatus expected=mode==4?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE;
            CHECK(status==expected);
            XrCompileResourceStats before_retry=rp_stats(context);
            shape=(XrXirCellProofInstructionShape){XR_XIR_CONST_INT,XR_XIR_I64};
            CHECK(xir_cell_proof_builder_span(builder,3,0,2,&shape)==expected);
            XrCompileResourceStats after_retry=rp_stats(context);CHECK(!memcmp(&before_retry,&after_retry,sizeof(before_retry)));
            CHECK(xir_cell_proof_builder_finish(&builder,&output)==expected && !builder && !output);
        } else {
            CHECK(xir_cell_proof_builder_span(builder,3,0,2,&shape)==XR_XIR_OK);
            XrXirInstruction actual;const XrXirInstruction *operation=NULL;
            CHECK(request->instruction(context,request->owner,3,2,&actual,&operation)==XR_XIR_OK &&
                operation==&actual && operation->op==XR_XIR_RETURN);
            CHECK(xir_cell_proof_builder_instruction(builder,3,2,
                operation->type,operation->op,operation->type)==XR_XIR_OK);
            const EffectCellBodyAdapter *adapter=request->owner;
            EffectBodyMaterial *material=(EffectBodyMaterial *)adapter->request.owner;
            EffectBodyFunction *function=&material->functions[3];
            uint32_t capacity=function->run_capacity,runs=function->run_count;
            if (mode==6) function->run_capacity=function->run_count-1;
            if (mode==7) --function->run_count;
            status=xir_cell_proof_builder_function(builder,3);
            function->run_capacity=capacity;function->run_count=runs;
            if (mode<8) CHECK(status==XR_XIR_BAD_STRUCTURE);
            else CHECK(status==XR_XIR_OK);
            CHECK(xir_cell_proof_builder_finish(&builder,&output)==(mode<8?XR_XIR_BAD_STRUCTURE:XR_XIR_OK) && !builder);
            if (mode<8) CHECK(!output);
            else CHECK(output && xr_xir_compile_cell_provenance_body_matches(context,output,request,NULL)==XR_XIR_OK);
            xr_xir_compile_cell_provenance_free(output);
        }
        CHECK(rp_stats(context).live_bytes==held_bytes);
    }
}

static void conditional_compact_cell_attacks(const XrXirCompileContext *context,
    XrXirEffects *effects,EffectCellBodyAdapter *adapter,XrXirCellProofBodyRead *request) {
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_I64,.immediate=INT64_MIN},out=sentinel;
    const XrXirInstruction *operation=&sentinel;
    CHECK(request->instruction(context,request->owner,0,5,&out,&operation)==XR_XIR_BAD_STRUCTURE &&
        operation==&sentinel && conditional_compact_same(&out,&sentinel));
    CHECK(request->instruction(context,request->owner,0,0,NULL,&operation)==XR_XIR_BAD_STRUCTURE &&
        operation==&sentinel && conditional_compact_same(&out,&sentinel));
    CHECK(request->instruction(context,request->owner,0,0,&out,NULL)==XR_XIR_BAD_STRUCTURE &&
        operation==&sentinel && conditional_compact_same(&out,&sentinel));
    XrXirCellProofBodyHead head={.result=XR_XIR_I64,.instruction_count=999},head_sentinel=head;
    CHECK(request->head(context,request->owner,4,&head)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&head,&head_sentinel,sizeof(head)));
    XrXirType type=XR_XIR_F64;
    CHECK(request->type(context,request->owner,0,5,&type)==XR_XIR_BAD_STRUCTURE && type==XR_XIR_F64);
    conditional_compact_builder_rows(context,request);
    conditional_compact_builder_spans(context,request);
    XrXirCellOriginView view={0};
    CHECK(xr_xir_compile_cell_origin_view(context,effects->cells,2,0,&view)==XR_XIR_OK &&
        !view.intrinsic_mask && view.parameter_count==1 && view.parameters[0]==0 &&
        view.word_count==1 && view.dependencies[0]==1 &&
        xr_xir_cell_provenance_role(effects->cells,2,0)==XR_XIR_CELL_PROOF_SCOPED_REF);
    uint64_t *dependencies=(uint64_t *)view.dependencies;dependencies[0]=0;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,effects->cells,request,NULL)==XR_XIR_BAD_STRUCTURE);
    dependencies[0]=1;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,effects->cells,request,NULL)==XR_XIR_OK);
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats before=rp_stats(&foreign);
    CHECK(request->instruction(&foreign,request->owner,0,0,&out,&operation)==XR_XIR_BAD_STRUCTURE &&
        operation==&sentinel && conditional_compact_same(&out,&sentinel));
    CHECK(xr_xir_compile_cell_provenance_body_matches(&foreign,effects->cells,request,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirCellProofSpanCursor cursor={.function=3},cursor_sentinel=cursor;
    CHECK(request->span(&foreign,request->owner,3,0,2,&cursor)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&cursor_sentinel,sizeof(cursor)));
    XrCompileResourceStats after=rp_stats(&foreign);CHECK(!memcmp(&before,&after,sizeof(before)));
    EffectCellBodyAdapter destination={0};XrXirCellProofBodyRead occupied=*request;
    CHECK(effect_cell_body_owned(&foreign,effects->invocations->body,&destination,&occupied)==XR_XIR_BAD_STRUCTURE &&
        !destination.work && !destination.request.owner && !memcmp(&occupied,request,sizeof(occupied)));
    rp_owner_free(&foreign,before.live_bytes);
    EffectBodyRequest mixed=adapter->request;mixed.instruction=effect_body_raw_instruction;
    CHECK(effect_cell_body_request(context,&mixed,&destination,&occupied)==XR_XIR_BAD_STRUCTURE &&
        !destination.work && !destination.request.owner && !memcmp(&occupied,request,sizeof(occupied)));
    mixed=adapter->request;++mixed.domain.function_count;
    CHECK(effect_cell_body_request(context,&mixed,&destination,&occupied)==XR_XIR_BAD_STRUCTURE &&
        !destination.work && !destination.request.owner && !memcmp(&occupied,request,sizeof(occupied)));
    const XrXirCompileContext *work=adapter->work;adapter->work=NULL;
    CHECK(request->instruction(context,request->owner,0,0,&out,&operation)==XR_XIR_BAD_STRUCTURE &&
        operation==&sentinel && conditional_compact_same(&out,&sentinel));adapter->work=work;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,effects->cells,request,NULL)==XR_XIR_OK);
}

/* The public effects constructor receives genuine owned Checked input. Both
 * raw and Checked producers die before replay, lookups and adversarial checks. */
static void conditional_compact_refinement_span(const XrXirCompileContext *context,
    const XrXirModule *source);

static XrXirStatus conditional_compact_owned(const XrXirCompileContext *context,bool oracle) {
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    fixture.init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1};
    fixture.init[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2};
    fixture.init[2]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture.init_block.count=fixture.functions[3].instruction_count=3;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead body_read={0};
    if (status==XR_XIR_OK) status=effect_cell_body_raw(context,xr_xir_compile_artifact_module(checked),&adapter,&body_read);
    if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_body_matches(context,effects->cells,&body_read,NULL);
    if (status==XR_XIR_OK && oracle) {
        CHECK(effects->invocations && effect_body_material_shape(effects->invocations->body));
        const XrXirModule *original=xr_xir_compile_artifact_module(checked);
        CHECK(original->function_count==4 && original->functions[0].instruction_count==5);
        CHECK(effect_body_source_match(context,effects->invocations->body,original)==XR_XIR_OK);
        EffectBodyRequest raw=effect_body_raw_request(context,original);
        XrXirInstruction raw_storage;const XrXirInstruction *raw_operation=NULL;
        EffectBodyInstructionFields raw_sentinel={XR_XIR_RETURN,XR_XIR_BOOL,UINT32_MAX},raw_fields=raw_sentinel;
        XrCompileResourceStats raw_before=rp_stats(context);
        CHECK(effect_body_request_fields(&raw,0,0,&raw_storage,&raw_operation,&raw_fields)==XR_XIR_OK &&
            raw_operation==&original->functions[0].instructions[0] && !memcmp(&raw_fields,&raw_sentinel,sizeof(raw_fields)));
        CHECK(rp_stats(context).work-raw_before.work==2+sizeof(raw_operation) &&
            rp_stats(context).allocation_count==raw_before.allocation_count && rp_stats(context).live_bytes==raw_before.live_bytes);
        conditional_compact_dimensions(context,&raw);CHECK(!body_read.span);
        conditional_compact_refinement_span(context,original);
        XrXirModule changed=*original;XrXirFunction functions[4];XrXirInstruction entry[5];
        memcpy(functions,original->functions,sizeof(functions));memcpy(entry,functions[0].instructions,sizeof(entry));
        functions[0].instructions=entry;changed.functions=functions;entry[0].immediate=99;
        CHECK(effect_body_source_match(context,effects->invocations->body,&changed)==XR_XIR_BAD_STRUCTURE);
        functions[0]=original->functions[0];XrXirInstruction initializer[3];
        memcpy(initializer,functions[3].instructions,sizeof(initializer));functions[3].instructions=initializer;
        initializer[1].targets[1]=UINT32_MAX;
        CHECK(effect_body_source_match(context,effects->invocations->body,&changed)==XR_XIR_BAD_STRUCTURE);
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    adapter=(EffectCellBodyAdapter){0};body_read=(XrXirCellProofBodyRead){0};
    if (status==XR_XIR_OK) status=effect_cell_body_owned(context,effects->invocations->body,&adapter,&body_read);
    if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_body_matches(context,effects->cells,&body_read,NULL);
    if (status==XR_XIR_OK && oracle) conditional_compact_cell_attacks(context,effects,&adapter,&body_read);
    if (status==XR_XIR_OK && oracle) {
        EffectInvocationCertificate *certificate=effects->invocations;
        CHECK(certificate->final);
        EffectBodyRequest request=effect_body_owned_request(context,certificate->body);
        EffectBodyFunctionHead head={0};XrXirInstruction storage;const XrXirInstruction *op=NULL;
        CHECK(effect_body_request_head(&request,0,&head)==XR_XIR_OK && head.parameter_count==0 &&
            head.instruction_count==5 && head.result==XR_XIR_I64 && head.name_length==5 && !memcmp(head.name,"entry",5));
        CHECK(effect_body_request_instruction(&request,0,0,&storage,&op)==XR_XIR_OK && op==&storage &&
            op->op==XR_XIR_CONST_INT && op->type==XR_XIR_I64 && op->immediate==42 && !op->args[0] && !op->args[1] &&
            !op->targets[0] && !op->targets[1] && !op->type_arguments[0] && !op->type_arguments[1]);
        EffectBodyInstructionFields fields_sentinel={XR_XIR_RETURN,XR_XIR_BOOL,UINT32_MAX},fields=fields_sentinel;
        const XrXirInstruction *narrow_operation=NULL;
        XrXirInstruction untouched={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL},narrow_storage=untouched;
        XrCompileResourceStats narrow_before=rp_stats(context);
        CHECK(effect_body_request_fields(&request,0,0,&narrow_storage,&narrow_operation,&fields)==XR_XIR_OK &&
            !narrow_operation && fields.op==XR_XIR_CONST_INT && fields.type==XR_XIR_I64 && !fields.source &&
            conditional_compact_same(&narrow_storage,&untouched));
        CHECK(rp_stats(context).allocation_count==narrow_before.allocation_count &&
            rp_stats(context).live_bytes==narrow_before.live_bytes);
        fields=fields_sentinel;
        CHECK(effect_body_request_fields(&request,0,head.instruction_count,&narrow_storage,&narrow_operation,&fields)==
            XR_XIR_BAD_STRUCTURE && !narrow_operation && !memcmp(&fields,&fields_sentinel,sizeof(fields)) &&
            conditional_compact_same(&narrow_storage,&untouched));
        const XrXirType *parameters=certificate->body->functions[0].parameters;
        uint32_t count=certificate->body->functions[0].parameter_count;
        certificate->body->functions[0].parameters=NULL;certificate->body->functions[0].parameter_count=1;
        EffectBodyFunctionHead sentinel_head=head;
        CHECK(effect_body_request_head(&request,0,&head)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&head,&sentinel_head,sizeof(head)));
        certificate->body->functions[0].parameters=parameters;
        certificate->body->functions[0].parameter_count=count;
        CHECK(effect_body_request_head(&request,0,&head)==XR_XIR_OK);
        XirEffectProducerView view={0};CHECK(xir_effects_producer(context,effects,0,2,&view)==XR_XIR_OK &&
            view.function==0 && view.instruction==2 && view.target==2 && !view.capture_count && view.type==(XrXirType)257);
        conditional_compact_null_body(context,effects);
        conditional_compact_public_selection(context,certificate);
        conditional_compact_cause_opcode(context,certificate);
        conditional_compact_projection_domains(context,certificate);
        conditional_compact_dimensions(context,&request);
        conditional_compact_dimension_damage(context,certificate);
        conditional_compact_edge_reads(context,certificate);
    }
    xr_xir_compile_effects_free(effects);return status;
}

/* Two genuine declaration paths use the same old per-value bounds as their
 * independent oracle. This helper changes no allocation or failure engine. */
static XrXirStatus conditional_compact_advertisements(const XrXirCompileContext *context,
    const XrXirModule *source,EffectRefinementBounds *refinement,bool spans,
    EffectInvocationDeclaredBounds **output) {
    EffectBodyAdvertisementBuilder *builder=NULL;
    XrXirStatus status=effect_body_advertisements_begin(context,source,refinement,&builder);
    for (uint32_t f=0;status==XR_XIR_OK && f<source->function_count;++f) {
        const XrXirFunction *function=&source->functions[f];
        status=effect_body_advertisements_function(builder,f);
        for (uint32_t v=0;status==XR_XIR_OK && v<function->parameter_count;++v)
            status=effect_body_advertisements_value(builder,f,v,function->parameters[v]);
        for (uint32_t i=0;status==XR_XIR_OK && i<function->instruction_count;++i) {
            const XrXirInstruction *operation=&function->instructions[i];
            if (spans && i==0 && function->instruction_count>=2 &&
                operation->op==XR_XIR_CONST_INT && operation->type==XR_XIR_I64 &&
                function->instructions[1].op==XR_XIR_CONST_INT && function->instructions[1].type==XR_XIR_I64) {
                status=effect_body_advertisements_span(builder,f,function->parameter_count,2,XR_XIR_I64);++i;
            } else status=effect_body_advertisements_value(builder,f,function->parameter_count+i,operation->type);
        }
    }
    if (status==XR_XIR_OK) status=effect_body_advertisements_finish(&builder,output);
    effect_body_advertisements_free(builder);return status;
}
static void conditional_compact_advertisement_pair(const XrXirCompileContext *context,
    const XrXirModule *source,EffectRefinementBounds *refinement) {
    uint64_t held_bytes=rp_stats(context).live_bytes;
    EffectInvocationDeclaredBounds *scalar=NULL,*span=NULL;
    CHECK(conditional_compact_advertisements(context,source,refinement,false,&scalar)==XR_XIR_OK && scalar);
    CHECK(conditional_compact_advertisements(context,source,refinement,true,&span)==XR_XIR_OK && span);
    CHECK(scalar->count==source->function_count && span->count==scalar->count);
    for (uint32_t f=0;f<scalar->count;++f) {
        const EffectInvocationFunctionBounds *a=&scalar->functions[f],*b=&span->functions[f];
        CHECK(a->values==b->values && a->count==b->count);
        for (uint32_t v=0;v<a->count;++v) CHECK(a->bounds[v].value==b->bounds[v].value &&
            a->bounds[v].mask==b->bounds[v].mask && a->bounds[v].callable==b->bounds[v].callable);
    }
    effect_invocation_bounds_free(scalar);effect_invocation_bounds_free(span);
    CHECK(rp_stats(context).live_bytes==held_bytes);
}
static void conditional_compact_refinement_span(const XrXirCompileContext *context,
    const XrXirModule *source) {
    uint64_t held_bytes=rp_stats(context).live_bytes;
    XrXirEffects captured={.resources=context->resources,.count=source->function_count};
    CHECK(xir_effects_refinement_capture(context,source,&captured)==XR_XIR_OK && captured.refinement);
    conditional_compact_advertisement_pair(context,source,captured.refinement);
    EffectBodyMaterial *material=NULL;
    CHECK(effect_body_material_generation(context,source,captured.refinement,&material)==XR_XIR_OK && material);
    CHECK(effect_body_source_match(context,material,source)==XR_XIR_OK);
    effect_body_material_free(material);
    XrXirType *declared=(XrXirType *)captured.refinement->functions[3].instructions;
    CHECK(source->functions[3].instruction_count==3 && declared[0]==XR_XIR_I64 && declared[1]==XR_XIR_I64);
    XrXirType saved=declared[1];declared[1]=(XrXirType)257;
    CHECK(xr_xir_callable_signature(source->types,declared[1]));
    uint64_t retained=rp_stats(context).live_bytes;
    for (uint32_t spans=0;spans<2;++spans) {
        EffectInvocationDeclaredBounds *output=NULL;
        CHECK(conditional_compact_advertisements(context,source,captured.refinement,spans!=0,&output)==
            XR_XIR_BAD_TYPE && !output && rp_stats(context).live_bytes==retained);
    }
    declared[1]=saved;conditional_compact_advertisement_pair(context,source,captured.refinement);
    effect_refinement_free(captured.refinement);CHECK(rp_stats(context).live_bytes==held_bytes);
}
/* The open forward definition has a real interface constraint, and specialize
 * supplies real INSTANCE origins/arguments. No phantom generic or fake evidence
 * selects this path. The two constants precede its original requirement call. */
static void conditional_compact_instance_span(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t held_bytes=rp_stats(&context).live_bytes;
    ConditionalRawFixture fixture;conditional_raw_fixture(&fixture,2);
    XrXirInstruction forward[4]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},
        fixture.forward_ops[0],{.op=XR_XIR_RETURN,.args={4,0}}};
    fixture.functions[1].instructions=forward;fixture.functions[1].instruction_count=4;
    /* The raw internal fixture has no CFG and its caller takes arguments.
     * A complete Artifact needs genuine blocks and a separate I64 entry. */
    XrXirInstruction entry[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirFunction functions[5]={0};XrXirGeneric generics[5]={0};
    XrXirFunctionIdentity identities[5]={0};XrXirBlock blocks[5]={0};
    memcpy(functions,fixture.functions,sizeof(fixture.functions));
    memcpy(generics,fixture.generics,sizeof(fixture.generics));
    memcpy(identities,fixture.identities,sizeof(fixture.identities));
    functions[4]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,
        .instructions=entry,.instruction_count=2};
    for (uint32_t f=0;f<5;++f) {
        blocks[f]=(XrXirBlock){.first=0,.count=functions[f].instruction_count};
        functions[f].blocks=&blocks[f];functions[f].block_count=1;
    }
    fixture.declarations.functions=identities;fixture.declarations.entry_function=4;
    fixture.module.functions=functions;fixture.module.function_count=5;fixture.module.generics=generics;
    CHECK(generics[1].parameter_count==1 && generics[1].constraints==&fixture.bound);
    XrXirArtifact *checked=NULL,*instance=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK && checked);
    const XrXirModule *source=xr_xir_compile_artifact_module(checked);
    CHECK(source->generics && source->generics[1].parameter_count==1);
    conditional_compact_advertisement_pair(&context,source,NULL);
    EffectBodyMaterial *generic_material=NULL;
    CHECK(effect_body_material_create(&context,source,&generic_material)==XR_XIR_OK && generic_material);
    CHECK(effect_body_source_match(&context,generic_material,source)==XR_XIR_OK);
    effect_body_material_free(generic_material);
    CHECK(xr_xir_compile_specialize(checked,&instance,NULL)==XR_XIR_OK && instance);
    const XrXirModule *module=xr_xir_compile_artifact_module(instance);
    CHECK(module->provenance && module->provenance->kind==XR_XIR_EVIDENCE_INSTANCE && module->provenance->source);
    uint32_t selected=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) if (module->provenance->origins[f].function==1) selected=f;
    CHECK(selected!=UINT32_MAX && module->functions[selected].instruction_count==4 &&
        module->provenance->origins[selected].argument_count==1 &&
        module->provenance->origins[selected].arguments);
    conditional_compact_advertisement_pair(&context,module,NULL);
    EffectBodyMaterial *material=NULL;
    CHECK(effect_body_material_create(&context,module,&material)==XR_XIR_OK && material);
    CHECK(effect_body_source_match(&context,material,module)==XR_XIR_OK);
    effect_body_material_free(material);
    const XrXirModule *original=&module->provenance->source->module;
    XrXirInstruction *instructions=(XrXirInstruction *)original->functions[1].instructions;
    XrXirType saved=instructions[1].type;CHECK(saved==XR_XIR_I64);
    instructions[1].type=(XrXirType)257;CHECK(xr_xir_callable_signature(original->types,instructions[1].type));
    uint64_t retained=rp_stats(&context).live_bytes;
    for (uint32_t spans=0;spans<2;++spans) {
        EffectInvocationDeclaredBounds *output=NULL;
        CHECK(conditional_compact_advertisements(&context,module,NULL,spans!=0,&output)==XR_XIR_BAD_TYPE &&
            !output && rp_stats(&context).live_bytes==retained);
    }
    instructions[1].type=saved;conditional_compact_advertisement_pair(&context,module,NULL);
    xr_xir_compile_artifact_free(instance);xr_xir_compile_artifact_free(checked);
    rp_owner_free(&context,held_bytes);rp_balanced(physical);
}

static XrXirStatus conditional_compact_case(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    return mode<2?conditional_compact_codec(context,mode,oracle):conditional_compact_owned(context,oracle);
}
#include "xir_invocation_compact_directory_cases.h"

static void conditional_compact_literals(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_compact_case(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    conditional_compact_instance_span();conditional_compact_directory_literals();
}
static void conditional_compact_resources(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        size_t sites=0;XrCompileResourceStats single={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats entry=rp_stats(&context);
            uint64_t baseline=entry.live_bytes;RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=conditional_compact_case(&context,mode,false);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context);
                printf("compact material mode%u census sites%zu work%llu allocated%llu live%llu peak%llu\n",
                    mode,sites,(unsigned long long)single.work,(unsigned long long)single.allocated_bytes,
                    (unsigned long long)single.live_bytes,(unsigned long long)single.peak_bytes);
            }
            else {
                CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
                ConditionalRetryTrial retry={.which=mode+87,.ordinal=pass-1,.sites=sites,
                    .resources=identity,.initial=entry,.single=single,.retained=retained,.first=status};
                conditional_retry_operation(&context,&retry);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,baseline);rp_balanced(physical);
        }
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        XrCompileResourceStats entry=rp_stats(&context);uint64_t baseline=entry.live_bytes;
        size_t before=rp_attempts;CHECK(conditional_compact_case(&context,mode,false)==XR_XIR_OK);
        XrCompileResourceStats census=rp_stats(&context);size_t census_sites=rp_attempts-before;
        rp_owner_free(&context,baseline);rp_balanced(physical);
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t pass=0;pass<2;++pass) {
            XrCompileResourceLimits limits=rp_caps();uint64_t value=measured[axis]-(pass?0:1);
            if (!axis) limits.allocated_bytes=value;else if (axis==1) limits.live_bytes=value;else limits.work=value;
            physical=rp_mark();context=rp_owner(limits);entry=rp_stats(&context);baseline=entry.live_bytes;
            RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            CHECK(conditional_compact_case(&context,mode,false)==(pass?XR_XIR_OK:XR_XIR_BUDGET));
            ConditionalRetryTrial retry={.which=mode+87,.ordinal=SIZE_MAX,.sites=census_sites,
                .resources=identity,.initial=entry,.single=census,.retained=retained,.first=pass?XR_XIR_OK:XR_XIR_BUDGET};
            conditional_retry_operation(&context,&retry);rp_owner_free(&context,baseline);rp_balanced(physical);
        }
    }
}
#endif // XIR_INVOCATION_COMPACT_SNAPSHOT_CASES_H
