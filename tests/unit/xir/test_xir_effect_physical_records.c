/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effect_physical_records.c - Independent complete physical record probes
 *
 * KEY CONCEPT:
 *   Raw storage preserves every word after producer death. It grants no IR
 *   admission, type fact, Cell permission or executable authority.
 */
#include "xir/xxir_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir/xxir_effects.c"

enum { PHYSICAL_MASKS=1024, PHYSICAL_ROWS=3*PHYSICAL_MASKS+2 };

/* This independent fixture writes physical fields directly. It does not use
 * the owned decoder to manufacture an expected instruction. */
static XrXirInstruction physical_literal(uint32_t mask,uint32_t ordinal) {
    uint32_t words[10]={XR_XIR_COPY,XR_XIR_I64,17*mask+13,UINT32_MAX-19,
        UINT32_MAX-23,29,UINT32_MAX-31,UINT32_C(0x80000000),37,UINT32_MAX-41};
    for (uint32_t w=0;w<10;++w) if (mask&(UINT32_C(1)<<w))
        words[w]+=ordinal*(w==0?1:2*w+1);
    uint64_t bits=words[6]|((uint64_t)words[7]<<32);int64_t immediate;
    memcpy(&immediate,&bits,sizeof(bits));
    return (XrXirInstruction){.op=(XrXirOp)words[0],.type=(XrXirType)words[1],
        .args={words[2],words[3]},.targets={words[4],words[5]},.immediate=immediate,
        .type_arguments={words[8],words[9]}};
}

static bool physical_equal(const XrXirInstruction *a,const XrXirInstruction *b) {
    return a->op==b->op && a->type==b->type && a->args[0]==b->args[0] && a->args[1]==b->args[1] &&
        a->targets[0]==b->targets[0] && a->targets[1]==b->targets[1] && a->immediate==b->immediate &&
        a->type_arguments[0]==b->type_arguments[0] && a->type_arguments[1]==b->type_arguments[1];
}

static XrXirInstruction *physical_input(const XrXirCompileContext *context) {
    XrXirStatus status=XR_XIR_OK;
    XrXirInstruction *rows=xir_compile_alloc(context,PHYSICAL_ROWS*sizeof(*rows),&status);
    CHECK(rows && status==XR_XIR_OK && xir_compile_work(context,PHYSICAL_ROWS*sizeof(*rows)));
    for (uint32_t mask=0;mask<PHYSICAL_MASKS;++mask)
        for (uint32_t i=0;i<3;++i) rows[3*mask+i]=physical_literal(mask,i);
    rows[PHYSICAL_ROWS-2]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64,.immediate=INT64_MIN};
    rows[PHYSICAL_ROWS-1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    return rows;
}

static void physical_roundtrip(const XrXirCompileContext *context,const EffectBodyFunction *body,
    const XrXirFunction *expected) {
    EffectBodyRead read=effect_body_compile_read(context);
    CHECK(effect_body_records_match(&read,body,expected)==XR_XIR_OK);
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    for (uint32_t i=0;i<PHYSICAL_ROWS;++i) {
        XrXirInstruction random={0},sequential={0};
        CHECK(effect_body_instruction_get(&read,body,i,&random)==XR_XIR_OK &&
            physical_equal(&random,&expected->instructions[i]));
        CHECK(effect_body_instruction_cursor_get(&read,body,i,&cursor,&sequential)==XR_XIR_OK &&
            physical_equal(&sequential,&expected->instructions[i]));
    }
    CHECK(body->run_count==PHYSICAL_MASKS+1);
    for (uint32_t mask=0;mask<PHYSICAL_MASKS;++mask)
        CHECK(body->runs[mask].first==3*mask && body->runs[mask].count==3 && body->runs[mask].mask==mask);
    const EffectBodyRun *pair=&body->runs[PHYSICAL_MASKS];
    CHECK(pair->count==2 && pair->mask==EFFECT_BODY_MASK && pair->delta[2]==0 && pair->delta[7]==0);
}

static XrXirStatus physical_construct(const XrXirCompileContext *context,const XrXirFunction *source,
    const XrXirFunction *expected) {
    EffectBodyFunction body={.instruction_count=source->instruction_count};
    XrXirStatus status=effect_body_records_copy(context,source,&body);
    if (status==XR_XIR_OK) physical_roundtrip(context,&body,expected);
    xr_compile_resources_free(body.runs);return status;
}

static void physical_faults(const XrXirCompileContext *context,const XrXirFunction *source,
    const XrXirFunction *expected) {
    size_t before=effects_compile_attempts;
    CHECK(physical_construct(context,source,expected)==XR_XIR_OK);
    size_t sites=effects_compile_attempts-before;CHECK(sites>1);
    size_t live=effects_compile_live,bytes=effects_compile_bytes;
    for (size_t at=0;at<sites;++at) {
        XrCompileResourceStats entry={0},failed={0},retried={0};
        CHECK(xr_compile_resources_stats(context->resources,&entry)==XR_COMPILE_RESOURCE_OK);
        effects_compile_injected=false;effects_compile_fail_at=effects_compile_attempts+at;
        XrXirStatus status=physical_construct(context,source,expected);
        effects_compile_fail_at=SIZE_MAX;
        CHECK(effects_compile_injected && status==XR_XIR_OUT_OF_MEMORY &&
            effects_compile_live==live && effects_compile_bytes==bytes);
        CHECK(xr_compile_resources_stats(context->resources,&failed)==XR_COMPILE_RESOURCE_OK &&
            failed.live_bytes==entry.live_bytes && failed.work>entry.work &&
            failed.allocated_bytes>=entry.allocated_bytes);
        CHECK(physical_construct(context,source,expected)==XR_XIR_OK);
        CHECK(xr_compile_resources_stats(context->resources,&retried)==XR_COMPILE_RESOURCE_OK &&
            retried.live_bytes==entry.live_bytes && retried.work>failed.work &&
            retried.allocated_bytes>failed.allocated_bytes);
    }
    fprintf(stderr,"physical records: all1024 affine masks, literal zero words, %zu constructor FI sites and same-owner retries\n",sites);
}

static void physical_controls(const XrXirCompileContext *context,EffectBodyFunction *body,
    const XrXirFunction *expected) {
    EffectBodyRead read=effect_body_compile_read(context);
    EffectBodyRun *pair=&body->runs[PHYSICAL_MASKS];
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.args={17,19},.immediate=INT64_MIN};
    for (uint32_t w=0;w<10;++w) {
        CHECK(xir_compile_work(context,2*sizeof(uint32_t)));
        uint32_t saved=pair->delta[w];pair->delta[w]^=1;
        CHECK(effect_body_records_match(&read,body,expected)==XR_XIR_BAD_STRUCTURE);
        if (w) {
            XrXirInstruction changed={0};
            CHECK(effect_body_instruction_get(&read,body,PHYSICAL_ROWS-1,&changed)==XR_XIR_OK &&
                !physical_equal(&changed,&expected->instructions[PHYSICAL_ROWS-1]));
        }
        pair->delta[w]=saved;
    }
    for (uint32_t mode=0;mode<5;++mode) {
        CHECK(xir_compile_work(context,sizeof(*pair)));
        EffectBodyRun saved=*pair;XrXirInstruction output=sentinel;
        if (!mode) pair->mask=0;
        else if (mode==1) pair->mask=UINT32_C(1024);
        else if (mode==2) ++pair->first;
        else if (mode==3) ++pair->count;
        else pair->delta[0]=XR_XIR_OP_COUNT;
        CHECK(effect_body_instruction_get(&read,body,PHYSICAL_ROWS-1,&output)==XR_XIR_BAD_STRUCTURE &&
            physical_equal(&output,&sentinel));
        *pair=saved;
    }
    for (uint32_t mask=1;mask<PHYSICAL_MASKS;++mask) {
        EffectBodyRun *run=&body->runs[mask];uint32_t w=0;
        while (!(mask&(UINT32_C(1)<<w))) ++w;
        CHECK(xir_compile_work(context,2*sizeof(uint32_t)));
        uint32_t saved=run->delta[w];run->delta[w]=0;XrXirInstruction output=sentinel;
        CHECK(effect_body_instruction_get(&read,body,run->first,&output)==XR_XIR_BAD_STRUCTURE &&
            physical_equal(&output,&sentinel));run->delta[w]=saved;
    }
    CHECK(effect_body_records_match(&read,body,expected)==XR_XIR_OK);
}

typedef struct PhysicalReadFault { const XrXirCompileContext *context;size_t calls,fail_at; } PhysicalReadFault;
static bool physical_read_consume(const void *owner,uint64_t units) {
    PhysicalReadFault *fault=(PhysicalReadFault *)owner;
    if (fault->calls++==fault->fail_at) return false;
    return xir_compile_work(fault->context,units);
}

static void physical_read_faults(const XrXirCompileContext *context,const EffectBodyFunction *body) {
    const EffectBodyRun *pair=&body->runs[PHYSICAL_MASKS];
    PhysicalReadFault fault={context,0,SIZE_MAX};EffectBodyRead read={&fault,physical_read_consume};
    XrXirInstruction sentinel={.op=XR_XIR_COPY,.args={31,37}},output=sentinel;
    CHECK(effect_body_instruction_ref_value(&read,pair,pair->first+1,&output)==XR_XIR_OK);
    size_t calls=fault.calls;CHECK(calls>1);
    for (size_t at=0;at<calls;++at) {
        fault.calls=0;fault.fail_at=at;output=sentinel;
        CHECK(effect_body_instruction_ref_value(&read,pair,pair->first+1,&output)==XR_XIR_BUDGET &&
            physical_equal(&output,&sentinel) && fault.calls==at+1);
    }
}

#include "xir_effect_literal_adapter_cases.h"

int main(void) {
    const XrXirCompileContext *context=effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrXirInstruction *producer=physical_input(context),*expected_rows=physical_input(context);
    XrXirFunction source={.instructions=producer,.instruction_count=PHYSICAL_ROWS};
    XrXirFunction expected={.instructions=expected_rows,.instruction_count=PHYSICAL_ROWS};
    physical_faults(context,&source,&expected);
    EffectBodyFunction body={.instruction_count=PHYSICAL_ROWS};
    CHECK(effect_body_records_copy(context,&source,&body)==XR_XIR_OK &&
        xir_compile_work(context,PHYSICAL_ROWS*sizeof(*producer)));
    memset(producer,0xCE,PHYSICAL_ROWS*sizeof(*producer));xr_compile_resources_free(producer);source=(XrXirFunction){0};
    physical_roundtrip(context,&body,&expected);physical_controls(context,&body,&expected);
    physical_read_faults(context,&body);xr_compile_resources_free(body.runs);
    xr_compile_resources_free(expected_rows);effects_source_owners_free();
    physical_literal_adapter_cases();return 0;
}
