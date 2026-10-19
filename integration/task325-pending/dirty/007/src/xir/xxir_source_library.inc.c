/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_library.inc.c - Checked library declaration import
 *
 * KEY CONCEPT: Checked definitions enter the existing inventory without an AST.
 */
#include "xxir_internal.h"
#include "xxir_library_import_shape.inc.c"
static const XrXirModule *source_library_module(SourceContext *ctx,uint32_t module) {
    const XrModuleResourceBinding *resource=ctx->graph->specs[module].resource;
    if(!resource||!resource->checked){source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Checked library resource is missing");return NULL;}
    const XrXirArtifact *artifact=resource->checked;
    const XrXirCompileContext *owner=xr_xir_compile_artifact_context(artifact);
    if (!owner || owner->resources!=ctx->compile.resources) {
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Checked library has a foreign resource owner");return NULL;
    }
    const XrXirModule *library=xr_xir_compile_artifact_module(artifact);
    if(library->linkage_kind!=XR_XIR_LIBRARY||!library->function_count){
        source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"Checked library kind is required");return NULL;
    }
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(artifact);
    XrXirStatus status=xr_xir_compile_verify_v2(&ctx->compile, library, construction, NULL);
    if(status!=XR_XIR_OK){source_fail(ctx,NULL,status,"Checked library verification failed");return NULL;}
    status=library_catalog_shape(library,construction,&ctx->compile);
    if(status!=XR_XIR_OK){source_fail(ctx,NULL,status,"Checked library import subset is not admitted");return NULL;}
    const XrXirDeclarations *d = library->declarations;
    const XrModuleSpec *spec = &ctx->graph->specs[module];
    if (!d || resource->checked_module >= d->module_count) {
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Checked library module ordinal is invalid");return NULL;
    }
    const XrXirSourceModule *view = &d->modules[resource->checked_module];
    size_t length = source_text_size(ctx,spec->canonical);
    if (!source_work(ctx,NULL)) return NULL;
    if (view->name_length != length || spec->dep_count < 0 ||
        (uint32_t)spec->dep_count != view->dependency_count ||
        resource->dependency_count != view->dependency_count ||
        (view->dependency_count && !resource->dependencies)) {
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Checked library module view disagrees with its declaration");return NULL;
    }
    for (size_t byte = 0; byte < length; ++byte) {
        if (!source_work(ctx,NULL)) return NULL;
        if (view->name[byte] != spec->canonical[byte]) {
            source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Checked library module identity is invalid");return NULL;
        }
    }
    for (uint32_t e = 0; e < view->dependency_count; ++e) {
        if (!source_work(ctx,NULL)) return NULL;
        const XrModuleResourceBinding *edge = resource->dependencies[e];
        int target = spec->dep_indices[e];
        if (!edge || edge->checked != artifact || edge->checked_module != view->dependencies[e] ||
            target < 0 || target >= ctx->graph->spec_count || ctx->graph->specs[target].resource != edge) {
            source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Checked library dependency view is invalid");return NULL;
        }
    }
    return library;
}
/* Each artifact is installed once even when several of its module views are
 * reachable. An absent module map entry is not an initialization dependency. */
static bool source_library_first(SourceContext *ctx, uint32_t module) {
    const void *artifact = ctx->graph->specs[module].resource->checked;
    for (uint32_t prior = 0; prior < module; ++prior) {
        if (!source_work(ctx,NULL)) return false;
        const XrModuleResourceBinding *resource = ctx->graph->specs[prior].resource;
        if (ctx->graph->specs[prior].representation == XR_MODULE_CHECKED_LIBRARY &&
            resource && resource->checked == artifact) return false;
    }
    return true;
}
static bool source_library_inventory(SourceContext *ctx, uint32_t module,
    const XrXirModule *library, uint32_t *count, uint32_t *slots) {
    const XrXirDeclarations *d = library->declarations;
    const XrModuleResourceBinding *resource = ctx->graph->specs[module].resource;
    if (!d || resource->checked_module >= d->module_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library module view is invalid");
    uint32_t total = 0;
    for (uint32_t f = 0; f < library->function_count; ++f) {
        if (!source_work(ctx,NULL)) return false;
        if (d->functions[f].module == resource->checked_module &&
            f != d->modules[resource->checked_module].initializer) ++total;
    }
    for (uint32_t s=0; s<d->slot_count; ++s) {
        if (!source_work(ctx,NULL)) return false;
        if (d->slots[s].module!=resource->checked_module) continue;
        if (*slots==UINT32_MAX)
            return source_fail(ctx,NULL,XR_XIR_BUDGET,"library slot inventory exhausted");
        ++*slots;
    }
    *count = total; return true;
}
#include "xxir_source_library_types.inc.c"

typedef struct SourceLibraryMap {
    SourceLibraryUnit *unit;
    uint32_t *modules;
    uint32_t module_count;
    uint32_t *functions;
    uint32_t function_count;
    uint32_t next_function;
    uint32_t *slots, slot_count, next_slot;
    uint32_t literal_begin, literal_count;
} SourceLibraryMap;
static void source_library_map_free(SourceLibraryMap *map) {
    xr_compile_resources_free(map->modules); xr_compile_resources_free(map->functions);
    xr_compile_resources_free(map->slots);
    *map = (SourceLibraryMap){0};
}
#include "xxir_source_library_slots.inc.c"
static bool source_library_map(SourceContext *ctx, const XrXirModule *library,
    uint32_t module, uint32_t first, uint32_t first_slot, SourceLibraryMap *output) {
    const XrXirDeclarations *d = library->declarations;
    if (!d || !d->module_count || !library->function_count ||
        ctx->graph->spec_count <= 0 || module >= (uint32_t)ctx->graph->spec_count ||
        first < (uint32_t)ctx->graph->spec_count || first > ctx->function_count ||
        !ctx->graph->specs || !ctx->graph->specs[module].resource ||
        !ctx->graph->specs[module].resource->checked || first_slot>ctx->slot_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library identity reservation is invalid");
    if (d->literal_count > UINT32_MAX - ctx->literal_count)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"library literal inventory exhausted");
    SourceLibraryMap map = {0};
    map.unit = source_library_unit(ctx,library);
    if (library->types && !map.unit)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library declaration map is missing");
    map.module_count = d->module_count; map.function_count = library->function_count;
    map.literal_begin = ctx->literal_count; map.literal_count = d->literal_count;
    map.modules = source_scratch(ctx,d->module_count,sizeof(*map.modules),false);
    map.functions = source_scratch(ctx,library->function_count,sizeof(*map.functions),false);
    if (!map.modules || !map.functions) goto failed;
    for (uint32_t m = 0; m < map.module_count; ++m) {
        if (!source_work(ctx,NULL)) goto failed;
        map.modules[m] = UINT32_MAX;
    }
    const void *artifact = ctx->graph->specs[module].resource->checked;
    for (uint32_t m = 0; m < (uint32_t)ctx->graph->spec_count; ++m) {
        if (!source_work(ctx,NULL)) goto failed;
        const XrModuleSpec *spec = &ctx->graph->specs[m];
        const XrModuleResourceBinding *resource = spec->resource;
        if (spec->representation != XR_MODULE_CHECKED_LIBRARY || !resource || resource->checked != artifact) continue;
        if (resource->checked_module >= map.module_count || map.modules[resource->checked_module] != UINT32_MAX) {
            source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library module view is duplicated"); goto failed;
        }
        map.modules[resource->checked_module] = m;
    }
    uint32_t next = first;
    for (uint32_t f = 0; f < map.function_count; ++f) {
        if (!source_work(ctx,NULL)) goto failed;
        uint32_t owner = d->functions[f].module;
        if (owner >= map.module_count) { source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library function owner is invalid"); goto failed; }
        uint32_t target = map.modules[owner];
        map.functions[f] = UINT32_MAX;
        if (target == UINT32_MAX) continue;
        if (f == d->modules[owner].initializer) map.functions[f] = target;
        else {
            if (next >= ctx->function_count) { source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library function inventory is inconsistent"); goto failed; }
            map.functions[f] = next++;
        }
    }
    if (!source_library_slot_map(ctx,library,&map,first_slot)) goto failed;
    map.next_function = next; *output = map; return true;
failed:
    source_library_map_free(&map); return false;
}

static bool source_library_instruction(SourceContext *ctx, const SourceLibraryMap *map,
    const XrXirInstruction *original, XrXirInstruction *output) {
    if (!source_work(ctx,NULL)) return false;
    XrXirInstruction result = *original;
    if (!source_library_type(ctx,map->unit,original->type,&result.type)) return false;
    switch (result.op) {
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_GO: case XR_XIR_FUNCTION_REF:
        if (result.immediate < 0 || (uint64_t)result.immediate >= map->function_count ||
            map->functions[(uint32_t)result.immediate] == UINT32_MAX)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library call identity is invalid");
        result.immediate = map->functions[(uint32_t)result.immediate];
        break;
    case XR_XIR_CALL_REQUIREMENT:
        if (!map->unit || !map->unit->library->types->interfaces ||
            result.targets[0]>=map->unit->library->types->interfaces->count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library requirement declaration is not mapped");
        result.targets[0]=map->unit->interfaces[result.targets[0]];
        break;
    case XR_XIR_CALL_DEFAULT:
        if (result.targets[0] >= map->function_count || map->functions[result.targets[0]] == UINT32_MAX)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library default owner identity is invalid");
        result.targets[0] = map->functions[result.targets[0]];
        break;
    case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_STORE:
    case XR_XIR_SLOT_PLACE: case XR_XIR_SLOT_GROUP_INIT:
        if (!source_library_slot_instruction(ctx,map,&result)) return false;
        break;
    case XR_XIR_CONST_STRING:
        if (result.immediate < 0 || (uint64_t)result.immediate >= map->literal_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library literal identity is invalid");
        if ((uint64_t)result.immediate > UINT32_MAX - map->literal_begin)
            return source_fail(ctx,NULL,XR_XIR_BUDGET,"library literal identity overflow");
        result.immediate += map->literal_begin;
        break;
    default:
        if (!library_catalog_local_instruction(result.op))
            return source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"library instruction remapping is not admitted");
        break;
    }
    *output = result;
    return true;
}

static bool source_library_bound_helper(SourceContext *ctx, const XrXirModule *library,
    uint32_t function, bool *bound) {
    *bound = false;
    if (!library->defaults) return true;
    for (uint32_t i = 0; i < library->defaults->count; ++i) {
        if (!source_work(ctx, NULL)) return false;
        if (library->defaults->records[i].function == function) {
            *bound = true;
            return true;
        }
    }
    return true;
}

static void *source_library_bytes(SourceContext *ctx, const void *input,
    uint32_t count, size_t size) {
    if (!count) return NULL;
    void *output = source_alloc(ctx, count, size);
    if (output && !source_copy_bytes(ctx, NULL, output, input, (size_t)count * size)) return NULL;
    return output;
}

/* Local binder ordinals remain local; each interface application and type ID
 * is deeply owned and remapped to the canonical receiver declarations. */
static bool source_library_generic(SourceContext *ctx, const XrXirModule *library, const SourceLibraryMap *map,
    uint32_t source, uint32_t target) {
    if (!library->generics) return true;
    const XrXirGeneric *original = &library->generics[source];
    XrXirGeneric *generic = &ctx->generics[target];
    generic->parameter_count = original->parameter_count;
    generic->argument_count = original->argument_count;
    if (!source_library_constraints(ctx,map->unit,original->constraints,
        original->parameter_count,&generic->constraints)) return false;
    generic->arguments = source_library_bytes(ctx,original->arguments,
        original->argument_count,sizeof(*original->arguments));
    generic->parameter_kinds = original->parameter_kinds ? source_library_bytes(ctx,
        original->parameter_kinds,original->parameter_count,sizeof(*original->parameter_kinds)) : NULL;
    if ((original->parameter_count && !generic->constraints) ||
        (original->argument_count && !generic->arguments) ||
        (original->parameter_kinds && !generic->parameter_kinds)) return false;
    for (uint32_t a=0; a<original->argument_count; ++a)
        if (!source_library_type(ctx,map->unit,original->arguments[a],&((XrXirType *)generic->arguments)[a])) return false;
    ctx->has_generics |= original->parameter_count != 0;
    ctx->bodies[target].type_parameter_count = original->parameter_count;
    return true;
}
/* Function identities move, but parameters, instructions and value IDs
 * remain local to the copied body. No evidence array borrows the catalog. */
static bool source_library_effect(SourceContext *ctx, const XrXirModule *library, const SourceLibraryMap *map,
    uint32_t source, uint32_t target) {
    const XrXirProvenance *evidence = library->provenance;
    if (!evidence) return true;
    if (!source_work(ctx,NULL)) return false;
    if (evidence->kind != XR_XIR_EVIDENCE_TEMPLATE)
        return source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"library effect template is required");
    if (evidence->contract_count != library->function_count ||
        source >= evidence->contract_count || !evidence->contracts)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library effect owner is invalid");
    const XrXirFunctionEffectContract *original = &evidence->contracts[source];
    XrXirFunctionEffectContract *owned = source_alloc(ctx,1,sizeof(*owned));
    if (!owned || !source_copy_bytes(ctx,NULL,owned,original,sizeof(*owned))) return false;
    owned->parameters = source_library_bytes(ctx,original->parameters,
        original->parameter_count,sizeof(*original->parameters));
    owned->formula.terms = source_library_bytes(ctx,original->formula.terms,
        original->formula.term_count,sizeof(*original->formula.terms));
    owned->values = source_library_bytes(ctx,original->values,
        original->value_count,sizeof(*original->values));
    owned->bindings = source_library_bytes(ctx,original->bindings,
        original->binding_count,sizeof(*original->bindings));
    if ((original->parameter_count && !owned->parameters) ||
        (original->formula.term_count && !owned->formula.terms) ||
        (original->value_count && !owned->values) ||
        (original->binding_count && !owned->bindings)) return false;
    if (!source_work(ctx,NULL)) return false;
    for (uint32_t v=0; v<owned->value_count; ++v)
        if (!source_library_type(ctx,map->unit,original->values[v].declared_type,
            &((XrXirRootValueIdentity *)owned->values)[v].declared_type)) return false;
    ctx->bodies[target].library_effect = owned;
    return true;
}

static bool source_library_function_query(SourceContext *ctx,uint32_t declaration) {
    if (!source_query_parameters(ctx,declaration)) return false;
    const XrXirFunction *function=&ctx->functions[ctx->function];
    source_library_query_type(ctx,declaration,function->result,declaration);
    XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)&ctx->query.declarations[declaration-1];
    XrXirSourceType *parameters=(XrXirSourceType *)query->parameters;
    for (uint32_t p=0; p<function->parameter_count; ++p) {
        if (!source_work(ctx,NULL)) return false;
        XrXirType shown=function->parameters[p];
        if (xr_xir_type_is_cell(&ctx->types,shown)) shown=xr_xir_cell_element(&ctx->types,shown);
        parameters[p].generic_owner=xr_xir_type_span(&ctx->types,shown) ? declaration : 0;
    }
    return true;
}
static bool source_library_function(SourceContext *ctx, const XrXirModule *library,
    const SourceLibraryMap *map, uint32_t index) {
    uint32_t target = map->functions[index];
    if (target == UINT32_MAX) return true;
    uint32_t original_module = library->declarations->functions[index].module;
    uint32_t module = map->modules[original_module];
    ctx->module = module;
    const XrXirFunction *original = &library->functions[index];
    XrXirFunction *function = &ctx->functions[target];
    *function = *original;
    char *name = source_alloc(ctx, (size_t)original->name_length + 1, 1);
    if (!name) return false;
    if (!source_copy_bytes(ctx, NULL, name, original->name, original->name_length)) return false;
    name[original->name_length] = 0;
    function->name = name;
    function->parameters = source_library_bytes(ctx, original->parameters,
        original->parameter_count, sizeof(*original->parameters));
    function->blocks = source_library_bytes(ctx, original->blocks,
        original->block_count, sizeof(*original->blocks));
    function->operands = source_library_bytes(ctx, original->operands,
        original->operand_count, sizeof(*original->operands));
    XrXirInstruction *instructions = source_alloc(ctx, original->instruction_count, sizeof(*instructions));
    function->instructions = instructions;
    if ((original->parameter_count && !function->parameters) || !function->blocks ||
        (original->operand_count && !function->operands) || !instructions) return false;
    if (!source_library_type(ctx,map->unit,original->result,&function->result)) return false;
    for (uint32_t p=0; p<original->parameter_count; ++p)
        if (!source_library_type(ctx,map->unit,original->parameters[p],&((XrXirType *)function->parameters)[p])) return false;
    for (uint32_t i = 0; i < original->instruction_count; ++i)
        if (!source_library_instruction(ctx, map, &original->instructions[i], &instructions[i])) return false;
    ctx->identities[target] = library->declarations->functions[index];
    ctx->identities[target].module = module;
    if (ctx->identities[target].nominal_owner) {
        uint32_t owner=ctx->identities[target].nominal_owner-1;
        if (!map->unit || !library->types || !library->types->nominals ||
            owner>=library->types->nominals->count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library member owner is not mapped");
        ctx->identities[target].nominal_owner=map->unit->nominals[owner]+1;
    }
    ctx->bodies[target].module = module;
    ctx->bodies[target].checked_library = true;
    ctx->bodies[target].parameters = (XrXirType *)function->parameters;
    if (!source_library_generic(ctx,library,map,index,target) ||
        !source_library_effect(ctx,library,map,index,target)) return false;
    bool helper;
    if (!source_library_bound_helper(ctx, library, index, &helper)) return false;
    if (helper || index == library->declarations->modules[original_module].initializer) return true;
    if (ctx->identities[target].nominal_owner &&
        (ctx->identities[target].method_kind==XR_XIR_CONSTRUCTOR ||
         ctx->identities[target].method_kind==XR_XIR_MEMBER_HELPER)) {
        uint32_t owner=ctx->identities[target].nominal_owner-1;
        ctx->bodies[target].declaration=ctx->nominal_sources[owner]->declaration;
        if (ctx->identities[target].method_kind==XR_XIR_CONSTRUCTOR) {
            if (ctx->nominal_constructors[owner])
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library constructor identity is duplicated");
            ctx->nominal_constructors[owner]=target;
        }
        return true;
    }
    if (ctx->identities[target].nominal_owner &&
        (ctx->identities[target].method_kind==XR_XIR_READ_METHOD ||
         ctx->identities[target].method_kind==XR_XIR_STATIC_METHOD)) {
        uint32_t owner=ctx->identities[target].nominal_owner-1;
        SourceName *nominal=ctx->nominal_sources[owner];
        const XrXirNominalDeclaration *declaration=&ctx->nominals.declarations[owner];
        if (declaration->kind==XR_XIR_NOMINAL_ENUM) {
            bool is_static=ctx->identities[target].method_kind==XR_XIR_STATIC_METHOD;
            if ((!is_static && (source_text_same(ctx,NULL,name,"name") ||
                    source_text_same(ctx,NULL,name,"ordinal") || source_text_same(ctx,NULL,name,"toString"))) ||
                (is_static && source_text_same(ctx,NULL,name,"variants")))
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"library method conflicts with enum builtin member");
            for (uint32_t v=0; v<declaration->variant_count; ++v) {
                if (!source_work(ctx,NULL)) return false;
                XrXirLiteral variant=declaration->variants[v].name;
                if (variant.length==original->name_length &&
                    source_span_same(ctx,NULL,variant.bytes,name,variant.length))
                    return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"library method conflicts with enum variant");
            }
        } else for (uint32_t field=0; field<declaration->field_count; ++field) {
            if (!source_work(ctx,NULL)) return false;
            XrXirLiteral field_name=declaration->fields[field].name;
            if (field_name.length==original->name_length &&
                source_span_same(ctx,NULL,field_name.bytes,name,field_name.length))
                return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"library field and method names must be distinct");
        }
        SourceName *method=add_name(ctx,&ctx->nominal_methods[owner],name,NULL);
        if (!method) return false;
        method->kind=SOURCE_FUNCTION;method->index=target;method->module=module;method->checked_library=true;
        ctx->function=target;
        if (!source_query_declare(ctx,method,XR_XIR_SOURCE_FUNCTION,nominal->declaration,
            (XrXirSourceRange){module,0,0,0,0})) return false;
        ctx->bodies[target].declaration=method->declaration;
        ctx->bodies[target].generic_owner=ctx->generics[target].parameter_count ? method->declaration : 0;
        if (!source_library_function_query(ctx,method->declaration)) return false;
        XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)&ctx->query.declarations[method->declaration-1];
        query->generic_parent=declaration->parameter_count ? nominal->declaration : 0;
        query->generic_parent_count=declaration->parameter_count;
        /* Source member access is represented by its owned function identity;
         * being public here never publishes a module-level function alias. */
        return true;
    }
    if (ctx->identities[target].nominal_owner || ctx->identities[target].cleanup_owner ||
        ctx->identities[target].method_kind != XR_XIR_NON_MEMBER)
        return source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"library member cannot be installed as a top-level function");
    SourceName *symbol = add_name(ctx, &ctx->names[module], name, NULL);
    if (!symbol) return false;
    symbol->kind = SOURCE_FUNCTION; symbol->index = target; symbol->module = module; ctx->function = target;
    if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_FUNCTION, 0,
        (XrXirSourceRange){module,0,0,0,0})) return false;
    ctx->bodies[target].declaration = symbol->declaration;
    ctx->bodies[target].generic_owner = ctx->generics[target].parameter_count ? symbol->declaration : 0;
    if (!source_library_function_query(ctx,symbol->declaration)) return false;
    XrXirSourceDeclaration *query = (XrXirSourceDeclaration *)ctx->query.declarations;
    query[symbol->declaration - 1].exported = ctx->identities[target].exported;
    return true;
}

static bool source_library_literals(SourceContext *ctx, const XrXirModule *library,
    const SourceLibraryMap *map) {
    const XrXirDeclarations *declarations = library->declarations;
    for (uint32_t i = 0; i < map->literal_count; ++i) {
        const XrXirLiteral *original = &declarations->literals[i];
        if (original->length && !original->bytes)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library literal bytes are missing");
        char *bytes = original->length ? source_alloc(ctx, original->length, 1) : NULL;
        if (original->length && !bytes) return false;
        if (!source_copy_bytes(ctx,NULL,bytes,original->bytes,original->length)) return false;
        uint32_t id;
        if (!source_literal_append(ctx, NULL, bytes, original->length, &id)) return false;
        if (id != map->literal_begin + i)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library literal interval is inconsistent");
    }
    return true;
}

/* Bind after every selected function identity has been installed. The inputs
 * are already commonly verified, but no old function ordinal is published. */
static bool source_library_initializers(SourceContext *ctx, const XrXirModule *library,
    const SourceLibraryMap *map) {
    const XrXirNominalTable *table=library->types ? library->types->nominals : NULL;
    for (uint32_t n=0; table && n<table->count; ++n) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t target=map->unit->nominals[n];
        const XrXirNominalDeclaration *from=&table->declarations[n];
        const XrXirConstructionRow *row=xr_xir_compile_construction_row(map->unit->construction,n);
        if (!row || row->field_count!=from->field_count || (row->field_count && !row->field_initializers))
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library construction row is incomplete");
        if (row->default_initializer) {
            uint32_t f=row->default_initializer-1;
            if (f>=map->function_count || map->functions[f]==UINT32_MAX ||
                ctx->nominal_constructors[target]!=map->functions[f])
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library implicit constructor is not mapped");
        }
        for (uint32_t field=0; field<row->field_count; ++field) {
            if (!source_work(ctx,NULL)) return false;
            if (!row->field_initializers[field]) continue;
            uint32_t f=row->field_initializers[field]-1;
            if (f>=map->function_count || map->functions[f]==UINT32_MAX)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library field initializer is not mapped");
            ctx->nominal_defaults[target][field]=map->functions[f];
        }
    }
    return true;
}

static bool source_library_implementations(SourceContext *ctx,const XrXirModule *library,
    const SourceLibraryMap *map) {
    const XrXirImplementationTable *input=library->declarations->implementations;
    if (!input) return true;
    if (!map->unit || map->unit->implementations.count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library witness unit is missing or already installed");
    XrXirImplementation *records=source_alloc(ctx,input->count,sizeof(*records));
    if (!records) return false;
    for (uint32_t i=0; i<input->count; ++i) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirImplementation *from=&input->records[i];
        if (!library->types->nominals || from->nominal_declaration>=library->types->nominals->count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library implementation owner is not mapped");
        XrXirImplementation *to=&records[i];
        to->nominal_declaration=map->unit->nominals[from->nominal_declaration];
        const XrXirInterfaceApplication *application=NULL;
        if (!source_library_applications(ctx,map->unit,&from->interface,1,&application)) return false;
        if (!source_work_units(ctx,NULL,sizeof(*application))) return false;
        to->interface=*application;
        XrXirImplementationBinding *bindings=from->binding_count ?
            source_alloc(ctx,from->binding_count,sizeof(*bindings)) : NULL;
        if (from->binding_count && !bindings) return false;
        to->bindings=bindings;to->binding_count=from->binding_count;
        for (uint32_t b=0; b<from->binding_count; ++b) {
            if (!source_work(ctx,NULL)) return false;
            const XrXirImplementationBinding *binding=&from->bindings[b];
            if (binding->function>=map->function_count || map->functions[binding->function]==UINT32_MAX)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library witness function is outside selected closure");
            if (!source_library_applications(ctx,map->unit,&binding->requirement,1,&application)) return false;
            if (!source_work_units(ctx,NULL,sizeof(*application))) return false;
            bindings[b].requirement=*application;
            bindings[b].member=binding->member;bindings[b].function=map->functions[binding->function];
        }
    }
    map->unit->implementations=(XrXirImplementationTable){records,input->count};
    return true;
}

static bool source_library_copy(SourceContext *ctx, const XrXirModule *library,
    const SourceLibraryMap *map) {
    if (!source_library_slots(ctx,library,map) || !source_library_literals(ctx, library, map)) return false;
    for (uint32_t f = 0; f < library->function_count; ++f) {
        if (!source_work(ctx, NULL) || !source_library_function(ctx, library, map, f)) return false;
    }
    if (!source_library_initializers(ctx,library,map) ||
        !source_library_implementations(ctx,library,map)) return false;
    if (!library->defaults) return true;
    for (uint32_t i = 0; i < library->defaults->count; ++i) {
        if (!source_work(ctx, NULL)) return false;
        const XrXirDefaultBinding *binding = &library->defaults->records[i];
        if (binding->owner >= map->function_count || binding->function >= map->function_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library default binding identity is invalid");
        if (map->functions[binding->owner] == UINT32_MAX) continue;
        if (map->functions[binding->function] == UINT32_MAX)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library default target is outside the selected closure");
        if (!source_default_binding_add(ctx, NULL, map->functions[binding->owner],
            binding->ordinal, map->functions[binding->function])) return false;
    }
    return true;
}

static bool source_library_install(SourceContext *ctx, uint32_t module, uint32_t *next_function, uint32_t *next_slot) {
    const XrModuleResourceBinding *resource = ctx->graph->specs[module].resource;
    const XrXirModule *library = xr_xir_compile_artifact_module(resource->checked);
    SourceLibraryMap map = {0};
    if (!source_library_map(ctx,library,module,*next_function,*next_slot,&map)) return false;
    bool ok = source_library_copy(ctx,library,&map);
    if (ok) { *next_function = map.next_function; *next_slot = map.next_slot; }
    source_library_map_free(&map);
    return ok;
}
