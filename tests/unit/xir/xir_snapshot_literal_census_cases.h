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
/* Freeze the complete owning prepare, including its original payload gate,
 * so the read-only kernel cannot validate itself through a shared oracle. */
static XrXirStatus literal_census_original_prepare(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyRun *run) {
    if(!source || !run || !source->instructions || !run->count || run->first>=source->instruction_count ||
        run->count>source->instruction_count-run->first ||
        (run->mask&~(EFFECT_BODY_MASK|EFFECT_BODY_LITERAL_ROWS)) || (run->count==1 && run->mask) ||
        ((run->mask&EFFECT_BODY_LITERAL_ROWS) && (run->mask&EFFECT_BODY_MASK)))return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(work,4))return XR_XIR_BUDGET;
    if(run->count==2 && run->mask){
        if(!xir_compile_work(work,2+sizeof(run->literal_tail[0])+sizeof(run->mask)))return XR_XIR_BUDGET;
        run->literal_tail[0]=source->instructions[run->first+1];run->mask=EFFECT_BODY_LITERAL_ROWS;
    }
    if(!run->count || (run->mask&~(EFFECT_BODY_MASK|EFFECT_BODY_LITERAL_ROWS)) ||
        (run->count==1 && run->mask))return XR_XIR_BAD_STRUCTURE;
    uint32_t selected=run->mask&EFFECT_BODY_MASK;
    if(run->mask&EFFECT_BODY_LITERAL_ROWS){
        if(run->count<2 || run->count>EFFECT_BODY_LITERAL_CAPACITY || selected)return XR_XIR_BAD_STRUCTURE;
        if(!xir_compile_work(work,2))return XR_XIR_BUDGET;
        if(run->base.op<=XR_XIR_INVALID || run->base.op>=XR_XIR_OP_COUNT)return XR_XIR_BAD_STRUCTURE;
        for(uint32_t i=1;i<run->count;++i){
            if(!xir_compile_work(work,2))return XR_XIR_BUDGET;
            if(run->literal_tail[i-1].op<=XR_XIR_INVALID || run->literal_tail[i-1].op>=XR_XIR_OP_COUNT)
                return XR_XIR_BAD_STRUCTURE;
        }
        return XR_XIR_OK;
    }
    if(run->count==2 && selected)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w)if(selected&(UINT32_C(1)<<w)){
        if(!xir_compile_work(work,3))return XR_XIR_BUDGET;
        if(!run->delta[w])return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
/* Keep the original row loop as an independent capacity and work oracle. */
static XrXirStatus literal_census_original(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyRun *run,void *owner) {
    EffectBodyRecordCensus *census=owner;
    XrXirStatus status=literal_census_original_prepare(work,source,run);
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
    unsigned char raw[sizeof(run)],current[sizeof(values)];memcpy(raw,&run,sizeof(run));memcpy(current,values,sizeof(values));
    EffectBodyRecordCensus census={16,2,tail},expected=census;EffectBodyRun original=run;
    CHECK(literal_census_original(&context,&source,&original,&expected)==XR_XIR_OK);
    uint64_t entry=rp_stats(&context).work;size_t attempts=rp_attempts;
    if(cut!=UINT64_MAX)context.resources->limits.work=entry+cut;
    XrXirStatus status=effect_body_record_count(&context,&source,&run,&census);uint64_t work=rp_stats(&context).work-entry;
    if(status==XR_XIR_OK){CHECK(census.next==expected.next && census.count==expected.count && census.tail==expected.tail);
        CHECK(!memcmp(&run,raw,sizeof(run)));}
    else {const EffectBodyRecordCensus unchanged={16,2,tail};CHECK(status==XR_XIR_BUDGET && !memcmp(&census,&unchanged,sizeof(census)));
        CHECK(effect_body_record_count(&context,&source,&run,&census)==XR_XIR_BUDGET && !memcmp(&census,&unchanged,sizeof(census)));}
    CHECK(!memcmp(&run,raw,sizeof(run)) && !memcmp(values,current,sizeof(values)) && rp_attempts==attempts);
    rp_owner_free(&context,live);rp_balanced(held);return status==XR_XIR_OK?work:0;
}
static void literal_census_masks_and_guards(void) {
    RootParameterMark held=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    XrXirInstruction values[32];XrXirFunction source;EffectBodyRun input;literal_census_input(values,&source,&input,2);
    size_t attempts=rp_attempts;
    for(uint32_t mask=0;mask<=EFFECT_BODY_MASK;++mask)for(uint32_t tail=0;tail<=8;++tail){
        EffectBodyRun actual=input,expected=input;actual.mask=expected.mask=mask;
        unsigned char raw[sizeof(actual)];memcpy(raw,&actual,sizeof(actual));
        EffectBodyRecordCensus a={16,2,tail},b=a;
        CHECK(effect_body_record_count(&context,&source,&actual,&a)==XR_XIR_OK &&
            literal_census_original(&context,&source,&expected,&b)==XR_XIR_OK);
        CHECK(!memcmp(&a,&b,sizeof(a)) && !memcmp(&actual,raw,sizeof(actual)));
        if(mask)CHECK(expected.mask==EFFECT_BODY_LITERAL_ROWS && conditional_compact_same(&expected.literal_tail[0],&values[17]));
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
/* The discarded tail is not current Source. Both gates must use the actual
 * second row for a converted pair, including every physical mask. */
static void literal_census_current_pair(void) {
    RootParameterMark held=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    XrXirInstruction values[32];XrXirFunction source;EffectBodyRun input;literal_census_input(values,&source,&input,2);
    size_t attempts=rp_attempts;
    for(uint32_t mask=1;mask<=EFFECT_BODY_MASK;++mask){EffectBodyRun raw=input;
        memset(&raw.literal_tail,0xCE,sizeof(raw.literal_tail));raw.mask=mask;
        for(unsigned fault=0;fault<5;++fault){EffectBodyRun run=raw,old=raw;EffectBodyRecordCensus a={16,2,3},b=a,unchanged=a;
            values[17].op=input.literal_tail[0].op;
            if(fault==1 || fault==2)values[17].op=fault==1?XR_XIR_INVALID:XR_XIR_OP_COUNT;
            else if(fault==3 || fault==4)run.base.op=old.base.op=fault==3?XR_XIR_INVALID:XR_XIR_OP_COUNT;
            unsigned char before[sizeof(run)];memcpy(before,&run,sizeof(run));
            XrXirStatus expected=literal_census_original(&context,&source,&old,&b);
            CHECK(effect_body_record_count(&context,&source,&run,&a)==expected && expected==(fault?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
            CHECK(!memcmp(&run,before,sizeof(run)) && !memcmp(&a,&b,sizeof(a)));
            if(fault)CHECK(!memcmp(&a,&unchanged,sizeof(a)));
        }
    }
    CHECK(rp_attempts==attempts);rp_owner_free(&context,live);rp_balanced(held);
}
static XrXirInstruction *literal_census_current_source;
static uint32_t literal_census_current_row,literal_census_current_word,literal_census_current_allocations;
static void literal_census_change_word(XrXirInstruction *value,uint32_t word) {
    if(!word)value->op=XR_XIR_CONST_BOOL;
    else if(word==1)value->type^=UINT32_C(0x80000001);
    else if(word==2 || word==3)value->args[word-2]^=UINT32_C(0x80000001);
    else if(word==4 || word==5)value->targets[word-4]^=UINT32_C(0x80000001);
    else if(word==6 || word==7){uint64_t bits;memcpy(&bits,&value->immediate,sizeof(bits));
        bits^=UINT64_C(0x80000001)<<((word-6)*32);memcpy(&value->immediate,&bits,sizeof(bits));}
    else value->type_arguments[word-8]^=UINT32_C(0x80000001);
}
static void literal_census_change_after_count(void *pointer) {
    (void)pointer;CHECK(literal_census_current_source && !literal_census_current_allocations++);
    literal_census_change_word(&literal_census_current_source[literal_census_current_row],literal_census_current_word);
}
/* Deliberate source exclusion violations between the passes still require
 * a complete fresh read; capacity is never a stale payload or proof. */
static void literal_census_current_receiving(void) {
    for(uint32_t row=0;row<17;++row)for(uint32_t word=0;word<10;++word){RootParameterMark held=rp_mark();
        XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        XrXirInstruction values[17],expected[17];literal_block_values(values);memcpy(expected,values,sizeof(expected));
        literal_census_change_word(&expected[row],word);XrXirFunction source={.instructions=values,.instruction_count=17};
        EffectBodyFunction body={.instruction_count=17};CHECK(!rp_allocation_observer);
        literal_census_current_source=values;literal_census_current_row=row;literal_census_current_word=word;literal_census_current_allocations=0;
        rp_allocation_observer=literal_census_change_after_count;XrXirStatus status=effect_body_records_copy(&context,&source,&body);
        rp_allocation_observer=NULL;literal_census_current_source=NULL;
        CHECK(status==XR_XIR_OK && literal_census_current_allocations==1 && body.run_count==3 && body.run_capacity==3);
        CHECK(!memcmp(values,expected,sizeof(values)));memset(values,0xCE,sizeof(values));
        EffectBodyRead read=effect_body_compile_read(&context);XrXirFunction oracle={.instructions=expected,.instruction_count=17};
        CHECK(effect_body_records_match(&read,&body,&oracle)==XR_XIR_OK);
        for(uint32_t i=0;i<17;++i){XrXirInstruction actual;
            CHECK(effect_body_instruction_get(&read,&body,i,&actual)==XR_XIR_OK && conditional_compact_same(&actual,&expected[i]));}
        xr_compile_resources_free(body.runs);rp_owner_free(&context,live);rp_balanced(held);
    }
}
static void snapshot_literal_census_controls(void) {
    literal_census_masks_and_guards();literal_census_current_pair();literal_census_current_receiving();uint64_t cuts=0;
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
    fprintf(stderr,"Literal census: original row-loop oracle for all72 tail/length pairs and all1024 masks at nine tails, complete raw input unchanged and original prepared-row oracle, stable uniform boundary, twelve structure and sixteen current opcode refusals, no allocation and atomic census at every work unit across%llu cuts with same finite-owner retry; all7 genuine Checked/Cell/Class bodies after producer death and independent publication/Class, six axes%llu/%llu/%llu exact/minusone physical0 PASS\n",
        (unsigned long long)cuts,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
    fprintf(stderr,"Census pair view: same shared shape and literal opcode kernels, all1023 changed masks prefer current second Source row over poisoned discarded tail, both invalid and upper current opcodes plus base faults, complete raw run/Source bytes read-only on success and every refusal; all170 after-census mutations across17 rows and ten fields observed by exact receiving and full word oracle after producer death, original changed-count/bad-opcode/semantic callback/EOF refusals retained PASS\n");
}
#endif // XIR_SNAPSHOT_LITERAL_CENSUS_CASES_H
