/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_source_inlay.h"
typedef struct SourceTypeHintsBuilder {XlspSourceTypeHints hints;size_t capacity;} SourceTypeHintsBuilder;
XR_FUNC void xlsp_source_type_hints_free(XlspSourceTypeHints *hints) {
    if(!hints)return;
    for(size_t i=0;i<hints->count;++i)xr_compile_resources_free(hints->items[i].label);
    xr_compile_resources_free(hints->items);*hints=(XlspSourceTypeHints){0};
}
static bool type_hint_position_before(XrLspPosition a,XrLspPosition b) {
    return a.line<b.line||(a.line==b.line&&a.character<b.character);
}
static XrXirStatus type_hint_append(const XlspSourceSnapshot *s,const XrXirSourceDeclaration *declaration,
    XrLspPosition position,SourceTypeHintsBuilder *builder) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s->query);
    LspHoverBuilder label={s,NULL,0,0,XR_XIR_OK};hover_text(&label,": ");hover_type(&label,view,declaration->type);
    XrXirStatus status=label.status;if(status!=XR_XIR_OK)goto done;
    if(builder->hints.count==builder->capacity) {
        size_t capacity=builder->capacity?builder->capacity*2:8;
        if(capacity<builder->capacity||capacity>SIZE_MAX/sizeof(XlspSourceTypeHint)){status=XR_XIR_BUDGET;goto done;}
        status=lsp_resource_status(xr_compile_resources_resize(s->resources,(void **)&builder->hints.items,
            capacity*sizeof(*builder->hints.items)));
        if(status!=XR_XIR_OK)goto done;
        builder->capacity=capacity;
    }
    status=lsp_work(s,sizeof(XlspSourceTypeHint));if(status!=XR_XIR_OK)goto done;
    builder->hints.items[builder->hints.count++]=(XlspSourceTypeHint){position,label.text,label.length};label.text=NULL;
done:
    xr_compile_resources_free(label.text);return status;
}
XR_FUNC XrXirStatus xlsp_source_type_hints(const XlspSourceSnapshot *s,const char *uri,size_t length,
    XrLspRange range,XlspSourceTypeHints *out) {
    if(!s||!uri||length==SIZE_MAX||!out||out->items||out->count)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xlsp_source_semantic_range(s,uri,length,range);if(status!=XR_XIR_OK)return status;
    const LspSourceText *text=NULL;
    for(size_t i=0;i<s->count;++i) {
        status=lsp_work(s,1);if(status!=XR_XIR_OK)return status;
        if(s->texts[i].uri_length!=length)continue;
        bool same=false;status=lsp_equal(s,s->texts[i].uri,uri,length,&same);if(status!=XR_XIR_OK)return status;
        if(same){text=&s->texts[i];break;}
    }
    if(!text)return XR_XIR_UNRESOLVED;
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s->query);
    const XrXirSourceSyntaxView *syntax=view?lsp_syntax(s,view):NULL;
    if(!syntax)return XR_XIR_UNRESOLVED;
    SourceTypeHintsBuilder builder={0};
    for(uint32_t i=0;i<view->declaration_count;++i) {
        status=lsp_work(s,sizeof(XrXirSourceDeclarationSyntax));if(status!=XR_XIR_OK)goto done;
        const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[i];
        if(!(fact->flags&XR_XIR_SOURCE_SYNTAX_INFERRED_BINDING))continue;
        bool matches=false;status=lsp_text_has_module(s,text,fact->name.module,&matches);
        if(status!=XR_XIR_OK)goto done;if(!matches)continue;
        const XrXirSourceDeclaration *declaration=&view->declarations[i];
        if(declaration->kind!=XR_XIR_SOURCE_BINDING||fact->role!=XR_XIR_SOURCE_SYNTAX_NAME){status=XR_XIR_BAD_STRUCTURE;goto done;}
        status=lsp_name_selection(s,text,fact->name,declaration->name);if(status!=XR_XIR_OK)goto done;
        XrLspPosition position={0};int line=fact->name.end_line,column=fact->name.end_column;
        status=lsp_position(s,text,false,&position,&line,&column);if(status!=XR_XIR_OK)goto done;
        if(type_hint_position_before(position,range.start)||!type_hint_position_before(position,range.end))continue;
        status=type_hint_append(s,declaration,position,&builder);if(status!=XR_XIR_OK)goto done;
    }
    *out=builder.hints;builder.hints=(XlspSourceTypeHints){0};
done:
    xlsp_source_type_hints_free(&builder.hints);return status;
}
