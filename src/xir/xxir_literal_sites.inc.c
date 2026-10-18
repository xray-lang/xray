/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_literal_sites.inc.c - Canonical origins precede opaque declarations
 *
 * KEY CONCEPT:
 *   A current genuine pending root supplies a site and real ordered result.
 *   Complete construction checking binds its literal before naming is admitted.
 */
static XrXirStatus literal_site_verify(const XrXirCompileContext *context,
    const XirCtfeInstanceView *view,const XrXirComptimeSite *site) {
    if (!site) return XR_XIR_OK;
    if (!xir_compile_work(context,128+sizeof(*site)+sizeof(XirEvaluationRoot))) return XR_XIR_BUDGET;
    const XrXirProvenance *proof=view->checked->module.provenance;
    if (!proof || proof->kind!=XR_XIR_EVIDENCE_EVALUATION || proof->source!=view->definition)
        return XR_XIR_BAD_STRUCTURE;
    const XirEvaluationRoot *root=&proof->evaluation;
    if (root->callee!=view->instance_function || root->instruction!=site->instruction ||
        proof->origins[root->caller].function!=site->caller || proof->origins[root->callee].function!=site->helper)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirComptimeSite *actual=xir_comptime_site(view->definition->construction,site->caller,site->instruction);
    return actual && actual->helper==site->helper?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static bool literal_site_bind(SourceContext *ctx,const XrXirConstruction *original,
    const XirLiteralBinding *binding,const XrXirComptimeSite *wanted,XrXirComptimeTable *table) {
    *table=original->comptime;
    if (!wanted) return true;
    if (!source_work_units(ctx,NULL,128+sizeof(*binding)+sizeof(*wanted))) return false;
    const XrXirComptimeSite *site=xir_comptime_site(original,wanted->caller,wanted->instruction);
    if (!site || site->helper!=wanted->helper || binding->definition!=wanted->helper)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"literal lost its current canonical origin");
    if (table->binding_count>=ctx->compile.limits.instructions)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"literal canonical inventory exhausted");
    XrXirComptimeBinding *rows=source_alloc(ctx,(uint64_t)table->binding_count+1,sizeof(*rows));
    if (!rows || !source_copy_bytes(ctx,NULL,rows,table->bindings,(size_t)table->binding_count*sizeof(*rows))) return false;
    XrXirComptimeBinding row={(uint32_t)(site-table->sites),binding->arguments,binding->argument_count,binding->provider};
    uint32_t at=table->binding_count;
    while (at) {
        if (!source_work_units(ctx,NULL,4+2*(uint64_t)row.argument_count+2*(uint64_t)rows[at-1].argument_count)) return false;
        int order=xir_comptime_key_order(&rows[at-1],&row);
        if (!order) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"literal cannot replace an existing canonical key");
        if (order<0) break;
        if (!source_work_units(ctx,NULL,sizeof(*rows))) return false;
        rows[at]=rows[at-1];--at;
    }
    if (!source_work_units(ctx,NULL,sizeof(row))) return false;
    rows[at]=row;table->bindings=rows;++table->binding_count;return true;
}
