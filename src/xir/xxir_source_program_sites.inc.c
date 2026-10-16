/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_program_sites.inc.c - Complete current symbolic occurrence inventory
 *
 * KEY CONCEPT:
 *   Uninstantiated sites survive alongside actually executed closed keys.
 *   Source coordinates identify original calls, never inferred permissions.
 */
static bool source_program_sites(SourceContext *ctx,const XrXirModule *module,XrXirComptimeTable *table) {
    uint32_t count=0;
    for (const SourceComptimeSite *s=ctx->comptime_sites;s;s=s->next) {
        if (!source_work_units(ctx,s->syntax,4)) return false;
        if (module->generics[s->helper].parameter_count) ++count;
    }
    XrXirComptimeSite *sites=count?source_alloc(ctx,count,sizeof(*sites)):NULL;
    if (count && !sites) return false;
    uint32_t next=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!source_work_units(ctx,NULL,sizeof(XrXirInstruction))) return false;
            const XrXirInstruction *op=&body->instructions[i];
            if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE) continue;
            for (const SourceComptimeSite *s=ctx->comptime_sites;s;s=s->next) {
                if (!source_work_units(ctx,s->syntax,4)) return false;
                if (s->owner!=f || op->immediate!=s->helper || !module->generics[s->helper].parameter_count) continue;
                if (next==count) return source_fail(ctx,s->syntax,XR_XIR_BAD_STRUCTURE,"comptime source call inventory grew");
                sites[next++]=(XrXirComptimeSite){f,i,s->helper};
            }
        }
    }
    if (!count || next!=count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime source call inventory is incomplete");
    table->sites=sites;table->site_count=count;return true;
}
static const SourceComptimeSite *source_program_syntax(SourceContext *ctx,uint32_t helper) {
    for (const SourceComptimeSite *site=ctx->comptime_sites;site;site=site->next) {
        if (!source_work_units(ctx,site->syntax,2)) return NULL;
        if (site->helper==helper) return site;
    }
    return NULL;
}
static bool source_program_failure(SourceContext *ctx,uint32_t helper,XrXirCtfeStatus status) {
    const SourceComptimeSite *site=source_program_syntax(ctx,helper);
    if (ctx->diagnostic.status!=XR_XIR_OK) return false;
    if (site) ctx->module=site->module;
    return source_comptime_failure(ctx,site?site->syntax:NULL,status);
}
static bool source_program_row_same(SourceContext *ctx,const XrXirTypes *types,
    const XrXirComptimeBinding *a,const XrXirComptimeBinding *b,bool *same) {
    *same=a->site==b->site && a->argument_count==b->argument_count;
    for (uint32_t i=0;*same && i<a->argument_count;++i) {
        XrXirStatus status=xr_xir_compile_type_substitution_matches_between(&ctx->compile,types,types,NULL,0,a->arguments[i],b->arguments[i]);
        if (status==XR_XIR_BAD_TYPE) *same=false;
        else if (status!=XR_XIR_OK) return source_fail(ctx,NULL,status,"comptime ordered key comparison failed");
    }
    return true;
}
static bool source_program_row_order(SourceContext *ctx,const XrXirComptimeBinding *a,
    const XrXirComptimeBinding *b,int *order) {
    if (!source_work_units(ctx,NULL,4)) return false;
    *order=a->site==b->site?0:a->site<b->site?-1:1;
    for (uint32_t i=0;!*order && i<a->argument_count && i<b->argument_count;++i) {
        if (!source_work_units(ctx,NULL,2)) return false;
        *order=a->arguments[i]==b->arguments[i]?0:a->arguments[i]<b->arguments[i]?-1:1;
    }
    if (!*order) *order=a->argument_count==b->argument_count?0:a->argument_count<b->argument_count?-1:1;
    return true;
}
static bool source_program_rows(SourceContext *ctx,const SourceProgramComptime *job,
    const XrXirTypes *types,XrXirComptimeTable *table) {
    XrXirComptimeBinding *rows=job->count?source_alloc(ctx,job->count,sizeof(*rows)):NULL;
    if (job->count && !rows) return false;
    uint32_t next=0;
    for (uint32_t i=0;i<job->count;++i) {
        const XirSourceComptimeBinding *actual=&job->actual[i];const XirLiteralBinding *literal=&job->literals.bindings[i];
        uint32_t site=UINT32_MAX;
        for (uint32_t s=0;s<table->site_count;++s) {
            if (!source_work_units(ctx,NULL,sizeof(XrXirComptimeSite))) return false;
            const XrXirComptimeSite *candidate=&table->sites[s];
            if (candidate->caller==actual->source_function && candidate->instruction==actual->source_instruction && candidate->helper==actual->helper) site=s;
        }
        if (site==UINT32_MAX || literal->definition!=actual->helper || literal->instance!=actual->callee)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime result lost its original call coordinates");
        XrXirComptimeBinding row={site,literal->arguments,literal->argument_count,literal->provider};bool duplicate=false;
        for (uint32_t b=0;b<next && !duplicate;++b)
            if (!source_program_row_same(ctx,types,&row,&rows[b],&duplicate)) return false;
        if (duplicate) continue;
        uint32_t at=next;
        while (at) {
            int order=0;if (!source_program_row_order(ctx,&rows[at-1],&row,&order)) return false;
            if (order<0) break;
            if (!source_work_units(ctx,NULL,sizeof(row))) return false;
            rows[at]=rows[at-1];--at;
        }
        if (!source_work_units(ctx,NULL,sizeof(row))) return false;
        rows[at]=row;++next;
    }
    table->bindings=next?rows:NULL;table->binding_count=next;return true;
}
