/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_defaults.c - Declaration-owned parameter default authority
 *
 * KEY CONCEPT:
 *   Only an authenticated owner and ordinal authorize a private default helper.
 */
#include "xxir_defaults_internal.h"
#include "xxir_compile_memory.h"
#include "xxir_declarations.h"
#include "xxir_constraints.h"
#include "xxir_types.h"
#include "xxir_nominal.h"
#include "xxir_generic.h"
#include "xxir_internal.h"
#include "xxir_implementation.h"
#include "../base/xmalloc.h"

static bool defaults_spend(XrXirCompileContext *b, uint64_t amount) {
    if (!xir_compile_work(b, amount)) return false;
     return true;
}
XR_FUNC XrXirStatus xr_xir_compile_default_lookup(const XrXirCompileContext *compile_context, const XrXirModule *m, uint32_t owner, uint32_t ordinal, const XrXirDefaultBinding **out) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *b = &compile_state;
    if (!out) return XR_XIR_BAD_STRUCTURE;
    if (!m || !b) return XR_XIR_BAD_STRUCTURE;
    if (!m->defaults) { *out = NULL; return XR_XIR_OK; }
    const XrXirDefaultTable *t=m->defaults;
    if (!t->count || !t->records) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=0;i<t->count;++i) {
        if (!defaults_spend(b,1)) return XR_XIR_BUDGET;
        const XrXirDefaultBinding *a=&t->records[i];
        if (a->owner==owner && a->ordinal==ordinal) { *out=a; return XR_XIR_OK; }
    }
    *out = NULL; return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_default_helper(const XrXirCompileContext *compile_context, const XrXirModule *m, uint32_t function, bool *out) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *b = &compile_state;
    if (!out || !m || !b || function>=m->function_count) return XR_XIR_BAD_STRUCTURE;
    if (xir_effect_evidence_is_instance(m)) {
        const XrXirProvenance *p=m->provenance;
        if (!p->source || !p->origins || function>=p->count) return XR_XIR_BAD_STRUCTURE;
        function=p->origins[function].function; m=&p->source->module;
    }
    if (!m->defaults) { *out = false; return XR_XIR_OK; }
    if (!m->defaults->count || !m->defaults->records) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=0;i<m->defaults->count;++i) {
        if (!defaults_spend(b,1)) return XR_XIR_BUDGET;
        if (m->defaults->records[i].function==function) { *out=true; return XR_XIR_OK; }
    }
    *out = false; return XR_XIR_OK;
}
static XrXirStatus default_signature(const XrXirModule *m,const XrXirDefaultBinding *a,XrXirCompileContext *b) {
    XrXirStatus allocation_status = XR_XIR_OK;
    const XrXirGeneric zero={0};
    const XrXirGeneric *f=m->generics?&m->generics[a->owner]:&zero;
    const XrXirGeneric *h=m->generics?&m->generics[a->function]:&zero;
    if (f->parameter_count!=h->parameter_count) return XR_XIR_BAD_TYPE;
    if (!!f->constraints!=!!f->parameter_count || !!h->constraints!=!!h->parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t n=f->parameter_count;
    for (uint32_t i=0;i<n;++i) {
        if (xr_xir_binder_kind(f,i)!=xr_xir_binder_kind(h,i)) return XR_XIR_BAD_TYPE;
        const XrXirConstraint *fc=&f->constraints[i],*hc=&h->constraints[i];
        if(fc->interface_count!=hc->interface_count) return XR_XIR_BAD_TYPE;
        if (!!fc->interfaces!=!!fc->interface_count || !!hc->interfaces!=!!hc->interface_count)
            return XR_XIR_BAD_STRUCTURE;
        for(uint32_t j=0;j<fc->interface_count;++j) {
            if(!defaults_spend(b,1)) return XR_XIR_BUDGET;
            if(fc->interfaces[j].declaration!=hc->interfaces[j].declaration) return XR_XIR_BAD_TYPE;
        }
        XrXirStatus s=xr_xir_compile_constraint_records_match(b, m->types, *fc, m->types, *hc, n);
        if (s!=XR_XIR_OK) return s;
    }
    uint64_t bytes=(uint64_t)n*sizeof(XrXirType);
    if (bytes>SIZE_MAX ||(bytes > SIZE_MAX) || !defaults_spend(b,n)) return XR_XIR_BUDGET;

    XrXirType *args=n?xir_compile_alloc(b, (size_t)bytes, &allocation_status):NULL;
    if (n && !args) {  return allocation_status; }
    for (uint32_t i=0;i<n;++i) args[i]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+i);
    XrXirStatus s=xr_xir_compile_type_substitution_matches(b, m->types, args, n, m->functions[a->owner].parameters[a->ordinal], m->functions[a->function].result);
    xr_compile_resources_free(args);  return s;
}
XR_FUNC XrXirStatus xr_xir_compile_defaults_verify(const XrXirCompileContext *compile_context, const XrXirModule *m) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *b = &compile_state;
    if (!m || !b) return XR_XIR_BAD_STRUCTURE;
    if (!m->defaults) return XR_XIR_OK;
    if (m->stage==XR_XIR_LOWERED || xir_effect_evidence_is_instance(m)) return XR_XIR_BAD_STAGE;
    const XrXirDefaultTable *t=m->defaults;
    const XrXirDeclarations *d=m->declarations;
    if (!t->count || !t->records || !d || !d->functions || !m->functions || !d->modules || !d->module_count) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes=sizeof(*t)+(uint64_t)t->count*sizeof(*t->records);
    if ((bytes > SIZE_MAX) || !defaults_spend(b,t->count)) return XR_XIR_BUDGET;

    for (uint32_t i=0;i<t->count;++i) {
        const XrXirDefaultBinding *a=&t->records[i];
        if (a->owner_kind!=XR_XIR_DEFAULT_PARAMETER || a->owner>=m->function_count ||
            a->function>=m->function_count || a->owner==a->function) return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *f=&m->functions[a->owner],*h=&m->functions[a->function];
        const XrXirFunctionIdentity *fi=&d->functions[a->owner],*hi=&d->functions[a->function];
        if (a->ordinal>=f->parameter_count || !f->parameters || h->parameter_count || h->parameters ||
            fi->cleanup_owner || fi->method_kind==XR_XIR_MEMBER_HELPER ||
            (fi->method_kind==XR_XIR_READ_METHOD && !a->ordinal) ||
            hi->module!=fi->module || hi->nominal_owner!=fi->nominal_owner ||
            hi->member_access!=fi->member_access || hi->exported || hi->cleanup_owner || hi->promises ||
            hi->method_kind!=(uint32_t)(hi->nominal_owner?XR_XIR_MEMBER_HELPER:XR_XIR_NON_MEMBER) ||
            (m->linkage_kind==XR_XIR_PROGRAM && (a->function==d->entry_function || a->owner==d->entry_function))) return XR_XIR_BAD_STRUCTURE;
        if (i && (t->records[i-1].owner>a->owner ||
            (t->records[i-1].owner==a->owner && t->records[i-1].ordinal+1!=a->ordinal))) return XR_XIR_BAD_STRUCTURE;
        if ((i+1==t->count || t->records[i+1].owner!=a->owner) && a->ordinal+1!=f->parameter_count)
            return XR_XIR_BAD_STRUCTURE;
        for (uint32_t j=0;j<t->count;++j) {
            if (!defaults_spend(b,1)) return XR_XIR_BUDGET;
            if ((i!=j && t->records[j].function==a->function) || t->records[j].owner==a->function)
                return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t j=0;j<d->module_count;++j) {
            if (!defaults_spend(b,1)) return XR_XIR_BUDGET;
            if (d->modules[j].initializer==a->function || d->modules[j].initializer==a->owner)
                return XR_XIR_BAD_STRUCTURE;
        }
        const XrXirImplementationTable *it=d->implementations;
        for (uint32_t j=0;it && j<it->count;++j) {
            if (!defaults_spend(b,1)) return XR_XIR_BUDGET;
            for (uint32_t k=0;k<it->records[j].binding_count;++k) {
                if (!defaults_spend(b,1)) return XR_XIR_BUDGET;
                if (it->records[j].bindings[k].function==a->function) return XR_XIR_BAD_STRUCTURE;
            }
        }
        XrXirStatus s=default_signature(m,a,b); if(s!=XR_XIR_OK) return s;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_default_call_verify(const XrXirCompileContext *compile_context, const XrXirModule *m, uint32_t caller, const XrXirInstruction *op) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *b = &compile_state;
    if (!m->declarations || caller>=m->function_count || op->immediate ||
        (op->op != XR_XIR_CALL_DEFAULT && op->op != XR_XIR_INVOKE_DEFAULT) ||
        (op->op == XR_XIR_CALL_DEFAULT && (op->args[0] || op->args[1])))
        return XR_XIR_BAD_STRUCTURE;
    const XrXirDefaultBinding *a=NULL;
    const uint32_t *identity=xr_xir_default_identity(op);
    XrXirStatus s=xr_xir_compile_default_lookup(b, m, identity[0], identity[1], &a);
    if (s!=XR_XIR_OK) return s;
    if (!a || a->owner>=m->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirDeclarations *d=m->declarations;
    const XrXirFunctionIdentity *f=&d->functions[a->owner],*c=&d->functions[caller];
    if(!defaults_spend(b,d->modules[c->module].dependency_count)) return XR_XIR_BUDGET;
    if ((f->member_access && c->nominal_owner!=f->nominal_owner) ||
        !xr_xir_module_imports(d,c->module,f->module) || (c->module!=f->module && !f->exported))
        return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction call=*op; call.immediate=a->owner;
    s=xr_xir_compile_generic_call(b, m, caller, &call);
    if (s==XR_XIR_OK) s=xr_xir_compile_call_type_matches(b, m, caller, &call, m->functions[a->owner].parameters[a->ordinal], op->type);
    return s;
}

#include "xxir_construction_internal.h"
#include "xxir_nominal_initializers.inc.c"
