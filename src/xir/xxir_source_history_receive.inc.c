/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_history_receive.inc.c - Registered bindings over relocated Checked bodies
 */
static uint32_t source_history_target(SourceContext *ctx,uint32_t original) {
    for (uint32_t m=0;m<(uint32_t)ctx->graph->spec_count;++m) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return UINT32_MAX;
        const XrModuleSpec *spec=&ctx->graph->specs[m];
        if (spec->representation==XR_MODULE_CHECKED_HISTORY && spec->history==ctx->history_input &&
            spec->history_module==original) return m;
    }
    source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history scope is not mapped");return UINT32_MAX;
}
static bool source_history_begin(SourceContext *ctx) {
    if (!ctx->history_input) return true;
    if (!ctx->history) {
        ctx->history=source_alloc(ctx,1,sizeof(*ctx->history));
        if (!ctx->history) return false;
        ctx->history->unit=ctx->history_input;
    }
    const SourceSemanticEnvironment *environment=ctx->history_input->semantic;
    if (!ctx->history->symbols && environment->symbol_count) {
        ctx->history->symbols=source_alloc(ctx,environment->symbol_count,sizeof(*ctx->history->symbols));
        if (!ctx->history->symbols) return false;
    }
    if (!ctx->history->latest && environment->latest_count) {
        ctx->history->latest=source_alloc(ctx,environment->latest_count,sizeof(*ctx->history->latest));
        if (!ctx->history->latest) return false;
    }
    ctx->history->latest_count=environment->latest_count;return true;
}
static bool source_history_latest(SourceContext *ctx) {
    const SourceSemanticEnvironment *environment=ctx->history->unit->semantic;
    for (uint32_t i=0;i<environment->latest_count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const SourceName *original=environment->latest[i];
        size_t index=(size_t)(original-environment->symbols);
        if (index>=environment->symbol_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history latest binding lost its actual registration");
        ctx->history->latest[i]=ctx->history->symbols[index];
    }
    if (environment->implicit_result) {
        size_t index=(size_t)(environment->implicit_result-environment->symbols);
        if (index>=environment->symbol_count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history result lost its actual registration");
        ctx->history->implicit_result=ctx->history->symbols[index];
    }
    return true;
}
static bool source_history_bindings(SourceContext *ctx,bool types_only) {
    if (!source_history_begin(ctx) || !ctx->history) return ctx->diagnostic.status==XR_XIR_OK;
    const SourceSemanticEnvironment *environment=ctx->history->unit->semantic;
    const XrXirModule *original=xr_xir_compile_artifact_module(ctx->history->unit->checked);
    SourceLibraryUnit *types=source_library_unit(ctx,original);
    for (uint32_t m=0;m<environment->module_count;++m) {
        uint32_t module=source_history_target(ctx,m);
        if (module==UINT32_MAX) return false;
        for (const SourceName *source=environment->names[m];source;source=source->next) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            size_t ordinal=(size_t)(source-environment->symbols);
            if (ordinal>=environment->symbol_count)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history symbol has no owned ordinal");
            SourceName *binding=ctx->history->symbols[ordinal];
            if (binding) continue;
            switch (source->kind) {
            case SOURCE_NOMINAL:
                if (!types || !original->types || !original->types->nominals || source->index>=original->types->nominals->count)
                    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history nominal binding is not relocated");
                binding=ctx->nominal_sources[types->nominals[source->index]];break;
            case SOURCE_INTERFACE:
                if (!types || !original->types || !original->types->interfaces || source->index>=original->types->interfaces->count)
                    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history interface binding is not relocated");
                binding=ctx->interface_sources[types->interfaces[source->index]];break;
            case SOURCE_FUNCTION:
                if (types_only) continue;
                if (!ctx->history->map || source->index>=ctx->history->map->function_count)
                    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history function binding is not relocated");
                for (SourceName *candidate=ctx->names[module];candidate;candidate=candidate->next) {
                    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
                    if (candidate->kind==SOURCE_FUNCTION && candidate->index==ctx->history->map->functions[source->index]) {
                        binding=candidate;break;
                    }
                }
                if (!binding) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history function has no received declaration");
                break;
            case SOURCE_SLOT:case SOURCE_UNIT_SLOT:case SOURCE_MODULE:case SOURCE_IMPORT:
                if (types_only && (source->kind==SOURCE_SLOT || source->kind==SOURCE_UNIT_SLOT)) continue;
                binding=add_name(ctx,&ctx->names[module],source_owned_text(ctx,source->name),NULL);
                if (!binding) return false;
                binding->kind=source->kind;binding->module=module;
                binding->mutable=source->mutable;binding->inferred=source->inferred;
                if (source->kind==SOURCE_SLOT || source->kind==SOURCE_UNIT_SLOT) {
                    if (!ctx->history->map || source->index>=ctx->history->map->slot_count)
                        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history slot binding is not relocated");
                    binding->index=ctx->history->map->slots[source->index];
                    if (!source_library_type(ctx,types,source->type,&binding->type)) return false;
                } else {
                    binding->module=source_history_target(ctx,source->module);
                    if (binding->module==UINT32_MAX) return false;
                    binding->imported=source->imported ? source_owned_text(ctx,source->imported) : NULL;
                    if (source->imported && !binding->imported) return false;
                }
                break;
            default:return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history contains an unfinished binding");
            }
            ctx->history->symbols[ordinal]=binding;
        }
    }
    return source_history_latest(ctx);
}
static bool source_history_map_copy(SourceContext *ctx,const SourceLibraryMap *source) {
    SourceLibraryMap *owned=source_alloc(ctx,1,sizeof(*owned));
    if (!owned) return false;
    *owned=*source;
    owned->modules=source_library_bytes(ctx,source->modules,source->module_count,sizeof(*source->modules));
    owned->functions=source_library_bytes(ctx,source->functions,source->function_count,sizeof(*source->functions));
    owned->slots=source_library_bytes(ctx,source->slots,source->slot_count,sizeof(*source->slots));
    if (!owned->modules || !owned->functions || (source->slot_count && !owned->slots)) return false;
    ctx->history->map=owned;return true;
}
static bool source_history_binding_accessor(SourceContext *ctx,const SourceName *symbol,SourceBindingAccessor mode,uint32_t *output) {
    if (!ctx->history || !ctx->history->map || !symbol || symbol->index>=ctx->slot_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"foreign binding has no actual history accessor");
    const SourceLibraryMap *map=ctx->history->map;
    const SourceSemanticEnvironment *environment=ctx->history->unit->semantic;
    bool write=mode==SOURCE_ACCESS_WRITE,push=mode==SOURCE_ACCESS_PUSH;
    const uint32_t *accessors=write ? environment->binding_writes : push ? environment->binding_pushes : environment->binding_reads;
    for (uint32_t s=0;accessors && s<map->slot_count && s<environment->slot_count;++s) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (map->slots[s]!=symbol->index) continue;
        uint32_t accessor=accessors[s];
        if (!accessor || accessor-1>=map->function_count) break;
        uint32_t function=map->functions[accessor-1];
        XrXirType type=write || push ? XR_XIR_UNIT : symbol->type;
        XrXirType input=push ? xr_xir_array_element(&ctx->types,symbol->type) : symbol->type;
        uint32_t parameters=(write || push) && input!=XR_XIR_UNIT ? 1u : 0u;
        if (function>=ctx->function_count || ctx->functions[function].parameter_count!=parameters ||
            (parameters && ctx->functions[function].parameters[0]!=input) ||
            ctx->functions[function].result!=type || ctx->identities[function].module!=symbol->module ||
            !ctx->identities[function].exported) break;
        /* The checked body performs storage access inside its authentic birth
         * module. A normal call preserves the common verifier's module boundary. */
        *output=function;return true;
    }
    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"foreign binding accessor is not in its exact received Checked owner");
}
/* Storage stays within the ordinary checked birth body. The caller supplies
 * only the already evaluated element through its exact received signature. */
static bool source_history_binding_push(SourceContext *ctx,const SourceName *symbol,SourceValue input,SourceValue *value) {
    if(!symbol->mutable || !xr_xir_type_is_array(&ctx->types,symbol->type))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"push requires an actual mutable Array binding");
    uint32_t function;
    return source_history_binding_accessor(ctx,symbol,SOURCE_ACCESS_PUSH,&function) &&
        source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL,XR_XIR_UNIT,{0},{0},function,{0}},
            &input,input.type==XR_XIR_UNIT ? 0u : 1u,value);
}
static bool source_history_binding_read(SourceContext *ctx,const SourceName *symbol,SourceValue *value) {
    uint32_t function;
    return source_history_binding_accessor(ctx,symbol,SOURCE_ACCESS_READ,&function) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CALL,symbol->type,{0},{0},function,{0}},value);
}
static bool source_history_binding_write(SourceContext *ctx,const SourceName *symbol,SourceValue value) {
    uint32_t function;
    return source_history_binding_accessor(ctx,symbol,SOURCE_ACCESS_WRITE,&function) &&
        source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL,XR_XIR_UNIT,{0},{0},function,{0}},
            &value,value.type==XR_XIR_UNIT ? 0u : 1u,NULL);
}
static bool source_history_binding_accessors(SourceContext *ctx) {
    if (!ctx->history) return true;
    const SourceLibraryMap *map=ctx->history->map;
    const SourceSemanticEnvironment *environment=ctx->history->unit->semantic;
    if (!map || map->slot_count!=environment->slot_count ||
        (map->slot_count && (!environment->binding_reads || !environment->binding_writes || !environment->binding_pushes)))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history binding accessors lost their complete slot owner");
    for (uint32_t s=0;s<map->slot_count;++s) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        uint32_t slot=map->slots[s],reader=environment->binding_reads[s],writer=environment->binding_writes[s],push=environment->binding_pushes[s];
        if (slot>=ctx->slot_count || (reader && reader-1>=map->function_count) ||
            (writer && writer-1>=map->function_count) || (push && push-1>=map->function_count))
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history binding accessor ordinal is invalid");
        if (reader) ctx->binding_reads[slot]=map->functions[reader-1]+1;
        if (writer) ctx->binding_writes[slot]=map->functions[writer-1]+1;
        if (push) ctx->binding_pushes[slot]=map->functions[push-1]+1;
    }
    return true;
}
static bool source_history_install(SourceContext *ctx,uint32_t module,uint32_t *next_function,uint32_t *next_slot) {
    if (!source_history_begin(ctx) || !ctx->history) return false;
    const XrXirModule *original=source_owned_module_view(ctx,module);
    SourceLibraryMap map={0};
    if (!original || !source_library_map(ctx,original,module,*next_function,*next_slot,&map)) return false;
    bool ok=source_library_copy(ctx,original,&map) && source_history_map_copy(ctx,&map);
    if (ok) {*next_function=map.next_function;*next_slot=map.next_slot;ok=source_history_bindings(ctx,false);}
    source_library_map_free(&map);return ok;
}
static bool source_history_module_dependency(SourceContext *ctx,uint32_t from,uint32_t to) {
    XrXirSourceModule *module=&ctx->modules[from];
    if (!module->name) return true;
    for (uint32_t i=0;i<module->dependency_count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (module->dependencies[i]==to) return true;
    }
    if (module->dependency_count==UINT32_MAX)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"history dependency count exhausted");
    uint32_t *dependencies=source_alloc(ctx,(size_t)module->dependency_count+1,sizeof(*dependencies));
    if (!dependencies || !source_copy_bytes(ctx,NULL,dependencies,module->dependencies,module->dependency_count*sizeof(*dependencies))) return false;
    dependencies[module->dependency_count++]=to;module->dependencies=dependencies;return true;
}
static bool source_history_dependencies(SourceContext *ctx) {
    if (ctx->history_input) {
        const XrXirDeclarations *original=xr_xir_compile_artifact_module(ctx->history_input->checked)->declarations;
        for (uint32_t m=0;m<(uint32_t)ctx->graph->spec_count;++m) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            const XrModuleSpec *spec=&ctx->graph->specs[m];
            if (spec->representation!=XR_MODULE_CHECKED_HISTORY) continue;
            uint32_t scope=original->modules[spec->history_module].lexical_scope;
            if (scope) {
                uint32_t target=source_history_target(ctx,scope-1);
                if (target==UINT32_MAX) return false;
                ctx->modules[m].lexical_scope=target+1;
            }
        }
        uint32_t entry=ctx->history_input->entry_module;
        uint32_t scope=original->modules[entry].lexical_scope;
        uint32_t target=source_history_target(ctx,scope ? scope-1 : entry);
        if (target==UINT32_MAX) return false;
        ctx->modules[ctx->graph->entry_index].lexical_scope=target+1;
    }
    for (uint32_t i=0;ctx->history && i<ctx->history->edge_count;++i) {
        SourceHistoryEdge edge=ctx->history->edges[i];
        if (!((ctx)->diagnostic.status == XR_XIR_OK) || !source_history_module_dependency(ctx,edge.from,edge.to)) return false;
    }
    return true;
}
static SourceName *source_history_select(SourceContext *ctx,SourceName *binding) {
    if (!binding) return NULL;
    uint32_t target=binding->module;
    if (target>=(uint32_t)ctx->graph->spec_count || !source_owned_checked(&ctx->graph->specs[target])) {
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"history reference has no received module");return NULL;
    }
    bool known=false;
    for (uint32_t e=0;e<ctx->history->edge_count;++e) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return NULL;
        known|=ctx->history->edges[e].from==ctx->module && ctx->history->edges[e].to==target;
    }
    if (!known) {
        SourceHistoryEdge *edges=source_query_append(ctx,ctx->history->edges,&ctx->history->edge_count,
            &ctx->history->edge_capacity,sizeof(*edges));
        if (!edges) return NULL;
        ctx->history->edges=edges;edges[ctx->history->edge_count-1]=(SourceHistoryEdge){ctx->module,target};
        if (!source_history_module_dependency(ctx,ctx->module,target)) return NULL;
    }
    return binding;
}
static SourceName *source_history_name(SourceContext *ctx,const char *name) {
    if (!ctx->history || !ctx->graph || ctx->module!=(uint32_t)ctx->graph->entry_index) return NULL;
    if (ctx->interactive && source_text_same(ctx,NULL,name,"it"))
        return source_history_select(ctx,ctx->history->implicit_result);
    for (uint32_t i=0;i<ctx->history->latest_count;++i) {
        SourceName *binding=ctx->history->latest[i];
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return NULL;
        if (binding && source_text_same(ctx,NULL,binding->name,name)) return source_history_select(ctx,binding);
    }
    return NULL;
}
static bool source_history_graph_commit(SourceContext *ctx) {
    if (!ctx->history || !ctx->history->edge_count) return true;
    for (uint32_t i=0;i<ctx->history->edge_count;++i) {
        SourceHistoryEdge edge=ctx->history->edges[i];
        XrModuleStatus status=xr_compile_module_graph_add_dependency(ctx->graph,(int)edge.from,(int)edge.to);
        if (status!=XR_MODULE_OK) return source_module_fail(ctx,NULL,status,"history dependency publication failed");
    }
    XrModuleStatus status=xr_compile_module_graph_topological_sort(ctx->graph);
    return status==XR_MODULE_OK || source_module_fail(ctx,NULL,status,"history dependency ordering failed");
}
