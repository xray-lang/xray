/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_local_errors.inc.c - Function-local declared error validation
 *
 * KEY CONCEPT:
 *   Every function checks its own escaping values against its declaration.
 *   Calls read signatures only. No function body, error summary or proof is
 *   retained after this synchronous check of the actual current input.
 */
#include "xxir_native_callable.h"

typedef struct LocalErrorAtom { XrXirType type;uint32_t variant; } LocalErrorAtom;
typedef struct LocalErrorCfg {
    const XrXirCompileContext *memory;
    const EffectBodyRequest *input;
    EffectBodyFunctionHead head;
    EffectTerms terms;
    XrXirInstruction *operations;
    XrXirType *types,*calls;
    LocalErrorAtom *atoms;
    uint32_t *slots,*roots,*queue;
    uint8_t *reachable,*queued;
    uint64_t *states,*row,*edge,*snapshot,*temporary;
    uint32_t function,block,instruction,values,active,atom_count,words,pending,front,back;
    size_t stride;
    XrXirType bound;
    XrXirDiagnostic *diagnostic;
} LocalErrorCfg;

static void *local_error_alloc(LocalErrorCfg *cfg,uint64_t count,size_t size,XrXirStatus *status) {
    if (*status!=XR_XIR_OK) return NULL;
    if (!count) return NULL;
    if (!size || count>SIZE_MAX/size) { *status=XR_XIR_OVERFLOW;return NULL; }
    return xir_compile_calloc(cfg->memory,(size_t)count,size,status);
}
static void local_error_free(LocalErrorCfg *cfg) {
    xr_compile_resources_free(cfg->operations);xr_compile_resources_free(cfg->types);
    xr_compile_resources_free(cfg->calls);xr_compile_resources_free(cfg->atoms);
    xr_compile_resources_free(cfg->slots);xr_compile_resources_free(cfg->roots);
    xr_compile_resources_free(cfg->queue);xr_compile_resources_free(cfg->reachable);
    xr_compile_resources_free(cfg->queued);xr_compile_resources_free(cfg->states);
    xr_compile_resources_free(cfg->row);xr_compile_resources_free(cfg->edge);
    xr_compile_resources_free(cfg->snapshot);xr_compile_resources_free(cfg->temporary);
    effect_terms_free(&cfg->terms);
}
static XrXirStatus local_error_failure(LocalErrorCfg *cfg,XrXirStatus status) {
    if (cfg->diagnostic) *cfg->diagnostic=(XrXirDiagnostic){status,cfg->function,
        cfg->block,cfg->instruction,XR_XIR_DIAGNOSTIC_NONE};
    return status;
}
static XrXirType local_error_payload(const XrXirTypes *types,XrXirType type) {
    return xr_xir_type_is_cell(types,type)?xr_xir_cell_element(types,type):type;
}
static bool local_error_active(const XrXirTypes *types,XrXirType type) {
    type=local_error_payload(types,type);
    return type==XR_XIR_ERROR || xr_xir_type_is_enum(types,type) ||
        ((uint32_t)type>=XR_XIR_TYPE_PARAMETER_BASE && (uint32_t)type<XR_XIR_TYPE_PARAMETER_LIMIT);
}
static bool local_error_copy(XrXirOp op) {
    return op==XR_XIR_COPY || op==XR_XIR_SCALAR_COPY || op==XR_XIR_ERROR_ERASE ||
        op==XR_XIR_ERROR_NARROW || op==XR_XIR_OWNED_RETAIN;
}
static uint64_t *local_error_value(LocalErrorCfg *cfg,uint64_t *row,uint32_t value) {
    return value<cfg->values && cfg->slots[value]!=UINT32_MAX ?
        row+(size_t)cfg->slots[value]*cfg->words:NULL;
}
static const uint64_t *local_error_read(LocalErrorCfg *cfg,const uint64_t *row,uint32_t value) {
    return value<cfg->values && cfg->slots[value]!=UINT32_MAX ?
        row+(size_t)cfg->slots[value]*cfg->words:cfg->temporary+cfg->words;
}
static bool local_error_any(const uint64_t *set,uint32_t words) {
    for (uint32_t w=0;w<words;++w) if (set[w]) return true;
    return false;
}
static bool local_error_join(uint64_t *to,const uint64_t *from,size_t words) {
    bool changed=false;
    for (size_t w=0;w<words;++w) { uint64_t joined=to[w]|from[w];changed|=joined!=to[w];to[w]=joined; }
    return changed;
}
static void local_error_type(LocalErrorCfg *cfg,XrXirType type,uint64_t *set) {
    type=local_error_payload(&cfg->terms.types,type);
    if (type==XR_XIR_ERROR) { set[0]|=1;return; }
    for (uint32_t a=0;a<cfg->atom_count;++a)
        if (xr_xir_error_bound_accepts(&cfg->terms.types,cfg->atoms[a].type,type)) set[(a+1)/64]|=UINT64_C(1)<<((a+1)%64);
}
static XrXirStatus local_error_subset(LocalErrorCfg *cfg,const uint64_t *set) {
    if (cfg->bound==XR_XIR_ERROR) return XR_XIR_OK;
    if (set[0]&1) return local_error_failure(cfg,XR_XIR_BAD_TYPE);
    for (uint32_t a=0;a<cfg->atom_count;++a)
        if ((set[(a+1)/64]&(UINT64_C(1)<<((a+1)%64))) && !xr_xir_error_bound_accepts(&cfg->terms.types,cfg->atoms[a].type,cfg->bound))
            return local_error_failure(cfg,XR_XIR_BAD_TYPE);
    return XR_XIR_OK;
}
static XrXirStatus local_error_substitute(LocalErrorCfg *cfg,const XrXirInstruction *op,
    XrXirType type,XrXirType *output) {
    uint32_t first=op->type_arguments[0],count=op->type_arguments[1];
    const XrXirGeneric *caller=cfg->input->domain.generics?&cfg->input->domain.generics[cfg->function]:NULL;
    if ((count && (!caller || !caller->arguments || first>caller->argument_count ||
        count>caller->argument_count-first)) || (!count && first)) return XR_XIR_BAD_STRUCTURE;
    XrXirGeneric arguments={0};arguments.argument_count=count;
    arguments.arguments=count?caller->arguments+first:NULL;
    return effect_terms_substitute(&cfg->terms,type,&arguments,output);
}
static XrXirStatus local_error_call(LocalErrorCfg *cfg,const XrXirInstruction *op,XrXirType *output) {
    *output=XR_XIR_NEVER;uint32_t target=UINT32_MAX;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE) {
        if (op->immediate<0 || (uint64_t)op->immediate>=cfg->input->domain.function_count) return XR_XIR_BAD_STRUCTURE;
        target=(uint32_t)op->immediate;
    } else if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        const uint32_t *identity=xr_xir_default_identity(op);const XrXirDefaultBinding *binding=NULL;
        XrXirStatus status=xir_metadata_default_lookup(cfg->memory,&cfg->input->domain,identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        if (!binding) return XR_XIR_BAD_STRUCTURE;
        target=binding->function;
    } else if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
        if (op->immediate<0 || (uint64_t)op->immediate>=cfg->values) return XR_XIR_BAD_STRUCTURE;
        *output=xr_xir_callable_error(cfg->input->domain.types,cfg->types[(uint32_t)op->immediate]);
        return *output==XR_XIR_UNIT?XR_XIR_BAD_TYPE:XR_XIR_OK;
    } else if (op->op==XR_XIR_CALL_REQUIREMENT) {
        const XrXirInterfaceTable *table=cfg->input->domain.types->interfaces;
        if (!table || !table->declarations || op->targets[0]>=table->count) return XR_XIR_BAD_STRUCTURE;
        const XrXirInterfaceDeclaration *owner=&table->declarations[op->targets[0]];
        if (op->targets[1]>=owner->method_count) return XR_XIR_BAD_STRUCTURE;
        XrXirType error=xr_xir_callable_error(cfg->input->domain.types,owner->methods[op->targets[1]].signature);
        if (error==XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
        return local_error_substitute(cfg,op,error,output);
    } else if (op->op==XR_XIR_TASK_AWAIT) {
        if (op->args[0]>=cfg->values) return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *task=xr_xir_type_node(&cfg->terms.types,cfg->types[op->args[0]]);
        XrXirType cancelled=xr_xir_cancelled_error_type(&cfg->terms.types);
        if (!task || task->kind!=XR_XIR_TYPE_TASK || task->result==XR_XIR_UNIT || cancelled==XR_XIR_UNIT)
            return XR_XIR_BAD_TYPE;
        XrXirCallableParameter errors[2]={{task->result,0},{cancelled,0}};
        *output=effect_term_intern(&cfg->terms,(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,
            .parameters=errors,.parameter_count=2});
        return cfg->terms.status;
    }
    else if (op->op==XR_XIR_ITERATOR_HAS_NEXT || op->op==XR_XIR_ITERATOR_NEXT || op->op==XR_XIR_ITERATOR_NTH) {
        if (op->args[0]>=cfg->values) return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *type=xr_xir_type_node(cfg->input->domain.types,cfg->types[op->args[0]]);
        if (!type || type->kind!=XR_XIR_TYPE_ITERATOR || type->result==XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
        *output=type->result;return XR_XIR_OK;
    } else return XR_XIR_OK;
    EffectBodyFunctionHead head={0};
    XrXirStatus status=effect_body_request_head(cfg->input,target,&head);
    if (status!=XR_XIR_OK) return status;
    return local_error_substitute(cfg,op,head.error_type,output);
}
static bool local_error_native(const EffectBodyRequest *input,uint32_t function) {
    const XrXirDeclarations *d=input->domain.declarations;
    return d && d->functions && function<input->domain.function_count && d->functions[function].native_callable;
}
static XrXirStatus local_error_native_header(const LocalErrorCfg *cfg) {
    const EffectBodyRequest *input=cfg->input;
    if (input->head==effect_body_raw_head)
        return xir_native_callable_verify(input->owner,cfg->function);
    const XrXirDeclarations *d=input->domain.declarations;
    const XrXirFunctionIdentity *id=&d->functions[cfg->function];
    const EffectBodyFunctionHead *head=&cfg->head;
    if (!xir_native_callable_identity(id) || id->module>=d->module_count || cfg->function==d->entry_function ||
        !head->name || !head->name_length || memchr(head->name,0,head->name_length) ||
        head->blocks || head->block_count || head->instruction_count || head->operands || head->operand_count ||
        (input->domain.generics && (input->domain.generics[cfg->function].parameter_count ||
            input->domain.generics[cfg->function].argument_count))) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t module=0;module<d->module_count;++module)
        if (d->modules[module].initializer==cfg->function) return XR_XIR_BAD_STRUCTURE;
    return xir_native_callable_types(id->native_callable,input->domain.types,head->parameters,
        head->parameter_count,head->result,head->error_type)?XR_XIR_OK:XR_XIR_BAD_TYPE;
}
static XrXirStatus local_error_prepare(LocalErrorCfg *cfg) {
    XrXirStatus status=effect_body_request_head(cfg->input,cfg->function,&cfg->head);
    if (status!=XR_XIR_OK) return status;
    cfg->bound=cfg->head.error_type;
    if (local_error_native(cfg->input,cfg->function)) return local_error_native_header(cfg);
    if (cfg->input->domain.declarations &&
        (cfg->input->domain.declarations->functions[cfg->function].promises&XR_XIR_FUNCTION_GENERATOR)) {
        const XrXirTypeNode *iterator=xr_xir_type_node(cfg->input->domain.types,cfg->head.result);
        if (!iterator || iterator->kind!=XR_XIR_TYPE_ITERATOR) return XR_XIR_BAD_TYPE;
        cfg->bound=iterator->result;
    }
    if (cfg->bound==XR_XIR_UNIT || !cfg->head.block_count ||
        (uint64_t)cfg->head.parameter_count+cfg->head.instruction_count>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    cfg->values=cfg->head.parameter_count+cfg->head.instruction_count;
    cfg->terms=(EffectTerms){.types=*cfg->input->domain.types,.remaining=cfg->memory};
    cfg->operations=local_error_alloc(cfg,cfg->head.instruction_count,sizeof(*cfg->operations),&status);
    cfg->types=local_error_alloc(cfg,cfg->values,sizeof(*cfg->types),&status);
    cfg->calls=local_error_alloc(cfg,cfg->head.instruction_count,sizeof(*cfg->calls),&status);
    cfg->slots=local_error_alloc(cfg,cfg->values,sizeof(*cfg->slots),&status);
    cfg->roots=local_error_alloc(cfg,cfg->values,sizeof(*cfg->roots),&status);
    if (status!=XR_XIR_OK) return status;
    for (uint32_t v=0;v<cfg->head.parameter_count;++v) cfg->types[v]=cfg->head.parameters[v];
    for (uint32_t i=0;i<cfg->head.instruction_count;++i) {
        XrXirInstruction storage;const XrXirInstruction *actual=NULL;
        status=effect_body_request_instruction(cfg->input,cfg->function,i,&storage,&actual);
        if (status!=XR_XIR_OK) return status;
        if (!actual || actual->op<=XR_XIR_INVALID || actual->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        cfg->operations[i]=*actual;cfg->types[cfg->head.parameter_count+i]=actual->type;
    }
    for (uint32_t v=0;v<cfg->values;++v) {
        cfg->slots[v]=local_error_active(&cfg->terms.types,cfg->types[v])?cfg->active++:UINT32_MAX;
        cfg->roots[v]=v;
        if (v>=cfg->head.parameter_count) {
            const XrXirInstruction *op=&cfg->operations[v-cfg->head.parameter_count];
            if (local_error_copy(op->op) && op->args[0]<v) cfg->roots[v]=cfg->roots[op->args[0]];
        }
    }
    for (uint32_t i=0;i<cfg->head.instruction_count;++i) {
        status=local_error_call(cfg,&cfg->operations[i],&cfg->calls[i]);
        if (status!=XR_XIR_OK) { cfg->instruction=i;return local_error_failure(cfg,status); }
    }
    return XR_XIR_OK;
}
static XrXirStatus local_error_storage(LocalErrorCfg *cfg) {
    const XrXirTypes *types=&cfg->terms.types;uint64_t atoms=0;
    uint32_t parameters=cfg->input->domain.generics?cfg->input->domain.generics[cfg->function].parameter_count:0;
    for (uint32_t t=0;t<types->count;++t) {
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        if (!xr_xir_type_is_enum(types,type)) continue;
        uint32_t n=types->nodes[t].nominal.declaration;
        atoms+=types->nominals->declarations?types->nominals->declarations[n].variant_count:types->nominals->identities[n].variant_count;
    }
    if (atoms+parameters>UINT32_MAX-1u) return XR_XIR_OVERFLOW;
    cfg->atom_count=(uint32_t)atoms+parameters;cfg->words=(uint32_t)((atoms+parameters+64)/64);
    XrXirStatus status=XR_XIR_OK;cfg->atoms=local_error_alloc(cfg,cfg->atom_count,sizeof(*cfg->atoms),&status);
    if (status!=XR_XIR_OK) return status;
    uint32_t at=0;
    for (uint32_t t=0;t<types->count;++t) {
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        if (!xr_xir_type_is_enum(types,type)) continue;
        uint32_t n=types->nodes[t].nominal.declaration;
        uint32_t count=types->nominals->declarations?types->nominals->declarations[n].variant_count:types->nominals->identities[n].variant_count;
        for (uint32_t v=0;v<count;++v) cfg->atoms[at++]=(LocalErrorAtom){type,v};
    }
    for (uint32_t p=0;p<parameters;++p) cfg->atoms[at++]=(LocalErrorAtom){(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p),UINT32_MAX};
    if ((uint64_t)cfg->active*cfg->words>SIZE_MAX/sizeof(uint64_t)) return XR_XIR_OVERFLOW;
    cfg->stride=(size_t)cfg->active*cfg->words;
    if (cfg->stride && cfg->head.block_count>SIZE_MAX/cfg->stride) return XR_XIR_OVERFLOW;
    cfg->states=local_error_alloc(cfg,(uint64_t)cfg->head.block_count*cfg->stride,sizeof(uint64_t),&status);
    cfg->row=local_error_alloc(cfg,cfg->stride,sizeof(uint64_t),&status);
    cfg->edge=local_error_alloc(cfg,cfg->stride,sizeof(uint64_t),&status);
    cfg->snapshot=local_error_alloc(cfg,cfg->stride,sizeof(uint64_t),&status);
    cfg->temporary=local_error_alloc(cfg,(uint64_t)cfg->words*2,sizeof(uint64_t),&status);
    cfg->queue=local_error_alloc(cfg,cfg->head.block_count,sizeof(uint32_t),&status);
    cfg->reachable=local_error_alloc(cfg,cfg->head.block_count,1,&status);
    cfg->queued=local_error_alloc(cfg,cfg->head.block_count,1,&status);
    return status;
}

#include "xxir_local_errors_transfer.inc.c"
#include "xxir_local_errors_cfg.inc.c"

static XrXirStatus local_errors_request(const XrXirCompileContext *memory,
    const EffectBodyRequest *input,XrXirDiagnostic *diagnostic) {
    for (uint32_t f=0;f<input->domain.function_count;++f) {
        LocalErrorCfg cfg={.memory=memory,.input=input,.function=f,.block=UINT32_MAX,
            .instruction=UINT32_MAX,.diagnostic=diagnostic};
        XrXirStatus status=local_error_prepare(&cfg);
        if (status==XR_XIR_OK && !local_error_native(input,f)) status=local_error_storage(&cfg);
        if (status==XR_XIR_OK && !local_error_native(input,f)) status=local_error_solve(&cfg);
        local_error_free(&cfg);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_local_errors_verify(const XrXirCompileContext *memory,
    const XrXirModule *module,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(memory) || !module) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest input=effect_body_raw_request(memory,module);XrXirTypes empty={0};
    if (!input.domain.types) input.domain.types=&empty;
    return local_errors_request(memory,&input,diagnostic);
}
static XrXirStatus local_errors_owned(const XrXirCompileContext *memory,
    const EffectBodyMaterial *material,XrXirDiagnostic *diagnostic) {
    if (!effect_body_material_shape(material) || material->resources!=memory->resources) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest input=effect_body_owned_request(memory,material);
    return local_errors_request(memory,&input,diagnostic);
}
static XrXirStatus local_error_outcomes(const XrXirCompileContext *memory,
    const EffectBodyMaterial *material,XrXirFunctionEffects *functions,bool compare) {
    EffectBodyRequest input=effect_body_owned_request(memory,material);
    for (uint32_t f=0;f<input.domain.function_count;++f) {
        EffectBodyFunctionHead head={0};XrXirStatus status=effect_body_request_head(&input,f,&head);
        if (status!=XR_XIR_OK) return status;
        XrXirType error=head.error_type;
        if (input.domain.declarations && (input.domain.declarations->functions[f].promises&XR_XIR_FUNCTION_GENERATOR)) {
            const XrXirTypeNode *iterator=xr_xir_type_node(input.domain.types,head.result);
            if (!iterator || iterator->kind!=XR_XIR_TYPE_ITERATOR) return XR_XIR_BAD_TYPE;
            error=iterator->result;
        }
        if (error==XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
        XrXirEffect bound=error==XR_XIR_NEVER?XR_XIR_EFFECT_NONE:XR_XIR_EFFECT_MAY;
        if (compare) { if (functions[f].throws!=bound) return XR_XIR_BAD_STRUCTURE; }
        else functions[f].throws=bound;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_worker_error_verify(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t function) {
    if (!xir_compile_context_valid(context) || !module || function>=module->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirType error=module->functions[function].error_type;
    if (error==XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
    if (error==XR_XIR_NEVER) return XR_XIR_OK;
    XrXirProofContext proof={module,{XR_XIR_CONTEXT_FUNCTION,function,0}};
    return xr_xir_compile_type_markers_prove(context,&proof,error,XR_XIR_CONSTRAINT_ERROR|XR_XIR_CONSTRAINT_SENDABLE);
}
