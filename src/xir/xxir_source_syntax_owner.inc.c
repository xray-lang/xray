/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_syntax_owner.inc.c - Immutable syntax fact ownership
 *
 * KEY CONCEPT:
 *   Deep-own display facts without construction or execution authority.
 */
#include "xxir_types.h"
/* Scalar syntax facts are copied after the semantic/construction owner exists,
 * before publication. A failed attachment leaves that owner unchanged. */
static bool query_syntax_range(const XrXirSourceView *view,XrXirSourceRange range,bool absent) {
    if(range.module>=view->module_count)return false;
    if(absent)return !range.line&&!range.column&&!range.end_line&&!range.end_column;
    return range.line>0&&range.column>0&&range.end_line==range.line&&range.end_column>range.column;
}
XR_FUNC XrXirStatus xr_xir_compile_source_snapshot_syntax_copy(XrXirSourceSnapshot *snapshot,
    const XrXirSourceSyntaxView *syntax) {
    if(!snapshot||!syntax||snapshot->syntax_ready)return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceView *view=&snapshot->view;
    if(syntax->declaration_count!=view->declaration_count||syntax->reference_count!=view->reference_count||
        (!!syntax->declarations!=!!syntax->declaration_count)||
        (!!syntax->references!=!!syntax->reference_count)|| (!!syntax->markers!=!!syntax->marker_count))return XR_XIR_BAD_STRUCTURE;
    SourceQueryCopy copy={snapshot,XR_XIR_OK};
    for(uint32_t i=0;i<syntax->declaration_count;++i) {
        if(!query_work(&copy,sizeof(syntax->declarations[i])))return copy.status;
        const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[i];
        const XrXirSourceDeclaration *decl=&view->declarations[i];
        bool absent=fact->role==XR_XIR_SOURCE_SYNTAX_NONE||fact->role==XR_XIR_SOURCE_SYNTAX_CLOSURE||fact->role==XR_XIR_SOURCE_SYNTAX_RECEIVER;
        if((uint32_t)fact->role>XR_XIR_SOURCE_SYNTAX_UNRESOLVED||fact->name.module!=decl->range.module||fact->name.module>=view->module_count||
            (fact->role!=XR_XIR_SOURCE_SYNTAX_UNRESOLVED&&!query_syntax_range(view,fact->name,absent))||
            (fact->flags&~XR_XIR_SOURCE_SYNTAX_STATIC)||
            (fact->flags&&fact->role!=XR_XIR_SOURCE_SYNTAX_METHOD))return XR_XIR_BAD_STRUCTURE;
        if(fact->role==XR_XIR_SOURCE_SYNTAX_METHOD&&
            ((decl->kind!=XR_XIR_SOURCE_FUNCTION&&decl->kind!=XR_XIR_SOURCE_MEMBER)||!decl->parent||decl->parent>view->declaration_count||
                view->declarations[decl->parent-1].kind!=XR_XIR_SOURCE_TYPE))return XR_XIR_BAD_STRUCTURE;
        if(fact->role==XR_XIR_SOURCE_SYNTAX_CLOSURE&&
            (decl->kind!=XR_XIR_SOURCE_FUNCTION||decl->parent>view->declaration_count||
                (decl->parent&&view->declarations[decl->parent-1].kind!=XR_XIR_SOURCE_FUNCTION&&
                    (view->declarations[decl->parent-1].kind!=XR_XIR_SOURCE_MEMBER||
                        syntax->declarations[decl->parent-1].role!=XR_XIR_SOURCE_SYNTAX_NAME))))return XR_XIR_BAD_STRUCTURE;
        if(fact->role==XR_XIR_SOURCE_SYNTAX_RECEIVER&&
            ((decl->kind!=XR_XIR_SOURCE_PARAMETER&&decl->kind!=XR_XIR_SOURCE_BINDING)||!decl->parent||decl->parent>syntax->declaration_count||
                syntax->declarations[decl->parent-1].role!=XR_XIR_SOURCE_SYNTAX_METHOD||
                (syntax->declarations[decl->parent-1].flags&XR_XIR_SOURCE_SYNTAX_STATIC)))return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t i=0;i<syntax->reference_count;++i) {
        if(!query_work(&copy,sizeof(syntax->references[i])))return copy.status;
        if(syntax->references[i].module!=view->references[i].range.module||
            syntax->references[i].module>=view->module_count)return XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t i=0;i<syntax->marker_count;++i) {
        if(!query_work(&copy,sizeof(syntax->markers[i])))return copy.status;
        const XrXirSourceMarker *marker=&syntax->markers[i];
        if(!marker->declaration||marker->declaration>view->declaration_count||
            (marker->role!=XR_XIR_SOURCE_MARKER_REF&&marker->role!=XR_XIR_SOURCE_MARKER_MOVE&&marker->role!=XR_XIR_SOURCE_MARKER_REF_ARGUMENT)||
            !query_syntax_range(view,marker->range,false))return XR_XIR_BAD_STRUCTURE;
        const XrXirSourceDeclaration *decl=&view->declarations[marker->declaration-1];
        const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[marker->declaration-1];
        if(marker->range.module!=decl->range.module)return XR_XIR_BAD_STRUCTURE;
        if(marker->role==XR_XIR_SOURCE_MARKER_REF_ARGUMENT) {
            /* A readonly Class binding may name an exclusive field projection.
             * This marker is observational; the checked place still authenticates
             * field mutability and the actual object's runtime loan separately. */
            bool class_root=decl->type.known&&xr_xir_type_is_class(view->types,decl->type.type);
            if((!decl->mutable&&!class_root)||
                (decl->kind!=XR_XIR_SOURCE_BINDING&&decl->kind!=XR_XIR_SOURCE_PARAMETER))return XR_XIR_BAD_STRUCTURE;
        } else if((decl->kind!=XR_XIR_SOURCE_PARAMETER&&fact->role!=XR_XIR_SOURCE_SYNTAX_METHOD)||
            (fact->flags&XR_XIR_SOURCE_SYNTAX_STATIC))return XR_XIR_BAD_STRUCTURE;
        if(marker->range.end_column-marker->range.column!=(marker->role==XR_XIR_SOURCE_MARKER_MOVE?4:3))return XR_XIR_BAD_STRUCTURE;
    }
    SourceQueryMemory *previous=snapshot->memory;
    XrXirSourceSyntaxView owned=*syntax;
    owned.declarations=query_copy(&copy,syntax->declarations,syntax->declaration_count,sizeof(*syntax->declarations));
    owned.references=query_copy(&copy,syntax->references,syntax->reference_count,sizeof(*syntax->references));
    owned.markers=query_copy(&copy,syntax->markers,syntax->marker_count,sizeof(*syntax->markers));
    (void)query_work(&copy,sizeof(owned)+sizeof(snapshot->syntax_ready));
    if(copy.status!=XR_XIR_OK) {
        while(snapshot->memory!=previous) {SourceQueryMemory *next=snapshot->memory->next;xr_compile_resources_free(snapshot->memory);snapshot->memory=next;}
        return copy.status;
    }
    snapshot->syntax=owned;snapshot->syntax_ready=true;return XR_XIR_OK;
}
XR_FUNC const XrXirSourceSyntaxView *xr_xir_compile_source_snapshot_syntax(const XrXirSourceSnapshot *snapshot) {
    return snapshot&&snapshot->syntax_ready?&snapshot->syntax:NULL;
}
