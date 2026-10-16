/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_compact_directory_cases.h - Actual multi-level ordinal directory
 */
#ifndef XIR_INVOCATION_COMPACT_DIRECTORY_CASES_H
#define XIR_INVOCATION_COMPACT_DIRECTORY_CASES_H

/* Alternating real affine groups produce an odd directory through the sole
 * writer. Expected words come from this independent physical source array. */
static void conditional_compact_directory_rows(XrXirInstruction *source,uint32_t count) {
    for (uint32_t i=0;i<count;++i) {
        uint32_t group=i/3,offset=i%3;
        uint64_t bits=UINT64_C(0x8000000000000000)+(uint64_t)group*128+offset*2;
        int64_t immediate;memcpy(&immediate,&bits,sizeof(immediate));
        source[i]=(XrXirInstruction){.op=group%2?XR_XIR_CONST_FLOAT:XR_XIR_CONST_INT,
            .type=group%2?XR_XIR_F64:XR_XIR_I64,.args={37+group+offset,UINT32_MAX-offset},
            .targets={group+offset*3,19+offset},.immediate=immediate,
            .type_arguments={group+offset*5,UINT32_MAX-offset*7}};
    }
}

/* Every real ordinal is visited in a nonmonotonic permutation. First and
 * last rows of all groups are therefore full ten-field receiving cases. */
static void conditional_compact_directory_values(const XrXirCompileContext *context,
    const EffectBodyFunction *body,const XrXirInstruction *expected) {
    EffectBodyRead read=effect_body_compile_read(context);
    for (uint32_t order=0;order<body->instruction_count;++order) {
        uint32_t i=(order*2+7)%body->instruction_count;
        XrXirInstruction decoded;
        CHECK(effect_body_instruction_get(&read,body,i,&decoded)==XR_XIR_OK &&
            conditional_compact_same(&decoded,&expected[i]));
        EffectBodyInstructionFields fields;
        CHECK(effect_body_instruction_fields_get(&read,body,i,&fields)==XR_XIR_OK &&
            fields.op==expected[i].op && fields.type==expected[i].type && fields.source==expected[i].args[0]);
    }
    conditional_compact_read_failures(context,body,body->instruction_count-2,&expected[body->instruction_count-2]);
    conditional_compact_fields_failures(context,body,body->instruction_count-2,&expected[body->instruction_count-2]);
}

/* Local projection is not complete semantic admission. An unexplored damaged
 * internal group is always rejected by the full current source correspondence. */
static void conditional_compact_directory_damage(const XrXirCompileContext *context,
    EffectBodyFunction *body,const XrXirFunction *expected) {
    EffectBodyRead read=effect_body_compile_read(context);
    uint32_t index=body->run_count/2;EffectBodyRun *middle=&body->runs[index];
    uint32_t first=middle->first,count=middle->count,mask=middle->mask;
    uint32_t word=0;while (!(mask&(UINT32_C(1)<<word))) ++word;
    uint32_t delta=middle->delta[word];
    for (uint32_t damage=0;damage<4;++damage) {
        if (!damage) middle->first=first+1;
        else if (damage==1) middle->count=0;
        else if (damage==2) middle->mask=mask|UINT32_C(1024);
        else middle->delta[word]=0;
        CHECK(effect_body_records_match(&read,body,expected)==XR_XIR_BAD_STRUCTURE);
        XrXirInstruction sentinel={.op=XR_XIR_RETURN,.immediate=INT64_MIN},decoded=sentinel;
        XrXirStatus status=effect_body_instruction_get(&read,body,expected->instruction_count-1,&decoded);
        CHECK((status==XR_XIR_OK && conditional_compact_same(&decoded,
            &expected->instructions[expected->instruction_count-1])) ||
            (status==XR_XIR_BAD_STRUCTURE && conditional_compact_same(&decoded,&sentinel)));
        middle->first=first;middle->count=count;middle->mask=mask;middle->delta[word]=delta;
        CHECK(effect_body_records_match(&read,body,expected)==XR_XIR_OK);
    }
    /* A bad first, exact tail or selected adjacent group is a local refusal. */
    const uint32_t selected[3]={0,body->run_count-1,body->run_count/2};
    for (uint32_t at=0;at<3;++at) {
        EffectBodyRun *run=&body->runs[selected[at]];uint32_t saved=run->count;
        run->count=0;
        XrXirInstruction sentinel={.op=XR_XIR_RETURN,.immediate=INT64_MIN},decoded=sentinel;
        CHECK(effect_body_instruction_get(&read,body,middle->first,&decoded)==XR_XIR_BAD_STRUCTURE &&
            conditional_compact_same(&decoded,&sentinel));
        run->count=saved;CHECK(effect_body_records_match(&read,body,expected)==XR_XIR_OK);
    }
}

static void conditional_compact_directory_literals(void) {
    const uint32_t sizes[3]={5,9,17};
    for (uint32_t mode=0;mode<3;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t held=rp_stats(&context).live_bytes;uint32_t count=sizes[mode]*3;
        XrXirInstruction source[51],expected[51];
        conditional_compact_directory_rows(source,count);memcpy(expected,source,count*sizeof(*source));
        XrXirFunction raw={.instructions=source,.instruction_count=count};
        EffectBodyFunction body={.instruction_count=count};
        CHECK(effect_body_records_copy(&context,&raw,&body)==XR_XIR_OK && body.run_count==sizes[mode]);
        memset(source,0xCE,sizeof(source));raw.instructions=expected;
        conditional_compact_directory_values(&context,&body,expected);
        conditional_compact_directory_damage(&context,&body,&raw);
        effect_body_runs_release(body.runs);rp_owner_free(&context,held);rp_balanced(physical);
    }
}
#endif // XIR_INVOCATION_COMPACT_DIRECTORY_CASES_H
