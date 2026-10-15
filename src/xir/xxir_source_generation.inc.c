/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_generation.inc.c - Exact Source records and allocation-free publication
 */
#include "xxir_source_generation.h"
typedef struct SourceGenerationRecord {
    struct SourceGenerationRecord *previous;
    XrXirSourceGenerationView view;
    XrXirSourceResult result;
    SourceInputCapture capture;
    SourceHistoryUnit unit;
    XrXirSourceGenerationBinding *bindings;
    size_t binding_count;
    char *source, *namespace_id;
} SourceGenerationRecord;
struct XrXirSourceGenerations {
    XrXirSourceGenerationConfig config;
    char *namespace_id, *stdlib_path;
    size_t namespace_length, binding_count;
    uint64_t serial;
    SourceGenerationRecord *published, *pending;
    XrXirSourceGenerationBinding *bindings;
};
static XrXirStatus generation_allocate(const XrXirCompileContext *context,
    size_t count, size_t size, void **output) {
    return xir_compile_resource_status(xr_compile_resources_calloc(context->resources,count,size,output));
}
static XrXirStatus generation_string(const XrXirCompileContext *context,
    const char *input, char **output, size_t *length) {
    if (!input) return XR_XIR_BAD_STRUCTURE;
    size_t count=0;
    for (;;) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (!input[count]) break;
        if (count==SIZE_MAX-1) return XR_XIR_BUDGET;
        ++count;
    }
    char *copy=NULL;
    XrXirStatus status=generation_allocate(context,count+1,1,(void **)&copy);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(context,count+1)) { xr_compile_resources_free(copy);return XR_XIR_BUDGET; }
    memcpy(copy,input,count+1);*output=copy;
    if (length) *length=count;
    return XR_XIR_OK;
}
static void generation_record_free(SourceGenerationRecord *record) {
    if (!record) return;
    source_semantic_free(record->capture.semantic);
    xr_xir_compile_source_result_free(&record->result);
    xr_compile_resources_free(record->bindings);
    xr_compile_resources_free(record->namespace_id);
    xr_compile_resources_free(record->source);
    xr_compile_resources_free(record);
}
XR_FUNC void xr_xir_compile_source_generations_free(XrXirSourceGenerations *owner) {
    if (!owner) return;
    generation_record_free(owner->pending);
    SourceGenerationRecord *record=owner->published;
    while (record) {
        SourceGenerationRecord *previous=record->previous;
        generation_record_free(record);record=previous;
    }
    xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner->stdlib_path);
    xr_compile_resources_free(owner->namespace_id);
    xr_compile_resources_free(owner);
}
XR_FUNC XrXirStatus xr_xir_compile_source_generations_new(
    const XrXirSourceGenerationConfig *config,XrXirSourceGenerations **output) {
    if (!config || !xir_compile_context_valid(&config->context) || !config->namespace_id || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    XrXirSourceGenerations *owner=NULL;
    XrXirStatus status=generation_allocate(&config->context,1,sizeof(*owner),(void **)&owner);
    if (status!=XR_XIR_OK) return status;
    owner->config=*config;
    status=generation_string(&config->context,config->namespace_id,&owner->namespace_id,&owner->namespace_length);
    if (status==XR_XIR_OK && config->stdlib_path)
        status=generation_string(&config->context,config->stdlib_path,&owner->stdlib_path,NULL);
    if (status==XR_XIR_OK) {
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,owner->namespace_id,NULL};
        char *identity=NULL;
        XrModuleStatus identity_status=xr_compile_module_identity_from_logical(
            config->context.resources,&authority,NULL,&identity);
        if (identity_status!=XR_MODULE_OK) {
            SourceContext failure={0};failure.compile=config->context;
            source_module_fail(&failure,NULL,identity_status,"source generation namespace is invalid");
            status=failure.diagnostic.status;
        }
        xr_compile_resources_free(identity);
    }
    if (status!=XR_XIR_OK) { xr_xir_compile_source_generations_free(owner);return status; }
    owner->config.namespace_id=owner->namespace_id;owner->config.stdlib_path=owner->stdlib_path;
    *output=owner;return XR_XIR_OK;
}
static bool generation_root_declaration(SourceContext *compare,const SourceGenerationRecord *record,uint32_t index) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(record->result.snapshot);
    const XrXirSourceDeclaration *declaration=&view->declarations[index];
    uint32_t module=record->capture.module;
    if(declaration->parent || declaration->range.module!=module || declaration->range.line<=0 ||
        (declaration->kind!=XR_XIR_SOURCE_FUNCTION && declaration->kind!=XR_XIR_SOURCE_BINDING &&
         declaration->kind!=XR_XIR_SOURCE_TYPE && declaration->kind!=XR_XIR_SOURCE_IMPORT))return false;
    /* Anonymous implementation bodies can have a root query range. Only an
     * actual registered root name participates in the lexical environment. */
    const SourceSemanticEnvironment *semantic=record->capture.semantic;
    if(!semantic || module>=semantic->module_count)return false;
    const SourceName *symbol=find_name(compare,semantic->names[module],declaration->name);
    return symbol && symbol->declaration==index+1;
}
static XrXirStatus generation_bindings(XrXirSourceGenerations *owner,SourceGenerationRecord *record) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(record->result.snapshot);
    size_t added=0;SourceContext compare={0};compare.compile=owner->config.context;
    for (uint32_t i=0;i<view->declaration_count;++i) {
        if (!xir_compile_work(&owner->config.context,1)) return XR_XIR_BUDGET;
        if (generation_root_declaration(&compare,record,i)) ++added;
        if (compare.diagnostic.status!=XR_XIR_OK) return compare.diagnostic.status;
    }
    if (added>SIZE_MAX-owner->binding_count) return XR_XIR_BUDGET;
    size_t capacity=owner->binding_count+added;
    XrXirStatus status=capacity ? generation_allocate(&owner->config.context,capacity,
        sizeof(*record->bindings),(void **)&record->bindings) : XR_XIR_OK;
    if (status!=XR_XIR_OK) return status;
    if (owner->binding_count) {
        size_t bytes=owner->binding_count*sizeof(*owner->bindings);
        if (!xir_compile_work(&owner->config.context,bytes)) return XR_XIR_BUDGET;
        memcpy(record->bindings,owner->bindings,bytes);
    }
    record->binding_count=owner->binding_count;
    for (uint32_t i=0;i<view->declaration_count;++i) {
        if (!xir_compile_work(&owner->config.context,1)) return XR_XIR_BUDGET;
        const XrXirSourceDeclaration *declaration=&view->declarations[i];
        if (!generation_root_declaration(&compare,record,i)) {
            if (compare.diagnostic.status!=XR_XIR_OK) return compare.diagnostic.status;
            continue;
        }
        size_t index=0;
        while (index<record->binding_count &&
            !source_text_same(&compare,NULL,record->bindings[index].declaration->name,declaration->name)) {
            if (compare.diagnostic.status!=XR_XIR_OK) return compare.diagnostic.status;
            ++index;
        }
        if (compare.diagnostic.status!=XR_XIR_OK) return compare.diagnostic.status;
        if (index==record->binding_count) ++record->binding_count;
        if (!xir_compile_work(&owner->config.context,sizeof(*record->bindings))) return XR_XIR_BUDGET;
        record->bindings[index]=(XrXirSourceGenerationBinding){record->view.serial,declaration};
    }
    return XR_XIR_OK;
}
static XrXirStatus generation_source(XrXirSourceGenerations *owner,const XrXirSourceText *text,
    SourceGenerationRecord *record) {
    if (text->length==SIZE_MAX || owner->namespace_length>SIZE_MAX-(sizeof("gen---")+40)) return XR_XIR_BUDGET;
    XrXirStatus status=generation_allocate(&owner->config.context,text->length+1,1,(void **)&record->source);
    if (status!=XR_XIR_OK) return status;
    for (size_t i=0;i<text->length;++i) {
        if (!xir_compile_work(&owner->config.context,1)) return XR_XIR_BUDGET;
        if (!text->text[i]) return XR_XIR_BAD_STRUCTURE;
    }
    if (!xir_compile_work(&owner->config.context,text->length+1)) return XR_XIR_BUDGET;
    memcpy(record->source,text->text,text->length);record->source[text->length]=0;
    size_t capacity=owner->namespace_length+sizeof("gen---")+40;
    status=generation_allocate(&owner->config.context,capacity,1,(void **)&record->namespace_id);
    if (status!=XR_XIR_OK) return status;
    SourceContext format={0};format.compile=owner->config.context;
    if (source_format(&format,record->namespace_id,capacity,"gen-%zu-%s-%llu",owner->namespace_length,owner->namespace_id,
        (unsigned long long)record->view.serial)!=XR_DIAG_OK) return XR_XIR_BUDGET;
    record->view.source=record->source;record->view.length=text->length;
    return XR_XIR_OK;
}
static XrXirStatus generation_prepare(XrXirSourceGenerations *owner,
    const XrXirSourceText *text,uint64_t *serial,XrXirSourceDiagnostic *diagnostic,bool readers) {
    if (!owner || !text || !text->text || (text->logical_path && *text->logical_path) || !serial || *serial)
        return XR_XIR_BAD_STRUCTURE;
    if (owner->pending) return XR_XIR_BAD_STAGE;
    if (owner->serial==UINT64_MAX) return XR_XIR_BUDGET;
    uint64_t reserved=++owner->serial;
    SourceGenerationRecord *record=NULL;
    XrXirStatus status=generation_allocate(&owner->config.context,1,sizeof(*record),(void **)&record);
    XrXirSourceDiagnostic location={0};XrCompilerSession *session=NULL;
    if (status!=XR_XIR_OK) goto done;
    record->view.serial=reserved;record->view.parent=owner->published ? owner->published->view.serial : 0;
    record->unit.owner=owner;record->unit.serial=reserved;
    status=generation_source(owner,text,record);
    if (status!=XR_XIR_OK) goto done;
    XrCompilerSessionStatus opened=xr_compile_session_new(owner->config.context.resources,&session);
    if (opened!=XR_COMPILER_SESSION_OK) {
        status=opened==XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            opened==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;goto done;
    }
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,record->namespace_id,NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner->config.context,owner->config.stdlib_path,
        owner->config.lockfile,XR_XIR_PROGRAM,owner->config.libraries};
    SourceInput input={.supplied=true,.text={NULL,record->source,text->length},.capture=&record->capture,.binding_readers=readers,
        .history=owner->published ? &owner->published->unit : NULL,.self=&record->unit};
    status=source_check_input(&request,input,&record->result,&location,NULL);
    if (status!=XR_XIR_OK) goto done;
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(record->result.snapshot);
    if (!record->capture.ast || !view || !view->complete || record->capture.module>=view->module_count) {
        status=XR_XIR_BAD_STRUCTURE;goto done;
    }
    record->view.ast=record->capture.ast;record->view.result=&record->result;
    record->unit.semantic=record->capture.semantic;record->unit.checked=record->result.checked;
    record->unit.entry_module=record->capture.module;
    record->view.identity=view->modules[record->capture.module].identity;
    record->view.fingerprint=view->modules[record->capture.module].fingerprint;
    status=generation_bindings(owner,record);
done:
    xr_compile_session_free(session);
    if (diagnostic) { *diagnostic=location;diagnostic->status=status; }
    if (status!=XR_XIR_OK) { generation_record_free(record);return status; }
    owner->pending=record;*serial=reserved;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare(XrXirSourceGenerations *owner,
    const XrXirSourceText *text,uint64_t *serial,XrXirSourceDiagnostic *diagnostic) {
    return generation_prepare(owner,text,serial,diagnostic,false);
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare_bindings(XrXirSourceGenerations *owner,
    const XrXirSourceText *text,uint64_t *serial,XrXirSourceDiagnostic *diagnostic) {
    return generation_prepare(owner,text,serial,diagnostic,true);
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_publish(XrXirSourceGenerations *owner,uint64_t serial) {
    if (!owner) return XR_XIR_BAD_STRUCTURE;
    SourceGenerationRecord *record=owner->pending;
    if (!serial || !record || record->view.serial!=serial) return XR_XIR_BAD_STAGE;
    XrXirSourceGenerationBinding *previous=owner->bindings;
    owner->bindings=record->bindings;owner->binding_count=record->binding_count;
    record->bindings=NULL;record->binding_count=0;
    record->view.published=true;record->previous=owner->published;
    record->unit.published=true;
    owner->published=record;owner->pending=NULL;
    xr_compile_resources_free(previous);return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_abort(XrXirSourceGenerations *owner,uint64_t serial) {
    if (!owner) return XR_XIR_BAD_STRUCTURE;
    SourceGenerationRecord *record=owner->pending;
    if (!serial || !record || record->view.serial!=serial) return XR_XIR_BAD_STAGE;
    owner->pending=NULL;generation_record_free(record);return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_view(const XrXirSourceGenerations *owner,
    uint64_t serial,XrXirSourceGenerationView *output) {
    if (!owner || !serial || !output) return XR_XIR_BAD_STRUCTURE;
    const SourceGenerationRecord *record=owner->pending;
    if (record && record->view.serial==serial) { *output=record->view;return XR_XIR_OK; }
    for (record=owner->published;record;record=record->previous) {
        if (!xir_compile_work(&owner->config.context,1)) return XR_XIR_BUDGET;
        if (record->view.serial==serial) { *output=record->view;return XR_XIR_OK; }
    }
    return XR_XIR_BAD_STAGE;
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_lookup(const XrXirSourceGenerations *owner,
    const char *name,XrXirSourceGenerationBinding *output) {
    if (!owner || !name || !output) return XR_XIR_BAD_STRUCTURE;
    SourceContext compare={0};compare.compile=owner->config.context;
    for (size_t i=0;i<owner->binding_count;++i) {
        if (source_text_same(&compare,NULL,owner->bindings[i].declaration->name,name)) {
            *output=owner->bindings[i];return XR_XIR_OK;
        }
        if (compare.diagnostic.status!=XR_XIR_OK) return compare.diagnostic.status;
    }
    return XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_definition(const XrXirSourceGenerations *owner,
    uint64_t serial,const char *name,XrXirSourceGenerationDefinition *output) {
    if (!owner || !serial || !name || !output) return XR_XIR_BAD_STRUCTURE;
    const SourceGenerationRecord *record=owner->pending;
    if (!record || record->view.serial!=serial) {
        record=owner->published;
        while (record && record->view.serial!=serial) {
            if (!xir_compile_work(&owner->config.context,1)) return XR_XIR_BUDGET;
            record=record->previous;
        }
    }
    if (!record) return XR_XIR_BAD_STAGE;
    const SourceSemanticEnvironment *environment=record->capture.semantic;
    uint32_t module=record->capture.module;
    if (!environment || module>=environment->module_count) return XR_XIR_BAD_STRUCTURE;
    SourceContext search={0};search.compile=owner->config.context;
    const SourceName *symbol=find_name(&search,environment->names[module],name);
    if (search.diagnostic.status!=XR_XIR_OK) return search.diagnostic.status;
    if (!symbol) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *checked=xr_xir_compile_artifact_module(record->result.checked);
    if (!checked || !checked->declarations || module>=checked->declarations->module_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirSourceGenerationDefinition definition={.generation=serial,.module=module,
        .ordinal=symbol->index,.dependency=UINT32_MAX,.reader=UINT32_MAX,.name=symbol->name,.syntax=symbol->node,
        .checked=record->result.checked,.mutable=symbol->mutable};
    switch (symbol->kind) {
    case SOURCE_UNIT_SLOT:
        if (!symbol->mutable) {
            definition.kind=XR_XIR_SOURCE_DEFINITION_UNIT;definition.ordinal=UINT32_MAX;break;
        }
        /* A mutable Unit still owns an actual Cell slot. */
        /* fall through */
    case SOURCE_SLOT: {
        if (symbol->index>=checked->declarations->slot_count ||
            checked->declarations->slots[symbol->index].module!=module ||
            checked->declarations->slots[symbol->index].mutable!=(uint32_t)symbol->mutable) return XR_XIR_BAD_STRUCTURE;
        XrXirType physical=checked->declarations->slots[symbol->index].type;
        if (symbol->mutable ? !xr_xir_type_is_cell(checked->types,physical) ||
            xr_xir_cell_element(checked->types,physical)!=symbol->type : physical!=symbol->type)
            return XR_XIR_BAD_STRUCTURE;
        definition.kind=XR_XIR_SOURCE_DEFINITION_BINDING;
        definition.logical_type=symbol->type;definition.stored_type=physical;break;
    }
    case SOURCE_FUNCTION:
        if (symbol->index>=checked->function_count || checked->declarations->functions[symbol->index].module!=module)
            return XR_XIR_BAD_STRUCTURE;
        definition.kind=XR_XIR_SOURCE_DEFINITION_FUNCTION;break;
    case SOURCE_NOMINAL:
        if (!checked->types || !checked->types->nominals || symbol->index>=checked->types->nominals->count)
            return XR_XIR_BAD_STRUCTURE;
        definition.kind=XR_XIR_SOURCE_DEFINITION_NOMINAL;break;
    case SOURCE_INTERFACE:
        if (!checked->types || !checked->types->interfaces || symbol->index>=checked->types->interfaces->count)
            return XR_XIR_BAD_STRUCTURE;
        definition.kind=XR_XIR_SOURCE_DEFINITION_INTERFACE;break;
    case SOURCE_MODULE:case SOURCE_IMPORT:
        if (symbol->module>=checked->declarations->module_count) return XR_XIR_BAD_STRUCTURE;
        definition.kind=symbol->kind==SOURCE_MODULE ? XR_XIR_SOURCE_DEFINITION_MODULE : XR_XIR_SOURCE_DEFINITION_IMPORT;
        definition.ordinal=UINT32_MAX;definition.dependency=symbol->module;definition.imported=symbol->imported;
        if (symbol->module<(uint32_t)environment->graph->spec_count)
            definition.dependency_syntax=environment->graph->specs[symbol->module].ast;
        break;
    default:return XR_XIR_BAD_STRUCTURE;
    }
    if((symbol->kind==SOURCE_SLOT || symbol->kind==SOURCE_UNIT_SLOT) && environment->binding_reads) {
        if(symbol->index>=environment->slot_count || !environment->binding_reads[symbol->index])return XR_XIR_BAD_STRUCTURE;
        uint32_t reader=environment->binding_reads[symbol->index]-1;
        if(reader>=checked->function_count || checked->functions[reader].parameter_count ||
            checked->functions[reader].result!=symbol->type || checked->declarations->functions[reader].module!=module ||
            !checked->declarations->functions[reader].exported ||
            !(checked->declarations->functions[reader].promises&XR_XIR_FUNCTION_NO_SUSPEND))return XR_XIR_BAD_STRUCTURE;
        definition.reader=reader;
    }
    *output=definition;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_candidate_bindings(const XrXirSourceGenerations *owner,
    uint64_t serial,const XrXirSourceGenerationBinding **output,size_t *count) {
    if (!owner || !serial || !output || *output || !count || *count) return XR_XIR_BAD_STRUCTURE;
    const SourceGenerationRecord *record=owner->pending;
    if (!record || record->view.serial!=serial) return XR_XIR_BAD_STAGE;
    if (!xir_compile_work(&owner->config.context,1)) return XR_XIR_BUDGET;
    *output=record->bindings;*count=record->binding_count;return XR_XIR_OK;
}
