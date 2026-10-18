/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_record_endpoint_cases.h - Complete record decoding and same-call boundaries
 */
#ifndef XIR_RECORD_ENDPOINT_CASES_H
#define XIR_RECORD_ENDPOINT_CASES_H
/* These literal rows include unused physical fields. Decoding them does not
 * admit their instruction semantics or confer an execution permission. */
static const XrXirInstruction record_endpoint_rows[]={
    {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.args={17,19},.targets={23,29},
        .immediate=-INT64_C(4294967298),.type_arguments={31,37}},
    {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.args={101,19},.targets={23,29},
        .immediate=-INT64_C(4294967298),.type_arguments={31,37}},
    {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.args={102,19},.targets={23,29},
        .immediate=-INT64_C(4294967298),.type_arguments={31,37}},
    {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.args={103,19},.targets={23,29},
        .immediate=-INT64_C(4294967298),.type_arguments={31,37}},
    {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.args={67,71},.targets={73,79},
        .immediate=INT64_MIN+3,.type_arguments={81,82}},
    {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.args={83,89},.targets={97,101},
        .immediate=-2,.type_arguments={103,107}},
    {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={109,113},.targets={127,131},
        .immediate=INT64_C(4294967297),.type_arguments={137,139}}
};
static EffectBodyFunction record_endpoint_fixture(EffectBodyRun *runs,unsigned mode) {
    runs[0]=(EffectBodyRun){.first=0,.count=mode?1:4,.base=record_endpoint_rows[0]};
    runs[1]=(EffectBodyRun){.first=1,.count=3,.mask=UINT32_C(4),
        .base=record_endpoint_rows[1],.delta={0,0,1}};
    runs[2]=(EffectBodyRun){.first=4,.count=2,.mask=EFFECT_BODY_MASK,
        .base=record_endpoint_rows[4],.delta={XR_XIR_CONST_INT,XR_XIR_I64,83,89,97,101,
            UINT32_C(4294967294),UINT32_MAX,103,107}};
    runs[3]=(EffectBodyRun){.first=6,.count=1,.base=record_endpoint_rows[6]};
    if (mode==3) { runs[0]=runs[2];runs[0].first=0; }
    if (mode==4) runs[0].count=1;
    uint32_t count=mode==2?4u:mode==1?2u:1u;
    return (EffectBodyFunction){.instruction_count=mode==2?7u:mode==3?2u:mode==4?1u:4u,
        .runs=runs,.run_count=count,.run_capacity=count};
}
static void record_endpoint_same(const XrXirInstruction *a,const XrXirInstruction *b) {
    CHECK(a->op==b->op && a->type==b->type && a->immediate==b->immediate);
    for (unsigned i=0;i<2;++i)
        CHECK(a->args[i]==b->args[i] && a->targets[i]==b->targets[i] &&
            a->type_arguments[i]==b->type_arguments[i]);
}
static XrXirStatus record_endpoint_decode(const XrXirCompileContext *context,unsigned mode) {
    EffectBodyRun runs[4];EffectBodyFunction body=record_endpoint_fixture(runs,mode);
    EffectBodyRead read=effect_body_compile_read(context);
    for (unsigned pass=0;pass<2;++pass) for (uint32_t n=0;n<body.instruction_count;++n) {
        uint32_t i=pass?body.instruction_count-1-n:n;
        XrXirInstruction output;memset(&output,0xA5,sizeof(output));
        XrXirInstruction before;memcpy(&before,&output,sizeof(before));
        XrXirStatus status=effect_body_instruction_get(&read,&body,i,&output);
        if (status!=XR_XIR_OK) { CHECK(!memcmp(&before,&output,sizeof(before)));return status; }
        record_endpoint_same(&output,&record_endpoint_rows[mode==3?i+4:mode==4?0:mode?i:0]);
    }
    return XR_XIR_OK;
}
static void record_endpoint_rejected(const XrXirCompileContext *context,
    const EffectBodyFunction *body,uint32_t instruction) {
    EffectBodyRead read=effect_body_compile_read(context);EffectBodyRun sentinel={0};
    const EffectBodyRun *output=&sentinel;
    CHECK(effect_body_record_ref(&read,body,instruction,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(output==&sentinel);
}
static void record_endpoint_corruption(const XrXirCompileContext *context) {
    for (unsigned fault=0;fault<9;++fault) {
        EffectBodyRun runs[4];EffectBodyFunction body=record_endpoint_fixture(runs,2);
        const uint32_t selected[]={0,0,0,2,4,3,1,0,0};
        uint32_t instruction=selected[fault];EffectBodyRead read=effect_body_compile_read(context);
        const EffectBodyRun *prior=NULL;
        CHECK(effect_body_record_ref(&read,&body,instruction,&prior)==XR_XIR_OK && prior);
        switch (fault) {
        case 0:runs[0].first=1;break;
        case 1:runs[0].count=0;break;
        case 2:runs[3].count=2;break;
        case 3:runs[1].first=2;instruction=2;break;
        case 4:runs[2].first=3;instruction=4;break;
        case 5:runs[2].mask=0;instruction=3;break;
        case 6:runs[1].delta[2]=0;instruction=1;break;
        case 7:runs[3].mask=1;break;
        case 8:body.run_capacity=3;break;
        }
        record_endpoint_rejected(context,&body,instruction);
    }
    EffectBodyRun runs[4];EffectBodyFunction body=record_endpoint_fixture(runs,0);
    runs[0].count=3;record_endpoint_rejected(context,&body,0);
    body=record_endpoint_fixture(runs,1);runs[1].first=2;
    record_endpoint_rejected(context,&body,2);
}
static void record_endpoint_cases(void) {
    for (unsigned mode=0;mode<5;++mode) {
        TraceMark mark=trace_mark();XrXirCompileContext context=trace_owner(trace_caps());
        XrCompileResourceStats initial=trace_stats(&context);
        CHECK(record_endpoint_decode(&context,mode)==XR_XIR_OK);
        XrCompileResourceStats exact=trace_stats(&context);
        CHECK(exact.allocated_bytes==initial.allocated_bytes && exact.live_bytes==initial.live_bytes &&
            exact.peak_bytes==initial.peak_bytes && exact.work>initial.work);
        trace_owner_free(&context,initial.live_bytes);trace_balanced(mark);
        for (unsigned minus=0;minus<2;++minus) {
            XrCompileResourceLimits caps=trace_caps();caps.work=exact.work-minus;
            context=trace_owner(caps);uint64_t baseline=trace_stats(&context).live_bytes;
            CHECK(record_endpoint_decode(&context,mode)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            trace_owner_free(&context,baseline);trace_balanced(mark);
        }
        printf("RECORD_ENDPOINT mode%u work%llu no-reader-allocation physical0\n",mode,
            (unsigned long long)exact.work);
    }
    TraceMark mark=trace_mark();XrXirCompileContext context=trace_owner(trace_caps());
    uint64_t baseline=trace_stats(&context).live_bytes;record_endpoint_corruption(&context);
    trace_owner_free(&context,baseline);trace_balanced(mark);
}
#endif // XIR_RECORD_ENDPOINT_CASES_H
