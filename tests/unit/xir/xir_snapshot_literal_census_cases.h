/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_snapshot_literal_census_cases.h - Independent physical capacity counts
 */
#ifndef XIR_SNAPSHOT_LITERAL_CENSUS_CASES_H
#define XIR_SNAPSHOT_LITERAL_CENSUS_CASES_H
/* Keep the original row loop as an independent capacity and work oracle. */
static XrXirStatus literal_census_original(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyRun *run,void *owner) {
    EffectBodyRecordCensus *census=owner;
    XrXirStatus status=effect_body_record_prepare(work,source,run);
    if(status!=XR_XIR_OK)return status;
    if(!census || census->next!=run->first || census->count>census->next ||
        census->tail>EFFECT_BODY_LITERAL_CAPACITY)return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(work,6+sizeof(uint32_t)))return XR_XIR_BUDGET;
    if(run->count==1 || (run->mask&EFFECT_BODY_LITERAL_ROWS)) {
        for(uint32_t i=0;i<run->count;++i) {
            if(!xir_compile_work(work,6+2*sizeof(uint32_t)))return XR_XIR_BUDGET;
            if(census->tail && census->tail<EFFECT_BODY_LITERAL_CAPACITY)++census->tail;
            else {if(census->count>=source->instruction_count)return XR_XIR_BAD_STRUCTURE;
                ++census->count;census->tail=1;}
        }
    } else {
        if(census->count>=source->instruction_count)return XR_XIR_BAD_STRUCTURE;
        if(!xir_compile_work(work,2*sizeof(uint32_t)))return XR_XIR_BUDGET;
        ++census->count;census->tail=0;
    }
    census->next+=run->count;return XR_XIR_OK;
}
static void literal_census_input(XrXirInstruction values[32],XrXirFunction *source,
    EffectBodyRun *run,uint32_t count) {
    XrXirInstruction rows[17];literal_block_values(rows);memset(values,0,32*sizeof(*values));
    for(uint32_t i=0;i<count;++i)values[16+i]=rows[i];
    *source=(XrXirFunction){.instructions=values,.instruction_count=32};
    *run=(EffectBodyRun){.first=16,.count=count,.mask=count>1?EFFECT_BODY_LITERAL_ROWS:0,.base=rows[0]};
    for(uint32_t i=1;i<count;++i)run->literal_tail[i-1]=rows[i];
}
static uint64_t literal_census_case(uint32_t tail,uint32_t count,uint64_t cut) {
    RootParameterMark held=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    XrXirInstruction values[32];XrXirFunction source;EffectBodyRun run;literal_census_input(values,&source,&run,count);
    EffectBodyRecordCensus census={16,2,tail},expected=census;EffectBodyRun original=run;
    CHECK(literal_census_original(&context,&source,&original,&expected)==XR_XIR_OK);
    uint64_t entry=rp_stats(&context).work;size_t attempts=rp_attempts;
    if(cut!=UINT64_MAX)context.resources->limits.work=entry+cut;
    XrXirStatus status=effect_body_record_count(&context,&source,&run,&census);uint64_t work=rp_stats(&context).work-entry;
    if(status==XR_XIR_OK){CHECK(census.next==expected.next && census.count==expected.count && census.tail==expected.tail);
        CHECK(!memcmp(&run,&original,sizeof(run)));}
    else {const EffectBodyRecordCensus unchanged={16,2,tail};CHECK(status==XR_XIR_BUDGET && !memcmp(&census,&unchanged,sizeof(census)));
        CHECK(effect_body_record_count(&context,&source,&run,&census)==XR_XIR_BUDGET && !memcmp(&census,&unchanged,sizeof(census)));}
    CHECK(rp_attempts==attempts);rp_owner_free(&context,live);rp_balanced(held);return status==XR_XIR_OK?work:0;
}
static void literal_census_masks_and_guards(void) {
    RootParameterMark held=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    XrXirInstruction values[32];XrXirFunction source;EffectBodyRun input;literal_census_input(values,&source,&input,2);
    size_t attempts=rp_attempts;
    for(uint32_t mask=0;mask<=EFFECT_BODY_MASK;++mask)for(uint32_t tail=0;tail<=8;++tail){
        EffectBodyRun actual=input,expected=input;actual.mask=expected.mask=mask;
        EffectBodyRecordCensus a={16,2,tail},b=a;
        CHECK(effect_body_record_count(&context,&source,&actual,&a)==XR_XIR_OK &&
            literal_census_original(&context,&source,&expected,&b)==XR_XIR_OK);
        CHECK(!memcmp(&a,&b,sizeof(a)) && !memcmp(&actual,&expected,sizeof(actual)));
        if(mask)CHECK(actual.mask==EFFECT_BODY_LITERAL_ROWS && conditional_compact_same(&actual.literal_tail[0],&values[17]));
        else CHECK(!a.tail && a.count==3);
    }
    for(uint32_t tail=0;tail<=8;++tail){EffectBodyRun run={.first=16,.count=9,.base=values[16]};
        EffectBodyRecordCensus a={16,2,tail},b=a;EffectBodyRun other=run;
        CHECK(effect_body_record_count(&context,&source,&run,&a)==XR_XIR_OK &&
            literal_census_original(&context,&source,&other,&b)==XR_XIR_OK && !memcmp(&a,&b,sizeof(a)) && !a.tail);}
    for(unsigned fault=0;fault<12;++fault){EffectBodyRun run=input;EffectBodyRecordCensus a={16,2,3},b=a;
        XrXirFunction current=source;EffectBodyRecordCensus *owner=&a;
        if(!fault)owner=NULL;else if(fault==1)a.next=15;else if(fault==2)a.count=17;else if(fault==3)a.tail=9;
        else if(fault==4)current.instructions=NULL;else if(fault==5)run.count=0;else if(fault==6)run.first=32;
        else if(fault==7)run.count=17;else if(fault==8)run.mask=UINT32_MAX;
        else if(fault==9){run.count=1;run.mask=1;}else if(fault==10){run.count=9;run.mask=EFFECT_BODY_LITERAL_ROWS;}
        else run.mask=EFFECT_BODY_LITERAL_ROWS|1;
        b=a;EffectBodyRun other=run;XrXirStatus expected=literal_census_original(&context,&current,&other,owner?&b:NULL);
        CHECK(expected==XR_XIR_BAD_STRUCTURE && effect_body_record_count(&context,&current,&run,owner)==expected && !memcmp(&a,&b,sizeof(a)));
    }
    for(uint32_t row=0;row<8;++row)for(unsigned upper=0;upper<2;++upper){EffectBodyRun run;
        literal_census_input(values,&source,&run,8);XrXirInstruction *operation=row?&run.literal_tail[row-1]:&run.base;
        operation->op=upper?XR_XIR_OP_COUNT:XR_XIR_INVALID;EffectBodyRecordCensus census={16,2,3},original=census;
        CHECK(effect_body_record_count(&context,&source,&run,&census)==XR_XIR_BAD_STRUCTURE && !memcmp(&census,&original,sizeof(census)));
    }
    CHECK(rp_attempts==attempts);rp_owner_free(&context,live);rp_balanced(held);
}
static void snapshot_literal_census_controls(void) {
    literal_census_masks_and_guards();uint64_t cuts=0;
    for(uint32_t tail=0;tail<=8;++tail)for(uint32_t count=1;count<=8;++count){
        uint64_t work=literal_census_case(tail,count,UINT64_MAX);CHECK(work);
        for(uint64_t cut=0;cut<=work;++cut){uint64_t actual=literal_census_case(tail,count,cut);
            CHECK(cut==work?actual==work:!actual);++cuts;}
    }
    RootParameterMark held=rp_mark();XrCompileResourceStats measured={0};
    for(uint32_t mode=0;mode<7;++mode){XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        CHECK(seed_cursor_genuine(&context,mode,false)==XR_XIR_OK);if(mode==6)measured=rp_stats(&context);
        rp_owner_free(&context,live);rp_balanced(held);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        XrXirCompileContext context=rp_owner(caps);uint64_t live=rp_stats(&context).live_bytes;
        CHECK(seed_cursor_genuine(&context,6,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,live);rp_balanced(held);}
    fprintf(stderr,"Literal census: original row-loop oracle for all72 tail/length pairs and all1024 masks at nine tails, complete prepared physical rows, stable uniform boundary, twelve structure and sixteen current opcode refusals, no allocation and atomic census at every work unit across%llu cuts with same finite-owner retry; all7 genuine Checked/Cell/Class bodies after producer death and independent publication/Class, six axes%llu/%llu/%llu exact/minusone physical0 PASS\n",
        (unsigned long long)cuts,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_SNAPSHOT_LITERAL_CENSUS_CASES_H
