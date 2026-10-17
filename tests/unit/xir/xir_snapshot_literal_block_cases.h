/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_snapshot_literal_block_cases.h - Complete bounded literal data and cleanup
 */
#ifndef XIR_SNAPSHOT_LITERAL_BLOCK_CASES_H
#define XIR_SNAPSHOT_LITERAL_BLOCK_CASES_H
static XrCompileResources *literal_block_sampling;
static uint64_t literal_block_allocation_work[32];
static size_t literal_block_allocation_count;
static void literal_block_allocation_observe(void *pointer) {
    (void)pointer;CHECK(literal_block_sampling && literal_block_allocation_count<32);
    /* The allocation callback runs under the same request's resource lock. */
    literal_block_allocation_work[literal_block_allocation_count++]=literal_block_sampling->stats.work;
}
static void literal_block_values(XrXirInstruction values[17]) {
    for(uint32_t i=0;i<17;++i){uint64_t bits=UINT64_C(0x80000000ffffffff)+i*i*UINT64_C(0x0000000300000011);
        int64_t immediate;memcpy(&immediate,&bits,sizeof(immediate));
        values[i]=(XrXirInstruction){.op=i%2?XR_XIR_CONST_INT:XR_XIR_RETURN,.type=i%2?XR_XIR_I64:XR_XIR_F64,
            .args={i*i+7,UINT32_MAX-i*3},.targets={i*7+11,UINT32_MAX-i*i},.immediate=immediate,
            .type_arguments={i*13+19,UINT32_MAX-i*17}};
    }
}
static XrXirStatus literal_block_operation(const XrXirCompileContext *context,uint32_t count) {
    CHECK(count && count<=17);XrXirInstruction expected[17],source[17];literal_block_values(expected);
    memcpy(source,expected,sizeof(source));XrXirFunction input={.instructions=source,.instruction_count=count};
    EffectBodyFunction body={.instruction_count=count};XrXirStatus status=effect_body_records_copy(context,&input,&body);
    if(status!=XR_XIR_OK){xr_compile_resources_free(body.runs);return status;}
    memset(source,0xce,sizeof(source));EffectBodyRead read=effect_body_compile_read(context);
    CHECK(body.run_count==(count+7)/8 && body.run_capacity==body.run_count);
    const CompileAllocation *allocation=(const CompileAllocation *)body.runs-1;
    CHECK(allocation->info.resources==context->resources && allocation->info.bytes==(size_t)body.run_count*sizeof(*body.runs));
    for(uint32_t i=0;i<count && status==XR_XIR_OK;++i){XrXirInstruction storage={0};const XrXirInstruction *actual=NULL;
        const EffectBodyRun *run=NULL;status=effect_body_record_ref(&read,&body,i,&run);
        if(status==XR_XIR_OK)status=effect_body_instruction_ref_read(&read,run,i,&storage,&actual);
        if(status!=XR_XIR_OK)break;
        CHECK(conditional_compact_same(actual,&expected[i]) && actual==effect_body_literal_row(run,i));
    }
    xr_compile_resources_free(body.runs);return status;
}
static XrXirStatus literal_block_visit(void *owner,const EffectBodyRun *run) {
    (void)run;++*(uint32_t *)owner;return XR_XIR_OK;
}
static void literal_block_partial_spans(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    XrXirInstruction source[17];literal_block_values(source);
    for(uint32_t i=0;i<17;++i){source[i].op=XR_XIR_CONST_INT;source[i].type=XR_XIR_I64;}
    XrXirFunction input={.instructions=source,.instruction_count=17};EffectBodyFunction body={.instruction_count=17};
    CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_OK && body.run_count==3);
    memset(source,0xce,sizeof(source));XrXirCellProofSpanCursor cursor={0};const uint32_t counts[]={2,9,6,0};uint32_t first=0;
    for(unsigned i=0;i<4;++i){CHECK(effect_cell_literal_span(&context,&body,first,counts[i],&cursor)==XR_XIR_OK);
        first+=counts[i];CHECK(cursor.next==first && cursor.run==(first==17?3:first/8) &&
            cursor.shape.op==XR_XIR_CONST_INT && cursor.shape.type==XR_XIR_I64);}
    for(unsigned mode=0;mode<6;++mode){XrXirCellProofSpanCursor held={.shape={XR_XIR_RETURN,XR_XIR_BOOL}};cursor=held;
        if(mode==0)body.runs[0].literal_tail[0].op=XR_XIR_CONST_FLOAT;
        else if(mode==1)body.runs[0].literal_tail[0].type=XR_XIR_F64;
        else if(mode==2)body.runs[1].literal_tail[0].op=XR_XIR_INVALID;
        else if(mode==3)++body.runs[1].first;else if(mode==4)cursor.run=held.run=1;else body.runs[0].mask|=1;
        CHECK(effect_cell_literal_span(&context,&body,0,2,&cursor)==(mode<2?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE) &&
            !memcmp(&cursor,&held,sizeof(cursor)));
        body.runs[0].literal_tail[0].op=body.runs[1].literal_tail[0].op=XR_XIR_CONST_INT;
        body.runs[0].literal_tail[0].type=XR_XIR_I64;body.runs[1].first=8;body.runs[0].mask=EFFECT_BODY_LITERAL_ROWS;
        cursor=(XrXirCellProofSpanCursor){0};CHECK(effect_cell_literal_span(&context,&body,0,2,&cursor)==XR_XIR_OK);
    }
    XrCompileResourceStats before=rp_stats(&context);cursor=(XrXirCellProofSpanCursor){0};
    CHECK(effect_cell_literal_span(&context,&body,0,2,&cursor)==XR_XIR_OK);uint64_t exact=rp_stats(&context).work-before.work+1;
    for(unsigned minus=0;minus<2;++minus){RootParameterMark retained=rp_mark();XrCompileResourceLimits caps=rp_caps();caps.work=exact-minus;
        XrXirCompileContext finite=rp_owner(caps);uint64_t live=rp_stats(&finite).live_bytes;
        XrXirCellProofSpanCursor held={.shape={XR_XIR_RETURN,XR_XIR_BOOL}};cursor=held;
        CHECK(effect_cell_literal_span(&finite,&body,0,2,&cursor)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        if(minus)CHECK(!memcmp(&cursor,&held,sizeof(cursor)) && effect_cell_literal_span(&finite,&body,0,2,&cursor)==XR_XIR_BUDGET &&
            !memcmp(&cursor,&held,sizeof(cursor)));rp_owner_free(&finite,live);rp_balanced(retained);}
    xr_compile_resources_free(body.runs);rp_owner_free(&context,baseline);rp_balanced(physical);
    fprintf(stderr,"Complete literal partial/cross-block spans: fresh entire canonical directory, current selected op/type and unselected opcode refusals, bad directory/cursor/mask, atomic output and exact%llu/minusone same finite-owner retry PASS\n",(unsigned long long)exact);
}
static void literal_block_malformed(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    XrXirInstruction expected[17],source[17];literal_block_values(expected);memcpy(source,expected,sizeof(source));
    XrXirFunction input={.instructions=source,.instruction_count=17};EffectBodyFunction body={.instruction_count=17};
    CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_OK && body.run_count==3);
    memset(source,0xce,sizeof(source));EffectBodyRead read=effect_body_compile_read(&context);
    XrXirFunction oracle={.instructions=expected,.instruction_count=17};
    for(uint32_t i=0;i<17;++i)for(uint32_t field=0;field<10;++field){XrXirInstruction held=expected[i];
        conditional_compact_change(&expected[i],field);CHECK(effect_body_records_match(&read,&body,&oracle)==XR_XIR_BAD_STRUCTURE);
        expected[i]=held;CHECK(effect_body_records_match(&read,&body,&oracle)==XR_XIR_OK);}
    for(uint32_t r=0;r<2;++r){EffectBodyRun *run=&body.runs[r];CHECK(run->mask==EFFECT_BODY_LITERAL_ROWS && run->count==8);
        for(uint32_t mode=0;mode<13;++mode){uint32_t count=run->count,mask=run->mask;
            if(mode<10)run->mask|=UINT32_C(1)<<mode;else run->count=mode==10?0:mode==11?1:9;
            uint32_t calls=0;EffectBodyRunVisitor visitor={&calls,literal_block_visit};
            CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_BAD_STRUCTURE && !calls);
            const XrXirInstruction sentinel={.op=XR_XIR_RETURN,.immediate=INT64_MIN};XrXirInstruction storage=sentinel;
            const XrXirInstruction *output=&sentinel;
            CHECK(effect_body_instruction_ref_read(&read,run,run->first,&storage,&output)==XR_XIR_BAD_STRUCTURE &&
                output==&sentinel && conditional_compact_same(&storage,&sentinel));run->count=count;run->mask=mask;
        }
        for(uint32_t i=0;i<run->count;++i){XrXirInstruction *row=i?&run->literal_tail[i-1]:&run->base;XrXirOp held=row->op;
            row->op=XR_XIR_INVALID;uint32_t calls=0;EffectBodyRunVisitor visitor={&calls,literal_block_visit};
            CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_BAD_STRUCTURE && !calls);
            row->op=held;CHECK(effect_body_records_match(&read,&body,&oracle)==XR_XIR_OK);}
    }
    for(uint32_t i=0;i<17;++i){conditional_compact_read_failures(&context,&body,i,&expected[i]);
        conditional_compact_fields_failures(&context,&body,i,&expected[i]);}
    xr_compile_resources_free(body.runs);rp_owner_free(&context,baseline);rp_balanced(physical);
}
static void snapshot_literal_block_controls(void) {
    for(uint32_t count=1;count<=17;++count){RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;CHECK(literal_block_operation(&context,count)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    literal_block_partial_spans();literal_block_malformed();RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;size_t first=rp_attempts;
    CHECK(!rp_allocation_observer);literal_block_sampling=context.resources;literal_block_allocation_count=0;
    rp_allocation_observer=literal_block_allocation_observe;XrXirStatus sampled=literal_block_operation(&context,17);
    rp_allocation_observer=NULL;literal_block_sampling=NULL;CHECK(sampled==XR_XIR_OK);
    size_t sites=rp_attempts-first;CHECK(sites && sites==literal_block_allocation_count);
    XrCompileResourceStats measured=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    for(size_t at=0;at<sites;++at){physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
        rp_fail_at=rp_attempts+at;rp_injected=false;XrXirStatus status=literal_block_operation(&context,17);rp_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && context.resources==identity);rp_balanced(retained);
        CHECK(literal_block_operation(&context,17)==XR_XIR_OK && context.resources==identity);rp_balanced(retained);
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(size_t at=0;at<literal_block_allocation_count;++at){physical=rp_mark();XrCompileResourceLimits caps=rp_caps();
        caps.work=literal_block_allocation_work[at];context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
        CHECK(literal_block_operation(&context,17)==XR_XIR_BUDGET && context.resources==identity);rp_balanced(retained);
        CHECK(literal_block_operation(&context,17)==XR_XIR_BUDGET && context.resources==identity);rp_balanced(retained);
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){physical=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(literal_block_operation(&context,17)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    fprintf(stderr,"Literal blocks lengths1..17: exact initialized native rows and allocation extents after producer death, all170 field changes, bad masks/counts/every literal opcode before callbacks, all row read-failure prefixes, actual%zu OOM and admitted-allocation work cutoffs with same finite-owner retry, six axes%llu/%llu/%llu exact/minusone PASS\n",
        sites,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_SNAPSHOT_LITERAL_BLOCK_CASES_H
