/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_owned_material_cases.h - Error replay after real producer death
 *
 * KEY CONCEPT:
 *   The genuine sealed certificate owns every CFG input and authority proof.
 */
#ifndef XIR_ERROR_OWNED_MATERIAL_CASES_H
#define XIR_ERROR_OWNED_MATERIAL_CASES_H

static unsigned owned_error_refusals;
static XrXirStatus owned_error_refuse_head(const EffectBodyRequest *read,uint32_t function,
    EffectBodyFunctionHead *head,EffectBodyFunctionDimensions *dimensions) {
    (void)read;(void)function;(void)head;(void)dimensions;
    ++owned_error_refusals;return XR_XIR_BAD_STRUCTURE;
}

static void owned_error_input_controls(const XrXirCompileContext *context,
    XrXirEffects *effects,ErrorFlow *flow) {
    ErrorInvocationContexts *sentinel=(ErrorInvocationContexts *)(uintptr_t)1;
    XrCompileResourceStats before=rp_stats(context);size_t attempts=rp_attempts;
    CHECK(error_invocation_contexts_new(flow,&sentinel)==XR_XIR_BAD_STRUCTURE &&
        sentinel==(ErrorInvocationContexts *)(uintptr_t)1);
    CHECK(rp_stats(context).work==before.work && rp_attempts==attempts);
    ErrorInvocationContexts *output=NULL;EffectBodyRequest saved=flow->body;
    flow->body.head=owned_error_refuse_head;owned_error_refusals=0;
    CHECK(error_invocation_contexts_new(flow,&output)==XR_XIR_BAD_STRUCTURE &&
        !output && owned_error_refusals==1);
    flow->body=saved;
    flow->body.domain.function_count++;
    CHECK(error_invocation_contexts_new(flow,&output)==XR_XIR_BAD_STRUCTURE && !output);
    flow->body=saved;
    flow->body.owner=NULL;
    CHECK(error_invocation_contexts_new(flow,&output)==XR_XIR_BAD_STRUCTURE && !output);
    flow->body=saved;
    XrXirCompileContext foreign=rp_owner(rp_caps());uint64_t baseline=rp_stats(&foreign).live_bytes;
    XrXirCompileContext *remaining=flow->remaining;flow->remaining=&foreign;
    before=rp_stats(&foreign);attempts=rp_attempts;
    CHECK(error_invocation_contexts_new(flow,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(rp_stats(&foreign).work==before.work && rp_attempts==attempts && effects->resources==context->resources);
    flow->remaining=remaining;rp_owner_free(&foreign,baseline);
}

static void owned_error_source_controls(const XrXirCompileContext *context,
    const EffectBodyMaterial *body,const XrXirModule *original) {
    CHECK(effect_body_source_match(context,body,original)==XR_XIR_OK);
    XrXirFunction functions[5];XrXirInstruction operations[4];
    CHECK(original->function_count==5 && original->functions[2].instruction_count<=4);
    memcpy(functions,original->functions,sizeof(functions));
    memcpy(operations,functions[2].instructions,functions[2].instruction_count*sizeof(*operations));
    XrXirModule changed=*original;changed.functions=functions;functions[2].instructions=operations;
    operations[0].targets[0]^=1;
    CHECK(effect_body_source_match(context,body,&changed)==XR_XIR_BAD_STRUCTURE);
    operations[0].targets[0]^=1;operations[0].immediate^=INT64_C(1)<<32;
    CHECK(effect_body_source_match(context,body,&changed)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_body_source_match(context,body,original)==XR_XIR_OK);
}

/* The pure returned target and the non-producing factory retain independent
 * known and UNKNOWN error outcomes after both producers have been destroyed. */
static XrXirStatus conditional_owned_error(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    InvocationReturnedFixture fixture;invocation_returned_fixture(&fixture,mode?4:0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    if (status==XR_XIR_OK && oracle)
        owned_error_source_controls(context,effects->invocations->body,xr_xir_compile_artifact_module(checked));
    xr_xir_compile_artifact_free(checked);checked=NULL;memset(&fixture,0xCE,sizeof(fixture));
    EffectTerms terms={0};terms.remaining=context;ErrorFlow flow={0};
    if (status==XR_XIR_OK) {
        flow.body=effect_body_owned_request(context,effects->invocations->body);
        terms.types=*flow.body.domain.types;flow.effects=effects;
        flow.remaining=(XrXirCompileContext *)context;flow.terms=&terms;
        flow.summary_count=effects->count;flow.invocation=UINT32_MAX;
        if (oracle) owned_error_input_controls(context,effects,&flow);
        status=error_invocation_contexts_new(&flow,&flow.invocations);
    }
    if (status==XR_XIR_OK && !flow.invocations) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) {
        size_t bytes=(size_t)effects->count*effects->words*sizeof(*effects->errors);
        if (!xir_compile_work(context,bytes)) status=XR_XIR_BUDGET;
        else memset(effects->errors,0,bytes);
    }
    if (status==XR_XIR_OK) status=error_invocation_summaries(&flow);
    if (status==XR_XIR_OK) status=error_invocation_functions(&flow);
    if (status==XR_XIR_OK) {
        CHECK(effects->count==5 && effects->errors && effects->words==1);
        CHECK(error_bit(effects->errors,0)==(mode!=0) && !error_bit(effects->errors,1));
        CHECK(!effects->invocations->equations->module && !effects->invocations->equations->work);
    }
    error_storage_free(&flow);error_invocation_contexts_free(flow.invocations);
    effect_terms_free(&terms);xr_xir_compile_effects_free(effects);return status;
}

/* Each scan starts at physical zero. The independent literal rows retain
 * unused words, signed high bits and both nonconstant affine runs. */
static void owned_error_cursor_values(const XrXirCompileContext *context,
    const EffectBodyFunction *body,const XrXirInstruction *expected) {
    EffectBodyRead read=effect_body_compile_read(context);
    CHECK(xir_compile_work(context,sizeof(EffectBodyRecordCursor)));
    EffectBodyRecordCursor cursor={0,0,0,UINT32_MAX};
    for (uint32_t i=0;i<body->instruction_count;++i) {
        XrXirInstruction decoded;
        CHECK(effect_body_instruction_cursor_get(&read,body,i,&cursor,&decoded)==XR_XIR_OK &&
            conditional_compact_same(&decoded,&expected[i]));
    }
    CHECK(xir_compile_work(context,sizeof(cursor)));cursor=(EffectBodyRecordCursor){0,0,0,UINT32_MAX};
    for (uint32_t i=0;i<body->instruction_count;++i) {
        EffectBodyInstructionFields fields;
        CHECK(effect_body_instruction_cursor_fields(&read,body,i,&cursor,&fields)==XR_XIR_OK &&
            fields.op==expected[i].op && fields.type==expected[i].type && fields.source==expected[i].args[0]);
    }
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_I64,.immediate=INT64_MIN},out=sentinel;
    CHECK(effect_body_instruction_cursor_get(&read,body,0,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        conditional_compact_same(&out,&sentinel));
    CHECK(effect_body_instruction_cursor_get(&read,body,body->instruction_count,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        conditional_compact_same(&out,&sentinel));
    CHECK(xir_compile_work(context,sizeof(cursor)));cursor=(EffectBodyRecordCursor){1,0,0,UINT32_MAX};
    CHECK(effect_body_instruction_cursor_get(&read,body,0,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        conditional_compact_same(&out,&sentinel));
}

/* Every real read/output charge is a refusal point. Cursor movement is
 * private progress and may survive a refusal; neither output is published. */
static void owned_error_cursor_refusals(const XrXirCompileContext *context,
    const EffectBodyFunction *body,const XrXirInstruction *expected) {
    for (uint32_t narrow=0;narrow<2;++narrow) {
        ConditionalCompactRead observation={context,0,UINT64_MAX};
        EffectBodyRead read={&observation,conditional_compact_read_work};
        CHECK(xir_compile_work(context,sizeof(EffectBodyRecordCursor)));
        EffectBodyRecordCursor cursor={0,0,0,UINT32_MAX};
        XrXirInstruction decoded;
        EffectBodyInstructionFields fields;
        if (narrow) CHECK(effect_body_instruction_cursor_fields(&read,body,4,&cursor,&fields)==XR_XIR_OK &&
            fields.op==expected->op && fields.type==expected->type && fields.source==expected->args[0]);
        else CHECK(effect_body_instruction_cursor_get(&read,body,4,&cursor,&decoded)==XR_XIR_OK &&
            conditional_compact_same(&decoded,expected));
        uint64_t points=observation.calls;CHECK(points);
        for (uint64_t at=0;at<points;++at) {
            observation.calls=0;observation.reject=at;
            CHECK(xir_compile_work(context,sizeof(cursor)));cursor=(EffectBodyRecordCursor){0,0,0,UINT32_MAX};
            XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,
                .args={11,12},.targets={13,14},.immediate=INT64_MIN,.type_arguments={15,16}};
            EffectBodyInstructionFields narrow_sentinel={XR_XIR_RETURN,XR_XIR_BOOL,UINT32_MAX};
            decoded=sentinel;fields=narrow_sentinel;
            if (narrow) CHECK(effect_body_instruction_cursor_fields(&read,body,4,&cursor,&fields)==XR_XIR_BUDGET &&
                !memcmp(&fields,&narrow_sentinel,sizeof(fields)));
            else CHECK(effect_body_instruction_cursor_get(&read,body,4,&cursor,&decoded)==XR_XIR_BUDGET &&
                conditional_compact_same(&decoded,&sentinel));
            CHECK(observation.calls==at+1);
        }
    }
}

static void owned_error_cursor_malformed(const XrXirCompileContext *context,
    EffectBodyFunction *body,const XrXirFunction *expected) {
    EffectBodyRead read=effect_body_compile_read(context);EffectBodyRunRef first;
    CHECK(body->stream_words<=64 && body->run_count==2);
    CHECK(effect_body_run_ref(&read,body,0,0,&first)==XR_XIR_OK);
    uint32_t saved[64];memcpy(saved,body->stream,body->stream_words*sizeof(*saved));
    uint32_t words=body->stream_words,runs=body->run_count,capacity=body->stream_capacity;
    for (uint32_t mode=0;mode<9;++mode) {
        if (!mode) body->stream[0]=1;
        else if (mode==1) body->stream[1]=0;
        else if (mode==2) body->stream[2]|=UINT32_C(1024);
        else if (mode==3) body->stream_words=EFFECT_BODY_HEADER-1;
        else if (mode==4) body->run_count=0;
        else if (mode==5) body->stream_capacity=body->stream_words-1;
        else if (mode==6) body->stream[EFFECT_BODY_HEADER]=0;
        else if (mode==7) body->stream[first.next]=first.first+first.count+1;
        else --body->instruction_count;
        CHECK(xir_compile_work(context,sizeof(EffectBodyRecordCursor)));
        EffectBodyRecordCursor cursor={0,0,0,UINT32_MAX};
        XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_I64,.immediate=INT64_MIN},out=sentinel;
        CHECK(effect_body_instruction_cursor_get(&read,body,4,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
            conditional_compact_same(&out,&sentinel));
        CHECK(xir_compile_work(context,sizeof(cursor)));cursor=(EffectBodyRecordCursor){0,0,0,UINT32_MAX};
        EffectBodyInstructionFields fields_sentinel={XR_XIR_RETURN,XR_XIR_BOOL,UINT32_MAX},fields=fields_sentinel;
        CHECK(effect_body_instruction_cursor_fields(&read,body,4,&cursor,&fields)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&fields,&fields_sentinel,sizeof(fields)));
        memcpy(body->stream,saved,words*sizeof(*saved));body->stream_words=words;
        body->run_count=runs;body->stream_capacity=capacity;body->instruction_count=8;
        CHECK(effect_body_records_match(&read,body,expected)==XR_XIR_OK);
    }
    CHECK(xir_compile_work(context,sizeof(EffectBodyRecordCursor)));
    EffectBodyRecordCursor cursor={0,0,0,UINT32_MAX};XrXirInstruction out;
    CHECK(effect_body_instruction_cursor_get(&read,body,0,&cursor,&out)==XR_XIR_OK);
    body->stream[first.next]=first.first+first.count+1;
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_I64,.immediate=INT64_MIN};out=sentinel;
    CHECK(effect_body_instruction_cursor_get(&read,body,1,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        conditional_compact_same(&out,&sentinel));
    memcpy(body->stream,saved,words*sizeof(*saved));
}

static void conditional_owned_error_cursor_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    XrXirInstruction original[8],producer[8];conditional_compact_rows(original);
    memcpy(producer,original,sizeof(producer));
    XrXirFunction input={.instructions=producer,.instruction_count=8};
    XrXirFunction expected={.instructions=original,.instruction_count=8};
    EffectBodyFunction body={.instruction_count=8};
    CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_OK);
    memset(producer,0xCE,sizeof(producer));
    owned_error_cursor_values(&context,&body,original);
    owned_error_cursor_refusals(&context,&body,&original[4]);
    owned_error_cursor_malformed(&context,&body,&expected);
    EffectBodyRead read=effect_body_compile_read(&context);
    for (uint32_t field=0;field<10;++field) {
        XrXirInstruction saved=original[2];conditional_compact_change(&original[2],field);
        CHECK(effect_body_records_match(&read,&body,&expected)==XR_XIR_BAD_STRUCTURE);
        original[2]=saved;CHECK(effect_body_records_match(&read,&body,&expected)==XR_XIR_OK);
    }
    xr_compile_resources_free(body.stream);rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void conditional_owned_error_literals(void) {
    conditional_owned_error_cursor_literals();
    for (uint32_t mode=0;mode<2;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_owned_error(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

/* The dedicated owned Error gate runs the complete two-operation census.
 * Other argv retain their original fault responsibility without repeating it. */
static void conditional_owned_error_resources(void) {
    for (uint32_t mode=0;mode<2;++mode) {
        size_t sites=0;XrCompileResourceStats census={0},entry={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());entry=rp_stats(&context);
            RootParameterMark retained=rp_mark();size_t first=rp_attempts;
            rp_fail_at=pass?first+pass-1:SIZE_MAX;
            XrXirStatus status=conditional_owned_error(&context,mode,false);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts-first;census=rp_stats(&context);CHECK(sites);
                printf("OWNED_ERROR_CENSUS mode=%u sites=%zu operation_work=%llu operation_bytes=%llu owner_work=%llu owner_bytes=%llu peak=%llu allocations=%llu\n",
                    mode,sites,(unsigned long long)(census.work-entry.work),
                    (unsigned long long)(census.allocated_bytes-entry.allocated_bytes),
                    (unsigned long long)census.work,(unsigned long long)census.allocated_bytes,
                    (unsigned long long)census.peak_bytes,
                    (unsigned long long)(census.allocation_count-entry.allocation_count));
            } else {
                CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts-first==pass);
                CHECK(rp_stats(&context).live_bytes==entry.live_bytes);rp_balanced(retained);
                XrCompileResourceStats consumed=rp_stats(&context);
                uint64_t work=census.work-entry.work,bytes=census.allocated_bytes-entry.allocated_bytes;
                bool fits=work<=context.resources->limits.work-consumed.work &&
                    bytes<=context.resources->limits.allocated_bytes-consumed.allocated_bytes;
                rp_fail_at=SIZE_MAX;size_t retry=rp_attempts;
                status=conditional_owned_error(&context,mode,false);
                CHECK(status==(fits?XR_XIR_OK:XR_XIR_BUDGET));
                CHECK(rp_stats(&context).live_bytes==entry.live_bytes);rp_balanced(retained);
                if (fits) CHECK(rp_stats(&context).work-consumed.work==work &&
                    rp_stats(&context).allocated_bytes-consumed.allocated_bytes==bytes &&
                    rp_stats(&context).allocation_count-consumed.allocation_count==census.allocation_count-entry.allocation_count &&
                    rp_attempts-retry==sites);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=rp_caps();uint64_t limit=measured[axis]-minus;
            if (axis==0) limits.allocated_bytes=limit;
            else if (axis==1) limits.live_bytes=limit;
            else limits.work=limit;
            RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(limits);
            uint64_t baseline=rp_stats(&context).live_bytes;
            CHECK(conditional_owned_error(&context,mode,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            rp_owner_free(&context,baseline);rp_balanced(physical);
        }
    }
}
#endif // XIR_ERROR_OWNED_MATERIAL_CASES_H
