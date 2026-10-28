/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_module_identity.inc.c - Owned interactive declaration identities
 */
static const char *source_declaration_module_name(SourceContext *ctx,const XrModuleSpec *spec) {
    /* Resolver canonical IDs and physical roots retain their actual I/O
     * authority. Fresh interactive declarations have distinct birth owners,
     * even when the same file is loaded again. Exact Checked history keeps its
     * admitted names; neither name spelling nor this scope grants storage. */
    /* The selected governed prelude keeps the resolver owner authenticated by
     * its native declarations. It does not acquire a user declaration birth. */
    bool prelude=ctx->prelude_module&&ctx->graph&&
        ctx->prelude_module<=(uint32_t)ctx->graph->spec_count&&
        spec==&ctx->graph->specs[ctx->prelude_module-1];
    if(!ctx->module_scope || spec->representation==XR_MODULE_CHECKED_HISTORY || prelude)
        return spec->canonical;
    size_t scope=0,canonical=0;
    if(!source_text_length(ctx,NULL,ctx->module_scope,&scope) ||
        !source_text_length(ctx,NULL,spec->canonical,&canonical))return NULL;
    const size_t overhead=sizeof("repl-module::::")+40;
    if(scope>SIZE_MAX-overhead || canonical>SIZE_MAX-overhead-scope) {
        source_fail(ctx,NULL,XR_XIR_BUDGET,"interactive module identity size exhausted");return NULL;
    }
    size_t capacity=overhead+scope+canonical;
    char *name=source_alloc(ctx,capacity,1);
    if(!name || source_format(ctx,name,capacity,"repl-module:%zu:%s:%zu:%s",scope,ctx->module_scope,
        canonical,spec->canonical)!=XR_DIAG_OK)return NULL;
    return name;
}
