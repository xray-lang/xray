/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_derivation_verify.c - Definition policy and exact generated authority
 *
 * KEY CONCEPT:
 *   A policy is a declaration fact. Every current binding and operation must
 *   correspond to that fact and to the ordinary implementation it names.
 */
#include "xxir_derivation_internal.h"
#include "xxir_compile_memory.h"
#include "xxir_constraint_proof_internal.h"
#include "xxir_effect_contract_internal.h"
#include "xxir_implementation.h"
#include "xxir_nominal.h"
#include "xxir_generic.h"
#include "xxir_types.h"

typedef struct DerivationScope {
    const XrXirModule *source;
    const XrXirDerivationTable *table;
    const XrXirProvenance *instance;
} DerivationScope;

static XrXirStatus derivation_scope(const XrXirModule *module,DerivationScope *scope) {
    scope->source=module;scope->instance=NULL;
    if (module->provenance && module->provenance->kind==XR_XIR_EVIDENCE_INSTANCE) {
        const XrXirProvenance *p=module->provenance;
        const XrXirModule *source=xr_xir_compile_artifact_module(p->source);
        if (!source || source==module || source->stage!=XR_XIR_CHECKED ||
            (source->provenance && source->provenance->kind==XR_XIR_EVIDENCE_INSTANCE) ||
            p->count!=module->function_count || !p->origins) return XR_XIR_BAD_STRUCTURE;
        scope->source=source;scope->instance=p;
    }
    scope->table=scope->source->declarations?scope->source->declarations->derivations:NULL;
    return XR_XIR_OK;
}
static XrXirStatus derivation_policy_for(const XrXirModule *module,uint32_t function,
    const XrXirDerivationGrant **binding,const XrXirDerivationPolicy **policy,
    DerivationScope *scope) {
    if (!module || !module->declarations || function>=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=derivation_scope(module,scope);
    if (status!=XR_XIR_OK) return status;
    uint32_t index=module->declarations->functions[function].derivation;
    const XrXirDerivationTable *current=module->declarations->derivations;
    if (!index || !current || index>current->count || !current->records)
        return XR_XIR_BAD_STRUCTURE;
    *binding=&current->records[index-1];
    if ((*binding)->function!=function || !scope->table || !scope->table->policies ||
        (*binding)->policy>=scope->table->policy_count) return XR_XIR_BAD_STRUCTURE;
    *policy=&scope->table->policies[(*binding)->policy];
    return XR_XIR_OK;
}
static XrXirStatus derivation_module_name(const XrXirCompileContext *context,
    const XrXirDeclarations *d,uint32_t module,XrXirLiteral name) {
    if (module>=d->module_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,(uint64_t)name.length+1)) return XR_XIR_BUDGET;
    const XrXirSourceModule *m=&d->modules[module];
    return m->name_length==name.length && !memcmp(m->name,name.bytes,name.length)?
        XR_XIR_OK:XR_XIR_BAD_TYPE;
}
static XrXirStatus derivation_protocol_visible(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirDerivationPolicy *p) {
    const XrXirDeclarations *d=module->declarations;
    const XrXirInterfaceDeclaration *protocol=&module->types->interfaces->declarations[p->protocol.declaration];
    uint32_t protocol_module=UINT32_MAX;
    for (uint32_t m=0; m<d->module_count; ++m) {
        XrXirStatus status=derivation_module_name(context,d,m,protocol->module);
        if (status==XR_XIR_OK) {protocol_module=m;break;}
        if (status!=XR_XIR_BAD_TYPE) return status;
    }
    if (protocol_module==UINT32_MAX || (!protocol->exported && protocol_module!=p->grantor_module) ||
        !xr_xir_module_imports(d,p->grantor_module,protocol_module)) return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
static XrXirStatus derivation_generator_visible(const XrXirModule *module,
    const XrXirDerivationPolicy *p) {
    if (p->generator>=module->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirDeclarations *d=module->declarations;
    const XrXirFunctionIdentity *generator=&d->functions[p->generator];
    if (generator->derivation || generator->cleanup_owner || generator->nominal_owner ||
        generator->module>=d->module_count ||
        !xr_xir_module_imports(d,p->grantor_module,generator->module) ||
        (generator->module!=p->grantor_module && !generator->exported)) return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
static XrXirStatus derivation_constructor_identity(const XrXirModule *module,
    const XrXirDerivationPolicy *p,uint32_t nominal) {
    if (!p->constructor) return XR_XIR_OK;
    uint32_t fn=p->constructor-1;
    if (fn>=module->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionIdentity *ctor=&module->declarations->functions[fn];
    if (ctor->module!=p->grantor_module || ctor->nominal_owner!=nominal+1 ||
        ctor->method_kind!=XR_XIR_CONSTRUCTOR || ctor->derivation || ctor->cleanup_owner)
        return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
static XrXirStatus derivation_policy_members(const XrXirCompileContext *context,
    const XrXirNominalDeclaration *target,const XrXirDerivationPolicy *p) {
    for (uint32_t i=0; i<p->member_count; ++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirDerivationMember *member=&p->members[i];
        if (member->field>=target->field_count) return XR_XIR_BAD_STRUCTURE;
        if ((member->operations&XR_XIR_DERIVE_WRITE) &&
            !(target->fields[member->field].flags&XR_XIR_FIELD_MUTABLE)) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static XrXirStatus derivation_policy_verify(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirDerivationPolicy *p) {
    const XrXirDeclarations *d=module->declarations;
    const XrXirTypeNode *node=xr_xir_type_node(module->types,p->target);
    if (!node || node->kind!=XR_XIR_TYPE_NOMINAL || !module->types->nominals ||
        !module->types->nominals->declarations ||
        node->nominal.declaration>=module->types->nominals->count) return XR_XIR_BAD_TYPE;
    uint32_t nominal=node->nominal.declaration;
    const XrXirNominalDeclaration *target=&module->types->nominals->declarations[nominal];
    XrXirStatus status=derivation_module_name(context,d,p->grantor_module,target->module);
    if (status!=XR_XIR_OK) return status;
    if (node->nominal.argument_count!=target->parameter_count) return XR_XIR_BAD_TYPE;
    for (uint32_t a=0; a<node->nominal.argument_count; ++a) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (node->nominal.arguments[a]!=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+a))
            return XR_XIR_BAD_TYPE;
    }
    XrXirProofContext proof={module,{XR_XIR_CONTEXT_NOMINAL,nominal,0}};
    status=xr_xir_compile_type_use_verify(context,&proof,p->target);
    XrXirConstraint constraint={0,&p->protocol,1};
    if (status==XR_XIR_OK) status=xr_xir_compile_constraint_structure(context,
        module->types,constraint,target->parameter_count);
    XirConstraintArguments use={module,{XR_XIR_CONTEXT_INTERFACE,p->protocol.declaration,0},
        p->protocol.arguments,p->protocol.argument_count};
    if (status==XR_XIR_OK) status=xr_xir_compile_constraint_arguments_prove(context,&proof,&use);
    if (status!=XR_XIR_OK) return status;
    status=derivation_protocol_visible(context,module,p);
    if (status==XR_XIR_OK) status=derivation_generator_visible(module,p);
    if (status==XR_XIR_OK) status=derivation_constructor_identity(module,p,nominal);
    if (status==XR_XIR_OK) status=derivation_policy_members(context,target,p);
    return status;
}
XR_FUNC XrXirStatus xir_derivation_policy_verify(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirDerivationPolicy *policy) {
    if (!xir_compile_context_valid(context) || !module || !module->declarations || !module->types || !policy)
        return XR_XIR_BAD_STRUCTURE;
    if (!module->declarations->modules || !module->declarations->module_count ||
        !module->declarations->functions || !module->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirDerivationTable single={policy,1,NULL,0};
    XrXirStatus status=xir_derivations_structure_verify(context,&single);
    return status==XR_XIR_OK?derivation_policy_verify(context,module,policy):status;
}
static XrXirStatus derivation_origin_equal(const XrXirCompileContext *context,
    const XrXirGeneratedOrigin *a,const XrXirGeneratedOrigin *b) {
    const XrXirGeneratedText left[]={a->module,a->file,a->field_path};
    const XrXirGeneratedText right[]={b->module,b->file,b->field_path};
    if (a->line!=b->line || a->column!=b->column) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=0; i<3; ++i) {
        if (!xir_compile_work(context,(uint64_t)left[i].length+1)) return XR_XIR_BUDGET;
        if (left[i].length!=right[i].length || (left[i].length &&
            memcmp(left[i].bytes,right[i].bytes,left[i].length))) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus derivation_instance_binding(const XrXirCompileContext *context,
    const XrXirModule *module,const DerivationScope *scope,const XrXirDerivationGrant *row) {
    const XrXirProvenance *p=scope->instance;
    const XrXirDerivationTable *source=scope->table;
    const XrXirOrigin *origin=&p->origins[row->function];
    if (!source || row->source_row>=source->count || !source->records ||
        origin->function>=scope->source->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirDerivationGrant *original=&source->records[row->source_row];
    if (original->function!=origin->function || row->policy!=original->policy ||
        row->root!=original->root || row->origins.instruction_count!=original->origins.instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xr_xir_compile_type_substitution_matches_between(context,
        scope->source->types,module->types,origin->arguments,origin->argument_count,
        original->target,row->target);
    if (status==XR_XIR_OK) status=derivation_origin_equal(context,&row->origins.entry,&original->origins.entry);
    for (uint32_t n=0; status==XR_XIR_OK && n<row->origins.instruction_count; ++n)
        status=derivation_origin_equal(context,&row->origins.instructions[n],&original->origins.instructions[n]);
    return status;
}
static XrXirStatus derivation_source_binding(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirDerivationTable *table,uint32_t index) {
    const XrXirDerivationGrant *row=&table->records[index];
    if (row->source_row!=index || row->policy>=table->policy_count || row->root>=table->count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirDerivationPolicy *policy=&table->policies[row->policy];
    const XrXirDerivationGrant *root=&table->records[row->root];
    if (root->root!=row->root || root->policy!=row->policy || root->target!=row->target ||
        row->target!=policy->target) return XR_XIR_BAD_TYPE;
    const XrXirFunctionIdentity *identity=&module->declarations->functions[row->function];
    const XrXirTypeNode *target=xr_xir_type_node(module->types,row->target);
    if (!target || target->kind!=XR_XIR_TYPE_NOMINAL || identity->module!=policy->grantor_module ||
        (identity->nominal_owner && identity->nominal_owner!=target->nominal.declaration+1))
        return XR_XIR_BAD_TYPE;
    if (identity->test_role || identity->method_kind==XR_XIR_CONSTRUCTOR ||
        module->declarations->modules[identity->module].initializer==row->function)
        return XR_XIR_BAD_TYPE;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    return XR_XIR_OK;
}
static XrXirStatus derivation_implementations(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirDerivationTable *table) {
    const XrXirImplementationTable *implementations=module->declarations->implementations;
    for (uint32_t i=0; implementations && i<implementations->count; ++i) {
        const XrXirImplementation *impl=&implementations->records[i];
        const XrXirDerivationPolicy *policy=NULL;
        if (impl->derivation_policy) {
            if (!table || impl->derivation_policy>table->policy_count) return XR_XIR_BAD_STRUCTURE;
            policy=&table->policies[impl->derivation_policy-1];
            const XrXirTypeNode *target=xr_xir_type_node(module->types,policy->target);
            if (impl->nominal_declaration!=target->nominal.declaration ||
                impl->interface.declaration!=policy->protocol.declaration ||
                impl->interface.argument_count!=policy->protocol.argument_count) return XR_XIR_BAD_TYPE;
            for (uint32_t a=0; a<impl->interface.argument_count; ++a) {
                if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
                if (impl->interface.arguments[a]!=policy->protocol.arguments[a]) return XR_XIR_BAD_TYPE;
            }
        }
        for (uint32_t b=0; b<impl->binding_count; ++b) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            uint32_t fn=impl->bindings[b].function;
            uint32_t grant=module->declarations->functions[fn].derivation;
            if (!policy) {if (grant) return XR_XIR_BAD_TYPE;continue;}
            if (!grant || grant>table->count) return XR_XIR_BAD_TYPE;
            const XrXirDerivationGrant *row=&table->records[grant-1];
            if (row->policy!=impl->derivation_policy-1 || row->root!=grant-1)
                return XR_XIR_BAD_TYPE;
        }
    }
    for (uint32_t i=0; table && i<table->count; ++i) {
        const XrXirDerivationGrant *row=&table->records[i];
        if (row->root!=i) continue;
        bool found=false;
        for (uint32_t j=0; implementations && j<implementations->count; ++j) {
            const XrXirImplementation *impl=&implementations->records[j];
            if (impl->derivation_policy!=row->policy+1) continue;
            for (uint32_t b=0; b<impl->binding_count; ++b) {
                if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
                if (impl->bindings[b].function==row->function) found=true;
            }
        }
        if (!found) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_derivations_verify(const XrXirCompileContext *context,
    const XrXirModule *module) {
    if (!xir_compile_context_valid(context) || !module) return XR_XIR_BAD_STRUCTURE;
    DerivationScope scope={0};XrXirStatus status=derivation_scope(module,&scope);
    if (status!=XR_XIR_OK) return status;
    const XrXirDeclarations *d=module->declarations;
    const XrXirDerivationTable *table=d?d->derivations:NULL;
    status=xir_derivations_structure_verify(context,table);
    if (status!=XR_XIR_OK) return status;
    /* Ordinary declaration admission already rejects every nonzero binding
     * without a table. Empty grants need no second per-function traversal. */
    if (!table && !scope.table) return XR_XIR_OK;
    if (scope.instance && table && table->policy_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=0; table && i<table->policy_count && status==XR_XIR_OK; ++i)
        status=derivation_policy_verify(context,module,&table->policies[i]);
    for (uint32_t i=0; table && i<table->count && status==XR_XIR_OK; ++i) {
        const XrXirDerivationGrant *row=&table->records[i];
        if (row->function>=module->function_count ||
            row->origins.instruction_count!=module->functions[row->function].instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        status=scope.instance?derivation_instance_binding(context,module,&scope,row):
            derivation_source_binding(context,module,table,i);
    }
    for (uint32_t f=0; d && f<module->function_count && status==XR_XIR_OK; ++f) {
        const XrXirFunctionIdentity *identity=&d->functions[f];
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (identity->cleanup_owner) {
            const XrXirFunctionIdentity *owner=&d->functions[identity->cleanup_owner-1];
            if (!!identity->derivation!=!!owner->derivation) return XR_XIR_BAD_TYPE;
            if (identity->derivation) {
                const XrXirDerivationGrant *a=&table->records[identity->derivation-1];
                const XrXirDerivationGrant *b=&table->records[owner->derivation-1];
                if (a->root!=b->root || a->policy!=b->policy || a->target!=b->target)
                    return XR_XIR_BAD_TYPE;
            }
        }
        if (scope.instance) {
            uint32_t original=scope.instance->origins[f].function;
            if (original>=scope.source->function_count) return XR_XIR_BAD_STRUCTURE;
            uint32_t expected=scope.source->declarations?scope.source->declarations->functions[original].derivation:0;
            if (!!identity->derivation!=!!expected) return XR_XIR_BAD_TYPE;
            if (expected && table->records[identity->derivation-1].source_row!=expected-1)
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    if (status==XR_XIR_OK && d && !scope.instance)
        status=derivation_implementations(context,module,table);
    return status;
}
XR_FUNC XrXirStatus xir_derivation_access(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirDerivationAccess *request,bool *authorized) {
    if (!xir_compile_context_valid(context) || !module || !request || !authorized ||
        request->function>=module->function_count) return XR_XIR_BAD_STRUCTURE;
    *authorized=false;
    if (!module->declarations || !module->declarations->functions[request->function].derivation)
        return XR_XIR_OK;
    const XrXirDerivationGrant *row=NULL;const XrXirDerivationPolicy *policy=NULL;
    DerivationScope scope={0};
    XrXirStatus status=derivation_policy_for(module,request->function,&row,&policy,&scope);
    if (status!=XR_XIR_OK) return status;
    const XrXirTypeNode *target=xr_xir_type_node(module->types,request->target);
    const XrXirTypeNode *expected=xr_xir_type_node(module->types,row->target);
    if (!target || !expected || target->kind!=XR_XIR_TYPE_NOMINAL ||
        expected->kind!=XR_XIR_TYPE_NOMINAL) return XR_XIR_BAD_TYPE;
    if (target->nominal.declaration!=expected->nominal.declaration) return XR_XIR_OK;
    if (request->target!=row->target || !request->operation ||
        (request->operation&~XR_XIR_DERIVE_MEMBER_MASK)) return XR_XIR_BAD_TYPE;
    for (uint32_t i=0; i<policy->member_count; ++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirDerivationMember *member=&policy->members[i];
        if (member->field==request->field && (member->operations&request->operation)==request->operation) {
            *authorized=true;return XR_XIR_OK;
        }
    }
    return XR_XIR_BAD_TYPE;
}
XR_FUNC XrXirStatus xir_derivation_call(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t caller,const XrXirInstruction *op,
    bool ordinary_binding_authorized,bool *constructor_authorized) {
    if (!xir_compile_context_valid(context) || !module || !op || !constructor_authorized || caller>=module->function_count ||
        op->immediate<0 || (uint64_t)op->immediate>=module->function_count) return XR_XIR_BAD_STRUCTURE;
    *constructor_authorized=false;
    const XrXirDeclarations *d=module->declarations;
    if (!d || !d->functions[caller].derivation) return XR_XIR_OK;
    const XrXirDerivationGrant *row=NULL;const XrXirDerivationPolicy *policy=NULL;
    DerivationScope scope={0};
    XrXirStatus status=derivation_policy_for(module,caller,&row,&policy,&scope);
    if (status!=XR_XIR_OK) return status;
    uint32_t callee=(uint32_t)op->immediate;
    const XrXirFunctionIdentity *identity=&d->functions[callee];
    const XrXirTypeNode *target=xr_xir_type_node(module->types,row->target);
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    if (identity->derivation) {
        const XrXirDerivationGrant *other=&d->derivations->records[identity->derivation-1];
        if (other->root==row->root && other->policy==row->policy && other->target==row->target)
            return XR_XIR_OK;
    }
    bool constructor=identity->method_kind==XR_XIR_CONSTRUCTOR &&
        identity->nominal_owner==target->nominal.declaration+1;
    if (constructor) {
        uint32_t original=scope.instance?scope.instance->origins[callee].function:callee;
        if (!policy->constructor || policy->constructor!=original+1 ||
            (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE)) return XR_XIR_BAD_TYPE;
        status=xr_xir_compile_call_type_matches(context,module,caller,op,
            module->functions[callee].result,row->target);
        if (status==XR_XIR_OK) *constructor_authorized=true;
        return status;
    }
    /* Instance export bits describe a private code image. Naming permission
     * still comes from the independently checked original declaration. */
    const XrXirFunctionIdentity *declaration=identity;
    if (scope.instance) {
        uint32_t original=scope.instance->origins[callee].function;
        if (original>=scope.source->function_count || !scope.source->declarations)
            return XR_XIR_BAD_STRUCTURE;
        declaration=&scope.source->declarations->functions[original];
    }
    if (!ordinary_binding_authorized &&
        (declaration->member_access!=XR_XIR_MEMBER_PUBLIC || !declaration->exported))
        return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
