/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * A default has at most one completed outer argument conversion. */
static bool source_hir_default_projection(SourceContext *ctx,SourceValue value,XrHirBodyProjection *out) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    uint32_t parameters=ctx->functions[ctx->function].parameter_count,id=value.id;
    if(id<parameters||id-parameters>=body->count)goto invalid;
    XrXirInstruction producer=body->recipes[id-parameters].instruction;
    XrXirOp conversion=XR_XIR_INVALID;
    if(producer.op==XR_XIR_COPY||producer.op==XR_XIR_FUNCTION_WEAKEN) {
        if(producer.type!=value.type||producer.args[0]>=id||producer.args[1])goto invalid;
        conversion=producer.op;id=producer.args[0];
        if(id<parameters||id-parameters>=body->count)goto invalid;
        producer=body->recipes[id-parameters].instruction;
    }
    if(producer.type!=value.type)goto invalid;
    uint32_t call=id;
    if(producer.op==XR_XIR_INVOKE_RESULT) {
        if(producer.immediate<0||(uint64_t)producer.immediate>=id-parameters)goto invalid;
        call=parameters+(uint32_t)producer.immediate;
        producer=body->recipes[(uint32_t)producer.immediate].instruction;
        if(producer.op!=XR_XIR_INVOKE_DEFAULT||producer.type!=value.type)goto invalid;
    } else if(producer.op!=XR_XIR_CALL_DEFAULT)goto invalid;
    XrXirGeneric generic=ctx->generics[ctx->function];
    uint32_t first=producer.type_arguments[0],count=producer.type_arguments[1];
    if(first>generic.argument_count||count>generic.argument_count-first||(count&&!generic.arguments)||(!count&&first))goto invalid;
    out->has_default=true;out->default_value=id;out->default_call=call;out->default_conversion=conversion;
    out->default_owner=producer.op==XR_XIR_CALL_DEFAULT?producer.targets[0]:producer.args[0];
    out->default_parameter=producer.op==XR_XIR_CALL_DEFAULT?producer.targets[1]:producer.args[1];
    out->default_type_arguments=count?generic.arguments+first:NULL;out->default_type_argument_count=count;
    return true;
invalid:
    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"default value lost its actual producer or completed conversion");
}
