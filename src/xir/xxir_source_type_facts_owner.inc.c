/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_type_facts_owner.inc.c - Validate and own declaration table observations
 */
static bool query_type_literal_same(SourceQueryCopy *copy,XrXirLiteral literal,const char *text) {
    size_t length=query_length(copy,text);
    return literal.bytes&&length&&length-1==literal.length&&((copy)->status == XR_XIR_OK)&&
        !memcmp(literal.bytes,text,literal.length);
}
static bool query_type_contains(XrXirSourceRange outer,XrXirSourceRange inner) {
    return outer.module==inner.module&&
        (inner.line>outer.line||(inner.line==outer.line&&inner.column>=outer.column))&&
        (inner.end_line<outer.end_line||(inner.end_line==outer.end_line&&inner.end_column<=outer.end_column));
}
static XrXirStatus query_type_identity(SourceQueryCopy *copy,const XrXirSourceTypeFact *fact,uint32_t declaration) {
    const XrXirSourceView *view=&copy->snapshot->view;
    const XrXirSourceDeclaration *source=&view->declarations[declaration];
    if((unsigned)fact->kind>XR_XIR_SOURCE_TYPE_INTERFACE)return XR_XIR_BAD_STRUCTURE;
    if(fact->kind==XR_XIR_SOURCE_TYPE_NONE)return fact->table_index||fact->range_known?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    if(source->kind!=XR_XIR_SOURCE_TYPE||source->range.module>=view->module_count||!view->types)return XR_XIR_BAD_STRUCTURE;
    XrXirLiteral module={0},name={0};
    if(fact->kind==XR_XIR_SOURCE_TYPE_INTERFACE){const XrXirInterfaceTable *table=view->types->interfaces;
        if(!table||!table->declarations||fact->table_index>=table->count)return XR_XIR_BAD_STRUCTURE;
        module=table->declarations[fact->table_index].module;name=table->declarations[fact->table_index].name;
    }else{const XrXirNominalTable *table=view->types->nominals;
        if(!table||!table->declarations||fact->table_index>=table->count)return XR_XIR_BAD_STRUCTURE;
        const XrXirNominalDeclaration *nominal=&table->declarations[fact->table_index];
        uint32_t kind=fact->kind==XR_XIR_SOURCE_TYPE_CLASS?XR_XIR_NOMINAL_CLASS:
            fact->kind==XR_XIR_SOURCE_TYPE_ENUM?XR_XIR_NOMINAL_ENUM:XR_XIR_NOMINAL_STRUCT;
        if(nominal->kind!=kind)return XR_XIR_BAD_STRUCTURE;module=nominal->module;name=nominal->name;}
    bool same=query_type_literal_same(copy,module,view->modules[source->range.module].identity)&&
        query_type_literal_same(copy,name,source->name);
    return copy->status!=XR_XIR_OK?copy->status:same?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus query_type_facts_validate(SourceQueryCopy *copy,const XrXirSourceTypeFacts *facts) {
    const XrXirSourceView *view=&copy->snapshot->view;
    uint32_t nominal_count=view->types&&view->types->nominals?view->types->nominals->count:0;
    uint32_t interface_count=view->types&&view->types->interfaces?view->types->interfaces->count:0;
    uint8_t *nominals=nominal_count?xir_compile_calloc(&copy->snapshot->context,nominal_count,1,&copy->status):NULL;
    uint8_t *interfaces=interface_count&&copy->status==XR_XIR_OK?
        xir_compile_calloc(&copy->snapshot->context,interface_count,1,&copy->status):NULL;
    XrXirStatus status=copy->status;
    for(uint32_t i=0;i<facts->declaration_count&&status==XR_XIR_OK;++i){
        if(!((copy)->status == XR_XIR_OK)){status=copy->status;break;}
        const XrXirSourceTypeFact *fact=&facts->declarations[i];XrXirSourceRange range=fact->range;
        status=query_type_identity(copy,fact,i);if(status!=XR_XIR_OK)break;
        if(fact->kind!=XR_XIR_SOURCE_TYPE_NONE){uint8_t *seen=fact->kind==XR_XIR_SOURCE_TYPE_INTERFACE?interfaces:nominals;
            if(seen[fact->table_index]){status=XR_XIR_BAD_STRUCTURE;break;}seen[fact->table_index]=1;}
        if(!fact->range_known){if(range.module||range.line||range.column||range.end_line||range.end_column)status=XR_XIR_BAD_STRUCTURE;continue;}
        const XrXirSourceDeclarationSyntax *syntax=&copy->snapshot->syntax.declarations[i];
        if(syntax->role!=XR_XIR_SOURCE_SYNTAX_NAME||range.line<=0||range.column<=0||range.end_line<range.line||range.end_column<=0||
            (range.end_line==range.line&&range.end_column<=range.column)||range.module!=view->declarations[i].range.module||
            !query_type_contains(range,syntax->name))status=XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_resources_free(nominals);xr_compile_resources_free(interfaces);return status;
}
XR_FUNC XrXirStatus xr_xir_compile_source_snapshot_type_facts_copy(XrXirSourceSnapshot *snapshot,const XrXirSourceTypeFacts *facts) {
    if(!snapshot||!facts||snapshot->type_facts_ready||!snapshot->syntax_ready||!snapshot->view.complete||
        facts->declaration_count!=snapshot->view.declaration_count|| (!!facts->declarations!=!!facts->declaration_count))return XR_XIR_BAD_STRUCTURE;
    SourceQueryCopy copy={snapshot,XR_XIR_OK};XrXirStatus status=query_type_facts_validate(&copy,facts);
    if(status!=XR_XIR_OK)return status;
    SourceQueryMemory *previous=snapshot->memory;XrXirSourceTypeFacts owned=*facts;
    owned.declarations=query_copy(&copy,facts->declarations,facts->declaration_count,sizeof(*facts->declarations));
    (void)((&copy)->status == XR_XIR_OK);
    if(copy.status!=XR_XIR_OK){while(snapshot->memory!=previous){SourceQueryMemory *next=snapshot->memory->next;
        xr_compile_resources_free(snapshot->memory);snapshot->memory=next;}return copy.status;}
    snapshot->type_facts=owned;snapshot->type_facts_ready=true;return XR_XIR_OK;
}
XR_FUNC const XrXirSourceTypeFacts *xr_xir_compile_source_snapshot_type_facts(const XrXirSourceSnapshot *snapshot) {
    return snapshot&&snapshot->type_facts_ready?&snapshot->type_facts:NULL;
}
