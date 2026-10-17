/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_compact_builder_cases.h - Complete Cell builder prefix controls
 */
#ifndef XIR_INVOCATION_COMPACT_BUILDER_CASES_H
#define XIR_INVOCATION_COMPACT_BUILDER_CASES_H
static void conditional_compact_builder_rows(const XrXirCompileContext *context,
    XrXirCellProofBodyRead *request) {
    XrXirCellProofBodyHead head={0};XrXirInstruction actual;
    const XrXirInstruction *operation=NULL;
    CHECK(request->head(context,request->owner,0,&head)==XR_XIR_OK && !head.parameter_count && head.instruction_count);
    CHECK(request->instruction(context,request->owner,0,0,&actual,&operation)==XR_XIR_OK && operation==&actual &&
        operation->op==XR_XIR_CONST_INT && operation->type==XR_XIR_I64);
    for (uint32_t mode=0;mode<4;++mode) {
        uint64_t held_bytes=rp_stats(context).live_bytes;
        XrXirCellProofBuilder *builder=NULL;XrXirCellProvenance *output=NULL;
        CHECK(xir_cell_proof_builder_begin(context,request,&builder)==XR_XIR_OK && builder);
        CHECK(xir_cell_proof_builder_head(builder,0,&head)==XR_XIR_OK);
        XrXirStatus expected=mode==0 || mode==3?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE;
        XrXirStatus status;
        if (!mode) status=xir_cell_proof_builder_instruction(builder,0,1,operation->type,operation->op,operation->type);
        else if (mode==1) status=xir_cell_proof_builder_parameter(builder,0,0,operation->type);
        else status=xir_cell_proof_builder_instruction(builder,0,0,operation->type,
            mode==3?XR_XIR_INVALID:operation->op,mode==2?XR_XIR_F64:operation->type);
        CHECK(status==expected);
        XrCompileResourceStats before=rp_stats(context);
        CHECK(xir_cell_proof_builder_instruction(builder,0,0,operation->type,operation->op,operation->type)==expected);
        XrCompileResourceStats after=rp_stats(context);CHECK(!memcmp(&before,&after,sizeof(before)));
        CHECK(xir_cell_proof_builder_finish(&builder,&output)==expected && !builder && !output);
        CHECK(rp_stats(context).live_bytes==held_bytes);
    }
}

/* The genuine producer dies before every span attack. Ordinary head,
 * scalar, role and edge replay remains complete for all four functions. */
static void conditional_compact_builder_spans(const XrXirCompileContext *context,
    XrXirCellProofBodyRead *request) {
    CHECK(request->span);
    XrXirCellProofSpanCursor sentinel={.function=3,.shape={XR_XIR_RETURN,XR_XIR_F64}},cursor=sentinel;
    CHECK(request->span(context,request->owner,4,0,2,&cursor)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&sentinel,sizeof(cursor)));
    CHECK(request->span(context,request->owner,3,1,2,&cursor)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&sentinel,sizeof(cursor)));
    cursor.function=sentinel.function=0;
    CHECK(request->span(context,request->owner,0,0,2,&cursor)==XR_XIR_BAD_TYPE &&
        !memcmp(&cursor,&sentinel,sizeof(cursor)));
    /* A zero supplied head has no scalar row. It must still be rejected
     * against the genuine nonempty owned head before any provider escapes. */
    uint64_t empty_held=rp_stats(context).live_bytes;
    XrXirCellProofBodyHead empty={0};
    CHECK(request->head(context,request->owner,0,&empty)==XR_XIR_OK);
    empty.instruction_count=0;
    XrXirCellProofBuilder *empty_builder=NULL;XrXirCellProvenance *empty_output=NULL;
    CHECK(xir_cell_proof_builder_begin(context,request,&empty_builder)==XR_XIR_OK);
    CHECK(xir_cell_proof_builder_head(empty_builder,0,&empty)==XR_XIR_OK);
    CHECK(xir_cell_proof_builder_function(empty_builder,0)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats empty_retry=rp_stats(context);
    CHECK(xir_cell_proof_builder_function(empty_builder,0)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats empty_after=rp_stats(context);
    CHECK(!memcmp(&empty_retry,&empty_after,sizeof(empty_retry)));
    CHECK(xir_cell_proof_builder_finish(&empty_builder,&empty_output)==XR_XIR_BAD_STRUCTURE &&
        !empty_builder && !empty_output && rp_stats(context).live_bytes==empty_held);
    for (uint32_t mode=0;mode<9;++mode) {
        uint64_t held_bytes=rp_stats(context).live_bytes;
        XrXirCellProofBuilder *builder=NULL;XrXirCellProvenance *output=NULL;
        CHECK(xir_cell_proof_builder_begin(context,request,&builder)==XR_XIR_OK && builder);
        for (uint32_t f=0;f<3;++f) {
            XrXirCellProofBodyHead head={0};
            CHECK(request->head(context,request->owner,f,&head)==XR_XIR_OK);
            CHECK(xir_cell_proof_builder_head(builder,f,&head)==XR_XIR_OK);
            for (uint32_t v=0;v<head.parameter_count;++v)
                CHECK(xir_cell_proof_builder_parameter(builder,f,v,head.parameters[v])==XR_XIR_OK);
            for (uint32_t i=0;i<head.instruction_count;++i) {
                XrXirInstruction actual;const XrXirInstruction *operation=NULL;
                CHECK(request->instruction(context,request->owner,f,i,&actual,&operation)==XR_XIR_OK &&
                    operation==&actual);
                CHECK(xir_cell_proof_builder_instruction(builder,f,head.parameter_count+i,
                    operation->type,operation->op,operation->type)==XR_XIR_OK);
            }
            XrCompileResourceStats scalar_end=rp_stats(context);
            CHECK(xir_cell_proof_builder_function(builder,f)==XR_XIR_OK);
            CHECK(rp_stats(context).work-scalar_end.work==
                2+8+sizeof(uint64_t)+sizeof(uint32_t)+sizeof(bool));
        }
        XrXirCellProofBodyHead head={0};
        CHECK(request->head(context,request->owner,3,&head)==XR_XIR_OK && !head.parameter_count && head.instruction_count==3);
        CHECK(xir_cell_proof_builder_head(builder,3,&head)==XR_XIR_OK);
        XrXirCellProofInstructionShape shape={XR_XIR_CONST_INT,XR_XIR_I64};
        XrXirStatus status;
        if (mode<6) {
            if (mode==4) shape.type=XR_XIR_F64;
            if (mode==5) shape.op=XR_XIR_INVALID;
            status=xir_cell_proof_builder_span(builder,mode==0?2:3,mode==1?1:0,
                mode==2?0:mode==3?4:2,&shape);
            XrXirStatus expected=mode==4?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE;
            CHECK(status==expected);
            XrCompileResourceStats before_retry=rp_stats(context);
            shape=(XrXirCellProofInstructionShape){XR_XIR_CONST_INT,XR_XIR_I64};
            CHECK(xir_cell_proof_builder_span(builder,3,0,2,&shape)==expected);
            XrCompileResourceStats after_retry=rp_stats(context);CHECK(!memcmp(&before_retry,&after_retry,sizeof(before_retry)));
            CHECK(xir_cell_proof_builder_finish(&builder,&output)==expected && !builder && !output);
        } else {
            CHECK(xir_cell_proof_builder_span(builder,3,0,2,&shape)==XR_XIR_OK);
            XrXirInstruction actual;const XrXirInstruction *operation=NULL;
            CHECK(request->instruction(context,request->owner,3,2,&actual,&operation)==XR_XIR_OK &&
                operation==&actual && operation->op==XR_XIR_RETURN);
            CHECK(xir_cell_proof_builder_instruction(builder,3,2,
                operation->type,operation->op,operation->type)==XR_XIR_OK);
            const EffectCellBodyAdapter *adapter=request->owner;
            EffectBodyMaterial *material=(EffectBodyMaterial *)adapter->request.owner;
            EffectBodyFunction *function=&material->functions[3];
            uint32_t capacity=function->run_capacity,runs=function->run_count;
            if (mode==6) function->run_capacity=function->run_count-1;
            if (mode==7) --function->run_count;
            status=xir_cell_proof_builder_function(builder,3);
            function->run_capacity=capacity;function->run_count=runs;
            if (mode<8) CHECK(status==XR_XIR_BAD_STRUCTURE);
            else CHECK(status==XR_XIR_OK);
            CHECK(xir_cell_proof_builder_finish(&builder,&output)==(mode<8?XR_XIR_BAD_STRUCTURE:XR_XIR_OK) && !builder);
            if (mode<8) CHECK(!output);
            else CHECK(output && xr_xir_compile_cell_provenance_body_matches(context,output,request,NULL)==XR_XIR_OK);
            xr_xir_compile_cell_provenance_free(output);
        }
        CHECK(rp_stats(context).live_bytes==held_bytes);
    }
}
#endif // XIR_INVOCATION_COMPACT_BUILDER_CASES_H
