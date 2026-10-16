/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_expression_intrinsic.inc.c - Readonly calls retain actual Core identities
 */
#include "../../shared/xr_core_intrinsic.h"
static bool expression_intrinsic_visible(ExpressionState *state,const char *name) {
    const XdapLexicalView *view=state->request->lexical;XrCompileResources *resources=view->launch->resources;
    if (view->function->function>=view->launch->function_count) { state->status=XR_XIR_BAD_STRUCTURE;return false; }
    uint32_t module=view->launch->functions[view->function->function].module;
    if (module>=view->launch->module_count) { state->status=XR_XIR_BAD_STRUCTURE;return false; }
    uint32_t query_module=view->launch->modules[module].query_module;size_t bytes=strlen(name)+1;
    for (uint32_t b=view->environment;b;) {
        if (xr_compile_resources_work(resources,bytes)!=XR_COMPILE_RESOURCE_OK) { state->status=XR_XIR_BUDGET;return false; }
        if (b>view->function->binding_count) { state->status=XR_XIR_BAD_STRUCTURE;return false; }
        const XrXirSourceDebugBinding *binding=&view->function->bindings[b-1];
        if (!binding->declaration || binding->declaration>view->source->declaration_count || binding->parent>=b) {
            state->status=XR_XIR_BAD_STRUCTURE;return false;
        }
        const char *candidate=view->source->declarations[binding->declaration-1].name;
        if (candidate && !strncmp(candidate,name,bytes)) { state->status=XR_XIR_BAD_TYPE;return false; }
        b=binding->parent;
    }
    for (uint32_t i=0;i<view->source->declaration_count;++i) {
        if (xr_compile_resources_work(resources,bytes+1)!=XR_COMPILE_RESOURCE_OK) { state->status=XR_XIR_BUDGET;return false; }
        const XrXirSourceDeclaration *declaration=&view->source->declarations[i];
        if (declaration->range.module!=query_module || declaration->kind==XR_XIR_SOURCE_INTRINSIC ||
            !declaration->name || strncmp(declaration->name,name,bytes)) continue;
        uint32_t scope=view->function->declaration;
        for (uint32_t depth=0;;++depth) {
            if (xr_compile_resources_work(resources,1)!=XR_COMPILE_RESOURCE_OK) { state->status=XR_XIR_BUDGET;return false; }
            if (declaration->parent==scope) { state->status=XR_XIR_BAD_TYPE;return false; }
            if (!scope) break;
            if (scope>view->source->declaration_count || depth>=view->source->declaration_count) {
                state->status=XR_XIR_BAD_STRUCTURE;return false;
            }
            scope=view->source->declarations[scope-1].parent;
        }
    }
    return true;
}
static XrXirStatus expression_intrinsic_proof(const ExpressionState *state,const XrXirSourceProduct *product) {
    if (!state->intrinsic_calls) return XR_XIR_OK;
    const XrXirSourceView *view=xr_xir_compile_source_product_view(product);
    const XrXirCompileContext *context=xr_xir_compile_source_product_context(product);
    if (!view || !view->complete || !context) return XR_XIR_BAD_STRUCTURE;
    uint32_t matched=0;const char identity[]="xray-core:prelude";
    for (uint32_t i=0;i<view->reference_count;++i) {
        if (xr_compile_resources_work(context->resources,sizeof(XrXirSourceReference)+sizeof(identity))!=XR_COMPILE_RESOURCE_OK)
            return XR_XIR_BUDGET;
        const XrXirSourceReference *reference=&view->references[i];
        if (reference->access!=XR_XIR_SOURCE_CALL) continue;
        if (!reference->target || reference->target>view->declaration_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirSourceDeclaration *declaration=&view->declarations[reference->target-1];
        if (declaration->kind!=XR_XIR_SOURCE_INTRINSIC || declaration->native_identity!=XR_CORE_BUILTIN_LEN) continue;
        if (!declaration->exported || !declaration->type.known || declaration->type.generic_owner ||
            declaration->type.type!=XR_XIR_I64 || declaration->parameter_count!=1 ||
            declaration->range.module>=view->module_count || !view->modules[declaration->range.module].identity ||
            strncmp(view->modules[declaration->range.module].identity,identity,sizeof(identity))) return XR_XIR_BAD_TYPE;
        if (matched==state->intrinsic_calls) return XR_XIR_BAD_STRUCTURE;
        ++matched;
    }
    return matched==state->intrinsic_calls ? XR_XIR_OK : XR_XIR_BAD_TYPE;
}
