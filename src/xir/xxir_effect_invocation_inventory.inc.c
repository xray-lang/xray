/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_inventory.inc.c - Actual callable basis and distinct environment identities
 *
 * KEY CONCEPT:
 *   The complete current Input defines the site basis and public root spaces.
 *   Owned key storage is framed before comparison; duplicate keys never add an environment.
 */
#include "xxir_effect_final_input_select.inc.c"
static XrXirStatus effect_final_inventory_sites(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final);
#include "xxir_effect_invocation_edge_inventory.inc.c"
#include "xxir_effect_capture_global.inc.c"
#include "xxir_effect_invocation_final_flow.inc.c"
#include "xxir_effect_invocation_final_view.inc.c"
static XrXirStatus effect_final_inventory_sites(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final) {
    XrXirStatus status=effect_role_range(work,final->sites,final->site_count,sizeof(*final->sites));
    if(status!=XR_XIR_OK || !!final->sites!=!!final->site_count) return status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
    EffectBodyRequest body=effect_body_owned_request(work,material);uint32_t at=0,bindings=0;
    for(uint32_t f=0;f<effect_body_count(material);++f) {
        EffectBodyFunctionHead head={0};status=effect_body_request_head(&body,f,&head);
        EffectFinalInputSelect cursor;
        if(status==XR_XIR_OK) status=effect_final_input_select_begin(&body,EFFECT_FINAL_SELECT_REFERENCE,&cursor);
        if(status!=XR_XIR_OK) return status;
        for(;;) {
            XrXirInstruction storage;const XrXirInstruction *op=&storage;uint32_t i=0;bool found=false;
            status=effect_final_input_select_next(&body,f,&cursor,&i,&storage,&found);
            if(status!=XR_XIR_OK) return status;
            if(!found) break;
            if(!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            if(at>=final->site_count || op->immediate<0 || (uint64_t)op->immediate>=effect_body_count(material))
                return XR_XIR_BAD_STRUCTURE;
            EffectBodyFunctionHead target={0};status=effect_body_request_head(&body,(uint32_t)op->immediate,&target);
            if(status!=XR_XIR_OK) return status;
            if(!xir_compile_work(work,6)) return XR_XIR_BUDGET;
            const EffectInvocationSite *site=&final->sites[at++];
            if(site->function!=f || site->instruction!=i || site->target!=(uint32_t)op->immediate ||
                site->captures!=op->args[1] || site->binding!=bindings || site->captures>target.parameter_count ||
                site->captures>UINT32_MAX-bindings) return XR_XIR_BAD_STRUCTURE;
            bindings+=site->captures;
        }

    }
    return at==final->site_count && bindings==final->binding_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus effect_final_inventory_tracked(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,XrXirType type,bool *output) {
    const XrXirTypes *types=material->domain.types;const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
    *output=node && (node->kind==XR_XIR_TYPE_CELL || node->kind==XR_XIR_TYPE_CALLABLE);
    if(!node || node->kind!=XR_XIR_TYPE_NOMINAL) return XR_XIR_OK;
    const XrXirNominalTable *table=types->nominals;
    XrXirStatus status=effect_role_range(work,table,1,sizeof(*table));if(status!=XR_XIR_OK) return status;
    if(!!table->declarations==!!table->identities || node->nominal.declaration>=table->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t kind=0;
    if(table->declarations) {
        status=effect_role_range(work,table->declarations,table->count,sizeof(*table->declarations));
        if(status==XR_XIR_OK) kind=table->declarations[node->nominal.declaration].kind;
    } else {
        status=effect_role_range(work,table->identities,table->count,sizeof(*table->identities));
        if(status==XR_XIR_OK) kind=table->identities[node->nominal.declaration].kind;
    }
    if(status==XR_XIR_OK) *output=kind==XR_XIR_NOMINAL_CLASS;return status;
}
static XrXirStatus effect_final_inventory_roots(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final) {
    uint32_t functions=effect_body_count(material),shared=UINT32_MAX;
    XrXirStatus status=effect_role_range(work,final->roots,functions,sizeof(*final->roots));
    if(status==XR_XIR_OK) status=effect_role_range(work,final->root_spaces,functions,sizeof(*final->root_spaces));
    if(status!=XR_XIR_OK) return status;
    for(uint32_t f=0;f<functions;++f) {
        const EffectBodyFunction *head=&material->functions[f];bool tracked=false;
        for(uint32_t p=0;p<head->parameter_count;++p) {
            bool current=false;status=effect_final_inventory_tracked(work,material,head->parameters[p],&current);
            if(status!=XR_XIR_OK) return status;
            tracked|=current;
        }
        if(!tracked && shared==UINT32_MAX) shared=f;
        uint32_t expected=tracked?f:shared;
        if(!xir_compile_work(work,5)) return XR_XIR_BUDGET;
        if(final->root_spaces[f]!=expected || final->roots[f]>=final->count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationNode *root=&final->nodes[final->roots[f]];
        if(root->root!=expected || root->body!=f) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_final_inventory_keys(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final) {
    uint32_t functions=effect_body_count(material);
    for(uint32_t n=0;n<final->count;++n) {
        const EffectInvocationNode *node=&final->nodes[n];
        if(!xir_compile_work(work,5)) return XR_XIR_BUDGET;
        if(node->root>=functions || node->body>=functions || node->parameter_count!=material->functions[node->body].parameter_count ||
            node->instruction_count!=material->functions[node->body].instruction_count) return XR_XIR_BAD_STRUCTURE;
        EffectInvocationBasis basis={0};XrXirStatus status=effect_invocation_basis_parameters_read(work,final,
            material->functions[node->root].parameter_count,&basis);if(status!=XR_XIR_OK) return status;
        uint64_t words=(uint64_t)node->parameter_count*basis.row_words;
        if(!!node->input!=!!words) return XR_XIR_BAD_STRUCTURE;
        status=effect_role_range(work,node->input,words,sizeof(*node->input));if(status!=XR_XIR_OK) return status;
        for(uint32_t p=0;p<node->parameter_count;++p) {
            bool tracked=false;
            status=effect_final_inventory_tracked(work,material,material->functions[node->body].parameters[p],&tracked);
            if(status!=XR_XIR_OK) return status;
            if(tracked || final->roots[node->body]==n) continue;
            for(uint32_t w=0;w<basis.row_words;++w) {
                if(!xir_compile_work(work,1)) return XR_XIR_BUDGET;
                if(node->input[(uint64_t)p*basis.row_words+w]) return XR_XIR_BAD_STRUCTURE;
            }
        }
        for(uint32_t earlier=0;earlier<n;++earlier) {
            const EffectInvocationNode *other=&final->nodes[earlier];
            if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if(node->root!=other->root || node->body!=other->body) continue;
            bool same=true;
            for(uint64_t w=0;w<words;++w) {
                if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                if(node->input[w]!=other->input[w]) same=false;
            }
            if(same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_final_inventory_seeds(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final) {
    EffectBodyRequest body=effect_body_owned_request(work,material);EffectBodyRead role=effect_body_compile_read(work);
    for(uint32_t f=0;f<effect_body_count(material);++f) {
        EffectInvocationSeedRead read={0};XrXirStatus status=effect_invocation_seed_open(work,&body,final,f,&read);
        if(status!=XR_XIR_OK) return status;
        const EffectInvocationNode *node=&final->nodes[final->roots[f]];
        for(uint32_t p=0;p<read.head.parameter_count;++p) {
            uint32_t kind=XR_XIR_EFFECT_PARAMETER_FIXED;
            if(xr_xir_callable_signature(body.domain.types,read.head.parameters[p]))
                status=effect_role_parameter_value(&role,material,&final->role_proof,f,p,&kind);
            EffectInvocationSeedBits bits={0};
            if(status==XR_XIR_OK) status=effect_invocation_seed_bits(&read,p,kind,&bits);
            if(status!=XR_XIR_OK) return status;
            for(uint32_t w=0;w<read.basis.row_words;++w) {
                uint64_t expected=0;status=effect_invocation_seed_word(&read,&bits,w,&expected);
                if(status!=XR_XIR_OK) return status;
                if(!xir_compile_work(work,1)) return XR_XIR_BUDGET;
                if(node->input[(uint64_t)p*read.basis.row_words+w]!=expected) return XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_final_inventory_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final) {
    XrXirStatus status=effect_role_range(work,final,1,sizeof(*final));if(status!=XR_XIR_OK) return status;
    status=effect_role_proof_match(work,material,&final->role_proof);if(status!=XR_XIR_OK) return status;
    uint32_t functions=effect_body_count(material);
    if(final->material!=material || final->physical!=material->physical || final->count<functions || final->count>final->capacity ||
        (uint64_t)final->count>(uint64_t)work->limits.functions*work->limits.instructions) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,final->nodes,final->capacity,sizeof(*final->nodes));
    if(status==XR_XIR_OK) status=effect_final_inventory_sites(work,material,final);
    if(status==XR_XIR_OK) status=effect_final_inventory_roots(work,material,final);
    if(status==XR_XIR_OK) status=effect_final_inventory_keys(work,material,final);
    if(status==XR_XIR_OK) status=effect_final_inventory_seeds(work,material,final);
    if(status==XR_XIR_OK) status=effect_final_inventory_edges(work,material,final);
    if(status==XR_XIR_OK) status=effect_capture_match(work,final);
    return status;
}
static XrXirStatus effect_final_inventories_match(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectFinalOutcomes *outcomes) {
    XrXirStatus status=effect_role_range(work,certificate,1,sizeof(*certificate));if(status!=XR_XIR_OK) return status;
    if(certificate->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    status=effect_final_inventory_match(work,certificate->body,certificate->final);if(status!=XR_XIR_OK) return status;
    status=effect_role_range(work,outcomes,1,sizeof(*outcomes));if(status!=XR_XIR_OK) return status;
    status=effect_role_contracts_match(work,certificate->body,&certificate->final->role_proof,
        outcomes->contracts,effect_body_count(certificate->body));
    if(status!=XR_XIR_OK || !outcomes->contexts) return status;
    const EffectContextOwner *contexts=outcomes->contexts;
    status=effect_role_range(work,contexts,1,sizeof(*contexts));if(status!=XR_XIR_OK) return status;
    status=effect_role_range(work,contexts->forest,1,sizeof(*contexts->forest));if(status!=XR_XIR_OK) return status;
    const EffectInvocationCertificate *nested=contexts->forest->invocations;
    status=effect_role_range(work,nested,1,sizeof(*nested));if(status!=XR_XIR_OK) return status;
    if(nested->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    return effect_final_inventory_match(work,nested->body,nested->final);
}
