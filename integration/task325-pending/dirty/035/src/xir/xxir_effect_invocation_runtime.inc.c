/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_runtime.inc.c - Existing sealed execution edge selection
 *
 * KEY CONCEPT:
 *   Runtime readers compare actual inputs with completed owned equations.
 *   No node, origin, role, advertisement or effect fact is created here.
 */
static XrXirStatus effect_runtime_work(const XirEffectRuntimeRead *read,uint64_t units) {
    return read && read->work && read->work(read->owner,units)?XR_XIR_OK:XR_XIR_BUDGET;
}

static XrXirStatus effect_runtime_basis(const EffectInvocationCertificate *certificate,
    uint32_t root,EffectInvocationBasis *output,const XirEffectRuntimeRead *read) {
    const EffectInvocationFinal *owner=certificate->final;
    if (root>=effect_body_count(certificate->body)) return XR_XIR_BAD_STRUCTURE;
    uint32_t p=certificate->body->functions[root].parameter_count;
    uint64_t fn_bits=(uint64_t)owner->site_count+p+3,cell_bits=(uint64_t)p+3;
    uint64_t origins=(fn_bits+63)/64,cells=(cell_bits+63)/64;
    uint64_t fn=origins+(uint64_t)owner->binding_count*(origins+cells);
    if (fn_bits>UINT32_MAX || cell_bits>UINT32_MAX || origins>UINT32_MAX ||
        cells>UINT32_MAX || fn+cells>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_runtime_work(read,sizeof(*output)+4);
    if (status==XR_XIR_OK) *output=(EffectInvocationBasis){p,(uint32_t)origins,(uint32_t)fn,
        (uint32_t)cells,(uint32_t)(fn+cells)};
    return status;
}

/* Absence of a dependency is accepted only from a completed certificate.
 * A symbolic parameter, deferred frontier or true unknown keeps the gate shut. */
static XrXirStatus effect_runtime_closed(const EffectInvocationCertificate *certificate,
    uint32_t selected,const XirEffectRuntimeRead *read) {
    const EffectInvocationFinal *owner=certificate->final;
    if (selected>=owner->count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->nodes[selected];
    if (node->body>=effect_body_count(certificate->body) || node->root>=effect_body_count(certificate->body) ||
        !node->causes ||
        node->cause_count!=(uint64_t)certificate->body->functions[node->root].parameter_count*2+3)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus shape_status=effect_runtime_work(read,9);
    if (shape_status!=XR_XIR_OK) return shape_status;
    if (!effect_invocation_node_shape(node,certificate->body->functions[node->root].parameter_count,
        certificate->body->functions[node->body].instruction_count)) return XR_XIR_BAD_STRUCTURE;
    if (node->intrinsic_mask || node->deferred) return XR_XIR_BAD_TYPE;
    for (uint32_t a=0;a<node->cause_count;++a) {
        XrXirStatus status=effect_runtime_work(read,4);
        if (status!=XR_XIR_OK) return status;
        if (effect_invocation_atom(node,certificate->body->functions[node->root].parameter_count,a,false) ||
            node->causes[a].distance!=UINT32_MAX) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_runtime_parent(const EffectInvocationCertificate *certificate,
    const XirEffectInvocationRequest *request,uint32_t *output) {
    const EffectInvocationFinal *owner=certificate->final;
    if (request->caller>=effect_body_count(certificate->body) || !owner->roots || !owner->root_spaces)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t parent=request->contextual?request->node:owner->roots[request->caller];
    if (parent>=owner->count || owner->nodes[parent].body!=request->caller) return XR_XIR_BAD_STRUCTURE;
    if (!request->contextual) {
        const EffectBodyFunction *function=&certificate->body->functions[request->caller];
        for (uint32_t p=0;p<function->parameter_count;++p) {
            XrXirStatus status=effect_runtime_work(&request->read,2);
            if (status!=XR_XIR_OK) return status;
            if (xr_xir_type_is_cell(&certificate->terms.types,function->parameters[p]) ||
                xr_xir_type_is_class(&certificate->terms.types,function->parameters[p]) ||
                effect_physical_origin(&certificate->terms.types,function->parameters[p]))
                return XR_XIR_BAD_TYPE;
        }
        if (owner->nodes[parent].root!=owner->root_spaces[request->caller]) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=effect_runtime_closed(certificate,parent,&request->read);
    if (status==XR_XIR_OK) *output=parent;
    return status;
}

#include "xxir_effect_invocation_runtime_capture.inc.c"

/* A runtime object cannot replace the caller's complete static relation.
 * UNKNOWN is kept even for a physically local object. Historical smaller
 * Class rows cannot be selected after the current relation has grown. */
static XrXirStatus effect_runtime_class(const EffectInvocationCertificate *certificate,
    const XirEffectInvocationRequest *request,const EffectInvocationNode *node,
    uint32_t parameter,uint32_t *output) {
    if (!output || !request->read.class_value || parameter>=request->count) return XR_XIR_BAD_TYPE;
    if(parameter<request->captures) {
        const EffectBodyFunction *caller=&certificate->body->functions[request->caller];
        EffectBodyRead read=effect_body_runtime_read(&request->read);XrXirInstruction op;
        XrXirStatus status=effect_body_instruction_get(&read,caller,request->instruction,&op);
        uint32_t mask=0,publication=UINT32_MAX;
        if(status!=XR_XIR_OK) return status;
        uint32_t value=UINT32_MAX;status=effect_runtime_closure_value(&op,&value);
        if(status!=XR_XIR_OK) return status;
        status=effect_runtime_capture_class_mask(certificate,request,value,
            request->producer,parameter,&mask);
        if(status==XR_XIR_OK) status=request->read.class_value(request->read.owner,parameter,&publication);
        if(status!=XR_XIR_OK) return status;
        if(publication>1) return XR_XIR_BAD_TYPE;
        *output=mask|(publication?EFFECT_CLASS_ROOT:0);return XR_XIR_OK;
    }
    const EffectInvocationFinal *owner=certificate->final;
    uint32_t parent=request->contextual?request->node:owner->roots[request->caller];
    if (parent>=owner->count || owner->nodes[parent].body!=request->caller ||
        owner->nodes[parent].root!=node->root) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *caller=&certificate->body->functions[request->caller];
    EffectBodyRead read=effect_body_runtime_read(&request->read);XrXirInstruction decoded;
    XrXirStatus decoded_status=effect_body_instruction_get(&read,caller,request->instruction,&decoded);
    if (decoded_status!=XR_XIR_OK) return decoded_status;
    const XrXirInstruction *op=&decoded;
    uint32_t suffix=parameter-request->captures;
    if (suffix>=op->args[1] || op->args[0]>caller->operand_count ||
        suffix>=caller->operand_count-op->args[0] || !caller->operands)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t actual=caller->operands[op->args[0]+suffix];
    if (actual>=(uint64_t)caller->parameter_count+caller->instruction_count) return XR_XIR_BAD_STRUCTURE;
    XrXirType type;decoded_status=effect_body_operand_type(&read,caller,actual,&type);
    if (decoded_status!=XR_XIR_OK) return decoded_status;
    if (!xr_xir_type_is_class(certificate->body->domain.types,type)) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *from=&owner->nodes[parent];
    uint32_t low=0,high=from->class_count;
    if (high && !from->classes) return XR_XIR_BAD_STRUCTURE;
    while (low<high) {
        XrXirStatus status=effect_runtime_work(&request->read,3);
        if (status!=XR_XIR_OK) return status;
        uint32_t middle=low+(high-low)/2;
        if (from->classes[middle].value<actual) low=middle+1;else high=middle;
    }
    XrXirStatus status=effect_runtime_work(&request->read,5);
    if (status!=XR_XIR_OK) return status;
    if (low>=from->class_count || from->classes[low].value!=actual) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationClassFact fact=from->classes[low];
    if ((fact.mask&~EFFECT_CLASS_VALID) || !(fact.mask&EFFECT_CLASS_KNOWN) ||
        (fact.mask==EFFECT_CLASS_KNOWN?!fact.rank:fact.rank!=0)) return XR_XIR_BAD_STRUCTURE;
    uint32_t publication=UINT32_MAX;
    status=request->read.class_value(request->read.owner,parameter,&publication);
    if (status!=XR_XIR_OK) return status;
    if (publication>1) return XR_XIR_BAD_TYPE;
    *output=fact.mask|(publication?EFFECT_CLASS_ROOT:0);return XR_XIR_OK;
}

/* Complete keys retain the original bound composition. The physical origin
 * member is authenticated separately, with its actual capture storage. */
#include "xxir_effect_invocation_runtime_iterator.inc.c"

#include "xxir_effect_invocation_runtime_key.inc.c"

static XrXirStatus effect_runtime_inputs(const EffectInvocationCertificate *certificate,
    const XirEffectInvocationRequest *request,const EffectInvocationNode *node,bool *output) {
    const EffectBodyFunction *target=&certificate->body->functions[request->target];
    if (node->parameter_count!=target->parameter_count || request->count!=target->parameter_count ||
        (node->parameter_count && !node->input)) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationBasis basis={0};
    XrXirStatus status=effect_runtime_basis(certificate,node->root,&basis,&request->read);
    bool same=true;
    for (uint32_t p=0;p<node->parameter_count && status==XR_XIR_OK;++p) {
        bool callable=xr_xir_callable_signature(&certificate->terms.types,target->parameters[p])!=NULL;
        bool iterator=effect_physical_iterator(&certificate->terms.types,target->parameters[p]);
        if (callable || iterator) {
            bool matched=false;
            status=effect_runtime_origin_member(certificate,request,node,p,&matched);
            if (status!=XR_XIR_OK) return status;
            if (!matched) same=false;continue;
        }
        bool cell=xr_xir_type_is_cell(&certificate->terms.types,target->parameters[p]);
        uint32_t actual=cell?request->read.cell(request->read.owner,p):0;
        if (cell && actual>1) return XR_XIR_BAD_TYPE;
        bool is_class=xr_xir_type_is_class(&certificate->terms.types,target->parameters[p]);
        uint32_t class_mask=0;
        if (is_class) {
            status=effect_runtime_class(certificate,request,node,p,&class_mask);
            if (status!=XR_XIR_OK) return status;
        }
        const uint64_t *row=node->input+(size_t)p*basis.row_words;
        for (uint32_t w=0;w<basis.row_words;++w) {
            status=effect_runtime_work(&request->read,3);
            if (status!=XR_XIR_OK) break;
            uint64_t expected=0;
            if (cell && w>=basis.fn_words) {
                uint32_t c=w-basis.fn_words;
                if (c==(basis.parameters+2)/64) expected|=UINT64_C(1)<<((basis.parameters+2)%64);
                if (actual && c==basis.parameters/64) expected|=UINT64_C(1)<<(basis.parameters%64);
            }
            if (is_class && w>=basis.fn_words) {
                uint32_t c=w-basis.fn_words;
                for (uint32_t bit=0;bit<3;++bit) {
                    uint32_t flag=bit==0?EFFECT_CLASS_ROOT:bit==1?EFFECT_CLASS_UNKNOWN:EFFECT_CLASS_KNOWN;
                    uint32_t index=basis.parameters+bit;
                    if ((class_mask&flag) && c==index/64) expected|=UINT64_C(1)<<(index%64);
                }
            }
            if (row[w]!=expected) same=false;
        }
    }
    if (status==XR_XIR_OK) *output=same;
    return status;
}

/* An advance uses its actual receiver and genuine GENERATOR_NEW producer.
 * hasNext/next/nth operands are never decoded as a call argument list. */
static XrXirStatus effect_runtime_frontier_shape(const EffectInvocationCertificate *certificate,
    const XirEffectInvocationRequest *request,const XrXirInstruction *op,const EffectBodyRead *read) {
    const EffectInvocationFinal *owner=certificate->final;
    bool iterator=effect_invocation_iterator_operation(op->op);
    if(request->captures>request->count ||
        (iterator?request->captures!=request->count:op->args[1]!=request->count-request->captures))
        return XR_XIR_BAD_STRUCTURE;
    if(request->producer==UINT32_MAX) {
        if(iterator || request->captures ||
            (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_GENERATOR_NEW) ||
            op->immediate!=(int64_t)request->target) return XR_XIR_BAD_STRUCTURE;
        return XR_XIR_OK;
    }
    const EffectInvocationSite *site=&owner->sites[request->producer];
    if((!iterator && op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) ||
        site->target!=request->target || site->captures!=request->captures ||
        site->function>=effect_body_count(certificate->body) ||
        site->instruction>=certificate->body->functions[site->function].instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction reference;XrXirStatus status=effect_body_instruction_get(read,
        &certificate->body->functions[site->function],site->instruction,&reference);
    if(status!=XR_XIR_OK) return status;
    if(reference.op!=(iterator?XR_XIR_GENERATOR_NEW:XR_XIR_FUNCTION_REF) ||
        reference.args[1]!=request->captures || reference.immediate!=(int64_t)request->target)
        return XR_XIR_BAD_STRUCTURE;
    if(iterator) {
        const EffectBodyFunction *caller=&certificate->body->functions[request->caller];
        XrXirType actual_type=0;
        if(op->args[0]>=(uint64_t)caller->parameter_count+caller->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        status=effect_body_operand_type(read,caller,op->args[0],&actual_type);
        if(status!=XR_XIR_OK) return status;
        if(!effect_physical_iterator(&certificate->terms.types,actual_type) ||
            !effect_physical_iterator(&certificate->terms.types,reference.type) ||
            reference.type!=certificate->body->functions[request->target].result)
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* This reads the original opcode and receiver type from the same certificate.
 * A staged ordinal or a producer identity cannot replace these body facts. */
XR_FUNC XrXirStatus xir_effects_iterator_instruction(const XrXirEffects *effects,
    uint32_t caller,uint32_t instruction,const XirEffectRuntimeRead *actual,XirEffectIteratorInstruction *output) {
    if(!effects || !effects->invocations || !actual || !output) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCertificate *certificate=effects->invocations;
    if(!certificate->body || certificate->resources!=effects->resources ||
        certificate->body->resources!=certificate->resources || !effect_body_material_shape(certificate->body) ||
        effect_body_count(certificate->body)!=effects->count || caller>=effects->count ||
        !certificate->body->functions) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *function=&certificate->body->functions[caller];
    if(instruction>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_runtime_work(actual,sizeof(*output)+8);if(status!=XR_XIR_OK) return status;
    EffectBodyRead read=effect_body_runtime_read(actual);XrXirInstruction op;
    status=effect_body_instruction_get(&read,function,instruction,&op);if(status!=XR_XIR_OK) return status;
    if(!effect_invocation_iterator_operation(op.op)) return XR_XIR_BAD_TYPE;
    if(op.args[0]>=(uint64_t)function->parameter_count+function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    XrXirType receiver=0;status=effect_body_operand_type(&read,function,op.args[0],&receiver);
    if(status!=XR_XIR_OK) return status;
    if(!effect_physical_iterator(&certificate->terms.types,receiver)) return XR_XIR_BAD_TYPE;
    *output=(XirEffectIteratorInstruction){op.op,receiver};return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_execution_edge(const XrXirEffects *effects,
    const XirEffectInvocationRequest *request,XirEffectInvocationSelection *output) {
    if (!effects || !effects->invocations || !request || !request->read.work || !request->read.cell ||
        !output) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCertificate *certificate=effects->invocations;
    const EffectInvocationFinal *owner=certificate->final;
    if (!effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || !owner || (!certificate->final) ||
        (owner->count && !owner->nodes) || (owner->edge_count && !owner->edges) ||
        (owner->site_count && !owner->sites) ||
        (effect_body_count(certificate->body) && !certificate->body->functions) ||
        certificate->resources!=effects->resources || effect_body_count(certificate->body)!=effects->count ||
        request->caller>=effects->count || request->target>=effects->count ||
        (request->producer!=UINT32_MAX && request->producer>=owner->site_count))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_runtime_work(&request->read,14);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunction *function=&certificate->body->functions[request->caller];
    if (request->instruction>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRead read=effect_body_runtime_read(&request->read);XrXirInstruction decoded;
    status=effect_body_instruction_get(&read,function,request->instruction,&decoded);
    if (status!=XR_XIR_OK) return status;
    const XrXirInstruction *op=&decoded;
    status=effect_runtime_frontier_shape(certificate,request,op,&read);
    if(status!=XR_XIR_OK) return status;
    uint32_t parent=UINT32_MAX;
    status=effect_runtime_parent(certificate,request,&parent);
    if (status!=XR_XIR_OK) return status;
    uint32_t selected=UINT32_MAX,edge_index=UINT32_MAX,walked=0;
    for (uint32_t e=owner->nodes[parent].edge_head;e!=UINT32_MAX;e=owner->edges[e].next) {
        status=effect_runtime_work(&request->read,8);
        if (status!=XR_XIR_OK) return status;
        if (e>=owner->edge_count || ++walked>owner->edge_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge *edge=&owner->edges[e];
        if (edge->caller!=parent || edge->target>=owner->count) return XR_XIR_BAD_STRUCTURE;
        if (edge->instruction!=request->instruction || edge->producer!=request->producer ||
            edge->latent!=(op->op==XR_XIR_GENERATOR_NEW) ||
            owner->nodes[edge->target].body!=request->target) continue;
        const EffectInvocationNode *node=&owner->nodes[edge->target];
        if (node->root!=owner->nodes[parent].root) return XR_XIR_BAD_STRUCTURE;
        bool matches=false;
        status=effect_runtime_current_key(certificate,request,op,parent,node,&matches);
        if(status!=XR_XIR_OK) return status;
        if(!matches) continue;
        status=effect_runtime_inputs(certificate,request,node,&matches);
        if (status!=XR_XIR_OK) return status;
        if (!matches) continue;
        status=effect_runtime_closed(certificate,edge->target,&request->read);
        if (status!=XR_XIR_OK) return status;
        if (selected!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        selected=edge->target;edge_index=e;
    }
    if (selected==UINT32_MAX) return XR_XIR_BAD_TYPE;
    status=effect_runtime_work(&request->read,sizeof(*output));
    if (status==XR_XIR_OK) *output=(XirEffectInvocationSelection){effects,request->caller,
        request->instruction,request->target,request->producer,parent,selected,edge_index};
    return status;
}

#include "xxir_effect_invocation_class_access.inc.c"
