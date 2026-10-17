/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_rename_facts.h - Select and collect exact canonical binding occurrences
 *
 * KEY CONCEPT:
 *   Canonical Source owns binding semantics. Consumers retain exact owned facts
 *   and reject unavailable evidence without publishing partial edits.
 */
#ifndef XLSP_SOURCE_RENAME_FACTS_H
#define XLSP_SOURCE_RENAME_FACTS_H
static XrXirStatus rename_fact_length(RenameWork *w,const char *text,size_t *out) {
    if(!text)return XR_XIR_UNRESOLVED;
    for(size_t i=0;;++i){XrXirStatus s=rename_work(w,1);if(s!=XR_XIR_OK)return s;if(!text[i]){*out=i;return XR_XIR_OK;}if(i==SIZE_MAX-1)return XR_XIR_BUDGET;}
}
static bool rename_absent(XrXirSourceRange r){return !r.line&&!r.column&&!r.end_line&&!r.end_column;}
static XrXirStatus rename_text_document(RenameWork *w,const XlspSourceBindings *queries,const XrXirSourceView *view,uint32_t module,size_t *out) {
    if(module>=view->module_count)return XR_XIR_BAD_STRUCTURE;const char *path=view->modules[module].path;size_t length=0;
    XrXirStatus status=rename_fact_length(w,path,&length);if(status!=XR_XIR_OK)return status;
    for(size_t i=0;i<w->count;++i){const char *candidate=xlsp_source_bindings_path(queries,i);size_t candidate_length=0;
        status=rename_fact_length(w,candidate,&candidate_length);if(status!=XR_XIR_OK)return status;bool same=false;
        status=rename_equal(w,path,length,candidate,candidate_length,&same);if(status!=XR_XIR_OK)return status;if(same){*out=i;return XR_XIR_OK;}}
    return XR_XIR_UNRESOLVED;
}
typedef struct RenameSpelling {XrXirSourceRange range;const char *name;} RenameSpelling;
static XrXirStatus rename_fact_range(RenameWork *w,const XlspSourceBindings *queries,const XlspSourceDocument *documents,
    const XrXirSourceView *view,RenameSpelling spelling,RenameEdit *out) {
    XrXirSourceRange range=spelling.range;const char *name=spelling.name;
    if(range.line<=0||range.column<=0||range.end_line!=range.line||range.end_column<=range.column)return XR_XIR_UNSUPPORTED;
    size_t document=0;XrXirStatus status=rename_text_document(w,queries,view,range.module,&document);if(status!=XR_XIR_OK)return status;
    const XlspSourceDocument *d=&documents[document];
    if(name) {
        size_t length=0;status=rename_fact_length(w,name,&length);if(status!=XR_XIR_OK)return status;
        if(length!=(size_t)(range.end_column-range.column))return XR_XIR_UNSUPPORTED;
        size_t offset=0;int line=1;
        while(offset<d->length&&line<range.line){status=rename_work(w,1);if(status!=XR_XIR_OK)return status;if(d->text[offset++]=='\n')++line;}
        if(line!=range.line||(size_t)(range.column-1)>d->length-offset)return XR_XIR_UNSUPPORTED;offset+=(size_t)(range.column-1);
        if(length>d->length-offset)return XR_XIR_UNSUPPORTED;bool same=false;status=rename_equal(w,d->text+offset,length,name,length,&same);if(status!=XR_XIR_OK)return status;if(!same)return XR_XIR_UNSUPPORTED;
    }
    XrLspRange result={0};status=xlsp_source_position(w->resources,d->text,d->length,range.line,range.column,&result.start);
    if(status==XR_XIR_OK)status=xlsp_source_position(w->resources,d->text,d->length,range.end_line,range.end_column,&result.end);
    if(status==XR_XIR_OK)*out=(RenameEdit){.document=document,.range=result};return status;
}
#include "xlsp_source_rename_imports.h"
static XrXirStatus rename_symbol(RenameWork *w,const XlspSourceBindings *queries,const XlspSourceDocument *documents,
    size_t root,uint32_t id,RenameSymbol *out) {
    const XrXirSourceView *view=xlsp_source_bindings_view(queries,root);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(queries,root);
    if(!view||!syntax||!id||id>view->declaration_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceDeclaration *d=&view->declarations[id-1];const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[id-1];
    bool implicit=d->kind==XR_XIR_SOURCE_MODULE&&fact->role==XR_XIR_SOURCE_SYNTAX_NONE;
    if(!implicit&&(fact->role==XR_XIR_SOURCE_SYNTAX_NONE||fact->role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED||rename_absent(fact->name)))return XR_XIR_UNSUPPORTED;
    if(fact->name.module!=d->range.module)return XR_XIR_BAD_STRUCTURE;
    RenameSymbol symbol={0};XrXirStatus status=implicit?rename_import_insertion(w,queries,documents,root,id,&symbol.declared):
        rename_fact_range(w,queries,documents,view,(RenameSpelling){fact->name,d->name},&symbol.declared);if(status!=XR_XIR_OK)return status;
    symbol.kind=d->kind;symbol.name=d->name;symbol.identity=view->modules[fact->name.module].identity;
    status=rename_fact_length(w,symbol.name,&symbol.name_length);if(status==XR_XIR_OK)status=rename_fact_length(w,symbol.identity,&symbol.identity_length);
    if(status==XR_XIR_OK)*out=symbol;return status;
}
static XrXirStatus rename_symbol_same(RenameWork *w,RenameSymbol a,RenameSymbol b,bool *out) {
    *out=false;if(a.kind!=b.kind||a.declared.document!=b.declared.document||!rename_same(a.declared.range,b.declared.range))return XR_XIR_OK;
    bool same=false;XrXirStatus status=rename_equal(w,a.identity,a.identity_length,b.identity,b.identity_length,&same);if(status!=XR_XIR_OK||!same)return status;
    return rename_equal(w,a.name,a.name_length,b.name,b.name_length,out);
}
static bool rename_contains(XrLspRange range,XrLspPosition position) {
    return range.start.line==position.line&&range.start.character<=position.character&&position.character<range.end.character;
}
static uint32_t rename_reference_id(const XrXirSourceView *view,const XrXirSourceReference *reference) {
    uint32_t id=reference->declaration;
    if(!id||id>view->declaration_count)return 0;
    /* A MODULE binding names the qualifier, while Source's member selection
     * names its target. IMPORT aliases and callable variables keep their
     * direct binding even when navigation follows another target. */
    if(view->declarations[id-1].kind==XR_XIR_SOURCE_MODULE&&reference->target)id=reference->target;
    return id<=view->declaration_count?id:0;
}
static XrXirStatus rename_choose(RenameWork *w,const XlspSourceBindings *queries,const XlspSourceDocument *documents,
    size_t root,uint32_t id,RenameSymbol *chosen) {
    RenameSymbol symbol={0};XrXirStatus status=rename_symbol(w,queries,documents,root,id,&symbol);if(status!=XR_XIR_OK)return status;
    if(chosen->name){bool same=false;status=rename_symbol_same(w,*chosen,symbol,&same);if(status!=XR_XIR_OK)return status;if(!same)return XR_XIR_UNSUPPORTED;}
    else *chosen=symbol;return XR_XIR_OK;
}
static XrXirStatus rename_pattern_edit(RenameWork *w,const XrXirSourceView *view,const XrXirSourceSyntaxView *syntax,
    XrXirSourceRange span,uint32_t selected,RenameEdit *edit) {
    XlspSourcePatternBinding pattern={0};XrXirStatus status=xlsp_source_pattern_binding(w->resources,view,syntax,span,&pattern);
    if(status!=XR_XIR_OK)return status;
    if(!pattern.binding)return rename_import_edit(w,view,syntax,span,selected,edit);
    if(selected==pattern.binding){edit->preserved=view->declarations[pattern.field-1].name;edit->prefix=true;}
    else if(selected==pattern.field){edit->preserved=view->declarations[pattern.binding-1].name;edit->prefix=false;}
    return XR_XIR_OK;
}
static XrXirStatus rename_symbol_at(RenameWork *w,const XlspSourceBindings *queries,const XlspSourceDocument *documents,
    const XlspSourceRenameInput *input,RenameSymbol *out) {
    RenameSymbol chosen={0};
    for(size_t root=0;root<xlsp_source_bindings_count(queries);++root) {
        const XrXirSourceView *view=xlsp_source_bindings_view(queries,root);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(queries,root);
        for(uint32_t i=0;i<view->declaration_count;++i) {
            XrXirStatus status=rename_work(w,sizeof(XrXirSourceDeclarationSyntax));if(status!=XR_XIR_OK)return status;
            XrXirSourceRange span=syntax->declarations[i].name;if(rename_absent(span)||syntax->declarations[i].role==XR_XIR_SOURCE_SYNTAX_NONE)continue;
            size_t document=0;status=rename_text_document(w,queries,view,span.module,&document);if(status==XR_XIR_UNRESOLVED)continue;if(status!=XR_XIR_OK)return status;if(document!=input->entry)continue;
            if(span.line<=0||span.column<=0)continue;
            RenameEdit occurrence={0};status=rename_fact_range(w,queries,documents,view,(RenameSpelling){span,NULL},&occurrence);if(status!=XR_XIR_OK)return status;
            if(rename_contains(occurrence.range,input->position)) {
                XlspSourcePatternBinding pattern={0};status=xlsp_source_pattern_binding(w->resources,view,syntax,span,&pattern);if(status!=XR_XIR_OK)return status;
                if(pattern.binding==i+1)continue;
                uint32_t imported=0;status=rename_import_binding(w,syntax,span,&imported);if(status!=XR_XIR_OK)return status;
                if(imported==i+1&&view->declarations[i].kind==XR_XIR_SOURCE_IMPORT)continue;
                status=rename_choose(w,queries,documents,root,i+1,&chosen);if(status!=XR_XIR_OK)return status;
            }
        }
        for(uint32_t i=0;i<view->reference_count;++i) {
            XrXirStatus status=rename_work(w,sizeof(XrXirSourceReference));if(status!=XR_XIR_OK)return status;XrXirSourceRange span=syntax->references[i];if(rename_absent(span))continue;
            size_t document=0;status=rename_text_document(w,queries,view,span.module,&document);if(status==XR_XIR_UNRESOLVED)continue;if(status!=XR_XIR_OK)return status;if(document!=input->entry)continue;
            uint32_t id=rename_reference_id(view,&view->references[i]);if(!id)return XR_XIR_BAD_STRUCTURE;
            RenameEdit occurrence={0};status=rename_fact_range(w,queries,documents,view,(RenameSpelling){span,NULL},&occurrence);if(status!=XR_XIR_OK)return status;
            /* Preserve the direct declaration. A callable's navigation target
             * is not the identity of the variable or import that names it. */
            if(rename_contains(occurrence.range,input->position)) {
                XlspSourcePatternBinding pattern={0};status=xlsp_source_pattern_binding(w->resources,view,syntax,span,&pattern);if(status!=XR_XIR_OK)return status;
                if(pattern.binding==id)continue;
                status=rename_choose(w,queries,documents,root,id,&chosen);if(status!=XR_XIR_OK)return status;
            }
        }
    }
    if(!chosen.name)return XR_XIR_UNRESOLVED;*out=chosen;return XR_XIR_OK;
}
static XrXirStatus rename_edit_append(RenameWork *w,RenameEdit edit,RenameEdit **items,size_t *count,size_t *capacity) {
    for(size_t i=0;i<*count;++i){XrXirStatus status=rename_work(w,sizeof(edit));if(status!=XR_XIR_OK)return status;if((*items)[i].document==edit.document&&rename_same((*items)[i].range,edit.range))return XR_XIR_OK;}
    if(*count==*capacity){size_t next=*capacity?*capacity*2:16;if(next<*capacity||next>SIZE_MAX/sizeof(edit))return XR_XIR_BUDGET;
        XrXirStatus status=rename_resource(xr_compile_resources_resize(w->resources,(void **)items,next*sizeof(edit)));if(status!=XR_XIR_OK)return status;*capacity=next;}
    size_t at=*count;
    while(at){XrXirStatus status=rename_work(w,sizeof(edit));if(status!=XR_XIR_OK)return status;if(rename_order((*items)[at-1],edit)<=0)break;(*items)[at]=(*items)[at-1];--at;}
    (*items)[at]=edit;++*count;return XR_XIR_OK;
}
static XrXirStatus rename_collect(RenameWork *w,const XlspSourceBindings *queries,const XlspSourceDocument *documents,
    const RenameSymbol *selected,RenameEdit **items,size_t *count) {
    size_t capacity=0;XrXirStatus status=XR_XIR_OK;
    for(size_t root=0;root<xlsp_source_bindings_count(queries);++root) {
        const XrXirSourceView *view=xlsp_source_bindings_view(queries,root);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(queries,root);
        for(uint32_t i=0;i<view->declaration_count;++i) {
            status=rename_work(w,sizeof(XrXirSourceDeclaration));if(status!=XR_XIR_OK)return status;const XrXirSourceDeclaration *d=&view->declarations[i];
            if(d->kind!=selected->kind)continue;
            if(d->kind!=XR_XIR_SOURCE_MODULE&&(syntax->declarations[i].role==XR_XIR_SOURCE_SYNTAX_NONE||rename_absent(syntax->declarations[i].name)))continue;
            size_t name_length=0;status=rename_fact_length(w,d->name,&name_length);if(status!=XR_XIR_OK)return status;bool same=false;
            status=rename_equal(w,d->name,name_length,selected->name,selected->name_length,&same);if(status!=XR_XIR_OK)return status;if(!same)continue;
            size_t document=0;status=rename_text_document(w,queries,view,d->range.module,&document);if(status==XR_XIR_UNRESOLVED)continue;if(status!=XR_XIR_OK)return status;if(document!=selected->declared.document)continue;
            RenameSymbol symbol={0};status=rename_symbol(w,queries,documents,root,i+1,&symbol);if(status!=XR_XIR_OK)return status;
            status=rename_symbol_same(w,symbol,*selected,&same);if(status!=XR_XIR_OK)return status;if(!same)continue;
            status=rename_pattern_edit(w,view,syntax,syntax->declarations[i].name,d->id,&symbol.declared);if(status!=XR_XIR_OK)return status;
            status=rename_edit_append(w,symbol.declared,items,count,&capacity);if(status!=XR_XIR_OK)return status;
            for(uint32_t j=0;j<view->reference_count;++j) {
                status=rename_work(w,sizeof(XrXirSourceReference));if(status!=XR_XIR_OK)return status;if(rename_reference_id(view,&view->references[j])!=d->id)continue;
                XrXirSourceRange span=syntax->references[j];if(rename_absent(span)){if(!rename_absent(view->references[j].range))return XR_XIR_UNSUPPORTED;continue;}
                RenameEdit occurrence={0};status=rename_fact_range(w,queries,documents,view,(RenameSpelling){span,d->name},&occurrence);if(status!=XR_XIR_OK)return status;
                status=rename_pattern_edit(w,view,syntax,span,d->id,&occurrence);if(status!=XR_XIR_OK)return status;
                status=rename_edit_append(w,occurrence,items,count,&capacity);if(status!=XR_XIR_OK)return status;
            }
        }
    }
    return *count?XR_XIR_OK:XR_XIR_UNRESOLVED;
}
static XrXirStatus rename_bindings(RenameWork *w,const XlspSourceRenameInput *input) {
    XrXirStatus status=rename_symbol_at(w,w->original,w->documents,input,&w->symbol);if(status!=XR_XIR_OK)return status;
    w->old_length=w->symbol.name_length;status=rename_copy(w,w->symbol.name,w->old_length,&w->old_name);if(status!=XR_XIR_OK)return status;
    status=rename_collect(w,w->original,w->documents,&w->symbol,&w->edits,&w->edit_count);if(status!=XR_XIR_OK)return status;
    bool selected=false,declared=false;
    for(size_t i=0;i<w->edit_count;++i){status=rename_work(w,sizeof(RenameEdit));if(status!=XR_XIR_OK)return status;
        if(w->edits[i].document==input->entry&&rename_contains(w->edits[i].range,input->position)){w->selected=w->edits[i].range;selected=true;}
        if(w->edits[i].document==w->symbol.declared.document&&rename_same(w->edits[i].range,w->symbol.declared.range)){w->definition=i;declared=true;}}
    return selected&&declared?XR_XIR_OK:XR_XIR_UNRESOLVED;
}
#endif // XLSP_SOURCE_RENAME_FACTS_H
