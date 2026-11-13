/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_specialize_lexical.inc.c - Substitute lexical facts with their real body
 */
static bool spec_lexical(SpecContext *c, XirLexicalProgram *output) {
    const XrXirProvenance *source=c->source->provenance;
    if (!source || !source->lexical.function_count) return true;
    if (source->lexical.function_count!=c->source->function_count || !source->lexical.functions)
        return false;
    XirLexicalFunction *functions=spec_alloc(c,c->count,sizeof(*functions));
    if (!functions) return false;
    for (uint32_t f=0;f<c->count;++f) {
        if (!spec_progress(c)) return false;
        SpecInstance instance=c->instances[f];
        XirLexicalFunction row=source->lexical.functions[instance.declaration];
        XirLexicalBinding *bindings=row.binding_count ? spec_alloc(c,row.binding_count,sizeof(*bindings)) : NULL;
        if (row.binding_count && (!bindings ||
            !spec_copy(c,bindings,row.bindings,(size_t)row.binding_count*sizeof(*bindings)))) return false;
        for (uint32_t b=0;b<row.binding_count;++b) {
            bindings[b].key.function=f;
            bindings[b].logical=spec_type(c,&instance,bindings[b].logical);
            bindings[b].physical=spec_type(c,&instance,bindings[b].physical);
            if (c->diagnostic.status!=XR_XIR_OK) return false;
        }
        row.bindings=bindings;
        functions[f]=row;
    }
    *output=(XirLexicalProgram){functions,c->count};
    return true;
}
