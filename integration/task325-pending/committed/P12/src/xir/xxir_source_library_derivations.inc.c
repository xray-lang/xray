/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_library_derivations.inc.c - Owned canonical derivation imports
 *
 * KEY CONCEPT:
 *   Imported facts retain their exact policy and origin. Every ordinal is
 *   remapped to this receiving Source before ordinary complete Built checking.
 */
static bool source_library_derivation_origin(SourceContext *ctx,const XrXirGeneratedOrigin *input,
    XrXirGeneratedOrigin *output) {
    if (!source_work_units(ctx,NULL,sizeof(*output))) return false;
    *output=*input;
    XrXirGeneratedText *texts[]={&output->module,&output->file,&output->field_path};
    for (uint32_t i=0;i<3;++i) {
        const char *bytes=texts[i]->bytes;
        texts[i]->bytes=source_library_bytes(ctx,bytes,texts[i]->length,1);
        if (texts[i]->length && !texts[i]->bytes) return false;
    }
    return true;
}
static bool source_library_derivation_function(SourceContext *ctx,const SourceLibraryMap *map,
    uint32_t input,uint32_t *output) {
    if (!source_work(ctx,NULL)) return false;
    if (input>=map->function_count || map->functions[input]==UINT32_MAX)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library derivation function is outside selected closure");
    *output=map->functions[input];return true;
}
static bool source_library_derivation_policy(SourceContext *ctx,const SourceLibraryMap *map,
    const XrXirDerivationPolicy *input,XrXirDerivationPolicy *output) {
    if (input->grantor_module>=map->module_count || map->modules[input->grantor_module]==UINT32_MAX)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library derivation module is not mapped");
    if (!source_work_units(ctx,NULL,sizeof(*output))) return false;
    *output=*input;output->grantor_module=map->modules[input->grantor_module];
    const XrXirInterfaceApplication *application=NULL;
    if (!source_library_type(ctx,map->unit,input->target,&output->target) ||
        !source_library_applications(ctx,map->unit,&input->protocol,1,&application) ||
        !source_library_derivation_function(ctx,map,input->generator,&output->generator)) return false;
    output->protocol=*application;
    if (input->constructor) {
        uint32_t constructor=0;
        if (!source_library_derivation_function(ctx,map,input->constructor-1,&constructor)) return false;
        output->constructor=constructor+1;
    }
    output->members=source_library_bytes(ctx,input->members,input->member_count,sizeof(*input->members));
    if (input->member_count && !output->members) return false;
    return source_library_derivation_origin(ctx,&input->origin,&output->origin);
}
static bool source_library_derivation_grant(SourceContext *ctx,const SourceLibraryMap *map,
    const XrXirDerivationGrant *input,const uint32_t *rows,XrXirDerivationGrant *output) {
    const XrXirDerivationTable *table=map->unit->library->declarations->derivations;
    if (input->policy>=table->policy_count || map->unit->derivation_policies[input->policy]==UINT32_MAX ||
        input->source_row>=table->count || rows[input->source_row]==UINT32_MAX ||
        input->root>=table->count || rows[input->root]==UINT32_MAX)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library derivation row is outside selected closure");
    if (!source_work_units(ctx,NULL,sizeof(*output))) return false;
    *output=*input;output->policy=map->unit->derivation_policies[input->policy];
    output->source_row=rows[input->source_row];output->root=rows[input->root];
    if (!source_library_derivation_function(ctx,map,input->function,&output->function) ||
        !source_library_type(ctx,map->unit,input->target,&output->target) ||
        !source_library_derivation_origin(ctx,&input->origins.entry,&output->origins.entry)) return false;
    uint32_t count=input->origins.instruction_count;
    XrXirGeneratedOrigin *origins=count?source_alloc(ctx,count,sizeof(*origins)):NULL;
    if (count && !origins) return false;
    output->origins.instructions=origins;
    for (uint32_t i=0;i<count;++i)
        if (!source_library_derivation_origin(ctx,&input->origins.instructions[i],&origins[i])) return false;
    return true;
}
static bool source_library_derivations(SourceContext *ctx,const XrXirModule *library,const SourceLibraryMap *map) {
    const XrXirDerivationTable *input=library->declarations->derivations;
    if (!input) return true;
    if (!map->unit || map->unit->derivation_policies)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library derivation unit is missing or duplicated");
    uint32_t policy_base=ctx->derivations.policy_count,policy_count=policy_base;
    uint32_t *policies=input->policy_count?source_alloc(ctx,input->policy_count,sizeof(*policies)):NULL;
    if (input->policy_count && !policies) return false;
    map->unit->derivation_policies=policies;
    for (uint32_t i=0;i<input->policy_count;++i) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t module=input->policies[i].grantor_module;policies[i]=UINT32_MAX;
        if (module>=map->module_count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library policy module is invalid");
        if (map->modules[module]==UINT32_MAX) continue;
        if (policy_count==UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"library policy count exhausted");
        policies[i]=policy_count++;
    }
    if (policy_count>policy_base) {
        XrXirDerivationPolicy *copied=source_alloc(ctx,policy_count,sizeof(*copied));
        if (!copied || !source_copy_bytes(ctx,NULL,copied,ctx->derivations.policies,(size_t)policy_base*sizeof(*copied))) return false;
        for (uint32_t i=0;i<input->policy_count;++i) {
            if (!source_work(ctx,NULL)) return false;
            if (policies[i]!=UINT32_MAX && !source_library_derivation_policy(ctx,map,&input->policies[i],&copied[policies[i]])) return false;
        }
        ctx->derivations.policies=copied;ctx->derivations.policy_count=policy_count;
    }
    uint32_t base=ctx->derivations.count,count=base;
    uint32_t *rows=input->count?source_alloc(ctx,input->count,sizeof(*rows)):NULL;
    if (input->count && !rows) return false;
    for (uint32_t i=0;i<input->count;++i) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t function=input->records[i].function;rows[i]=UINT32_MAX;
        if (function>=map->function_count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library grant function is invalid");
        if (map->functions[function]==UINT32_MAX) continue;
        if (count==UINT32_MAX) return source_fail(ctx,NULL,XR_XIR_BUDGET,"library grant count exhausted");
        rows[i]=count++;
    }
    if (count==base) return true;
    XrXirDerivationGrant *records=source_alloc(ctx,count,sizeof(*records));
    if (!records || !source_copy_bytes(ctx,NULL,records,ctx->derivations.records,(size_t)base*sizeof(*records))) return false;
    for (uint32_t i=0;i<input->count;++i) {
        if (!source_work(ctx,NULL)) return false;
        if (rows[i]==UINT32_MAX) continue;
        uint32_t row=rows[i];XrXirDerivationGrant *to=&records[row];
        if (!source_library_derivation_grant(ctx,map,&input->records[i],rows,to)) return false;
        if (row && records[row-1].function>=to->function)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library grant function order is not canonical");
        ctx->identities[to->function].derivation=row+1;
    }
    ctx->derivations.records=records;ctx->derivations.count=count;return true;
}
