/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_constructor_boundary.inc.c - Ordinary constructor body boundaries
 */
#include "xxir_view_prepare_internal.h"
static XrXirStatus effect_class_constructor_boundary(XirEffectDefaultAccessScope *scope,
    EffectClassCallAccessQuery *query,const XrXirModule *module,uint32_t caller,bool *output) {
    *output=false;
    const XrXirInstruction *op=query->operation;
    const XrXirFunctionIdentity *identity=module->declarations->functions+query->callee;
    const XrXirTypeNode *type=xr_xir_type_node(module->types,op->type);
    const XrXirNominalTable *table=module->types?module->types->nominals:NULL;
    if(op->op!=XR_XIR_CALL || op->type_arguments[0] || op->type_arguments[1] ||
        identity->method_kind!=XR_XIR_CONSTRUCTOR || identity->native_callable ||
        identity->cleanup_owner || identity->test_role || !identity->exported ||
        identity->member_access!=XR_XIR_MEMBER_PUBLIC ||
        (identity->promises&~XR_XIR_FUNCTION_NO_SUSPEND) || query->target.error_type!=XR_XIR_NEVER ||
        !type || !xr_xir_type_is_class(module->types,op->type) || type->parameter_span ||
        type->nominal.argument_count || !table || type->nominal.declaration>=table->count ||
        identity->nominal_owner!=type->nominal.declaration+1u ||
        (!table->declarations && !table->identities))return XR_XIR_OK;
    if(module->generics && (module->generics[caller].parameter_count || module->generics[caller].argument_count ||
        module->generics[query->callee].parameter_count || module->generics[query->callee].argument_count))return XR_XIR_OK;
    uint32_t owner=type->nominal.declaration;
    const XrXirNominalDeclaration *decl=table->declarations?table->declarations+owner:NULL;
    const XrXirNominalIdentity *nominal=decl?NULL:table->identities+owner;
    XrXirLiteral name=decl?decl->module:nominal->module;
    const XrXirSourceModule *source=module->declarations->modules+identity->module;
    if((decl?decl->kind:nominal->kind)!=XR_XIR_NOMINAL_CLASS || !(decl?decl->exported:nominal->exported) ||
        (decl?decl->parameter_count:nominal->arity) || (decl?decl->native.native_id:nominal->native.native_id) ||
        !name.bytes || !source->name || name.length!=source->name_length ||
        memcmp(name.bytes,source->name,name.length))return XR_XIR_OK;
    /* Reuse ordinary call authority, parameter modes, consume and SSA checks
     * on this exact immutable generation instead of creating a second checker. */
    if(!scope->graph.input_facts || !scope->graph.input_facts->construction)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    if(!scope->ordinary_verified) {
        status=xir_compile_view_prepare_ordinary(&query->work,
            (XirEffectInput){module,scope->graph.input_facts->construction,scope->control},NULL);
        if(status!=XR_XIR_OK)return status;
        /* Each site still matches the complete immutable input. Only this
         * lexical scope shares a successful ordinary check of that input. */
        scope->ordinary_verified=true;
    }
    const XrXirFunction *body=module->functions+query->callee;
    uint32_t publication=UINT32_MAX,created=0,returned=0,steps=0;
    for(uint32_t i=0;i<body->instruction_count;++i) {
        status=xir_receive_step(scope->control,&steps);if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *operation=body->instructions+i;
        if(operation->op==XR_XIR_CLASS_NEW && operation->type==body->result) {
            if(created)return XR_XIR_OK;
            ++created;
        }
        if(operation->op!=XR_XIR_RETURN)continue;
        uint32_t value=operation->args[0];
        if(value<body->parameter_count || value-body->parameter_count>=i)return XR_XIR_OK;
        const XrXirInstruction *result=body->instructions+value-body->parameter_count;
        if(result->op!=XR_XIR_CLASS_NEW || result->type!=body->result ||
            (returned && value!=publication))return XR_XIR_OK;
        publication=value;++returned;
    }
    *output=created==1u && returned!=0;
    return XR_XIR_OK;
}
