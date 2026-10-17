/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_snapshot_streaming_writer_cases.h - Completed record intake and stable spans
 */
#ifndef XIR_SNAPSHOT_STREAMING_WRITER_CASES_H
#define XIR_SNAPSHOT_STREAMING_WRITER_CASES_H
typedef struct StreamingWriterFixture {
    const XrXirCompileContext *work;
    EffectBodyFunction *body;
    const XrXirInstruction *expected,*producer;
    EffectCellBodyAdapter adapter;
    XrXirCellProofSpanCursor cursor;
    uint32_t calls,rows,spans,reject;
} StreamingWriterFixture;
static XrCompileResources *streaming_writer_sampling;
static uint64_t streaming_writer_allocation_work[32];
static size_t streaming_writer_allocation_count;
static void streaming_writer_allocation_observe(void *pointer) {
    (void)pointer;CHECK(streaming_writer_sampling && streaming_writer_allocation_count<32);
    /* The allocation callback observes the same request under its resource lock. */
    streaming_writer_allocation_work[streaming_writer_allocation_count++]=streaming_writer_sampling->stats.work;
}
static void streaming_writer_values(XrXirInstruction values[42]) {
    for(uint32_t i=0;i<42;++i){uint64_t bits=UINT64_C(0x80000000ffffffff)+i*i*UINT64_C(0x0000000300000011);
        int64_t immediate;memcpy(&immediate,&bits,sizeof(immediate));
        values[i]=(XrXirInstruction){.op=i%2?XR_XIR_CONST_INT:XR_XIR_RETURN,.type=i%2?XR_XIR_I64:XR_XIR_F64,
            .args={i*i+7,UINT32_MAX-i*3},.targets={i*7+11,UINT32_MAX-i*i},.immediate=immediate,
            .type_arguments={i*13+19,UINT32_MAX-i*17}};
    }
    for(uint32_t i=16;i<22;++i)values[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=36+2*(i-16)};
    values[40]=values[41]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=10000};
}
static XrXirStatus streaming_writer_prefix(StreamingWriterFixture *fixture) {
    const EffectBodyFunction *body=fixture->body;EffectBodyRead read=effect_body_compile_read(fixture->work);
    uint32_t first=0;CHECK(body->run_count && body->run_count<=body->run_capacity);
    for(uint32_t r=0;r<body->run_count;++r){const EffectBodyRun *run=NULL;
        XrXirStatus status=effect_body_run_ref(&read,body,r,first,&run);if(status!=XR_XIR_OK)return status;
        for(uint32_t i=first;i<first+run->count;++i){XrXirInstruction storage;const XrXirInstruction *actual=NULL;
            status=effect_body_instruction_ref_read(&read,run,i,&storage,&actual);if(status!=XR_XIR_OK)return status;
            CHECK(conditional_compact_same(actual,&fixture->expected[i]));}
        first+=run->count;
    }
    CHECK(first<=42);
    if(fixture->spans){CHECK(body->run_count>=3 && body->runs[2].first==16 && body->runs[2].count==6 &&
        !(body->runs[2].mask&EFFECT_BODY_LITERAL_ROWS) && fixture->cursor.next>=22 && fixture->cursor.run>=3);}
    return XR_XIR_OK;
}
static XrXirStatus streaming_writer_row(void *owner,uint32_t row,const XrXirInstruction *value) {
    StreamingWriterFixture *fixture=owner;
    if(fixture->calls++==fixture->reject)return XR_XIR_IO;
    CHECK(row<42 && value==&fixture->producer[row] && conditional_compact_same(value,&fixture->expected[row]));
    ++fixture->rows;return streaming_writer_prefix(fixture);
}
static XrXirStatus streaming_writer_span(void *owner,uint32_t first,uint32_t count) {
    StreamingWriterFixture *fixture=owner;
    if(fixture->calls++==fixture->reject)return XR_XIR_IO;
    CHECK((!fixture->spans && first==16 && count==6) || (fixture->spans==1 && first==40 && count==2));
    XrXirStatus status=effect_cell_body_span(fixture->work,&fixture->adapter,0,first,count,&fixture->cursor);
    if(status!=XR_XIR_OK)return status;
    ++fixture->spans;CHECK(fixture->cursor.next==first+count && fixture->cursor.shape.op==XR_XIR_CONST_INT &&
        fixture->cursor.shape.type==XR_XIR_I64);return streaming_writer_prefix(fixture);
}
static XrXirStatus streaming_writer_operation(const XrXirCompileContext *context,uint32_t reject,uint32_t *calls) {
    XrXirInstruction expected[42],source[42];streaming_writer_values(expected);memcpy(source,expected,sizeof(source));
    XrXirFunction input={.instructions=source,.instruction_count=42};EffectBodyFunction body={.instruction_count=42};
    EffectBodyMaterial material={.resources=context->resources,.references=1,.functions=&body};
    material.domain.types=&material.terms.types;material.domain.function_count=1;
    StreamingWriterFixture fixture={.work=context,.body=&body,.expected=expected,.producer=source,.reject=reject};
    fixture.adapter.work=context;fixture.adapter.request=effect_body_owned_request(context,&material);
    EffectBodyWriteSink sink={&fixture,streaming_writer_row,streaming_writer_span};
    XrXirStatus status=effect_body_records_write(context,&input,&body,&sink);
    if(status==XR_XIR_OK){CHECK(fixture.calls==36 && fixture.rows==34 && fixture.spans==2 &&
        fixture.cursor.next==42 && fixture.cursor.run==7 && body.run_count==7);
        status=effect_cell_body_span(context,&fixture.adapter,0,42,0,&fixture.cursor);}
    if(status==XR_XIR_OK)status=effect_body_runs_finish(context,&body);
    if(status==XR_XIR_OK){memset(source,0xce,sizeof(source));CHECK(body.run_count==body.run_capacity);
        const CompileAllocation *allocation=(const CompileAllocation *)body.runs-1;
        CHECK(allocation->info.resources==context->resources && allocation->info.bytes==7*sizeof(*body.runs));
        status=streaming_writer_prefix(&fixture);}
    *calls=fixture.calls;xr_compile_resources_free(body.runs);return status;
}
static void snapshot_streaming_writer_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;uint32_t calls=0;size_t first=rp_attempts;
    CHECK(!rp_allocation_observer);streaming_writer_sampling=context.resources;streaming_writer_allocation_count=0;
    rp_allocation_observer=streaming_writer_allocation_observe;
    XrXirStatus status=streaming_writer_operation(&context,UINT32_MAX,&calls);
    rp_allocation_observer=NULL;streaming_writer_sampling=NULL;
    if(status!=XR_XIR_OK)fprintf(stderr,"Streaming writer first operation status%d callbacks%u\n",status,calls);
    CHECK(status==XR_XIR_OK && calls==36);size_t sites=rp_attempts-first;
    CHECK(sites && sites==streaming_writer_allocation_count);XrCompileResourceStats measured=rp_stats(&context);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    for(uint32_t reject=0;reject<36;++reject){physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
        CHECK(streaming_writer_operation(&context,reject,&calls)==XR_XIR_IO && calls==reject+1 && context.resources==identity);
        rp_balanced(retained);CHECK(streaming_writer_operation(&context,UINT32_MAX,&calls)==XR_XIR_OK && calls==36 &&
            context.resources==identity);rp_balanced(retained);rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(size_t at=0;at<sites;++at){physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
        rp_fail_at=rp_attempts+at;rp_injected=false;status=streaming_writer_operation(&context,UINT32_MAX,&calls);rp_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && context.resources==identity);rp_balanced(retained);
        CHECK(streaming_writer_operation(&context,UINT32_MAX,&calls)==XR_XIR_OK && context.resources==identity);rp_balanced(retained);
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(size_t at=0;at<sites;++at){physical=rp_mark();XrCompileResourceLimits caps=rp_caps();caps.work=streaming_writer_allocation_work[at];
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;RootParameterMark retained=rp_mark();
        CHECK(streaming_writer_operation(&context,UINT32_MAX,&calls)==XR_XIR_BUDGET);rp_balanced(retained);
        CHECK(streaming_writer_operation(&context,UINT32_MAX,&calls)==XR_XIR_BUDGET);rp_balanced(retained);
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){physical=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(streaming_writer_operation(&context,UINT32_MAX,&calls)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    fprintf(stderr,"Streaming completed records: independent42-row oracle, stable affine/constant span prefix and EOF before immutable receiving, 36 actual callback refusals and same finite-owner retries, actual%zu OOM/work cutoffs, six axes%llu/%llu/%llu exact/minusone PASS\n",
        sites,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_SNAPSHOT_STREAMING_WRITER_CASES_H
