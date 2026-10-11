/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_single_column_cases.h - Complete physical column comparisons
 */
#ifndef XIR_INVOCATION_SINGLE_COLUMN_CASES_H
#define XIR_INVOCATION_SINGLE_COLUMN_CASES_H
/* A mismatch in the last physical word starts a new complete seed. These
 * storage-only values preserve nonzero unused words and signed high bits. */
static XrXirStatus conditional_compact_writer_boundary(const XrXirCompileContext *context,bool oracle) {
    XrXirInstruction expected[6],source[6];
    for (uint32_t i=0;i<6;++i) {
        uint64_t bits=UINT64_C(0x8000000000000000)+i;
        int64_t immediate;memcpy(&immediate,&bits,sizeof(immediate));
        expected[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,
            .args={UINT32_MAX,19},.targets={7,12},.immediate=immediate,
            .type_arguments={65536,i<3?65537u:65538u}};
    }
    memcpy(source,expected,sizeof(source));
    XrXirFunction input={.instructions=source,.instruction_count=6};
    XrXirFunction original={.instructions=expected,.instruction_count=6};
    EffectBodyFunction body={.instruction_count=6};
    XrXirStatus status=effect_body_records_copy(context,&input,&body);
    memset(source,0xCD,sizeof(source));EffectBodyRead read=effect_body_compile_read(context);
    if (status==XR_XIR_OK) status=effect_body_records_match(&read,&body,&original);
    for (uint32_t i=0;i<6 && status==XR_XIR_OK;++i) {
        XrXirInstruction decoded;status=effect_body_instruction_get(&read,&body,i,&decoded);
        if (status==XR_XIR_OK && oracle) CHECK(conditional_compact_same(&decoded,&expected[i]));
    }
    if (status==XR_XIR_OK && oracle) CHECK(body.run_count==2);
    xr_compile_resources_free(body.stream);
    if (status!=XR_XIR_OK || !oracle) return status;
    uint64_t held_bytes=rp_stats(context).live_bytes;
    const XrXirOp invalid[]={XR_XIR_INVALID,XR_XIR_OP_COUNT,(XrXirOp)UINT32_MAX};
    for (uint32_t i=0;i<4;++i) for (uint32_t bad=0;bad<3;++bad) {
        for (uint32_t row=0;row<4;++row)
            source[row]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=row};
        source[i].op=invalid[bad];input.instruction_count=4;
        body=(EffectBodyFunction){.instruction_count=4};size_t allocations=rp_attempts;
        CHECK(effect_body_records_copy(context,&input,&body)==XR_XIR_BAD_STRUCTURE &&
            !body.stream && !body.stream_words && !body.run_count && rp_attempts==allocations &&
            rp_stats(context).live_bytes==held_bytes);
    }
    /* A real affine opcode column can reach an invalid ordinal. Its actual
     * third value is checked even when every encoded delta predicts it. */
    for (uint32_t i=0;i<3;++i)
        source[i]=(XrXirInstruction){.op=(XrXirOp)(XR_XIR_OP_COUNT-2+i),.type=XR_XIR_I64};
    input.instruction_count=3;body=(EffectBodyFunction){.instruction_count=3};size_t allocations=rp_attempts;
    CHECK(effect_body_records_copy(context,&input,&body)==XR_XIR_BAD_STRUCTURE && !body.stream &&
        !body.stream_words && !body.run_count && rp_attempts==allocations && rp_stats(context).live_bytes==held_bytes);
    return XR_XIR_OK;
}

/* The two four-row runs have independent constant and one-delta models.
 * Unused fields remain nonzero and the exact signed immediate bits survive. */
static XrXirStatus conditional_compact_sparse_codec(const XrXirCompileContext *context,bool oracle) {
    XrXirStatus boundary=conditional_compact_writer_boundary(context,oracle);
    if (boundary!=XR_XIR_OK) return boundary;
    XrXirInstruction expected[8],source[8];
    for (uint32_t i=0;i<8;++i) {
        uint64_t bits=i<4?UINT64_C(0x8000000000000000):UINT64_C(0xfffffff000000003)+(i-4);
        int64_t immediate;memcpy(&immediate,&bits,sizeof(immediate));
        expected[i]=(XrXirInstruction){.op=i<4?XR_XIR_CONST_INT:XR_XIR_CONST_FLOAT,
            .type=i<4?XR_XIR_I64:XR_XIR_F64,.args={UINT32_MAX,19},.targets={7,12},
            .immediate=immediate,.type_arguments={65536,65537}};
    }
    memcpy(source,expected,sizeof(source));
    XrXirFunction input={.instructions=source,.instruction_count=8};
    XrXirFunction original={.instructions=expected,.instruction_count=8};
    EffectBodyFunction body={.instruction_count=8};
    XrXirStatus status=effect_body_records_copy(context,&input,&body);
    memset(source,0xCD,sizeof(source));EffectBodyRead read=effect_body_compile_read(context);
    if (status==XR_XIR_OK) status=effect_body_records_match(&read,&body,&original);
    for (uint32_t i=0;i<8 && status==XR_XIR_OK;++i) {
        XrXirInstruction decoded;status=effect_body_instruction_get(&read,&body,i,&decoded);
        if (status==XR_XIR_OK && oracle) CHECK(conditional_compact_same(&decoded,&expected[i]));
    }
    if (status==XR_XIR_OK && oracle) {
        CHECK(body.run_count==2);
        for (uint32_t i=2;i<=6;i+=4) for (uint32_t field=0;field<10;++field) {
            XrXirInstruction saved=expected[i];conditional_compact_change(&expected[i],field);
            CHECK(effect_body_records_match(&read,&body,&original)==XR_XIR_BAD_STRUCTURE);
            expected[i]=saved;CHECK(effect_body_records_match(&read,&body,&original)==XR_XIR_OK);
        }
        ConditionalCompactRead observation={context,0,UINT64_MAX};
        EffectBodyRead finite={&observation,conditional_compact_read_work};uint32_t column=123;
        CHECK(effect_body_single_column(&finite,UINT32_C(64),&column)==XR_XIR_OK && column==6);
        uint64_t points=observation.calls;
        for (uint64_t at=0;at<points;++at) {
            observation.calls=0;observation.reject=at;column=123;
            CHECK(effect_body_single_column(&finite,UINT32_C(64),&column)==XR_XIR_BUDGET &&
                column==123 && observation.calls==at+1);
        }
        observation.calls=0;observation.reject=UINT64_MAX;
        for (uint32_t word=0;word<10;++word) {
            column=123;CHECK(effect_body_single_column(&finite,UINT32_C(1)<<word,&column)==XR_XIR_OK && column==word);
        }
        column=123;CHECK(effect_body_single_column(&finite,0,&column)==XR_XIR_OK && column==10);
        column=123;CHECK(effect_body_single_column(&finite,3,&column)==XR_XIR_OK && column==UINT32_MAX);
        column=123;CHECK(effect_body_single_column(&finite,1024,&column)==XR_XIR_BAD_STRUCTURE && column==123);
        EffectBodyRunRef first,second;
        CHECK(effect_body_run_ref(&read,&body,0,0,&first)==XR_XIR_OK && first.count==4 && !first.mask);
        CHECK(effect_body_run_ref(&read,&body,first.next,4,&second)==XR_XIR_OK && second.count==4 && second.mask==64);
        CHECK(effect_body_column_match(&read,&first,&original,0,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(effect_body_column_match(&read,&second,&original,10,NULL)==XR_XIR_BAD_STRUCTURE);
        XrXirFunction short_source=original;short_source.instruction_count=7;
        CHECK(effect_body_column_match(&read,&second,&short_source,6,NULL)==XR_XIR_BAD_STRUCTURE);
        uint64_t held_bytes=rp_stats(context).live_bytes;
        observation.calls=0;observation.reject=UINT64_MAX;
        CHECK(effect_body_records_match(&finite,&body,&original)==XR_XIR_OK);
        points=observation.calls;
        for (uint64_t at=0;at<points;++at) {
            observation.calls=0;observation.reject=at;
            CHECK(effect_body_records_match(&finite,&body,&original)==XR_XIR_BUDGET &&
                observation.calls==at+1 && rp_stats(context).live_bytes==held_bytes);
        }
        observation.calls=0;observation.reject=UINT64_MAX;
        CHECK(effect_body_records_match(&finite,&body,&original)==XR_XIR_OK);
        /* Continuation comparison records data changes but still completes
         * every physical field and EOF. A later fee failure is never a match. */
        XrXirInstruction held=expected[2];conditional_compact_change(&expected[2],2);
        bool same=true;observation.calls=0;observation.reject=UINT64_MAX;
        CHECK(effect_body_records_compare(&finite,&body,&original,&same)==XR_XIR_OK && !same);
        points=observation.calls;CHECK(points>1);
        same=true;observation.calls=0;observation.reject=points-1;
        CHECK(effect_body_records_compare(&finite,&body,&original,&same)==XR_XIR_BUDGET &&
            !same && observation.calls==points);
        expected[2]=held;observation.calls=0;observation.reject=UINT64_MAX;same=true;
        CHECK(effect_body_records_compare(&finite,&body,&original,&same)==XR_XIR_OK && same);
        same=false;CHECK(effect_body_records_compare(&finite,&body,&original,&same)==XR_XIR_OK && !same);
        uint32_t stream_words=body.stream_words;--body.stream_words;same=true;
        CHECK(effect_body_records_compare(&finite,&body,&original,&same)==XR_XIR_BAD_STRUCTURE);
        body.stream_words=stream_words;
        CHECK(effect_body_records_match(&read,&body,&original)==XR_XIR_OK &&
            rp_stats(context).live_bytes==held_bytes);
    }
    xr_compile_resources_free(body.stream);return status;
}

#endif
