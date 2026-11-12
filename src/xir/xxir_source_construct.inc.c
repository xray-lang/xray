/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_construct.inc.c - Ordinary Built checking for every source origin
 */
/* Dense construction facts freeze before Built enters the common checker.
 * The producer's bookkeeping supplies bindings, not independent authority. */
#include "xxir_source_generator_methods.inc.c"
static bool source_mem_return_facts(SourceContext *ctx,XrXirBorrowReturns *output) {
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirBorrowReturn *rows=source_alloc(ctx,ctx->function_count,sizeof(*rows));
    if (!rows) return false;
    uint32_t count=0;
    for (uint32_t f=0;f<ctx->function_count;++f) {
        if (!(f%64u) && !source_request_poll(ctx,NULL)) return false;
        XrXirBorrowReturn row={0};bool present=false;
        XrXirStatus status=xir_borrow_return_describe(&ctx->compile,&module,f,&row,&present,ctx->request);
        if (status!=XR_XIR_OK) return source_fail(ctx,ctx->bodies[f].node,status,"Buffer borrowed return has no complete receiver path");
        if (present) rows[count++]=row;
    }
    uint32_t expected=0,received=0;
    for (uint32_t i=0;i<count;++i) expected+=ctx->bodies[rows[i].function].checked_library;
    for (SourceLibraryUnit *unit=ctx->library_units;unit;unit=unit->next) {
        for (uint32_t i=0;i<unit->borrow_returns.count;++i) {
            if (!(i%64u) && !source_request_poll(ctx,NULL)) return false;
            const XrXirBorrowReturn *from=&unit->borrow_returns.rows[i];
            uint32_t lo=0,hi=count;
            while (lo<hi) {uint32_t mid=lo+(hi-lo)/2u;if (rows[mid].function<from->function) lo=mid+1u;else hi=mid;}
            if (lo==count || rows[lo].function!=from->function || rows[lo].receiver!=from->receiver ||
                rows[lo].nominal!=from->nominal || rows[lo].field!=from->field ||
                rows[lo].native_callable!=from->native_callable || !ctx->bodies[from->function].checked_library)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"received receiver facts do not match the current body");
            ++received;
        }
    }
    if (expected!=received) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"received receiver facts are incomplete");
    *output=(XrXirBorrowReturns){count?rows:NULL,count};return true;
}
static bool source_slot_facts_prepare(SourceContext *ctx,XrXirSlotFacts *output) {
    if (ctx->slot_count>ctx->slot_capacity ||
        (ctx->slot_count && (!ctx->slots || !ctx->slot_facts || !ctx->slot_fact_present)))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source slot fact capacity is incomplete");
    uint32_t previous_module=ctx->module;
    /* Artifact modules include received Checked internals without a Source
     * namespace. Their facts were copied with the actual library slot map. */
    for (uint32_t m=0;m<(uint32_t)ctx->graph->spec_count;++m) {
        if (source_owned_checked(&ctx->graph->specs[m])) continue;
        ctx->module=m;
        for (const SourceName *name=ctx->names[m];name;name=name->next) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if (name->kind!=SOURCE_SLOT && name->kind!=SOURCE_UNIT_SLOT) continue;
            uint32_t slot=name->index;
            if (slot>=ctx->slot_count)
                return source_fail(ctx,name->node,XR_XIR_BAD_STRUCTURE,"source slot name lost its actual coordinates");
            if (ctx->slot_fact_present[slot]) continue;
            if (ctx->slots[slot].module!=m || name->checked_library || !name->node || (!name->hidden &&
                name->node->type!=AST_VAR_DECL && name->node->type!=AST_CONST_DECL &&
                name->node->type!=AST_DESTRUCTURE_DECL))
                return source_fail(ctx,name->node,XR_XIR_BAD_STRUCTURE,"source slot has no declaration facts");
            uint32_t exported=!name->hidden && name->node->is_exported;
            size_t length=source_text_size(ctx,name->name);
            if (ctx->diagnostic.status!=XR_XIR_OK ||
                !source_slot_fact(ctx,slot,name->name,(uint32_t)length,exported)) return false;
        }
    }
    ctx->module=previous_module;
    for (uint32_t s=0;s<ctx->slot_count;++s) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (!ctx->slot_fact_present[s] || ctx->slot_facts[s].slot!=s)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source complete slot fact is absent");
    }
    *output=(XrXirSlotFacts){ctx->slot_count?ctx->slot_facts:NULL,ctx->slot_count};return true;
}
static bool source_construction_prepare(SourceContext *ctx) {
    if (ctx->construction)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source construction owner was already published");
    uint32_t count=ctx->nominals.count;
    XrXirConstructionRow *rows=count ? source_alloc(ctx,count,sizeof(*rows)) : NULL;
    if (count && !rows) return false;
    for (uint32_t n=0; n<count; ++n) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        XrHirNominalDefaults received=source_hir_nominal_defaults(ctx,n);
        if(ctx->diagnostic.status!=XR_XIR_OK)return false;
        rows[n].field_count=received.count;
        uint32_t *fields=received.count ? source_alloc(ctx,received.count,sizeof(*fields)) : NULL;
        if (received.count && !fields) return false;
        rows[n].field_initializers=fields;
        if (received.available) {
            uint32_t function=received.constructor;
            if (!function || function>=ctx->function_count)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source implicit construction target is missing");
            rows[n].default_initializer=function+1;
        }
        for (uint32_t f=0; f<received.count; ++f) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            uint32_t function=received.fields[f];
            if (function>=ctx->function_count)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source field construction target is invalid");
            fields[f]=function ? function+1 : 0;
        }
    }
    XrXirMaterializationBinding *bindings=NULL;uint32_t binding_count=0;
    if (!source_materialization_bindings(ctx,&bindings,&binding_count)) return false;
    XrXirComptimeTable comptime={0};
    if (!source_library_comptime_table(ctx,&comptime)) return false;
    XrXirSlotFacts slots={0};
    if (!source_slot_facts_prepare(ctx,&slots)) return false;
    XrXirBorrowReturns returns={0};
    if (!source_mem_return_facts(ctx,&returns)) return false;
    XrXirConstructionSpec input={rows,count,bindings,binding_count,&comptime,&returns};
    XrXirStatus status=xr_xir_compile_construction_new_v5(&ctx->compile,&ctx->types,&input,&slots,&ctx->construction,ctx->request);
    return status==XR_XIR_OK || source_fail(ctx,NULL,status,"source construction ownership failed");
}
#include "xxir_source_quiescent_storage.inc.c"

#include "xxir_source_binding_readers.inc.c"
#include "xxir_source_view_prepare.inc.c"
static bool source_item_resource(SourceContext *ctx,AstNode *node,XrCompileResourceStatus status) {
    if(status==XR_COMPILE_RESOURCE_OK)return true;
    return source_fail(ctx,node,status==XR_COMPILE_RESOURCE_MEMORY_LIMIT?XR_XIR_MEMORY_LIMIT:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        status==XR_COMPILE_RESOURCE_OVERFLOW?XR_XIR_OVERFLOW:XR_XIR_BAD_STRUCTURE,
        "module item collection failed");
}
static bool source_item_reserve(SourceContext *ctx,AstNode *node,XrHirItemSpec spec,uint32_t *out) {
    if(!spec.name||!*spec.name)return true;
    uint32_t ignored=0;
    return source_item_resource(ctx,node,xr_hir_items_reserve(ctx->items,&spec,out?out:&ignored));
}
static bool source_parameter_items(SourceContext *ctx,uint32_t module,AstNode *node,uint32_t parent) {
    bool method=node->type==AST_METHOD_DECL,requirement=node->type==AST_INTERFACE_METHOD;
    int count=method?node->as.method_decl.param_count:
        requirement?node->as.interface_method.param_count:node->as.function_decl.param_count;
    XrParamNode **parameters=method?node->as.method_decl.params:
        requirement?node->as.interface_method.params:node->as.function_decl.params;
    if(count<0||count>65536||(count&&!parameters))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"function parameters are malformed");
    for(int p=0;p<count;++p) {
        if(!source_request_poll(ctx,node))return false;
        const XrParamNode *param=parameters[p];
        if(!param)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"function parameter is missing");
        XrHirItemSpec spec={param->name,module,parent,node->node_id,param->line,param->column,XR_HIR_ITEM_PARAMETER};
        if(!source_item_reserve(ctx,node,spec,NULL))return false;
    }
    return true;
}
static bool source_nominal_parameter_items(SourceContext *ctx,uint32_t module,AstNode *node,uint32_t parent) {
    SourceNominalDeclaration declaration;
    if(!source_nominal_declaration(ctx,node,&declaration))return false;
    for(int m=0;m<declaration.method_count;++m) {
        AstNode *method=declaration.methods[m];
        if(!source_request_poll(ctx,method))return false;
        if(!method||method->type!=AST_METHOD_DECL)
            return source_fail(ctx,method,XR_XIR_BAD_STRUCTURE,"nominal method declaration is malformed");
        const char *name=method->as.method_decl.is_constructor?"init":method->as.method_decl.name;
        if(!name||!*name)return source_fail(ctx,method,XR_XIR_BAD_STRUCTURE,"method name is missing");
        uint32_t item=0;
        XrHirItemKind kind=method->as.method_decl.is_constructor?XR_HIR_ITEM_PARAMETER_SCOPE:XR_HIR_ITEM_METHOD;
        XrHirItemSpec spec={name,module,parent,method->node_id,method->line,method->column,kind};
        if(!source_item_reserve(ctx,method,spec,&item))return false;
    }
    return true;
}
static bool source_interface_parameter_items(SourceContext *ctx,uint32_t module,AstNode *node,uint32_t parent) {
    const InterfaceDeclNode *declaration=&node->as.interface_decl;
    if(declaration->method_count<0||(declaration->method_count&&!declaration->methods))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"interface method declarations are malformed");
    for(int m=0;m<declaration->method_count;++m) {
        AstNode *method=declaration->methods[m];
        if(!source_request_poll(ctx,method))return false;
        if(!method||method->type!=AST_INTERFACE_METHOD)
            return source_fail(ctx,method,XR_XIR_BAD_STRUCTURE,"interface method declaration is malformed");
        const char *name=method->as.interface_method.name;
        if(!name||!*name)return source_fail(ctx,method,XR_XIR_BAD_STRUCTURE,"interface method name is missing");
        uint32_t item=0;
        XrHirItemSpec spec={name,module,parent,method->node_id,method->line,method->column,XR_HIR_ITEM_METHOD};
        if(!source_item_reserve(ctx,method,spec,&item)||!source_parameter_items(ctx,module,method,item+1))return false;
    }
    return true;
}
typedef struct SourceParameterItems {
    SourceContext *ctx;
    uint32_t module,parent,depth;
} SourceParameterItems;
static bool source_closure_parameter_items(AstNode *node,void *pointer) {
    SourceParameterItems *walk=pointer;
    SourceContext *ctx=walk->ctx;
    if(!node)return true;
    if(!source_request_poll(ctx,node))return false;
    if(walk->depth==128)
        return source_fail(ctx,node,XR_XIR_BUDGET,"parameter scope depth exhausted");
    uint32_t parent=walk->parent;
    bool closure=node->type==AST_FUNCTION_EXPR||(node->type==AST_FUNCTION_DECL&&walk->depth);
    if(closure) {
        uint32_t item=0;
        /* Scope markers are keyed by AST identity and never enter a name namespace. */
        XrHirItemSpec spec={"$closure",walk->module,parent,node->node_id,node->line,node->column,
            XR_HIR_ITEM_PARAMETER_SCOPE};
        if(!source_item_reserve(ctx,node,spec,&item)||
            !source_parameter_items(ctx,walk->module,node,item+1))return false;
        walk->parent=item+1;
    } else if(node->type==AST_METHOD_DECL) {
        uint32_t item=0;
        if(!xr_hir_items_find_scope(ctx->items,walk->module,node->node_id,&item))
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"method parameter scope was not reserved");
        if(!source_parameter_items(ctx,walk->module,node,item+1))return false;
        walk->parent=item+1;
    }
    ++walk->depth;
    bool ok=xr_ast_for_each_child(node,source_closure_parameter_items,walk);
    --walk->depth;walk->parent=parent;
    return ok||source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"parameter scope syntax is unknown");
}
static bool source_items_prepare(SourceContext *ctx) {
    if(ctx->items)return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"module items already prepared");
    if(!source_item_resource(ctx,NULL,xr_hir_items_new(ctx->compile.resources,&ctx->items)))return false;
    uint32_t previous_module=ctx->module;
    for(uint32_t m=0;m<(uint32_t)ctx->graph->spec_count;++m) {
        ctx->module=m;
        if(!source_request_poll(ctx,NULL))return false;
        const XrModuleSpec *spec=&ctx->graph->specs[m];
        const uint8_t zero[32]={0};const uint8_t *revision=memcmp(spec->source_content_fingerprint.bytes,zero,32)?spec->source_content_fingerprint.bytes:NULL;
        if(!source_item_resource(ctx,NULL,xr_hir_items_source(ctx->items,m,spec->source_path,
            spec->source_text,spec->source_length,revision)))return false;
        AstNode *ast=spec->ast;if(!ast||ast->type!=AST_PROGRAM)continue;
        for(int i=0;i<ast->as.program.count;++i) {
            AstNode *node=ast->as.program.statements[i];const char *name=NULL;uint32_t parent=0;
            if(!source_request_poll(ctx,node))return false;
            switch(node->type) {
            case AST_FUNCTION_DECL:name=node->as.function_decl.name;break;
            case AST_VAR_DECL:case AST_CONST_DECL:name=node->as.var_decl.name;break;
            case AST_STRUCT_DECL:name=node->as.struct_decl.name;break;
            case AST_CLASS_DECL:name=node->as.class_decl.name;break;
            case AST_ENUM_DECL:name=node->as.enum_decl.name;break;
            case AST_INTERFACE_DECL:name=node->as.interface_decl.name;break;
            default:break;
            }
            if(name&&*name) {
                uint32_t item=0;
                XrHirItemSpec item_spec={name,m,0,node->node_id,node->line,node->column,XR_HIR_ITEM_DECLARATION};
                if(!source_item_reserve(ctx,node,item_spec,&item))return false;
                parent=item+1;
                if(node->type==AST_FUNCTION_DECL&&!source_parameter_items(ctx,m,node,item+1))return false;
                if((node->type==AST_STRUCT_DECL||node->type==AST_CLASS_DECL||node->type==AST_ENUM_DECL)&&
                    !source_nominal_parameter_items(ctx,m,node,item+1))return false;
                if(node->type==AST_INTERFACE_DECL&&!source_interface_parameter_items(ctx,m,node,item+1))return false;
            }
            if(node->type==AST_DESTRUCTURE_DECL) {
                SourceDestructurePlan plan={0};
                if(!source_destructure_pattern(ctx,node,&plan))return false;
                for(uint32_t f=0;f<plan.count;++f) {
                    const XrDestructurePattern *field=plan.fields[f].pattern;
                    XrHirItemSpec item_spec={field->as.identifier.name,m,0,node->node_id,field->line,field->column,XR_HIR_ITEM_DECLARATION};
                    if(!source_item_reserve(ctx,node,item_spec,NULL))return false;
                }
            }
            if(node->type==AST_IMPORT_STMT) {
                const ImportStmtNode *import=&node->as.import_stmt;
                XrNameSpan binding=source_import_binding_span(import,0);
                XrHirItemSpec item_spec={import->alias,m,0,node->node_id,binding.line,binding.column,XR_HIR_ITEM_DECLARATION};
                if(!source_item_reserve(ctx,node,item_spec,NULL))return false;
                for(int p=0;p<import->member_count;++p) {
                    const ImportMember *member=&import->members[p];
                    XrNameSpan span=member->alias?member->alias_span:member->name_span;
                    item_spec=(XrHirItemSpec){member->alias?member->alias:member->name,m,0,node->node_id,span.line,span.column,XR_HIR_ITEM_DECLARATION};
                    if(!source_item_reserve(ctx,node,item_spec,NULL))return false;
                }
            }
            SourceParameterItems walk={ctx,m,parent,0};
            if(!source_closure_parameter_items(node,&walk))return false;
        }
    }
    ctx->module=previous_module;
    return !xr_hir_items_errors(ctx->items)||source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"duplicate declaration name");
}
/* Source maps numeric parameter ordinals while its own AST is alive. */
typedef struct SourceSuspendExplanation {
    SourceContext *source;
    const XrXirModule *module;
    size_t used;
    uint32_t parameter_source;
    bool truncated;
} SourceSuspendExplanation;

static const char *source_suspend_parameter_name(SourceContext *ctx, uint32_t function, uint32_t parameter) {
    if (function >= ctx->function_count) return NULL;
    const SourceFunction *body = &ctx->bodies[function];
    if (parameter < body->root_capture_count && body->root_capture_names)
        return body->root_capture_names[parameter];
    AstNode *node = body->node;
    XrParamNode *const *parameters = NULL;
    uint32_t count = 0;
    if (node && (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR)) {
        parameters = node->as.function_decl.params;
        count = (uint32_t)node->as.function_decl.param_count;
    } else if (node && node->type == AST_METHOD_DECL) {
        parameters = node->as.method_decl.params;
        count = (uint32_t)node->as.method_decl.param_count;
    }
    uint32_t physical = ctx->functions[function].parameter_count;
    if (count > physical || parameter < physical - count || parameter - (physical - count) >= count) return NULL;
    return parameters[parameter - (physical - count)]->name;
}

static XrXirStatus source_suspend_explain_step(void *context, const XirSuspendCauseStep *step) {
    SourceSuspendExplanation *walk = context;
    SourceContext *ctx = walk->source;
    if (step->function >= walk->module->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body = &walk->module->functions[step->function];
    char *message = ctx->diagnostic.message;
    size_t capacity = sizeof(ctx->diagnostic.message);
    if (walk->truncated) return XR_XIR_OK;
    char segment[512];
    /* The parameter transition has already named the literal's creator. */
    if (walk->parameter_source == step->function && !step->parameter_dependency && !step->terminal) {
        walk->parameter_source = UINT32_MAX;
        return XR_XIR_OK;
    }
    int written;
    if (step->parameter_dependency && step->callee < walk->module->function_count) {
        const XrXirFunction *callee = &walk->module->functions[step->callee];
        const char *name = source_suspend_parameter_name(ctx, step->callee, step->parameter);
        written = snprintf(segment, sizeof(segment), "%.*s via %.*s parameter `%s`%s",
            (int)body->name_length, body->name, (int)callee->name_length, callee->name,
            name ? name : "capture", step->terminal ? " (unqualified callable)" : " -> ");
        walk->parameter_source = step->function;
    } else {
        const char *tail = step->terminal ? (step->suspending ?
            " (suspending operation)" : " (unqualified callable)") : " -> ";
        written = snprintf(segment, sizeof(segment), "%.*s%s",
            (int)body->name_length, body->name, tail);
        walk->parameter_source = UINT32_MAX;
    }
    if (walk->used + 4 > capacity) return XR_XIR_BAD_STRUCTURE;
    if (written < 0 || (size_t)written >= sizeof(segment) ||
        (size_t)written > capacity - walk->used - 4) {
        memcpy(message + walk->used, "...", 4);
        walk->truncated = true;
    } else {
        memcpy(message + walk->used, segment, (size_t)written + 1);
        walk->used += (size_t)written;
    }
    return XR_XIR_OK;
}

static XrXirStatus source_suspend_has_parameter(void *context, const XirSuspendCauseStep *step) {
    bool *present = context;
    *present |= step->parameter_dependency;
    return XR_XIR_OK;
}

/* Diagnostic text is copied before the immutable numeric owner is released. */
static void source_effect_explain(void *context, const XrXirModule *module,
    const XrXirEffects *effects, const XrXirDiagnostic *location) {
    SourceContext *ctx = context;
    bool cleanup = location->reason == XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND;
    if (ctx->diagnostic.status != XR_XIR_OK ||
        (location->reason != XR_XIR_DIAGNOSTIC_NO_SUSPEND && !cleanup) ||
        location->function >= module->function_count) return;
    uint32_t function = location->function;
    if (cleanup) {
        bool parameter = false;
        XrXirStatus status = xir_effects_suspend_trace(effects, function, source_suspend_has_parameter, &parameter);
        if (status != XR_XIR_OK || !parameter) return;
    }
    ctx->module = ctx->bodies[function].module;
    source_fail(ctx, ctx->bodies[function].node, location->status, cleanup ?
        "E0392: defer may suspend or create a task. Reason " :
        "declared no_suspend function may suspend or call an unqualified callable. Reason ");
    SourceSuspendExplanation walk = {ctx, module, strlen(ctx->diagnostic.message), UINT32_MAX, false};
    XrXirStatus status = xir_effects_suspend_trace(effects, function, source_suspend_explain_step, &walk);
    if (status != XR_XIR_OK) {
        ctx->diagnostic.status = status;
        memcpy(ctx->diagnostic.message, "invalid numeric suspension cause", sizeof("invalid numeric suspension cause"));
    }
}

#include "xxir_source_escape.inc.c"
static void source_check_failure(SourceContext *ctx, XrXirDiagnostic location, XrXirStatus status) {
    char message[128] = {0};
    if (status != XR_XIR_BUDGET && status != XR_XIR_OUT_OF_MEMORY &&
        source_format(ctx, message, sizeof(message),
            "constructed XIR failed checking at function %u block %u instruction %u",
            location.function, location.block, location.instruction) != XR_DIAG_OK)
        status = XR_XIR_BUDGET;
    AstNode *site = NULL;
    if (location.function < ctx->function_count) {
        site = ctx->bodies[location.function].node;
        ctx->module = ctx->bodies[location.function].module;
    }
    const char *cause = location.reason == XR_XIR_DIAGNOSTIC_GO_ROOT_REQUIRED ?
        "GO target requires the current instance root execution" :
        location.reason == XR_XIR_DIAGNOSTIC_GO_ROOT_UNRESOLVED ?
        "GO target lacks proof for worker execution" :
        location.reason == XR_XIR_DIAGNOSTIC_NO_SUSPEND ?
        "declared no_suspend function may suspend or call an unqualified callable" :
        location.reason == XR_XIR_DIAGNOSTIC_CLEANUP_THROW ?
        "E0387: an error can escape the defer body" :
        location.reason == XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND ?
        "E0392: defer may suspend or create a task" :
        location.reason == XR_XIR_DIAGNOSTIC_UNINITIALIZED_READ ?
        "read requires storage initialized on every incoming path" :
        location.reason == XR_XIR_DIAGNOSTIC_READONLY_WRITE ?
        "write may overwrite already initialized const storage" :
        location.reason == XR_XIR_DIAGNOSTIC_TASK_SENDABLE ?
        "Task arguments and captures must satisfy Sendable" :
        location.reason == XR_XIR_DIAGNOSTIC_TASK_CELL_CAPTURE ?
        "Cell captures cannot cross a Task boundary" : message;
    source_fail(ctx, site, status, cause);
    bool task_transfer=location.reason==XR_XIR_DIAGNOSTIC_TASK_SENDABLE ||
        location.reason==XR_XIR_DIAGNOSTIC_TASK_CELL_CAPTURE;
    if (task_transfer && location.function<ctx->function_count &&
        location.instruction<ctx->functions[location.function].instruction_count &&
        ctx->bodies[location.function].debug_points) {
        XrXirSourceRange range=ctx->bodies[location.function].debug_points[location.instruction].range;
        if (range.line>0) {
            ctx->diagnostic.module=range.module;ctx->diagnostic.line=range.line;ctx->diagnostic.column=range.column;
        }
    }
}

static XrXirStatus source_semantic_begin(void *context, const XrXirEffects *effects) {
    SourceContext *ctx = context;
    XrXirStatus status = source_effect_bindings_prepare(ctx, effects);
    if (status == XR_XIR_OK && !source_escape_prepare(ctx)) status = ctx->diagnostic.status;
    return status;
}

static XrXirRootRefiner source_semantic_refiner(SourceContext *ctx) {
    return (XrXirRootRefiner){.context=ctx,.update=source_root_refine,.explain=source_effect_explain,
        .begin=source_semantic_begin,.observe=source_escape_observe,.points=source_escape_points,
        .prepare=source_escape_finish,.admit=source_view_admit};
}

static bool source_semantic_prepare(SourceContext *ctx) {
    XrXirDeclarations declarations;
    XrXirModule module=source_module_view(ctx,&declarations);
    XrXirDiagnostic location={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    XirEffectInput input={&module,ctx->construction,ctx->request};
    XrXirStatus status=xir_compile_structure_verify_input(&ctx->compile,input,&location);
    for (uint32_t f=0;status==XR_XIR_OK && f<ctx->function_count;++f)
        if (ctx->bodies[f].initialization_regions)
            status=xr_xir_compile_initialization_check(&ctx->compile,&module,f,
                ctx->bodies[f].initialization_regions,&location);
    if (status==XR_XIR_OK) {
        XrXirRootRefiner refiner=source_semantic_refiner(ctx);
        status=xir_effects_prepare_verified_input(&ctx->compile,input,&refiner,&ctx->semantic_prepare);
    }
    if (status==XR_XIR_OK) return true;
    if (ctx->diagnostic.status==XR_XIR_OK) source_check_failure(ctx,location,status);
    return false;
}

static bool source_typed_body_finish(SourceContext *ctx) {
    XrXirStatus status=xr_hir_body_finish(ctx->typed_body,ctx->request);
    return status==XR_XIR_OK || source_fail(ctx,NULL,status,"typed body has an unconsumed or unprojected use");
}

static void source_construct(SourceContext *ctx, XrXirSourceResult *output) {
    if(!source_hir_body_begin(ctx))return;
    ctx->query_ready = true;
    ctx->declarations_building = true;
    bool declarations_ready = source_query_modules(ctx) && source_items_prepare(ctx) && collect_declarations(ctx);
    ctx->declarations_building = false;
    if (declarations_ready && (!ctx->core_factory || source_core_contract(ctx)) && source_manifests_bind(ctx) &&
        source_implementations_bind(ctx) && source_generator_named_factories(ctx) && build_bodies(ctx) && source_binding_readers(ctx) && source_history_graph_commit(ctx) &&
        source_instance_literals_finish(ctx) && source_materialization_finish(ctx) && source_class_carriers(ctx) && source_local_errors_finish(ctx) && source_typed_body_finish(ctx) && source_effect_prepare(ctx) && source_construction_ready(ctx) && source_semantic_prepare(ctx) && source_view_prepare(ctx)) {
        source_release_retired_arrays(ctx);
        if (!source_quiescent_storage(ctx)){xr_hir_body_end(ctx->typed_body);return;}
        XrXirDeclarations declarations;
        XrXirModule built = source_module_view(ctx,&declarations);
        XrXirDiagnostic location = {0};
        XrXirRootRefiner refiner=source_semantic_refiner(ctx);
        XrXirStatus status = xr_xir_compile_check_refined_v2(&ctx->compile, &built, ctx->construction, &refiner, ctx->request, &ctx->semantic_prepare, &output->checked, &location);
#if XR_XIR_PARANOID
        for (uint32_t f=0;status==XR_XIR_OK && f<ctx->function_count;++f) {
            if (!ctx->bodies[f].initialization_regions) continue;
            location.function=f;
            status=xr_xir_compile_initialization_check(&ctx->compile, &built, f, ctx->bodies[f].initialization_regions, &location);
        }
#endif
        if (status != XR_XIR_OK) {
            xr_xir_compile_artifact_free(output->checked); output->checked=NULL;
            source_check_failure(ctx, location, status);
        }
    }
    xr_hir_body_end(ctx->typed_body);
}
