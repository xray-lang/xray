/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_rename_imports.h - Preserve canonical import selector identities
 *
 * KEY CONCEPT:
 *   Shared selector tokens and absent aliases use producer-owned markers.
 *   Text is checked against facts, never used to invent a binding or location.
 */
#ifndef XLSP_SOURCE_RENAME_IMPORTS_H
#define XLSP_SOURCE_RENAME_IMPORTS_H
static XrXirStatus rename_import_binding(RenameWork *w,const XrXirSourceSyntaxView *syntax,
    XrXirSourceRange range,uint32_t *out) {
    *out=0;
    for(uint32_t i=0;i<syntax->marker_count;++i) {
        XrXirStatus status=rename_work(w,sizeof(syntax->markers[i]));if(status!=XR_XIR_OK)return status;
        const XrXirSourceMarker *marker=&syntax->markers[i];
        if(marker->role!=XR_XIR_SOURCE_MARKER_IMPORT_BINDING||!xlsp_source_pattern_range(marker->range,range))continue;
        if(*out)return XR_XIR_BAD_STRUCTURE;*out=marker->declaration;
    }
    return XR_XIR_OK;
}
static XrXirStatus rename_import_insertion(RenameWork *w,const XlspSourceBindings *queries,
    const XlspSourceDocument *documents,size_t root,uint32_t id,RenameEdit *out) {
    const XrXirSourceView *view=xlsp_source_bindings_view(queries,root);
    const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(queries,root);
    bool found=false;XrXirSourceRange point={0};
    for(uint32_t i=0;i<syntax->marker_count;++i) {
        XrXirStatus status=rename_work(w,sizeof(syntax->markers[i]));if(status!=XR_XIR_OK)return status;
        const XrXirSourceMarker *marker=&syntax->markers[i];
        if(marker->declaration!=id||marker->role!=XR_XIR_SOURCE_MARKER_IMPORT_IMPLICIT)continue;
        if(found)return XR_XIR_BAD_STRUCTURE;point=marker->range;found=true;
    }
    if(!found)return XR_XIR_UNSUPPORTED;
    if(point.line<=0||point.column<=0||point.end_line!=point.line||point.end_column!=point.column)return XR_XIR_BAD_STRUCTURE;
    size_t document=0;XrXirStatus status=rename_text_document(w,queries,view,point.module,&document);if(status!=XR_XIR_OK)return status;
    const XlspSourceDocument *d=&documents[document];XrLspPosition position={0};
    status=xlsp_source_position(w->resources,d->text,d->length,point.line,point.column,&position);
    if(status==XR_XIR_OK)*out=(RenameEdit){.document=document,.range={position,position},.preserved="",.separator=" as ",.prefix=true};
    return status;
}
static XrXirStatus rename_import_edit(RenameWork *w,const XrXirSourceView *view,const XrXirSourceSyntaxView *syntax,
    XrXirSourceRange span,uint32_t selected,RenameEdit *edit) {
    uint32_t binding=0;XrXirStatus status=rename_import_binding(w,syntax,span,&binding);
    if(status!=XR_XIR_OK||!binding)return status;
    if(binding>view->declaration_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceDeclaration *decl=&view->declarations[binding-1];
    if(selected==binding){edit->preserved=decl->name;edit->prefix=true;edit->separator=" as ";}
    else if(decl->kind==XR_XIR_SOURCE_IMPORT&&selected==decl->target) {
        edit->preserved=decl->name;edit->prefix=false;edit->separator=" as ";
    }
    return XR_XIR_OK;
}
#endif // XLSP_SOURCE_RENAME_IMPORTS_H
