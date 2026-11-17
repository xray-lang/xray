/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_callback_loan.inc.c - Paired entry-loan facts in the ROOT owner
 */
#include "xxir_effect_callback_loan_internal.h"
#include "xxir_view_liveness_internal.h"

static XrXirStatus effect_callback_loan_context(const XrXirCompileContext *memory,
    const XrXirModule *module,const XrXirEffects *effects,EffectCheckedRoots **output) {
    if (!module || !module->functions || !module->declarations || !module->declarations->functions ||
        !effects || !effects->checked_roots ||
        !xir_effects_context_matches(memory,effects,module->function_count)) return XR_XIR_BAD_STRUCTURE;
    EffectCheckedRoots *roots=effects->checked_roots;
    if (roots->templates!=module->function_count) return XR_XIR_BAD_STRUCTURE;
    *output=roots;return XR_XIR_OK;
}
static XrXirStatus effect_callback_loan_record(const XrXirCompileContext *memory,EffectCheckedRoots *roots,
    uint32_t function,XirModuleCallbackLoan relation,bool source) {
    if (roots->callback_loans==UINT32_MAX) return XR_XIR_OVERFLOW;
    uint32_t fields[]={function,relation.callback_parameter,(uint32_t)relation.loan_kind,relation.loan_parameter};
    uint32_t hash=xr_hash_bytes(fields,sizeof(fields));
    uint32_t slot=roots->callback_loan_index_capacity ? hash&(roots->callback_loan_index_capacity-1u) : 0;
    while (roots->callback_loan_index_capacity && roots->callback_loan_index[slot]) {
        uint32_t link=roots->callback_loan_index[slot];
        if (link>roots->callback_loans) return XR_XIR_BAD_STRUCTURE;
        EffectCheckCallbackLoan row=roots->callback_loan[link-1];
        if (row.relation.callback_parameter==relation.callback_parameter &&
            row.function==function && row.relation.loan_kind==relation.loan_kind &&
            row.relation.loan_parameter==relation.loan_parameter) {
            if (relation.witness_instruction<row.relation.witness_instruction)
                roots->callback_loan[link-1].relation=relation;
            return XR_XIR_OK;
        }
        slot=(slot+1u)&(roots->callback_loan_index_capacity-1u);
    }
    XrXirStatus status=XR_XIR_OK;
    if (!roots->callback_loan_index_capacity || roots->callback_loans>=roots->callback_loan_index_capacity/2u) {
        uint64_t capacity=roots->callback_loan_index_capacity ? (uint64_t)roots->callback_loan_index_capacity*2u : 16u;
        if (capacity>UINT32_MAX) return XR_XIR_OVERFLOW;
        uint32_t *index=xir_compile_calloc(memory,(size_t)capacity,sizeof(*index),&status);
        if (!index) return status;
        for (uint32_t n=0;n<roots->callback_loans;++n) {
            EffectCheckCallbackLoan row=roots->callback_loan[n];
            uint32_t key[]={row.function,row.relation.callback_parameter,(uint32_t)row.relation.loan_kind,
                row.relation.loan_parameter};
            uint32_t position=xr_hash_bytes(key,sizeof(key))&((uint32_t)capacity-1u);
            while (index[position]) position=(position+1u)&((uint32_t)capacity-1u);
            index[position]=n+1u;
        }
        xr_compile_resources_free(roots->callback_loan_index);
        roots->callback_loan_index=index;roots->callback_loan_index_capacity=(uint32_t)capacity;
        slot=hash&(roots->callback_loan_index_capacity-1u);
        while (index[slot]) slot=(slot+1u)&(roots->callback_loan_index_capacity-1u);
    }
    if (roots->callback_loans==roots->callback_loan_capacity) {
        EffectCheckCallbackLoan *grown=xir_compile_array_grow(memory,roots->callback_loan,
            roots->callback_loans,sizeof(*grown),&roots->callback_loan_capacity,&status);
        if (!grown) return status;
        roots->callback_loan=grown;
    }
    uint32_t head=roots->function[function].callback_loans;
    roots->callback_loan[roots->callback_loans++]=(EffectCheckCallbackLoan){relation,function,head,source};
    roots->function[function].callback_loans=roots->callback_loans;
    if (!source) roots->function[function].callback_loan_revision=roots->callback_loans;
    roots->callback_loan_index[slot]=roots->callback_loans;
    return XR_XIR_OK;
}
static void effect_callback_loans_reindex(EffectCheckedRoots *roots) {
    if (!roots->callback_loan_index_capacity) return;
    memset(roots->callback_loan_index,0,(size_t)roots->callback_loan_index_capacity*sizeof(*roots->callback_loan_index));
    for (uint32_t n=0;n<roots->callback_loans;++n) {
        EffectCheckCallbackLoan row=roots->callback_loan[n];
        uint32_t fields[]={row.function,row.relation.callback_parameter,(uint32_t)row.relation.loan_kind,
            row.relation.loan_parameter};
        uint32_t at=xr_hash_bytes(fields,sizeof(fields))&(roots->callback_loan_index_capacity-1u);
        while (roots->callback_loan_index[at]) at=(at+1u)&(roots->callback_loan_index_capacity-1u);
        roots->callback_loan_index[at]=n+1u;
    }
}
static bool effect_callback_loans_schedule(EffectCheckBuild *build) {
    EffectCheckedRoots *roots=build->roots;
    if (!roots->callback_loans_observed || !roots->callback_loans || roots->callback_loans_scheduled) return true;
    for (uint32_t f=0;f<roots->functions;++f) {
        EffectCheckFunction row=roots->function[f];
        if (row.declaration>=roots->templates) {build->status=XR_XIR_BAD_STRUCTURE;return false;}
        if (f!=row.declaration &&
            (!effect_check_dependency(build,roots->function[row.declaration].root,row.root) ||
             !effect_check_dependency(build,row.root,roots->function[row.declaration].root))) return false;
        uint32_t visited=0;
        for (uint32_t link=roots->function[row.declaration].callback_loans;link;) {
            if (link>roots->callback_loans || visited++>=roots->callback_loans) {
                build->status=XR_XIR_BAD_STRUCTURE;return false;
            }
            EffectCheckCallbackLoan seed=roots->callback_loan[link-1];link=seed.next;
            if (!seed.source) continue;
            if (seed.function!=row.declaration || seed.relation.loan_parameter>=row.values) {
                build->status=XR_XIR_BAD_STRUCTURE;return false;
            }
            if (!effect_check_dependency(build,row.first+seed.relation.loan_parameter,row.root)) return false;
        }
    }
    roots->callback_loans_scheduled=true;return true;
}
static bool effect_callback_loan_node_formal(const EffectCheckBuild *build,uint32_t node,
    uint32_t function,uint32_t *parameter) {
    const EffectCheckedRoots *roots=build->roots;EffectCheckFunction row=roots->function[function];
    for (uint32_t depth=0;node<roots->nodes && depth<row.values;++depth) {
        EffectCheckNode value=roots->node[node];
        if (value.function!=function || value.cell || value.callable_opaque || value.escaping)
            return false;
        if (node>=row.first && node-row.first<row.parameters) {
            uint32_t ordinal=node-row.first;
            if (ordinal<row.capture_count || value.erased || !value.callable_input ||
                xr_xir_type_is_c_callable(effect_check_types_read(build),value.type)) return false;
            *parameter=ordinal;return true;
        }
        if ((value.erased && !value.conversion) || value.calls!=UINT32_MAX || value.aliases==UINT32_MAX ||
            value.aliases>=roots->aliases)
            return false;
        EffectCheckAlias alias=roots->alias[value.aliases];
        if (alias.source>=node || (!alias.callable_origin && !value.conversion)) return false;
        uint32_t visited=0;
        for (uint32_t e=alias.next;e!=UINT32_MAX;e=roots->alias[e].next) {
            if (e>=roots->aliases || visited++>=roots->aliases || roots->alias[e].source!=alias.source ||
                (!roots->alias[e].callable_origin && !value.conversion)) return false;
        }
        node=alias.source;
    }
    return false;
}
static bool effect_callback_loan_origins(EffectCheckBuild *build,uint32_t function,uint32_t callback,
    uint32_t value,XirModuleCallbackLoan witness) {
    EffectCheckedRoots *roots=build->roots;EffectCheckFunction row=roots->function[function];
    if (value>=roots->nodes) {build->status=XR_XIR_BAD_STRUCTURE;return false;}
    if (roots->node[value].function!=function) return true;
    uint32_t visited=0;
    for (uint32_t m=roots->node[value].module_sets[EFFECT_MODULE_ORIGIN];m!=UINT32_MAX;) {
        if (m>=roots->module_members || visited++>=roots->module_members) {
            build->status=XR_XIR_BAD_STRUCTURE;return false;
        }
        EffectModuleMember member=roots->module_member[m];m=member.next;
        if (member.kind!=EFFECT_MODULE_PARAMETER && member.kind!=EFFECT_MODULE_CAPTURE) continue;
        if (member.identity>=row.parameters ||
            (member.kind==EFFECT_MODULE_CAPTURE)!=(member.identity<row.capture_count)) {
            build->status=XR_XIR_BAD_STRUCTURE;return false;
        }
        XrXirType type=roots->node[row.first+member.identity].type;
        const XrXirTypeNode *shape=xr_xir_type_node(effect_check_types_read(build),type);
        if (!shape || shape->kind!=XR_XIR_TYPE_SLICE ||
            (shape->element!=XR_XIR_I64 && shape->element!=XR_XIR_U8)) continue;
        witness.callback_parameter=callback;
        witness.loan_kind=member.kind==EFFECT_MODULE_CAPTURE?XIR_MODULE_EFFECT_CAPTURE:XIR_MODULE_EFFECT_PARAMETER;
        witness.loan_parameter=member.identity;
        build->status=effect_callback_loan_record(build->memory,roots,row.declaration,witness,false);
        if (build->status!=XR_XIR_OK) return false;
    }
    return true;
}
/* Relation propagation is another property of the existing ROOT equations.
 * It uses their actual call argument IDs and their ordinary SCC work queue;
 * no function body or second call-graph solver is read at this stage. */
static bool effect_callback_loans_evaluate(EffectCheckBuild *build,uint32_t node) {
    EffectCheckedRoots *roots=build->roots;
    if (!roots->callback_loans_observed || !roots->callback_loans) return false;
    uint32_t f=roots->node[node].function;
    EffectCheckFunction row=roots->function[f];
    if (node!=row.root) return false;
    uint32_t declaration=row.declaration,visited=0;
    for (uint32_t link=roots->function[declaration].callback_loans;link;) {
        if (link>roots->callback_loans || visited++>=roots->callback_loans) {
            build->status=XR_XIR_BAD_STRUCTURE;return false;
        }
        EffectCheckCallbackLoan seed=roots->callback_loan[link-1];link=seed.next;
        if (!seed.source) continue;
        if (seed.relation.loan_parameter>=row.values) {build->status=XR_XIR_BAD_STRUCTURE;return false;}
        if (!effect_callback_loan_origins(build,f,seed.relation.callback_parameter,
            row.first+seed.relation.loan_parameter,seed.relation)) return false;
    }
    for (uint32_t c=roots->node[node].calls;c!=UINT32_MAX;) {
        EffectCheckCall call=roots->call[c];c=call.next;
        EffectCheckFunction callee=roots->function[roots->node[call.target].function];
        if (call.protocol!=2 || call.target!=callee.root) continue;
        visited=0;
        for (uint32_t link=roots->function[callee.declaration].callback_loans;link;) {
            if (link>roots->callback_loans || visited++>=roots->callback_loans) {
                build->status=XR_XIR_BAD_STRUCTURE;return false;
            }
            EffectCheckCallbackLoan pair=roots->callback_loan[link-1];link=pair.next;
            if (pair.source) continue;
            if (pair.relation.callback_parameter>=call.count || pair.relation.loan_parameter>=call.count ||
                call.first>roots->arguments || call.count>roots->arguments-call.first) {
                build->status=XR_XIR_UNRESOLVED;return false;
            }
            uint32_t callback=0;
            if (!effect_callback_loan_node_formal(build,roots->argument[call.first+pair.relation.callback_parameter],f,&callback))
                continue;
            uint32_t loan=roots->argument[call.first+pair.relation.loan_parameter];
            if (!effect_callback_loan_origins(build,f,callback,loan,pair.relation)) return false;
        }
    }
    uint32_t revision=roots->function[declaration].callback_loan_revision;
    bool changed=roots->function[f].callback_loan_seen!=revision;
    roots->function[f].callback_loan_seen=revision;
    return changed;
}
XR_FUNC XrXirStatus xir_effects_callback_loan_observe(const XrXirCompileContext *memory,
    const XrXirModule *module,const XrXirEffects *effects,const XrXirViewLiveEvent *event) {
    if (module && (!module->declarations || !module->declarations->functions)) return XR_XIR_OK;
    EffectCheckedRoots *roots=NULL;
    XrXirStatus status=effect_callback_loan_context(memory,module,effects,&roots);
    if (status!=XR_XIR_OK || !event) return status!=XR_XIR_OK?status:XR_XIR_BAD_STRUCTURE;
    if (effect_final_outcomes(effects) || event->kind!=XR_XIR_VIEW_LIVE_INSTRUCTION) return XR_XIR_OK;
    if (!xir_effects_prepared_execution(effects) || event->function>=roots->templates)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=module->functions+event->function;
    if (event->instruction>=body->instruction_count ||
        event->before.words<((uint64_t)event->before.count+63u)/64u ||
        (event->before.count && (!event->before.values || !event->before.bits))) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=body->instructions+event->instruction;
    if (op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) return XR_XIR_OK;
    if (op->immediate<0 || (uint64_t)op->immediate>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    EffectCheckFunction row=roots->function[event->function];
    if (row.parameters!=body->parameter_count || row.values!=(uint64_t)body->parameter_count+body->instruction_count ||
        (uint64_t)op->immediate>=row.values) return XR_XIR_BAD_STRUCTURE;
    EffectCheckBuild binding={.memory=memory,.module=module,.roots=roots};uint32_t callback=0;
    if (!effect_callback_loan_node_formal(&binding,row.first+(uint32_t)op->immediate,event->function,&callback))
        return XR_XIR_OK;
    for (uint32_t p=0;p<event->before.count;++p) {
        if (!(event->before.bits[p/64u]&(UINT64_C(1)<<(p%64u)))) continue;
        uint32_t loan=event->before.values[p];
        if (loan>=(uint64_t)body->parameter_count+body->instruction_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *type=xr_xir_type_node(module->types,xr_xir_operand_type(body,loan));
        if (!type || type->kind!=XR_XIR_TYPE_SLICE ||
            (type->element!=XR_XIR_I64 && type->element!=XR_XIR_U8)) continue;
        status=effect_callback_loan_record(memory,roots,event->function,
            (XirModuleCallbackLoan){callback,XIR_MODULE_EFFECT_UNKNOWN,loan,event->function,event->instruction},true);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_effects_callback_loan_observed(const XrXirCompileContext *memory,
    const XrXirModule *module,const XrXirEffects *effects) {
    if (module && (!module->declarations || !module->declarations->functions)) return XR_XIR_OK;
    EffectCheckedRoots *roots=NULL;
    XrXirStatus status=effect_callback_loan_context(memory,module,effects,&roots);
    if (status==XR_XIR_OK && !effect_final_outcomes(effects)) roots->callback_loans_observed=true;
    return status;
}
XR_FUNC XrXirStatus xir_effects_callback_loan_contract(const XrXirCompileContext *memory,
    const XrXirModule *module,const XrXirEffects *effects,uint32_t function,uint32_t callback,
    XirModuleEffectTermKind kind,uint32_t loan,bool *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output=false;EffectCheckedRoots *roots=NULL;
    XrXirStatus status=effect_callback_loan_context(memory,module,effects,&roots);
    if (status!=XR_XIR_OK) return status;
    if (function>=roots->templates) return XR_XIR_BAD_STRUCTURE;
    if (!roots->callback_loans_observed || !roots->callback_loans_closed) return XR_XIR_OK;
    uint32_t visited=0;
    for (uint32_t link=roots->function[function].callback_loans;link;) {
        if (link>roots->callback_loans || visited++>=roots->callback_loans) return XR_XIR_BAD_STRUCTURE;
        EffectCheckCallbackLoan row=roots->callback_loan[link-1];
        if (row.function!=function) return XR_XIR_BAD_STRUCTURE;
        if (row.relation.callback_parameter==callback && row.relation.loan_kind==kind &&
            row.relation.loan_parameter==loan) {*output=true;return XR_XIR_OK;}
        link=row.next;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_effects_callback_loan_receiver(const XrXirCompileContext *memory,
    const XrXirModule *module,const XrXirEffects *effects,uint32_t function,uint32_t value,
    uint32_t *formal,bool *output) {
    if (!formal || !output) return XR_XIR_BAD_STRUCTURE;
    *output=false;EffectCheckedRoots *roots=NULL;
    XrXirStatus status=effect_callback_loan_context(memory,module,effects,&roots);
    if (status!=XR_XIR_OK) return status;
    if (function>=roots->templates || value>=roots->function[function].values) return XR_XIR_BAD_STRUCTURE;
    EffectCheckBuild binding={.memory=memory,.module=module,.roots=roots};uint32_t parameter=0;
    if (effect_callback_loan_node_formal(&binding,roots->function[function].first+value,function,&parameter)) {
        *formal=parameter;*output=true;
    }
    return XR_XIR_OK;
}
static bool effect_callback_loan_physical(const EffectCheckedRoots *roots,uint32_t function,
    uint32_t node,uint32_t *value) {
    if (node>=roots->nodes || roots->node[node].function!=function) return false;
    EffectCheckFunction row=roots->function[function];
    if (node<row.first || node-row.first>=row.values) return false;
    *value=node-row.first;return true;
}
XR_FUNC XrXirStatus xir_effects_callback_loan_applications(const XrXirCompileContext *memory,
    const XrXirModule *module,const XrXirEffects *effects,uint32_t function,uint32_t instruction,
    XirCallbackLoanApplicationVisitor visitor,void *context) {
    EffectCheckedRoots *roots=NULL;
    XrXirStatus status=effect_callback_loan_context(memory,module,effects,&roots);
    if (status!=XR_XIR_OK) return status;
    if (!visitor || function>=roots->templates || instruction>=module->functions[function].instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!roots->callback_loans_observed || !roots->callback_loans_closed) return XR_XIR_UNRESOLVED;
    EffectCheckFunction caller=roots->function[function];uint32_t visited_calls=0;
    for (uint32_t c=roots->node[caller.root].calls;c!=UINT32_MAX;) {
        if (c>=roots->calls || visited_calls++>=roots->calls) return XR_XIR_BAD_STRUCTURE;
        EffectCheckCall call=roots->call[c];c=call.next;
        if (call.instruction!=instruction || call.protocol!=2) continue;
        if (call.target>=roots->nodes) return XR_XIR_BAD_STRUCTURE;
        EffectCheckFunction callee=roots->function[roots->node[call.target].function];
        if (call.target!=callee.root) continue;
        if (callee.declaration>=roots->templates || call.first>roots->arguments ||
            call.count>roots->arguments-call.first) return XR_XIR_BAD_STRUCTURE;
        uint32_t visited=0;
        for (uint32_t link=roots->function[callee.declaration].callback_loans;link;) {
            if (link>roots->callback_loans || visited++>=roots->callback_loans) return XR_XIR_BAD_STRUCTURE;
            EffectCheckCallbackLoan pair=roots->callback_loan[link-1];link=pair.next;
            if (pair.source) continue;
            if (pair.function!=callee.declaration) return XR_XIR_BAD_STRUCTURE;
            if (pair.relation.callback_parameter>=call.count || pair.relation.loan_parameter>=call.count)
                return XR_XIR_UNRESOLVED;
            XirCallbackLoanApplication actual={.function=function,.instruction=instruction};
            if (!effect_callback_loan_physical(roots,function,
                    roots->argument[call.first+pair.relation.callback_parameter],&actual.callback) ||
                !effect_callback_loan_physical(roots,function,
                    roots->argument[call.first+pair.relation.loan_parameter],&actual.loan)) return XR_XIR_UNRESOLVED;
            status=visitor(context,&actual);if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
