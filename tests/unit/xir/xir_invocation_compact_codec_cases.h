/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_compact_codec_cases.h - Independent lossless body codec controls
 */
#ifndef XIR_INVOCATION_COMPACT_CODEC_CASES_H
#define XIR_INVOCATION_COMPACT_CODEC_CASES_H

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

#endif // XIR_INVOCATION_COMPACT_CODEC_CASES_H
