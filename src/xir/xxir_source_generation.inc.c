/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_generation.inc.c - Exact Source records and allocation-free publication
 */
#include "xxir_source_generation.h"
#include "xxir_instance_generation_internal.h"
#include "xxir_value_internal.h"
#include "xxir_type_arena_internal.h"
#include "xxir_specialization_bindings.h"
typedef struct SourceGenerationRecord {
    struct SourceGenerationRecord *previous;
    XrXirSourceGenerationView view;
    XrXirSourceResult result;
    SourceInputCapture capture;
    SourceHistoryUnit unit;
    XrXirSourceGenerationBinding *bindings;
    size_t binding_count;
    XrXirSourceGenerationBinding implicit_result;
    XrXirSourceGenerationName *names;
    size_t name_count;
    char *source, *namespace_id;
    XrXirSourceAstOwner ast_owner;
    uint32_t *received_functions;
    uint32_t received_function_count;
} SourceGenerationRecord;
struct XrXirSourceGenerations {
    XrXirSourceGenerationConfig config;
    char *namespace_id, *stdlib_path;
    size_t namespace_length, binding_count;
    uint64_t serial;
    SourceGenerationRecord *published, *pending;
    XrXirSourceGenerationBinding *bindings;
    XrXirDomain *domain;
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
    xr_xir_compile_program_drop(record->unit.program);
    source_semantic_free(record->capture.semantic);
    if (record->ast_owner.owner) record->ast_owner.release(record->ast_owner.owner);
    xr_xir_compile_source_result_free(&record->result);
    for(size_t i=0;i<record->name_count;++i)xr_compile_resources_free((void *)record->names[i].type_name);
    xr_compile_resources_free(record->names);
    xr_compile_resources_free(record->bindings);
    xr_compile_resources_free(record->namespace_id);
    xr_compile_resources_free(record->source);
    xr_compile_resources_free(record->received_functions);
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
    xr_xir_domain_drop(owner->domain);
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
    if(declaration->parent || declaration->range.module!=module ||
        (declaration->kind!=XR_XIR_SOURCE_FUNCTION && declaration->kind!=XR_XIR_SOURCE_BINDING &&
         declaration->kind!=XR_XIR_SOURCE_TYPE && declaration->kind!=XR_XIR_SOURCE_IMPORT &&
         declaration->kind!=XR_XIR_SOURCE_MODULE))return false;
    /* Anonymous bodies can have a root query range, while import aliases have
     * no individual token location. Exact root registration, not a nonzero
     * display range, selects the actual lexical declaration. */
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
static XrXirStatus generation_identity(XrXirSourceGenerations *owner,SourceGenerationRecord *record) {
    if (owner->namespace_length>SIZE_MAX-(sizeof("gen---")+40)) return XR_XIR_BUDGET;
    size_t capacity=owner->namespace_length+sizeof("gen---")+40;
    XrXirStatus status=generation_allocate(&owner->config.context,capacity,1,(void **)&record->namespace_id);
    if (status!=XR_XIR_OK) return status;
    SourceContext format={0};format.compile=owner->config.context;
    if (source_format(&format,record->namespace_id,capacity,"gen-%zu-%s-%llu",owner->namespace_length,owner->namespace_id,
        (unsigned long long)record->view.serial)!=XR_DIAG_OK) return XR_XIR_BUDGET;
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
    record->view.source=record->source;record->view.length=text->length;
    return generation_identity(owner,record);
}
static XrXirStatus generation_result_binding(XrXirSourceGenerations *owner,SourceGenerationRecord *record,
    const XrXirSourceView *view) {
    if(!record->unit.interactive)return XR_XIR_OK;
    const SourceName *implicit=record->capture.semantic->implicit_result;
    if(!implicit)return XR_XIR_OK;
    if(implicit->module!=record->capture.module) {
        if(!owner->published || !owner->published->implicit_result.declaration)return XR_XIR_BAD_STRUCTURE;
        record->implicit_result=owner->published->implicit_result;return XR_XIR_OK;
    }
    if(!implicit->declaration || implicit->declaration>view->declaration_count ||
        implicit->kind!=SOURCE_SLOT || implicit->mutable)return XR_XIR_BAD_STRUCTURE;
    SourceContext compare={0};compare.compile=owner->config.context;
    bool registered=generation_root_declaration(&compare,record,implicit->declaration-1);
    if(compare.diagnostic.status!=XR_XIR_OK)return compare.diagnostic.status;
    if(!registered)return XR_XIR_BAD_STRUCTURE;
    record->implicit_result=(XrXirSourceGenerationBinding){record->view.serial,&view->declarations[implicit->declaration-1]};
    record->view.repl_result=record->implicit_result.declaration;return XR_XIR_OK;
}
#include "xxir_source_generation_names.inc.c"
typedef struct GenerationInput {
    const XrXirSourceText *text;
    const XrXirSourceAstOwner *ast;
    const XrXirSourceReplInput *repl;
    bool readers, interactive, type_query;
} GenerationInput;
static XrXirStatus source_generation_prepare(XrXirSourceGenerations *owner,
    const GenerationInput *input,uint64_t *serial,XrXirSourceDiagnostic *diagnostic) {
    if (!owner || !input || ((input->text==NULL)==(input->ast==NULL)) || !serial || *serial)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceText *text=input->text;
    const XrXirSourceAstOwner *ast=input->ast;
    if (text && (!text->text || (text->logical_path && *text->logical_path))) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceReplInput *repl=input->repl;
    bool rooted=repl && repl->authority;
    if (repl && (repl->ast!=ast || !input->interactive || (repl->failure_path && *repl->failure_path) ||
        (rooted ? !repl->absolute_path || !*repl->absolute_path || !repl->logical_path || !*repl->logical_path ||
            repl->authority->kind==XR_MODULE_IDENTITY_MEMORY || !repl->authority->physical_root :
            repl->absolute_path || repl->logical_path || repl->lockfile))) return XR_XIR_BAD_STRUCTURE;
    if (ast && (!ast->input.program || (!rooted && ast->input.logical_path && *ast->input.logical_path) ||
        (rooted && (!ast->input.logical_path || !*ast->input.logical_path)) ||
        ast->resources!=owner->config.context.resources || !ast->owner || !ast->retain || !ast->release))
        return XR_XIR_BAD_STRUCTURE;
    if (owner->pending || (owner->published && owner->published->unit.interactive!=input->interactive))
        return XR_XIR_BAD_STAGE;
    if (owner->serial==UINT64_MAX) return XR_XIR_BUDGET;
    uint64_t reserved=++owner->serial;
    SourceGenerationRecord *record=NULL;
    XrXirStatus status=generation_allocate(&owner->config.context,1,sizeof(*record),(void **)&record);
    XrXirSourceDiagnostic location={0};XrCompilerSession *session=NULL;
    if (status!=XR_XIR_OK) goto done;
    record->view.serial=reserved;record->view.parent=owner->published ? owner->published->view.serial : 0;
    record->unit.owner=owner;record->unit.serial=reserved;record->unit.interactive=input->interactive;
    record->view.interactive=input->interactive;record->view.type_query=input->type_query;
    if (ast) {
        status=ast->retain(ast->owner,&ast->input);
        if (status!=XR_XIR_OK) goto done;
        record->ast_owner=*ast;
        status=generation_identity(owner,record);
    } else status=generation_source(owner,text,record);
    if (status!=XR_XIR_OK) goto done;
    XrCompilerSessionStatus opened=xr_compile_session_new(owner->config.context.resources,&session);
    if (opened!=XR_COMPILER_SESSION_OK) {
        status=opened==XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            opened==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;goto done;
    }
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,record->namespace_id,NULL};
    XrXirSourceRequest request={session,rooted?repl->absolute_path:NULL,rooted?repl->authority:&authority,
        &owner->config.context,owner->config.stdlib_path,rooted?repl->lockfile:owner->config.lockfile,
        XR_XIR_PROGRAM,owner->config.libraries};
    SourceInput source={.supplied=text!=NULL,.text={rooted?repl->logical_path:NULL,record->source,text?text->length:0},
        .ast=ast?ast->input.program:NULL,.ast_input=ast!=NULL,.ast_owner=ast?&record->ast_owner:NULL,
        .capture=&record->capture,.binding_readers=input->readers,.interactive=input->interactive,.type_query=input->type_query,
        .module_scope=input->interactive?record->namespace_id:NULL,
        .history=owner->published ? &owner->published->unit : NULL,.self=&record->unit};
    status=source_check_input(&request,source,&record->result,&location,repl?repl->failure_path:NULL);
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
    if (status==XR_XIR_OK) status=generation_result_binding(owner,record,view);
    if (status==XR_XIR_OK) status=generation_names(owner,record);
done:
    xr_compile_session_free(session);
    if (diagnostic) { *diagnostic=location;diagnostic->status=status; }
    if (status!=XR_XIR_OK) { generation_record_free(record);return status; }
    owner->pending=record;*serial=reserved;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare(XrXirSourceGenerations *owner,
    const XrXirSourceText *text,uint64_t *serial,XrXirSourceDiagnostic *diagnostic) {
    const GenerationInput input={.text=text};
    return source_generation_prepare(owner,&input,serial,diagnostic);
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare_bindings(XrXirSourceGenerations *owner,
    const XrXirSourceText *text,uint64_t *serial,XrXirSourceDiagnostic *diagnostic) {
    const GenerationInput input={.text=text,.readers=true};
    return source_generation_prepare(owner,&input,serial,diagnostic);
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare_ast_bindings(XrXirSourceGenerations *owner,
    const XrXirSourceAstOwner *ast,uint64_t *serial,XrXirSourceDiagnostic *diagnostic) {
    const GenerationInput input={.ast=ast,.readers=true};
    return source_generation_prepare(owner,&input,serial,diagnostic);
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare_repl_ast_bindings(XrXirSourceGenerations *owner,
    const XrXirSourceReplInput *repl,uint64_t *serial,XrXirSourceDiagnostic *diagnostic) {
    const GenerationInput input={.ast=repl?repl->ast:NULL,.repl=repl,.readers=true,.interactive=true};
    return source_generation_prepare(owner,&input,serial,diagnostic);
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_prepare_repl_type_ast_bindings(XrXirSourceGenerations *owner,
    const XrXirSourceReplInput *repl,uint64_t *serial,XrXirSourceDiagnostic *diagnostic) {
    const GenerationInput input={.ast=repl?repl->ast:NULL,.repl=repl,.readers=true,.interactive=true,.type_query=true};
    return source_generation_prepare(owner,&input,serial,diagnostic);
}
/* Common specialization preserves module and slot ordinals. Complete code
 * matching above those tables binds the actual closed pools; no name search
 * or type-shape comparison creates this receiving correspondence. */
static XrXirStatus generation_program_correspondence(const XrXirCompileContext *context,
    const SourceGenerationRecord *record,const XrXirProgram *program) {
    const SourceHistoryCorrespondence *map=&record->capture.semantic->history;
    if (!map->unit) return XR_XIR_OK;
    const SourceHistoryUnit *unit=map->unit;
    if (!unit->published || unit->owner!=record->unit.owner || unit->serial>=record->unit.serial ||
        !unit->program) return XR_XIR_BAD_STAGE;
    const XrXirProgram *prior=unit->program;
    const XrXirModule *before=xr_xir_compile_artifact_module(unit->checked);
    const XrXirModule *after=xr_xir_compile_artifact_module(record->result.checked);
    if (!before || !after || !before->declarations || !after->declarations ||
        !map->modules || (map->slot_count && !map->slots) ||
        !map->functions || (map->type_count && !map->types) ||
        map->module_count!=before->declarations->module_count ||
        map->module_count!=prior->declarations->module_count ||
        map->slot_count!=before->declarations->slot_count || map->slot_count!=prior->declarations->slot_count ||
        map->function_count!=before->function_count ||
        after->declarations->module_count!=program->declarations->module_count ||
        after->declarations->slot_count!=program->declarations->slot_count) return XR_XIR_BAD_STRUCTURE;
    if (prior->permissions->target.architecture!=program->permissions->target.architecture ||
        prior->permissions->target.abi_version!=program->permissions->target.abi_version) return XR_XIR_BAD_LAYOUT;
    for (uint32_t m=0;m<map->module_count;++m) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (map->modules[m]>=after->declarations->module_count ||
            map->modules[m]==program->declarations->root_module) return XR_XIR_BAD_STRUCTURE;
        uint32_t storage=prior->declarations->modules[m].storage==XR_XIR_MODULE_BIRTH_STORAGE ||
            m==prior->declarations->root_module ? XR_XIR_MODULE_BIRTH_STORAGE : XR_XIR_MODULE_FRESH_STORAGE;
        if (program->declarations->modules[map->modules[m]].storage!=storage) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t f=0;f<map->function_count;++f) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (map->functions[f]>=after->function_count) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t s=0;s<map->slot_count;++s) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        uint32_t target=map->slots[s];
        if (target>=after->declarations->slot_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirSlot *from=&before->declarations->slots[s],*to=&after->declarations->slots[target];
        if (from->module>=map->module_count || to->module!=map->modules[from->module] ||
            from->mutable!=to->mutable) return XR_XIR_BAD_STRUCTURE;
        XrXirType type=from->type;
        if ((uint32_t)type>=XR_XIR_TYPE_PARAMETER_BASE) return XR_XIR_BAD_TYPE;
        if ((uint32_t)type>=XR_XIR_CONSTRUCTED_TYPE_BASE) {
            uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
            if (index>=map->type_count) return XR_XIR_BAD_TYPE;
            type=map->types[index];
        }
        if (type!=to->type) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
#include "xxir_source_generation_functions.inc.c"
static XrXirStatus generation_type_identities(const XrXirCompileContext *context,
    const SourceGenerationRecord *record,XrXirProgram *program) {
    const SourceHistoryCorrespondence *map=&record->capture.semantic->history;
    if (!map->unit) return XR_XIR_OK;
    uint32_t count=0;
    for (uint32_t i=0;i<map->slot_count;++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        XrXirType from=map->unit->program->declarations->slots[i].type;
        XrXirType to=program->declarations->slots[map->slots[i]].type;
        if (from==XR_XIR_ERROR || (uint32_t)from>=XR_XIR_CONSTRUCTED_TYPE_BASE) ++count;
        else if (from!=to) return XR_XIR_BAD_TYPE;
    }
    for (uint32_t f=0;f<record->received_function_count;++f) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (record->received_functions[f]==UINT32_MAX) continue;
        const XrXirCallEntry *from=&map->unit->program->entries[f];
        const XrXirCallEntry *to=&program->entries[record->received_functions[f]];
        for (uint32_t p=0;p<=from->parameter_count;++p) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            XrXirType a=p ? from->parameters[p-1] : from->result;
            XrXirType b=p ? to->parameters[p-1] : to->result;
            if (a!=XR_XIR_ERROR && (uint32_t)a<XR_XIR_CONSTRUCTED_TYPE_BASE) {
                if (a!=b) return XR_XIR_BAD_TYPE;
            } else {
                if (count==UINT32_MAX) return XR_XIR_BUDGET;
                ++count;
            }
        }
    }
    if (!count) return XR_XIR_OK;
    if (atomic_load_explicit(&program->references,memory_order_acquire)!=2) return XR_XIR_BAD_STAGE;
    XrXirStatus status=XR_XIR_OK;
    XirTypeReceivingRoot *roots=xir_compile_calloc(context,count,sizeof(*roots),&status);
    if (!roots) return status;
    uint32_t at=0;
    for (uint32_t i=0;i<map->slot_count;++i) {
        if (!xir_compile_work(context,sizeof(*roots))) {status=XR_XIR_BUDGET;break;}
        XrXirType from=map->unit->program->declarations->slots[i].type;
        if (from!=XR_XIR_ERROR && (uint32_t)from<XR_XIR_CONSTRUCTED_TYPE_BASE) continue;
        roots[at++]=(XirTypeReceivingRoot){from,
            program->declarations->slots[map->slots[i]].type};
    }
    for (uint32_t f=0;status==XR_XIR_OK && f<record->received_function_count;++f) {
        if (!xir_compile_work(context,1)) {status=XR_XIR_BUDGET;break;}
        uint32_t target=record->received_functions[f];
        if (target==UINT32_MAX) continue;
        const XrXirCallEntry *from=&map->unit->program->entries[f],*to=&program->entries[target];
        for (uint32_t p=0;p<=from->parameter_count;++p) {
            if (!xir_compile_work(context,1)) {status=XR_XIR_BUDGET;break;}
            XrXirType a=p ? from->parameters[p-1] : from->result;
            if (a!=XR_XIR_ERROR && (uint32_t)a<XR_XIR_CONSTRUCTED_TYPE_BASE) continue;
            if (!xir_compile_work(context,sizeof(*roots))) {status=XR_XIR_BUDGET;break;}
            roots[at++]=(XirTypeReceivingRoot){a,p ? to->parameters[p-1] : to->result};
        }
    }
    if (status==XR_XIR_OK) {
        XrXirValueStatus received=xir_compile_type_arena_receive(context,program->arena,map->unit->program->arena,
            roots,count);
        status=received==XR_XIR_VALUE_OK ? XR_XIR_OK : received==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
            received==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
    }
    xr_compile_resources_free(roots);return status;
}
static XrXirStatus generation_storage_admit(const XrXirCompileContext *context,
    const SourceGenerationRecord *record,const XrXirProgram *program) {
    const XrXirDeclarations *declarations=program->declarations;
    const SourceHistoryCorrespondence *map=&record->capture.semantic->history;
    uint8_t *received=NULL;XrXirStatus status=XR_XIR_OK;
    if (program->requires_birth_storage) {
        if (!map->unit || !map->unit->program) return XR_XIR_BAD_STRUCTURE;
        uint8_t *modules=xir_compile_calloc(context,declarations->module_count,1,&status);
        if (!modules) return status;
        for (uint32_t m=0;m<map->module_count;++m) {
            if (!xir_compile_work(context,1)) {status=XR_XIR_BUDGET;break;}
            modules[map->modules[m]]=1;
        }
        for (uint32_t m=0;status==XR_XIR_OK && m<declarations->module_count;++m) {
            if (!xir_compile_work(context,1)) {status=XR_XIR_BUDGET;break;}
            if (declarations->modules[m].storage==XR_XIR_MODULE_BIRTH_STORAGE && !modules[m]) status=XR_XIR_BAD_STRUCTURE;
        }
        xr_compile_resources_free(modules);if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t s=0;s<declarations->slot_count;++s) {
        if (!xir_compile_work(context,1)) {status=XR_XIR_BUDGET;break;}
        const XrXirSlot *slot=&declarations->slots[s];
        bool birth=declarations->modules[slot->module].storage==XR_XIR_MODULE_BIRTH_STORAGE;
        if (!birth && (!slot->mutable || slot->module==declarations->root_module ||
            !program->active_modules[slot->module])) continue;
        if (!map->unit || !map->unit->program) {status=XR_XIR_BAD_STRUCTURE;break;}
        if (!received) {
            received=xir_compile_calloc(context,declarations->slot_count,1,&status);
            if (!received) break;
            for (uint32_t prior=0;prior<map->slot_count;++prior) {
                if (!xir_compile_work(context,1)) {status=XR_XIR_BUDGET;break;}
                /* Sealed code correspondence has already checked every ordinal
                 * and its module, mutability and receiving type. Runtime
                 * preparation must still authenticate actual birth storage. */
                received[map->slots[prior]]=1;
            }
            if (status!=XR_XIR_OK) break;
        }
        if (!received[s]) {status=XR_XIR_BAD_STRUCTURE;break;}
    }
    xr_compile_resources_free(received);return status;
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_bind_program(XrXirSourceGenerations *owner,
    uint64_t serial,XrXirProgram *program) {
    if (!owner || !program || !serial) return XR_XIR_BAD_STRUCTURE;
    SourceGenerationRecord *record=owner->pending;
    if (!record || record->view.serial!=serial) return XR_XIR_BAD_STAGE;
    if (record->unit.program) return record->unit.program==program ? XR_XIR_OK : XR_XIR_BAD_STAGE;
    if (program->context.resources!=owner->config.context.resources) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=generation_function_correspondence(&owner->config.context,record,program);
    if (status!=XR_XIR_OK) return status;
    status=generation_program_correspondence(&owner->config.context,record,program);
    if (status==XR_XIR_OK) status=generation_storage_admit(&owner->config.context,record,program);
    if (status!=XR_XIR_OK) return status;
    if (!xr_xir_compile_program_retain(program)) return XR_XIR_BUDGET;
    status=generation_type_identities(&owner->config.context,record,program);
    if (status!=XR_XIR_OK) {xr_xir_compile_program_drop(program);return status;}
    record->unit.program=program;return XR_XIR_OK;
}
XR_FUNC XrXirCallStatus xr_xir_source_generation_prepare_instance(XrXirSourceGenerations *owner,
    uint64_t serial,XrXirInstance *instance,uint64_t *runtime_serial) {
    if (!owner || !instance || !runtime_serial || *runtime_serial) return XR_XIR_CALL_BAD_ARGUMENT;
    SourceGenerationRecord *record=owner->pending;
    if (!serial || !record || record->view.serial!=serial || !record->unit.program) return XR_XIR_CALL_BAD_STATE;
    const SourceHistoryCorrespondence *map=&record->capture.semantic->history;
    XrXirDomain *domain=xir_instance_generation_domain(instance);
    if (!domain || (owner->domain && owner->domain!=domain)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (map->unit && (!owner->domain || !map->unit->program)) return XR_XIR_CALL_BAD_STATE;
    bool retained=!owner->domain;
    if (retained && !xr_xir_domain_retain(domain)) return XR_XIR_CALL_LIMIT;
    XrXirCallStatus status;
    if (!map->unit) status=xr_xir_instance_generation_prepare(instance,record->unit.program,runtime_serial);
    else {
        XirGenerationStorage storage={map->unit->program,record->unit.program,
            map->modules,map->slots,map->module_count,map->slot_count,
            record->received_functions,record->received_function_count};
        status=xir_instance_generation_prepare_received(instance,&storage,runtime_serial);
    }
    if (status==XR_XIR_CALL_READY) owner->domain=domain;
    else if (retained) xr_xir_domain_drop(domain);
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_source_generation_publish(XrXirSourceGenerations *owner,uint64_t serial) {
    if (!owner) return XR_XIR_BAD_STRUCTURE;
    SourceGenerationRecord *record=owner->pending;
    if (!serial || !record || record->view.serial!=serial || record->view.type_query) return XR_XIR_BAD_STAGE;
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
    if (owner->published && owner->published->unit.interactive && source_text_same(&compare,NULL,name,"it")) {
        if (!owner->published->implicit_result.declaration) return XR_XIR_BAD_STRUCTURE;
        *output=owner->published->implicit_result;return XR_XIR_OK;
    }
    if (compare.diagnostic.status!=XR_XIR_OK) return compare.diagnostic.status;
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
        .ordinal=symbol->index,.dependency=UINT32_MAX,.reader=UINT32_MAX,.writer=UINT32_MAX,.name=symbol->name,.syntax=symbol->node,
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
        definition.kind=XR_XIR_SOURCE_DEFINITION_FUNCTION;
        if(symbol->reader) {
            uint32_t reader=symbol->reader-1;
            if(reader>=checked->function_count || checked->functions[reader].parameter_count ||
                checked->declarations->functions[reader].module!=module || !checked->declarations->functions[reader].exported ||
                !(checked->declarations->functions[reader].promises&XR_XIR_FUNCTION_NO_SUSPEND) ||
                !xr_xir_callable_signature(checked->types,checked->functions[reader].result))return XR_XIR_BAD_STRUCTURE;
            definition.reader=reader;definition.logical_type=checked->functions[reader].result;
        }
        break;
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
        if(symbol->mutable) {
            if(!environment->binding_writes || !environment->binding_writes[symbol->index])return XR_XIR_BAD_STRUCTURE;
            uint32_t writer=environment->binding_writes[symbol->index]-1;
            uint32_t parameters=symbol->type==XR_XIR_UNIT ? 0u : 1u;
            if(writer>=checked->function_count || checked->functions[writer].parameter_count!=parameters ||
                (parameters && (!checked->functions[writer].parameters || checked->functions[writer].parameters[0]!=symbol->type)) ||
                checked->functions[writer].result!=XR_XIR_UNIT || checked->declarations->functions[writer].module!=module ||
                !checked->declarations->functions[writer].exported ||
                !(checked->declarations->functions[writer].promises&XR_XIR_FUNCTION_NO_SUSPEND))return XR_XIR_BAD_STRUCTURE;
            definition.writer=writer;
        }
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
