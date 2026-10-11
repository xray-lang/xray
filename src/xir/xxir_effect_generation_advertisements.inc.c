/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_generation_advertisements.inc.c - Authentic bounds during the sole body write
 *
 * KEY CONCEPT:
 *   Each actual physical row consumes its original declaration while that
 *   producer is alive. No refined SSA flag reconstructs an advertisement.
 */
static XrXirStatus effect_invocation_source_declarations(const XrXirCompileContext *work,
    const XrXirModule *source,uint32_t function,const XrXirType *prebottom,XrXirType **output);

struct EffectBodyAdvertisementBuilder {
    const XrXirCompileContext *work;
    const XrXirModule *source;
    EffectRefinementBounds *refinement;
    EffectInvocationDeclaredBounds *bounds;
    const XrXirType *instructions;
    XrXirType *source_types;
    uint32_t function,next,capacity,identity;
    bool open;
};

static void effect_body_advertisements_free(EffectBodyAdvertisementBuilder *builder) {
    if (!builder) return;
    xr_compile_resources_free(builder->source_types);
    effect_invocation_bounds_free(builder->bounds);xr_compile_resources_free(builder);
}
static XrXirStatus effect_body_advertisements_begin(const XrXirCompileContext *work,
    const XrXirModule *source,EffectRefinementBounds *refinement,EffectBodyAdvertisementBuilder **output) {
    if (!xir_compile_context_valid(work) || !source || (!!source->functions!=!!source->function_count) ||
        !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=refinement?effect_refinement_match(work,source,refinement):XR_XIR_OK;
    if (status!=XR_XIR_OK) return status;
    EffectBodyAdvertisementBuilder *builder=xir_compile_calloc(work,1,sizeof(*builder),&status);
    if (!builder) return status;
    builder->work=work;builder->source=source;builder->refinement=refinement;
    status=effect_invocation_bounds_new(work,&builder->bounds);
    if (status==XR_XIR_OK) {
        builder->bounds->functions=source->function_count?xir_compile_calloc(work,source->function_count,
            sizeof(*builder->bounds->functions),&status):NULL;
        if (status==XR_XIR_OK) builder->bounds->count=builder->bounds->capacity=source->function_count;
    }
    if (status!=XR_XIR_OK) { effect_body_advertisements_free(builder);return status; }
    *output=builder;return XR_XIR_OK;
}
static XrXirStatus effect_body_advertisements_function(EffectBodyAdvertisementBuilder *builder,uint32_t function) {
    if (!builder || function!=builder->function || function>=builder->bounds->count) return XR_XIR_BAD_STRUCTURE;
    if (builder->open) return XR_XIR_BAD_STRUCTURE;
    xr_compile_resources_free(builder->source_types);builder->source_types=NULL;builder->instructions=NULL;
    builder->next=builder->capacity=builder->identity=0;
    const XrXirFunction *body=&builder->source->functions[function];
    uint64_t values=(uint64_t)body->parameter_count+body->instruction_count;
    if (values>UINT32_MAX) return XR_XIR_BUDGET;
    if (!xir_compile_work(builder->work,6)) return XR_XIR_BUDGET;
    builder->bounds->functions[function].values=(uint32_t)values;
    if (builder->refinement) builder->instructions=builder->refinement->functions[function].instructions;
    bool instance=builder->source->provenance && builder->source->provenance->kind==XR_XIR_EVIDENCE_INSTANCE;
    XrXirStatus status=instance?XR_XIR_OK:effect_invocation_source_declarations(builder->work,
        builder->source,function,builder->instructions,&builder->source_types);
    if (status!=XR_XIR_OK) return status;
    if (builder->source_types) builder->instructions=builder->source_types;
    builder->open=values!=0;
    if (!values) ++builder->function;
    return XR_XIR_OK;
}
static XrXirStatus effect_body_advertisements_append(EffectBodyAdvertisementBuilder *builder,
    EffectInvocationValueBound record) {
    EffectInvocationFunctionBounds *function=&builder->bounds->functions[builder->function];
    if (function->count==builder->capacity) {
        uint32_t capacity=builder->capacity?builder->capacity:8;
        if (capacity>function->values) capacity=function->values;
        else if (builder->capacity) capacity=capacity>function->values/2?function->values:capacity*2;
        if (capacity<=function->count || (uint64_t)capacity>SIZE_MAX/sizeof(*function->bounds)) return XR_XIR_BUDGET;
        XrXirStatus status=XR_XIR_OK;
        EffectInvocationValueBound *fresh=xir_compile_alloc(builder->work,(size_t)capacity*sizeof(*fresh),&status);
        if (!fresh) return status;
        uint64_t bytes=(uint64_t)function->count*sizeof(*fresh);
        if (!xir_compile_work(builder->work,bytes+2)) { xr_compile_resources_free(fresh);return XR_XIR_BUDGET; }
        if (bytes) memcpy(fresh,function->bounds,(size_t)bytes);
        xr_compile_resources_free(function->bounds);function->bounds=fresh;builder->capacity=capacity;
    }
    if (!xir_compile_work(builder->work,sizeof(record)+1)) return XR_XIR_BUDGET;
    function->bounds[function->count++]=record;return XR_XIR_OK;
}
static XrXirStatus effect_body_advertisements_value(EffectBodyAdvertisementBuilder *builder,
    uint32_t function,uint32_t value,XrXirType actual) {
    if (!builder || !builder->open || function!=builder->function || value!=builder->next ||
        function>=builder->bounds->count || value>=builder->bounds->functions[function].values)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module=builder->source;const XrXirFunction *body=&module->functions[function];
    if (!xir_compile_work(builder->work,3)) return XR_XIR_BUDGET;
    XrXirType declared=actual;EffectInvocationValueBound record={0};XrXirStatus status=XR_XIR_OK;
    const XrXirProvenance *instance=module->provenance;
    if (instance && instance->kind==XR_XIR_EVIDENCE_INSTANCE) {
        const XrXirOrigin *origin=&instance->origins[function];
        const XrXirModule *source=&instance->source->module;
        const XrXirFunction *original=&source->functions[origin->function];
        XirEffectCallableBound request={source->types,module->types,origin->arguments,origin->argument_count,XR_XIR_UNIT,actual};
        const XrXirFunctionEffectContract *contract=NULL;
        if (source->provenance) {
            if (source->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE ||
                source->provenance->contract_count!=source->function_count || !source->provenance->contracts)
                return XR_XIR_BAD_STRUCTURE;
            contract=&source->provenance->contracts[origin->function];
            if (contract->value_count && !contract->values) return XR_XIR_BAD_STRUCTURE;
        }
        if (value<body->parameter_count) {
            if (source->provenance && (origin->effect_argument_count!=body->parameter_count ||
                !origin->effect_arguments || origin->effect_arguments[value].parameter!=value ||
                origin->effect_arguments[value].type!=actual)) return XR_XIR_BAD_STRUCTURE;
            request.source=module->types;request.arguments=NULL;request.argument_count=0;request.declared=actual;
        } else {
            uint32_t instruction=value-body->parameter_count;
            request.declared=original->instructions[instruction].type;
            if (contract && builder->identity<contract->value_count) {
                if (!xir_compile_work(builder->work,2)) return XR_XIR_BUDGET;
                const XrXirRootValueIdentity *identity=&contract->values[builder->identity];
                if (identity->instruction<instruction) return XR_XIR_BAD_STRUCTURE;
                if (identity->instruction==instruction) { request.declared=identity->declared_type;++builder->identity; }
            }
        }
        status=effect_invocation_instance_bound(builder->work,&request,&record);
        if (status==XR_XIR_OK && value+1==builder->bounds->functions[function].values &&
            contract && builder->identity!=contract->value_count) status=XR_XIR_BAD_STRUCTURE;
    } else {
        if (value<body->parameter_count && builder->refinement)
            declared=builder->refinement->functions[function].parameters[value];
        else if (value>=body->parameter_count && builder->instructions)
            declared=builder->instructions[value-body->parameter_count];
        const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,declared);
        bool current=xr_xir_callable_signature(module->types,actual)!=NULL;
        if (!!signature!=current || (signature && !xr_xir_callable_flags_valid(signature->flags))) return XR_XIR_BAD_TYPE;
        if (signature) record=(EffectInvocationValueBound){.mask=signature->flags&
            (XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED),.callable=true};
    }
    if (status==XR_XIR_OK && record.callable) { record.value=value;status=effect_body_advertisements_append(builder,record); }
    if (status!=XR_XIR_OK) return status;
    ++builder->next;
    if (builder->next==builder->bounds->functions[function].values) { ++builder->function;builder->open=false; }
    return XR_XIR_OK;
}
/* Only a declaration-identical non-generic definition can aggregate its
 * advertisement intake. Other scopes keep every original per-value proof;
 * the independent Cell/physical span does not grant any declared bound. */
static bool effect_body_advertisements_span_ready(const EffectBodyAdvertisementBuilder *builder,uint32_t f) {
    if (!builder || !builder->open || f!=builder->function || f>=builder->bounds->count ||
        builder->instructions || (builder->source->provenance &&
        builder->source->provenance->kind==XR_XIR_EVIDENCE_INSTANCE)) return false;
    const XrXirModule *source=builder->source;
    return !source->generics || !source->generics[f].parameter_count;
}
static XrXirStatus effect_body_advertisements_span(EffectBodyAdvertisementBuilder *builder,
    uint32_t f,uint32_t value,uint32_t count,XrXirType actual) {
    if (!builder || !builder->open || f!=builder->function || f>=builder->bounds->count ||
        !count || value!=builder->next || value>=builder->bounds->functions[f].values ||
        count>builder->bounds->functions[f].values-value)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=&builder->source->functions[f];
    if (value<body->parameter_count || value-body->parameter_count>=body->instruction_count ||
        body->instructions[value-body->parameter_count].type!=actual ||
        (actual!=XR_XIR_UNIT && actual!=XR_XIR_BOOL && actual!=XR_XIR_I64 &&
        actual!=XR_XIR_F64 && actual!=XR_XIR_RUNE) || xr_xir_callable_signature(builder->source->types,actual))
        return XR_XIR_BAD_TYPE;
    if (!effect_body_advertisements_span_ready(builder,f)) {
        if (!xir_compile_work(builder->work,7)) return XR_XIR_BUDGET;
        for (uint32_t i=0;i<count;++i) {
            if (!xir_compile_work(builder->work,1)) return XR_XIR_BUDGET;
            XrXirStatus status=effect_body_advertisements_value(builder,f,value+i,actual);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    }
    if (!xir_compile_work(builder->work,7+sizeof(builder->next))) return XR_XIR_BUDGET;
    builder->next+=count;
    if (builder->next==builder->bounds->functions[f].values) { ++builder->function;builder->open=false; }
    return XR_XIR_OK;
}
static XrXirStatus effect_body_advertisements_finish(EffectBodyAdvertisementBuilder **input,
    EffectInvocationDeclaredBounds **output) {
    if (!input || !*input || !output || *output) return XR_XIR_BAD_STRUCTURE;
    EffectBodyAdvertisementBuilder *builder=*input;
    if (builder->open || builder->function!=builder->bounds->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(builder->work,2)) return XR_XIR_BUDGET;
    *output=builder->bounds;builder->bounds=NULL;*input=NULL;effect_body_advertisements_free(builder);return XR_XIR_OK;
}
