/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_snapshot_literal_correspondence_cases.h - Complete physical literal matching
 */
#ifndef XIR_SNAPSHOT_LITERAL_CORRESPONDENCE_CASES_H
#define XIR_SNAPSHOT_LITERAL_CORRESPONDENCE_CASES_H
/* Freeze the complete original correspondence loop, including both physical
 * layouts, so the new dispatch cannot validate itself through its own loop. */
static XrXirStatus literal_correspondence_original(const EffectBodyRead *read,
    const EffectBodyFunction *function,const XrXirFunction *source,bool *same) {
    if (!function || !source || (source->instruction_count && !source->instructions) ||
        function->run_count>function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (function->instruction_count!=source->instruction_count) {
        if (!same) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
        *same=false;return XR_XIR_OK;
    }
    uint32_t first=0;
    for (uint32_t r=0;r<function->run_count;++r) {
        const EffectBodyRun *ref;
        XrXirStatus status=effect_body_run_ref(read,function,r,first,&ref);
        if (status!=XR_XIR_OK) return status;
        uint32_t column=UINT32_MAX;
        if (!effect_body_read_work(read,sizeof(column)+2)) return XR_XIR_BUDGET;
        if (ref->count>2 && !(ref->mask&EFFECT_BODY_LITERAL_ROWS)) {
            status=effect_body_single_column(read,ref->mask,&column);
            if (status!=XR_XIR_OK) return status;
        }
        if (column<=EFFECT_BODY_WORDS) {
            status=effect_body_column_match(read,ref,source,column,same);
            if (status!=XR_XIR_OK) return status;
        } else for (uint32_t i=first;i-first<ref->count;++i) {
            for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                bool delta=!!(ref->mask&(UINT32_C(1)<<w));
                bool pair=!!(ref->mask&EFFECT_BODY_LITERAL_ROWS);
                if (!effect_body_read_work(read,3+(pair?2:delta?4:0))) return XR_XIR_BUDGET;
                uint32_t actual=pair?xir_instruction_word(effect_body_literal_row(ref,i),w):
                    xir_instruction_word(&ref->base,w)+(delta?ref->delta[w]*(i-first):0);
                if (actual!=
                    xir_instruction_word(&source->instructions[i],w)) {
                    if (!same) return XR_XIR_BAD_STRUCTURE;
                    if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
                    *same=false;
                }
            }
        }
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        first+=ref->count;
    }
    return first==function->instruction_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

typedef struct LiteralCorrespondenceCase {
    XrXirInstruction rows[10];EffectBodyRun runs[3];
    EffectBodyFunction body;XrXirFunction source;
} LiteralCorrespondenceCase;
static void literal_correspondence_case(LiteralCorrespondenceCase *test,uint32_t count) {
    CHECK(count>=2 && count<=8);memset(test,0,sizeof(*test));
    for(uint32_t i=0;i<count+2;++i) {
        test->rows[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,
            .args={UINT32_MAX-i,13+7*i},.targets={31+11*i,UINT32_MAX-17*i},
            .immediate=INT64_MIN+7+UINT32_C(0x1234567)*i,.type_arguments={43+19*i,UINT32_MAX-23*i}};
    }
    test->runs[0]=(EffectBodyRun){.first=0,.count=1,.base=test->rows[0]};
    test->runs[1]=(EffectBodyRun){.first=1,.count=count,.mask=EFFECT_BODY_LITERAL_ROWS,.base=test->rows[1]};
    for(uint32_t i=1;i<count;++i)test->runs[1].literal_tail[i-1]=test->rows[i+1];
    test->runs[2]=(EffectBodyRun){.first=count+1,.count=1,.base=test->rows[count+1]};
    test->body=(EffectBodyFunction){.instruction_count=count+2,.run_count=3,.run_capacity=3,.runs=test->runs};
    test->source=(XrXirFunction){.instruction_count=count+2,.instructions=test->rows};
}
static uint64_t literal_correspondence_diff(XrXirCompileContext *context,LiteralCorrespondenceCase *test,
    unsigned mode,bool expected_same) {
    EffectBodyRead read=effect_body_compile_read(context);bool original=mode!=2,actual=original;
    bool *a=mode?&original:NULL,*b=mode?&actual:NULL;
    LiteralCorrespondenceCase before=*test;uint64_t entry=rp_stats(context).work;
    XrXirStatus old=literal_correspondence_original(&read,&test->body,&test->source,a);
    uint64_t old_work=rp_stats(context).work-entry;entry=rp_stats(context).work;
    XrXirStatus status=effect_body_records_compare(&read,&test->body,&test->source,b);
    uint64_t work=rp_stats(context).work-entry;
    CHECK(status==old && (!b || actual==original) && !memcmp(test,&before,sizeof(before)));
    if(status==XR_XIR_OK) {
        CHECK(!mode || actual==(mode==1 && expected_same));
        CHECK(old_work-work==20*test->runs[1].count-3);
    } else CHECK(!mode && !expected_same && status==XR_XIR_BAD_STRUCTURE);
    return work;
}
static uint64_t literal_correspondence_budget(uint32_t count,bool changed,uint64_t cut) {
    RootParameterMark held=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t live=rp_stats(&context).live_bytes;LiteralCorrespondenceCase test;
    literal_correspondence_case(&test,count);
    if(changed)conditional_compact_change(&test.rows[count],9);
    LiteralCorrespondenceCase before=test;EffectBodyRead read=effect_body_compile_read(&context);
    XrCompileResources *identity=context.resources;uint64_t entry=rp_stats(&context).work;
    size_t attempts=rp_attempts;bool same=true;
    if(cut!=UINT64_MAX)context.resources->limits.work=entry+cut;
    uint64_t limit=context.resources->limits.work;
    XrXirStatus status=effect_body_records_compare(&read,&test.body,&test.source,&same);
    uint64_t work=rp_stats(&context).work-entry;
    if(status==XR_XIR_OK)CHECK(same==!changed);
    else {
        CHECK(status==XR_XIR_BUDGET);bool prior=same;
        CHECK(effect_body_records_compare(&read,&test.body,&test.source,&same)==XR_XIR_BUDGET);
        CHECK(prior || !same);
    }
    CHECK(context.resources==identity && context.resources->limits.work==limit && rp_attempts==attempts);
    CHECK(!memcmp(&test,&before,sizeof(test)));rp_owner_free(&context,live);rp_balanced(held);
    return status==XR_XIR_OK?work:0;
}
static void literal_correspondence_shapes(XrXirCompileContext *context) {
    for(unsigned fault=0;fault<10;++fault) {
        LiteralCorrespondenceCase test;literal_correspondence_case(&test,8);
        switch(fault) {
            case 0:test.runs[1].mask|=1;break;
            case 1:test.runs[1].count=9;break;
            case 2:test.runs[1].first=2;break;
            case 3:test.runs[1].base.op=XR_XIR_INVALID;break;
            case 4:test.runs[1].literal_tail[6].op=XR_XIR_OP_COUNT;break;
            case 5:test.body.runs=NULL;break;
            case 6:test.body.run_count=11;break;
            case 7:test.source.instructions=NULL;break;
            case 8:test.runs[2].count=2;break;
            default:test.runs[1].count=1;break;
        }
        LiteralCorrespondenceCase before=test;EffectBodyRead read=effect_body_compile_read(context);
        bool old_same=true,same=true;
        CHECK(literal_correspondence_original(&read,&test.body,&test.source,&old_same)==XR_XIR_BAD_STRUCTURE);
        CHECK(effect_body_records_compare(&read,&test.body,&test.source,&same)==XR_XIR_BAD_STRUCTURE);
        CHECK(old_same==same && !memcmp(&test,&before,sizeof(test)));
    }
}
static void snapshot_literal_correspondence_controls(void) {
    RootParameterMark held=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t live=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;uint64_t mutations=0,cuts=0;
    for(uint32_t count=2;count<=8;++count) {
        LiteralCorrespondenceCase test;literal_correspondence_case(&test,count);
        for(unsigned mode=0;mode<3;++mode)CHECK(literal_correspondence_diff(&context,&test,mode,true));
        for(uint32_t i=0;i<count+2;++i)for(unsigned field=0;field<10;++field) {
            XrXirInstruction saved=test.rows[i];conditional_compact_change(&test.rows[i],field);
            for(unsigned mode=0;mode<3;++mode)CHECK(literal_correspondence_diff(&context,&test,mode,false));
            test.rows[i]=saved;++mutations;
        }
        conditional_compact_change(&test.rows[1],2);conditional_compact_change(&test.rows[count],9);
        for(unsigned mode=0;mode<3;++mode)CHECK(literal_correspondence_diff(&context,&test,mode,false));
    }
    literal_correspondence_shapes(&context);CHECK(rp_attempts==attempts);
    rp_owner_free(&context,live);rp_balanced(held);
    for(uint32_t count=2;count<=8;++count)for(unsigned changed=0;changed<2;++changed) {
        uint64_t work=literal_correspondence_budget(count,changed!=0,UINT64_MAX);CHECK(work);
        for(uint64_t cut=0;cut<=work;++cut) {
            uint64_t spent=literal_correspondence_budget(count,changed!=0,cut);
            CHECK(cut==work?spent==work:!spent);++cuts;
        }
    }
    fprintf(stderr,"Literal correspondence: frozen complete original kernel across all lengths2..8 with nonzero first and adjacent singleton records, all%llu physical word mutations in strict/true/initial-false modes, multiple differences and ten original shape/opcode refusals; every actual work unit across%llu cuts, same finite-owner retry without cap reset, input bytes unchanged and no allocation PASS\n",
        (unsigned long long)mutations,(unsigned long long)cuts);
}
#endif // XIR_SNAPSHOT_LITERAL_CORRESPONDENCE_CASES_H
