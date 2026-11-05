/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_bindings.inc.c - Finite structural generic Error bindings
 *
 * Each selected pair replays the current call arguments and complete type
 * children. Receiving a binding allocates nothing and inherits no saved PASS.
 */
typedef struct ErrorTypeBinding {
    uint32_t function,instruction,first;
    XrXirType source,actual;
} ErrorTypeBinding;
struct EffectErrorBindings {
    XrCompileResources *resources;
    ErrorTypeBinding *items;
    EffectErrorMarkers *pairs;
    uint32_t count,capacity;
};
static void error_bindings_free(EffectErrorBindings *bindings) {
    if(!bindings) return;
    error_markers_free(bindings->pairs);xr_compile_resources_free(bindings->items);
    xr_compile_resources_free(bindings);
}
static XrXirStatus error_bindings_reserve(const XrXirCompileContext *work,EffectErrorBindings *bindings) {
    if(bindings->count<bindings->capacity) return XR_XIR_OK;
    if(bindings->capacity>UINT32_MAX/2) return XR_XIR_BUDGET;
    uint32_t capacity=bindings->capacity?bindings->capacity*2:8;
    if((uint64_t)capacity>SIZE_MAX/sizeof(*bindings->items)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    ErrorTypeBinding *items=xir_compile_alloc(work,(size_t)capacity*sizeof(*items),&status);
    if(!items) return status;

    if(bindings->count) memcpy(items,bindings->items,(size_t)bindings->count*sizeof(*items));
    xr_compile_resources_free(bindings->items);bindings->items=items;bindings->capacity=capacity;
    return XR_XIR_OK;
}
static XrXirStatus error_bindings_append(const XrXirCompileContext *work,EffectErrorFinite *proof,
    const ErrorTypeBinding *binding) {
    EffectErrorBindings *bindings=proof->bindings;
    XrXirStatus status=error_bindings_reserve(work,bindings);if(status!=XR_XIR_OK) return status;
    ErrorMarkerAppend request={.parent=UINT32_MAX,.ordinal=bindings->count,
        .goal={binding->function,ERROR_MARKER_CALL_PAIR,(uint32_t)binding->source,
            (uint32_t)binding->actual,binding->instruction,1}};
    uint32_t first=UINT32_MAX;status=error_marker_append(work,proof,bindings->pairs,&request,&first);
    if(status==XR_XIR_OK) status=error_marker_tree(work,proof,bindings->pairs,first);
    if(status!=XR_XIR_OK) return status;
    if(!bindings->pairs->nodes[first].value) return XR_XIR_BAD_STRUCTURE;

    bindings->items[bindings->count++]=(ErrorTypeBinding){binding->function,binding->instruction,first,
        binding->source,binding->actual};return XR_XIR_OK;
}
/* A matching attempt holds only current borrowed descriptors. Missing scratch
 * unwinds this whole attempt before its outer constructor can grow storage. */
static XrXirStatus error_bindings_match(const XrXirCompileContext *work,const EffectErrorFinite *proof,
    const ErrorTypeBinding *binding,XirReadScratch *memory) {
    XrXirStatus status=error_finite_types_frame(work,proof);if(status!=XR_XIR_OK) return status;
    const XrXirGeneric *generics=proof->material->generics;
    status=effect_role_range(work,generics,proof->functions,sizeof(*generics));if(status!=XR_XIR_OK) return status;
    if(!generics || binding->function>=proof->functions) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest body=effect_body_owned_request(work,proof->material);XrXirInstruction op;
    status=error_equation_operation(&(EffectErrorEquationRead){.body=body,.work=work,.function=binding->function},
        binding->instruction,&op);if(status!=XR_XIR_OK) return status;
    const XrXirGeneric *generic=&generics[binding->function];
    uint32_t begin=op.type_arguments[0],count=op.type_arguments[1];
    if(!count || begin>generic->argument_count || count>generic->argument_count-begin) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,generic->arguments,generic->argument_count,sizeof(*generic->arguments));
    if(status!=XR_XIR_OK) return status;
    const XrXirTypes *types=error_finite_types(proof);
    XirTypeMatchRead request={types,types,generic->arguments+begin,count,binding->source,binding->actual,memory};
    return xir_type_match_read(work,&request);
}
static XrXirStatus error_bindings_call(const XrXirCompileContext *work,EffectErrorFinite *proof,
    uint32_t function,uint32_t instruction,const XrXirInstruction *op,XirReadScratch *memory) {
    uint32_t count=op->type_arguments[1];
    for(uint32_t a=0;a<proof->atom_count;++a) {

        XrXirType type=proof->atoms[a].type;uint32_t span=xr_xir_type_span(error_finite_types(proof),type);
        if(proof->atoms[a].variant==XR_XIR_ERROR_SYMBOLIC_VARIANT || !span || span>count) continue;
        bool duplicate=false;
        for(uint32_t prior=0;prior<a;++prior) {

            if(proof->atoms[prior].type==type) { duplicate=true;break; }
        }
        if(duplicate) continue;
        for(uint32_t candidate=0;candidate<proof->atom_count;++candidate) {

            if(proof->atoms[candidate].variant==XR_XIR_ERROR_SYMBOLIC_VARIANT) continue;
            ErrorTypeBinding binding={function,instruction,0,type,proof->atoms[candidate].type};
            XrXirStatus status=XR_XIR_OK;
            for(;;) {
                status=xir_read_scratch_begin(work,memory);
                if(status==XR_XIR_OK) status=error_bindings_match(work,proof,&binding,memory);
                if(status!=XR_XIR_BUDGET || !memory->required) break;
                status=xir_read_scratch_grow(work,memory);if(status!=XR_XIR_OK) break;
            }
            if(status==XR_XIR_BAD_TYPE) continue;
            if(status!=XR_XIR_OK) return status;
            status=error_bindings_append(work,proof,&binding);if(status!=XR_XIR_OK) return status;
            break;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus error_bindings_build_attempt(const XrXirCompileContext *work,EffectErrorFinite *proof,
    XirReadScratch *memory) {
    bool needed=false;
    for(uint32_t a=0;a<proof->atom_count;++a) {

        needed|=proof->atoms[a].variant!=XR_XIR_ERROR_SYMBOLIC_VARIANT &&
            xr_xir_type_span(error_finite_types(proof),proof->atoms[a].type)!=0;
    }
    if(!needed) return XR_XIR_OK;
    XrXirStatus status=XR_XIR_OK;
    proof->bindings=xir_compile_calloc(work,1,sizeof(*proof->bindings),&status);if(!proof->bindings) return status;
    proof->bindings->resources=work->resources;
    proof->bindings->pairs=xir_compile_calloc(work,1,sizeof(*proof->bindings->pairs),&status);
    if(!proof->bindings->pairs) return status;
    proof->bindings->pairs->resources=work->resources;
    EffectBodyRequest body=effect_body_owned_request(work,proof->material);
    for(uint32_t f=0;f<proof->functions;++f) {
        EffectBodyFunctionHead head={0};status=effect_body_request_head(&body,f,&head);if(status!=XR_XIR_OK) return status;
        for(uint32_t i=0;i<head.instruction_count;++i) {
            XrXirInstruction op;
            status=error_equation_operation(&(EffectErrorEquationRead){.body=body,.work=work,.function=f},i,&op);
            if(status!=XR_XIR_OK) return status;
            if(!op.type_arguments[1] || (op.op!=XR_XIR_CALL && op.op!=XR_XIR_INVOKE && op.op!=XR_XIR_GO &&
                op.op!=XR_XIR_CALL_DEFAULT && op.op!=XR_XIR_INVOKE_DEFAULT)) continue;
            status=error_bindings_call(work,proof,f,i,&op,memory);if(status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus error_bindings_build(const XrXirCompileContext *work,EffectErrorFinite *proof) {
    XirReadScratch memory={.resources=work->resources};
    XrXirStatus status=error_bindings_build_attempt(work,proof,&memory);
    xir_read_scratch_free(&memory);return status;
}
static XrXirStatus error_finite_substitution(const XrXirCompileContext *work,
    const EffectErrorFinite *proof,uint32_t function,uint32_t instruction,XrXirType source,XrXirType *output) {
    const EffectErrorBindings *bindings=proof->bindings;
    XrXirStatus status=effect_role_range(work,bindings,1,sizeof(*bindings));if(status!=XR_XIR_OK) return status;
    if(!bindings || bindings->resources!=work->resources || bindings->count>bindings->capacity ||
        !!bindings->items!=!!bindings->capacity) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,bindings->items,bindings->capacity,sizeof(*bindings->items));
    if(status!=XR_XIR_OK) return status;
    status=error_marker_nodes_match(work,proof,bindings->pairs);if(status!=XR_XIR_OK) return status;
    const EffectErrorMarkers *pairs=bindings->pairs;
    for(uint32_t i=0;i<bindings->count;++i) {

        const ErrorTypeBinding *binding=&bindings->items[i];
        if(binding->function!=function || binding->instruction!=instruction || binding->source!=source) continue;
        if(binding->first>=pairs->count) return XR_XIR_BAD_STRUCTURE;
        const ErrorMarkerNode *root=&pairs->nodes[binding->first];
        ErrorMarkerGoal expected={function,ERROR_MARKER_CALL_PAIR,(uint32_t)source,
            (uint32_t)binding->actual,instruction,1};
        if(root->parent!=UINT32_MAX || root->ordinal!=i || !root->value ||
            !error_marker_goal_equal(&root->goal,&expected)) return XR_XIR_BAD_STRUCTURE;

        *output=binding->actual;return XR_XIR_OK;
    }
    return XR_XIR_BAD_STRUCTURE;
}
