/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_cell_replay.inc.c - Call-local physical rows for complete Cell replay
 *
 * KEY CONCEPT:
 *   Every canonical field is decoded before the unchanged provider compares
 *   its complete proof. The private rows and borrowed metadata expire together;
 *   no permission or comparison result survives this read-only operation.
 */
typedef struct EffectCellReplayRows {
    XrXirModule module;
    XrXirFunction *functions;
} EffectCellReplayRows;

static XrXirStatus effect_cell_replay_rows_create(const XrXirCompileContext *work,
    const EffectBodyRequest *body,EffectCellReplayRows *output) {
    if (!xir_compile_context_valid(work) || !body || !output || output->functions ||
        output->module.functions || output->module.function_count ||
        !effect_body_request_sequence_owned(body) || body->read.owner!=work) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,2*sizeof(*output))) return XR_XIR_BUDGET;
    XrXirStatus status=effect_body_request_sequence_binding(body);
    uint32_t count=body->domain.function_count;
    if (status!=XR_XIR_OK) return status;
    XrXirFunction *functions=count ? xir_compile_calloc(work,count,sizeof(*functions),&status) : NULL;
    if (count && !functions) return status;
    XrXirModule view={.functions=functions,.function_count=count,.types=body->domain.types,
        .declarations=body->domain.declarations,.generics=body->domain.generics,.defaults=body->domain.defaults};
    for (uint32_t f=0;f<count && status==XR_XIR_OK;++f) {
        EffectBodyFunctionHead head={0};status=effect_body_request_head(body,f,&head);
        if (status!=XR_XIR_OK) break;
        uint64_t bytes=(uint64_t)head.instruction_count*sizeof(XrXirInstruction);
        if (bytes>SIZE_MAX) {status=XR_XIR_BUDGET;break;}
        XrXirInstruction *rows=bytes ? xir_compile_alloc(work,(size_t)bytes,&status) : NULL;
        if (bytes && !rows) break;
        if (!xir_compile_work(work,sizeof(*functions))) {xr_compile_resources_free(rows);status=XR_XIR_BUDGET;break;}
        functions[f]=(XrXirFunction){.name=head.name,.name_length=head.name_length,
            .parameters=head.parameters,.parameter_count=head.parameter_count,.result=head.result,
            .blocks=head.blocks,.block_count=head.block_count,.instructions=rows,
            .instruction_count=head.instruction_count,.operands=head.operands,.operand_count=head.operand_count,
            .error_type=head.error_type};
        EffectBodyRecordCursor cursor;status=effect_body_request_sequence_begin(body,&cursor);
        for (uint32_t i=0;i<head.instruction_count && status==XR_XIR_OK;++i) {
            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            status=effect_body_request_sequence_operation(body,f,i,&cursor,&storage,&op);
            if (status!=XR_XIR_OK) break;
            if (!op) {status=XR_XIR_BAD_STRUCTURE;break;}
            if (!xir_compile_work(work,sizeof(*rows))) {status=XR_XIR_BUDGET;break;}
            rows[i]=*op;
        }
        if (status==XR_XIR_OK) status=effect_body_request_sequence_finish(body,f,&cursor);
    }
    output->module=view;output->functions=functions;return status;
}

static void effect_cell_replay_rows_free(EffectCellReplayRows *rows) {
    for (uint32_t f=0;f<rows->module.function_count;++f)
        xr_compile_resources_free((void *)rows->functions[f].instructions);
    xr_compile_resources_free(rows->functions);
}
