/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_literal_body_cases.h - Sole physical literal storage and fresh readers
 */
#ifndef XIR_LITERAL_BODY_CASES_H
#define XIR_LITERAL_BODY_CASES_H
enum { LITERAL_BODY_COUNT=34 };
typedef struct LiteralBodySink { uint32_t next; } LiteralBodySink;
static XrXirInstruction literal_body_row(uint32_t i,bool literal) {
    return (XrXirInstruction){.op=literal && (i&1)?XR_XIR_CONST_BOOL:XR_XIR_CONST_INT,
        .type=literal && (i&1)?XR_XIR_BOOL:XR_XIR_I64,
        .args={UINT32_C(100003)+i*17,UINT32_MAX-i*3},.targets={i*11,1234+i*13},
        .immediate=INT64_MIN+(int64_t)i*INT64_C(4294967297),.type_arguments={i+300,i*7+900}};
}
static XrXirStatus literal_body_sink_row(void *owner,uint32_t i,const XrXirInstruction *op) {
    LiteralBodySink *sink=owner;
    CHECK(op && i==sink->next);++sink->next;return XR_XIR_OK;
}
static XrXirStatus literal_body_sink_span(void *owner,uint32_t first,uint32_t count) {
    LiteralBodySink *sink=owner;CHECK(first==sink->next && count);sink->next+=count;return XR_XIR_OK;
}
/* Unused words deliberately make these storage inputs unsuitable as program
 * admission. Independent field literals establish copying and decoding only. */
static XrXirStatus literal_body_roundtrip(const XrXirCompileContext *context,bool literal,bool boundaries) {
    XrXirInstruction rows[LITERAL_BODY_COUNT];
    for (uint32_t i=0;i<LITERAL_BODY_COUNT;++i) rows[i]=literal_body_row(i,literal);
    XrXirFunction source={.instructions=rows,.instruction_count=LITERAL_BODY_COUNT};
    EffectBodyFunction function={.instruction_count=LITERAL_BODY_COUNT};
    EffectBodyMaterial material={.resources=context->resources,.references=1,.functions=&function,
        .terms={.remaining=context}};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectBodyWriter writer={.work=context,.material=&material};LiteralBodySink received={0};
    EffectBodyWriteSink sink={&received,literal_body_sink_row,literal_body_sink_span};
    XrXirStatus status=effect_body_material_records(&writer,&source,&function,&sink);
    if (status!=XR_XIR_OK) goto done;
    CHECK(received.next==LITERAL_BODY_COUNT && effect_body_function_storage_shape(&function));
    CHECK(literal ? function.literals && !function.runs && function.run_count==LITERAL_BODY_COUNT :
        !function.literals && function.runs && function.run_count==1);
    EffectBodyRead read=effect_body_compile_read(context);
    status=effect_body_records_match(&read,&function,&source);if (status!=XR_XIR_OK) goto done;
    EffectBodyRequest request=effect_body_owned_request(context,&material);EffectBodyRecordCursor cursor;
    status=effect_body_request_sequence_begin(&request,&cursor);if (status!=XR_XIR_OK) goto done;
    size_t reader_allocations=rp_stats(context).allocation_count;
    for (uint32_t i=0;i<LITERAL_BODY_COUNT;++i) {
        XrXirInstruction expected=literal_body_row(i,literal),storage;const XrXirInstruction *op=NULL;
        memset(&storage,0xA5,sizeof(storage));XrXirInstruction sentinel=storage;
        status=effect_body_request_sequence_instruction(&request,0,i,&cursor,&storage,&op);
        if (status!=XR_XIR_OK) {CHECK(!op && !memcmp(&storage,&sentinel,sizeof(storage)));goto done;}
        CHECK(op==&storage && conditional_compact_same(op,&expected));
    }
    status=effect_body_request_sequence_finish(&request,0,&cursor);if (status!=XR_XIR_OK) goto done;
    if (boundaries && literal) {
        XrXirInstruction saved=((XrXirInstruction *)function.literals)[5],output=literal_body_row(3,true);
        XrXirInstruction sentinel=output;((XrXirInstruction *)function.literals)[5].op=XR_XIR_INVALID;
        CHECK(effect_body_instruction_get(&read,&function,5,&output)==XR_XIR_BAD_STRUCTURE &&
            conditional_compact_same(&output,&sentinel));
        ((XrXirInstruction *)function.literals)[5]=saved;
        uint32_t count=function.run_count;--function.run_count;
        CHECK(effect_body_instruction_get(&read,&function,5,&output)==XR_XIR_BAD_STRUCTURE &&
            conditional_compact_same(&output,&sentinel));function.run_count=count;
        ++rows[5].type_arguments[1];CHECK(effect_body_records_match(&read,&function,&source)==XR_XIR_BAD_STRUCTURE);
        --rows[5].type_arguments[1];CHECK(effect_body_records_match(&read,&function,&source)==XR_XIR_OK);
        conditional_compact_read_failures(context,&function,5,&saved);
    }
    memset(rows,0xCE,sizeof(rows));
    for (uint32_t n=0;n<LITERAL_BODY_COUNT;++n) {
        uint32_t i=LITERAL_BODY_COUNT-1-n;XrXirInstruction expected=literal_body_row(i,literal),actual;
        memset(&actual,0xA5,sizeof(actual));XrXirInstruction sentinel=actual;
        status=effect_body_instruction_get(&read,&function,i,&actual);
        if (status!=XR_XIR_OK) {CHECK(!memcmp(&actual,&sentinel,sizeof(actual)));goto done;}
        CHECK(conditional_compact_same(&actual,&expected));
        XrXirType type=XR_XIR_U8;status=effect_body_operand_type(&read,&function,i,&type);
        if (status!=XR_XIR_OK) {CHECK(type==XR_XIR_U8);goto done;}CHECK(type==expected.type);
    }
    CHECK(rp_stats(context).allocation_count==reader_allocations);
done:
    xr_compile_resources_free(function.runs);effect_terms_free(&material.terms);return status;
}
static void literal_body_checked(void) {
    RootParameterMark initial=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;XrXirInstruction rows[LITERAL_BODY_COUNT];
    for (uint32_t i=0;i<LITERAL_BODY_COUNT-1;++i)
        rows[i]=(i&1)?(XrXirInstruction){.op=XR_XIR_ADD_INT,.type=XR_XIR_I64,.args={i-1,i-1}}:
            (XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=(int64_t)i+1};
    rows[LITERAL_BODY_COUNT-1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={LITERAL_BODY_COUNT-2,0}};
    XrXirBlock block={.count=LITERAL_BODY_COUNT};
    XrXirFunction function={.name="main",.name_length=4,.result=XR_XIR_I64,
        .blocks=&block,.block_count=1,.instructions=rows,.instruction_count=LITERAL_BODY_COUNT};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,
        .linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*decoded=NULL;XrXirCheckedPacket packet={0};
    XrXirDiagnostic diagnostic={0};XrXirStatus admitted=xir_fixture_check(&context,&module,&checked,&diagnostic);
    if (admitted!=XR_XIR_OK) fprintf(stderr,"LITERAL_CHECK status%u function%u block%u instruction%u\n",admitted,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(admitted==XR_XIR_OK && checked);
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK && decoded);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_verify(decoded,NULL)==XR_XIR_OK);
    const XrXirModule *owned=xr_xir_compile_artifact_module(decoded);EffectBodyMaterial *material=NULL;
    CHECK(effect_body_material_create(&context,owned,&material)==XR_XIR_OK && material);
    CHECK(material->functions[0].literals && !material->functions[0].runs);
    CHECK(effect_body_source_match(&context,material,owned)==XR_XIR_OK);
    XrXirInstruction *literal=(XrXirInstruction *)material->functions[0].literals;
    literal[4].type_arguments[1]=1;
    CHECK(effect_body_source_match(&context,material,owned)==XR_XIR_BAD_STRUCTURE);
    literal[4].type_arguments[1]=0;CHECK(effect_body_source_match(&context,material,owned)==XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);memset(rows,0xCE,sizeof(rows));
    EffectBodyRead read=effect_body_compile_read(&context);
    for (uint32_t i=0;i<LITERAL_BODY_COUNT;++i) {
        XrXirInstruction op;
        CHECK(effect_body_instruction_get(&read,&material->functions[0],i,&op)==XR_XIR_OK);
        if (i==LITERAL_BODY_COUNT-1) CHECK(op.op==XR_XIR_RETURN && op.args[0]==LITERAL_BODY_COUNT-2);
        else if (i&1) CHECK(op.op==XR_XIR_ADD_INT && op.type==XR_XIR_I64 && op.args[0]==i-1 && op.args[1]==i-1);
        else CHECK(op.op==XR_XIR_CONST_INT && op.type==XR_XIR_I64 && op.immediate==(int64_t)i+1);
    }
    effect_body_material_free(material);rp_owner_free(&context,baseline);rp_balanced(initial);
    puts("LITERAL_BODY real Checked/write/read/current-match/unused-word-rejection producer-death physical0");
}
static void literal_body_rejections(void) {
    RootParameterMark initial=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;XrXirInstruction rows[LITERAL_BODY_COUNT];
    for (uint32_t i=0;i<LITERAL_BODY_COUNT;++i) rows[i]=literal_body_row(i,true);
    XrXirFunction source={.instructions=rows,.instruction_count=LITERAL_BODY_COUNT};
    EffectBodyFunction target={.instruction_count=LITERAL_BODY_COUNT};
    EffectBodyMaterial material={.resources=context.resources,.references=1,.terms={.remaining=&context}};
    EffectBodyWriter writer={.work=&context,.material=&material};LiteralBodySink received={0};
    EffectBodyWriteSink sink={&received,literal_body_sink_row,literal_body_sink_span};
    XrCompileResourceStats before=rp_stats(&context);
    for (unsigned fault=0;fault<5;++fault) {
        if (!fault) source.instructions=NULL;
        else if (fault==1) --target.instruction_count;
        else if (fault==2) target.literals=rows;
        else if (fault==3) sink.owner=NULL;
        else material.resources=NULL;
        CHECK(effect_body_material_records(&writer,&source,&target,&sink)==XR_XIR_BAD_STRUCTURE);
        if (fault==2) CHECK(target.literals==rows);
        source.instructions=rows;target.instruction_count=LITERAL_BODY_COUNT;target.literals=NULL;
        sink.owner=&received;material.resources=context.resources;
    }
    bool literal=true;CHECK(effect_body_literal_layout(&context,NULL,&literal)==XR_XIR_BAD_STRUCTURE && literal);
    XrCompileResourceStats after=rp_stats(&context);
    CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes &&
        before.live_bytes==after.live_bytes && before.allocation_count==after.allocation_count && !received.next);
    rp_owner_free(&context,baseline);rp_balanced(initial);
}
static void literal_body_cases(void) {
    literal_body_rejections();
    for (unsigned mode=0;mode<2;++mode) {
        size_t sites=0;XrCompileResourceStats measured={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark initial=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
            uint64_t baseline=rp_stats(&context).live_bytes;size_t first=rp_attempts;
            rp_fail_at=pass?first+pass-1:SIZE_MAX;rp_injected=false;
            XrXirStatus status=literal_body_roundtrip(&context,mode!=0,false);
            if (!pass) {CHECK(status==XR_XIR_OK);sites=rp_attempts-first;measured=rp_stats(&context);CHECK(sites);}
            else {
                CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_stats(&context).live_bytes==baseline);
                rp_fail_at=SIZE_MAX;CHECK(literal_body_roundtrip(&context,mode!=0,false)==XR_XIR_OK);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,baseline);rp_balanced(initial);
        }
        uint64_t exact[]={measured.allocated_bytes,measured.peak_bytes,measured.work};
        for (unsigned axis=0;axis<3;++axis) for (unsigned below=0;below<2;++below) {
            RootParameterMark initial=rp_mark();XrCompileResourceLimits caps=rp_caps();
            if (!axis) caps.allocated_bytes=exact[axis]-below;
            else if (axis==1) caps.live_bytes=exact[axis]-below;else caps.work=exact[axis]-below;
            XrXirCompileContext context=rp_owner(caps);uint64_t baseline=rp_stats(&context).live_bytes;
            CHECK(literal_body_roundtrip(&context,mode!=0,false)==(below?XR_XIR_BUDGET:XR_XIR_OK));
            rp_owner_free(&context,baseline);rp_balanced(initial);
        }
        RootParameterMark initial=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;CHECK(literal_body_roundtrip(&context,mode!=0,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(initial);
        printf("LITERAL_BODY mode%u sites%zu work%llu allocated%llu peak%llu all-FI exact-minus1 same-owner physical0\n",
            mode,sites,(unsigned long long)measured.work,(unsigned long long)measured.allocated_bytes,
            (unsigned long long)measured.peak_bytes);
    }
    literal_body_checked();
}
#endif // XIR_LITERAL_BODY_CASES_H
