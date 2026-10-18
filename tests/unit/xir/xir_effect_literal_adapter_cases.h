/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_literal_adapter_cases.h - Independent dense body reader regression
 */
#ifndef XIR_EFFECT_LITERAL_ADAPTER_CASES_H
#define XIR_EFFECT_LITERAL_ADAPTER_CASES_H

enum { PHYSICAL_LITERAL_ROWS=34 };
static XrXirInstruction physical_adapter_row(uint32_t i) {
    return (XrXirInstruction){.op=i&1?XR_XIR_CONST_BOOL:XR_XIR_CONST_INT,
        .type=i&1?XR_XIR_BOOL:XR_XIR_I64,.args={17+i*3,91-i},.targets={i*7,121+i},
        .immediate=INT64_MIN+(int64_t)i*INT64_C(4294967297),.type_arguments={211+i,413+i*11}};
}
static XrXirStatus physical_adapter_sink_row(void *owner,uint32_t i,const XrXirInstruction *op) {
    uint32_t *next=owner;CHECK(op && i==*next);++*next;return XR_XIR_OK;
}
static XrXirStatus physical_adapter_sink_span(void *owner,uint32_t first,uint32_t count) {
    uint32_t *next=owner;CHECK(first==*next && count);*next+=count;return XR_XIR_OK;
}
static void physical_adapter_boundaries(const XrXirCompileContext *context,
    EffectBodyMaterial *material,const XrXirCellProofBodyRead *read) {
    EffectBodyFunction *function=&material->functions[0];
    XrXirInstruction *rows=(XrXirInstruction *)function->literals;
    XrXirCellProofRunCursor cursor={.next=PHYSICAL_LITERAL_ROWS,.run=PHYSICAL_LITERAL_ROWS};
    XrXirCellProofRunCursor before=cursor;XrXirCellProofRun range;
    memset(&range,0xA5,sizeof(range));XrXirCellProofRun saved_range=range;
    CHECK(read->range(context,read,&cursor,&range)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&before,sizeof(cursor)) && !memcmp(&range,&saved_range,sizeof(range)));
    XrXirCellProofBodyHead head;memset(&head,0xA5,sizeof(head));
    XrXirCellProofBodyHead saved_head=head;--function->run_count;
    CHECK(read->head(context,read->owner,0,&head)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&head,&saved_head,sizeof(head)));++function->run_count;
    XrXirInstruction output=physical_adapter_row(4),sentinel=output;
    XrXirCellProofInstructionShape shape={XR_XIR_RETURN,XR_XIR_F64},saved_shape=shape;
    cursor=(XrXirCellProofRunCursor){.next=6,.run=6};
    XrXirOp saved=rows[5].op;rows[5].op=XR_XIR_INVALID;
    CHECK(read->range_row(context,read,&cursor,5,5,&output)==XR_XIR_BAD_STRUCTURE &&
        physical_equal(&output,&sentinel));
    CHECK(read->range_shape(context,read,&cursor,5,5,&shape)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&shape,&saved_shape,sizeof(shape)));
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead fresh={0};
    CHECK(effect_cell_body_owned(context,material,&adapter,&fresh)==XR_XIR_OK);
    CHECK(effect_cell_body_project(&adapter)==XR_XIR_BAD_STRUCTURE && !adapter.projection_offsets &&
        !adapter.projection_body && !adapter.projection_types && !adapter.projection_values);
    effect_cell_body_projection_drop(&adapter);rows[5].op=saved;
    cursor.run=5;
    CHECK(read->range_row(context,read,&cursor,5,5,&output)==XR_XIR_BAD_STRUCTURE &&
        physical_equal(&output,&sentinel));
}
/* Storage probes deliberately carry unused nonzero fields. They prove whole
 * physical reads and failure atomicity without claiming executable admission. */
static XrXirStatus physical_adapter_attempt(const XrXirCompileContext *context,bool boundaries) {
    XrXirInstruction rows[PHYSICAL_LITERAL_ROWS];
    for (uint32_t i=0;i<PHYSICAL_LITERAL_ROWS;++i) rows[i]=physical_adapter_row(i);
    XrXirType parameters[]={XR_XIR_I64,XR_XIR_BOOL};uint32_t received=0;
    XrXirFunction source={.instructions=rows,.instruction_count=PHYSICAL_LITERAL_ROWS};
    EffectBodyFunction function={.parameters=parameters,.parameter_count=2,.instruction_count=PHYSICAL_LITERAL_ROWS};
    EffectBodyMaterial material={.resources=context->resources,.references=1,.functions=&function,
        .terms={.remaining=context}};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectBodyWriter writer={.work=context,.material=&material};
    EffectBodyWriteSink sink={&received,physical_adapter_sink_row,physical_adapter_sink_span};
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    XrXirStatus status=effect_body_material_records(&writer,&source,&function,&sink);
    if (status!=XR_XIR_OK) goto done;
    CHECK(received==PHYSICAL_LITERAL_ROWS && function.literals && !function.runs);
    status=effect_cell_body_owned(context,&material,&adapter,&read);if (status!=XR_XIR_OK) goto done;
    XrXirCellProofBodyHead head={0};
    status=read.head(context,read.owner,0,&head);if (status!=XR_XIR_OK) goto done;
    CHECK(head.parameter_count==2 && head.instruction_count==PHYSICAL_LITERAL_ROWS);
    status=effect_cell_body_project(&adapter);if (status!=XR_XIR_OK) goto done;
    memset(rows,0xCE,sizeof(rows));
    for (uint32_t v=0;v<PHYSICAL_LITERAL_ROWS+2;++v) {
        XrXirType actual=XR_XIR_F64;
        status=read.type(context,read.owner,0,v,&actual);if (status!=XR_XIR_OK) goto done;
        CHECK(actual==(v<2?parameters[v]:physical_adapter_row(v-2).type));
    }
    XrXirCellProofRunCursor cursor={0};
    for (uint32_t i=0;i<PHYSICAL_LITERAL_ROWS;++i) {
        XrXirCellProofRun range={0};XrXirInstruction actual={0},expected=physical_adapter_row(i);
        XrXirCellProofInstructionShape shape={0};
        status=read.range(context,&read,&cursor,&range);if (status!=XR_XIR_OK) goto done;
        CHECK(range.first==i && range.count==1 && !range.varying && physical_equal(&range.operation,&expected));
        status=read.range_row(context,&read,&cursor,i,i,&actual);if (status!=XR_XIR_OK) goto done;
        CHECK(physical_equal(&actual,&expected));
        status=read.range_shape(context,&read,&cursor,i,i,&shape);if (status!=XR_XIR_OK) goto done;
        CHECK(shape.op==expected.op && shape.type==expected.type);
    }
    CHECK(cursor.next==PHYSICAL_LITERAL_ROWS && cursor.run==PHYSICAL_LITERAL_ROWS);
    if (boundaries) physical_adapter_boundaries(context,&material,&read);
done:
    effect_cell_body_projection_drop(&adapter);xr_compile_resources_free(function.runs);
    effect_terms_free(&material.terms);return status;
}
static void physical_literal_adapter_cases(void) {
    size_t sites=0;XrCompileResourceStats measured={0};
    for (size_t pass=0;pass<=sites;++pass) {
        const XrXirCompileContext *context=effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
        size_t first=effects_compile_attempts;
        effects_compile_fail_at=pass?first+pass-1:SIZE_MAX;effects_compile_injected=false;
        XrXirStatus status=physical_adapter_attempt(context,false);
        effects_compile_fail_at=SIZE_MAX;
        if (!pass) {
            CHECK(status==XR_XIR_OK);sites=effects_compile_attempts-first;CHECK(sites);
            CHECK(xr_compile_resources_stats(context->resources,&measured)==XR_COMPILE_RESOURCE_OK);
        } else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY && effects_compile_injected);
            CHECK(physical_adapter_attempt(context,false)==XR_XIR_OK);
        }
        effects_source_owners_free();
    }
    uint64_t exact[]={measured.allocated_bytes,measured.peak_bytes,measured.work};
    for (unsigned axis=0;axis<3;++axis) for (unsigned below=0;below<2;++below) {
        XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
        if (!axis) limits.allocated_bytes=exact[axis]-below;
        else if (axis==1) limits.live_bytes=exact[axis]-below;else limits.work=exact[axis]-below;
        XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(physical_adapter_attempt(&context,false)==(below?XR_XIR_BUDGET:XR_XIR_OK));
        XrCompileResourceStats final={0};
        CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK && !final.live_bytes);
        xr_compile_resources_release(context.resources);CHECK(!effects_compile_live && !effects_compile_bytes);
    }
    const XrXirCompileContext *context=effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    CHECK(physical_adapter_attempt(context,true)==XR_XIR_OK);effects_source_owners_free();
    fprintf(stderr,"literal adapter: full head/type/34 rows, producer death, malformed boundaries, %zu FI sites, three exact/minus-one axes, physical0\n",sites);
}
#endif // XIR_EFFECT_LITERAL_ADAPTER_CASES_H
