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

static XrXirStatus effect_invocation_edge_shape(const XrXirCompileContext *work,
    const EffectInvocationOwner *equations,const XrXirModule *module,
    const XrXirTypes *types,uint32_t caller,uint32_t edge_index) {
    if (caller>=equations->count || edge_index>=equations->edge_count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationEdge edge=equations->edges[edge_index];
    const EffectInvocationNode *from=&equations->nodes[caller];
    if (edge.caller!=caller || edge.target>=equations->count || from->body>=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *to=&equations->nodes[edge.target];
    const XrXirFunction *function=&module->functions[from->body];
    if (to->root!=from->root || to->body>=module->function_count ||
        edge.instruction>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&function->instructions[edge.instruction];
    if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_CLEANUP_REGISTER || op->op==XR_XIR_GO)
        return edge.producer==UINT32_MAX && op->immediate>=0 && (uint64_t)op->immediate==to->body &&
            edge.latent==(op->op==XR_XIR_GO)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        const uint32_t *identity=xr_xir_default_identity(op);const XrXirDefaultBinding *binding=NULL;
        XrXirStatus status=xr_xir_compile_default_lookup(work,module,identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        return !edge.latent && edge.producer==UINT32_MAX && binding && binding->function==to->body?
            XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    if (edge.producer>=equations->site_count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationSite site=equations->sites[edge.producer];
    if (site.function>=module->function_count || site.target!=to->body ||
        site.binding>equations->binding_count || site.captures>equations->binding_count-site.binding)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *producer=&module->functions[site.function];
    if (site.instruction>=producer->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *reference=&producer->instructions[site.instruction];
    if (reference->op!=XR_XIR_FUNCTION_REF || reference->immediate<0 ||
        (uint64_t)reference->immediate!=site.target || reference->args[1]!=site.captures)
        return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_FUNCTION_REF)
        return edge.latent && site.function==from->body && site.instruction==edge.instruction?
            XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if ((op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) || edge.latent ||
        op->immediate<0 || (uint64_t)op->immediate>=(uint64_t)function->parameter_count+function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *actual=xr_xir_callable_signature(types,
        xr_xir_operand_type(function,(uint32_t)op->immediate));
    const XrXirTypeNode *bound=xr_xir_callable_signature(types,reference->type);
    const XrXirFunction *target=&module->functions[to->body];
    if (!actual || !bound || actual->parameter_count!=op->args[1] ||
        site.captures>target->parameter_count || bound->parameter_count!=target->parameter_count-site.captures ||
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
            types,types,NULL,0,
            actual->parameters[p].type,bound->parameters[p].type,&scratch);
    }
    xr_xir_type_match_scratch_free(&scratch);
    return status==XR_XIR_BAD_TYPE?XR_XIR_BAD_STRUCTURE:status;
}

static XrXirStatus effect_invocation_certificate_edge(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t caller,uint32_t edge_index) {
    return effect_invocation_edge_shape(work,certificate->equations,&certificate->bodies,
        &certificate->terms.types,caller,edge_index);
}

/* Each walk consumes the immutable local terminal copy. Actual call links
 * name the full child equation and real producer site, and strictly decrease
 * the owned forest distance. A same-number target cannot stand in for a link. */
static XrXirStatus effect_invocation_certificate_cause(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t selected,uint32_t atom) {
    const EffectInvocationOwner *equations=certificate->equations;
    uint32_t root=equations->nodes[selected].root;
    uint32_t parameters=certificate->bodies.functions[root].parameter_count;
    uint32_t current=selected,walked=0;
    for (;;) {
        if (!xir_compile_work(work,10)) return XR_XIR_BUDGET;
        if (current>=equations->count || ++walked>equations->count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationNode *node=&equations->nodes[current];
        if (node->root!=root || node->body>=certificate->bodies.function_count || atom>=node->cause_count ||
            !node->causes || !node->local_causes || !effect_invocation_atom(node,parameters,atom,false))
            return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationCause *cause=&node->causes[atom];
        const XrXirFunction *function=&certificate->bodies.functions[node->body];
        if (cause->distance>=equations->count || cause->witness.distance!=cause->distance ||
            !cause->witness.cause) return XR_XIR_BAD_STRUCTURE;
        if (!cause->distance) {
            if (cause->next!=UINT32_MAX || cause->edge!=UINT32_MAX ||
                !effect_invocation_atom(node,parameters,atom,true) ||
                !effect_invocation_witness_equal(&cause->witness,&node->local_causes[atom].witness))
                return XR_XIR_BAD_STRUCTURE;
            XrXirRootEffectWitness witness=cause->witness;
            if (witness.cause==XR_XIR_ROOT_CAUSE_INITIALIZER) {
                const XrXirDeclarations *declarations=certificate->declarations;
                if (witness.instruction!=UINT32_MAX) {
                    if (witness.instruction>=function->instruction_count || !declarations || atom ||
                        witness.callee!=UINT32_MAX || witness.slot>=declarations->slot_count)
                        return XR_XIR_BAD_STRUCTURE;
                    const XrXirInstruction *op=&function->instructions[witness.instruction];
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
            XrXirOp op=function->instructions[witness.instruction].op;
            if (atom>=2 && atom-2<parameters)
                return witness.cause==XR_XIR_ROOT_CAUSE_PARAMETER && witness.slot==atom-2 &&
                    (op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            if (atom>=2+parameters && atom-2-parameters<parameters)
                return witness.cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER && witness.slot==atom-2-parameters &&
                    (op==XR_XIR_CELL_READ || op==XR_XIR_CELL_WRITE || op==XR_XIR_CELL_LOCAL_WRITE ||
                    op==XR_XIR_PLACE_READ || op==XR_XIR_PLACE_WRITE || op==XR_XIR_CELL_PROJECT)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            if (atom==(uint64_t)parameters*2+2)
                return witness.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL && witness.slot==UINT32_MAX &&
                    effect_invocation_frontier(op)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            if (atom>1) return XR_XIR_BAD_STRUCTURE;
            if (witness.cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS)
                return (op==XR_XIR_CELL_READ || op==XR_XIR_CELL_WRITE || op==XR_XIR_CELL_LOCAL_WRITE ||
                    op==XR_XIR_PLACE_READ || op==XR_XIR_PLACE_WRITE || op==XR_XIR_CELL_PROJECT) &&
                    witness.slot==function->instructions[witness.instruction].args[0]?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
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
            target->body>=certificate->bodies.function_count || edge.instruction>=function->instruction_count ||
            cause->witness.instruction!=edge.instruction || cause->witness.callee!=target->body ||
            cause->witness.slot!=UINT32_MAX || target->causes[atom].distance+1!=cause->distance)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus edge_status=effect_invocation_certificate_edge(work,certificate,current,cause->edge);
        if (edge_status!=XR_XIR_OK) return edge_status;
        XrXirOp op=function->instructions[edge.instruction].op;
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
    if (!xir_compile_context_valid(work) || !certificate || certificate->resources!=work->resources ||
        !certificate->equations || !certificate->declared || !output ||
        selected>=certificate->equations->count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationOwner *equations=certificate->equations;
    const EffectInvocationNode *node=&equations->nodes[selected];
    if (node->root>=certificate->bodies.function_count || node->body>=certificate->bodies.function_count ||
        !node->expanded || node->queued || node->cause_count!=(uint64_t)
            certificate->bodies.functions[node->root].parameter_count*2+3)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t parameters=certificate->bodies.functions[node->root].parameter_count;
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
    if (!xir_compile_context_valid(work) || !certificate || certificate->resources!=work->resources ||
        !certificate->equations || !output || caller>=certificate->bodies.function_count ||
        target>=certificate->bodies.function_count ||
        instruction>=certificate->bodies.functions[caller].instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationOwner *equations=certificate->equations;
    const XrXirInstruction *op=&certificate->bodies.functions[caller].instructions[instruction];
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
