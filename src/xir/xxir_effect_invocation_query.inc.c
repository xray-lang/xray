/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_query.inc.c - Typed actual edge masks from sealed equations
 *
 * KEY CONCEPT:
 *   A query consumes the exact real call frontier and its complete equation.
 *   A missing edge or unbound input is UNKNOWN, never an empty permission.
 */
static bool effect_invocation_witness_equal(const XrXirRootEffectWitness *a,
    const XrXirRootEffectWitness *b) {
    return a->cause==b->cause && a->instruction==b->instruction && a->callee==b->callee &&
        a->slot==b->slot && a->distance==b->distance;
}

typedef struct EffectInvocationEdgeShape {
    const EffectInvocationFinal *equations;
    const XirMetadataDomain *domain;
    const XrXirTypes *types;
    const XrXirInstruction *op,*reference;
    uint32_t caller,edge_index,parameters,instructions,producer_instructions,target_parameters;
    XrXirType actual_type;
} EffectInvocationEdgeShape;

/* Raw construction and final owning readers supply the same complete request.
 * No reconstructed executable Module or alternate permission path is needed. */
static XrXirStatus effect_invocation_edge_shape_request(const XrXirCompileContext *work,
    const EffectInvocationEdgeShape *request) {
    const EffectInvocationFinal *equations=request->equations;
    const XirMetadataDomain *domain=request->domain;
    uint32_t caller=request->caller,edge_index=request->edge_index;
    if (!domain || !request->op || caller>=equations->count || edge_index>=equations->edge_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationEdge edge=equations->edges[edge_index];
    const EffectInvocationNode *from=&equations->nodes[caller];
    if (edge.caller!=caller || edge.target>=equations->count || from->body>=domain->function_count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *to=&equations->nodes[edge.target];
    if (to->root!=from->root || to->body>=domain->function_count ||
        edge.instruction>=request->instructions) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=request->op;const XrXirTypes *types=request->types;
    if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_CLEANUP_REGISTER || op->op==XR_XIR_GO)
        return edge.producer==UINT32_MAX && op->immediate>=0 && (uint64_t)op->immediate==to->body &&
            edge.latent==(op->op==XR_XIR_GO)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        const uint32_t *identity=xr_xir_default_identity(op);const XrXirDefaultBinding *binding=NULL;
        XrXirStatus status=xir_metadata_default_lookup(work,domain,identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        return !edge.latent && edge.producer==UINT32_MAX && binding && binding->function==to->body?
            XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    if (edge.producer>=equations->site_count || !request->reference) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationSite site=equations->sites[edge.producer];
    if (site.function>=domain->function_count || site.target!=to->body ||
        site.binding>equations->binding_count || site.captures>equations->binding_count-site.binding ||
        site.instruction>=request->producer_instructions) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *reference=request->reference;
    if (reference->op!=XR_XIR_FUNCTION_REF || reference->immediate<0 ||
        (uint64_t)reference->immediate!=site.target || reference->args[1]!=site.captures)
        return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_FUNCTION_REF)
        return edge.latent && site.function==from->body && site.instruction==edge.instruction?
            XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if ((op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) || edge.latent ||
        op->immediate<0 || (uint64_t)op->immediate>=(uint64_t)request->parameters+request->instructions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *actual=xr_xir_callable_signature(types,request->actual_type);
    const XrXirTypeNode *bound=xr_xir_callable_signature(types,reference->type);
    if (!actual || !bound || actual->parameter_count!=op->args[1] ||
        site.captures>request->target_parameters || bound->parameter_count!=request->target_parameters-site.captures ||
        actual->parameter_count!=bound->parameter_count) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch={work->resources,NULL};
    XrXirStatus status=xr_xir_compile_type_substitution_matches_between_scratch(work,
        types,types,NULL,0,actual->result,bound->result,&scratch);
    for (uint32_t p=0;p<actual->parameter_count && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
        if (actual->parameters[p].mode!=bound->parameters[p].mode ||
            !xr_xir_callable_parameter_storage_valid(types,&actual->parameters[p]) ||
            !xr_xir_callable_parameter_storage_valid(types,&bound->parameters[p])) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        status=xr_xir_compile_type_substitution_matches_between_scratch(work,
            types,types,NULL,0,actual->parameters[p].type,bound->parameters[p].type,&scratch);
    }
    xr_xir_type_match_scratch_free(&scratch);
    return status==XR_XIR_BAD_TYPE?XR_XIR_BAD_STRUCTURE:status;
}

typedef struct EffectInvocationEdgeRead {
    uint32_t caller,index;
    XrXirOp *opcode;
} EffectInvocationEdgeRead;

/* The optional opcode belongs only to this completed edge request. Full
 * physical decoding and exact edge/type admission precede its publication. */
static XrXirStatus effect_invocation_edge_read_selected(const XrXirCompileContext *work,
    const EffectInvocationFinal *equations,const EffectBodyRequest *body,
    const XrXirTypes *types,const EffectInvocationEdgeRead *selection) {
    if (!body || !equations || !selection || selection->caller>=equations->count ||
        selection->index>=equations->edge_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t caller=selection->caller,edge_index=selection->index;
    const EffectInvocationEdge *edge=&equations->edges[edge_index];
    const EffectInvocationNode *from=&equations->nodes[caller];
    if (edge->target>=equations->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t target=equations->nodes[edge->target].body;
    if (!xir_compile_work(work,2*sizeof(EffectBodyFunctionDimensions))) return XR_XIR_BUDGET;
    EffectBodyFunctionDimensions function={0},callee={0};
    XrXirStatus status=effect_body_request_dimensions(body,from->body,&function);
    if (status==XR_XIR_OK) status=effect_body_request_dimensions(body,target,&callee);
    XrXirInstruction op_storage,reference_storage;const XrXirInstruction *op=NULL,*reference=NULL;
    if (status==XR_XIR_OK)
        status=effect_body_request_instruction(body,from->body,edge->instruction,&op_storage,&op);
    if (status!=XR_XIR_OK) return status;
    EffectInvocationEdgeShape request={.equations=equations,.domain=&body->domain,.types=types,.op=op,
        .caller=caller,.edge_index=edge_index,.parameters=function.parameter_count,
        .instructions=function.instruction_count,.target_parameters=callee.parameter_count};
    if (edge->producer<equations->site_count) {
        const EffectInvocationSite *site=&equations->sites[edge->producer];
        if (!xir_compile_work(work,sizeof(EffectBodyFunctionDimensions))) return XR_XIR_BUDGET;
        EffectBodyFunctionDimensions producer={0};
        status=effect_body_request_dimensions(body,site->function,&producer);
        if (status==XR_XIR_OK)
            status=effect_body_request_instruction(body,site->function,site->instruction,&reference_storage,&reference);
        if (status!=XR_XIR_OK) return status;
        request.reference=reference;request.producer_instructions=producer.instruction_count;
    }
    if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
        if (op->immediate<0 || (uint64_t)op->immediate>=(uint64_t)function.parameter_count+function.instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        status=effect_body_request_type(body,from->body,(uint32_t)op->immediate,&request.actual_type);
        if (status!=XR_XIR_OK) return status;
    }
    status=effect_invocation_edge_shape_request(work,&request);
    if (status!=XR_XIR_OK) return status;
    if (selection->opcode) {
        if (!effect_body_read_work(&body->read,1+sizeof(*selection->opcode))) return XR_XIR_BUDGET;
        *selection->opcode=op->op;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_edge_shape_read(const XrXirCompileContext *work,
    const EffectInvocationFinal *equations,const EffectBodyRequest *body,
    const XrXirTypes *types,uint32_t caller,uint32_t edge_index) {
    if (!body || !equations || caller>=equations->count || edge_index>=equations->edge_count ||
        equations->edges[edge_index].target>=equations->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(EffectInvocationEdgeRead))) return XR_XIR_BUDGET;
    const EffectInvocationEdgeRead request={caller,edge_index,NULL};
    return effect_invocation_edge_read_selected(work,equations,body,types,&request);
}

static inline XrXirStatus effect_invocation_edge_shape(const XrXirCompileContext *work,
    const EffectInvocationFinal *equations,const XrXirModule *module,
    const XrXirTypes *types,uint32_t caller,uint32_t edge_index) {
    if (!module) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest body=effect_body_raw_request(work,module);
    return effect_invocation_edge_shape_read(work,equations,&body,types,caller,edge_index);
}

static XrXirStatus effect_invocation_certificate_edge_selected(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t caller,uint32_t edge_index,XrXirOp *opcode) {
    if (!certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || !certificate->final)
        return XR_XIR_BAD_STRUCTURE;
    if (caller>=certificate->final->count || edge_index>=certificate->final->edge_count ||
        certificate->final->edges[edge_index].target>=certificate->final->count)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(EffectInvocationEdgeRead))) return XR_XIR_BUDGET;
    const EffectInvocationEdgeRead request={caller,edge_index,opcode};
    EffectBodyRequest body=effect_body_owned_request(work,certificate->body);
    return effect_invocation_edge_read_selected(work,certificate->final,&body,
        &certificate->terms.types,&request);
}

static XrXirStatus effect_invocation_certificate_edge(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t caller,uint32_t edge_index) {
    return effect_invocation_certificate_edge_selected(work,certificate,caller,edge_index,NULL);
}

/* Each walk consumes the immutable local terminal copy. Actual call links
 * name the full child equation and real producer site, and strictly decrease
 * the owned forest distance. A same-number target cannot stand in for a link. */
static XrXirStatus effect_invocation_certificate_cause(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t selected,uint32_t atom) {
    if (!certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || !certificate->final ||
        selected>=certificate->final->count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *equations=certificate->final;
    uint32_t root=equations->nodes[selected].root;
    if (root>=effect_body_count(certificate->body)) return XR_XIR_BAD_STRUCTURE;
    uint32_t parameters=certificate->body->functions[root].parameter_count;
    uint32_t current=selected,walked=0;
    for (;;) {
        if (!xir_compile_work(work,10)) return XR_XIR_BUDGET;
        if (current>=equations->count || ++walked>equations->count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationNode *node=&equations->nodes[current];
        if (!xir_compile_work(work,9)) return XR_XIR_BUDGET;
        if (node->root!=root || node->body>=effect_body_count(certificate->body) || atom>=node->cause_count ||
            !effect_invocation_node_shape(node,parameters,certificate->body->functions[node->body].instruction_count) ||
            !effect_invocation_local_causes(node) || !effect_invocation_atom(node,parameters,atom,false))
            return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationCause *cause=&node->causes[atom];
        const EffectBodyFunction *function=&certificate->body->functions[node->body];
        if (cause->distance>=equations->count || cause->witness.distance!=cause->distance ||
            !cause->witness.cause) return XR_XIR_BAD_STRUCTURE;
        if (!cause->distance) {
            if (cause->next!=UINT32_MAX || cause->edge!=UINT32_MAX ||
                !effect_invocation_atom(node,parameters,atom,true) ||
                !effect_invocation_witness_equal(&cause->witness,&effect_invocation_local_causes(node)[atom].witness))
                return XR_XIR_BAD_STRUCTURE;
            XrXirRootEffectWitness witness=cause->witness;
            if (witness.cause==XR_XIR_ROOT_CAUSE_INITIALIZER) {
                const XrXirDeclarations *declarations=certificate->declarations;
                if (witness.instruction!=UINT32_MAX) {
                    if (witness.instruction>=function->instruction_count || !declarations || atom ||
                        witness.callee!=UINT32_MAX || witness.slot>=declarations->slot_count)
                        return XR_XIR_BAD_STRUCTURE;
                    EffectBodyRead read=effect_body_compile_read(work);XrXirInstruction decoded;
                    XrXirStatus status=effect_body_instruction_get(&read,function,witness.instruction,&decoded);
                    if (status!=XR_XIR_OK) return status;
                    const XrXirInstruction *op=&decoded;
                    uint32_t slot=op->op==XR_XIR_SLOT_GROUP_INIT?
                        (uint32_t)((uint64_t)op->immediate>>32):(uint32_t)op->immediate;
                    return (op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_GROUP_INIT) &&
                        slot==witness.slot?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
                }
                if (witness.instruction!=UINT32_MAX || witness.callee!=UINT32_MAX || witness.slot!=UINT32_MAX ||
                    !declarations || !declarations->functions || !declarations->modules ||
                    declarations->functions[node->body].module>=declarations->module_count ||
                    declarations->modules[declarations->functions[node->body].module].initializer!=node->body)
                    return XR_XIR_BAD_STRUCTURE;
                return atom==0?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            }
            if (witness.instruction>=function->instruction_count || witness.callee!=UINT32_MAX)
                return XR_XIR_BAD_STRUCTURE;
            EffectBodyRead read=effect_body_compile_read(work);
            /* This terminal really consumes args0. All other branches below
             * ask only for the opcode through the same canonical reader. */
            if (atom<2 && witness.cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS) {
                XrXirInstruction decoded;
                XrXirStatus status=effect_body_instruction_get(&read,function,witness.instruction,&decoded);
                if (status!=XR_XIR_OK) return status;
                XrXirOp op=decoded.op;
                return (op==XR_XIR_CELL_READ || op==XR_XIR_CELL_WRITE || op==XR_XIR_CELL_STRING_APPEND || op==XR_XIR_CELL_LOCAL_WRITE ||
                    op==XR_XIR_PLACE_READ || op==XR_XIR_PLACE_WRITE || op==XR_XIR_CELL_PROJECT) &&
                    witness.slot==decoded.args[0]?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            }
            if (!xir_compile_work(work,sizeof(XrXirOp))) return XR_XIR_BUDGET;
            XrXirOp op=XR_XIR_INVALID;
            XrXirStatus status=effect_body_instruction_op(&read,function,witness.instruction,&op);
            if (status!=XR_XIR_OK) return status;
            if (atom>=2 && atom-2<parameters)
                return witness.cause==XR_XIR_ROOT_CAUSE_PARAMETER && witness.slot==atom-2 &&
                    (op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            if (atom>=2+parameters && atom-2-parameters<parameters)
                return witness.cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER && witness.slot==atom-2-parameters &&
                    (op==XR_XIR_CELL_READ || op==XR_XIR_CELL_WRITE || op==XR_XIR_CELL_STRING_APPEND || op==XR_XIR_CELL_LOCAL_WRITE ||
                    op==XR_XIR_PLACE_READ || op==XR_XIR_PLACE_WRITE || op==XR_XIR_CELL_PROJECT)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            if (atom==(uint64_t)parameters*2+2)
                return witness.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL && witness.slot==UINT32_MAX &&
                    effect_invocation_frontier(op)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            if (atom>1) return XR_XIR_BAD_STRUCTURE;
            if (witness.cause==XR_XIR_ROOT_CAUSE_INDIRECT)
                return (op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT) &&
                    witness.slot==UINT32_MAX?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            if (witness.cause==XR_XIR_ROOT_CAUSE_REQUIREMENT)
                return op==XR_XIR_CALL_REQUIREMENT && witness.slot==UINT32_MAX?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            if ((witness.cause==XR_XIR_ROOT_CAUSE_MUTABLE_SLOT ||
                witness.cause==XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST) && !atom) {
                return certificate->declarations && witness.slot<certificate->declarations->slot_count &&
                    (op==XR_XIR_SLOT_LOAD || op==XR_XIR_SLOT_PLACE || op==XR_XIR_SLOT_STORE)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            }
            return XR_XIR_BAD_STRUCTURE;
        }
        if (cause->next>=equations->count || cause->edge>=equations->edge_count) return XR_XIR_BAD_STRUCTURE;
        EffectInvocationEdge edge=equations->edges[cause->edge];
        const EffectInvocationNode *target=&equations->nodes[cause->next];
        if (edge.latent || edge.caller!=current || edge.target!=cause->next || target->root!=root ||
            target->body>=effect_body_count(certificate->body) || edge.instruction>=function->instruction_count ||
            cause->witness.instruction!=edge.instruction || cause->witness.callee!=target->body ||
            cause->witness.slot!=UINT32_MAX || target->causes[atom].distance+1!=cause->distance)
            return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(work,sizeof(XrXirOp))) return XR_XIR_BUDGET;
        XrXirOp op=XR_XIR_INVALID;
        XrXirStatus edge_status=effect_invocation_certificate_edge_selected(work,certificate,current,cause->edge,&op);
        if (edge_status!=XR_XIR_OK) return edge_status;
        if (cause->witness.cause!=(op==XR_XIR_CLEANUP_REGISTER?XR_XIR_ROOT_CAUSE_CLEANUP:XR_XIR_ROOT_CAUSE_CALL))
            return XR_XIR_BAD_STRUCTURE;
        if (edge.producer!=UINT32_MAX && (edge.producer>=equations->site_count ||
            equations->sites[edge.producer].target!=target->body ||
            (op!=XR_XIR_CALL_INDIRECT && op!=XR_XIR_INVOKE_INDIRECT))) return XR_XIR_BAD_STRUCTURE;
        current=cause->next;
    }
}

static XrXirStatus effect_invocation_certificate_node_mask(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t selected,uint32_t *output) {
    if (!xir_compile_context_valid(work) || !certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || certificate->resources!=work->resources ||
        !certificate->final || !certificate->declared || !output ||
        selected>=certificate->final->count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *equations=certificate->final;
    const EffectInvocationNode *node=&equations->nodes[selected];
    if (node->root>=effect_body_count(certificate->body) || node->body>=effect_body_count(certificate->body) ||
        node->cause_count!=(uint64_t)
            certificate->body->functions[node->root].parameter_count*2+3)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t parameters=certificate->body->functions[node->root].parameter_count;
    if (!xir_compile_work(work,9)) return XR_XIR_BUDGET;
    if (!effect_invocation_node_shape(node,parameters,
        certificate->body->functions[node->body].instruction_count)) return XR_XIR_BAD_STRUCTURE;
    uint32_t mask=node->intrinsic_mask;
    if (mask&~(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t atom=0;atom<node->cause_count;++atom) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        bool fact=effect_invocation_atom(node,parameters,atom,false);
        const EffectInvocationCause *cause=&node->causes[atom];
        if (fact!=(cause->distance!=UINT32_MAX) || (fact && (!cause->witness.cause ||
            cause->distance>=equations->count || cause->witness.distance!=cause->distance)))
            return XR_XIR_BAD_STRUCTURE;
        if (!fact) continue;
        XrXirStatus verified=effect_invocation_certificate_cause(work,certificate,selected,atom);
        if (verified!=XR_XIR_OK) return verified;
        if (atom<2) continue;
        if (atom-2<parameters) {
            uint32_t p=atom-2,original=0;
            if (!certificate->parameter_kinds || !certificate->parameter_kinds[node->root] ||
                certificate->parameter_kinds[node->root][p]!=XR_XIR_EFFECT_PARAMETER_VARIABLE)
                return XR_XIR_BAD_STRUCTURE;
            XrXirStatus status=effect_invocation_bounds_mask(work,certificate->declared,node->root,p,&original);
            if (status!=XR_XIR_OK) return status;
            mask|=original;
        } else mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    }
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    *output=mask;return XR_XIR_OK;
}

/* This is a compile-time edge projection within the single common owner.
 * The public Source ordinals and physical vectors are matched by its existing
 * context selector before these private contextual IDs can reach the query. */
static inline XrXirStatus effect_invocation_certificate_edge_mask(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t caller,uint32_t instruction,
    uint32_t target,uint32_t *output) {
    if (!xir_compile_context_valid(work) || !certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || certificate->resources!=work->resources ||
        !certificate->final || !output || caller>=effect_body_count(certificate->body) ||
        target>=effect_body_count(certificate->body) ||
        instruction>=certificate->body->functions[caller].instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *equations=certificate->final;
    EffectBodyRead read=effect_body_compile_read(work);XrXirInstruction decoded;
    XrXirStatus decoded_status=effect_body_instruction_get(&read,
        &certificate->body->functions[caller],instruction,&decoded);
    if (decoded_status!=XR_XIR_OK) return decoded_status;
    const XrXirInstruction *op=&decoded;
    uint32_t parent=equations->roots[caller];
    if (!equations->root_spaces || parent>=equations->count || equations->nodes[parent].body!=caller ||
        equations->nodes[parent].root!=equations->root_spaces[caller]) return XR_XIR_BAD_STRUCTURE;
    uint32_t root=equations->nodes[parent].root;
    bool executing=op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_CLEANUP_REGISTER ||
        op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT;
    bool transport=op->op==XR_XIR_GO,reference=op->op==XR_XIR_FUNCTION_REF;
    if (!executing && !transport && !reference) return XR_XIR_BAD_STRUCTURE;
    uint32_t mask=0;bool found=false;uint64_t walked=0;
    for (uint32_t e=equations->nodes[parent].edge_head;e!=UINT32_MAX;e=equations->edges[e].next) {
        if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
        if (e>=equations->edge_count || ++walked>equations->edge_count) return XR_XIR_BAD_STRUCTURE;
        EffectInvocationEdge edge=equations->edges[e];
        if (edge.caller!=parent || edge.target>=equations->count ||
            (edge.next!=UINT32_MAX && edge.next>=e)) return XR_XIR_BAD_STRUCTURE;
        if (edge.instruction!=instruction || equations->nodes[edge.target].body!=target) continue;
        XrXirStatus edge_status=effect_invocation_certificate_edge(work,certificate,parent,e);
        if (edge_status!=XR_XIR_OK) return edge_status;
        if (edge.latent!=!executing || equations->nodes[edge.target].root!=root ||
            (reference ? edge.producer==UINT32_MAX : edge.producer!=UINT32_MAX))
            return XR_XIR_BAD_STRUCTURE;
        if (reference) {
            if (edge.producer>=equations->site_count) return XR_XIR_BAD_STRUCTURE;
            EffectInvocationSite site=equations->sites[edge.producer];
            if (site.function!=caller || site.instruction!=instruction || site.target!=target ||
                site.captures!=op->args[1]) return XR_XIR_BAD_STRUCTURE;
        }
        uint32_t actual=0;
        XrXirStatus status=effect_invocation_certificate_node_mask(work,certificate,edge.target,&actual);
        if (status!=XR_XIR_OK) return status;
        found=true;mask|=actual;
    }
    if (!found) mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    *output=mask;return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_class_value(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t selected,uint32_t function,
    uint32_t value,uint32_t *output) {
    if (!xir_compile_context_valid(context) || !effects || !effects->invocations ||
        effects->resources!=context->resources || !output || *output) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCertificate *certificate=effects->invocations;
    const EffectInvocationFinal *owner=certificate->final;
    if (!effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || !owner ||
        certificate->resources!=context->resources || function>=effect_body_count(certificate->body) ||
        selected>=owner->count || !owner->nodes) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->nodes[selected];
    const EffectBodyFunction *body=&certificate->body->functions[function];
    if (node->body!=function ||
        value>=(uint64_t)body->parameter_count+body->instruction_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRead read=effect_body_compile_read(context);XrXirType type;
    XrXirStatus status=effect_body_operand_type(&read,body,value,&type);
    if (status!=XR_XIR_OK) return status;
    if (!xr_xir_type_is_class(certificate->body->domain.types,type)) return XR_XIR_BAD_STRUCTURE;
    uint32_t at=0;status=effect_invocation_class_find(context,node,value,&at);
    if (status!=XR_XIR_OK) return status;
    EffectInvocationClassFact fact=node->classes[at];
    if ((fact.mask&~EFFECT_CLASS_VALID) || !(fact.mask&EFFECT_CLASS_KNOWN) ||
        (fact.mask==EFFECT_CLASS_KNOWN?!fact.rank:fact.rank!=0)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,6)) return XR_XIR_BUDGET;
    *output=fact.mask;return XR_XIR_OK;
}
