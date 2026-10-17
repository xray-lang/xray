/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_comptime_verify.inc.c - Current call, substitution and literal boundaries
 *
 * KEY CONCEPT:
 *   Only current calls, closed substitutions and private literal providers correspond.
 */
#include "xxir_constraint_proof.h"
#include "xxir_type_match_internal.h"
/* Construction metadata is checked before ordinary instruction validation. */
static XrXirStatus comptime_body_storage(const XrXirCompileContext *context,const XrXirModule *module) {
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!xir_compile_work(context,4)) return XR_XIR_BUDGET;
        const XrXirFunction *function=&module->functions[f];
        if (!!function->instructions!=!!function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        if (function->instruction_count>context->limits.instructions) return XR_XIR_BUDGET;
    }
    return XR_XIR_OK;
}
static XrXirStatus comptime_provider_verify(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t helper,uint32_t provider) {
    if (provider>=module->function_count || provider==helper) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=&module->functions[provider];
    const XrXirFunctionIdentity *a=&module->declarations->functions[helper],*b=&module->declarations->functions[provider];
    if (a->module!=b->module || a->nominal_owner!=b->nominal_owner || a->method_kind!=b->method_kind ||
        b->exported || b->member_access || b->cleanup_owner || b->test_role || b->promises ||
        body->parameter_count || body->parameters || body->block_count!=1 || !body->blocks ||
        !body->instruction_count || !body->instructions || module->generics[provider].parameter_count ||
        module->generics[provider].argument_count || provider==module->declarations->entry_function)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t m=0;m<module->declarations->module_count;++m) {
        if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
        if (module->declarations->modules[m].initializer==provider) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=materialization_data_type(context,module->types,body->result);
    for (uint32_t i=0;status==XR_XIR_OK && i<body->instruction_count;++i) {
        if (!xir_compile_work(context,sizeof(XrXirInstruction))) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&body->instructions[i];
        if (!materialization_opcode(op->op)) return XR_XIR_BAD_TYPE;
        XrXirType type=op->type;
        if (xr_xir_type_is_cell(module->types,type)) {
            type=xr_xir_cell_element(module->types,type);
            if (!xr_xir_type_is_array(module->types,type)) return XR_XIR_BAD_TYPE;
        }
        status=materialization_data_type(context,module->types,type);
        if (op->op==XR_XIR_CALL && (op->immediate<0 || (uint64_t)op->immediate>=module->function_count || op->args[1] ||
            module->declarations->functions[op->immediate].method_kind!=XR_XIR_MATERIALIZER)) return XR_XIR_BAD_TYPE;
    }
    return status;
}
XR_FUNC XrXirStatus xir_comptime_verify(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *owner) {
    const XrXirComptimeTable *table=&owner->comptime;
    if (!table->site_count) return table->binding_count?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    if (module->stage==XR_XIR_LOWERED || !module->generics || !module->declarations ||
        !module->declarations->functions || xir_effect_evidence_is_closed(module)) return XR_XIR_BAD_STAGE;
    XrXirStatus storage=comptime_body_storage(context,module);
    if (storage!=XR_XIR_OK) return storage;
    for (uint32_t s=0;s<table->site_count;++s) {
        if (!xir_compile_work(context,sizeof(XrXirComptimeSite)+16)) return XR_XIR_BUDGET;
        const XrXirComptimeSite *site=&table->sites[s];
        if (site->caller>=module->function_count || site->helper>=module->function_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *caller=&module->functions[site->caller],*helper=&module->functions[site->helper];
        const XrXirFunctionIdentity *caller_id=&module->declarations->functions[site->caller],*helper_id=&module->declarations->functions[site->helper];
        if (site->instruction>=caller->instruction_count || !module->generics[site->helper].parameter_count || helper->parameter_count ||
            helper_id->exported || helper_id->cleanup_owner || helper_id->test_role || helper_id->member_access || helper_id->promises ||
            caller_id->module!=helper_id->module || caller_id->nominal_owner!=helper_id->nominal_owner ||
            helper_id->method_kind!=(uint32_t)(helper_id->nominal_owner?XR_XIR_MEMBER_HELPER:XR_XIR_NON_MEMBER)) return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *op=&caller->instructions[site->instruction];
        if ((op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE) || op->immediate!=site->helper || op->args[1]) return XR_XIR_BAD_STRUCTURE;
        uint32_t count=module->generics[site->helper].parameter_count;
        if (module->generics[site->caller].parameter_count!=count || op->type_arguments[1]!=count) return XR_XIR_BAD_TYPE;
        const XrXirGeneric *actual=&module->generics[site->caller];
        if (!actual->arguments || op->type_arguments[0]>actual->argument_count || count>actual->argument_count-op->type_arguments[0])
            return XR_XIR_BAD_STRUCTURE;
        /* A lexical thunk receives the caller's complete ordered binders. */
        for (uint32_t a=0;a<count;++a) {
            if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
            if (actual->arguments[op->type_arguments[0]+a]!=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+a))
                return XR_XIR_BAD_TYPE;
        }
    }
    for (uint32_t b=0;b<table->binding_count;++b) {
        if (!xir_compile_work(context,sizeof(XrXirComptimeBinding)+8)) return XR_XIR_BUDGET;
        const XrXirComptimeBinding *row=&table->bindings[b];
        const XrXirComptimeSite *site=&table->sites[row->site];
        const XrXirGeneric *generic=&module->generics[site->helper];
        if (row->argument_count!=generic->parameter_count) return XR_XIR_BAD_TYPE;
        XrXirStatus status=comptime_provider_verify(context,module,site->helper,row->provider);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t a=0;a<row->argument_count;++a) {
            if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
            if (!xr_xir_type_in_context(module,site->helper,row->arguments[a]) || xr_xir_type_span(module->types,row->arguments[a])) return XR_XIR_BAD_TYPE;
            XrXirProofContext proof={module,{XR_XIR_CONTEXT_CLOSED,0,0}};
            XrXirConstraintUse use={module,{XR_XIR_CONTEXT_FUNCTION,site->helper,0},a,row->arguments,row->argument_count};
            status=xr_xir_compile_constraints_prove(context,&proof,&use);
            if (status!=XR_XIR_OK) return status;
        }
        status=xr_xir_compile_type_substitution_matches_between(context,module->types,module->types,row->arguments,row->argument_count,
            module->functions[site->helper].result,module->functions[row->provider].result);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t previous=b;previous && table->bindings[previous-1].site==row->site;) {
            const XrXirComptimeBinding *other=&table->bindings[--previous];bool same=other->argument_count==row->argument_count;
            if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
            for (uint32_t a=0;same && a<row->argument_count;++a) {
                status=xr_xir_compile_type_substitution_matches_between(context,module->types,module->types,NULL,0,other->arguments[a],row->arguments[a]);
                if (status==XR_XIR_BAD_TYPE) same=false;else if (status!=XR_XIR_OK) return status;
            }
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    /* A helper's complete use inventory prevents unbound runtime references. */
    for (uint32_t f=0;f<module->function_count;++f) for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
        if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&module->functions[f].instructions[i];
        if (!xr_xir_op_references_function(op->op)) continue;
        for (uint32_t s=0;s<table->site_count;++s) {
            if (!xir_compile_work(context,4)) return XR_XIR_BUDGET;
            if (op->immediate==table->sites[s].helper && (f!=table->sites[s].caller || i!=table->sites[s].instruction))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
