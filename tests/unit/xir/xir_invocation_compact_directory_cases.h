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

/* The oracle is an independent physical instruction array. Every subset of
 * the ten varying words is decoded after its source storage has been erased. */
static void conditional_compact_tiny_rows(XrXirInstruction *rows,uint32_t mask) {
    for (uint32_t i=0;i<3;++i) {
        uint32_t low=UINT32_C(0xfffffff0)+((mask&64)?i*17:0);
        uint32_t high=UINT32_C(0x80000000)+((mask&128)?i*3:0);
        uint64_t bits=((uint64_t)high<<32)|low;int64_t immediate;
        memcpy(&immediate,&bits,sizeof(immediate));
        rows[i]=(XrXirInstruction){.op=(XrXirOp)(XR_XIR_CONST_INT+((mask&1)?i:0)),
            .type=XR_XIR_I64+((mask&2)?i:0),
            .args={11+((mask&4)?i*3:0),UINT32_MAX-((mask&8)?i*5:0)},
            .targets={19+((mask&16)?i*7:0),31+((mask&32)?i*11:0)},
            .immediate=immediate,.type_arguments={43+((mask&256)?i*13:0),
                UINT32_MAX-((mask&512)?i*19:0)}};
    }
    rows[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,
        .args={997,991},.targets={983,977},.immediate=INT64_MIN,
        .type_arguments={971,967}};
}

/* Tiny directories must reject corruption anywhere in the complete local
 * directory, including the unselected second run. Output remains untouched. */
static void conditional_compact_tiny_damage(const XrXirCompileContext *context,
    EffectBodyFunction *body,const XrXirInstruction *expected) {
    EffectBodyRead read=effect_body_compile_read(context);
    for (uint32_t r=0;r<body->run_count;++r) {
        EffectBodyRun *run=&body->runs[r];
        uint32_t first=run->first,count=run->count,mask=run->mask;
        for (uint32_t damage=0;damage<5;++damage) {
            if (!damage) ++run->first;
            else if (damage==1) run->count=0;
            else if (damage==2) ++run->count;
            else if (damage==3) run->mask|=UINT32_C(1024);
            else if (run->count==1) run->mask=1;
            else continue;
            XrXirInstruction sentinel={.op=XR_XIR_RETURN,.immediate=INT64_MIN},out=sentinel;
            CHECK(effect_body_instruction_get(&read,body,0,&out)==XR_XIR_BAD_STRUCTURE &&
                conditional_compact_same(&out,&sentinel));
            run->first=first;run->count=count;run->mask=mask;
            CHECK(effect_body_instruction_get(&read,body,0,&out)==XR_XIR_OK &&
                conditional_compact_same(&out,&expected[0]));
        }
        for (uint32_t w=0;w<10;++w) if (run->mask&(UINT32_C(1)<<w)) {
            uint32_t delta=run->delta[w];run->delta[w]=0;
            XrXirInstruction sentinel={.op=XR_XIR_RETURN,.immediate=INT64_MIN},out=sentinel;
            CHECK(effect_body_instruction_get(&read,body,0,&out)==XR_XIR_BAD_STRUCTURE &&
                conditional_compact_same(&out,&sentinel));
            run->delta[w]=delta;
        }
    }
}

static void conditional_compact_tiny_masks(void) {
    _Static_assert(XR_XIR_CONST_INT+2<XR_XIR_OP_COUNT,"Physical test opcodes must be representable");
    for (uint32_t mask=0;mask<1024;++mask) for (uint32_t tail=0;tail<2;++tail) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        XrXirInstruction source[4],expected[4];conditional_compact_tiny_rows(source,mask);
        memcpy(expected,source,sizeof(expected));
        XrXirFunction raw={.instructions=source,.instruction_count=3+tail};
        EffectBodyFunction body={.instruction_count=raw.instruction_count};
        CHECK(effect_body_records_copy(&context,&raw,&body)==XR_XIR_OK &&
            body.run_count==1+tail && body.runs[0].mask==mask);
        memset(source,0xCE,sizeof(source));raw.instructions=expected;
        EffectBodyRead read=effect_body_compile_read(&context);
        for (uint32_t order=0;order<body.instruction_count;++order) {
            uint32_t i=body.instruction_count-order-1;XrXirInstruction out;
            CHECK(effect_body_instruction_get(&read,&body,i,&out)==XR_XIR_OK &&
                conditional_compact_same(&out,&expected[i]));
            EffectBodyInstructionFields fields;
            CHECK(effect_body_instruction_fields_get(&read,&body,i,&fields)==XR_XIR_OK &&
                fields.op==expected[i].op && fields.type==expected[i].type && fields.source==expected[i].args[0]);
            conditional_compact_seed_opcode(&context,&body,i,expected[i].op);
        }
        CHECK(effect_body_records_match(&read,&body,&raw)==XR_XIR_OK);
        conditional_compact_tiny_damage(&context,&body,expected);
        conditional_compact_read_failures(&context,&body,1,&expected[1]);
        conditional_compact_fields_failures(&context,&body,1,&expected[1]);
        xr_compile_resources_free(body.runs);rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    puts("compact tiny directories: all 1024 physical masks, one/two runs, damage and every read refusal passed");
    puts("seed opcode: all 1024 masks, erased producer, unused delta damage and every refusal passed");
}

static void conditional_compact_directory_literals(void) {
    conditional_compact_tiny_masks();
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
        xr_compile_resources_free(body.runs);rp_owner_free(&context,held);rp_balanced(physical);
    }
}
#endif // XIR_INVOCATION_COMPACT_DIRECTORY_CASES_H
