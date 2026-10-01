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
static const XrXirModule *source_library_module(SourceContext *ctx,uint32_t module) {
    const XrModuleResourceBinding *resource=ctx->graph->specs[module].resource;
    if(!resource||!resource->checked){source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Checked library resource is missing");return NULL;}
    const XrXirArtifact *artifact=resource->checked;
    const XrXirModule *library=xr_xir_artifact_module(artifact);
    if(library->linkage_kind!=XR_XIR_LIBRARY||!library->function_count){
        source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"Checked library kind is required");return NULL;
    }
    XrXirStatus status=xr_xir_verify_remaining(library,&ctx->budget,NULL);
    if(status!=XR_XIR_OK){source_fail(ctx,NULL,status,"Checked library verification failed");return NULL;}
    return library;
}
/* Builder-owned IDs are reserved before any body is copied. The artifact
 * binding authenticates the input; this map does not authenticate by name.
 * The current catalog admits one module and no other identity-bearing table. */
typedef struct SourceLibraryMap {
    uint32_t module;
    uint32_t *functions;
    uint32_t function_count;
    uint32_t next_function;
} SourceLibraryMap;

static bool source_library_map(SourceContext *ctx, const XrXirModule *library,
    uint32_t module, uint32_t first, SourceLibraryMap *output) {
    const XrXirDeclarations *declarations = library->declarations;
    if (!declarations || declarations->module_count != 1 || !library->function_count ||
        ctx->graph->spec_count <= 0 || module >= (uint32_t)ctx->graph->spec_count ||
        first < (uint32_t)ctx->graph->spec_count ||
        first > ctx->function_count ||
        library->function_count - 1 > ctx->function_count - first)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library identity reservation is invalid");
    uint32_t initializer = declarations->modules[0].initializer;
    if (initializer >= library->function_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library initializer identity is invalid");
    size_t bytes = (size_t)library->function_count * sizeof(uint32_t);
    if (bytes / sizeof(uint32_t) != library->function_count ||
        bytes > ctx->budget.scratch_bytes)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"library identity map scratch exhausted");
    ctx->budget.scratch_bytes -= bytes;
    uint32_t *functions = xr_malloc(bytes);
    if (!functions) {
        ctx->budget.scratch_bytes += bytes;
        return source_fail(ctx,NULL,XR_XIR_OUT_OF_MEMORY,"library identity map allocation failed");
    }
    uint32_t next = first;
    for (uint32_t f = 0; f < library->function_count; ++f) {
        if (!source_work(ctx,NULL)) {
            xr_free(functions); ctx->budget.scratch_bytes += bytes; return false;
        }
        functions[f] = f == initializer ? module : next++;
    }
    *output = (SourceLibraryMap){module,functions,library->function_count,next};
    return true;
}

static bool source_library_instruction(SourceContext *ctx, const SourceLibraryMap *map,
    const XrXirInstruction *original, XrXirInstruction *output) {
    if (!source_work(ctx,NULL)) return false;
    XrXirInstruction result = *original;
    switch (result.op) {
    case XR_XIR_CALL:
        if (result.immediate < 0 || (uint64_t)result.immediate >= map->function_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library call identity is invalid");
        result.immediate = map->functions[(uint32_t)result.immediate];
        break;
    case XR_XIR_RETURN: case XR_XIR_CONST_INT: case XR_XIR_ADD_INT:
        break;
    default:
        return source_fail(ctx,NULL,XR_XIR_BAD_STAGE,"library instruction remapping is not admitted");
    }
    *output = result;
    return true;
}

static bool source_library_copy(SourceContext *ctx, const XrXirModule *library,
    const SourceLibraryMap *map) {
    uint32_t module = map->module;
    uint32_t initializer = library->declarations->modules[0].initializer;
    for(uint32_t f=0;f<library->function_count;++f){
        if(!source_work(ctx,NULL))return false;
        uint32_t target=map->functions[f];
        const XrXirFunction *original=&library->functions[f];
        XrXirInstruction *instructions=source_alloc(ctx,original->instruction_count,sizeof(*instructions));
        if(!instructions)return false;
        for(uint32_t i=0;i<original->instruction_count;++i){
            if (!source_library_instruction(ctx,map,&original->instructions[i],&instructions[i])) return false;
        }
        ctx->functions[target]=*original;ctx->functions[target].instructions=instructions;
        ctx->identities[target]=library->declarations->functions[f];ctx->identities[target].module=map->module;
        ctx->bodies[target].module=module;ctx->bodies[target].checked_library=true;
        if(f==initializer)continue;
        char *name=source_alloc(ctx,(size_t)original->name_length+1,1);if(!name)return false;
        memcpy(name,original->name,original->name_length);name[original->name_length]=0;
        SourceName *symbol=add_name(ctx,&ctx->names[module],name,NULL);if(!symbol)return false;
        symbol->kind=SOURCE_FUNCTION;symbol->index=target;ctx->function=target;
        if(!source_query_declare(ctx,symbol,XR_XIR_SOURCE_FUNCTION,0,(XrXirSourceRange){module,0,0,0,0}))return false;
        ctx->bodies[target].declaration=symbol->declaration;
        XrXirSourceDeclaration *query=(XrXirSourceDeclaration *)ctx->query.declarations;
        query[symbol->declaration-1].exported=ctx->identities[target].exported;
    }
    return true;
}

static bool source_library_install(SourceContext *ctx, uint32_t module, uint32_t *next_function) {
    const XrModuleResourceBinding *resource = ctx->graph->specs[module].resource;
    const XrXirModule *library = xr_xir_artifact_module(resource->checked);
    SourceLibraryMap map = {0};
    if (!source_library_map(ctx,library,module,*next_function,&map)) return false;
    bool ok = source_library_copy(ctx,library,&map);
    xr_free(map.functions);
    ctx->budget.scratch_bytes += (size_t)map.function_count * sizeof(uint32_t);
    if (ok) *next_function = map.next_function;
    return ok;
}
