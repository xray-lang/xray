/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_submission_bindings.inc.c - Source declarations to actual birth storage
 */
static XrXirStatus submission_compile_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : status==XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET :
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus submission_binding_mismatch(SubmissionBindingDraft *draft,const char *message) {
    draft->failure=message;return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus submission_reader_key(XrSourceSubmission *owner,uint64_t source,uint32_t reader,uint8_t key[16]) {
    XrXirStatus status=submission_compile_status(xr_compile_state_work(owner->state,16));
    if(status!=XR_XIR_OK)return status;
    /* An embedded NUL separates internal selectors from Source identifiers.
     * The serial and Checked ordinal select a copied binding, never authority. */
    key[0]=0;key[1]='r';key[2]='d';key[3]='r';
    for(unsigned i=0;i<8;++i)key[i+4]=(uint8_t)(source>>(i*8));
    for(unsigned i=0;i<4;++i)key[i+12]=(uint8_t)(reader>>(i*8));
    return XR_XIR_OK;
}
static XrXirStatus submission_receive_reader(SubmissionBindingDraft *draft,
    const XrXirSpecializationMapView *mapping,const XrXirSourceGenerationDefinition *definition) {
    if(definition->reader==UINT32_MAX)return XR_XIR_OK;
    const XrXirModule *closed=xr_xir_compile_artifact_module(mapping->closed);
    if(definition->reader>=mapping->function_count)return submission_binding_mismatch(draft,"Binding reader is outside its actual Checked owner");
    uint32_t physical=mapping->identity ? definition->reader : mapping->functions[definition->reader];
    if(physical==UINT32_MAX || physical>=closed->function_count ||
        closed->declarations->functions[physical].module!=definition->module ||
        !closed->declarations->functions[physical].exported ||
        !(closed->declarations->functions[physical].promises&XR_XIR_FUNCTION_NO_SUSPEND) ||
        closed->functions[physical].parameter_count)
        return submission_binding_mismatch(draft,"Binding reader has no admitted ordinary zero-argument entry");
    uint8_t *key=draft->reader_keys+(size_t)draft->readers*16;
    XrXirStatus status=submission_reader_key(draft->owner,definition->generation,definition->reader,key);
    if(status==XR_XIR_OK) {
        ++draft->readers;
        draft->rows[draft->count++]=(XrXirInstanceBindingSpec){(const char *)key,16,XR_XIR_INSTANCE_BINDING_FUNCTION,physical};
    }
    return status;
}
static XrXirStatus submission_receive_bindings(void *context,const XrXirSpecializationMapView *mapping) {
    SubmissionBindingDraft *draft=context;XrSourceSubmission *owner=draft->owner;
    draft->received=true;
    const XrXirModule *source=xr_xir_compile_artifact_module(mapping->source);
    const XrXirModule *closed=xr_xir_compile_artifact_module(mapping->closed);
    if(mapping->source!=draft->checked || !source || !closed || !source->declarations || !closed->declarations ||
        mapping->function_count!=source->function_count || (!mapping->identity && !mapping->functions) ||
        mapping->slot_count!=source->declarations->slot_count || mapping->slot_count!=closed->declarations->slot_count ||
        source->declarations->root_module!=closed->declarations->root_module)
        return submission_binding_mismatch(draft,"Specialization source or declaration shape differs");
    size_t capacity=0;
    for(size_t i=0;i<draft->name_count;++i) {
        if(xr_compile_state_work(owner->state,1)!=XR_COMPILE_RESOURCE_OK)
            return submission_compile_status(xr_compile_state_status(owner->state));
        const XrXirSourceDeclaration *declaration=draft->names[i].declaration;
        if(draft->names[i].generation==owner->source_serial && declaration &&
            (declaration->kind==XR_XIR_SOURCE_FUNCTION || declaration->kind==XR_XIR_SOURCE_BINDING))
            capacity+=declaration->kind==XR_XIR_SOURCE_BINDING ? 2 : 1;
    }
    if(capacity>UINT32_MAX)return XR_XIR_BUDGET;
    XrXirStatus status=capacity ? submission_compile_status(xr_compile_state_calloc(owner->state,
        capacity,sizeof(*draft->rows)+16,(void **)&draft->rows)) : XR_XIR_OK;
    if(status!=XR_XIR_OK)return status;
    draft->reader_keys=capacity ? (uint8_t *)(draft->rows+capacity) : NULL;
    for(size_t i=0;i<draft->name_count;++i) {
        if(xr_compile_state_work(owner->state,1)!=XR_COMPILE_RESOURCE_OK)
            return submission_compile_status(xr_compile_state_status(owner->state));
        const XrXirSourceGenerationBinding *name=&draft->names[i];
        if(name->generation!=owner->source_serial)continue;
        if(!name->declaration || !name->declaration->name)return XR_XIR_BAD_STRUCTURE;
        if(name->declaration->kind!=XR_XIR_SOURCE_FUNCTION && name->declaration->kind!=XR_XIR_SOURCE_BINDING)continue;
        XrXirSourceGenerationDefinition definition={0};
        status=xr_xir_compile_source_generation_definition(owner->source,owner->source_serial,
            name->declaration->name,&definition);
        if(status!=XR_XIR_OK){draft->failure="Source name has no actual semantic declaration";return status;}
        if(definition.checked!=mapping->source || definition.module!=source->declarations->root_module)
            return submission_binding_mismatch(draft,"Source definition belongs to a different Checked root");
        uint32_t ordinal=definition.ordinal,kind=0;
        if(definition.kind==XR_XIR_SOURCE_DEFINITION_BINDING) {
            if(ordinal>=mapping->slot_count || source->declarations->slots[ordinal].module!=definition.module ||
                closed->declarations->slots[ordinal].module!=definition.module ||
                source->declarations->slots[ordinal].type!=definition.stored_type ||
                closed->declarations->slots[ordinal].mutable!=(uint32_t)definition.mutable)
                return submission_binding_mismatch(draft,"Source slot has a different semantic identity");
            kind=XR_XIR_INSTANCE_BINDING_SLOT;
        } else if(definition.kind==XR_XIR_SOURCE_DEFINITION_FUNCTION) {
            if(ordinal>=mapping->function_count)return submission_binding_mismatch(draft,"Source function is outside its actual origin map");
            /* A generic template has no ordinary physical entry. Its complete
             * Checked declaration remains available to later compilation. */
            ordinal=mapping->identity ? ordinal : mapping->functions[ordinal];
            if(ordinal==UINT32_MAX)continue;
            if(ordinal>=closed->function_count)return submission_binding_mismatch(draft,"Ordinary function is outside its actual closed owner");
            const XrXirFunctionIdentity *identity=&closed->declarations->functions[ordinal];
            if(identity->module!=definition.module)return submission_binding_mismatch(draft,"Ordinary function has a different module owner");
            if(identity->cleanup_owner || identity->test_role ||
                ordinal==closed->declarations->modules[definition.module].initializer)continue;
            kind=XR_XIR_INSTANCE_BINDING_FUNCTION;
        } else if(definition.kind!=XR_XIR_SOURCE_DEFINITION_UNIT)continue;
        if(kind) {
            size_t length=0;
            status=submission_compile_status(xr_compile_state_string_length(owner->state,definition.name,&length));
            if(status!=XR_XIR_OK)return status;
            if(!length || length>UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
            draft->rows[draft->count++]=(XrXirInstanceBindingSpec){definition.name,(uint32_t)length,kind,ordinal};
        }
        status=submission_receive_reader(draft,mapping,&definition);
        if(status!=XR_XIR_OK)return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus submission_lowered_bindings(const SubmissionBindingDraft *draft,
    const XrXirArtifact *closed,const XrXirArtifact *lowered) {
    const XrXirModule *a=xr_xir_compile_artifact_module(closed),*b=xr_xir_compile_artifact_module(lowered);
    const XrXirDeclarations *x=a ? a->declarations : NULL,*y=b ? b->declarations : NULL;
    if(!x || !y || a->function_count!=b->function_count || x->slot_count!=y->slot_count ||
        x->root_module!=y->root_module || x->module_count!=y->module_count ||
        x->entry_function!=y->entry_function)return XR_XIR_BAD_STRUCTURE;
    /* The actual lower producer preserves ordinals. Match bound declaration
     * shapes before transfer; the Program receiver still decides permissions. */
    for(uint32_t i=0;i<draft->count;++i) {
        if(xr_compile_state_work(draft->owner->state,1)!=XR_COMPILE_RESOURCE_OK)
            return submission_compile_status(xr_compile_state_status(draft->owner->state));
        const XrXirInstanceBindingSpec *row=&draft->rows[i];uint32_t index=row->ordinal;
        if(row->kind==XR_XIR_INSTANCE_BINDING_SLOT) {
            if(index>=x->slot_count || x->slots[index].module!=y->slots[index].module ||
                x->slots[index].type!=y->slots[index].type || x->slots[index].mutable!=y->slots[index].mutable)
                return XR_XIR_BAD_STRUCTURE;
        } else {
            if(index>=a->function_count || x->functions[index].module!=y->functions[index].module ||
                a->functions[index].result!=b->functions[index].result ||
                a->functions[index].parameter_count!=b->functions[index].parameter_count)return XR_XIR_BAD_STRUCTURE;
            for(uint32_t p=0;p<a->functions[index].parameter_count;++p) {
                if(xr_compile_state_work(draft->owner->state,1)!=XR_COMPILE_RESOURCE_OK)
                    return submission_compile_status(xr_compile_state_status(draft->owner->state));
                if(a->functions[index].parameters[p]!=b->functions[index].parameters[p])return XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    return XR_XIR_OK;
}
