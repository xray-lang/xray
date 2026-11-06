/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_owned_material_cases.h - Owned inference after real producer death
 *
 * KEY CONCEPT:
 *   The genuine sealed certificate owns every CFG input and authority proof.
 */
#ifndef XIR_ERROR_OWNED_MATERIAL_CASES_H
#define XIR_ERROR_OWNED_MATERIAL_CASES_H
#include "xir_error_cell_index_fixture.h"

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
    XrXirInstruction expected={0};char name[64]={0};uint32_t name_length=0;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    if (status==XR_XIR_OK) {
        const XrXirModule *source=xr_xir_compile_artifact_module(checked);
        CHECK(source->function_count==5 && source->functions[2].instruction_count);
        expected=source->functions[2].instructions[0];name_length=source->functions[2].name_length;
        CHECK(name_length<sizeof(name));
        if (name_length) {
            CHECK(source->functions[2].name);memcpy(name,source->functions[2].name,name_length);
        }
        if (oracle) owned_error_source_controls(context,effects->invocations->body,source);
    }
    xr_xir_compile_artifact_free(checked);checked=NULL;memset(&fixture,0xCE,sizeof(fixture));
    if (status==XR_XIR_OK) {
        const EffectBodyMaterial *body=effects->invocations->body;
        CHECK(effects->count==5 && effect_body_material_shape(body) &&
            effects->invocations->final->material && body->resources==context->resources);
        EffectBodyRead read={.owner=context,.execution=NULL};XrXirInstruction actual;
        CHECK(body->functions[2].name_length==name_length &&
            (!name_length || !memcmp(body->functions[2].name,name,name_length)) &&
            effect_body_instruction_get(&read,&body->functions[2],0,&actual)==XR_XIR_OK &&
            conditional_compact_same(&actual,&expected));
    }
    xr_xir_compile_effects_free(effects);return status;
}

/* Each scan starts at physical zero. The independent literal rows retain
 * unused words, signed high bits and both nonconstant affine runs. */
static void owned_error_cursor_values(const XrXirCompileContext *context,
    const EffectBodyFunction *body,const XrXirInstruction *expected) {
    EffectBodyRead read=(EffectBodyRead){.owner=context,.execution=NULL};

    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    for (uint32_t i=0;i<body->instruction_count;++i) {
        XrXirInstruction decoded;
        CHECK(effect_body_instruction_cursor_get(&read,body,i,&cursor,&decoded)==XR_XIR_OK &&
            conditional_compact_same(&decoded,&expected[i]));
    }
    cursor=(EffectBodyRecordCursor){0,0,UINT32_MAX};
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
    cursor=(EffectBodyRecordCursor){1,0,UINT32_MAX};
    CHECK(effect_body_instruction_cursor_get(&read,body,0,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        conditional_compact_same(&out,&sentinel));
}

static void owned_error_cursor_malformed(const XrXirCompileContext *context,
    EffectBodyFunction *body,const XrXirFunction *expected) {
    EffectBodyRead read=(EffectBodyRead){.owner=context,.execution=NULL};
    CHECK(body->runs && body->run_count==2 && body->run_capacity==2);
    EffectBodyRun *first=&body->runs[0],*second=&body->runs[1];
    uint32_t column=0;while (!(first->mask&(UINT32_C(1)<<column))) ++column;
    uint32_t begin=first->first,count=first->count,mask=first->mask,delta=first->delta[column];
    uint32_t neighbor=second->first,runs=body->run_count,capacity=body->run_capacity;
    for (uint32_t mode=0;mode<9;++mode) {
        if (!mode) first->first=1;
        else if (mode==1) first->count=0;
        else if (mode==2) first->mask|=UINT32_C(1024);
        else if (mode==3) body->run_count=1;
        else if (mode==4) body->run_count=0;
        else if (mode==5) body->run_capacity=body->run_count-1;
        else if (mode==6) first->delta[column]=0;
        else if (mode==7) second->first=begin+count+1;
        else --body->instruction_count;

        EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
        XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_I64,.immediate=INT64_MIN},out=sentinel;
        CHECK(effect_body_instruction_cursor_get(&read,body,4,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
            conditional_compact_same(&out,&sentinel));
        cursor=(EffectBodyRecordCursor){0,0,UINT32_MAX};
        EffectBodyInstructionFields fields_sentinel={XR_XIR_RETURN,XR_XIR_BOOL,UINT32_MAX},fields=fields_sentinel;
        CHECK(effect_body_instruction_cursor_fields(&read,body,4,&cursor,&fields)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&fields,&fields_sentinel,sizeof(fields)));

        EffectBodyRecordCursor range={0,0,UINT32_MAX},range_before=range;
        const EffectBodyRun *ref_sentinel=(const EffectBodyRun *)(uintptr_t)1,*ref_output=ref_sentinel;
        CHECK(effect_body_range_ref(&read,body,&range,&ref_output)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&range,&range_before,sizeof(range)) && ref_output==ref_sentinel);
        first->first=begin;first->count=count;first->mask=mask;first->delta[column]=delta;
        second->first=neighbor;body->run_count=runs;body->run_capacity=capacity;body->instruction_count=8;
        CHECK(effect_body_records_match(&read,body,expected)==XR_XIR_OK);
    }

    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};XrXirInstruction out;
    CHECK(effect_body_instruction_cursor_get(&read,body,0,&cursor,&out)==XR_XIR_OK);
    second->first=begin+count+1;
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_I64,.immediate=INT64_MIN};out=sentinel;
    CHECK(effect_body_instruction_cursor_get(&read,body,1,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        conditional_compact_same(&out,&sentinel));
    second->first=neighbor;
}

/* A complete range walk is independent of semantic membership. Full rows
 * retain unused words; the narrow projection returns only actual COPY sources. */
static void owned_error_range_values(const XrXirCompileContext *context,
    const EffectBodyFunction *body,const XrXirInstruction *expected) {
    EffectBodyRead read=(EffectBodyRead){.owner=context,.execution=NULL};

    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    for (uint32_t run=0;run<body->run_count;++run) {
        const EffectBodyRun *ref=NULL;
        CHECK(effect_body_range_ref(&read,body,&cursor,&ref)==XR_XIR_OK);
        for (uint32_t i=ref->first;i-ref->first<ref->count;++i) {
            uint32_t words[EFFECT_BODY_WORDS];XrXirInstruction decoded;
            CHECK(effect_body_instruction_ref_words(&read,ref,i,words)==XR_XIR_OK &&
                effect_body_instruction_words_publish(&read,words,&decoded)==XR_XIR_OK &&
                conditional_compact_same(&decoded,&expected[i]));
            CHECK(effect_body_instruction_ref_value(&read,ref,i,&decoded)==XR_XIR_OK &&
                conditional_compact_same(&decoded,&expected[i]));
        }
    }
    CHECK(cursor.first==body->instruction_count && cursor.run==body->run_count);
    const EffectBodyRun *sentinel=(const EffectBodyRun *)(uintptr_t)1,*output=sentinel;
    EffectBodyRecordCursor saved=cursor;
    CHECK(effect_body_range_ref(&read,body,&cursor,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&saved,sizeof(cursor)) && !memcmp(&output,&sentinel,sizeof(output)));
}

/* Malformed projection inputs fail before publishing the destination. A
 * canonical reader still owns stream length, adjacency and complete EOF. */
static void owned_error_direct_ref_controls(const XrXirCompileContext *context,
    const EffectBodyFunction *body) {
    EffectBodyRead read=(EffectBodyRead){.owner=context,.execution=NULL};

    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};const EffectBodyRun *ref=NULL;
    CHECK(effect_body_range_ref(&read,body,&cursor,&ref)==XR_XIR_OK);
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.args={17,19},
        .targets={23,29},.immediate=INT64_MIN,.type_arguments={31,37}};
    for (uint32_t mode=0;mode<8;++mode) {

        EffectBodyRun bad;bad.first=ref->first;bad.count=ref->count;bad.mask=ref->mask;
        bad.base=ref->base;
        for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) if (ref->mask&(UINT32_C(1)<<w)) {
            bad.delta[w]=ref->delta[w];
        }
        uint32_t index=ref->first;const EffectBodyRun *input=&bad;XrXirInstruction output=sentinel;
        if (!mode) input=NULL;
        else if (mode==1) { bad.count=1;bad.mask=1; }
        else if (mode==2) bad.mask|=UINT32_C(1024);
        else if (mode==3) bad.count=0;
        else if (mode==4) index=UINT32_MAX;
        else if (mode==5) index=ref->first+ref->count;
        else if (mode==6) { bad.base.op=XR_XIR_OP_COUNT;bad.mask&=~UINT32_C(1); }
        else { bad.mask|=UINT32_C(1);bad.delta[0]=0; }
        size_t attempts=rp_attempts;
        CHECK(effect_body_instruction_ref_value(&read,input,index,&output)==XR_XIR_BAD_STRUCTURE &&
            conditional_compact_same(&output,&sentinel));
        if (mode<6) CHECK(rp_attempts==attempts);
    }
}

/* Two complete storage literals distinguish a constant run from one whose
 * ten physical columns all vary. Neither encoding is a semantic admission. */
static void owned_error_direct_fields(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).held_bytes;
    for (uint32_t mode=0;mode<2;++mode) {
        const XrXirInstruction first={.op=XR_XIR_COPY,.type=XR_XIR_I64,
            .args={UINT32_MAX,19},.targets={23,UINT32_MAX-29},
            .immediate=INT64_MIN+INT64_C(17),.type_arguments={31,37}};
        const XrXirInstruction second={.op=XR_XIR_SCALAR_COPY,.type=XR_XIR_F64,
            .args={17,UINT32_MAX-19},.targets={UINT32_MAX-23,29},
            .immediate=INT64_MAX-INT64_C(41),.type_arguments={UINT32_MAX-31,UINT32_MAX-37}};

        XrXirInstruction original[2]={first,mode?second:first},producer[2];
        memcpy(producer,original,sizeof(producer));
        XrXirFunction input={.instructions=producer,.instruction_count=2};
        EffectBodyFunction body={.instruction_count=2};
        CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_OK && body.run_count==1 &&
            body.runs[0].mask==(mode?(uint32_t)EFFECT_BODY_MASK:UINT32_C(0)));
        memset(producer,0xCE,sizeof(producer));
        owned_error_range_values(&context,&body,original);
        owned_error_direct_ref_controls(&context,&body);input.instructions=original;
        EffectBodyRead read=(EffectBodyRead){.owner=&context,.execution=NULL};
        for (uint32_t field=0;field<EFFECT_BODY_WORDS;++field) {

            XrXirInstruction saved=original[1];conditional_compact_change(&original[1],field);
            CHECK(effect_body_records_match(&read,&body,&input)==XR_XIR_BAD_STRUCTURE);
            original[1]=saved;CHECK(effect_body_records_match(&read,&body,&input)==XR_XIR_OK);
        }
        effect_body_runs_release(body.runs);CHECK(rp_stats(&context).held_bytes==baseline);
    }
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

/* Storage-only COPY literals keep five independent source identities. They
 * do not claim complete IR admission or reuse Error facts from a producer. */
static void owned_error_range_copies(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).held_bytes;
    const XrXirOp kinds[5]={XR_XIR_COPY,XR_XIR_SCALAR_COPY,XR_XIR_OWNED_RETAIN,
        XR_XIR_ERROR_ERASE,XR_XIR_ERROR_NARROW};
    XrXirInstruction original[5],producer[5];
    for (uint32_t i=0;i<5;++i) original[i]=(XrXirInstruction){.op=kinds[i],.type=XR_XIR_ERROR,
        .args={19+i,29+i},.targets={39+i,49+i},.immediate=INT64_MIN+(int64_t)i,
        .type_arguments={59+i,69+i}};
    memcpy(producer,original,sizeof(producer));
    XrXirFunction input={.instructions=producer,.instruction_count=5};
    EffectBodyFunction body={.instruction_count=5};
    CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_OK);
    memset(producer,0xCE,sizeof(producer));
    owned_error_range_values(&context,&body,original);

    effect_body_runs_release(body.runs);rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void conditional_owned_error_cursor_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).held_bytes;
    XrXirInstruction original[8],producer[8];conditional_compact_rows(original);
    memcpy(producer,original,sizeof(producer));
    XrXirFunction input={.instructions=producer,.instruction_count=8};
    XrXirFunction expected={.instructions=original,.instruction_count=8};
    EffectBodyFunction body={.instruction_count=8};
    CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_OK);
    memset(producer,0xCE,sizeof(producer));
    owned_error_cursor_values(&context,&body,original);
    owned_error_range_values(&context,&body,original);

    owned_error_cursor_malformed(&context,&body,&expected);
    EffectBodyRead read=(EffectBodyRead){.owner=&context,.execution=NULL};
    for (uint32_t field=0;field<10;++field) {
        XrXirInstruction saved=original[2];conditional_compact_change(&original[2],field);
        CHECK(effect_body_records_match(&read,&body,&expected)==XR_XIR_BAD_STRUCTURE);
        original[2]=saved;CHECK(effect_body_records_match(&read,&body,&expected)==XR_XIR_OK);
    }
    effect_body_runs_release(body.runs);rp_owner_free(&context,baseline);rp_balanced(physical);
}

/* Mutations are independent of the fixture's classification. A custom reader
 * remains observable even after the canonical inventory has been constructed. */

/* This genuine checked library retains a Cell formal, a new Cell and its COPY
 * alias. The two literals distinguish an immutable snapshot from invalidation. */
static XrXirStatus conditional_owned_error_cells(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    CellIndexFixture fixture;ci_fixture(&fixture,CI_CALL,mode!=0,false);
    const XrXirInstruction expected=fixture.subject[1];
    XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_verify(context,&fixture.module,NULL);
    if (status==XR_XIR_OK) status=conditional_effects_from_raw(context,&fixture.module,&effects);
    if (status==XR_XIR_OK && oracle) {
        CHECK(effect_body_source_match(context,effects->invocations->body,&fixture.module)==XR_XIR_OK);
        XrXirInstruction saved=fixture.subject[1];fixture.subject[1].type=XR_XIR_I64;
        CHECK(effect_body_source_match(context,effects->invocations->body,&fixture.module)==XR_XIR_BAD_STRUCTURE);
        fixture.subject[1]=saved;
        CHECK(effect_body_source_match(context,effects->invocations->body,&fixture.module)==XR_XIR_OK);
    }
    memset(&fixture,0xCE,sizeof(fixture));
    if (status==XR_XIR_OK) {
        const EffectBodyMaterial *body=effects->invocations->body;
        CHECK(effects->count==3 && effect_body_material_shape(body) &&
            effects->invocations->final->material && body->resources==context->resources);
        EffectBodyRead read={.owner=context,.execution=NULL};XrXirInstruction actual;
        CHECK(effect_body_instruction_get(&read,&body->functions[1],1,&actual)==XR_XIR_OK &&
            conditional_compact_same(&actual,&expected));
    }
    xr_xir_compile_effects_free(effects);return status;
}

/* The original two recipes retain their complete loops unchanged. These two
 * new Cell recipes measure their own real census, retry and three exact axes. */
static void conditional_owned_error_cell_resources(void) {
    for (uint32_t mode=0;mode<2;++mode) {
        size_t sites=0;XrCompileResourceStats census={0},entry={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());entry=rp_stats(&context);
            RootParameterMark retained=rp_mark();size_t first=rp_attempts;
            rp_fail_at=pass?first+pass-1:SIZE_MAX;
            XrXirStatus status=conditional_owned_error_cells(&context,mode,false);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts-first;census=rp_stats(&context);CHECK(sites);
                printf("OWNED_ERROR_CELL_CENSUS mode=%u sites=%zu allocations=%llu bytes=%llu held=%llu peak=%llu\n",
                    mode,sites,(unsigned long long)census.allocation_count,
                    (unsigned long long)census.allocated_bytes,(unsigned long long)census.held_bytes,
                    (unsigned long long)census.peak_bytes);
            } else {
                CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts-first==pass);
                CHECK(rp_stats(&context).held_bytes==entry.held_bytes);rp_balanced(retained);
                XrCompileResourceStats consumed=rp_stats(&context);
                uint64_t bytes=census.allocated_bytes-entry.allocated_bytes;
                bool fits=census.peak_bytes<=context.resources->limits.memory_limit;
                rp_fail_at=SIZE_MAX;size_t retry=rp_attempts;
                status=conditional_owned_error_cells(&context,mode,false);
                CHECK(status==(fits?XR_XIR_OK:XR_XIR_MEMORY_LIMIT));
                CHECK(rp_stats(&context).held_bytes==entry.held_bytes);rp_balanced(retained);
                if (fits) CHECK(rp_stats(&context).allocated_bytes-consumed.allocated_bytes==bytes &&
                    rp_stats(&context).allocation_count-consumed.allocation_count==census.allocation_count-entry.allocation_count &&
                    rp_attempts-retry==sites);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,entry.held_bytes);rp_balanced(physical);
        }

        for (uint32_t minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=rp_caps();uint64_t limit=census.peak_bytes-minus;
            limits.memory_limit=limit;
            RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(limits);
            uint64_t baseline=rp_stats(&context).held_bytes;
            CHECK(conditional_owned_error_cells(&context,mode,false)==(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
            rp_owner_free(&context,baseline);rp_balanced(physical);
        }
    }
}

static void conditional_owned_error_literals(void) {
    owned_error_direct_fields();
    conditional_owned_error_cursor_literals();
    owned_error_range_copies();
    for (uint32_t mode=0;mode<2;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).held_bytes;
        CHECK(conditional_owned_error(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    for (uint32_t mode=0;mode<2;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).held_bytes;
        CHECK(conditional_owned_error_cells(&context,mode,true)==XR_XIR_OK);
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
                printf("OWNED_ERROR_CENSUS mode=%u sites=%zu allocations=%llu bytes=%llu held=%llu peak=%llu\n",
                    mode,sites,(unsigned long long)census.allocation_count,
                    (unsigned long long)census.allocated_bytes,(unsigned long long)census.held_bytes,
                    (unsigned long long)census.peak_bytes);
            } else {
                CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts-first==pass);
                CHECK(rp_stats(&context).held_bytes==entry.held_bytes);rp_balanced(retained);
                XrCompileResourceStats consumed=rp_stats(&context);
                uint64_t bytes=census.allocated_bytes-entry.allocated_bytes;
                bool fits=census.peak_bytes<=context.resources->limits.memory_limit;
                rp_fail_at=SIZE_MAX;size_t retry=rp_attempts;
                status=conditional_owned_error(&context,mode,false);
                CHECK(status==(fits?XR_XIR_OK:XR_XIR_MEMORY_LIMIT));
                CHECK(rp_stats(&context).held_bytes==entry.held_bytes);rp_balanced(retained);
                if (fits) CHECK(rp_stats(&context).allocated_bytes-consumed.allocated_bytes==bytes &&
                    rp_stats(&context).allocation_count-consumed.allocation_count==census.allocation_count-entry.allocation_count &&
                    rp_attempts-retry==sites);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,entry.held_bytes);rp_balanced(physical);
        }

        for (uint32_t minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=rp_caps();uint64_t limit=census.peak_bytes-minus;
            limits.memory_limit=limit;
            RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(limits);
            uint64_t baseline=rp_stats(&context).held_bytes;
            CHECK(conditional_owned_error(&context,mode,false)==(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
            rp_owner_free(&context,baseline);rp_balanced(physical);
        }
    }
    conditional_owned_error_cell_resources();
}
#endif // XIR_ERROR_OWNED_MATERIAL_CASES_H
