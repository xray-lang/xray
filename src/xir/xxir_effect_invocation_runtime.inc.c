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
    const EffectInvocationOwner *owner=certificate->equations;
    if (root>=certificate->bodies.function_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t p=certificate->bodies.functions[root].parameter_count;
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
    const EffectInvocationOwner *owner=certificate->equations;
    if (selected>=owner->count || owner->pending) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->nodes[selected];
    if (node->body>=certificate->bodies.function_count || node->root>=certificate->bodies.function_count ||
        !node->expanded || node->queued || !node->causes || !node->local_causes ||
        node->cause_count!=(uint64_t)certificate->bodies.functions[node->root].parameter_count*2+3)
        return XR_XIR_BAD_STRUCTURE;
    if (node->intrinsic_mask || node->deferred) return XR_XIR_BAD_TYPE;
    for (uint32_t a=0;a<node->cause_count;++a) {
        XrXirStatus status=effect_runtime_work(read,4);
        if (status!=XR_XIR_OK) return status;
        if (effect_invocation_atom(node,certificate->bodies.functions[node->root].parameter_count,a,false) ||
            node->causes[a].distance!=UINT32_MAX) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_runtime_parent(const EffectInvocationCertificate *certificate,
    const XirEffectInvocationRequest *request,uint32_t *output) {
    const EffectInvocationOwner *owner=certificate->equations;
    if (request->caller>=certificate->bodies.function_count || !owner->roots || !owner->root_spaces)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t parent=request->contextual?request->node:owner->roots[request->caller];
    if (parent>=owner->count || owner->nodes[parent].body!=request->caller) return XR_XIR_BAD_STRUCTURE;
    if (!request->contextual) {
        const XrXirFunction *function=&certificate->bodies.functions[request->caller];
        for (uint32_t p=0;p<function->parameter_count;++p) {
            XrXirStatus status=effect_runtime_work(&request->read,2);
            if (status!=XR_XIR_OK) return status;
            if (xr_xir_type_is_cell(&certificate->terms.types,function->parameters[p]) ||
                xr_xir_callable_signature(&certificate->terms.types,function->parameters[p]))
                return XR_XIR_BAD_TYPE;
        }
        if (owner->nodes[parent].root!=owner->root_spaces[request->caller]) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=effect_runtime_closed(certificate,parent,&request->read);
    if (status==XR_XIR_OK) *output=parent;
    return status;
}

/* The original bound is copied by the common owner before refinement.
 * It validates a retained opaque atom; it cannot create an origin or grant
 * permission to a value without the independently checked actual producer. */
static XrXirStatus effect_runtime_original_none(const EffectInvocationCertificate *certificate,
    const XirEffectInvocationRequest *request,uint32_t parameter,bool *output) {
    const EffectInvocationDeclaredBounds *bounds=certificate->declared;
    const XrXirFunction *caller=&certificate->bodies.functions[request->caller];
    const XrXirInstruction *op=&caller->instructions[request->instruction];
    if (!bounds || bounds->resources!=certificate->resources ||
        bounds->count!=certificate->bodies.function_count || !bounds->functions ||
        !certificate->parameter_kinds || !certificate->parameter_kinds[request->target] ||
        parameter>=request->count || parameter<request->captures || op->args[0]>caller->operand_count ||
        op->args[1]>caller->operand_count-op->args[0] || !caller->operands)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t actual=caller->operands[op->args[0]+parameter-request->captures];
    const EffectInvocationFunctionBounds *source=&bounds->functions[request->caller];
    const EffectInvocationFunctionBounds *target=&bounds->functions[request->target];
    if (actual>=source->values || parameter>=target->values || !source->bounds || !target->bounds)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_runtime_work(&request->read,8);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationValueBound *provided=&source->bounds[actual],*expected=&target->bounds[parameter];
    uint32_t valid=XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    if (!provided->callable || !expected->callable || (provided->mask&~valid) || (expected->mask&~valid))
        return XR_XIR_BAD_STRUCTURE;
    bool fixed=certificate->parameter_kinds[request->target][parameter]==XR_XIR_EFFECT_PARAMETER_FIXED;
    const XrXirTypeNode *signature=xr_xir_callable_signature(&certificate->terms.types,
        certificate->bodies.functions[request->target].parameters[parameter]);
    if (!signature) return XR_XIR_BAD_STRUCTURE;
    bool ref=false;
    for (uint32_t a=0;a<signature->parameter_count;++a) {
        status=effect_runtime_work(&request->read,1);
        if (status!=XR_XIR_OK) return status;
        if (signature->parameters[a].mode==XR_PARAM_REF) ref=true;
    }
    *output=!provided->mask || (fixed && !ref && !expected->mask);return XR_XIR_OK;
}

typedef struct EffectRuntimeFunctionRow {
    const XirEffectInvocationRequest *request;
    const EffectInvocationBasis *basis;
    uint32_t parameter,producer;
} EffectRuntimeFunctionRow;

/* Every real owned Cell prefix has a distinct flat column. The complete
 * actual row is compared with the immutable equation input, never a subset
 * or a smaller mask selected from one captured environment. */
static XrXirStatus effect_runtime_function_word(const EffectInvocationCertificate *certificate,
    const EffectRuntimeFunctionRow *row,uint32_t word,uint64_t *output) {
    const EffectInvocationOwner *owner=certificate->equations;
    const EffectInvocationSite *site=&owner->sites[row->producer];
    const EffectInvocationBasis *basis=row->basis;
    if (site->target>=certificate->bodies.function_count || site->binding>owner->binding_count ||
        site->captures>owner->binding_count-site->binding) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&certificate->bodies.functions[site->target];
    if (site->captures>function->parameter_count || (site->captures && !function->parameters))
        return XR_XIR_BAD_STRUCTURE;
    uint64_t expected=word==row->producer/64?UINT64_C(1)<<(row->producer%64):0;
    for (uint32_t c=0;c<site->captures;++c) {
        XrXirStatus status=effect_runtime_work(&row->request->read,3);
        if (status!=XR_XIR_OK) return status;
        XrXirType type=function->parameters[c];
        bool cell=xr_xir_type_is_cell(&certificate->terms.types,type);
        if (!cell) {
            if (xr_xir_type_node(&certificate->terms.types,type)) return XR_XIR_BAD_TYPE;
            continue;
        }
        uint64_t column=(uint64_t)basis->origin_words+
            ((uint64_t)site->binding+c)*(basis->origin_words+basis->cell_words)+basis->origin_words;
        if (column+basis->cell_words>basis->fn_words) return XR_XIR_BAD_STRUCTURE;
        if (word<column || word-column>=basis->cell_words) continue;
        if (!row->request->read.capture_cell) return XR_XIR_BAD_TYPE;
        uint32_t actual=UINT32_MAX;
        status=row->request->read.capture_cell(row->request->read.owner,row->parameter,c,row->producer,&actual);
        if (status!=XR_XIR_OK) return status;
        if (actual>1) return XR_XIR_BAD_TYPE;
        uint32_t relative=(uint32_t)(word-column);
        if (relative==(basis->parameters+2)/64) expected|=UINT64_C(1)<<((basis->parameters+2)%64);
        if (actual && relative==basis->parameters/64) expected|=UINT64_C(1)<<(basis->parameters%64);
    }
    *output=expected;return XR_XIR_OK;
}

/* This reader closes scalar and owned Cell prefixes with scoped REF suffixes.
 * Scalar captures have no relation columns; callable captures and other
 * constructed environments remain refused by the complete actual reader. */
static XrXirStatus effect_runtime_inputs(const EffectInvocationCertificate *certificate,
    const XirEffectInvocationRequest *request,const EffectInvocationNode *node,bool *output) {
    const XrXirFunction *target=&certificate->bodies.functions[request->target];
    if (node->parameter_count!=target->parameter_count || request->count!=target->parameter_count ||
        (node->parameter_count && !node->input)) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationBasis basis={0};
    XrXirStatus status=effect_runtime_basis(certificate,node->root,&basis,&request->read);
    bool same=true;
    for (uint32_t p=0;p<node->parameter_count && status==XR_XIR_OK;++p) {
        bool callable=xr_xir_callable_signature(&certificate->terms.types,target->parameters[p])!=NULL;
        uint32_t producer=UINT32_MAX,opaque=certificate->equations->site_count+basis.parameters;
        bool original_none=false;
        if (callable) {
            if (!request->read.function) return XR_XIR_BAD_TYPE;
            status=request->read.function(request->read.owner,p,&producer);
            if (status!=XR_XIR_OK) return status;
            const EffectInvocationOwner *owner=certificate->equations;
            if (producer>=owner->site_count) return XR_XIR_BAD_TYPE;
            const uint64_t *row=node->input+(size_t)p*basis.row_words;
            if (row[opaque/64]&(UINT64_C(1)<<(opaque%64))) {
                status=effect_runtime_original_none(certificate,request,p,&original_none);
                if (status!=XR_XIR_OK) return status;
                if (!original_none) return XR_XIR_BAD_TYPE;
            }
        }
        bool cell=xr_xir_type_is_cell(&certificate->terms.types,target->parameters[p]);
        uint32_t actual=cell?request->read.cell(request->read.owner,p):0;
        if (cell && actual>1) return XR_XIR_BAD_TYPE;
        const uint64_t *row=node->input+(size_t)p*basis.row_words;
        for (uint32_t w=0;w<basis.row_words;++w) {
            status=effect_runtime_work(&request->read,3);
            if (status!=XR_XIR_OK) break;
            uint64_t expected=0;
            if (callable) {
                const EffectRuntimeFunctionRow capture={request,&basis,p,producer};
                status=effect_runtime_function_word(certificate,&capture,w,&expected);
                if (status!=XR_XIR_OK) break;
            }
            if (original_none && w==opaque/64) expected|=UINT64_C(1)<<(opaque%64);
            if (cell && w>=basis.fn_words) {
                uint32_t c=w-basis.fn_words;
                if (c==(basis.parameters+2)/64) expected|=UINT64_C(1)<<((basis.parameters+2)%64);
                if (actual && c==basis.parameters/64) expected|=UINT64_C(1)<<(basis.parameters%64);
            }
            if (row[w]!=expected) same=false;
        }
    }
    if (status==XR_XIR_OK) *output=same;
    return status;
}

XR_FUNC XrXirStatus xir_effects_execution_edge(const XrXirEffects *effects,
    const XirEffectInvocationRequest *request,XirEffectInvocationSelection *output) {
    if (!effects || !effects->invocations || !request || !request->read.work || !request->read.cell ||
        !output) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCertificate *certificate=effects->invocations;
    const EffectInvocationOwner *owner=certificate->equations;
    if (!owner || owner->work || owner->module || owner->graph || !certificate->solution ||
        (owner->count && !owner->nodes) || (owner->edge_count && !owner->edges) ||
        (owner->site_count && !owner->sites) ||
        (certificate->bodies.function_count && !certificate->bodies.functions) ||
        certificate->resources!=effects->resources || certificate->bodies.function_count!=effects->count ||
        request->caller>=effects->count || request->target>=effects->count ||
        (request->producer!=UINT32_MAX && request->producer>=owner->site_count))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_runtime_work(&request->read,12);
    if (status!=XR_XIR_OK) return status;
    const XrXirFunction *function=&certificate->bodies.functions[request->caller];
    if (request->instruction>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&function->instructions[request->instruction];
    if (request->captures>request->count || op->args[1]!=request->count-request->captures)
        return XR_XIR_BAD_STRUCTURE;
    if (request->producer==UINT32_MAX) {
        if (request->captures || (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE) ||
            op->immediate!=(int64_t)request->target) return XR_XIR_BAD_STRUCTURE;
    } else {
        const EffectInvocationSite *site=&owner->sites[request->producer];
        if ((op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) ||
            site->target!=request->target || site->captures!=request->captures || site->function>=effects->count ||
            site->instruction>=certificate->bodies.functions[site->function].instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *reference=&certificate->bodies.functions[site->function].instructions[site->instruction];
        if (reference->op!=XR_XIR_FUNCTION_REF || reference->args[1]!=request->captures ||
            reference->immediate!=(int64_t)request->target)
            return XR_XIR_BAD_STRUCTURE;
    }
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
        if (edge->instruction!=request->instruction || edge->producer!=request->producer || edge->latent ||
            owner->nodes[edge->target].body!=request->target) continue;
        const EffectInvocationNode *node=&owner->nodes[edge->target];
        if (node->root!=owner->nodes[parent].root) return XR_XIR_BAD_STRUCTURE;
        bool matches=false;
        status=effect_runtime_inputs(certificate,request,node,&matches);
        if (status!=XR_XIR_OK) return status;
        if (!matches) continue;
        status=effect_runtime_closed(certificate,edge->target,&request->read);
        if (status!=XR_XIR_OK) return status;
        if (selected!=UINT32_MAX && selected!=edge->target) return XR_XIR_BAD_STRUCTURE;
        selected=edge->target;edge_index=e;
    }
    if (selected==UINT32_MAX) return XR_XIR_BAD_TYPE;
    status=effect_runtime_work(&request->read,sizeof(*output));
    if (status==XR_XIR_OK) *output=(XirEffectInvocationSelection){effects,request->caller,
        request->instruction,request->target,request->producer,parent,selected,edge_index};
    return status;
}
