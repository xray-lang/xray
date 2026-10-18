/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_root_seeds.inc.c - Public environment seeds from actual parameter types
 *
 * KEY CONCEPT:
 *   The same current Input rule constructs and verifies a root key. A role is
 *   an operand whose derivation is proved separately; stored advertisements do not qualify it.
 */
static XrXirStatus effect_role_range(const XrXirCompileContext *work,const void *pointer,uint64_t count,size_t size);
typedef struct EffectInvocationSeedRead {
    const XrXirCompileContext *work;
    const XrXirTypes *types;
    EffectBodyFunctionSignature head;
    EffectInvocationBasis basis;
    uint32_t sites;
} EffectInvocationSeedRead;
typedef struct EffectInvocationSeedBits {
    uint32_t origins[3],cells[2],origin_count,cell_count;
} EffectInvocationSeedBits;
static XrXirStatus effect_invocation_seed_open(const XrXirCompileContext *work,
    const EffectBodyRequest *body,const EffectInvocationFinal *final,uint32_t function,EffectInvocationSeedRead *output) {
    if(!body || !output || function>=body->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,final,1,sizeof(*final));if(status!=XR_XIR_OK) return status;
    status=effect_role_range(work,final->root_spaces,body->domain.function_count,sizeof(*final->root_spaces));
    if(status!=XR_XIR_OK) return status;
    uint32_t root=final->root_spaces[function];if(root>=body->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    /* Seeds consume current parameter types and their namespace dimensions.
     * Keep only those projections; no copied body adapter or unused block,
     * name, instruction and operand vectors enter the seed lifetime. */
    EffectBodyFunctionSignature head={0};EffectBodyFunctionDimensions space={0};
    status=effect_body_request_signature(body,function,&head);
    if(status==XR_XIR_OK) {
        /* Only the namespace's actual formal count enters the basis. The
         * current compile read has no callback between these projections. */
        if(root==function && body->read.consume==effect_body_compile_consume)
            space.parameter_count=head.parameter_count;
        else status=effect_body_request_dimensions(body,root,&space);
    }
    if(status==XR_XIR_OK) status=effect_role_range(work,head.parameters,head.parameter_count,sizeof(*head.parameters));
    const XrXirTypes *types=body->domain.types;
    if(status==XR_XIR_OK) status=effect_role_range(work,types,1,sizeof(*types));
    if(status==XR_XIR_OK) status=effect_role_range(work,types->nodes,types->count,sizeof(*types->nodes));
    if(status!=XR_XIR_OK) return status;
    if(types->nominals) {
        const XrXirNominalTable *table=types->nominals;
        status=effect_role_range(work,table,1,sizeof(*table));if(status!=XR_XIR_OK) return status;
        if(table->count && !!table->declarations==!!table->identities) return XR_XIR_BAD_STRUCTURE;
        if(table->declarations) status=effect_role_range(work,table->declarations,table->count,sizeof(*table->declarations));
        else if(table->identities) status=effect_role_range(work,table->identities,table->count,sizeof(*table->identities));
        if(status!=XR_XIR_OK) return status;
    }
    EffectInvocationBasis basis={0};status=effect_invocation_basis_parameters_read(work,final,space.parameter_count,&basis);
    if(status!=XR_XIR_OK) return status;
    if(!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(EffectInvocationSeedRead){work,types,head,basis,final->site_count};return XR_XIR_OK;
}
static XrXirStatus effect_invocation_seed_bits(const EffectInvocationSeedRead *read,
    uint32_t parameter,uint32_t kind,EffectInvocationSeedBits *output) {
    if(!read || !output || parameter>=read->head.parameter_count || kind>XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC)
        return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(read->work,5)) return XR_XIR_BUDGET;
    output->origin_count=0;output->cell_count=0;
    const XrXirTypes *types=read->types;
    const XrXirTypeNode *node=xr_xir_type_node(types,read->head.parameters[parameter]);
    if(!node) return XR_XIR_OK;
    bool object=false;
    if(node->kind==XR_XIR_TYPE_NOMINAL) {
        const XrXirNominalTable *table=types->nominals;
        if(!table || node->nominal.declaration>=table->count) return XR_XIR_BAD_STRUCTURE;
        uint32_t category=table->declarations?table->declarations[node->nominal.declaration].kind:
            table->identities[node->nominal.declaration].kind;
        object=category==XR_XIR_NOMINAL_CLASS;
    }
    if(node->kind!=XR_XIR_TYPE_CELL && node->kind!=XR_XIR_TYPE_CALLABLE && node->kind!=XR_XIR_TYPE_ITERATOR && !object) return XR_XIR_OK;
    if(parameter>=read->basis.parameters) return XR_XIR_BAD_STRUCTURE;
    if(node->kind==XR_XIR_TYPE_CELL || object) {
        if(!xir_compile_work(read->work,2*sizeof(uint32_t)+1)) return XR_XIR_BUDGET;
        output->cells[0]=object?read->basis.parameters+1:parameter;
        output->cells[1]=read->basis.parameters+2;output->cell_count=2;return XR_XIR_OK;
    }
    if(node->kind==XR_XIR_TYPE_ITERATOR) {
        if(!xir_compile_work(read->work,1+sizeof(uint32_t))) return XR_XIR_BUDGET;
        output->origins[0]=read->sites+parameter;output->origin_count=1;return XR_XIR_OK;
    }
    if(!xr_xir_callable_flags_valid(node->flags) || !!node->parameters!=!!node->parameter_count) return XR_XIR_BAD_TYPE;
    XrXirStatus status=effect_role_range(read->work,node->parameters,node->parameter_count,sizeof(*node->parameters));
    if(status!=XR_XIR_OK) return status;
    bool ref=false;
    for(uint32_t p=0;p<node->parameter_count;++p) {
        if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        if(!xr_xir_callable_parameter_storage_valid(types,&node->parameters[p])) return XR_XIR_BAD_TYPE;
        if(node->parameters[p].mode==XR_PARAM_REF) ref=true;
    }
    uint32_t mask=node->flags&(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    uint32_t opaque=read->sites+read->basis.parameters;
    if(!xir_compile_work(read->work,8)) return XR_XIR_BUDGET;
    if(mask && ((ref && (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)) || kind==XR_XIR_EFFECT_PARAMETER_VARIABLE)) {
        output->origins[output->origin_count++]=read->sites+parameter;
        if(ref && (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)) {
            output->origins[output->origin_count++]=opaque;
            output->origins[output->origin_count++]=opaque+1;
        }
    } else {
        output->origins[output->origin_count++]=opaque;
        if(mask&XR_XIR_CALLABLE_ROOT_REQUIRED) output->origins[output->origin_count++]=opaque+1;
        if(mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED) output->origins[output->origin_count++]=opaque+2;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_invocation_seed_word(const EffectInvocationSeedRead *read,
    const EffectInvocationSeedBits *bits,uint32_t word,uint64_t *output) {
    if(!read || !bits || !output || word>=read->basis.row_words || bits->origin_count>3 || bits->cell_count>2)
        return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(read->work,2+bits->origin_count+bits->cell_count)) return XR_XIR_BUDGET;
    uint64_t result=0;
    for(uint32_t b=0;b<bits->origin_count;++b) {
        if(bits->origins[b]/64>=read->basis.origin_words) return XR_XIR_BAD_STRUCTURE;
        if(word==bits->origins[b]/64) result|=UINT64_C(1)<<(bits->origins[b]%64);
    }
    for(uint32_t b=0;b<bits->cell_count;++b) {
        if(bits->cells[b]/64>=read->basis.cell_words) return XR_XIR_BAD_STRUCTURE;
        if(word==read->basis.fn_words+bits->cells[b]/64) result|=UINT64_C(1)<<(bits->cells[b]%64);
    }
    *output=result;return XR_XIR_OK;
}
