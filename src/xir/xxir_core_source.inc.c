/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_core_source.inc.c - Closed source origin for intrinsic declarations
 *
 * KEY CONCEPT:
 *   Only this loader binds immutable compiler source to typed intrinsic bodies.
 *   User declarations and serialized facts cannot enter the binding boundary.
 */
#include "xir_core_declarations_source.inc.c"

static bool source_core_contract(SourceContext *ctx) {
    if (!ctx->core_factory || ctx->graph->spec_count != 1 || ctx->function_count != 5 ||
        !ctx->has_generics || ctx->types.count != 1 || ctx->types.nominals || ctx->types.interfaces ||
        ctx->slot_count || ctx->defaults.count != 2 || ctx->graph->specs[0].dep_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"core declaration inventory is invalid");
    const XrXirFunction *owner = &ctx->functions[1];
    const XrXirDefaultBinding *binding = &ctx->defaults.records[0];
    AstNode *node = ctx->bodies[1].node;
    if (!node || node->type != AST_FUNCTION_DECL || !node->is_exported ||
        owner->parameter_count != 2 || owner->parameters[0] != XR_XIR_BOOL ||
        owner->parameters[1] != XR_XIR_STRING || owner->result != XR_XIR_UNIT ||
        strcmp(owner->name,"assert") || binding->owner_kind != XR_XIR_DEFAULT_PARAMETER ||
        binding->owner != 1 || binding->ordinal != 1 || binding->function != 3 ||
        ctx->functions[3].parameter_count || ctx->functions[3].result != XR_XIR_STRING ||
        node->as.function_decl.body->type != AST_BLOCK || node->as.function_decl.body->as.block.count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"core assertion declaration is invalid");
    XrXirSourceDeclaration *declarations = (XrXirSourceDeclaration *)ctx->query.declarations;
    XrXirSourceDeclaration *declaration = &declarations[ctx->bodies[1].declaration-1];
    declaration->kind = XR_XIR_SOURCE_INTRINSIC;
    declaration->native_identity = XR_CORE_BUILTIN_ASSERT;
    owner = &ctx->functions[2]; node = ctx->bodies[2].node; binding = &ctx->defaults.records[1];
    const XrXirTypeNode *action = xr_xir_callable_signature(&ctx->types,owner->parameters[0]);
    if (!node || node->type != AST_FUNCTION_DECL || !node->is_exported ||
        owner->parameter_count != 2 || owner->parameters[1] != XR_XIR_STRING || owner->result != XR_XIR_UNIT ||
        strcmp(owner->name,"assertPanics") || !action || action->parameter_count || action->flags ||
        action->result != XR_XIR_TYPE_PARAMETER_BASE ||
        ctx->generics[2].parameter_count != 1 || xr_xir_binder_kind(&ctx->generics[2],0) != XR_XIR_BINDER_RESULT_VARIABLE ||
        binding->owner_kind != XR_XIR_DEFAULT_PARAMETER || binding->owner != 2 || binding->ordinal != 1 || binding->function != 4 ||
        ctx->functions[4].parameter_count || ctx->functions[4].result != XR_XIR_STRING ||
        node->as.function_decl.body->type != AST_BLOCK || node->as.function_decl.body->as.block.count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"core panic declaration is invalid");
    declaration = &declarations[ctx->bodies[2].declaration-1];
    declaration->kind = XR_XIR_SOURCE_INTRINSIC; declaration->native_identity = XR_CORE_BUILTIN_ASSERT_PANICS;
    return true;
}

#include "xxir_source_construct.inc.c"

static void source_core_dispose(SourceContext *ctx) {
    for (SourceManifest *manifest=ctx->manifests;manifest;manifest=manifest->next)
        xr_declaration_manifest_free(manifest->declarations);
    while (ctx->memory) {
        SourceMemory *next=ctx->memory->next;
        xr_free(ctx->memory); ctx->memory=next;
    }
    xr_module_graph_free(ctx->graph);
}

static char *source_core_text(SourceContext *ctx, const char *input, size_t length) {
    if (!input) return NULL;
    if (length > ctx->budget.work) {
        source_fail(ctx,NULL,XR_XIR_BUDGET,"core source copy work exhausted"); return NULL;
    }
    char *output=source_alloc(ctx,length+1,1);
    if (output) {memcpy(output,input,length);output[length]=0;ctx->budget.work-=length;}
    return output;
}
static char *source_core_string(SourceContext *ctx, const char *input) {
    return input ? source_core_text(ctx,input,strlen(input)) : NULL;
}

static XrXirSourceType source_core_query_type(XrXirSourceType type, uint32_t declaration, XrXirType action) {
    if (type.type == XR_XIR_CONSTRUCTED_TYPE_BASE) type.type = action;
    if (type.generic_owner) type.generic_owner += declaration;
    return type;
}
static bool source_core_query(SourceContext *ctx, const SourceContext *core, uint32_t owners[2], XrXirType action) {
    uint32_t module=ctx->query.module_count, first=ctx->query.declaration_count;
    XrXirSourceQueryModule *modules=source_query_append(ctx,ctx->query.modules,
        &ctx->query.module_count,&ctx->query_module_capacity,sizeof(*modules));
    if (!modules) return false;
    ctx->query.modules=modules;
    const XrModuleSpec *spec=&core->graph->specs[0];
    char *identity=source_core_string(ctx,spec->canonical);
    if (!identity) return false;
    modules[module]=(XrXirSourceQueryModule){identity,NULL,spec->source_content_fingerprint};
    for (uint32_t d=0;d<core->query.declaration_count;++d) {
        XrXirSourceDeclaration original=core->query.declarations[d];
        XrXirSourceDeclaration *records=source_query_append(ctx,ctx->query.declarations,
            &ctx->query.declaration_count,&ctx->declaration_capacity,sizeof(*records));
        if (!records) return false;
        ctx->query.declarations=records;
        original.id+=first;
        if (original.parent) original.parent+=first;
        if (original.target) original.target+=first;
        if (original.generic_parent) original.generic_parent+=first;
        original.range.module=module;
        original.type=source_core_query_type(original.type,first,action);
        original.name=source_core_string(ctx,original.name);
        original.signature=source_core_string(ctx,original.signature);
        original.parameters=source_library_bytes(ctx,original.parameters,
            original.parameter_count,sizeof(*original.parameters));
        original.generic_constraints=source_library_bytes(ctx,original.generic_constraints,
            original.generic_parameter_count,sizeof(*original.generic_constraints));
        original.type_parameter_kinds=source_library_bytes(ctx,original.type_parameter_kinds,
            original.type_parameter_kinds ? original.generic_parameter_count : 0,sizeof(*original.type_parameter_kinds));
        if (!original.name || (original.parameter_count && !original.parameters) ||
            (original.generic_parameter_count && !original.generic_constraints) ||
            (core->query.declarations[d].type_parameter_kinds && !original.type_parameter_kinds) ||
            (core->query.declarations[d].signature && !original.signature)) return false;
        for (uint32_t p=0;p<original.parameter_count;++p) {
            if (!source_work(ctx,NULL)) return false;
            XrXirSourceType *parameters=(XrXirSourceType *)original.parameters;
            parameters[p]=source_core_query_type(parameters[p],first,action);
        }
        records[ctx->query.declaration_count-1]=original;
    }
    for (uint32_t e=0;e<core->query.expression_count;++e) {
        XrXirSourceExpression *records=source_query_append(ctx,ctx->query.expressions,
            &ctx->query.expression_count,&ctx->expression_capacity,sizeof(*records));
        if (!records) return false;
        ctx->query.expressions=records;
        records[ctx->query.expression_count-1]=core->query.expressions[e];
        records[ctx->query.expression_count-1].range.module=module;
        records[ctx->query.expression_count-1].type=source_core_query_type(records[ctx->query.expression_count-1].type,first,action);
    }
    owners[0]=core->bodies[1].declaration+first; owners[1]=core->bodies[2].declaration+first;
    return true;
}

typedef struct SourceCoreMap { uint32_t literal; XrXirType action; } SourceCoreMap;
static bool source_core_function_copy(SourceContext *ctx, const XrXirModule *module,
    uint32_t source, uint32_t target, SourceCoreMap map) {
    const XrXirFunction *original=&module->functions[source];
    XrXirFunction *function=&ctx->functions[target];
    *function=*original;
    function->name=source_core_text(ctx,original->name,original->name_length);
    function->parameters=source_library_bytes(ctx,original->parameters,
        original->parameter_count,sizeof(*original->parameters));
    function->blocks=source_library_bytes(ctx,original->blocks,
        original->block_count,sizeof(*original->blocks));
    function->operands=source_library_bytes(ctx,original->operands,
        original->operand_count,sizeof(*original->operands));
    XrXirInstruction *instructions=source_library_bytes(ctx,original->instructions,
        original->instruction_count,sizeof(*original->instructions));
    function->instructions=instructions;
    if (!function->name || !function->blocks || !instructions ||
        (original->parameter_count && !function->parameters) ||
        (original->operand_count && !function->operands)) return false;
    for (uint32_t p=0;p<function->parameter_count;++p) {
        if (!source_work(ctx,NULL)) return false;
        XrXirType *parameters=(XrXirType *)function->parameters;
        if (parameters[p]==XR_XIR_CONSTRUCTED_TYPE_BASE) parameters[p]=map.action;
    }
    for (uint32_t i=0;i<original->instruction_count;++i) {
        if (!source_work(ctx,NULL)) return false;
        if (instructions[i].op==XR_XIR_CONST_STRING) instructions[i].immediate+=map.literal;
        if (instructions[i].type==XR_XIR_CONSTRUCTED_TYPE_BASE) instructions[i].type=map.action;
    }
    if (module->generics) {
        XrXirGeneric generic=module->generics[source];
        generic.constraints=source_library_bytes(ctx,generic.constraints,generic.parameter_count,sizeof(*generic.constraints));
        generic.arguments=source_library_bytes(ctx,generic.arguments,generic.argument_count,sizeof(*generic.arguments));
        generic.parameter_kinds=source_library_bytes(ctx,generic.parameter_kinds,
            generic.parameter_kinds ? generic.parameter_count : 0,sizeof(*generic.parameter_kinds));
        if ((generic.parameter_count && !generic.constraints) || (generic.argument_count && !generic.arguments) ||
            (module->generics[source].parameter_kinds && !generic.parameter_kinds)) return false;
        ctx->generics[target]=generic;
        ctx->has_generics |= generic.parameter_count!=0;
    }
    ctx->identities[target]=module->declarations->functions[source];
    ctx->identities[target].module=ctx->module_count;
    ctx->bodies[target].module=ctx->module_count;
    ctx->bodies[target].checked_library=true;
    ctx->bodies[target].parameters=(XrXirType *)function->parameters;
    return true;
}

static bool source_core_install(SourceContext *ctx, const SourceContext *core,
    const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_artifact_module(artifact);
    uint32_t first=ctx->function_count, count=module->function_count;
    if (first > ctx->function_capacity || count > ctx->function_capacity-first ||
        first > ctx->budget.functions || count > ctx->budget.functions-first ||
        ctx->module_count==UINT32_MAX)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"core declaration capacity exhausted");
    uint32_t literal=ctx->literal_count;
    XrXirType action;
    if (!module->types || module->types->count != 1 ||
        !source_intern_type(ctx,module->types->nodes[0],&action)) return false;
    for (uint32_t l=0;l<module->declarations->literal_count;++l) {
        const XrXirLiteral *original=&module->declarations->literals[l];
        char *bytes=source_library_bytes(ctx,original->bytes,original->length,1);
        uint32_t id;
        if ((original->length && !bytes) ||
            !source_literal_append(ctx,NULL,bytes,original->length,&id)) return false;
    }
    for (uint32_t f=0;f<count;++f)
        if (!source_core_function_copy(ctx,module,f,first+f,(SourceCoreMap){literal,action})) return false;
    uint32_t declarations[2];
    if (!source_core_query(ctx,core,declarations,action)) return false;
    XrXirSourceModule *modules=source_alloc(ctx,(size_t)ctx->module_count+1,sizeof(*modules));
    if (!modules) return false;
    memcpy(modules,ctx->modules,ctx->module_count*sizeof(*modules));
    const XrXirSourceModule *original=&module->declarations->modules[0];
    char *identity=source_core_text(ctx,original->name,original->name_length);
    if (!identity) return false;
    modules[ctx->module_count]=(XrXirSourceModule){identity,original->name_length,NULL,0,first};
    ctx->modules=modules;
    for (uint32_t d=0;d<module->defaults->count;++d) {
        const XrXirDefaultBinding *binding=&module->defaults->records[d];
        if (!source_default_binding_add(ctx,NULL,first+binding->owner,
            binding->ordinal,first+binding->function)) return false;
    }
    for (uint32_t i=0;i<2;++i) {
        SourceName *symbol=source_alloc(ctx,1,sizeof(*symbol));
        if (!symbol) return false;
        symbol->name=ctx->functions[first+1+i].name;
        symbol->kind=SOURCE_FUNCTION; symbol->index=first+1+i;
        symbol->module=ctx->module_count; symbol->declaration=declarations[i];
        ctx->bodies[first+1+i].declaration=declarations[i];
        if (i) ctx->assert_panics=symbol; else ctx->assertion=symbol;
    }
    ctx->function_count+=count; ++ctx->module_count;
    return true;
}

/* There is no external syntax, resource or identity input at this boundary.
 * The signature and default expression are parsed from one immutable source;
 * ordinary checking then qualifies the internally bound executable body. */
static bool source_core_load(SourceContext *ctx, AstNode *site) {
    SourceContext core={0};
    core.budget=ctx->budget; core.linkage_kind=XR_XIR_LIBRARY; core.core_factory=true;
    XrCompilerSession *session=xr_compiler_session_new(NULL);
    XrModuleResolverConfig config={0};
    XrModuleResolver *resolver=xr_module_resolver_new(&config);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"xray-core-assertions-v1",NULL};
    XrXirSourceResult result={0};
    char *error=NULL;
    core.graph=session && resolver ? xr_module_graph_new(session,resolver) : NULL;
    if (!core.graph) source_fail(&core,NULL,XR_XIR_OUT_OF_MEMORY,"core source graph allocation failed");
    else if (xr_module_graph_build_source(core.graph,&authority,xir_core_declaration_source,&error) ||
        xr_module_graph_topological_sort(core.graph) || core.graph->has_cycle)
        source_fail(&core,NULL,XR_XIR_BAD_STRUCTURE,error ? error : "core source graph is invalid");
    else if (source_manifests_load(&core)) {
        XrXirBudget checking=core.budget;
        source_construct(&core,&checking,&result);
    }
    ctx->budget=core.budget;
    bool ok=core.diagnostic.status==XR_XIR_OK && result.checked && source_core_install(ctx,&core,result.checked);
    if (!ok && ctx->diagnostic.status==XR_XIR_OK)
        source_fail(ctx,site,core.diagnostic.status==XR_XIR_OK ? XR_XIR_BAD_STRUCTURE : core.diagnostic.status,
            core.diagnostic.message[0] ? core.diagnostic.message : "core source construction failed");
    xr_xir_source_result_free(&result);
    source_core_dispose(&core);
    xr_module_resolver_free(resolver);
    xr_compiler_session_delete(session);
    xr_free(error);
    return ok;
}

static SourceName *source_core_assertion(SourceContext *ctx, AstNode *site, uint32_t intrinsic) {
    if (!ctx->assertion && !source_core_load(ctx,site)) return NULL;
    SourceName *symbol=intrinsic==XR_CORE_BUILTIN_ASSERT ? ctx->assertion : ctx->assert_panics;
    if (!symbol) return NULL;
    XrXirSourceModule *module=&ctx->modules[ctx->module];
    for (uint32_t d=0;d<module->dependency_count;++d) {
        if (!source_work(ctx,site)) return NULL;
        if (module->dependencies[d]==symbol->module) return symbol;
    }
    uint32_t *dependencies=source_alloc(ctx,(size_t)module->dependency_count+1,sizeof(*dependencies));
    if (!dependencies) return NULL;
    if (module->dependency_count) memcpy(dependencies,module->dependencies,module->dependency_count*sizeof(*dependencies));
    dependencies[module->dependency_count++]=symbol->module;
    module->dependencies=dependencies;
    return symbol;
}
