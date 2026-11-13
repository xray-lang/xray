/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_lexical_view.inc.c - Relocate the same lexical program boundaries
 */
static bool source_view_lexical(SourceContext *ctx, const XirViewNormalization *normalization,
    XrXirProvenance *evidence) {
    if (!evidence->lexical.function_count) return true;
    if (evidence->lexical.function_count!=ctx->function_count || !evidence->lexical.functions)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Source lexical owner changed shape");
    XirLexicalFunction *functions=source_view_copy(ctx,evidence->lexical.functions,
        ctx->function_count,sizeof(*functions));
    if (!functions) return false;
    for (uint32_t f=0;f<ctx->function_count;++f) {
        const XirViewNormalizedFunction *map=&normalization->functions[f];
        if (!map->instructions) continue;
        XirLexicalFunction *row=&functions[f];
        uint32_t parameters=ctx->functions[f].parameter_count;
        XirLexicalBinding *bindings=source_view_copy(ctx,row->bindings,row->binding_count,sizeof(*bindings));
        XirLexicalEvent *events=source_view_copy(ctx,row->events,row->event_count,sizeof(*events));
        if ((row->binding_count && !bindings) || (row->event_count && !events)) return false;
        for (uint32_t b=0;b<row->binding_count;++b)
            if (!source_view_value(ctx,map,parameters,&bindings[b].value)) return false;
        for (uint32_t e=0;e<row->event_count;++e) {
            XirLexicalEvent *event=&events[e];
            if (!source_view_value(ctx,map,parameters,&event->value)) return false;
            bool retiring=event->kind==XIR_LEXICAL_LEAVE || event->kind==XIR_LEXICAL_TEMP_END;
            bool instruction_trigger=retiring || event->kind==XIR_LEXICAL_RETURN ||
                event->kind==XIR_LEXICAL_BREAK || event->kind==XIR_LEXICAL_CONTINUE;
            bool value_completion=event->kind==XIR_LEXICAL_BIND || event->kind==XIR_LEXICAL_REPLACE ||
                event->kind==XIR_LEXICAL_TAKE;
            if (event->position==XIR_LEXICAL_BLOCK_ENTRY) {
                if (event->block>=normalization->bodies[f].block_count) return false;
                /* Transfers retain their actual operation after inserted loan cleanup. */
                if (instruction_trigger) {
                    if (!source_view_instruction(ctx,map,parameters,&event->instruction)) return false;
                    event->position=XIR_LEXICAL_BEFORE;
                } else event->instruction=normalization->bodies[f].blocks[event->block].first;
            } else if (event->position==XIR_LEXICAL_AFTER && !value_completion) {
                if (event->instruction>=map->old_instructions ||
                    map->boundaries[event->instruction+1]<=map->boundaries[event->instruction]) return false;
                event->instruction=map->boundaries[event->instruction+1]-1;
            } else if (!source_view_instruction(ctx,map,parameters,&event->instruction)) return false;
            uint32_t *frontiers[]={&event->frontier,&event->target_frontier};
            for (uint32_t n=0;n<2;++n) if (*frontiers[n]) {
                uint32_t instruction=*frontiers[n]-1;
                if (!source_view_instruction(ctx,map,parameters,&instruction)) return false;
                *frontiers[n]=instruction+1;
            }
        }
        row->bindings=bindings;
        row->events=events;
    }
    evidence->lexical.functions=functions;
    return true;
}
