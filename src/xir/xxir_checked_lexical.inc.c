/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_checked_lexical.inc.c - Transport the complete owned lexical directory
 */
static void checked_lexical_bindings(CheckedCursor *c, XirLexicalFunction *function) {
    uint32_t count=checked_u32(c,function->binding_count);
    XirLexicalBinding *bindings=checked_array(c,function->bindings,count,sizeof(*bindings),56);
    if (c->reading) { function->bindings=bindings; function->binding_count=bindings ? count : 0; }
    for (uint32_t b=0;b<count && c->status==XR_XIR_OK;++b) {
        XirLexicalBinding row=bindings[b];
        uint32_t *fields[]={&row.key.module,&row.key.declaration,&row.key.node,&row.key.parent_declaration,
            &row.key.function,&row.key.storage,&row.scope,&row.order,&row.value};
        for (uint32_t n=0;n<sizeof(fields)/sizeof(fields[0]);++n) *fields[n]=checked_u32(c,*fields[n]);
        row.logical=(XrXirType)checked_u32(c,(uint32_t)row.logical);
        row.physical=(XrXirType)checked_u32(c,(uint32_t)row.physical);
        row.mode=(XrParamMode)checked_u32(c,(uint32_t)row.mode);
        row.role=(XirLexicalBindingRole)checked_u32(c,(uint32_t)row.role);
        uint32_t flags=checked_u32(c,(row.rebindable ? 1u : 0u) | (row.direct_borrowed ? 2u : 0u));
        if (flags>3) c->status=XR_XIR_BAD_STRUCTURE;
        row.rebindable=(flags&1u)!=0;
        row.direct_borrowed=(flags&2u)!=0;
        if (c->reading) bindings[b]=row;
    }
}

static void checked_lexical_events(CheckedCursor *c, XirLexicalFunction *function) {
    uint32_t count=checked_u32(c,function->event_count);
    XirLexicalEvent *events=checked_array(c,function->events,count,sizeof(*events),52);
    if (c->reading) { function->events=events; function->event_count=events ? count : 0; }
    for (uint32_t e=0;e<count && c->status==XR_XIR_OK;++e) {
        XirLexicalEvent row=events[e];
        uint32_t *fields[]={&row.block,&row.instruction,&row.ordinal,&row.scope,&row.target,
            &row.binding,&row.value,&row.frontier,&row.target_frontier,&row.statement,&row.parent_statement};
        for (uint32_t n=0;n<sizeof(fields)/sizeof(fields[0]);++n) *fields[n]=checked_u32(c,*fields[n]);
        row.position=(XirLexicalPositionKind)checked_u32(c,(uint32_t)row.position);
        row.kind=(XirLexicalEventKind)checked_u32(c,(uint32_t)row.kind);
        if (c->reading) events[e]=row;
    }
}

static void checked_lexical(CheckedCursor *c, XrXirModule *module, XrXirProvenance *provenance) {
    uint32_t count=checked_u32(c,provenance->lexical.function_count);
    if (count && count!=module->function_count) c->status=XR_XIR_BAD_STRUCTURE;
    XirLexicalFunction *functions=checked_array(c,provenance->lexical.functions,count,sizeof(*functions),12);
    if (c->reading) provenance->lexical=(XirLexicalProgram){functions,functions ? count : 0};
    for (uint32_t f=0;f<count && c->status==XR_XIR_OK;++f) {
        XirLexicalFunction row=functions[f];
        uint32_t scopes=checked_u32(c,row.scope_count);
        XirLexicalScope *items=checked_array(c,row.scopes,scopes,sizeof(*items),8);
        if (c->reading) { row.scopes=items; row.scope_count=items ? scopes : 0; }
        for (uint32_t s=0;s<scopes && c->status==XR_XIR_OK;++s) {
            XirLexicalScope scope=items[s];
            scope.parent=checked_u32(c,scope.parent);
            scope.node=checked_u32(c,scope.node);
            if (c->reading) items[s]=scope;
        }
        checked_lexical_bindings(c,&row);
        checked_lexical_events(c,&row);
        if (c->reading) functions[f]=row;
    }
}
