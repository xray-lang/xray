/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_edge_inventory.inc.c - Current Input edge coordinates and linkage
 *
 * KEY CONCEPT:
 *   Every stored edge names a real frontier and occurs once in both indices.
 *   Structural ancestry is separate from full actual argument and coverage proofs.
 */
static XrXirStatus effect_final_edge_signature(const XrXirCompileContext *work,
    const XrXirTypes *types,XrXirType type,const XrXirTypeNode **output) {
    const XrXirTypeNode *node=xr_xir_callable_signature(types,type);
    if(!node || !xr_xir_callable_flags_valid(node->flags) || !!node->parameters!=!!node->parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,node->parameters,node->parameter_count,sizeof(*node->parameters));
    if(status!=XR_XIR_OK) return status;
    for(uint32_t p=0;p<node->parameter_count;++p) {
        if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if(!xr_xir_callable_parameter_storage_valid(types,&node->parameters[p])) return XR_XIR_BAD_STRUCTURE;
    }
    *output=node;return XR_XIR_OK;
}
static XrXirStatus effect_final_edge_default(const XrXirCompileContext *work,
    const EffectBodyRequest *body,const XrXirInstruction *op,uint32_t *target) {
    const XrXirDefaultTable *table=body->domain.defaults;
    XrXirStatus status=effect_role_range(work,table,1,sizeof(*table));if(status!=XR_XIR_OK) return status;
    status=effect_role_range(work,table->records,table->count,sizeof(*table->records));if(status!=XR_XIR_OK) return status;
    const uint32_t *identity=xr_xir_default_identity(op);const XrXirDefaultBinding *binding=NULL;
    status=xir_metadata_default_lookup(work,&body->domain,identity[0],identity[1],&binding);
    if(status!=XR_XIR_OK) return status;
    if(!binding || binding->function>=body->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    *target=binding->function;return XR_XIR_OK;
}
static XrXirStatus effect_final_edge_operands(const XrXirCompileContext *work,
    const EffectBodyFunctionHead *head,const XrXirInstruction *op,uint32_t count) {
    if(op->args[1]!=count || op->args[0]>head->operand_count || count>head->operand_count-op->args[0])
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,head->operands,head->operand_count,sizeof(*head->operands));
    if(status!=XR_XIR_OK) return status;
    uint64_t values=(uint64_t)head->parameter_count+head->instruction_count;
    for(uint32_t p=0;p<count;++p) {
        if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if(head->operands[op->args[0]+p]>=values) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_final_edge_payload(const XrXirCompileContext *work,
    const EffectBodyRequest *body,const EffectInvocationFinal *final,uint32_t selected) {
    const EffectInvocationEdge *edge=&final->edges[selected];
    const EffectInvocationNode *from=&final->nodes[edge->caller],*to=&final->nodes[edge->target];
    if(from->root!=to->root || edge->instruction>=from->instruction_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionHead head={0},target={0};XrXirInstruction storage;const XrXirInstruction *op=NULL;
    XrXirStatus status=effect_body_request_head(body,from->body,&head);
    if(status==XR_XIR_OK) status=effect_body_request_head(body,to->body,&target);
    if(status==XR_XIR_OK) status=effect_body_request_instruction(body,from->body,edge->instruction,&storage,&op);
    if(status!=XR_XIR_OK) return status;
    if(!op || !xir_compile_work(work,8)) return op?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    if(op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_CLEANUP_REGISTER || op->op==XR_XIR_GO || op->op==XR_XIR_GENERATOR_NEW) {
        if(op->immediate<0 || (uint64_t)op->immediate!=to->body || edge->producer!=UINT32_MAX ||
            edge->latent!=(op->op==XR_XIR_GO || op->op==XR_XIR_GENERATOR_NEW)) return XR_XIR_BAD_STRUCTURE;
        if(op->op==XR_XIR_CLEANUP_REGISTER) {
            const XrXirDeclarations *declarations=body->domain.declarations;
            status=effect_role_range(work,declarations,1,sizeof(*declarations));if(status!=XR_XIR_OK) return status;
            status=effect_role_range(work,declarations->functions,body->domain.function_count,sizeof(*declarations->functions));
            if(status!=XR_XIR_OK) return status;
            if(declarations->functions[to->body].cleanup_owner!=from->body+1) return XR_XIR_BAD_STRUCTURE;
        }
        return effect_final_edge_operands(work,&head,op,target.parameter_count);
    }
    if(op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        uint32_t actual=UINT32_MAX;status=effect_final_edge_default(work,body,op,&actual);
        if(status!=XR_XIR_OK) return status;
        return actual==to->body && !edge->latent && edge->producer==UINT32_MAX && !target.parameter_count?
            XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    if(edge->producer>=final->site_count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationSite *site=&final->sites[edge->producer];
    if(site->target!=to->body || site->captures>target.parameter_count) return XR_XIR_BAD_STRUCTURE;
    if(op->op==XR_XIR_FUNCTION_REF) {
        if(!edge->latent || site->function!=from->body || site->instruction!=edge->instruction) return XR_XIR_BAD_STRUCTURE;
        return effect_final_edge_operands(work,&head,op,site->captures);
    }
    if(effect_invocation_iterator_operation(op->op)) {
        if(edge->latent || site->captures!=target.parameter_count ||
            op->args[0]>=(uint64_t)head.parameter_count+head.instruction_count) return XR_XIR_BAD_STRUCTURE;
        XrXirType receiver=0;status=effect_body_request_type(body,from->body,op->args[0],&receiver);
        if(status!=XR_XIR_OK) return status;
        return effect_physical_iterator(body->domain.types,receiver)?XR_XIR_OK:XR_XIR_BAD_TYPE;
    }
    if((op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) || edge->latent ||
        op->immediate<0 || (uint64_t)op->immediate>=(uint64_t)head.parameter_count+head.instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirType type=0;status=effect_body_request_type(body,from->body,(uint32_t)op->immediate,&type);
    const XrXirTypeNode *signature=NULL;
    if(status==XR_XIR_OK) status=effect_final_edge_signature(work,body->domain.types,type,&signature);
    if(status!=XR_XIR_OK) return status;
    if(signature->parameter_count!=target.parameter_count-site->captures) return XR_XIR_BAD_STRUCTURE;
    return effect_final_edge_operands(work,&head,op,signature->parameter_count);
}
/* Strictly descending outgoing links give linear exact membership. The reverse
 * link is compared with the previous real target occurrence, without scratch. */
static XrXirStatus effect_final_edge_links(const XrXirCompileContext *work,const EffectInvocationFinal *final) {
    uint64_t linked=0;
    for(uint32_t n=0;n<final->count;++n) {
        if(!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        for(uint32_t e=final->nodes[n].edge_head;e!=UINT32_MAX;e=final->edges[e].next) {
            if(!xir_compile_work(work,4)) return XR_XIR_BUDGET;
            if(e>=final->edge_count || ++linked>final->edge_count || final->edges[e].caller!=n ||
                (final->edges[e].next!=UINT32_MAX && final->edges[e].next>=e)) return XR_XIR_BAD_STRUCTURE;
        }
    }
    if(linked!=final->edge_count) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t e=0;e<final->edge_count;++e) {
        const EffectInvocationEdge *edge=&final->edges[e];uint32_t previous=UINT32_MAX;
        for(uint32_t p=e;p>0;) {
            --p;if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            const EffectInvocationEdge *other=&final->edges[p];
            if(other->target!=edge->target) continue;
            if(previous==UINT32_MAX) previous=p;
            if(other->caller==edge->caller && other->instruction==edge->instruction &&
                other->producer==edge->producer && other->latent==edge->latent) return XR_XIR_BAD_STRUCTURE;
        }
        if(edge->consumer_next!=previous) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_final_edge_present(const XrXirCompileContext *work,const EffectInvocationFinal *final,
    uint32_t caller,uint32_t instruction,uint32_t target,bool latent) {
    for(uint32_t e=final->nodes[caller].edge_head;e!=UINT32_MAX;e=final->edges[e].next) {
        if(!xir_compile_work(work,5)) return XR_XIR_BUDGET;
        const EffectInvocationEdge *edge=&final->edges[e];
        if(edge->instruction==instruction && final->nodes[edge->target].body==target && edge->latent==latent)
            return XR_XIR_OK;
    }
    return XR_XIR_BAD_STRUCTURE;
}
/* Closed direct/default/GO frontiers cannot disappear. Indirect target coverage
 * and captured value contents require their independent full relation proof. */
static XrXirStatus effect_final_edge_frontiers(const XrXirCompileContext *work,
    const EffectBodyRequest *body,const EffectInvocationFinal *final) {
    for(uint32_t n=0;n<final->count;++n) {
        EffectBodyFunctionHead head={0};XrXirStatus status=effect_body_request_head(body,final->nodes[n].body,&head);
        EffectFinalInputSelect cursor;
        if(status==XR_XIR_OK) status=effect_final_input_select_begin(body,EFFECT_FINAL_SELECT_EDGE,&cursor);
        if(status!=XR_XIR_OK) return status;
        for(;;) {
            XrXirInstruction storage;const XrXirInstruction *op=&storage;uint32_t i=0;bool found=false;
            status=effect_final_input_select_next(body,final->nodes[n].body,&cursor,&i,&storage,&found);
            if(status!=XR_XIR_OK) return status;
            if(!found) break;
            if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            bool direct=op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_CLEANUP_REGISTER || op->op==XR_XIR_GO || op->op==XR_XIR_GENERATOR_NEW;
            bool defaults=op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT;
            if(!direct && !defaults) continue;
            uint32_t target=UINT32_MAX;
            if(defaults) status=effect_final_edge_default(work,body,op,&target);
            else if(op->immediate<0 || (uint64_t)op->immediate>=body->domain.function_count) status=XR_XIR_BAD_STRUCTURE;
            else target=(uint32_t)op->immediate;
            EffectBodyFunctionHead callee={0};
            if(status==XR_XIR_OK) status=effect_body_request_head(body,target,&callee);
            if(status!=XR_XIR_OK) return status;
            bool closed=!xr_xir_type_span(body->domain.types,callee.result);
            if(defaults && callee.parameter_count) return XR_XIR_BAD_STRUCTURE;
            if(direct) {
                status=effect_final_edge_operands(work,&head,op,callee.parameter_count);
                if(status!=XR_XIR_OK) return status;
                for(uint32_t p=0;p<callee.parameter_count;++p) {
                    if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                    XrXirType actual=0;status=effect_body_request_type(body,final->nodes[n].body,
                        head.operands[op->args[0]+p],&actual);
                    if(status!=XR_XIR_OK) return status;
                    if(xr_xir_type_span(body->domain.types,actual) || xr_xir_type_span(body->domain.types,callee.parameters[p])) closed=false;
                }
            }
            if(closed) {
                status=effect_final_edge_present(work,final,n,i,target,op->op==XR_XIR_GO || op->op==XR_XIR_GENERATOR_NEW);
                if(status!=XR_XIR_OK) return status;
            }
        }

    }
    return XR_XIR_OK;
}
static XrXirStatus effect_final_inventory_edges(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final) {
    if(final->edge_count>final->edge_capacity ||
        (uint64_t)final->edge_capacity>(uint64_t)work->limits.functions*work->limits.instructions ||
        !!final->edges!=!!final->edge_capacity) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,final->edges,final->edge_capacity,sizeof(*final->edges));
    if(status!=XR_XIR_OK) return status;
    EffectBodyRequest body=effect_body_owned_request(work,material);
    for(uint32_t e=0;e<final->edge_count;++e) {
        if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if(final->edges[e].caller>=final->count || final->edges[e].target>=final->count) return XR_XIR_BAD_STRUCTURE;
        status=effect_final_edge_payload(work,&body,final,e);if(status!=XR_XIR_OK) return status;
    }
    status=effect_final_edge_links(work,final);if(status!=XR_XIR_OK) return status;
    for(uint32_t n=0;n<final->count;++n) {
        if(!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if(final->roots[final->nodes[n].body]==n) continue;
        bool parent=false;
        for(uint32_t e=0;e<final->edge_count;++e) {
            if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if(final->edges[e].target==n && final->edges[e].caller<n) { parent=true;break; }
        }
        if(!parent) return XR_XIR_BAD_STRUCTURE;
    }
    return effect_final_edge_frontiers(work,&body,final);
}
