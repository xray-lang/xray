/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_pattern_facts.h - Observe producer-certified shared pattern tokens
 *
 * KEY CONCEPT:
 *   A marker identifies the lexical binding; its field reference supplies the
 *   independent payload identity. No source-text inference grants either one.
 */
#ifndef XLSP_SOURCE_PATTERN_FACTS_H
#define XLSP_SOURCE_PATTERN_FACTS_H
#include "../../xir/xxir_source_query.h"
#include "../../base/xcompile_resources.h"
typedef struct XlspSourcePatternBinding {uint32_t binding,field;} XlspSourcePatternBinding;
static bool xlsp_source_pattern_range(XrXirSourceRange a,XrXirSourceRange b) {
    return a.module==b.module&&a.line==b.line&&a.column==b.column&&a.end_line==b.end_line&&a.end_column==b.end_column;
}
static XrXirStatus xlsp_source_pattern_binding(XrCompileResources *resources,const XrXirSourceView *view,
    const XrXirSourceSyntaxView *syntax,XrXirSourceRange range,XlspSourcePatternBinding *out) {
    *out=(XlspSourcePatternBinding){0};
    for(uint32_t i=0;i<syntax->marker_count;++i) {
        XrCompileResourceStatus work=xr_compile_resources_work(resources,sizeof(syntax->markers[i]));
        if(work!=XR_COMPILE_RESOURCE_OK)return work==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
        const XrXirSourceMarker *marker=&syntax->markers[i];
        if(marker->role!=XR_XIR_SOURCE_MARKER_PATTERN_FIELD_BINDING||!xlsp_source_pattern_range(marker->range,range))continue;
        if(!marker->declaration||marker->declaration>view->declaration_count||
            view->declarations[marker->declaration-1].kind!=XR_XIR_SOURCE_BINDING)return XR_XIR_BAD_STRUCTURE;
        if(out->binding&&out->binding!=marker->declaration)return XR_XIR_BAD_STRUCTURE;
        out->binding=marker->declaration;
    }
    if(!out->binding)return XR_XIR_OK;
    for(uint32_t i=0;i<view->reference_count;++i) {
        XrCompileResourceStatus work=xr_compile_resources_work(resources,sizeof(view->references[i])+sizeof(syntax->references[i]));
        if(work!=XR_COMPILE_RESOURCE_OK)return work==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
        const XrXirSourceReference *ref=&view->references[i];
        if(!xlsp_source_pattern_range(syntax->references[i],range)||!ref->declaration||ref->declaration>view->declaration_count||
            ref->declaration!=ref->target||view->declarations[ref->declaration-1].kind!=XR_XIR_SOURCE_MEMBER)continue;
        if(out->field&&out->field!=ref->declaration)return XR_XIR_BAD_STRUCTURE;
        out->field=ref->declaration;
    }
    return out->field?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
#endif // XLSP_SOURCE_PATTERN_FACTS_H
