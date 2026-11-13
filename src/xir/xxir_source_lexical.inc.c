/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_lexical.inc.c - Export the unique typed lexical declarations
 */
typedef struct SourceLexicalMaps {
    uint32_t *scopes, *bindings;
    uint32_t scope_count, binding_count;
} SourceLexicalMaps;
#include "xxir_source_lexical_flow.inc.c"
#include "xxir_source_lexical_need.inc.c"

static bool source_lexical_scopes(SourceContext *ctx, XirLexicalFunction *functions,
    SourceLexicalMaps *maps) {
    for (uint32_t s=0;s<maps->scope_count;++s) {
        XrHirBodyScope parent;
        XrHirBodyOrigin origin;
        if (!xr_hir_body_lexical_scope(ctx->typed_body,s,&parent,&origin)) return false;
        maps->scopes[s]=UINT32_MAX;
        if (origin.function!=UINT32_MAX) {
            if (origin.function>=ctx->function_count) return false;
            ++functions[origin.function].scope_count;
        }
    }
    for (uint32_t f=0;f<ctx->function_count;++f) {
        XirLexicalFunction *row=&functions[f];
        if (!row->scope_count) continue;
        row->scopes=source_alloc(ctx,row->scope_count,sizeof(*row->scopes));
        if (!row->scopes) return false;
        row->scope_count=0;
    }
    for (uint32_t s=0;s<maps->scope_count;++s) {
        XrHirBodyScope parent;
        XrHirBodyOrigin origin;
        if (!xr_hir_body_lexical_scope(ctx->typed_body,s,&parent,&origin)) return false;
        if (origin.function==UINT32_MAX) continue;
        XirLexicalFunction *row=&functions[origin.function];
        uint32_t at=row->scope_count++;
        if (parent.owner && (parent.index>=s || maps->scopes[parent.index]==UINT32_MAX)) return false;
        ((XirLexicalScope *)row->scopes)[at]=(XirLexicalScope){
            parent.owner ? maps->scopes[parent.index] : UINT32_MAX,origin.node};
        maps->scopes[s]=at;
    }
    return true;
}

static bool source_lexical_materialized(const XrHirLexicalBinding *binding) {
    return binding->materialized && binding->declaration.origin.function!=UINT32_MAX &&
        (binding->value!=UINT32_MAX || binding->declaration.logical==XR_XIR_UNIT);
}

static bool source_lexical_binding_types(SourceContext *ctx,const XrXirFunction *function,
    XirLexicalBinding *binding,bool *changed) {
    XrXirType physical=XR_XIR_UNIT;
    if (binding->value!=UINT32_MAX) {
        if (binding->value<function->parameter_count) physical=function->parameters[binding->value];
        else {
            uint32_t i=binding->value-function->parameter_count;
            if (i>=function->instruction_count)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"lexical binding lost its actual value");
            physical=function->instructions[i].type;
        }
    }
    XrXirType logical=binding->rebindable && !binding->direct_borrowed ?
        xr_xir_cell_element(&ctx->types,physical) : physical;
    if (binding->physical!=physical || binding->logical!=logical) {
        binding->physical=physical;binding->logical=logical;
        if (changed) *changed=true;
    }
    return true;
}

static bool source_lexical_bindings(SourceContext *ctx, XirLexicalFunction *functions,
    SourceLexicalMaps *maps) {
    for (uint32_t b=0;b<maps->binding_count;++b) {
        XrHirLexicalBinding binding;
        if (!xr_hir_body_lexical_binding(ctx->typed_body,b,&binding)) return false;
        maps->bindings[b]=UINT32_MAX;
        if (source_lexical_materialized(&binding)) {
            uint32_t f=binding.declaration.origin.function;
            if (f>=ctx->function_count) return false;
            ++functions[f].binding_count;
        }
    }
    for (uint32_t f=0;f<ctx->function_count;++f) {
        XirLexicalFunction *row=&functions[f];
        if (!row->binding_count) continue;
        row->bindings=source_alloc(ctx,row->binding_count,sizeof(*row->bindings));
        if (!row->bindings) return false;
        row->binding_count=0;
    }
    for (uint32_t b=0;b<maps->binding_count;++b) {
        XrHirLexicalBinding binding;
        if (!xr_hir_body_lexical_binding(ctx->typed_body,b,&binding)) return false;
        if (!source_lexical_materialized(&binding)) continue;
        const XrHirBindingInput declaration=binding.declaration;
        uint32_t f=declaration.origin.function;
        XirLexicalFunction *row=&functions[f];
        const XrXirFunction *function=&ctx->functions[f];
        if (declaration.scope.index>=maps->scope_count || maps->scopes[declaration.scope.index]==UINT32_MAX)
            return false;
        uint32_t at=row->binding_count++;
        XrHirBindingKey key=declaration.key;
        ((XirLexicalBinding *)row->bindings)[at]=(XirLexicalBinding){
            {key.module,key.declaration,key.node,key.parent_declaration,f,key.storage},
            maps->scopes[declaration.scope.index],at,binding.value,XR_XIR_UNIT,XR_XIR_UNIT,
            declaration.mode,binding.role,declaration.rebindable,declaration.direct_borrowed};
        if (!source_lexical_binding_types(ctx,function,&((XirLexicalBinding *)row->bindings)[at],NULL)) return false;
        maps->bindings[b]=at;
    }
    return true;
}

static bool source_lexical_events(SourceContext *ctx, uint32_t f,
    XirLexicalFunction *row, const SourceLexicalMaps *maps) {
    SourceFunction *body=&ctx->bodies[f];
    uint32_t count=0;
    for (SourceLexicalEvent *event=body->lexical_events;event;event=event->next) {
        if (count==UINT32_MAX) return false;
        ++count;
    }
    SourceLexicalFlow flow={.context=ctx,.function=f,.maps=maps,.directory=row};
    XrXirStatus status=source_lexical_flow(&flow);
    if (status!=XR_XIR_OK || flow.count>UINT32_MAX-count)
        return source_fail(ctx,NULL,status!=XR_XIR_OK ? status : XR_XIR_BUDGET,"lexical outcome directory is incomplete");
    uint32_t total=count+flow.count;
    XirLexicalEvent *events=total ? source_alloc(ctx,total,sizeof(*events)) : NULL;
    if (total && !events) return false;
    row->events=events;
    row->event_count=total;
    flow.events=total ? events+count : NULL;
    flow.count=0;
    flow.throw_index=0;
    status=source_lexical_flow(&flow);
    if (status!=XR_XIR_OK) return false;
    for (SourceLexicalEvent *record=body->lexical_events;record;record=record->next) {
        XirLexicalEvent event=record->event;
        if (event.scope>=maps->scope_count || maps->scopes[event.scope]==UINT32_MAX) return false;
        event.scope=maps->scopes[event.scope];
        if (event.target!=UINT32_MAX) {
            if (event.target>=maps->scope_count || maps->scopes[event.target]==UINT32_MAX) return false;
            event.target=maps->scopes[event.target];
        }
        if (event.binding!=UINT32_MAX) {
            if (event.binding>=maps->binding_count || maps->bindings[event.binding]==UINT32_MAX) return false;
            event.binding=maps->bindings[event.binding];
        }
        events[--count]=event;
    }
    return true;
}

static bool source_lexical_prepare(SourceContext *ctx) {
    if (ctx->effect_evidence->lexical.function_count) return true;
    SourceLexicalNeed need;
    if (!source_lexical_need_prepare(ctx,&need)) return false;
    SourceLexicalMaps maps={0};
    if (!xr_hir_body_lexical_counts(ctx->typed_body,&maps.scope_count,&maps.binding_count))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"lexical facts lost their typed body owner");
    XirLexicalFunction *functions=source_alloc(ctx,ctx->function_count,sizeof(*functions));
    maps.scopes=maps.scope_count ? source_alloc(ctx,maps.scope_count,sizeof(*maps.scopes)) : NULL;
    maps.bindings=maps.binding_count ? source_alloc(ctx,maps.binding_count,sizeof(*maps.bindings)) : NULL;
    if (!functions || (maps.scope_count && !maps.scopes) || (maps.binding_count && !maps.bindings)) return false;
    if (!source_lexical_scopes(ctx,functions,&maps) || !source_lexical_bindings(ctx,functions,&maps))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"lexical declarations lost their emitted identity");
    for (uint32_t f=0;f<ctx->function_count;++f) {
        if (!source_request_poll(ctx,NULL)) return false;
        bool required=source_lexical_function_needed(&need,f);
        if (ctx->diagnostic.status!=XR_XIR_OK) return false;
        if (!required) {
            functions[f]=(XirLexicalFunction){0};
            ctx->bodies[f].lexical=functions[f];
            continue;
        }
        if (ctx->bodies[f].checked_library) {
            functions[f]=ctx->bodies[f].lexical;
            continue;
        }
        if (ctx->bodies[f].lexical_unresolved) {
            functions[f]=(XirLexicalFunction){0};
            ctx->bodies[f].lexical=functions[f];
            continue;
        }
        if (!source_lexical_events(ctx,f,&functions[f],&maps))
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"lexical event lost its emitted boundary");
        ctx->bodies[f].lexical=functions[f];
    }
    ctx->effect_evidence->lexical=(XirLexicalProgram){functions,ctx->function_count};
    return true;
}

/* Canonical Cell identity refinement and lifetime relocation can replace the
 * type of the same actual value. They keep the declaration and extent key. */
static bool source_lexical_refresh(SourceContext *ctx,bool *changed) {
    if (!ctx->effect_evidence || !ctx->effect_evidence->lexical.function_count) return true;
    XirLexicalProgram program=ctx->effect_evidence->lexical;
    if (program.function_count!=ctx->function_count || !program.functions)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"lexical type owner changed shape");
    for (uint32_t f=0;f<program.function_count;++f) {
        const XrXirFunction *function=&ctx->functions[f];
        const XirLexicalFunction *row=&program.functions[f];
        for (uint32_t b=0;b<row->binding_count;++b) {
            if (!source_request_poll(ctx,NULL)) return false;
            if (!source_lexical_binding_types(ctx,function,
                &((XirLexicalBinding *)row->bindings)[b],changed)) return false;
        }
    }
    return true;
}
