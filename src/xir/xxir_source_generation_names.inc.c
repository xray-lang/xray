/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_generation_names.inc.c - Visible names from authentic Source facts
 */
static XrXirStatus generation_birth(const XrXirSourceGenerations *owner,
    const SourceGenerationRecord *pending,uint64_t serial,const SourceGenerationRecord **output) {
    if(pending && pending->view.serial==serial){*output=pending;return XR_XIR_OK;}
    for(const SourceGenerationRecord *record=owner->published;record;record=record->previous) {

        if(record->view.serial==serial){*output=record;return XR_XIR_OK;}
    }
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus generation_display_type(XrXirSourceGenerations *owner,
    const SourceGenerationRecord *pending,XrXirSourceGenerationName *name) {
    const XrXirSourceDeclaration *declaration=name->binding.declaration;
    const SourceGenerationRecord *birth=NULL;
    XrXirStatus found=generation_birth(owner,pending,name->binding.generation,&birth);
    if(found!=XR_XIR_OK)return found;
    const char *literal=declaration->kind==XR_XIR_SOURCE_FUNCTION ? TYPE_NAME_FUNCTION :
        declaration->kind==XR_XIR_SOURCE_TYPE ? declaration->name : NULL;
    if(literal)return generation_string(&owner->config.context,literal,(char **)&name->type_name,NULL);
    if(!declaration->type.known)return XR_XIR_OK;
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(birth->result.snapshot);
    if(!view)return XR_XIR_BAD_STRUCTURE;
    SourceContext context={0};context.compile=owner->config.context;
    if(view->types) {
        context.types=*view->types;
        if(view->types->nominals)context.nominals=*view->types->nominals;
    }
    SourceTypeName text={{0},0,false};
    if(!source_type_name_collect(&context,NULL,declaration->type.type,true,0,&text)) {
        /* Missing display support is an absent observation, not permission to
         * reject a valid input or invent a replacement type name. */
        return context.diagnostic.status==XR_XIR_BUDGET || context.diagnostic.status==XR_XIR_OUT_OF_MEMORY ?
            context.diagnostic.status : XR_XIR_OK;
    }
    if(text.overflow)return XR_XIR_OK;
    char *copy=NULL;XrXirStatus status=generation_allocate(&owner->config.context,text.length+1,1,(void **)&copy);
    if(status!=XR_XIR_OK)return status;

    memcpy(copy,text.bytes,text.length);name->type_name=copy;return XR_XIR_OK;
}
static XrXirStatus generation_names(XrXirSourceGenerations *owner,SourceGenerationRecord *record) {
    if(!record->view.interactive || record->view.type_query)return XR_XIR_OK;
    const SourceSemanticEnvironment *environment=record->capture.semantic;
    size_t count=(size_t)environment->latest_count+(record->implicit_result.declaration?1u:0u);
    if(!count)return XR_XIR_OK;
    XrXirStatus status=generation_allocate(&owner->config.context,count,sizeof(*record->names),(void **)&record->names);
    if(status!=XR_XIR_OK)return status;
    for(size_t i=0;i<count;++i) {

        XrXirSourceGenerationName *name=&record->names[record->name_count++];
        if(i==environment->latest_count) {
            name->binding=record->implicit_result;name->name="it";name->implicit_result=true;
        } else {
            const SourceName *symbol=environment->latest[i];name->name=symbol->name;
            SourceContext compare={0};compare.compile=owner->config.context;
            for(size_t b=0;b<record->binding_count;++b) {
                if(source_text_same(&compare,NULL,name->name,record->bindings[b].declaration->name)) {
                    name->binding=record->bindings[b];break;
                }
                if(compare.diagnostic.status!=XR_XIR_OK)return compare.diagnostic.status;
            }
            if(!name->binding.declaration)return XR_XIR_BAD_STRUCTURE;
        }
        status=generation_display_type(owner,record,name);if(status!=XR_XIR_OK)return status;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_names(const XrXirSourceGenerations *owner,
    uint64_t serial,const XrXirSourceGenerationName **output,size_t *count) {
    if(!owner || !output || *output || !count || *count)return XR_XIR_BAD_STRUCTURE;
    const SourceGenerationRecord *record=serial ? owner->pending : owner->published;
    if(serial && (!record || record->view.serial!=serial))return XR_XIR_BAD_STAGE;
    if(record){*output=record->names;*count=record->name_count;}
    return XR_XIR_OK;
}
