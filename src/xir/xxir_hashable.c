/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xxir_hashable_internal.h"
#include "xxir_types.h"
#include "xxir_nominal.h"
#include "xxir_receive_control.h"
#include "xxir_constraint_proof_internal.h"
#include "xxir_effect_contract_internal.h"
#include "xxir_type_match_internal.h"

typedef struct SignatureIdentity {XrCompileRequestControl *control;uint64_t work;XrXirStatus status;} SignatureIdentity;
static bool signature_identity_bytes(SignatureIdentity *c,XrXirLiteral a,XrXirLiteral b) {
    if(a.length!=b.length)return false;
    if(a.length>c->work){c->status=XR_XIR_BUDGET;return false;}
    c->work-=a.length;int order=0;
    c->status=xir_receive_compare(c->control,a.bytes,b.bytes,a.length,&order);
    return c->status==XR_XIR_OK && !order;
}
static bool signature_nominal_identity(void *owner,const XrXirTypes *source,
    const XrXirTypeNode *from,const XrXirTypes *actual,const XrXirTypeNode *to) {
    SignatureIdentity *c=owner;
    if(c->status!=XR_XIR_OK || !source || !actual || !source->nominals || !actual->nominals ||
        from->nominal.declaration!=to->nominal.declaration ||
        from->nominal.declaration>=source->nominals->count || to->nominal.declaration>=actual->nominals->count)return false;
    uint32_t d=from->nominal.declaration;
    const XrXirNominalTable *a=source->nominals,*b=actual->nominals;
    if((!a->declarations && !a->identities) || (!b->declarations && !b->identities))return false;
    XrXirLiteral am=a->declarations?a->declarations[d].module:a->identities[d].module;
    XrXirLiteral bm=b->declarations?b->declarations[d].module:b->identities[d].module;
    XrXirLiteral an=a->declarations?a->declarations[d].name:a->identities[d].name;
    XrXirLiteral bn=b->declarations?b->declarations[d].name:b->identities[d].name;
    uint32_t ak=a->declarations?a->declarations[d].kind:a->identities[d].kind;
    uint32_t bk=b->declarations?b->declarations[d].kind:b->identities[d].kind;
    uint32_t aa=a->declarations?a->declarations[d].parameter_count:a->identities[d].arity;
    uint32_t ba=b->declarations?b->declarations[d].parameter_count:b->identities[d].arity;
    uint32_t af=a->declarations?a->declarations[d].flags:a->identities[d].flags;
    uint32_t bf=b->declarations?b->declarations[d].flags:b->identities[d].flags;
    uint32_t ai=a->declarations?a->declarations[d].native.native_id:a->identities[d].native.native_id;
    uint32_t bi=b->declarations?b->declarations[d].native.native_id:b->identities[d].native.native_id;
    uint32_t ap=a->declarations?a->declarations[d].parent:a->identities[d].parent;
    uint32_t bp=b->declarations?b->declarations[d].parent:b->identities[d].parent;
    return ak==bk && aa==ba && af==bf && ai==bi && ap==bp &&
        signature_identity_bytes(c,am,bm) && signature_identity_bytes(c,an,bn);
}

/* A closed function's declaration name belongs to the retained Source. This
 * receiving prior is independent of witness candidates and physical mangling:
 * it authenticates the real owner and every substituted signature type. The
 * complete body/effects/name correspondence is still checked separately. */
static XrXirStatus signature_declaration(const XrXirCompileContext *work,
    const XrXirModule *module,uint32_t index,XirReadScratch *read,
    XrCompileRequestControl *control,const XrXirFunction **output) {
    const XrXirFunction *actual=&module->functions[index];
    if(!xir_effect_evidence_is_closed(module)){*output=actual;return XR_XIR_OK;}
    const XrXirProvenance *p=module->provenance;
    if(!p->source || p->count!=module->function_count || !p->origins)return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *source=xr_xir_compile_artifact_module(p->source);
    const XrXirOrigin *origin=&p->origins[index];
    if(!source || source->stage!=XR_XIR_CHECKED || xir_effect_evidence_is_closed(source) ||
        !source->functions || origin->function>=source->function_count ||
        origin->argument_count!=(source->generics?source->generics[origin->function].parameter_count:0) ||
        (origin->argument_count && !origin->arguments))return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *declared=&source->functions[origin->function];
    if(declared->parameter_count!=actual->parameter_count ||
        (declared->parameter_count && (!declared->parameters || !actual->parameters)))return XR_XIR_BAD_TYPE;
    if(declared->parameter_count>work->limits.parameters || origin->argument_count>work->limits.parameters)
        return XR_XIR_BUDGET;
    if(!!source->declarations!=!!module->declarations)return XR_XIR_BAD_STRUCTURE;
    if(source->declarations) {
        if(!source->declarations->functions || !module->declarations->functions)return XR_XIR_BAD_STRUCTURE;
        const XrXirFunctionIdentity *from=&source->declarations->functions[origin->function];
        const XrXirFunctionIdentity *to=&module->declarations->functions[index];
        if(from->module!=to->module || from->module>=source->declarations->module_count ||
            to->module>=module->declarations->module_count || !source->declarations->modules ||
            !module->declarations->modules || from->nominal_owner!=to->nominal_owner ||
            from->member_access!=to->member_access || from->method_kind!=to->method_kind ||
            from->native_callable!=to->native_callable || from->capture_count!=to->capture_count ||
            from->promises!=to->promises)return XR_XIR_BAD_STRUCTURE;
        const XrXirSourceModule *a=&source->declarations->modules[from->module];
        const XrXirSourceModule *b=&module->declarations->modules[to->module];
        SignatureIdentity identity={control,work->limits.instructions,XR_XIR_OK};
        if(!signature_identity_bytes(&identity,(XrXirLiteral){a->name,a->name_length},
            (XrXirLiteral){b->name,b->name_length}))return identity.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:identity.status;
    }
    for(uint32_t a=0;a<origin->argument_count;++a) {
        XrXirStatus s=xir_receive_poll(control);if(s!=XR_XIR_OK)return s;
        const XrXirTypeNode *n=xr_xir_type_node(module->types,origin->arguments[a]);
        if(n && n->parameter_span)return XR_XIR_BAD_TYPE;
        if(!n) {s=xr_xir_compile_type_expression_shape(work,module->types,origin->arguments[a],0);
            if(s!=XR_XIR_OK)return s;}
    }
    XirTypeMatchRead match={source->types,module->types,origin->arguments,origin->argument_count,
        declared->result,actual->result,read};
    SignatureIdentity identity={control,work->limits.instructions,XR_XIR_OK};
    XrXirStatus s=xir_type_match_read_controlled(work,&match,control,&identity,signature_nominal_identity);
    if(s==XR_XIR_OK){match.expected=declared->error_type;match.actual=actual->error_type;
        s=xir_type_match_read_controlled(work,&match,control,&identity,signature_nominal_identity);}
    for(uint32_t a=0;s==XR_XIR_OK && a<declared->parameter_count;++a) {
        match.expected=declared->parameters[a];match.actual=actual->parameters[a];
        s=xir_type_match_read_controlled(work,&match,control,&identity,signature_nominal_identity);
    }
    if(identity.status!=XR_XIR_OK)return identity.status;
    if(s==XR_XIR_OK)*output=declared;
    return s;
}

XrXirStatus xir_metadata_module_signature(const void *owner,const XrXirCompileContext *work,
    uint32_t index,XirReadScratch *read,XrCompileRequestControl *control,XirMetadataFunctionSignature *output) {
    const XrXirModule *module=owner;
    if(!xir_compile_context_valid(work) || !module || !output || !module->functions || index>=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *f=&module->functions[index];
    if((f->name_length && !f->name) || (f->parameter_count && !f->parameters) ||
        (f->consume_parameter_count && !f->consume_parameters) ||
        (f->escaping_parameter_count && !f->escaping_parameters)) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *declaration=NULL;
    XrXirStatus s=signature_declaration(work,module,index,read,control,&declaration);
    if(s!=XR_XIR_OK)return s;
    *output=(XirMetadataFunctionSignature){declaration->name,declaration->name_length,f->parameters,
        f->parameter_count,f->result,f->error_type,f->consume_parameters,
        f->consume_parameter_count,f->escaping_parameters,f->escaping_parameter_count};
    return XR_XIR_OK;
}
static const XrXirTypeNode *hashable_node(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *n=xr_xir_type_node(types,type);
    return n && n->kind==XR_XIR_TYPE_NOMINAL ? n : NULL;
}
static int hashable_order(const XrXirTypes *types,const XrXirHashableWitness *a,
    const XrXirHashableWitness *b) {
    const XrXirTypeNode *an=hashable_node(types,a->nominal_type),*bn=hashable_node(types,b->nominal_type);
    if(an->nominal.declaration!=bn->nominal.declaration)
        return an->nominal.declaration<bn->nominal.declaration?-1:1;
    if(!!an->parameter_span!=!!bn->parameter_span) return an->parameter_span?-1:1;
    return a->nominal_type<b->nominal_type?-1:a->nominal_type!=b->nominal_type;
}
XrXirStatus xir_hashable_witnesses_shape(const XrXirCompileContext *work,
    const XrXirTypes *types,const XrXirDeclarations *d,uint32_t functions,XrCompileRequestControl *control) {
    if(!xir_compile_context_valid(work) || !d ||
        (d->hashable_witness_count!=0)!=(d->hashable_witnesses!=NULL))return XR_XIR_BAD_STRUCTURE;
    if(d->hashable_witness_count>work->limits.instructions ||
        (uint64_t)d->hashable_witness_count*sizeof(*d->hashable_witnesses)>SIZE_MAX)return XR_XIR_BUDGET;
    for(uint32_t i=0;i<d->hashable_witness_count;++i) {
        XrXirStatus s=xir_receive_poll(control);if(s!=XR_XIR_OK)return s;
        const XrXirHashableWitness *r=&d->hashable_witnesses[i];
        if(r->equals_function>=functions || r->hash_function>=functions ||
            r->equals_function==r->hash_function)return XR_XIR_BAD_STRUCTURE;
        if(!types)continue; /* clone owns spans; the receiving type prior follows */
        const XrXirTypeNode *n=hashable_node(types,r->nominal_type);
        if(!n || !types->nominals || n->nominal.declaration>=types->nominals->count)
            return XR_XIR_BAD_TYPE;
        if(i) {
            const XrXirHashableWitness *p=&d->hashable_witnesses[i-1];
            const XrXirTypeNode *pn=hashable_node(types,p->nominal_type);
            if(!pn || hashable_order(types,p,r)>=0 ||
                (n->parameter_span && pn->parameter_span &&
                 n->nominal.declaration==pn->nominal.declaration))return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus hashable_sift(const XrXirTypes *types,
    XrXirHashableWitness *rows,uint32_t count,uint32_t root,uint64_t *steps,
    XrCompileRequestControl *control) {
    while(root<count/2) {
        if(!*steps)return XR_XIR_BUDGET;
        --*steps;
        XrXirStatus s=xir_receive_poll(control);if(s!=XR_XIR_OK)return s;
        uint32_t child=root*2u+1u;
        if(child+1<count && hashable_order(types,&rows[child],&rows[child+1])<0)++child;
        if(hashable_order(types,&rows[root],&rows[child])>=0)break;
        XrXirHashableWitness swap=rows[root];rows[root]=rows[child];rows[child]=swap;root=child;
    }
    return XR_XIR_OK;
}
XrXirStatus xir_hashable_witnesses_sort(const XrXirCompileContext *work,
    const XrXirTypes *types,XrXirHashableWitness *rows,uint32_t count,XrCompileRequestControl *control) {
    if(!xir_compile_context_valid(work) || (count && !rows))return XR_XIR_BAD_STRUCTURE;
    uint64_t steps=work->limits.instructions;
    if(count>steps)return XR_XIR_BUDGET;
    for(uint32_t i=0;i<count;++i) {
        --steps;XrXirStatus s=xir_receive_poll(control);if(s!=XR_XIR_OK)return s;
        if(!hashable_node(types,rows[i].nominal_type))return XR_XIR_BAD_TYPE;
    }
    for(uint32_t i=count/2;i;) {
        XrXirStatus s=hashable_sift(types,rows,count,--i,&steps,control);if(s!=XR_XIR_OK)return s;
    }
    for(uint32_t end=count;end>1;) {
        --end;XrXirHashableWitness swap=rows[0];rows[0]=rows[end];rows[end]=swap;
        XrXirStatus s=hashable_sift(types,rows,end,0,&steps,control);if(s!=XR_XIR_OK)return s;
    }
    return xir_receive_poll(control);
}
static XrXirStatus hashable_method(const XrXirCompileContext *work,const XirMetadataDomain *d,
    const XrXirHashableWitness *row,const XrXirTypeNode *node,uint32_t index,bool equals,
    XirReadScratch *read,XrCompileRequestControl *control) {
    if(!d->signature || !d->signature_owner || !d->declarations->functions || index>=d->function_count)
        return XR_XIR_BAD_TYPE;
    const XrXirFunctionIdentity *id=&d->declarations->functions[index];
    if(id->nominal_owner!=node->nominal.declaration+1 || id->method_kind!=XR_XIR_READ_METHOD ||
        id->member_access==XR_XIR_MEMBER_PRIVATE || id->member_access>XR_XIR_MEMBER_PROTECTED ||
        id->native_callable || id->cleanup_owner ||
        id->capture_owner || id->capture_count || id->test_role ||
        (id->promises&(XR_XIR_FUNCTION_GENERATOR|XR_XIR_FUNCTION_REST)) ||
        id->module>=d->declarations->module_count)return XR_XIR_BAD_TYPE;
    const XrXirNominalTable *table=d->types->nominals;
    if(!table->declarations && !table->identities)return XR_XIR_BAD_TYPE;
    XrXirLiteral module=table->declarations?table->declarations[node->nominal.declaration].module:
        table->identities[node->nominal.declaration].module;
    const XrXirSourceModule *actual=&d->declarations->modules[id->module];
    if(actual->name_length!=module.length)return XR_XIR_BAD_TYPE;
    if(module.length>work->limits.instructions)return XR_XIR_BUDGET;
    int order=0;XrXirStatus s=xir_receive_compare(control,actual->name,module.bytes,module.length,&order);
    if(s!=XR_XIR_OK)return s;
    if(order)return XR_XIR_BAD_TYPE;
    XirMetadataFunctionSignature f={0};s=d->signature(d->signature_owner,work,index,read,control,&f);
    if(s!=XR_XIR_OK)return s;
    uint32_t length=equals?6u:4u;
    if(f.name_length!=length || !f.name || memcmp(f.name,equals?"equals":"hash",length) ||
        f.parameter_count!=(equals?2u:1u) || !f.parameters ||
        f.parameters[0]!=row->nominal_type || (equals && f.parameters[1]!=row->nominal_type) ||
        f.result!=(equals?XR_XIR_BOOL:XR_XIR_I64) || f.error_type!=XR_XIR_NEVER ||
        f.consume_parameter_count || f.escaping_parameter_count)return XR_XIR_BAD_TYPE;
    uint32_t binders=d->generics?d->generics[index].parameter_count:0;
    if(binders!=(node->parameter_span?node->nominal.argument_count:0))return XR_XIR_BAD_TYPE;
    return xir_receive_poll(control);
}
XrXirStatus xir_hashable_witness_targets(const XrXirCompileContext *work,
    const XirMetadataProofContext *context,XrXirType type,XirReadScratch *read,
    XrCompileRequestControl *control,XirHashableTargets *output) {
    if(!xir_compile_context_valid(work) || !context || !context->domain || !output)
        return XR_XIR_BAD_STRUCTURE;
    const XirMetadataDomain *d=context->domain;
    const XrXirTypeNode *subject=hashable_node(d->types,type);
    if(!subject || !d->types->nominals || subject->nominal.declaration>=d->types->nominals->count ||
        !d->declarations || !d->declarations->hashable_witnesses)return XR_XIR_BAD_TYPE;
    const XrXirDeclarations *decl=d->declarations;
    uint32_t lo=0,hi=decl->hashable_witness_count;
    uint64_t steps=work->limits.instructions;
    while(lo<hi) {
        if(!steps--)return XR_XIR_BUDGET;
        XrXirStatus s=xir_receive_poll(control);if(s!=XR_XIR_OK)return s;
        uint32_t mid=lo+(hi-lo)/2;
        const XrXirTypeNode *n=hashable_node(d->types,decl->hashable_witnesses[mid].nominal_type);
        if(!n)return XR_XIR_BAD_TYPE;
        if(n->nominal.declaration<subject->nominal.declaration)lo=mid+1;else hi=mid;
    }
    if(lo==decl->hashable_witness_count)return XR_XIR_BAD_TYPE;
    const XrXirHashableWitness *row=&decl->hashable_witnesses[lo];
    const XrXirTypeNode *node=hashable_node(d->types,row->nominal_type);
    if(!node || node->nominal.declaration!=subject->nominal.declaration)return XR_XIR_BAD_TYPE;
    if(!node->parameter_span) {
        hi=decl->hashable_witness_count;
        while(lo<hi) {
            if(!steps--)return XR_XIR_BUDGET;
            XrXirStatus s=xir_receive_poll(control);if(s!=XR_XIR_OK)return s;
            uint32_t mid=lo+(hi-lo)/2;
            const XrXirTypeNode *n=hashable_node(d->types,decl->hashable_witnesses[mid].nominal_type);
            if(!n)return XR_XIR_BAD_TYPE;
            if(n->nominal.declaration<subject->nominal.declaration ||
                (n->nominal.declaration==subject->nominal.declaration && decl->hashable_witnesses[mid].nominal_type<type))lo=mid+1;
            else hi=mid;
        }
        if(lo>=decl->hashable_witness_count || decl->hashable_witnesses[lo].nominal_type!=type)return XR_XIR_BAD_TYPE;
        row=&decl->hashable_witnesses[lo];node=hashable_node(d->types,row->nominal_type);
    }
    if(!node || node->nominal.argument_count!=subject->nominal.argument_count ||
        (node->nominal.argument_count && (!node->nominal.arguments || !subject->nominal.arguments)))return XR_XIR_BAD_TYPE;
    if(node->parameter_span) {
        if(node->nominal.argument_count>steps)return XR_XIR_BUDGET;
        for(uint32_t a=0;a<node->nominal.argument_count;++a) {
            XrXirStatus s=xir_receive_poll(control);if(s!=XR_XIR_OK)return s;
            if(node->nominal.arguments[a]!=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+a))return XR_XIR_BAD_TYPE;
        }
    }
    XrXirStatus s=hashable_method(work,d,row,node,row->equals_function,true,read,control);
    if(s==XR_XIR_OK)s=hashable_method(work,d,row,node,row->hash_function,false,read,control);
    if(s!=XR_XIR_OK)return s;
    *output=(XirHashableTargets){{row->equals_function,row->hash_function},
        node->parameter_span?subject->nominal.arguments:NULL,node->parameter_span?subject->nominal.argument_count:0};
    return XR_XIR_OK;
}
XrXirStatus xir_hashable_witness_read(const XrXirCompileContext *work,
    const XirMetadataProofContext *context,XrXirType type,XirReadScratch *read,XrCompileRequestControl *control) {
    XirHashableTargets targets;
    return xir_hashable_witness_targets(work,context,type,read,control,&targets);
}
XrXirStatus xir_hashable_witnesses_verify(const XrXirCompileContext *work,
    const XrXirModule *module,XrCompileRequestControl *control) {
    if(!module)return XR_XIR_BAD_STRUCTURE;
    const XrXirDeclarations *d=module->declarations;
    if(!d)return XR_XIR_OK;
    XrXirStatus s=xir_hashable_witnesses_shape(work,module->types,d,module->function_count,control);
    if(s!=XR_XIR_OK)return s;
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    for(uint32_t i=0;i<d->hashable_witness_count;++i) {
        const XrXirHashableWitness *r=&d->hashable_witnesses[i];
        const XrXirTypeNode *n=hashable_node(module->types,r->nominal_type);
        XirMetadataProofContext context={&domain,{XR_XIR_CONTEXT_NOMINAL,n->nominal.declaration,0}};
        s=xir_hashable_witness_read(work,&context,r->nominal_type,NULL,control);
        if(s==XR_XIR_OK)s=xr_xir_compile_method_signature_verify(work,module,r->equals_function);
        if(s==XR_XIR_OK)s=xr_xir_compile_method_signature_verify(work,module,r->hash_function);
        if(s==XR_XIR_OK) {
            XrXirProofContext proof={module,context.owner};
            s=xir_compile_type_markers_prove_controlled(work,&proof,r->nominal_type,XR_XIR_CONSTRAINT_HASHABLE,control);
        }
        if(s!=XR_XIR_OK)return s;
    }
    return XR_XIR_OK;
}
