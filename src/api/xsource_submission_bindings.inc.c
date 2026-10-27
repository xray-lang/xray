/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_submission_bindings.inc.c - Source declarations to actual birth storage
 */
static XrXirStatus submission_compile_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : status==XR_COMPILE_RESOURCE_MEMORY_LIMIT || status==XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_BUDGET :
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus submission_binding_mismatch(SubmissionBindingDraft *draft,const char *message) {
    draft->failure=message;return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus submission_accessor_key(uint64_t source,uint32_t accessor,uint8_t key[16]) {
    /* An embedded NUL separates internal selectors from Source identifiers.
     * The serial and Checked ordinal select a copied binding, never authority. */
    key[0]=0;key[1]='a';key[2]='c';key[3]='c';
    for(unsigned i=0;i<8;++i)key[i+4]=(uint8_t)(source>>(i*8));
    for(unsigned i=0;i<4;++i)key[i+12]=(uint8_t)(accessor>>(i*8));
    return XR_XIR_OK;
}
static XrXirStatus submission_receive_accessor(SubmissionBindingDraft *draft,
    const XrXirSpecializationMapView *mapping,const XrXirSourceGenerationDefinition *definition,bool write) {
    uint32_t accessor=write ? definition->writer : definition->reader;
    if(accessor==UINT32_MAX)return XR_XIR_OK;
    const XrXirModule *source=xr_xir_compile_artifact_module(mapping->source);
    const XrXirModule *closed=xr_xir_compile_artifact_module(mapping->closed);
    if(accessor>=mapping->function_count)return submission_binding_mismatch(draft,"Binding accessor is outside its actual Checked owner");
    uint32_t physical=mapping->identity ? accessor : mapping->functions[accessor];
    if(physical==UINT32_MAX || physical>=closed->function_count ||
        closed->declarations->functions[physical].module!=definition->module ||
        !closed->declarations->functions[physical].exported ||
        !(closed->declarations->functions[physical].promises&XR_XIR_FUNCTION_NO_SUSPEND) ||
        closed->functions[physical].parameter_count!=source->functions[accessor].parameter_count ||
        (!write && closed->functions[physical].parameter_count) ||
        (write && (!definition->mutable || closed->functions[physical].result!=XR_XIR_UNIT)))
        return submission_binding_mismatch(draft,"Binding accessor has no admitted ordinary entry");
    if(definition->kind==XR_XIR_SOURCE_DEFINITION_BINDING || definition->kind==XR_XIR_SOURCE_DEFINITION_UNIT) {
        XrXirType logical=XR_XIR_UNIT;
        if(definition->kind==XR_XIR_SOURCE_DEFINITION_BINDING) {
            if(definition->ordinal>=closed->declarations->slot_count)return XR_XIR_BAD_STRUCTURE;
            XrXirType stored=closed->declarations->slots[definition->ordinal].type;
            if(definition->mutable && !xr_xir_type_is_cell(closed->types,stored))return XR_XIR_BAD_STRUCTURE;
            logical=definition->mutable ? xr_xir_cell_element(closed->types,stored) : stored;
        }
        uint32_t parameters=write && logical!=XR_XIR_UNIT ? 1u : 0u;
        const XrXirFunction *function=&closed->functions[physical];
        if(function->result!=(write ? XR_XIR_UNIT : logical) || function->parameter_count!=parameters ||
            (parameters && (!function->parameters || function->parameters[0]!=logical)))
            return submission_binding_mismatch(draft,"Binding accessor differs from its actual closed storage type");
    }
    if(!write && accessor==draft->repl_reader) {
        if(draft->repl_entry!=UINT32_MAX || definition->generation!=draft->owner->source_serial)
            return submission_binding_mismatch(draft,"Interactive result has no unique actual reader");
        draft->repl_entry=physical;
    }
    uint8_t prepared[16];
    XrXirStatus status=submission_accessor_key(definition->generation,accessor,prepared);
    if(status!=XR_XIR_OK)return status;
    for(uint32_t i=0;i<draft->accessors;++i) {

        if(!memcmp(draft->accessor_keys+(size_t)i*16,prepared,16))return XR_XIR_OK;
    }
    uint8_t *key=draft->accessor_keys+(size_t)draft->accessors*16;
    memcpy(key,prepared,16);
    ++draft->accessors;
    draft->rows[draft->count++]=(XrXirInstanceBindingSpec){(const char *)key,16,XR_XIR_INSTANCE_BINDING_FUNCTION,physical};
    return XR_XIR_OK;
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

        const XrXirSourceDeclaration *declaration=draft->names[i].declaration;
        if(draft->names[i].generation==owner->source_serial && declaration &&
            (declaration->kind==XR_XIR_SOURCE_FUNCTION || declaration->kind==XR_XIR_SOURCE_BINDING))
        {
            if(capacity>UINT32_MAX-3u)return XR_XIR_BUDGET;
            capacity+=3;
        }
    }
    if(capacity>UINT32_MAX)return XR_XIR_BUDGET;
    XrXirStatus status=capacity ? submission_compile_status(xr_compile_resources_calloc(owner->resources,
        capacity,sizeof(*draft->rows)+16,(void **)&draft->rows)) : XR_XIR_OK;
    if(status!=XR_XIR_OK)return status;
    draft->accessor_keys=capacity ? (uint8_t *)(draft->rows+capacity) : NULL;
    for(size_t i=0;i<draft->name_count;++i) {

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
            size_t length=strlen(definition.name);
            if(!length || length>UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
            draft->rows[draft->count++]=(XrXirInstanceBindingSpec){definition.name,(uint32_t)length,kind,ordinal};
        }
        status=submission_receive_accessor(draft,mapping,&definition,false);
        if(status==XR_XIR_OK)status=submission_receive_accessor(draft,mapping,&definition,true);
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

                if(a->functions[index].parameters[p]!=b->functions[index].parameters[p])return XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    return XR_XIR_OK;
}
